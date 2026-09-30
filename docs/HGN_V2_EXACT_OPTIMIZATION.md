# Order-preserving v2 optimization — 2026-09-30

Follow-up to [the first kernel round](HGN_V2_OPTIMIZATION.md), keeping
`GDEC_V2_MOE=exact` and `GDEC_V2_NATIVE_HT=0`. This does not enable the
FP16 WMMA expert path or change/requantize the weight files.

## Measured result

Same-machine A/B against commit `2c0b343e461c8e307067f00f722971dc3ac2a7b5`,
three repetitions per case, median token/s:

| Workload | Previous exact | Updated exact | Change |
|---|---:|---:|---:|
| Prefill 197 tokens | 192.82 | 201.16 | +4.3% |
| Prefill 978 tokens | 287.02 | 301.32 | +5.0% |
| Prefill 3,018 tokens | 323.35 | 339.51 | +5.0% |
| Prefill 8,138 tokens | 340.94 | 357.98 | +5.0% |
| Serial generation 128 tokens | 13.34 | 15.35 | +15.0% |
| Chained MTP 128 tokens | 12.12 | 15.59 | +28.7% |

For the 50-token prose prompt, median first-token latency changed from
0.923 s to 0.451 s (serial mode).

All 18 paired requests produced identical output token sequences. Runs were
sequential, old then new, with identical v2 weights, tokenizer, 16K capacity,
one slot, BF16 KV, checkpoint reuse off, and no vision. The same synthetic
prompts from `tools/hgn_v2_bench.py` were used. Engine D records supply PP/TG
timings; loading is excluded. No per-case warm-up is discarded. Three runs
are limited evidence; thermal and OS effects remain. This compares the same
weight format and exact mode, not v2 against a different w4b quantization.

## Implementation

* Coalesced 16-bit **integer** reads fetch four original Q4 nibbles per lane.
  Wave shuffles redistribute them into the reference column order. This is
  a storage-load optimization, not FP16 arithmetic: scale multiplication,
  FMA order and the FP32 reduction tree stay unchanged.
* The batched exact kernel uses these reads while retaining eight-token
  weight reuse. Small batches use the new order-preserving packed GEMV.
* Inverse gate/up rotation, SiLU and down-input rotation share one kernel
  with FP32 intermediates. Per-expert down-output rotation remains before
  router-rank weighted accumulation, as in the reference.
* Larger token tiles and an LDS-staged variant were measured and rejected
  as slower. BF16 projection launch-geometry experiments did not justify
  a general dispatch change; that path is unchanged.

## Validation and limits

* V2 CPU/GPU format checks and the routed MoE differential suite passed.
  Complete FP32 output arrays equal the original expert reference in all
  tested small and real-shape fixtures, including P=1/4/16/64/256/1024.
* Final prefill diagnostic: 1,020 positions, same-top-token 100%, mean KLD
  at the stored-reference rounding floor, PPL 2.314203 on both paths.
* Final teacher-forced serial diagnostic: 126 positions, same-top-token
  100%, mean KLD at the same rounding floor; PPL 1.858163 vs stored 1.858167.
* Synthetic ledger, three generated-code fixtures, tool invocation and
  tool-result continuation passed. Middle retrieval at 2,878 and 12,146
  tokens passed; the latter crosses the 8,192-token prefill chunk boundary.
* The 1-to-40 sequence is token-identical with and without chained MTP.

This establishes preservation of the previous implementation on these
checks, not full-precision model fidelity or a guarantee for every prompt.
The pre-existing dense BF16 decoding approximation and original
prefill/serial differences remain. No business-workbook audit, standard
quality benchmark, 256K, concurrency, or Linux validation was added.

Reproduce kernel checks with `bash build_win.sh v2-test` and whole-model
timings with `tools/hgn_v2_bench.py` as documented in the previous report.
The default mode remains `exact`; reference and opt-in speed modes remain
available. There is no additional user-facing configuration switch.
