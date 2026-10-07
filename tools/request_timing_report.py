"""Summarize host prompt timings without treating them as pure GPU time.

Usage: python tools/request_timing_report.py engine.log [--out timings.json]
No model, process, configuration or log file is modified.
"""
import argparse
import json
import math
import re
import statistics
from pathlib import Path

TAGS = {
    'prefill-wall': ('req', 'base', 'P', 'prepare_ms', 'main_ms', 'mtp_submit_ms', 'yield_ms', 'total_ms'),
    'request-prefill-wall': ('req', 'prompt', 'cached', 'setup_ms', 'ingest_ms', 'first_token_ms', 'total_ms'),
    'ple-row-io': ('rows', 'bytes', 'total_ms'),
    'ple-row-wait': ('rows', 'submit_ms', 'completion_wait_ms', 'max_submit_ms', 'max_completion_wait_ms'),
    'ple-page-io': ('rows', 'pages', 'plan_ms', 'read_scatter_ms', 'total_ms'),
}

def summarize(text, stall_ms=5000):
    if not math.isfinite(stall_ms) or stall_ms <= 0:
        raise ValueError('stall_ms must be positive and finite')
    chunks, requests, errors, ple_reads, ple_waits, fallbacks = [], [], [], [], [], []
    for line_number, line in enumerate(text.splitlines(), 1):
        fallback = re.search(r'\[ple-lookahead\] fallback req=(\d+) base=(\d+) rows=(\d+) reason=([\w-]+)', line)
        if fallback:
            req, base, rows, reason = fallback.groups()
            fallbacks.append({'req': int(req), 'base': int(base), 'rows': int(rows),
                              'reason': reason, 'line': line_number})
        match = re.search(r'\[(' + '|'.join(TAGS) + r')\]\s+(.*)', line)
        if not match:
            continue
        tag, payload = match.groups()
        raw = dict(re.findall(r'(\w+)=([^\s]+)', payload))
        try:
            row = {k: float(raw[k]) if k.endswith('_ms') else int(raw[k]) for k in TAGS[tag]}
            if any(not math.isfinite(v) or v < 0 for k, v in row.items() if k.endswith('_ms')):
                raise ValueError('negative or nonfinite duration')
            if any(v < 0 for k, v in row.items() if not k.endswith('_ms')):
                raise ValueError('negative count or identifier')
            fields = {
                'prefill-wall': ('prepare_ms', 'main_ms', 'mtp_submit_ms', 'yield_ms'),
                'request-prefill-wall': ('setup_ms', 'ingest_ms', 'first_token_ms'),
                'ple-page-io': ('plan_ms', 'read_scatter_ms'),
            }.get(tag)
            if fields and abs(sum(row[k] for k in fields) - row['total_ms']) > 0.02:
                raise ValueError('phase sum differs from total')
            row['line'] = line_number
            if tag == 'prefill-wall': chunks.append(row)
            elif tag == 'request-prefill-wall': requests.append(row)
            elif tag == 'ple-row-wait': ple_waits.append(row)
            else:
                row['mode'] = 'rows' if tag == 'ple-row-io' else 'pages'
                ple_reads.append(row)
        except (KeyError, ValueError, OverflowError) as exc:
            errors.append({'line': line_number, 'error': str(exc)})
    return {
        'schema': 1,
        'semantics': 'Host intervals; async GPU work can cross boundaries. Request totals contain chunk totals: never sum both. PLE prefetch overlaps computation: never add PLE and chunk durations or treat their ratio as a stall fraction. PLE records lack request IDs and are not attributed to adjacent requests. Wait counters have coarse clock resolution. A threshold flags slow reads, not a disk fault. Lookahead concurrency fallback can mean multiple configured slots, not active competing requests. No inferred root cause.',
        'chunks': chunks, 'requests': requests, 'invalid_records': errors,
        'slowest_chunks': sorted(chunks, key=lambda x: x['total_ms'], reverse=True)[:10],
        'ple_reads': ple_reads, 'ple_waits': ple_waits, 'lookahead_fallbacks': fallbacks,
        'ple_summary': {
            'read_batches': len(ple_reads), 'stall_threshold_ms': stall_ms,
            'slow_read_batches': sum(x['total_ms'] >= stall_ms for x in ple_reads),
            'median_batch_ms': statistics.median(x['total_ms'] for x in ple_reads) if ple_reads else None,
            'max_batch_ms': max((x['total_ms'] for x in ple_reads), default=None),
            'slowest_reads': sorted(ple_reads, key=lambda x: x['total_ms'], reverse=True)[:10],
        },
    }

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('log', type=Path)
    ap.add_argument('--out', type=Path)
    ap.add_argument('--stall-ms', type=float, default=5000,
                    help='flag read batches at or above this duration (default: 5000)')
    args = ap.parse_args()
    try:
        result = summarize(args.log.read_text(encoding='utf8', errors='replace'), args.stall_ms)
    except ValueError as exc:
        ap.error(str(exc))
    output = json.dumps(result, ensure_ascii=False, indent=2)
    if args.out:
        args.out.write_text(output + '\n', encoding='utf8')
    else:
        print(output)
    if result['invalid_records']:
        raise SystemExit(1)

if __name__ == '__main__':
    main()
