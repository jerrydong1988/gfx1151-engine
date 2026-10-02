"""Before model tests: independent SQL/Python reconciliation and oracle checks."""
import importlib.util
import json
from pathlib import Path
import tempfile
from r22_variants import VARIANTS, configure
from test_fixture_cpu import CTE

HERE = Path(__file__).parent.resolve()
results = []
for variant in VARIANTS:
    spec = importlib.util.spec_from_file_location('variant_fixture', HERE / 'fixture.py')
    f = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(f)
    provenance = configure(f, variant)
    expected = f.office_truth()
    assert f.office_data() == f.office_data(variant)
    with tempfile.TemporaryDirectory(prefix='r22-check-', dir=HERE) as temp:
        root = Path(temp) / 'office'
        f.create_run('office', root)
        def check(sql, rows):
            assert f.query(root, sql)['rows'] == rows, (variant, sql)
        check(CTE + 'SELECT COUNT(*),SUM(amount_cents),SUM(paid),SUM(amount_cents-paid) FROM balances',
              [[expected[k] for k in ('eligible_invoice_count','receivable_cents','applied_payment_cents','balance_cents')]])
        for field, key in [('department','by_department'),('issued_year','by_year')]:
            check(CTE + f'SELECT {field},COUNT(*),SUM(amount_cents),SUM(paid),SUM(amount_cents-paid) FROM balances GROUP BY {field} ORDER BY {field}',
                [[r[k] for k in (field,'invoice_count','receivable_cents','applied_payment_cents','balance_cents')] for r in expected[key]])
        check(CTE + "SELECT invoice_id,department,due_date,amount_cents-paid balance FROM balances WHERE due_date<'2026-09-30' AND balance>0 ORDER BY balance DESC,invoice_id ASC LIMIT 5",
            [[r[k] for k in ('invoice_id','department','due_date','balance_cents')] for r in expected['overdue_top5']])
        inv = """WITH d AS (SELECT DISTINCT invoice_id,revision,department,issued_year,due_date,amount_cents,status,currency FROM invoices),
        latest AS (SELECT * FROM d a WHERE revision=(SELECT MAX(revision) FROM d b WHERE a.invoice_id=b.invoice_id)),
        reason AS (SELECT invoice_id,CASE WHEN COUNT(*)>1 THEN 'conflicting_latest_revision' WHEN MIN(status)<>'posted' THEN 'non_posted'
        WHEN MIN(currency)<>'CNY' THEN 'non_cny' WHEN MIN(amount_cents) IS NULL OR MIN(amount_cents)<0 THEN 'missing_amount' ELSE 'eligible' END r
        FROM latest GROUP BY invoice_id) SELECT r,COUNT(*) FROM reason WHERE r<>'eligible' GROUP BY r ORDER BY r"""
        check(inv, [[k,v] for k,v in sorted(expected['invoice_exclusions'].items()) if v])
        pay = CTE + """, pc AS (SELECT event_id,CASE WHEN COUNT(*)>1 THEN 'conflicting_event' WHEN MIN(status)<>'settled' THEN 'not_settled'
        WHEN MIN(currency)<>'CNY' THEN 'non_cny' WHEN MIN(paid_on)>'2026-09-30' THEN 'after_cutoff'
        WHEN MIN(invoice_id) NOT IN (SELECT invoice_id FROM eligible) THEN 'ineligible_or_missing_invoice' ELSE 'eligible' END r
        FROM unique_pay GROUP BY event_id) SELECT r,COUNT(*) FROM pc WHERE r<>'eligible' GROUP BY r ORDER BY r"""
        check(pay, [[k,v] for k,v in sorted(expected['payment_exclusions'].items()) if v])
        # Scorer must reject a plausible-looking but wrong monetary total.
        f.dispatch('office','read_resource',{'resource':'overview'},root)
        for resource in ('invoices','payments'):
            f.dispatch('office','read_resource',{'resource':resource},root)
        f.dispatch('office','query_ledger',{'sql':'SELECT COUNT(*) FROM invoices'},root)
        f.dispatch('office','submit_result',{'result':expected},root)
        assert f.evaluate('office', root, [])['success']
        wrong = json.loads(json.dumps(expected)); wrong['balance_cents'] += 1
        f.dispatch('office','submit_result',{'result':wrong},root)
        assert not f.evaluate('office', root, [])['success']
    reference = (HERE / 'selftest_reference_parser.py').read_text(encoding='utf-8')
    assert f.check_tests(reference, True)['success']
    assert not f.check_tests(f.INITIAL_CODE, True)['success']
    assert not f.check_tests(f.INITIAL_CODE, False)['success']
    results.append(provenance | {'sql_python_reconciliation': True, 'mutation_rejected': True,
        'reference_parser_passed': True, 'initial_parser_fails': True})
assert len({x['office_input_sha256'] for x in results}) == len(VARIANTS)
assert len({x['office_expected_sha256'] for x in results}) == len(VARIANTS)
assert len({x['initial_code_sha256'] for x in results}) == len(VARIANTS)
print(json.dumps({'passed': True, 'variants': results}, ensure_ascii=False, indent=2))
