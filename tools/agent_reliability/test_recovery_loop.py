"""Offline fault injection: invalid responses never dispatch partial tools."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from test_r14_agent_client_cpu import c, FakeAPI, FakeFixture, rec

import test_r14_agent_client_cpu as base

class RecoveryTests(unittest.TestCase):
    def test_partial_stream_then_error_has_no_usable_result(self):
        a=c.ChatAssembler()
        a.add(json.dumps({'choices':[{'delta':{'tool_calls':[{'index':0,'id':'partial','function':{'name':'read_resource','arguments':'{}'}}]}}]}),0)
        with self.assertRaises(c.ToolCallRejected):
            a.add('{"error":{"code":"invalid_tool_call","message":"bad second call"}}',1)
        with self.assertRaises(c.FatalError): a.result()

    def run_loop(self, errors, limit=2):
        class FaultAPI(FakeAPI):
            def chat(self, body, path, turn):
                if self.calls<len(errors):
                    error=errors[self.calls]
                    self.calls+=1;self.rows.append(rec(self.calls));turn['http_wall_s']=.4
                    raise error
                return super().chat(body,path,turn)
        with (tempfile.TemporaryDirectory()as temp, patch.object(c,'load_fixture',return_value=(FakeFixture,'fake')),
              patch.object(c,'LocalAPI',FaultAPI), patch.object(FakeFixture,'dispatch',wraps=FakeFixture.dispatch)as dispatch):
            args=base.LoopTests().args(Path(temp),tool_repair=limit)
            result=c.execute(args)
            self.assertEqual(dispatch.call_count,0)
            return result

    def test_one_failure_regenerates_once_and_keeps_failed_cost(self):
        result=self.run_loop([c.ToolCallRejected('Unknown function: fake')])
        task=result['tasks'][0]
        self.assertTrue(task['success'])
        self.assertEqual(len(task['turns']),2)
        self.assertEqual(len(result['records']),1)
        self.assertEqual(task['turns'][0]['failed_reqstat']['req_seq'],1)
        messages=task['turns'][1]['request_body']['messages']
        self.assertEqual([m['role']for m in messages],['system','user','user'])
        self.assertIn('fake',messages[-1]['content'])

    def test_same_error_stops_without_loop(self):
        task=self.run_loop([c.ToolCallRejected('same'),c.ToolCallRejected('same')])['tasks'][0]
        self.assertFalse(task['success']);self.assertEqual(task['finish'],'api_error')
        self.assertEqual(len(task['turns']),2)

    def test_two_distinct_retries_then_stops(self):
        task=self.run_loop([c.ToolCallRejected(x)for x in ('one','two','three')])['tasks'][0]
        self.assertEqual(task['finish'],'api_error');self.assertEqual(len(task['turns']),3)
        self.assertEqual(sum('recovery_feedback'in t for t in task['turns']),2)

    def test_disabled_recovery_stops_first(self):
        task=self.run_loop([c.ToolCallRejected('first')],limit=0)['tasks'][0]
        self.assertEqual(task['finish'],'api_error');self.assertEqual(len(task['turns']),1)

    def test_transport_failure_is_not_retried(self):
        with self.assertRaises(OSError):self.run_loop([OSError('uncertain transport status')])

if __name__=='__main__':unittest.main(verbosity=2)
