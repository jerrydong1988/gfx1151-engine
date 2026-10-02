"""R14 local OpenAI SSE agent client. Offline unless --run; no engine lifecycle code.

Each output/work directory must be new. Task success is separate from completion.
Only the owned runner selects serial/MTP; every API request verifies reqstat route.
"""
import argparse
import codecs
import copy
from datetime import datetime, timezone
import hashlib
import http.client
import importlib.util
import json
import math
import os
from pathlib import Path
import sys
import time
from urllib.parse import urlsplit


HERE = Path(__file__).resolve().parent
SEED = 20261001


class FatalError(RuntimeError):
    pass


class ToolCallRejected(FatalError):
    def __init__(self, diagnostic):
        self.diagnostic=str(diagnostic)
        super().__init__('invalid_tool_call: '+self.diagnostic)


from tool_recovery import ToolRecovery


def require(condition, message):
    if not condition:
        raise FatalError(message)


def now():
    return datetime.now(timezone.utc).isoformat()


def encoded(value):
    return json.dumps(value, ensure_ascii=False, allow_nan=False, separators=(',', ':')).encode('utf-8')


def digest(value):
    return hashlib.sha256(encoded(value)).hexdigest()


def reject_json_constant(value):
    raise ValueError(f'Nonstandard JSON constant: {value}')


def save_new(path, value):
    with Path(path).open('x', encoding='utf-8') as f:
        json.dump(value, f, ensure_ascii=False, allow_nan=False, indent=2)
        f.write('\n')


def update_owned(path, value):
    """Only called on an output reserved by this invocation, never old cases."""
    temporary = path.with_name(path.name + '.writing')
    save_new(temporary, value)
    os.replace(temporary, path)


def checked_base_url(value):
    u = urlsplit(value)
    require(u.scheme == 'http' and u.hostname in ('127.0.0.1', '::1', 'localhost')
            and u.port == 18871 and u.path.rstrip('/') == '/v1'
            and not u.query and not u.fragment and not u.username and not u.password,
            'Only http://loopback:18871/v1 is allowed')
    return u


class SSEDecoder:
    """Incremental UTF-8/line/event decoder, including split CRLF and data lines."""
    def __init__(self):
        self.decoder = codecs.getincrementaldecoder('utf-8')('strict')
        self.pending = ''
        self.data = []
        self.skip_lf = False

    def feed(self, chunk, final=False):
        text = self.decoder.decode(chunk, final=final)
        events = []
        for char in text:
            if self.skip_lf:
                self.skip_lf = False
                if char == '\n':
                    continue
            if char in '\r\n':
                line, self.pending = self.pending, ''
                self.skip_lf = char == '\r'
                if not line:
                    if self.data:
                        events.append('\n'.join(self.data))
                        self.data = []
                elif line.startswith('data:'):
                    value = line[5:]
                    self.data.append(value[1:] if value.startswith(' ') else value)
                # Comments/event/id/retry fields are preserved in raw SSE only.
            else:
                self.pending += char
        if final:
            require(not self.pending and not self.data, 'Truncated SSE event at EOF')
        return events


