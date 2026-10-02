"""Reproducible fixture tests only. Does not make model/API/network requests."""
import hashlib
import json
from pathlib import Path
import tempfile
import fixture as f

HERE=Path(__file__).resolve().parent
GOOD=(HERE/'selftest_reference_parser.py').read_text(encoding='utf8')
CTE='''WITH latest AS (
 SELECT DISTINCT invoice_id,revision,department,issued_year,due_date,amount_cents,status,currency
 FROM invoices i WHERE revision=(SELECT MAX(revision) FROM invoices j WHERE j.invoice_id=i.invoice_id)),
 eligible AS (SELECT * FROM latest l WHERE (SELECT COUNT(*) FROM latest x WHERE x.invoice_id=l.invoice_id)=1
 AND status='posted' AND currency='CNY' AND amount_cents IS NOT NULL AND amount_cents>=0),
 unique_pay AS (SELECT DISTINCT event_id,invoice_id,paid_on,amount_cents,status,currency FROM payments),
 goodpay AS (SELECT * FROM unique_pay p WHERE (SELECT COUNT(*) FROM unique_pay x WHERE x.event_id=p.event_id)=1
 AND status='settled' AND currency='CNY' AND paid_on<='2026-09-30'
 AND invoice_id IN (SELECT invoice_id FROM eligible)),
 balances AS (SELECT e.*,COALESCE((SELECT SUM(p.amount_cents) FROM goodpay p WHERE p.invoice_id=e.invoice_id),0) paid
 FROM eligible e) '''


