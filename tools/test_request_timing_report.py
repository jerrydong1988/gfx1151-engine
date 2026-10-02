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

if __name__ == '__main__':
    unittest.main()