class ChatAssembler:
    def __init__(self):
        self.content = ''
        self.reasoning = ''
        self.calls = {}
        self.finish = None
        self.usage = None
        self.done = False
        self.response_id = None
        self.model = None
        self.first_nonempty_delta_s = None
        self.first_output_delta_s = None
        self.event_count = 0

    def add(self, payload, elapsed):
        require(not self.done, 'SSE event after [DONE]')
        self.event_count += 1
        if payload == '[DONE]':
            self.done = True
            return
        frame = json.loads(payload)
        require(isinstance(frame, dict), 'Invalid SSE object')
        if isinstance(frame.get('error'),dict) and frame['error'].get('code')=='invalid_tool_call':
            raise ToolCallRejected(frame['error'].get('message',''))
        require('error' not in frame, f'API SSE error: {frame}')
        for attr, key in [('response_id', 'id'), ('model', 'model')]:
            if key in frame:
                require(getattr(self, attr) in (None, frame[key]), f'Changing SSE {key}')
                setattr(self, attr, frame[key])
        if frame.get('usage') is not None:
            require(self.usage is None, 'Multiple usage records')
            self.usage = frame['usage']
        choices = frame.get('choices', [])
        require(isinstance(choices, list) and len(choices) <= 1, 'Expected one chat choice')
        for choice in choices:
            require(choice.get('index', 0) == 0, 'Unexpected choice index')
            delta = choice.get('delta', {})
            require(isinstance(delta, dict), 'Invalid delta')
            if any(v not in (None, '', [], {}) for v in delta.values()):
                if self.first_nonempty_delta_s is None:
                    self.first_nonempty_delta_s = elapsed
            if any(delta.get(k) for k in ('content', 'reasoning_content', 'tool_calls')):
                if self.first_output_delta_s is None:
                    self.first_output_delta_s = elapsed
            if delta.get('role') is not None:
                require(delta['role'] == 'assistant', 'Unexpected streamed role')
            for key, attr in [('content', 'content'), ('reasoning_content', 'reasoning')]:
                value = delta.get(key)
                if value is not None:
                    require(isinstance(value, str), f'Nonstring {key}')
                    setattr(self, attr, getattr(self, attr) + value)
            require('function_call' not in delta, 'Legacy function_call is unsupported')
            for part in delta.get('tool_calls') or []:
                index = part.get('index')
                require(type(index) is int and index >= 0, 'Invalid tool-call index')
                call = self.calls.setdefault(index, {'id': '', 'type': 'function',
                                                     'function': {'name': '', 'arguments': ''}})
                if part.get('type') is not None:
                    require(part['type'] == 'function', 'Unsupported tool-call type')
                for key in ('id',):
                    if key in part:
                        require(isinstance(part[key], str), 'Nonstring tool-call id')
                        call[key] += part[key]
                function = part.get('function', {})
                for key in ('name', 'arguments'):
                    if key in function:
                        require(isinstance(function[key], str), f'Nonstring tool {key}')
                        call['function'][key] += function[key]
            if choice.get('finish_reason') is not None:
                require(self.finish is None, 'Multiple finish reasons')
                self.finish = choice['finish_reason']

    def result(self):
        require(self.done and self.finish is not None and isinstance(self.usage, dict),
                'Missing [DONE], finish_reason or usage')
        require(list(sorted(self.calls)) == list(range(len(self.calls))), 'Noncontiguous tool indexes')
        message = {'role': 'assistant', 'content': self.content}
        if self.reasoning:
            message['reasoning_content'] = self.reasoning
        if self.calls:
            message['tool_calls'] = [self.calls[k] for k in sorted(self.calls)]
        return {'id': self.response_id, 'model': self.model, 'message': message,
                'finish_reason': self.finish, 'usage': self.usage,
                'first_nonempty_delta_s': self.first_nonempty_delta_s,
                'first_output_delta_s': self.first_output_delta_s, 'sse_events': self.event_count}


def tail_records(value):
    require(isinstance(value, dict) and isinstance(value.get('records'), list), 'Invalid reqstat tail')
    require(value.get('bad_crc', 0) == 0, 'reqstat CRC errors')
    records = value['records']
    ids = [r.get('req_seq') for r in records]
    require(all(type(x) is int and x > 0 for x in ids) and len(ids) == len(set(ids)),
            'Invalid/duplicate reqstat sequence')
    return records


def associate(before, after):
    old = {r['req_seq']: r for r in tail_records(before)}
    new = tail_records(after)
    require(all(old[r['req_seq']] == r for r in new if r['req_seq'] in old),
            'Existing reqstat record changed (possible server restart)')
    added = [r for r in new if r['req_seq'] not in old]
    require(len(added) == 1, f'Expected exactly one new reqstat record; got {len(added)}')
    require(not old or added[0]['req_seq'] > max(old), 'reqstat sequence restarted')
    return added[0]


