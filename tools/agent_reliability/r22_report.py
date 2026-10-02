"""Summarize paired task evidence; no model calls or outcome-dependent retries."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
from fixture import normalize_office, exact_typed


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, ensure_ascii=False,
        separators=(',', ':')).encode()).hexdigest()


def case(root, item):
    result = read(root / item['tag'] / 'result.json')
    identity_path = root / (item['tag'] + '.identity.json')
    identity = read(identity_path) if identity_path.exists() else {}
    task = result['tasks'][0]
    records = []
    protocol_errors = []
    for turn in task['turns']:
        if 'reqstat' in turn:
            records.append(turn['reqstat'])
        elif 'failed_reqstat' in turn:
            records.append(turn['failed_reqstat'])
        if turn.get('protocol_error'):
            protocol_errors.append(turn['protocol_error'])
    assert len({r['req_seq'] for r in records}) == len(records)
    assert all(r['drafter'] == 'mtp' for r in records)
    total_pp = sum(r['prefill_ms'] for r in records) / 1000
    total_tg = sum(r['decode_ms'] for r in records) / 1000
    processed = sum(r['prompt_tokens'] - r['cached_tokens'] for r in records)
    output = sum(r['output_tokens'] for r in records)
    edits, baseline_failures, post_edit_failures = 0, 0, 0
    sql_calls, tool_errors, submissions = 0, [], 0
    for event in task['tool_results']:
        name = event['call']['function']['name']
        args = event.get('parsed_arguments', {})
        value = event.get('result')
        if event.get('returned_failure'):
            tool_errors.append(dict(tool=name, turn=event['turn'], output=value))
        if name == 'query_ledger':
            sql_calls += 1
        if name == 'submit_result':
            submissions += 1
        if name == 'code_action':
            if args.get('action') == 'replace' and isinstance(value, dict) and value.get('ok'):
                edits += 1
            nested = value.get('result', {}) if isinstance(value, dict) else {}
            if args.get('action') == 'test' and nested.get('success') is False:
                if edits:
                    post_edit_failures += 1
                else:
                    baseline_failures += 1
    evaluation = task['evaluation']
    details = evaluation.get('details', {})
    office_checks = None
    if item['scenario'] == 'office' and isinstance(details.get('submitted'), dict):
        expected = normalize_office(details['expected'])
        submitted = normalize_office(details['submitted'])
        office_checks = {'totals': all(exact_typed(submitted.get(k), expected[k]) for k in
            ('eligible_invoice_count','receivable_cents','applied_payment_cents','balance_cents'))}
        office_checks.update({k:exact_typed(submitted.get(k), expected[k]) for k in
            ('by_department','by_year','overdue_top5','invoice_exclusions','payment_exclusions')})
    first = task['turns'][0]
    prompts = dict(first['request_body'])
    assert prompts['temperature'] == 0 and prompts['enable_thinking'] is True
    terminal = task['turns'][-1].get('assembled_response', {})
    message = terminal.get('message', {})
    if task['success']:
        failure_category = None
    elif task['finish'] == 'model_finish:length':
        failure_category = 'output_budget_exhausted'
    elif protocol_errors:
        failure_category = 'protocol_failure'
    elif task['finish'] == 'max_turns':
        failure_category = 'turn_budget_exhausted'
    elif item['scenario'] == 'office' and submissions and not evaluation.get('checks', {}).get('exact_structured_ground_truth'):
        failure_category = 'wrong_statistics'
    else:
        failure_category = 'independent_grading_or_delivery_failure'
    return dict(tag=item['tag'], variant=item['variant'], scenario=item['scenario'], arm=item['arm'],
        success=task['success'], finish=task['finish'], task_wall_s=task['task_wall_s'],
        engine_ready_s=identity.get('ready_s'),
        failure_category=failure_category,
        terminal_reasoning_chars=len(message.get('reasoning_content', '')),
        terminal_content_chars=len(message.get('content') or ''),
        terminal_tool_calls=len(message.get('tool_calls') or []),
        requests=len(task['turns']), metric_records=len(records), output_tokens=output,
        processed_prompt_tokens=processed, total_prompt_tokens=sum(r['prompt_tokens'] for r in records),
        cached_prompt_tokens=sum(r['cached_tokens'] for r in records),
        prefill_s=total_pp, decode_s=total_tg,
        weighted_fresh_prefill_tps=processed / total_pp if total_pp else None,
        weighted_decode_tps=output / total_tg if total_tg else None,
        max_prompt_tokens=max(r['prompt_tokens'] for r in records),
        initial_cached_tokens=records[0]['cached_tokens'],
        engine_ttft_median_s=statistics.median(r['ttft_ms'] for r in records) / 1000,
        http_wall_s=sum(t.get('http_wall_s', 0) for t in task['turns']),
        tools=len(task['tool_results']), sql_calls=sql_calls, code_edits=edits,
        baseline_expected_test_failures=baseline_failures, post_edit_test_failures=post_edit_failures,
        protocol_error_count=len(protocol_errors), protocol_errors=protocol_errors,
        protocol_recovery_count=sum('recovery_feedback' in t for t in task['turns']),
        tool_error_count=len(tool_errors), tool_errors=tool_errors, submissions=submissions,
        checks=evaluation.get('checks', {}),
        failed_checks=[k for k,v in evaluation.get('checks', {}).items() if not v],
        private_code_tests=details.get('private_test'),
        office_exact_values=evaluation.get('checks', {}).get('exact_structured_ground_truth') if item['scenario']=='office' else None,
        office_checks_by_category=office_checks,
        initial_request_sha256=digest(prompts), fixture_provenance=result['r22_provenance'],
        artifact_sha256=hashlib.sha256((root/item['tag']/'result.json').read_bytes()).hexdigest())


def summarize(root):
    manifest = read(root / 'manifest.json')
    cases = [case(root, item) for item in manifest['records']]
    profile_env = {}
    for item in manifest['records']:
        identity = read(root / (item['tag'] + '.identity.json'))
        assert identity['engine_sha256'] == manifest['engine_sha256']
        env = {k:v for k,v in identity['env'].items() if k != 'GDEC_REQSTAT_DIR'}
        if item['arm'] in profile_env:
            assert profile_env[item['arm']] == env, 'Profile changed mid-campaign'
        profile_env[item['arm']] = env
    pairs = []
    for variant, scenario, order in manifest['pairs']:
        selected = {c['arm']:c for c in cases if c['variant']==variant and c['scenario']==scenario}
        if len(selected) < 2:
            continue
        fast, exact = selected['hybrid'], selected['exact']
        assert fast['initial_request_sha256'] == exact['initial_request_sha256']
        assert fast['fixture_provenance'] == exact['fixture_provenance']
        assert fast['initial_cached_tokens'] == exact['initial_cached_tokens'] == 0
        pairs.append(dict(variant=variant,scenario=scenario,order=order,identical_initial_inputs=True,
            hybrid_success=fast['success'],exact_success=exact['success'],
            hybrid_wall_s=fast['task_wall_s'],exact_wall_s=exact['task_wall_s'],
            both_succeeded=fast['success'] and exact['success'],
            hybrid_wall_reduction_percent=(100*(1-fast['task_wall_s']/exact['task_wall_s'])
                if fast['success'] and exact['success'] else None),
            hybrid_output_tokens=fast['output_tokens'],exact_output_tokens=exact['output_tokens'],
            note='Independent agents may use different plans/tokens; wall difference is not a pure kernel speedup'))
    aggregates = []
    for scenario in ('office','code'):
        for arm in ('hybrid','exact'):
            group = [c for c in cases if c['arm']==arm and c['scenario']==scenario]
            if not group:
                continue
            successful = [c for c in group if c['success']]
            pp = sum(c['prefill_s'] for c in group)
            tg = sum(c['decode_s'] for c in group)
            aggregates.append(dict(scenario=scenario,arm=arm,tasks=len(group),
                success=sum(c['success'] for c in group),
                median_attempt_wall_s=statistics.median(c['task_wall_s'] for c in group),
                median_successful_wall_s=(statistics.median(c['task_wall_s'] for c in successful) if successful else None),
                total_wall_s=sum(c['task_wall_s'] for c in group),
                weighted_fresh_prefill_tps=sum(c['processed_prompt_tokens'] for c in group)/pp,
                weighted_decode_tps=sum(c['output_tokens'] for c in group)/tg,
                protocol_errors=sum(c['protocol_error_count'] for c in group),
                tool_errors=sum(c['tool_error_count'] for c in group),
                post_edit_test_failures=sum(c['post_edit_test_failures'] for c in group)))
    return dict(schema='r22-route-quality-v1',completed='completed_utc' in manifest,
        experiment={k:v for k,v in manifest.items() if k not in ('weights','records','active_case')},
        engine_sha256=manifest['engine_sha256'],api_sha256=manifest['api_sha256'],
        profile_env=profile_env,
        paired_instances=len(pairs),cases=cases,pairs=pairs,aggregates=aggregates,
        limitations=['Few selected task instances; counts do not estimate population success rates',
            'Greedy MTP only; no serial, stochastic, concurrent or multimodal comparison',
            'Task wall excludes model load and independent scoring; includes cold first request',
            'Same R21 binary with two arithmetic profiles; not a comparison of all R19/R21 changes',
            'No Octop memory or previous run artifacts provided to either model',
            'Code tools use a bounded AST interpreter, not unrestricted Python; execution-rule errors are recorded separately from private test failures',
            'Task-family aggregates combine different instances; they are not repeated-run variance estimates',
            'No HTML/PPTX layout grading; synthetic ledger and bounded function tasks only'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('evidence', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    report = summarize(args.evidence.resolve())
    if args.output:
        with args.output.open('x', encoding='utf-8') as f:
            json.dump(report,f,ensure_ascii=False,indent=2)
    print(json.dumps({'completed':report['completed'],'aggregates':report['aggregates'],
        'pairs':report['pairs']},ensure_ascii=False,indent=2))
