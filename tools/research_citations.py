"""Resolve [S001] citations from a trusted local source manifest.

This is a report-workflow helper, not an engine setting or a fact checker.
It never fetches URLs and never changes the model's claims. Keep the original
Markdown for audit. Usage:
  python tools/research_citations.py report.md sources.json rendered.md
The manifest is a JSON array of {"id": "S001", "url": "https://..."} objects.
"""
import argparse
import json
import re
from pathlib import Path
from urllib.parse import urlsplit

def render(text, sources):
    urls = {}
    for source in sources:
        key, url = source['id'], source['url']
        if not re.fullmatch(r'S\d{3,}', key) or key in urls:
            raise ValueError(f'invalid or duplicate source ID: {key}')
        parsed = urlsplit(url)
        if parsed.scheme not in ('https', 'http') or not parsed.netloc or any(c.isspace() or c in '<>' for c in url):
            raise ValueError(f'invalid source URL for {key}')
        urls[key] = url
    # Retain fenced/inline code and already-linked citations verbatim. A linked
    # citation with the wrong destination is rejected, not silently corrected.
    pattern = re.compile(r'(```[\s\S]*?```|~~~[\s\S]*?~~~|`[^`\n]*`)|\[(S\d+)\](?:\((<[^>]*>|[^)]*)\))?')
    counts = {'resolved': 0, 'existing_links': 0}
    def replace(m):
        if m[1]: return m[0]
        key, destination = m[2], m[3]
        if key not in urls: raise ValueError(f'unknown citation ID: {key}')
        if destination is not None:
            if destination.removeprefix('<').removesuffix('>') != urls[key]:
                raise ValueError(f'citation URL differs from manifest: {key}')
            counts['existing_links'] += 1
            return m[0]
        counts['resolved'] += 1
        return f'[{key}](<{urls[key]}>)'
    return pattern.sub(replace, text), counts

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('input', type=Path); ap.add_argument('sources', type=Path); ap.add_argument('output', type=Path)
    args = ap.parse_args()
    if args.output.resolve() in (args.input.resolve(), args.sources.resolve()):
        raise SystemExit('Output must not overwrite input or source manifest')
    output, counts = render(args.input.read_text(encoding='utf8'), json.loads(args.sources.read_text(encoding='utf8')))
    with args.output.open('x', encoding='utf8') as f: f.write(output)
    print(json.dumps(counts))

if __name__ == '__main__': main()