def checked_metrics(record, usage, mode):
    expected = ('serial', 'mtp')[mode]
    require(record.get('drafter') == expected, f'Actual drafter {record.get("drafter")} != {expected}')
    for key in ('prompt_tokens', 'cached_tokens', 'output_tokens', 'proposed', 'commit', 'rounds'):
        require(type(record.get(key)) is int and record[key] >= 0, f'Invalid reqstat {key}')
    prompt, cached, output = (record[k] for k in ('prompt_tokens', 'cached_tokens', 'output_tokens'))
    require(cached <= prompt, 'Cached tokens exceed prompt tokens')
    for key in ('prefill_ms', 'decode_ms', 'ttft_ms'):
        value = record.get(key)
        require(type(value) in (float, int) and math.isfinite(value) and value >= 0, f'Invalid {key}')
    for key, expected in [('prompt_tokens', prompt), ('completion_tokens', output), ('total_tokens', prompt + output)]:
        require(type(usage.get(key)) is int and usage[key] == expected, f'usage mismatch: {key}')
    require(usage.get('prompt_tokens_details', {}).get('cached_tokens', 0) == cached,
            'usage cached token mismatch')
    if mode == 0:
        require(record['proposed'] == record['rounds'] == record['commit'] == 0,
                'Serial request has speculative counters')
    processed = prompt - cached
    return {'processed_prompt_tokens': processed,
            'effective_prefill_tokens_per_s': processed / record['prefill_ms'] * 1000 if record['prefill_ms'] else None,
            'decode_tokens_per_s': output / record['decode_ms'] * 1000 if record['decode_ms'] else None,
            'usage_matches_reqstat': True}


def check_server_policy(health, overrides, model):
    require(health.get('status') == 'ok' and health.get('model') == model, 'Health/model mismatch')
    require(health.get('in_flight', 0) == 0 and health.get('queued', 0) == 0,
            'API has another active/queued request')
    require(isinstance(overrides.get('overrides'), dict) and not overrides['overrides'],
            'Benchmark requires an empty server override table; preserve/review snapshot, do not alter policy')


class LocalAPI:
    def __init__(self, base_url):
        self.url = checked_base_url(base_url)

    def connection(self):
        # No proxy, redirects, retry or remote endpoint support.
        return http.client.HTTPConnection(self.url.hostname, 18871, timeout=1800)

    def get_json(self, path, artifact):
        conn = self.connection()
        record = {'method': 'GET', 'path': path, 'started_utc': now()}
        start = time.perf_counter()
        try:
            conn.request('GET', path, headers={'Connection': 'close'})
            response = conn.getresponse()
            raw = response.read()
            record.update(status=response.status, headers=response.getheaders(), raw_body=raw.decode('utf-8'))
            require(response.status == 200, f'GET {path}: HTTP {response.status}')
            value = json.loads(record['raw_body'])
            return value
        finally:
            record['http_wall_s'] = time.perf_counter() - start
            conn.close()
            save_new(artifact, record)

    def chat(self, body, turn_dir, turn):
        raw_body = encoded(body)
        (turn_dir / 'request-body.json').write_bytes(raw_body)  # turn_dir is exclusively new.
        conn = self.connection()
        parser, assembler = SSEDecoder(), ChatAssembler()
        started = time.perf_counter()
        offset = 0
        turn['http_started_utc'] = now()
        try:
            conn.request('POST', '/v1/chat/completions', raw_body,
                         {'Content-Type': 'application/json', 'Accept': 'text/event-stream', 'Connection': 'close'})
            response = conn.getresponse()
            turn.update(http_status=response.status, http_headers=response.getheaders())
            require(not response.getheader('X-Gdec-Overrides'), 'Server changed request policy via overrides')
            if response.status != 200:
                turn['http_error_body'] = response.read().decode('utf-8', errors='replace')
                try: error=json.loads(turn['http_error_body']).get('error',{})
                except (ValueError,AttributeError): error={}
                if response.status==502 and isinstance(error,dict) and error.get('code')=='invalid_tool_call':
                    raise ToolCallRejected(error.get('message',''))
                raise FatalError(f'Chat HTTP {response.status}: {turn["http_error_body"]}')
            require('text/event-stream' in response.getheader('Content-Type', '').lower(), 'Response is not SSE')
            with (turn_dir / 'response.sse').open('xb') as raw, (turn_dir / 'sse-timing.jsonl').open('x', encoding='utf-8') as timings:
                while True:
                    chunk = response.readline()
                    elapsed = time.perf_counter() - started
                    if not chunk:
                        for event in parser.feed(b'', final=True):
                            assembler.add(event, elapsed)
                        break
                    raw.write(chunk)
                    raw.flush()
                    timings.write(json.dumps({'offset': offset, 'bytes': len(chunk), 'elapsed_s': elapsed}) + '\n')
                    timings.flush()
                    offset += len(chunk)
                    for event in parser.feed(chunk):
                        assembler.add(event, elapsed)
            result = assembler.result()
            require(result['model'] == body['model'], 'Response model mismatch')
            return result
        finally:
            turn['http_wall_s'] = time.perf_counter() - started
            turn['sse_bytes'] = offset
            conn.close()


