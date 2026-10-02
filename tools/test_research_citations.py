import unittest
from research_citations import render

class CitationTests(unittest.TestCase):
    def setUp(self):
        self.sources = [{'id':'S001','url':'https://example.test/a(b)?x=1&y=2'}]
    def test_preserves_claim_and_code(self):
        raw = '数值 608/873。[S001]\n```text\n[S999]\n```\n`[S777]`'
        result, counts = render(raw, self.sources)
        self.assertEqual(result, raw.replace('。[S001]', '。[S001](<https://example.test/a(b)?x=1&y=2>)'))
        self.assertEqual(counts['resolved'], 1)
        again, counts = render(result, self.sources)
        self.assertEqual(again, result); self.assertEqual(counts['existing_links'], 1)
    def test_rejects_unknown_and_wrong_link(self):
        for text in ['[S999]', '[S001](https://wrong.test)']:
            with self.assertRaises(ValueError): render(text, self.sources)
    def test_bad_source_manifest(self):
        for sources in [self.sources * 2, [{'id':'S001','url':'javascript:alert(1)'}], [{'id':'S001','url':'https://example.test/<x>'}]]:
            with self.assertRaises(ValueError): render('[S001]', sources)
    def test_rejects_short_id_without_silently_normalizing(self):
        sources = [{'id':'S058', 'url':'https://example.test/source'}]
        for text in ['[S58]', '[S58](https://example.test/source)']:
            with self.assertRaisesRegex(ValueError, 'unknown citation ID: S58'):
                render(text, sources)

if __name__ == '__main__': unittest.main()
