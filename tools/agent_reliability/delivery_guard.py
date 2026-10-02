"""Attach a delivery-time SQL check without changing the hidden final grader."""
import copy
import hashlib
import json
from ledger_verifier import LedgerVerifier, canonical, file_digest


def install(fixture):
    guards = {}
    create, dispatch, evaluate = fixture.create_run, fixture.dispatch, fixture.evaluate
    fixture.SCENARIOS = copy.deepcopy(fixture.SCENARIOS)
    for tool in fixture.SCENARIOS['office']['tools']:
        if tool['function']['name'] == 'submit_result':
            tool['function']['description'] = (
                '提交结构化结果前执行独立只读校验；不一致则拒绝保存并只返回错误字段，不提供正确值。'
                '最多两次拒绝，此后任务停止。通过校验后才保存交付物。')

    def new_run(scenario, root):
        value = create(scenario, root)
        if scenario == 'office':
            guards[str(root.resolve())] = LedgerVerifier(root/'ledger.sqlite', fixture.CUTOFF)
        return value

    def guarded(scenario, name, args, root):
        key = str(root.resolve())
        if scenario == 'office' and name == 'submit_result':
            guard = guards[key]
            report = guard.check(args.get('result'))
            number = len(guard.events)-1
            with (root/f'delivery-check-{number:03d}.json').open('x', encoding='utf-8') as f:
                json.dump(dict(candidate=args.get('result'),check=report),f,ensure_ascii=False,indent=2)
            if not report['accepted']:
                value = {'ok':False,'error':{'code':'ledger_reconciliation_failed',
                    'failed_fields':report['failed_fields'],'recoverable':report['recoverable'],
                    'message':'未保存交付结果。请根据原始表和既有业务规则复核所列字段；不要宣称已经通过。'}}
                with (root/'trace.jsonl').open('a',encoding='utf-8') as f:
                    f.write(json.dumps(dict(tool=name,arguments=args,output=value,
                        elapsed_ns=int(report['verifier_wall_s']*1e9)),ensure_ascii=False)+'\n')
                return value
        return dispatch(scenario,name,args,root)

    def score(scenario, root, messages):
        result = evaluate(scenario,root,messages)
        if scenario == 'office':
            guard = guards[str(root.resolve())]
            files = sorted(root.glob('submission-*.json'))
            stored = json.loads(files[-1].read_text(encoding='utf-8')) if files else None
            match = (bool(guard.accepted_sha256) and not guard.blocked and
                     file_digest(root/'ledger.sqlite') == guard.input_sha256 and
                     hashlib.sha256(canonical(stored).encode()).hexdigest() == guard.accepted_sha256)
            result['checks']['independent_delivery_accepted'] = match
            result['details']['delivery_guard'] = dict(events=guard.events,
                method='SQL recomputed from declared source contract; hidden Python grader is separate')
            result['success'] = all(result['checks'].values())
        return result

    fixture.create_run = new_run
    fixture.dispatch = guarded
    fixture.evaluate = score
    fixture.task_stop_reason = lambda scenario,root: (
        'delivery_guard_limit' if scenario=='office' and guards[str(root.resolve())].blocked else None)
