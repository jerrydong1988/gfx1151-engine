import unittest
from request_timing_report import summarize

class TimingsTest(unittest.TestCase):
    def test_nested_intervals_kept_separate(self):
        x = summarize('[prefill-wall] req=123 base=59639 P=8192 prepare_ms=1 main_ms=5 mtp_submit_ms=2 yield_ms=0 total_ms=8\n[request-prefill-wall] req=123 prompt=67831 cached=59639 setup_ms=2 ingest_ms=8 first_token_ms=1 total_ms=11')
        self.assertEqual(x['requests'][0]['total_ms'], 11)
        self.assertEqual(x['chunks'][0]['total_ms'], 8)
        self.assertFalse(x['invalid_records'])

    def test_bad_records_not_silently_accepted(self):
        valid = '[request-prefill-wall] req=1 prompt=2 cached=0 setup_ms=1 ingest_ms=2 first_token_ms=3 total_ms=6'
        for changed in (valid.replace('ingest_ms=2', 'ingest_ms=nan'), valid.replace('total_ms=6', 'total_ms=66'), valid.replace('req=1', 'req=x'), valid.replace('cached=0 ', '')):
            self.assertEqual(len(summarize(changed)['invalid_records']), 1)

    def test_legacy_log_and_extra_fields(self):
        self.assertFalse(summarize('req 3 | decode eval = 100ms')['requests'])
        s = '[prefill-wall] req=9007199254740993 base=0 P=1 prepare_ms=0 main_ms=0.001 mtp_submit_ms=0 yield_ms=0 total_ms=0.001 future=1'
        self.assertEqual(summarize(s)['chunks'][0]['req'], 9007199254740993)

    def test_slow_io_is_not_attributed_to_adjacent_request_or_added_to_gpu_time(self):
        s = '''[ple-row-wait] rows=131072 submit_ms=27642 completion_wait_ms=15437 max_submit_ms=500 max_completion_wait_ms=31
[prefill-wall] req=2 base=0 P=8192 prepare_ms=0 main_ms=5000 mtp_submit_ms=50 yield_ms=0 total_ms=5050
[ple-row-io] rows=131072 bytes=20971520 total_ms=43067.620
[ple-lookahead] fallback req=1 base=0 rows=144829 reason=concurrency
[ple-page-io] rows=1000 pages=900 plan_ms=1 read_scatter_ms=200 total_ms=201'''
        x = summarize(s)
        self.assertEqual(x['ple_summary']['slow_read_batches'], 1)
        self.assertEqual(x['ple_summary']['max_batch_ms'], 43067.620)
        self.assertNotIn('req', x['ple_reads'][0])
        self.assertEqual(x['chunks'][0]['total_ms'], 5050)
        self.assertEqual(x['ple_reads'][1]['mode'], 'pages')
        self.assertEqual(x['lookahead_fallbacks'][0]['reason'], 'concurrency')
        self.assertFalse(x['invalid_records'])

    def test_io_boundary_threshold_and_malformed_records(self):
        good = '[ple-row-io] rows=512 bytes=81920 total_ms=5000'
        self.assertEqual(summarize(good)['ple_summary']['slow_read_batches'], 1)
        self.assertEqual(summarize(good, 5001)['ple_summary']['slow_read_batches'], 0)
        for bad in (good.replace('5000', 'inf'), good.replace('rows=512', 'rows=-1'),
                    good.replace('bytes=81920 ', ''),
                    '[ple-row-wait] rows=512 submit_ms=-1 completion_wait_ms=0 max_submit_ms=0 max_completion_wait_ms=0'):
            self.assertEqual(len(summarize(bad)['invalid_records']), 1)
        for threshold in (0, -1, float('nan'), float('inf')):
            with self.assertRaises(ValueError): summarize(good, threshold)
        self.assertIsNone(summarize('legacy')['ple_summary']['max_batch_ms'])

if __name__ == '__main__':
    unittest.main()