def tool_response(fixture, scenario, call, run_dir, allowed):
    started = time.perf_counter()
    function = call['function']
    entry = {'call': copy.deepcopy(call), 'started_utc': now(), 'error': None}
    try:
        args = json.loads(function['arguments'], parse_constant=reject_json_constant)
        if not isinstance(args, dict):
            raise ValueError('Tool arguments must be a JSON object')
        entry['parsed_arguments'] = args
        if function['name'] not in allowed:
            raise ValueError(f'Unknown tool: {function["name"]}')
        value = fixture.dispatch(scenario, function['name'], args, run_dir)
        content = value if isinstance(value, str) else encoded(value).decode('utf-8')
        entry['result'] = value
        # Returned business/tool errors remain unchanged, even if the model recovers.
        entry['returned_failure'] = isinstance(value, dict) and (bool(value.get('error')) or value.get('success') is False or value.get('ok') is False)
        nested = value.get('result') if isinstance(value, dict) else None
        entry['test_suite_failure'] = (function['name'] == 'code_action' and args.get('action') == 'test'
                                       and isinstance(nested, dict) and nested.get('success') is False)
    except Exception as error:
        entry['error'] = {'type': type(error).__name__, 'message': str(error)}
        entry['result'] = {'error': entry['error']}
        entry['returned_failure'] = True
        entry['test_suite_failure'] = False
        content = encoded(entry['result']).decode('utf-8')
    entry['wall_s'] = time.perf_counter() - started
    entry['response_content'] = content
    return entry, {'role': 'tool', 'tool_call_id': call['id'], 'content': content}


