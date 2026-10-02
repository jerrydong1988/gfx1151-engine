"""CPU-only R14 client tests. No HTTP/API/GPU/subprocess calls."""
import argparse
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location('r14_agent_client', HERE / 'r14_agent_client.py')
c = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(c)


def frame(delta=None, finish=None, usage=None):
    value = {'id': 'chat-1', 'model': 'model', 'choices': []}
    if delta is not None or finish is not None:
        value['choices'] = [{'index': 0, 'delta': delta or {}, 'finish_reason': finish}]
    if usage is not None:
        value['usage'] = usage
    return 'data: ' + json.dumps(value, ensure_ascii=False) + '\r\n\r\n'


def rec(seq=1, mode=0, cached=80):
    return {'req_seq': seq, 'ts_ms': seq * 1000, 'finish': 'stop', 'drafter': ('serial', 'mtp')[mode],
            'prompt_tokens': 100, 'cached_tokens': cached, 'output_tokens': 10,
            'prefill_ms': 200, 'decode_ms': 100, 'ttft_ms': 210,
            'proposed': 12 if mode else 0, 'commit': 10 if mode else 0, 'rounds': 4 if mode else 0}


def usage(cached=80, output=10):
    return {'prompt_tokens': 100, 'completion_tokens': output, 'total_tokens': 100 + output,
            'prompt_tokens_details': {'cached_tokens': cached}}


class ParserTests(unittest.TestCase):
    def test_all_byte_fragmentation_and_interleaved_tools(self):
        text = ': keepalive\r\n\r\n' + frame({'role': 'assistant'}) + frame({'content': '中文'})
        text += frame({'tool_calls': [{'index': 1, 'id': 'b', 'function': {'name': 'read_', 'arguments': '{'}},
                                      {'index': 0, 'id': 'a', 'function': {'name': 'query_', 'arguments': '{'}}]})
        text += frame({'tool_calls': [{'index': 0, 'function': {'name': 'ledger', 'arguments': '"键":1}'}},
                                      {'index': 1, 'function': {'name': 'resource', 'arguments': '"path":"a"}'}}]})
        text += frame(finish='tool_calls') + frame(usage=usage()) + 'data: [DONE]\r\n\r\n'
        decoder, assembler = c.SSEDecoder(), c.ChatAssembler()
        for i, byte in enumerate(text.encode()):
            for event in decoder.feed(bytes([byte])):
                assembler.add(event, i / 1000)
        decoder.feed(b'', final=True)
        result = assembler.result()
        self.assertEqual(result['message']['content'], '中文')
        calls = result['message']['tool_calls']
        self.assertEqual([x['id'] for x in calls], ['a', 'b'])
        self.assertEqual(calls[0]['function'], {'name': 'query_ledger', 'arguments': '{"键":1}'})
        self.assertEqual(calls[1]['function']['name'], 'read_resource')
        self.assertLess(result['first_nonempty_delta_s'], result['first_output_delta_s'])

    def test_multiple_data_lines(self):
        decoder = c.SSEDecoder()
        self.assertEqual(decoder.feed(b'data: one\ndata:two\n\n'), ['one\ntwo'])

    def test_cr_and_lf_split(self):
        decoder = c.SSEDecoder()
        self.assertEqual(decoder.feed(b'data: a\r'), [])
        self.assertEqual(decoder.feed(b'\n\r'), ['a'])
        self.assertEqual(decoder.feed(b'\n'), [])

    def test_truncated_utf8(self):
        decoder = c.SSEDecoder()
        decoder.feed(b'\xe4')
        with self.assertRaises(UnicodeDecodeError):
            decoder.feed(b'', final=True)

    def test_truncated_event(self):
        decoder = c.SSEDecoder()
        decoder.feed(b'data: no boundary')
        with self.assertRaises(c.FatalError):
            decoder.feed(b'', final=True)

    def test_missing_done_or_usage(self):
        with self.assertRaises(c.FatalError):
            c.ChatAssembler().result()

    def test_api_error(self):
        with self.assertRaises(c.FatalError):
            c.ChatAssembler().add('{"error":{"message":"failed"}}', 0)

    def test_empty_wait_not_visible_delta(self):
        assembler = c.ChatAssembler()
        assembler.add(json.dumps({'choices': [{'delta': {'content': ''}}]}), 1)
        self.assertIsNone(assembler.first_nonempty_delta_s)
        assembler.add(json.dumps({'choices': [{'delta': {'content': 'a'}}]}), 2)
        self.assertEqual(assembler.first_output_delta_s, 2)

    def test_events_after_done_rejected(self):
        assembler = c.ChatAssembler()
        assembler.add('[DONE]', 0)
        with self.assertRaises(c.FatalError):
            assembler.add('{}', 0)