def main():
    checks=[]
    def ok(condition,label):
        assert condition,label;checks.append(label)
    ok(f.check_tests(GOOD)['success'],'correct implementation public tests')
    ok(f.check_tests(GOOD,True)['success'],'correct implementation private tests')
    bare=GOOD.replace("raise ValueError('text')",'raise ValueError')
    ok(f.check_tests(bare,True)['success'],'bare raise ValueError is a real contract rejection')
    fake=GOOD.replace("raise ValueError('text')",'missing_name')
    fake_result=f.check_tests(fake,True)
    ok(not fake_result['success'] and any(x['actual'].get('error')=='FixtureExecutionError' for x in fake_result['failures']),
       'unknown name cannot masquerade as contract ValueError')
    unsupported=GOOD.replace("raise ValueError('text')",'return {"unsupported": 1}')
    unsupported_result=f.check_tests(unsupported,True)
    ok(not unsupported_result['success'] and any(x['actual'].get('error')=='FixtureExecutionError' for x in unsupported_result['failures']),
       'unsupported expression cannot masquerade as contract ValueError')
    native=GOOD.replace("raise ValueError('text')","return int('not-an-integer')")
    ok(f.check_tests(native,True)['success'],'native int ValueError still satisfies error-type contract')
    try:f.FunctionVM('def parse_amount_cents(text):\n return str([[1]])').run('x')
    except f.FixtureExecutionError:checks.append('str cannot expand nested containers')
    else:raise AssertionError('nested str expansion accepted')
    ok(not f.check_tests(f.INITIAL_CODE)['success'],'initial buggy implementation fails')
    ok(len(f.TOOLS)==4 and all(len(f.SCENARIOS[s]['tools'])==4 for s in f.SCENARIOS),'four tools per scenario')
    for scenario in f.SCENARIOS:
        schema=f.SCENARIOS[scenario]['tools'][-1]['function']['parameters']['properties']['result']
        ok(schema['additionalProperties'] is False and schema['properties']['kind']['const']==scenario,
           'submission contract is explicit for '+scenario)
    office_schema=f.SCENARIOS['office']['tools'][-1]['function']['parameters']['properties']['result']
    ok(set(office_schema['required'])==set(f.office_truth()),'declared office keys match independent acceptance contract')
    with tempfile.TemporaryDirectory(prefix='fixture-cpu-',dir=HERE) as temporary:
        root=Path(temporary);office=root/'office';code=root/'code';other=root/'duplicate-office'
        f.create_run('office',office);f.new_run(code,'code');f.new_run(other,'office')
        try:f.new_run(office,'office')
        except FileExistsError:checks.append('existing run refuses overwrite')
        else:raise AssertionError('overwrite accepted')
        try:f.new_run(HERE.parent/'not-r14','office')
        except ValueError:checks.append('outside-R14 path refused')
        else:raise AssertionError('path escape accepted')
        for resource in ('overview','invoices','payments'):
            a=f.dispatch('read_resource',{'resource':resource},office)
            b=f.dispatch('office','read_resource',{'resource':resource},other)
            ok(a==b,'deterministic resource '+resource)
        inv=f.dispatch('read_resource',{'resource':'invoices'},office)['result']
        pay=f.dispatch('read_resource',{'resource':'payments'},office)['result']
        ok(inv['row_count']+pay['row_count']==1002,'real structured record count')
        gt=f.office_truth()
        q=f.dispatch('query_ledger',{'sql':CTE+'SELECT COUNT(*),SUM(amount_cents),SUM(paid),SUM(amount_cents-paid) FROM balances'},office)
        ok(q['ok'] and q['result']['rows']==[[gt['eligible_invoice_count'],gt['receivable_cents'],gt['applied_payment_cents'],gt['balance_cents']]],'independent SQLite totals match Python oracle')
        for field,key in [('department','by_department'),('issued_year','by_year')]:
            q=f.dispatch('query_ledger',{'sql':CTE+f'SELECT {field},COUNT(*),SUM(amount_cents),SUM(paid),SUM(amount_cents-paid) FROM balances GROUP BY {field} ORDER BY {field}'},office)
            expected=[[r[field],r['invoice_count'],r['receivable_cents'],r['applied_payment_cents'],r['balance_cents']]for r in gt[key]]
            ok(q['ok']and q['result']['rows']==expected,'independent SQL '+key)
        q=f.dispatch('query_ledger',{'sql':CTE+"SELECT invoice_id,department,due_date,amount_cents-paid balance FROM balances WHERE due_date<'2026-09-30' AND balance>0 ORDER BY balance DESC,invoice_id ASC LIMIT 5"},office)
        ok(q['ok']and q['result']['rows']==[[r[k]for k in ('invoice_id','department','due_date','balance_cents')]for r in gt['overdue_top5']],'independent SQL overdue top5')
        for sql in ["DELETE FROM invoices","ATTACH DATABASE ':memory:' AS stolen","PRAGMA database_list","SELECT load_extension('anything')","SELECT random()","SELECT * FROM invoices","SELECT 1; SELECT 2"]:
            result=f.dispatch('query_ledger',{'sql':sql},office)
            ok(result['ok']is False,'SQL guard '+sql[:25])
        ok(f.dispatch('read_resource',{'resource':'../../fixture.py'},office)['ok']is False,'resource traversal rejected')
        submitted=f.dispatch('submit_result',{'result':gt},office)
        ok(submitted['ok']and 'expected'not in json.dumps(submitted)and 'success'not in submitted['result'],'submit stores without ground truth disclosure')
        ok(f.validate_final('any final prose',office)['success'],'office correct structured submission passes')
        bad=dict(gt);bad['balance_cents']+=1
        f.dispatch('submit_result',{'result':bad},office)
        ok(not f.evaluate('office',office,[])['success'],'one-cent wrong answer fails')
        bad=dict(gt);bad['eligible_invoice_count']=float(bad['eligible_invoice_count'])
        f.dispatch('submit_result',{'result':bad},office)
        ok(not f.validate_final('',office)['success'],'noninteger money/count representation rejected')
        for resource in ('overview','parser','tests'):f.dispatch('read_resource',{'resource':resource},code)
        initial=f.dispatch('code_action',{'action':'test'},code)
        ok(initial['ok']and not initial['result']['success'],'actual failing baseline tool result')
        evil=["def parse_amount_cents(text):\n import os\n return 0", "def parse_amount_cents(text):\n return open('x')", "def parse_amount_cents(text):\n return text.__class__", "def parse_amount_cents(text):\n while True: pass"]
        for src in evil:ok(not f.dispatch('code_action',{'action':'replace','source':src},code)['ok'],'code sandbox '+src.splitlines()[1].strip())
        ok(f.dispatch('code_action',{'action':'replace','source':GOOD},code)['ok'],'fixed function replacement')
        tested=f.dispatch('code_action',{'action':'test'},code)
        ok(tested['ok']and tested['result']['success'],'real public tests after replacement pass')
        f.dispatch('submit_result',{'result':{'kind':'code','summary':'Strict integer parsing with ASCII grouping and range checks.','tests_passed':True}},code)
        ok(f.evaluate('code',code,[])['success'],'full code evaluation including private oracle')
        ok(f.dispatch('query_ledger',{'sql':'SELECT 1'},code)['ok']is False,'cross-scenario ledger denied')
        ok(f.dispatch('code_action',{'action':'test'},office)['ok']is False,'cross-scenario code denied')
        ok(f.FunctionVM('def parse_amount_cents(text):\n return len').run('x')is len,'VM cannot execute returned builtin')
        safe=f.check_tests('def parse_amount_cents(text):\n return len')
        json.dumps(safe);ok(not safe['success'],'unexpected return type serialized safely')
        payload_chars={s:len(json.dumps(f.SCENARIOS[s],ensure_ascii=False,separators=(',',':')))for s in f.SCENARIOS}
        payload_chars['office_two_table_tool_outputs']=len(json.dumps(inv,ensure_ascii=False,separators=(',',':')))+len(json.dumps(pay,ensure_ascii=False,separators=(',',':')))
    report={'pass':True,'cpu_only':True,'model_requests':0,'GPU_requests':0,'network_requests':0,'checks':checks,'check_count':len(checks),'fixture_sha256':hashlib.sha256((HERE/'fixture.py').read_bytes()).hexdigest(),'public_code_cases':len(f.PUBLIC_INPUTS),'private_code_cases':len(f.hidden_inputs()),'office_records':1002,'payload_characters_not_tokens':payload_chars,'office_ground_truth':f.office_truth()}
    print(json.dumps(report,ensure_ascii=False,indent=2))


if __name__=='__main__':main()