def load_fixture():
    path = HERE / 'fixture.py'
    require(path.is_file(), f'Missing fixture: {path}')
    spec = importlib.util.spec_from_file_location('r14_fixture', path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module, hashlib.sha256(path.read_bytes()).hexdigest()


def execute(args):
    checked_base_url(args.base_url)
    output, work = args.output.resolve(), args.work_dir.resolve()
    require(not output.exists() and not work.exists(), 'Output and work-dir must both be new; refusing overwrite')
    fixture, fixture_sha = load_fixture()
    fixture.DATA_VARIANT=getattr(args, 'fixture_variant', 'base')
    scenarios = ['office', 'code'] if args.scenario == 'all' else [args.scenario]
    require(all(s in fixture.SCENARIOS for s in scenarios), 'Fixture scenario unavailable')
    work.mkdir(parents=True, exist_ok=False)
    output.parent.mkdir(parents=True, exist_ok=True)
    result = {'schema': 'r14-agent-client-v1', 'status': 'running', 'identity': args.identity,
              'mode': args.mode, 'expected_drafter': ('serial', 'mtp')[args.mode], 'started_utc': now(),
              'configuration': vars(args) | {'output': str(output), 'work_dir': str(work)},
              'client_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'fixture_sha256': fixture_sha, 'tasks': [], 'records': [],
              'timing_scope': {'http_wall': 'POST start through complete SSE EOF; excludes reqstat GETs and tool execution',
                               'task_wall': 'Agent loop to final SSE completion, or final allowed tool execution; excludes independent scoring. Includes prior-turn bookkeeping/reqstat GET overhead.',
                               'first_nonempty_delta': 'First delta with any nonempty value (role included)',
                               'first_output_delta': 'First nonempty content/reasoning/tool-call delta',
                               'engine_ttft': 'Separate reqstat.ttft_ms; never substituted by client timings'},
              'fatal_error': None}
    save_new(output, result)
    api = LocalAPI(args.base_url)
    active_task = None
    try:
        models = api.get_json('/v1/models', work / 'models.json')
        require(isinstance(models.get('data'), list) and len(models['data']) == 1
                and isinstance(models['data'][0].get('id'), str) and models['data'][0]['id'],
                'Expected exactly one named model')
        model = models['data'][0]['id']
        result['model'] = model
        health = api.get_json('/health', work / 'health-before.json')
        overrides = api.get_json('/admin/overrides', work / 'overrides-before.json')
        check_server_policy(health, overrides, model)
        result['server_policy_checked'] = {'health': health, 'overrides': overrides}
        for repeat in range(1, args.repeat + 1):
            for scenario in scenarios:
                task_dir = work / f'{scenario}-{repeat:02d}'
                task_dir.mkdir()
                run_dir = task_dir / 'fixture'
                fixture.create_run(scenario, run_dir)
                setup = fixture.SCENARIOS[scenario]
                messages = [{'role': 'system', 'content': setup['system_prompt']},
                            {'role': 'user', 'content': setup['user_prompt']}]
                tools = copy.deepcopy(setup['tools'])
                allowed = {tool['function']['name'] for tool in tools}
                recovery = ToolRecovery(getattr(args, 'tool_repair', 0))
                task = {'scenario': scenario, 'repeat': repeat, 'status': 'running', 'success': False,
                        'finish': None, 'started_utc': now(), 'messages': messages, 'turns': [], 'tool_results': []}
                result['tasks'].append(task)
                active_task = task
                task_start = time.perf_counter()
                agent_finished = None
                update_owned(output, result)
                for index in range(1, args.max_turns + 1):
                    turn_dir = task_dir / f'turn-{index:02d}'
                    turn_dir.mkdir()
                    body = {'model': model, 'messages': copy.deepcopy(messages), 'tools': tools,
                            'tool_choice': 'auto', 'temperature': 0, 'enable_thinking': getattr(args, 'reasoning', 'false') == 'true',
                            'seed': SEED, 'stream': True, 'stream_options': {'include_usage': True},
                            'max_tokens': args.max_tokens}
                    turn = {'turn': index, 'artifact_dir': str(turn_dir), 'request_body': body,
                            'messages_sha256': digest(body['messages']), 'request_sha256': digest(body)}
                    task['turns'].append(turn)
                    try:
                        before = api.get_json('/reqstat/tail?n=128', turn_dir / 'reqstat-before.json')
                        tail_records(before)
                        assembled = api.chat(body, turn_dir, turn)
                        model_completed = time.perf_counter()
                        turn['assembled_response'] = assembled
                        after = api.get_json('/reqstat/tail?n=128', turn_dir / 'reqstat-after.json')
                        raw_record = associate(before, after)
                        metrics = checked_metrics(raw_record, assembled['usage'], args.mode)
                        turn['reqstat'] = raw_record
                        turn['metrics'] = metrics
                        flat = {'identity': args.identity, 'scenario': scenario, 'repeat': repeat, 'turn': index,
                                'mode': args.mode, **raw_record, **metrics, 'http_wall_s': turn['http_wall_s'],
                                'first_nonempty_delta_s': assembled['first_nonempty_delta_s'],
                                'first_output_delta_s': assembled['first_output_delta_s'],
                                'request_sha256': turn['request_sha256'], 'messages_sha256': turn['messages_sha256'],
                                'artifact_dir': str(turn_dir), 'usage': assembled['usage']}
                        result['records'].append(flat)
                        message = assembled['message']
                        messages.append(message)
                        finish = assembled['finish_reason']
                        calls = message.get('tool_calls', [])
                        if finish == 'stop' and not calls:
                            task['finish'] = 'natural_final'
                            agent_finished = model_completed
                            break
                        if finish != 'tool_calls' or not calls:
                            task['finish'] = f'model_finish:{finish}'
                            agent_finished = model_completed
                            break
                        call_ids = [call['id'] for call in calls]
                        if any(not x for x in call_ids) or len(set(call_ids)) != len(call_ids):
                            task['finish'] = 'invalid_tool_call_ids'
                            agent_finished = model_completed
                            break
                        for call in calls:
                            entry, response = tool_response(fixture, scenario, call, run_dir, allowed)
                            entry['turn'] = index
                            task['tool_results'].append(entry)
                            messages.append(response)
                        agent_finished = time.perf_counter()
                        # Ordinary successful tool turns receive no extra correction prompt.
                    except ToolCallRejected as error:
                        # Protocol/model failure is a failed task, not failure of the
                        # whole benchmark campaign. Preserve the failed attempt too.
                        turn['protocol_error'] = str(error)
                        try:
                            after = api.get_json('/reqstat/tail?n=128', turn_dir / 'reqstat-failed.json')
                            turn['failed_reqstat'] = associate(before, after)
                        except Exception as metric_error:
                            turn['failed_reqstat_error'] = str(metric_error)
                        agent_finished = time.perf_counter()
                        feedback=recovery.feedback('invalid_tool_call',error.diagnostic,tools)
                        if feedback:
                            task['repair_used']=True
                            turn['recovery_feedback']=feedback
                            messages.append(feedback)
                            continue
                        task['finish'] = 'api_error'
                        break
                    finally:
                        save_new(turn_dir / 'turn.json', turn)
                        update_owned(output, result)
                else:
                    task['finish'] = 'max_turns'
                task['task_wall_s'] = agent_finished - task_start
                scoring_start = time.perf_counter()
                try:
                    evaluation = fixture.evaluate(scenario, run_dir, messages)
                    require(isinstance(evaluation, dict) and type(evaluation.get('success')) is bool,
                            'Fixture evaluate must return a boolean success')
                    task['evaluation'] = evaluation
                    task['success'] = task['finish'] == 'natural_final' and evaluation['success']
                except Exception as error:
                    task['evaluation'] = {'success': False, 'details': {'error_type': type(error).__name__, 'error': str(error)}}
                task.update(status='complete', finished_utc=now(), scoring_wall_s=time.perf_counter() - scoring_start,
                            tool_failure_count=sum(bool(e['returned_failure']) for e in task['tool_results']),
                            test_suite_failure_count=sum(bool(e['test_suite_failure']) for e in task['tool_results']))
                save_new(task_dir / 'task.json', task)
                update_owned(output, result)
                active_task = None
        health_after = api.get_json('/health', work / 'health-after.json')
        overrides_after = api.get_json('/admin/overrides', work / 'overrides-after.json')
        check_server_policy(health_after, overrides_after, model)
        result['server_policy_after'] = {'health': health_after, 'overrides': overrides_after}
        result['status'] = 'complete'
    except BaseException as error:
        result['status'] = 'fatal'
        result['fatal_error'] = {'type': type(error).__name__, 'message': str(error)}
        if active_task is not None:
            active_task.update(status='fatal', finish='client_fatal', success=False)
        raise
    finally:
        result['finished_utc'] = now()
        result['all_tasks_success'] = bool(result['tasks']) and all(t['success'] for t in result['tasks'])
        result['completed_tasks'] = sum(t['status'] == 'complete' for t in result['tasks'])
        update_owned(output, result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tool-repair', type=int, choices=[0,1,2], default=0)
    parser.add_argument('--fixture-variant', choices=['base','amounts-v2'], default='base')
    parser.add_argument('--run', action='store_true')
    parser.add_argument('--base-url', default='http://127.0.0.1:18871/v1')
    parser.add_argument('--scenario', choices=['office', 'code', 'all'], default='all')
    parser.add_argument('--repeat', type=int, default=1)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--mode', type=int, choices=[0, 1])
    mode.add_argument('--mode0', dest='mode', action='store_const', const=0)
    mode.add_argument('--mode1', dest='mode', action='store_const', const=1)
    parser.add_argument('--identity', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--work-dir', type=Path, required=True)
    parser.add_argument('--max-turns', type=int, default=12)
    parser.add_argument('--max-tokens', type=int, default=2048)
    parser.add_argument('--reasoning', choices=['false', 'true'], default='false',
                        help='Explicit experimental factor; never changed by retry')
    args = parser.parse_args()
    checked_base_url(args.base_url)
    require(args.repeat >= 1 and 1 <= args.max_turns <= 24 and 1 <= args.max_tokens <= 8192,
            'Require repeat>=1, max-turns 1..24, max-tokens 1..8192')
    if not args.run:
        print(json.dumps({'offline': True, 'network_calls': 0, 'files_written': False,
                          'configuration': vars(args)}, ensure_ascii=False, default=str, indent=2))
        return 0
    result = execute(args)
    print(json.dumps({'status': result['status'], 'all_tasks_success': result['all_tasks_success'],
                      'tasks': len(result['tasks']), 'records': len(result['records']), 'output': str(args.output)}, ensure_ascii=False))
    return 0  # Business/tool failures are explicit data, not infrastructure exit failures.


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (Exception, KeyboardInterrupt) as error:
        print(f'R14 client fatal: {type(error).__name__}: {error}', file=sys.stderr)
        raise SystemExit(1)
