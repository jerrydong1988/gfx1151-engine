"""Read frozen R23 evidence, including failed attempts and regeneration costs."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


def digest(value):
    return hashlib.sha256(json.dumps(value, ensure_ascii=False, sort_keys=True,
        separators=(',', ':')).encode()).hexdigest()


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def request_without_cap(body):
    return {key:value for key,value in body.items() if key != 'max_tokens'}


def records_for(task):
    records = [turn.get('reqstat', turn.get('failed_reqstat')) for turn in task['turns']]
    assert all(record is not None for record in records), 'Unaccounted request cost'
    assert len({r['req_seq'] for r in records}) == len(records), 'Duplicate metric record'
    assert all(r['drafter'] == 'mtp' and r['prompt_tokens'] >= r['cached_tokens'] >= 0 for r in records)
    return records


def retry_audit(task):
    """Check actual history/cap/dispatch evidence instead of trusting a retry label."""
    audits = []
    turns = task['turns']
    for index, turn in enumerate(turns):
        decision = turn.get('budget_decision')
        if not decision:
            continue
        assert turn['assembled_response']['finish_reason'] == 'length'
        dispatched = [e for e in task['tool_results'] if e['turn'] == turn['turn']]
        assert not dispatched, 'Truncated response dispatched tools'
        row = dict(turn=turn['turn'], reason=decision['reason'], retried=decision['retry'],
            old_cap=turn['request_body']['max_tokens'], discarded_output_tokens=turn['reqstat']['output_tokens'],
            discarded_request_wall_s=turn['http_wall_s'], discarded_prefill_s=turn['reqstat']['prefill_ms']/1000,
            discarded_decode_s=turn['reqstat']['decode_ms']/1000, tools_dispatched=0)
        if decision['retry']:
            assert index+1 < len(turns), 'Promised regeneration did not occur'
            following = turns[index+1]
            assert request_without_cap(turn['request_body']) == request_without_cap(following['request_body'])
            assert following['request_body']['max_tokens'] == decision['next_cap'] > row['old_cap']
            assert turn['messages_sha256'] == following['messages_sha256'] == decision['retry_messages_sha256']
            prior_message = turn['assembled_response']['message']
            next_message = following.get('assembled_response', {}).get('message', {})
            reasoning = prior_message.get('reasoning_content') or ''
            row.update(new_cap=decision['next_cap'], identical_valid_request_except_cap=True,
                valid_messages_sha256=turn['messages_sha256'],
                original_cached_tokens=turn['reqstat']['cached_tokens'],
                regeneration_cached_tokens=following.get('reqstat',following.get('failed_reqstat',{})).get('cached_tokens'),
                regeneration_output_tokens=following.get('reqstat',following.get('failed_reqstat',{})).get('output_tokens'),
                regeneration_wall_s=following.get('http_wall_s'),
                truncated_reasoning_chars=len(reasoning),
                reasoning_prefix_preserved=((next_message.get('reasoning_content') or '').startswith(reasoning)
                                            if reasoning else None))
        audits.append(row)
    assert sum(a['retried'] for a in audits) <= 1, 'More than one escalation'
    return audits


def public_case(root, item):
    path = root/item['tag']/'result.json'
    result = read(path)
    assert result['status'] == 'complete', 'Incomplete or infrastructure-fatal case'
    task = result['tasks'][0]
    records = records_for(task)
    retries = retry_audit(task)
    evaluation = task.get('evaluation', {})
    details = evaluation.get('details', {})
    checks = evaluation.get('checks', {})
    guard = details.get('delivery_guard', {}).get('events', [])
    protocol_errors = [t['protocol_error'] for t in task['turns'] if 'protocol_error' in t]
    tool_failures = []
    edits = baseline_failures = edited_failures = 0
    submitted_code_sha256 = None
    actions = []
    for entry in task['tool_results']:
        name = entry['call']['function']['name']
        args = entry.get('parsed_arguments', {})
        value = entry.get('result')
        actions.append([name,args])
        if entry.get('returned_failure'):
            error = value.get('error', {}) if isinstance(value,dict) else {}
            error_code = error.get('code') if isinstance(error,dict) else None
            tool_failures.append(dict(turn=entry['turn'],tool=name,code=error_code,
                error_type=error.get('type') if isinstance(error,dict) else None,
                category='delivery_guard' if error_code=='ledger_reconciliation_failed' else 'ordinary_tool_error'))
        if name == 'code_action':
            if args.get('action') == 'replace' and isinstance(value,dict) and value.get('ok'):
                edits += 1
                submitted_code_sha256 = value.get('result',{}).get('source_sha256')
            nested = value.get('result', {}) if isinstance(value,dict) else {}
            if args.get('action') == 'test' and nested.get('success') is False:
                if edits:
                    edited_failures += 1
                else:
                    baseline_failures += 1
    first = task['turns'][0]['request_body']
    assert first['temperature'] == 0 and first['enable_thinking'] is True
    assert first['max_tokens'] == (8192 if item['policy']=='fixed8192' else 4096)
    assert records[0]['cached_tokens'] == 0, 'Initial cache was not cold'
    pp = sum(r['prefill_ms'] for r in records)/1000
    tg = sum(r['decode_ms'] for r in records)/1000
    output = sum(r['output_tokens'] for r in records)
    fresh = sum(r['prompt_tokens']-r['cached_tokens'] for r in records)
    rejection_count = sum(not event['accepted'] for event in guard)
    if task['success']:
        failure = None
    elif task['finish'] == 'delivery_guard_limit':
        failure = 'incorrect_or_unverifiable_delivery_blocked'
    elif task['finish'] in ('output_limit_no_retry_remaining','context_limit','unexplained_length_stop',
                            'task_output_limit','task_wall_admission_limit','max_turns'):
        failure = task['finish']
    elif protocol_errors:
        failure = 'protocol_failure'
    else:
        failure = 'independent_grading_or_delivery_failure'
    false_acceptance = bool(guard and guard[-1]['accepted'] and checks.get('exact_structured_ground_truth') is False)
    return item | dict(success=task['success'],finish=task['finish'],failure_category=failure,
        task_wall_s=task['task_wall_s'],hidden_scoring_wall_s=task['scoring_wall_s'],
        requests=len(task['turns']),tools=len(task['tool_results']),output_tokens=output,
        processed_prompt_tokens=fresh,total_prompt_tokens=sum(r['prompt_tokens'] for r in records),
        cached_prompt_tokens=sum(r['cached_tokens'] for r in records),initial_cached_tokens=0,
        max_prompt_tokens=max(r['prompt_tokens'] for r in records),prefill_s=pp,decode_s=tg,
        weighted_fresh_prefill_tps=fresh/pp if pp else None,weighted_decode_tps=output/tg if tg else None,
        request_wall_s=sum(t.get('http_wall_s',0) for t in task['turns']),
        initial_cap=first['max_tokens'],final_cap=task['turns'][-1]['request_body']['max_tokens'],
        length_events=retries,length_retries=sum(r['retried'] for r in retries),
        discarded_output_tokens=sum(r['discarded_output_tokens'] for r in retries),
        discarded_request_wall_s=sum(r['discarded_request_wall_s'] for r in retries),
        delivery_attempts=len(guard),delivery_rejections=rejection_count,
        delivery_failed_fields=[e['failed_fields'] for e in guard if not e['accepted']],
        delivery_guard_s=sum(e['verifier_wall_s'] for e in guard),
        first_delivery_accepted=(guard[0]['accepted'] if guard else None),
        delivery_guard_false_acceptance=false_acceptance,
        success_without_policy_recovery=bool(task['success'] and not retries and not rejection_count and not protocol_errors),
        protocol_error_count=len(protocol_errors),protocol_recoveries=sum('recovery_feedback' in t for t in task['turns']),
        tool_failures=tool_failures,ordinary_tool_errors=sum(e['category']=='ordinary_tool_error' for e in tool_failures),
        code_edits=edits,baseline_expected_test_failures=baseline_failures,post_edit_test_failures=edited_failures,
        submitted_code_sha256=submitted_code_sha256,
        private_code_tests=details.get('private_test'),checks=checks,
        failed_checks=[key for key,value in checks.items() if not value],
        initial_request_except_cap_sha256=digest(request_without_cap(first)),
        executed_action_sequence_sha256=digest(actions),fixture_provenance=result['r23_provenance'],
        artifact_sha256=sha(path))


def aggregate(cases):
    successful = [c for c in cases if c['success']]
    pp = sum(c['prefill_s'] for c in cases)
    tg = sum(c['decode_s'] for c in cases)
    return dict(tasks=len(cases),success=len(successful),
        success_without_policy_recovery=sum(c['success_without_policy_recovery'] for c in cases),
        median_attempt_wall_s=statistics.median(c['task_wall_s'] for c in cases),
        median_successful_wall_s=statistics.median(c['task_wall_s'] for c in successful) if successful else None,
        total_wall_s=sum(c['task_wall_s'] for c in cases),
        weighted_fresh_prefill_tps=sum(c['processed_prompt_tokens'] for c in cases)/pp if pp else None,
        weighted_decode_tps=sum(c['output_tokens'] for c in cases)/tg if tg else None,
        requests=sum(c['requests'] for c in cases),output_tokens=sum(c['output_tokens'] for c in cases),
        length_retries=sum(c['length_retries'] for c in cases),
        discarded_output_tokens=sum(c['discarded_output_tokens'] for c in cases),
        discarded_request_wall_s=sum(c['discarded_request_wall_s'] for c in cases),
        delivery_attempts=sum(c['delivery_attempts'] for c in cases),
        delivery_rejections=sum(c['delivery_rejections'] for c in cases),
        delivery_guard_s=sum(c['delivery_guard_s'] for c in cases),
        delivery_guard_false_acceptance=sum(c['delivery_guard_false_acceptance'] for c in cases),
        protocol_errors=sum(c['protocol_error_count'] for c in cases),
        ordinary_tool_errors=sum(c['ordinary_tool_errors'] for c in cases))


def compare_pair(fixed, adaptive):
    assert fixed['initial_request_except_cap_sha256'] == adaptive['initial_request_except_cap_sha256']
    assert fixed['fixture_provenance'] == adaptive['fixture_provenance']
    both = fixed['success'] and adaptive['success']
    return dict(variant=fixed['variant'],scenario=fixed['scenario'],arm=fixed['arm'],repeat=fixed['repeat'],
        phase=fixed['phase'],identical_initial_input_except_cap=True,both_successful=both,
        fixed_success=fixed['success'],adaptive_success=adaptive['success'],
        fixed_wall_s=fixed['task_wall_s'],adaptive_wall_s=adaptive['task_wall_s'],
        adaptive_minus_fixed_wall_s=adaptive['task_wall_s']-fixed['task_wall_s'],
        adaptive_wall_change_percent=(100*(adaptive['task_wall_s']/fixed['task_wall_s']-1) if both else None),
        same_executed_action_sequence=fixed['executed_action_sequence_sha256']==adaptive['executed_action_sequence_sha256'],
        fixed_output_tokens=fixed['output_tokens'],adaptive_output_tokens=adaptive['output_tokens'],
        adaptive_length_retries=adaptive['length_retries'])


def summarize(root, allow_partial=False):
    manifest = read(root/'manifest.json')
    complete = 'completed_utc' in manifest
    assert complete or allow_partial, 'Use --partial for unfinished evidence'
    cases = [public_case(root,item) for item in manifest['records']]
    if complete:
        assert len(cases) == len(manifest['cases']) == 22 and manifest['weights_unchanged']
        identity_keys=('variant','scenario','arm','policy','repeat','phase')
        assert {tuple(c[k] for k in identity_keys) for c in cases} == {
            tuple(c[k] for k in identity_keys) for c in manifest['cases']}, 'Registered matrix mismatch'
    environments = {}
    for case in cases:
        identity = read(root/(case['tag']+'.identity.json'))
        api_identity = read(root/case['tag']/'api-identity.json')
        assert identity['engine_sha256'] == manifest['engine_sha256']
        assert api_identity['sha256'] == manifest['api_sha256']
        env = {k:v for k,v in identity['env'].items() if k!='GDEC_REQSTAT_DIR'}
        if case['arm'] in environments:
            assert environments[case['arm']] == env, 'Arithmetic profile changed'
        environments[case['arm']] = env
        for name,checksum in case['fixture_provenance']['implementation_sha256'].items():
            assert manifest['client_sources'][name] == checksum, 'Client changed'
        case['engine_ready_s'] = identity['ready_s']
    pairs = []
    groups = {}
    for case in cases:
        key = (case['variant'],case['scenario'],case['arm'],case['repeat'])
        assert case['policy'] not in groups.setdefault(key,{})
        groups[key][case['policy']] = case
    for group in groups.values():
        if len(group)==2:
            pairs.append(compare_pair(group['fixed8192'],group['adaptive4096']))
    aggregates = []
    for phase in ('primary','exact_regression','repeat_timing'):
        for scenario in ('office','code'):
            for policy in ('fixed8192','adaptive4096'):
                selected=[c for c in cases if (c['phase'],c['scenario'],c['policy'])==(phase,scenario,policy)]
                if selected:
                    aggregates.append(dict(phase=phase,scenario=scenario,policy=policy,**aggregate(selected)))
    repeats = []
    for policy in ('fixed8192','adaptive4096'):
        selected=[c for c in cases if (c['variant'],c['scenario'],c['arm'],c['policy'])==('r22-a','code','hybrid',policy)]
        if selected:
            times=[c['task_wall_s'] for c in selected]
            repeats.append(dict(policy=policy,repeat_count=len(selected),all_success=all(c['success'] for c in selected),
                wall_s=times,median_s=statistics.median(times),minimum_s=min(times),maximum_s=max(times),
                timing_interpretation='All attempt durations; a failed attempt is not time to a correct result',
                sample_stdev_s=statistics.stdev(times) if len(times)>1 else None,
                identical_actions_across_repeats=len({c['executed_action_sequence_sha256'] for c in selected})==1,
                output_tokens=[c['output_tokens'] for c in selected]))
    keys=('registered_utc','started_utc','completed_utc','engine_source_commit','engine_sha256','api_sha256',
          'client_sources','runner_sha256','context_capacity','mtp_gamma','kv','thinking','temperature','seed',
          'max_turns','max_task_output_tokens','task_wall_admission_limit_s','hard_client_timeout_s',
          'max_delivery_rejections','primary_scope','separate_scope','weights_unchanged')
    return dict(schema='r23-task-reliability-v1',complete=complete,experiment={k:manifest[k] for k in keys if k in manifest},
        weights=[dict(filename=w['path'].replace('\\','/').rsplit('/',1)[-1],
                      **{k:v for k,v in w.items() if k!='path'}) for w in manifest['weights']],
        profile_env=environments,cases=cases,pairs=pairs,aggregates=aggregates,code_a_repeats=repeats,
        limitations=[
            'Only two synthetic task families, four primary instances; assisted workflow, not population model accuracy',
            'Fixed 8192 is a maximum, not a minimum; adaptive regeneration adds discarded generation cost',
            'Each request counts whether successful, truncated or rejected; failed attempt time is not time to a correct answer',
            'success_without_policy_recovery excludes budget, format and delivery repair; ordinary SQL/code iteration is counted separately',
            'Output-cap pairs use identical initial prompts except max_tokens; later plans and text may differ',
            'Office tool description now declares verification; R22-to-R23 changes cannot isolate recovery causality',
            'SQL verifier is independent of Python final grader but limited to this declared synthetic schema/contract',
            'Delivery guard checks structured fields, not all final narrative claims or visual chart mappings',
            'No Octop memory, unrestricted code execution, real workbook, HTML/PPTX, concurrent or stochastic evaluation',
            'Model load and final hidden scoring excluded from task time; runtime checking and retries included',
            'Wall limit checks request admission only; weight identity uses metadata and sampled first/last MiB, not full-file hashes',
            'No engine or deployment changes; task timing differences are not kernel gains'])


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('evidence',type=Path)
    parser.add_argument('--partial',action='store_true')
    parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    report=summarize(args.evidence.resolve(),args.partial)
    if args.output:
        with args.output.open('x',encoding='utf-8') as stream:
            json.dump(report,stream,ensure_ascii=False,indent=2)
    print(json.dumps(dict(complete=report['complete'],cases=len(report['cases']),
                         aggregates=report['aggregates'],repeats=report['code_a_repeats']),ensure_ascii=False,indent=2))
