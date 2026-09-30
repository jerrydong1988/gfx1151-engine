# HGN v2 development validation — 2026-09-30

Platform: Windows 11, Radeon 8060S / gfx1151, TheRock 10.0.0 toolchain.
Fork base: upstream `52e0aad9c4c612c57db38541b3d1a3ada61e7717`.

## Passed

| Check | Observed result |
|---|---|
| CPU synthetic format checks | Passed, including Q6 row padding and unknown HT variant rejection |
| All 65,536 procedural codebook states | CPU and GPU exactly equal |
| HT dense decode, 256 x 384 | Relative L2 0.00167821 vs FP32 CPU result; includes BF16 output rounding |
| Q6 dense decode, 7 x 320 | Exact on the synthetic representable-value fixture |
| Rotated expert GEMV, per-token input | Relative L2 1.59342e-7 vs independently materialized CPU matrix multiply |
| Rotated expert GEMV, per-slot input | Relative L2 1.54112e-7 |
| Weighted expert reduction | Passed |
| Existing kernel test suite | 187 PASS lines, final `ALL PASS` |
| Windows engine and launcher builds | Passed |
| Actual v2 metadata and sample decode | All 699 new-format matrices checked (313 HT, 96 expert, 290 Q6), finite samples |
| Actual v2 vs v1 Q4 sample comparison | 699 matrices, mean cosine 0.993526, minimum 0.987619 |
| Missing separate PLE table | Rejected explicitly before model allocation |

The comparison samples the first 128 rows of each rotated matrix and four
rows of each Q6 matrix. Its minimum occurred at
`layers.0.mlp.experts.gate_up_proj.weight`. It is a useful layout check, but
v1 and v2 use different quantizers: these cosines are **not** accuracy scores
against full-precision weights. File sizes checked were 66,687,678,432 bytes
for the main v2 file and 51,200,246,144 bytes for its PLE sidecar. This does
not substitute for a complete file hash/checksum verification.

## Windows full-model test

Tested engine commit: `bce7a1b9a36f81c9be942e7639417546ab95d626`.
The user stopped the working service before testing. Both v2 and the v1
comparison used this isolated build, a 4096-token context, one slot, BF16 KV,
paged KV, no vision tower and no disk snapshots. They ran sequentially; no
two full models were resident together. The v2 file used its separate PLE
table and embedded MTP head, without an old overlay. The v1 run used w4b,
the quality overlay and its external MTP head. Thus this compares the two
weight/execution paths; it does not isolate a quantizer or kernel alone.

V2 started at 17:36:16 and was ready at 17:36:36 (UTC+08:00), including
62.1 GiB of weight upload and 603 dense tensors converted to BF16 (7.83 GiB).
Engine accounting at readiness: 80.75 GiB device allocation and 0.04 GiB
pinned host allocation. HIP reported 26.44 GiB free out of 107.87 GiB.
At the same context the v1 path reported 78.80 GiB device allocation.
These are allocator observations, not an independent OS-wide peak-memory
measurement. The v2 file is smaller on disk, but this implementation retains
its compressed dense tensors as well as the decoded BF16 copies.

### Functional checks

* English exact response and Chinese `17 * 23 = 391`: passed in both serial
  and chained drafting modes, with identical output token IDs.
* Six synthetic ledger rows, last record per `(year, id)`: exactly five
  retained rows, total 920, year totals 350 and 570. JSON parsed and matched.
* A generated Python deduplication/sum function passed three small fixtures
  (empty input, overwritten ID, negative and fractional amounts).
* OpenAI-style `sum_values([125, 375, 500])` tool call parsed with exact
  arguments; a tool-result continuation correctly answered 1000.
* Tail retrieval at 197, 978 and 3018 prompt tokens returned `BLUE-7629`.
  A separate 2878-token prompt with a middle-position fact returned
  `ALPHA-4837-Z`. This is short-context retrieval, not a 256K-context test.
* A 128-token Chinese prose fixture and an exact 1-to-40 number sequence
  produced identical token IDs in serial and chained drafting modes.
* `tools/api_live_smoke.py --base http://127.0.0.1:18871`: all eight checks
  passed (health, models, invalid-type rejection, thinking-disabled content,
  streaming stop equivalence, sampled logprobs, Responses JSON and SSE).

The general Chinese office advice was fluent, but its generic recommendation
to merge across years and retain the latest record lacked metric-specific
business definitions. These smoke checks do **not** establish correctness on
the real contract workbook or prove that previous statistical errors are fixed.
The JSON fixture requested JSON in natural language; it did not test strict
JSON-schema constrained decoding. Tool round-trip success does not replace
an Octop end-to-end test.

### Initial performance

One measured request per row and path, explicit greedy decoding, thinking
disabled, no reused prompt tokens. Rates use engine-reported phase times;
TTFT uses client wall time. No sustained-load or variance claim is made.
Synthetic tail-retrieval cases stop after six output tokens, so their generation
rates are not used as a decode benchmark. Prose outputs intentionally stop at
128 tokens; v1 and v2 generated different prose.

| Fixture | V2 | V1 w4b + quality overlay |
|---|---:|---:|
| Prefill, 197 tokens | 58.95 tok/s | 328.55 tok/s |
| Prefill, 978 tokens | 59.22 tok/s | 912.14 tok/s |
| Prefill, 3018 tokens | 59.28 tok/s | 1172.59 tok/s |
| TTFT, 3018-token fixture | 50.932 s | 2.597 s |
| Prose decode, 128 tokens, serial | 12.61 tok/s | 29.77 tok/s |
| Prose decode, 128 tokens, chained drafting | 11.76 tok/s | 31.33 tok/s |

Chained drafting is workload-dependent: on v2 the 110-token number sequence
improved from 12.48 to 23.97 tok/s, while the prose case became slightly slower.
Its presence is not a general speed guarantee. Token parity was checked only
on the listed greedy fixtures, not across all prompts or sampling settings.

The expert path still performs direct grouped GEMV in 16-token tiles instead
of reusing weights through a WMMA batched path. This is a clear optimization
target, consistent with nearly flat prefill throughput. A kernel-level profile
is still needed to attribute exact time shares. The dense BF16 conversion also
has a memory cost. The current result is **functional compatibility, not a
performance upgrade**; do not replace the daily deployment yet.

The working installation was restored after the test, with its original
262144-token context and vision configuration. Its health endpoint was ready;
the isolated test service was stopped. Model files and deployment configuration
were unchanged.

## Not yet established

* End-to-end logits parity against an authoritative v2 runtime or FP reference.
* Representative quality scores, real workbook accuracy, or multi-step Octop use.
* V2 long context, vision, concurrent requests, cancellation and sustained load.
* Linux runtime behavior and optimized v2 throughput.
