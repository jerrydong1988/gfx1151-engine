# HGN v2 kernel optimization — Windows gfx1151, 2026-09-30

This records the first kernel round at `2c0b343`. A subsequent
[order-preserving optimization](HGN_V2_EXACT_OPTIMIZATION.md) improves the
default exact path further; the measurements below remain a historical snapshot.

This is an experimental continuation of [the initial v2 port](HGN_V2_VALIDATION.md).
It preserves the original compressed files. It does not establish accuracy
parity with Halogen, an unquantized model, or the older w4b model.

## Result

The conservative default now batches expert work without changing its FP32
accumulation order. An opt-in matrix-core path nearly matches the installed
upstream w4b+quality-overlay prefill speed, but changes model outputs.
Generation throughput still trails that upstream path.

Three repetitions per case, median token/s:

| Workload | v2 exact (default) | v2 WMMA + native HT | Upstream w4b + quality overlay |
|---|---:|---:|---:|
| Prefill, 197 input tokens | 191.62 | 551.51 | 473.33 |
| Prefill, 978 input tokens | 283.96 | 960.14 | 900.06 |
| Prefill, 3,018 input tokens | 322.43 | 1,174.50 | 1,195.48 |
| Prefill, 8,138 input tokens | 341.25 | 1,341.53 | 1,386.84 |
| Serial generation, 128 output tokens | 13.17 | 25.97 | 31.63 |
| Chained MTP generation, 128 output tokens | 12.03 | 26.57 | 31.75 |

The fast path is 98.2% / 96.7% of upstream at 3K / 8K prefill and
82.1% / 83.7% at serial / chained generation. The shorter prefill cases
are faster in this sample, but three repeats are not a statistical proof
of a general advantage. At 3K, median time to first token is 9.363 s
(exact), 2.572 s (fast), and 2.540 s (upstream).

The earlier v2 reference implementation measured 59.28 token/s at 3,018
tokens: exact and fast are approximately 5.4x and 19.8x that result.
That historical run used a 4K context; this run used 16K. This is a useful
development comparison, not a fully controlled context-matched speedup.

## Conditions and reproduction

* Windows 11, Ryzen AI Max+ 395 / Radeon 8060S, 128 GiB UMA;
  TheRock 10.0.0, `gfx1151`.
* Fork base: `52e0aad9c4c612c57db38541b3d1a3ada61e7717`.
* Upstream deployment revision: `78a41cc87289dc759ace2f9c030ad5483b30673c`;
  installed engine SHA256:
  `ff37af1ae4d8e4339cbebb9459ab85f84725e814ee695b7ef136fccdabfa9432`.
  Its GPU sources match the fork base. The intervening changes concern
  cancellation and launcher configuration, not GPU kernels.
* One full model at a time, one slot, 16,384-token capacity, 8,192-token
  Windows prefill chunks, BF16 KV, paged KV, no vision, no checkpoint reuse
  (`RCKPT_MAX=0`, `KVSNAP_MAX_GB=0`). Other launcher defaults were retained.
* v2 uses its PLE sidecar and embedded MTP; upstream uses w4b with the
  quality overlay and external MTP. Different weights and MTP heads mean
  this comparison cannot isolate kernel performance or quantization alone.
* GEN wire mode 0 / 4, deterministic generation, thinking disabled by the
  prompt, fixed inputs, 128-token output cap. PP/TG use the engine's D record;
  wall-clock first-token latency is measured separately. Model loading is
  excluded. No per-case warm-up is discarded, so startup/kernel effects can
  affect the first repetition. Thermal and background OS effects remain.

Build and validate with `bash build_win.sh v2-test`, `bash build_win.sh test`,
and `bash build_win.sh`. Use the isolated configuration in [HGN_V2.md](HGN_V2.md),
raising `MAX_CONTEXT` to `16384` and setting `RCKPT_MAX="0"` for this benchmark.
Then, while exactly one variant is serving:

```sh
python tools/hgn_v2_bench.py --tokenizer-dir /path/to/tokenizer \
  --port 18870 --repeat 3 --output results-exact.json
```

