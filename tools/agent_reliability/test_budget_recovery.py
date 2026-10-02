"""Fault injection: no partial tool dispatch or replay of completed actions."""
import copy
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from test_r14_agent_client_cpu import c, FakeAPI, FakeFixture, rec, usage
import test_r14_agent_client_cpu as base
from budget_recovery import BudgetRecovery


class BudgetTests(unittest.TestCase):
    def run_case(self, repeat_length=False, context=10000, wrong_length=False, limit=0):
        class API(FakeAPI):
            def get_json(self,path,artifact):
                data=super().get_json(path,artifact)
                if path=='/health':
                    data['context']=context
                return data

            def chat(self,body,path,turn):
                self.calls+=1
                is_length=self.calls==2 or (self.calls==3 and repeat_length)
                output=(body['max_tokens']-int(wrong_length)) if is_length else 10
                row=rec(self.calls);row['output_tokens']=output;self.rows.append(row)
                turn['http_wall_s']=.4
                message={'role':'assistant','content':''}
                finish='length' if is_length else 'tool_calls' if self.calls in (1,3) else 'stop'
                if finish!='stop':
                    message['tool_calls']=[{'id':f'call-{self.calls}','type':'function',
                        'function':{'name':'read_resource','arguments':'{"phase":'+str(self.calls)+'}'}}]
                if is_length:
                    message['tool_calls'].append({'id':'partial','type':'function',
                        'function':{'name':'read_resource','arguments':'{"half":'}})
                return dict(model='model',message=message,usage=usage(output=output),finish_reason=finish,
                            first_nonempty_delta_s=.1,first_output_delta_s=.1)

        with (tempfile.TemporaryDirectory() as directory,
              patch.object(c,'load_fixture',return_value=(FakeFixture,'fake')),
              patch.object(c,'LocalAPI',API),
              patch.object(FakeFixture,'dispatch',wraps=FakeFixture.dispatch) as dispatch):
            args=base.LoopTests().args(Path(directory),max_tokens=20,budget_monitor=True,
                length_retry_max_tokens=40,max_task_output_tokens=limit)
            result=c.execute(args)
            return result,[copy.deepcopy(call.args) for call in dispatch.call_args_list]

    def test_same_history_retry_preserves_completed_actions(self):
        result,calls=self.run_case()
        task=result['tasks'][0]
        self.assertTrue(task['success'])
        self.assertEqual(len(calls),2)
        self.assertEqual([call[2]['phase'] for call in calls],[1,3])
        turns=task['turns']
        self.assertEqual(turns[1]['request_body']['messages'],turns[2]['request_body']['messages'])
        self.assertEqual([t['request_body']['max_tokens'] for t in turns],[20,20,40,40])
        self.assertEqual(len(result['records']),4)
        self.assertEqual(sum(r['output_tokens'] for r in result['records']),50)
        self.assertTrue(turns[1]['budget_decision']['retry'])
        self.assertNotIn('partial',str(task['messages']))

    def test_second_length_stops_without_dispatch_or_infinite_retry(self):
        result,calls=self.run_case(repeat_length=True)
        task=result['tasks'][0]
        self.assertFalse(task['success'])
        self.assertEqual(task['finish'],'output_limit_no_retry_remaining')
        self.assertEqual(len(calls),1)
        self.assertEqual(len(task['turns']),3)

    def test_context_limit_is_not_output_retry(self):
        result,calls=self.run_case(context=120)
        self.assertEqual(result['tasks'][0]['finish'],'context_limit')
        self.assertEqual(len(calls),1)

    def test_unexplained_length_is_not_retried(self):
        result,_=self.run_case(wrong_length=True)
        self.assertEqual(result['tasks'][0]['finish'],'unexplained_length_stop')

    def test_retry_respects_total_task_budget(self):
        result,_=self.run_case(limit=50)
        self.assertEqual(result['tasks'][0]['finish'],'task_output_limit')
        self.assertEqual(len(result['records']),2)

    def test_fixed_cap_and_invalid_limits(self):
        b=BudgetRecovery(20,20,1000)
        self.assertFalse(b.on_length({'prompt_tokens':100,'output_tokens':20},100)['retry'])
        for values in [(True,40,1000),(20,10,1000),(0,40,1000)]:
            with self.assertRaises(ValueError):BudgetRecovery(*values)


if __name__=='__main__':
    unittest.main(verbosity=2)
