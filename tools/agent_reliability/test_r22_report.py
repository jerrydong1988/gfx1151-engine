"""Evidence accounting: failed generations cost time; baseline bugs are expected."""
import json
from pathlib import Path
import tempfile
import unittest
from r22_report import case


class ReportTests(unittest.TestCase):
    def test_failed_request_included_and_baseline_separated(self):
        def record(seq, cached, pp, tg, output):
            return dict(req_seq=seq,drafter='mtp',prompt_tokens=1000,cached_tokens=cached,
                prefill_ms=pp,decode_ms=tg,output_tokens=output,ttft_ms=100)
        first = dict(request_body=dict(temperature=0, enable_thinking=True),
            reqstat=record(1,0,2000,4000,100),http_wall_s=6.5)
        failed = dict(failed_reqstat=record(2,500,1000,2000,50),http_wall_s=3.5,
            protocol_error='invalid_tool_call',recovery_feedback={'role':'user','content':'format only'})
        def event(action, result):
            return dict(call={'function':{'name':'code_action'}},parsed_arguments={'action':action},
                result={'ok':True,'result':result},turn=1,returned_failure=False)
        task = dict(success=True,finish='natural_final',task_wall_s=12,turns=[first,failed],
            tool_results=[event('test',{'success':False}),event('replace',{'saved':True}),
                event('test',{'success':False}),event('replace',{'saved':True}),event('test',{'success':True})],
            evaluation=dict(checks={'private_contract_cases_pass':True},details={'private_test':{'success':True}}))
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); (root/'case').mkdir()
            (root/'case'/'result.json').write_text(json.dumps(dict(tasks=[task],r22_provenance={})),encoding='utf8')
            result=case(root,dict(tag='case',variant='a',scenario='code',arm='exact'))
        self.assertEqual(result['weighted_fresh_prefill_tps'],500)
        self.assertEqual(result['weighted_decode_tps'],25)
        self.assertEqual(result['output_tokens'],150)
        self.assertEqual(result['protocol_error_count'],1)
        self.assertEqual(result['protocol_recovery_count'],1)
        self.assertEqual(result['baseline_expected_test_failures'],1)
        self.assertEqual(result['post_edit_test_failures'],1)
        self.assertEqual(result['code_edits'],2)


if __name__=='__main__':
    unittest.main()
