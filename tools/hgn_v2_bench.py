#!/usr/bin/env python3
"""Reproducible local GEN-wire throughput checks; synthetic prompts only.

Start one server at a time, with >=16384 context and checkpoint reuse off.
Use the same tokenizer, inputs, context, flags and repeats for each variant.
This measures speed and simple retrieval, not general model quality.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import statistics
import time


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--tokenizer-dir', required=True)
    ap.add_argument('--host', default='127.0.0.1')
    ap.add_argument('--port', type=int, default=18870)
    ap.add_argument('--repeat', type=int, default=3)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    if args.repeat < 1:
        ap.error('--repeat must be positive')
    os.environ['TOK_DIR'] = args.tokenizer_dir
    from qwentok import Tokenizer
    tk = Tokenizer()
    results = []

    def run(text, mode, limit, case, repeat):
        prompt = '<|im_start|>user\n' + text + '<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n'
        ids, eos = tk.encode(prompt), tk.encode('<|im_end|>')
        rid = time.time_ns() % 1000000000
        request = f'GEN {rid} {limit} {len(eos)} ' + ' '.join(map(str, eos))
        request += f' {len(ids)} ' + ' '.join(map(str, ids)) + f' {mode}\n'
        tokens, first = [], None
        start = time.perf_counter()
        with socket.create_connection((args.host, args.port), timeout=600) as sock:
            sock.sendall(request.encode())
            with sock.makefile('r', encoding='utf8') as stream:
                for line in stream:
                    fields = line.split()
                    if not fields:
                        continue
                    if fields[0] == 'T':
                        tokens.append(int(fields[2]))
                        if first is None:
                            first = time.perf_counter()
                    elif fields[0] == 'D':
                        break
                    elif fields[0] in ('E', 'ERR'):
                        raise RuntimeError(line.strip())
                else:
                    raise RuntimeError('Engine disconnected before D record')
        elapsed = time.perf_counter() - start
        pp, tg = float(fields[5]), float(fields[6])
        record = dict(case=case, mode=mode, repeat=repeat, prompt_tokens=len(ids),
                      prompt_sha256=hashlib.sha256(prompt.encode()).hexdigest(),
                      tokens=tokens, text=tk.decode(tokens), done=line.strip(),
                      ttft_s=None if first is None else first-start, elapsed_s=elapsed,
                      prefill_ms=pp, decode_ms=tg,
                      prefill_tps=int(fields[3])*1000/pp,
                      decode_tps=int(fields[4])*1000/tg)
        if case.startswith('prefill_'):
            record['passed'] = record['text'].strip() == 'BLUE-7629'
        results.append(record)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding='utf8')
        print(case, mode, repeat, f'PP={record["prefill_tps"]:.2f}',
              f'TG={record["decode_tps"]:.2f}', flush=True)

    for target in (256, 1024, 3072, 8192):
        text = f'测试批次{target}。下面是背景记录，请忽略普通记录，只按最后的问题回答。\n'
        i = 0
        while len(tk.encode(text)) < target-95:
            i += 1
            text += f'记录{i:04d}：设备{(i*137)%997:03d}已登记，校验码{(i*37)%103:03d}。\n'
        text += '唯一重要事实：本次项目的验收密码是 BLUE-7629。\n请只输出验收密码。'
        for repeat in range(args.repeat):
            run(text, 0, 32, f'prefill_{target}', repeat)
    text = '请用中文写一篇约500字的说明，介绍如何通过数据字典、唯一标识、分组汇总和勾稽检查来提高办公统计的可靠性。直接开始正文，不要重复题目。'
    for mode in (0, 4):
        for repeat in range(args.repeat):
            run(text, mode, 128, 'decode_128', repeat)
    for case, mode in dict.fromkeys((r['case'], r['mode']) for r in results):
        rows = [r for r in results if (r['case'], r['mode']) == (case, mode)]
        print('MEDIAN', case, mode,
              'PP', statistics.median(r['prefill_tps'] for r in rows),
              'TG', statistics.median(r['decode_tps'] for r in rows),
              'TTFT', statistics.median(r['ttft_s'] for r in rows))
    if any(r.get('passed') is False for r in results):
        raise SystemExit('Retrieval check failed; inspect the saved outputs before using these timings.')


if __name__ == '__main__':
    main()
