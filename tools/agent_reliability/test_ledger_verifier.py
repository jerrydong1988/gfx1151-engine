"""Independent source reconciliation and delivery failure containment."""
import copy
import importlib.util
import json
from pathlib import Path
import sqlite3
import tempfile
import unittest
from unittest.mock import patch
from ledger_verifier import LedgerVerifier, recompute, exact, normalized
from delivery_guard import install
from r23_variants import VARIANTS, configure

HERE=Path(__file__).resolve().parent


def fixture(variant):
    spec=importlib.util.spec_from_file_location('r23_cpu_fixture',HERE/'fixture.py')
    f=importlib.util.module_from_spec(spec);spec.loader.exec_module(f)
    provenance=configure(f,variant)
    return f,provenance


class LedgerTests(unittest.TestCase):
    def test_hand_calculated_boundaries_and_exclusion_precedence(self):
        # A small oracle written as literal amounts, independent of both SQL and
        # fixture.office_truth: duplicate latest rows, overpayment, refund, ties,
        # exact cutoff, superseded conflicts and mutually exclusive exclusions.
        with tempfile.TemporaryDirectory(dir=HERE,prefix='r23-cpu-') as temp:
            f,_=fixture('r22-a');root=Path(temp)/'data';f.create_run('office',root)
            def inv(key,amount,revision=1,status='posted',currency='CNY',due='2026-09-29',dept='X',year=2026):
                return [key,revision,dept,year,due,amount,status,currency]
            invoices=[inv('A',99999),inv('A',1200,2),inv('A',1200,2),
                inv('B',200),inv('B',201),inv('C',-1,status='draft',currency='USD'),
                inv('D',-1,currency='USD'),inv('E',None),inv('F',-7),inv('G',0),
                inv('H',100,due='2026-09-30'),inv('I',500),inv('J',500),
                inv('K',1),inv('K',2),inv('K',100,2,due='2026-10-01',dept='Y',year=2025)]
            def pay(key,target,amount,status='settled',currency='CNY',day='2026-09-30'):
                return [key,target,day,amount,status,currency]
            payments=[pay('p1','A',1500),pay('p1','A',1500),pay('p2','B',10),
                pay('p3','missing',20,status='pending',currency='USD',day='2026-10-01'),
                pay('p4','missing',30,currency='USD',day='2026-10-01'),
                pay('p5','missing',40,day='2026-10-01'),pay('p6','missing',50),
                pay('p7','A',60),pay('p7','A',61,status='pending'),pay('p8','G',-50)]
            con=sqlite3.connect(root/'ledger.sqlite')
            con.execute('DELETE FROM invoices');con.execute('DELETE FROM payments')
            con.executemany('INSERT INTO invoices VALUES (?,?,?,?,?,?,?,?,?)',[[i,*r] for i,r in enumerate(invoices,1)])
            con.executemany('INSERT INTO payments VALUES (?,?,?,?,?,?,?)',[[i,*r] for i,r in enumerate(payments,1)])
            con.commit();con.close()
            result=recompute(root/'ledger.sqlite')
            self.assertEqual([result[k] for k in ('eligible_invoice_count','receivable_cents','applied_payment_cents','balance_cents')],
                             [6,2400,1450,950])
            self.assertEqual(result['invoice_exclusions'],dict(conflicting_latest_revision=1,non_posted=1,non_cny=1,missing_amount=2))
            self.assertEqual(result['payment_exclusions'],dict(conflicting_event=1,not_settled=1,non_cny=1,after_cutoff=1,ineligible_or_missing_invoice=2))
            self.assertEqual(result['overdue_top5'],[
                dict(invoice_id=k,department='X',due_date='2026-09-29',balance_cents=v) for k,v in [('I',500),('J',500),('G',50)]])
            values=[dict(invoice_count=5,receivable_cents=2300,applied_payment_cents=1450,balance_cents=850),
                    dict(invoice_count=1,receivable_cents=100,applied_payment_cents=0,balance_cents=100)]
            self.assertEqual(result['by_department'],[dict(department='X',**values[0]),dict(department='Y',**values[1])])
            self.assertEqual(result['by_year'],[dict(issued_year=2025,**values[1]),dict(issued_year=2026,**values[0])])

    def test_empty_source_has_zero_totals_and_no_groups(self):
        with tempfile.TemporaryDirectory(dir=HERE,prefix='r23-cpu-') as temp:
            f,_=fixture('r22-a');root=Path(temp)/'data';f.create_run('office',root)
            con=sqlite3.connect(root/'ledger.sqlite')
            con.execute('DELETE FROM invoices');con.execute('DELETE FROM payments');con.commit();con.close()
            result=recompute(root/'ledger.sqlite')
            self.assertEqual([result[k] for k in ('eligible_invoice_count','receivable_cents','applied_payment_cents','balance_cents')],[0,0,0,0])
            for key in ('by_department','by_year','overdue_top5'):self.assertEqual(result[key],[])
            for key in ('invoice_exclusions','payment_exclusions'):self.assertTrue(all(v==0 for v in result[key].values()))

    def test_independent_sql_matches_python_on_all_instances(self):
        seen=[]
        good=(HERE/'selftest_reference_parser.py').read_text(encoding='utf-8')
        for variant in VARIANTS:
            with self.subTest(variant=variant),tempfile.TemporaryDirectory(dir=HERE,prefix='r23-cpu-') as temp:
                f,p=fixture(variant);root=Path(temp)/'data';f.create_run('office',root)
                self.assertTrue(exact(normalized(recompute(root/'ledger.sqlite')),normalized(f.office_truth())))
                self.assertTrue(f.check_tests(good,True)['success'])
                self.assertFalse(f.check_tests(f.INITIAL_CODE,False)['success'])
                self.assertFalse(f.check_tests(f.INITIAL_CODE,True)['success'])
                seen.append(p['office_input_sha256'])
        self.assertEqual(len(set(seen)),len(VARIANTS))

    def test_wrong_but_self_balancing_amount_and_shape_are_rejected(self):
        with tempfile.TemporaryDirectory(dir=HERE,prefix='r23-cpu-') as temp:
            f,_=fixture('r22-b');root=Path(temp)/'data';f.create_run('office',root)
            expected=f.office_truth()
            for mutation in ('balanced_error','float','excluded_count','top5','duplicate_group'):
                bad=copy.deepcopy(expected)
                if mutation=='balanced_error':
                    bad['receivable_cents']+=100;bad['balance_cents']+=100
                elif mutation=='float':bad['balance_cents']=float(bad['balance_cents'])
                elif mutation=='excluded_count':bad['invoice_exclusions']['conflicting_latest_revision']+=1
                elif mutation=='top5':bad['overdue_top5'].reverse()
                else:bad['by_department'].append(copy.deepcopy(bad['by_department'][0]))
                report=LedgerVerifier(root/'ledger.sqlite').check(bad)
                self.assertFalse(report['accepted'],mutation)
                self.assertNotIn(str(expected['receivable_cents']),json.dumps(report))

    def test_guard_does_not_store_rejected_output_or_report_success(self):
        with tempfile.TemporaryDirectory(dir=HERE,prefix='r23-cpu-') as temp:
            f,_=fixture('r22-b');install(f);root=Path(temp)/'data';f.create_run('office',root)
            for resource in ('overview','invoices','payments'):f.dispatch('office','read_resource',{'resource':resource},root)
            f.dispatch('office','query_ledger',{'sql':'SELECT COUNT(*) FROM invoices'},root)
            expected=f.office_truth();bad=copy.deepcopy(expected);bad['balance_cents']+=1
            reply=f.dispatch('office','submit_result',{'result':bad},root)
            self.assertFalse(reply['ok']);self.assertFalse(list(root.glob('submission-*.json')))
            self.assertFalse(f.evaluate('office',root,[])['success'])
            self.assertNotIn(str(expected['balance_cents']),json.dumps(reply))
            self.assertTrue(f.dispatch('office','submit_result',{'result':expected},root)['ok'])
            self.assertTrue(f.evaluate('office',root,[])['success'])
            # A later rejected candidate must not inherit an earlier valid delivery.
            self.assertFalse(f.dispatch('office','submit_result',{'result':bad},root)['ok'])
            self.assertFalse(f.evaluate('office',root,[])['success'])
            self.assertEqual(f.task_stop_reason('office',root),'delivery_guard_limit')

    def test_source_changes_and_unavailable_checker_block_delivery(self):
        with tempfile.TemporaryDirectory(dir=HERE,prefix='r23-cpu-') as temp:
            f,_=fixture('r22-a');root=Path(temp)/'data';f.create_run('office',root)
            verifier=LedgerVerifier(root/'ledger.sqlite')
            con=sqlite3.connect(root/'ledger.sqlite');con.execute('UPDATE invoices SET amount_cents=17');con.commit();con.close()
            report=verifier.check(f.office_truth())
            self.assertFalse(report['accepted']);self.assertIn('source_changed',report['failed_fields'])
            self.assertFalse(report['recoverable'])
            verifier=LedgerVerifier(root/'ledger.sqlite')
            with patch('ledger_verifier.recompute',side_effect=sqlite3.OperationalError('injected')):
                report=verifier.check(f.office_truth())
            self.assertIn('independent_verifier_unavailable',report['failed_fields'])
            self.assertFalse(report['recoverable'])

    def test_agent_loop_stops_after_two_rejected_deliveries(self):
        import test_r14_agent_client_cpu as base
        f,_=fixture('r22-b');install(f)
        bad=f.office_truth();bad['balance_cents']+=1

        class API(base.FakeAPI):
            def chat(self,body,path,turn):
                self.calls+=1
                self.rows.append(base.rec(self.calls))
                turn['http_wall_s']=.4
                return dict(model='model',message={'role':'assistant','content':'','tool_calls':[
                    {'id':f'bad-{self.calls}','type':'function','function':{'name':'submit_result',
                    'arguments':json.dumps({'result':bad},ensure_ascii=False)}}]},usage=base.usage(),
                    finish_reason='tool_calls',first_nonempty_delta_s=.1,first_output_delta_s=.1)

        with (tempfile.TemporaryDirectory(dir=HERE,prefix='r23-cpu-') as temp,
              patch.object(base.c,'load_fixture',return_value=(f,'fake')),
              patch.object(base.c,'LocalAPI',API)):
            result=base.c.execute(base.LoopTests().args(Path(temp),fixture_variant='r22-b'))
            task=result['tasks'][0]
            self.assertFalse(task['success'])
            self.assertEqual(task['finish'],'delivery_guard_limit')
            self.assertEqual(len(task['turns']),2)
            self.assertEqual(len(task['tool_results']),2)
            self.assertFalse(task['evaluation']['checks']['structured_submission_stored'])
            self.assertFalse(task['evaluation']['checks']['independent_delivery_accepted'])


if __name__=='__main__':
    unittest.main(verbosity=2)