class MetricsTests(unittest.TestCase):
    def test_effective_prefill_excludes_cache(self):
        value = c.checked_metrics(rec(), usage(), 0)
        self.assertEqual(value['processed_prompt_tokens'], 20)
        self.assertEqual(value['effective_prefill_tokens_per_s'], 100)
        self.assertEqual(value['decode_tokens_per_s'], 100)

    def test_zero_decode_is_null(self):
        record = rec()
        record.update(output_tokens=1, decode_ms=0)
        self.assertIsNone(c.checked_metrics(record, usage(output=1), 0)['decode_tokens_per_s'])

    def test_bad_route_fatal(self):
        with self.assertRaises(c.FatalError):
            c.checked_metrics(rec(mode=0), usage(), 1)

    def test_usage_mismatch(self):
        with self.assertRaises(c.FatalError):
            c.checked_metrics(rec(), usage(cached=0), 0)

    def test_bad_numbers(self):
        for key, value in [('decode_ms', float('nan')), ('cached_tokens', 101), ('output_tokens', -1), ('rounds', True)]:
            with self.subTest(key=key), self.assertRaises(c.FatalError):
                c.checked_metrics(rec() | {key: value}, usage(), 0)

    def test_exact_one_new_record(self):
        self.assertEqual(c.associate({'records': [rec(1)]}, {'records': [rec(1), rec(2)]})['req_seq'], 2)

    def test_zero_or_multiple_new_rejected(self):
        for new in [[rec(1)], [rec(1), rec(2), rec(3)]]:
            with self.assertRaises(c.FatalError):
                c.associate({'records': [rec(1)]}, {'records': new})

    def test_restart_and_changed_record_rejected(self):
        for new in [[rec(1)], [rec(2) | {'ts_ms': 42}, rec(3)]]:
            with self.assertRaises(c.FatalError):
                c.associate({'records': [rec(2)]}, {'records': new})

    def test_bad_crc_rejected(self):
        with self.assertRaises(c.FatalError):
            c.tail_records({'records': [], 'bad_crc': 1})

    def test_urls(self):
        for value in ['http://127.0.0.1:18871/v1', 'http://[::1]:18871/v1/', 'http://localhost:18871/v1']:
            c.checked_base_url(value)
        for value in ['http://example.com:18871/v1', 'https://127.0.0.1:18871/v1', 'http://127.0.0.1:80/v1',
                      'http://u@127.0.0.1:18871/v1', 'http://127.0.0.1:18871/v1?x=y']:
            with self.assertRaises(c.FatalError):
                c.checked_base_url(value)


class FakeFixture:
    SCENARIOS = {s: {'system_prompt': 'System', 'user_prompt': 'User',
                    'tools': [{'type': 'function', 'function': {'name': 'read_resource'}}]} for s in ('office', 'code')}

    @staticmethod
    def create_run(scenario, path):
        path.mkdir()

    @staticmethod
    def dispatch(scenario, name, args, path):
        return {'ok': True, 'result': args}

    @staticmethod
    def evaluate(scenario, path, messages):
        return {'success': True, 'details': {'fixture': 'synthetic'}}


