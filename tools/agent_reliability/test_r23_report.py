"""Evidence-accounting failures must not turn into performance or quality wins."""
import copy
import unittest
from r23_report import records_for, retry_audit, request_without_cap


class ReportTests(unittest.TestCase):
    def record(self, seq=1):
        return dict(req_seq=seq,drafter='mtp',prompt_tokens=100,cached_tokens=20,
                    output_tokens=4096,prefill_ms=50,decode_ms=1000)

    def task(self):
        body=dict(messages=[{'role':'user','content':'test'}],tools=[],max_tokens=4096)
        turn=dict(turn=1,reqstat=self.record(),request_body=body,http_wall_s=1.2,messages_sha256='same',
            assembled_response={'finish_reason':'length','message':{'reasoning_content':'prefix'}},
            budget_decision=dict(reason='output_limit_retry',retry=True,next_cap=8192,retry_messages_sha256='same'))
        next_turn=dict(turn=2,reqstat=self.record(2),request_body=copy.deepcopy(body)|{'max_tokens':8192},
            http_wall_s=2,messages_sha256='same',assembled_response={'message':{'reasoning_content':'prefix continued'}})
        return dict(turns=[turn,next_turn],tool_results=[])

    def test_failed_request_cost_not_dropped_or_doubled(self):
        task={'turns':[{'reqstat':self.record()},{'failed_reqstat':self.record(2)}]}
        self.assertEqual(sum(r['output_tokens'] for r in records_for(task)),8192)
        task['turns'][1]['failed_reqstat']['req_seq']=1
        with self.assertRaises(AssertionError):records_for(task)
        with self.assertRaises(AssertionError):records_for({'turns':[{}]})

    def test_retry_checked_against_actual_messages_and_dispatch(self):
        task=self.task()
        row=retry_audit(task)[0]
        self.assertEqual(row['discarded_output_tokens'],4096)
        self.assertEqual(row['discarded_request_wall_s'],1.2)
        self.assertTrue(row['reasoning_prefix_preserved'])
        mutated=copy.deepcopy(task)
        mutated['turns'][1]['request_body']['messages'].append({'role':'assistant','content':'partial'})
        with self.assertRaises(AssertionError):retry_audit(mutated)
        task['tool_results']=[{'turn':1}]
        with self.assertRaises(AssertionError):retry_audit(task)

    def test_only_cap_may_differ_in_initial_pair(self):
        one={'max_tokens':4096,'seed':1,'messages':[]}
        two={'max_tokens':8192,'seed':1,'messages':[]}
        self.assertEqual(request_without_cap(one),request_without_cap(two))
        two['seed']=2
        self.assertNotEqual(request_without_cap(one),request_without_cap(two))


if __name__=='__main__':unittest.main(verbosity=2)
