"""Summarize host prompt timings without treating them as pure GPU time.

Usage: python tools/request_timing_report.py engine.log [--out timings.json]
No model, process, configuration or log file is modified.
"""
import argparse
import json
import math
import re
from pathlib import Path

TAGS = {
    'prefill-wall': ('req', 'base', 'P', 'prepare_ms', 'main_ms', 'mtp_submit_ms', 'yield_ms', 'total_ms'),
    'request-prefill-wall': ('req', 'prompt', 'cached', 'setup_ms', 'ingest_ms', 'first_token_ms', 'total_ms'),
}

def summarize(text):
    chunks, requests, errors = [], [], []
    for line_number, line in enumerate(text.splitlines(), 1):
        match = re.search(r'\[(prefill-wall|request-prefill-wall)\]\s+(.*)', line)
        if not match:
            continue
        tag, payload = match.groups()
        raw = dict(re.findall(r'(\w+)=([^\s]+)', payload))
        try:
            row = {k: float(raw[k]) if k.endswith('_ms') else int(raw[k]) for k in TAGS[tag]}
            if any(not math.isfinite(v) or v < 0 for k, v in row.items() if k.endswith('_ms')):
                raise ValueError('negative or nonfinite duration')
            fields = ('prepare_ms', 'main_ms', 'mtp_submit_ms', 'yield_ms') if tag == 'prefill-wall' else ('setup_ms', 'ingest_ms', 'first_token_ms')
            if abs(sum(row[k] for k in fields) - row['total_ms']) > 0.02:
                raise ValueError('phase sum differs from total')
            row['line'] = line_number
            (chunks if tag == 'prefill-wall' else requests).append(row)
        except (KeyError, ValueError, OverflowError) as exc:
            errors.append({'line': line_number, 'error': str(exc)})
    return {
        'schema': 1,
        'semantics': 'Host intervals; async GPU work can cross boundaries. Request totals contain chunk totals: never sum both. Setup may include MTP boundary ingest; first_token includes readback/sampling. No inferred root cause.',
        'chunks': chunks, 'requests': requests, 'invalid_records': errors,
        'slowest_chunks': sorted(chunks, key=lambda x: x['total_ms'], reverse=True)[:10],
    }

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('log', type=Path)
    ap.add_argument('--out', type=Path)
    args = ap.parse_args()
    result = summarize(args.log.read_text(encoding='utf8', errors='replace'))
    output = json.dumps(result, ensure_ascii=False, indent=2)
    if args.out:
        args.out.write_text(output + '\n', encoding='utf8')
    else:
        print(output)
    if result['invalid_records']:
        raise SystemExit(1)

if __name__ == '__main__':
    main()