class FakeAPI:
    fail_route = False
    never_final = False

    def __init__(self, base):
        self.rows = []
        self.calls = 0

    def get_json(self, path, artifact):
        if path == '/v1/models':
            value = {'data': [{'id': 'model'}]}
        elif path == '/health':
            value = {'status': 'ok', 'model': 'model', 'in_flight': 0, 'queued': 0}
        elif path == '/admin/overrides':
            value = {'overrides': {}}
        else:
            value = {'records': copy.deepcopy(self.rows)}
        c.save_new(artifact, value)
        return value

    def chat(self, body, path, turn):
        self.calls += 1
        self.rows.append(rec(self.calls, mode=1 if self.fail_route else 0))
        turn['http_wall_s'] = .4
        message = {'role': 'assistant', 'content': ''}
        finish = 'stop'
        if self.calls == 1 or self.never_final:
            message['tool_calls'] = [{'id': 'call-1', 'type': 'function',
                                     'function': {'name': 'read_resource', 'arguments': '{invalid'}}]
            finish = 'tool_calls'
        return {'model': 'model', 'message': message, 'usage': usage(), 'finish_reason': finish,
                'first_nonempty_delta_s': .1, 'first_output_delta_s': .1}


class LoopTests(unittest.TestCase):
    def args(self, root, **changes):
        value = dict(run=True, base_url='http://127.0.0.1:18871/v1', scenario='office', repeat=1, mode=0,
                     identity='CPU-ONLY', output=root / 'out.json', work_dir=root / 'artifacts', max_turns=12,
                     max_tokens=2048, reasoning='false')
        value.update(changes)
        return argparse.Namespace(**value)

    def test_unknown_and_invalid_tools_pass_errors_unchanged(self):
        for function in [{'name': 'unknown', 'arguments': '{}'}, {'name': 'read_resource', 'arguments': '{oops'},
                         {'name': 'read_resource', 'arguments': '{"x":NaN}'}]:
            call = {'id': 'x', 'type': 'function', 'function': function}
            entry, response = c.tool_response(FakeFixture, 'office', call, Path('.'), {'read_resource'})
            self.assertTrue(entry['returned_failure'])
            self.assertEqual(json.loads(response['content']), entry['result'])

    def test_fixture_error_result_unchanged(self):
        value = {'ok': False, 'error': {'type': 'ActualFailure', 'message': 'fixture failed'}}
        with patch.object(FakeFixture, 'dispatch', return_value=value):
            call = {'id': 'a', 'function': {'name': 'read_resource', 'arguments': '{}'}}
            entry, response = c.tool_response(FakeFixture, 'office', call, Path('.'), {'read_resource'})
        self.assertEqual(json.loads(response['content']), value)
        self.assertTrue(entry['returned_failure'])

    def test_complete_with_recovered_error_preserves_messages(self):
        with (tempfile.TemporaryDirectory() as directory, patch.object(c, 'load_fixture', return_value=(FakeFixture, 'fake')),
                patch.object(c, 'LocalAPI', FakeAPI)):
            args = self.args(Path(directory))
            result = c.execute(args)
            self.assertEqual(result['status'], 'complete')
            self.assertTrue(result['all_tasks_success'])
            task = result['tasks'][0]
            self.assertEqual(task['tool_failure_count'], 1)
            second = task['turns'][1]['request_body']['messages']
            self.assertEqual([m['role'] for m in second], ['system', 'user', 'assistant', 'tool'])
            self.assertEqual(len(result['records']), 2)
            self.assertNotIn('drafter', task['turns'][0]['request_body'])
            self.assertEqual(task['turns'][0]['request_body']['max_tokens'], 2048)
            self.assertTrue((args.work_dir / 'office-01/turn-01/turn.json').exists())
            self.assertEqual(json.loads(args.output.read_text())['status'], 'complete')
            with self.assertRaises(c.FatalError):
                c.execute(args)

    def test_max_turns_business_failure(self):
        with (tempfile.TemporaryDirectory() as directory, patch.object(c, 'load_fixture', return_value=(FakeFixture, 'fake')),
                patch.object(c, 'LocalAPI', FakeAPI), patch.object(FakeAPI, 'never_final', True)):
            result = c.execute(self.args(Path(directory), max_turns=1))
            self.assertEqual(result['status'], 'complete')
            self.assertFalse(result['all_tasks_success'])
            self.assertEqual(result['tasks'][0]['finish'], 'max_turns')

    def test_drafter_failure_is_fatal_and_saved(self):
        with (tempfile.TemporaryDirectory() as directory, patch.object(c, 'load_fixture', return_value=(FakeFixture, 'fake')),
                patch.object(c, 'LocalAPI', FakeAPI), patch.object(FakeAPI, 'fail_route', True)):
            args = self.args(Path(directory))
            with self.assertRaises(c.FatalError):
                c.execute(args)
            result = json.loads(args.output.read_text())
            self.assertEqual(result['status'], 'fatal')
            self.assertIn('drafter', result['fatal_error']['message'])
            self.assertTrue((args.work_dir / 'office-01/turn-01/turn.json').exists())

    def test_nested_test_failure_distinct_from_tool_execution_error(self):
        value = {'ok': True, 'result': {'success': False, 'passed': 3, 'total': 4}}
        call = {'id': 'a', 'function': {'name': 'code_action', 'arguments': '{"action":"test"}'}}
        with patch.object(FakeFixture, 'dispatch', return_value=value):
            entry, response = c.tool_response(FakeFixture, 'code', call, Path('.'), {'code_action'})
        self.assertTrue(entry['test_suite_failure'])
        self.assertFalse(entry['returned_failure'])
        self.assertEqual(json.loads(response['content']), value)

    def test_scoring_clock_excluded_from_agent_clock(self):
        clock = [0.0]

        def evaluate(*unused):
            clock[0] = 100.0
            return {'success': True, 'details': {}}

        with (tempfile.TemporaryDirectory() as directory, patch.object(c, 'load_fixture', return_value=(FakeFixture, 'fake')),
              patch.object(c, 'LocalAPI', FakeAPI), patch.object(c.time, 'perf_counter', side_effect=lambda: clock[0]),
              patch.object(FakeFixture, 'evaluate', side_effect=evaluate)):
            result = c.execute(self.args(Path(directory)))
        self.assertEqual(result['tasks'][0]['task_wall_s'], 0)
        self.assertEqual(result['tasks'][0]['scoring_wall_s'], 100)

    def test_business_failure_is_complete_not_success(self):
        with (tempfile.TemporaryDirectory() as directory, patch.object(c, 'load_fixture', return_value=(FakeFixture, 'fake')),
              patch.object(c, 'LocalAPI', FakeAPI), patch.object(FakeFixture, 'evaluate', return_value={'success': False, 'details': {}})):
            result = c.execute(self.args(Path(directory)))
        self.assertEqual(result['status'], 'complete')
        self.assertFalse(result['all_tasks_success'])
        self.assertEqual(result['tasks'][0]['finish'], 'natural_final')

    def test_network_error_is_fatal_and_saved(self):
        with (tempfile.TemporaryDirectory() as directory, patch.object(c, 'load_fixture', return_value=(FakeFixture, 'fake')),
              patch.object(c, 'LocalAPI', FakeAPI), patch.object(FakeAPI, 'chat', side_effect=OSError('synthetic offline network failure'))):
            args = self.args(Path(directory))
            with self.assertRaises(OSError):
                c.execute(args)
            result = json.loads(args.output.read_text())
            self.assertEqual(result['status'], 'fatal')
            self.assertEqual(result['fatal_error']['type'], 'OSError')
            self.assertEqual(len(result['records']), 0)
            self.assertTrue((args.work_dir / 'office-01/turn-01/turn.json').exists())

    def test_server_override_or_busy_rejected(self):
        good = {'status': 'ok', 'model': 'model', 'in_flight': 0, 'queued': 0}
        c.check_server_policy(good, {'overrides': {}}, 'model')
        with self.assertRaises(c.FatalError):
            c.check_server_policy(good, {'overrides': {'temperature': {'mode': 'force', 'value': 1}}}, 'model')
        with self.assertRaises(c.FatalError):
            c.check_server_policy(good | {'in_flight': 1}, {'overrides': {}}, 'model')


if __name__ == '__main__':
    unittest.main(verbosity=2)
