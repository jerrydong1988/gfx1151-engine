"""Cross-check both deterministic datasets without a model request."""
import json
import tempfile
from pathlib import Path
import fixture as f
from test_fixture_cpu import CTE

HERE=Path(__file__).resolve().parent
checks=[]
truths={}
for variant in ('base','amounts-v2'):
    f.DATA_VARIANT=variant
    gt=f.office_truth();truths[variant]=gt
    with tempfile.TemporaryDirectory(prefix='r18-variant-',dir=HERE) as temp:
        root=Path(temp)/'office';f.create_run('office',root)
        assert f.read_json(root/'run.json')['data_variant']==variant
        f.DATA_VARIANT='amounts-v2'if variant=='base'else 'base'
        assert f.validate_final('',root)['details']['expected']==gt
        f.DATA_VARIANT=variant
        checks.append(variant+': saved variant controls independent rescoring')
        def check(sql, expected, name):
            value=f.query(root,sql)['rows']
            assert value==expected,(variant,name,value,expected)
            checks.append(variant+': '+name)
        check(CTE+'SELECT COUNT(*),SUM(amount_cents),SUM(paid),SUM(amount_cents-paid) FROM balances',
              [[gt[k]for k in ('eligible_invoice_count','receivable_cents','applied_payment_cents','balance_cents')]],'totals')
        for field,key in [('department','by_department'),('issued_year','by_year')]:
            check(CTE+f'SELECT {field},COUNT(*),SUM(amount_cents),SUM(paid),SUM(amount_cents-paid) FROM balances GROUP BY {field} ORDER BY {field}',
                  [[r[k]for k in (field,'invoice_count','receivable_cents','applied_payment_cents','balance_cents')]for r in gt[key]],key)
        check(CTE+"SELECT invoice_id,department,due_date,amount_cents-paid balance FROM balances WHERE due_date<'2026-09-30' AND balance>0 ORDER BY balance DESC,invoice_id ASC LIMIT 5",
              [[r[k]for k in ('invoice_id','department','due_date','balance_cents')]for r in gt['overdue_top5']],'overdue top5')
        # Independent SQL classifies unique business identities using the policy priority.
        inv="""WITH d AS (SELECT DISTINCT invoice_id,revision,department,issued_year,due_date,amount_cents,status,currency FROM invoices),
        latest AS (SELECT * FROM d a WHERE revision=(SELECT MAX(revision) FROM d b WHERE a.invoice_id=b.invoice_id)),
        reason AS (SELECT invoice_id,CASE WHEN COUNT(*)>1 THEN 'conflicting_latest_revision' WHEN MIN(status)<>'posted' THEN 'non_posted'
        WHEN MIN(currency)<>'CNY' THEN 'non_cny' WHEN MIN(amount_cents) IS NULL OR MIN(amount_cents)<0 THEN 'missing_amount' ELSE 'eligible' END r
        FROM latest GROUP BY invoice_id) SELECT r,COUNT(*) FROM reason WHERE r<>'eligible' GROUP BY r ORDER BY r"""
        check(inv,[[k,v]for k,v in sorted(gt['invoice_exclusions'].items())if v],'invoice exclusions')
        pay=CTE+""", pc AS (SELECT event_id,CASE WHEN COUNT(*)>1 THEN 'conflicting_event' WHEN MIN(status)<>'settled' THEN 'not_settled'
        WHEN MIN(currency)<>'CNY' THEN 'non_cny' WHEN MIN(paid_on)>'2026-09-30' THEN 'after_cutoff'
        WHEN MIN(invoice_id) NOT IN (SELECT invoice_id FROM eligible) THEN 'ineligible_or_missing_invoice' ELSE 'eligible' END r
        FROM unique_pay GROUP BY event_id) SELECT r,COUNT(*) FROM pc WHERE r<>'eligible' GROUP BY r ORDER BY r"""
        check(pay,[[k,v]for k,v in sorted(gt['payment_exclusions'].items())if v],'payment exclusions')
assert truths['base']['balance_cents']!=truths['amounts-v2']['balance_cents']
assert truths['base']['overdue_top5']!=truths['amounts-v2']['overdue_top5']
f.DATA_VARIANT='base'
print(json.dumps({'pass':True,'checks':checks,'truths':truths,'model_requests':0},ensure_ascii=False,indent=2))
print('PASS',len(checks),'variant provenance and independent SQL/Python checks')