Restart between variants. Default means `GDEC_V2_MOE=exact` and native HT
off; fast means `GDEC_V2_MOE=wmma` and `GDEC_V2_NATIVE_HT=1`.
The benchmark stores input hashes, raw timing records and output tokens.
Its public entry point was also run with one repetition as a smoke test.

## What changed

1. **Exact batched experts:** each wave reuses a compressed expert row for
   eight tokens. Column FMA order, reduction tree, per-expert output
   rotation and router-rank accumulation match the original path. Batches
   up to 64 tokens retain the reference kernel.
2. **Opt-in WMMA experts:** reuse the existing expert bucketing and tiled
   matrix-core pipeline for raw group-128 Q4. Gate/up must be unrotated
   before SiLU; its result is rotated for down. No expert-weight conversion
   or persistent dequantized expert copy is introduced.
3. **Transforms and small batches:** wave shuffles reduce Hadamard LDS
   traffic; fused activation/rotation and packed expert loads reduce memory
   traffic and launches. The fast path changes floating-point reduction order.
4. **Optional native HT generation:** decode compressed trellis coefficients
   in the projection kernel, with split-K and four-token weight reuse.
   This improved chained generation from 19.53 to 26.57 token/s on the
   same prose fixture. Dense BF16 copies remain for prefill, so their
   approximately 7.83 GiB overhead is not eliminated.
5. **Diagnostics:** compensated WMMA operands and teacher-forced serial
   logits permit independent numerical comparisons. Compensated mode is
   slower and is not promoted as a proven quality improvement.

On a synthetic real-shape MoE batch (P=1024, E=512, D=2560, M=640, top-k=10),
the final device-timing test measured about 388.4 ms for the reference,
63.3 ms for exact, and 9.3 ms for FP16 WMMA. This roughly 6.1x / 41.8x
**kernel** gain must not be confused with whole-model speedup.

## Correctness and numerical limits

* Existing GPU suite: 187 PASS results, final `ALL PASS`. V2 CPU/GPU
  format tests and the new routed-MoE tests passed.
* Synthetic complete MoE outputs match exactly in exact mode, across
  small fixtures and the real expert dimensions with P=1/4/16/64/256/1024.
  The test fails on any unequal FP32 output.
* Native HT was checked against independently materialized FP32 CPU
  matrices, with split-K 1/2/8 and strided FP32/BF16 inputs for P=1/3/5/8.
  Observed relative L2 errors are approximately 1.5e-7 to 2.8e-7.
* Final full-model diagnostic: four synthetic 512-token chunks, 1,020
  scored positions. Exact vs original prefill has 100% same-top-token,
  PPL 2.314203 on both, and mean KLD at the stored-reference rounding floor.
* Fast vs that original prefill: same-top-token 86.863%, mean KLD 0.342929,
  PPL 2.778572 vs 2.314203. **It is not output-equivalent.** Locally small
  arithmetic differences can produce much larger downstream differences.
* The original prefill itself is not a full-precision oracle: against a
  512-token teacher-forced serial reference it has KLD 0.672534 and 74.510%
  same-top-token (255 positions). Compensated prefill yielded KLD 0.696662
  on that check. Matching the old batched path therefore does not establish
  semantic accuracy, and lower local error does not prove better quality.
* A separate 126-position serial-only diagnostic measured KLD 0.000809
  and 100% same-top-token for fast expert GEMV; native HT yielded 0.003580
  and 99.206%. These are short diagnostics, not general quality scores.

Final fast-mode functional checks passed: a synthetic ledger with explicit
deduplication rules, a generated Python function with three fixtures, an
OpenAI-style tool call and result continuation, and retrieval at 197/978/3018/
8138 input tokens. Middle-position retrieval also passed at 2878 and
12,146 tokens; the latter crosses the 8192-token prefill chunk boundary.
The 1-to-40 sequence is token-identical with and without MTP. Prose is
coherent but differs between those paths; it is not marked token-identical.

No real business workbook audit, standard model-quality benchmark, 256K
context validation, concurrent-serving validation, or Linux run was completed
by this optimization test. Keep fast mode opt-in while evaluating those
cases. Exact mode preserves the prior implementation's tested behavior;
it does not fix limitations already present in that implementation.
