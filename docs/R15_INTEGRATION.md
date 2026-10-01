# R15: optional dual-output MTP normalization in the engine

2026-10-01, Windows 11 / Ryzen AI Max+ 395 / Radeon 8060S (gfx1151), 128 GiB UMA.

R15 is now integrated behind **`GDEC_MTP_TAP_NORM_DUAL=1`, default off**.
The tested numerical contract passed. Complete-request measurements do **not**
establish a stable speedup; no promotion to default or daily deployment was made.
This does not resolve the earlier long-input MTP-versus-serial performance gap.

中文结论：已完成候选接入、冷/热数值检查和三组完整请求对照；局部归一化提速
没有转化为可信的整机收益，保留为默认关闭的研究开关。未改动模型权重。

## Implementation and activation

The original FP32 reduction, RMSNorm expression and BF16 RNE conversion are
retained. One producer writes both FP32 and BF16; its consumer omits the
otherwise separate FP32 read and cast. FP32 writes are deliberately retained
for fallback. No projection, expert precision or accumulation order was changed
by this R15 patch.

- A call-local BF16-ready pointer connects the producer and consumer. It is not
  persisted across requests/slots or represented as a fabricated conversion-cache hit.
- The conversion-cache identity is invalidated **before** shared scratch is
  written. Capability and Lt checks precede that write; the consumer rechecks
  eligibility and still has a complete FP32 fallback.
- R13's guards remain: normal prompt, a single unborrowed slot, P > 16,
  sufficient aligned scratch, the expected q8g64 2560-by-2560 projection,
  supported selected Lt algorithm 1251, and no repair/graph/mrope route.
  The four GEMMs retain their original shapes and strides.
- Opt-in saved-state fingerprints are separated. The default-off checkpoint
  identity is unchanged. Persistent checkpoint restore is **not** claimed tested.

Source: [producer](../src/gpu/parts/22_kernels_prefill.inc),
[call site](../src/gpu/parts/40_model.inc),
[consumer and guards](../src/gpu/parts/41_mtp_branch_strided.inc),
[saved-state fingerprint](../src/gpu/parts/51_host_cfg.inc).

For an existing eligible R13 research configuration, set these process variables
**before** starting the engine; setting a flag alone does not prove dispatch:

```powershell
$env:GDEC_MTP_TAP_NORM_FUSED = '1'
$env:GDEC_MTP_BRANCH_STRIDED = '1'
$env:GDEC_MTP_TAP_NORM_DUAL = '1'
```

Look for `[mtp-tap-norm-dual] ... BF16-ready=1 FP32 fallback retained` in the
engine log. Remove `GDEC_MTP_TAP_NORM_DUAL` (or set it to `0`) and restart to
return to the R13 norm-plus-cast path. These are not `service.conf` keys.
The complete measured flags are in the [sanitized evidence](benchmarks/r15-integration.json).

## Numerical checks and build boundaries

The branch's `tools/ktest.cu` now contains 54 additional production-kernel cases:
P=1/2/7/16/17/31/32/33/1024, group counts 1/4, and three finite-input patterns.
Two repeats poison the writable outputs before dispatch. Full FP32 and BF16
bits, independently calculated BF16 rounding, finite values, guard regions and
unchanged inputs/weights are checked. **241 total kernel cases passed**,
including the 54 new cases. Repeats are not additional distinct cases.
Build/run through the existing `bash build_win.sh test` target with a configured
Windows TheRock toolchain. The separate historical 88-case microprobe remains
documented in [R15 microkernel research](R15_NORM_OUTPUT_PROBE.md).

| Check | Inputs and generated tokens | Recorded state result |
|---|---|---|
| Cold, first integration build | 2049 / 8194 / 8209 / 8210 / 32769 input tokens; 64 output each; 320 corresponding output tokens equal | 404 events, 872 tensor records, 415231284 payload bytes equal |
| Prefix reuse, final build | 8194 input, then 8334 input; second request reused 8258 tokens; 64 output each; 128 corresponding output tokens equal | 162 events, 354 tensor records, 131510860 payload bytes equal |

Recorded tensors cover ingested MTP K/V ranges, completed pooled keys, raw ring,
token IDs and every captured full-vocabulary draft-logit step (248320 logits).
They were finite, metadata/positions aligned, and corresponding captured bytes
were identical. Cold input coverage checks include the required N−1 shifted IDs.
Both off-path captures also match the previously retained R13 cold/warm captures.
The run exercised actual dual-output dispatch (including P=17), as well as small
ingest sizes that retain fallback. These are **not** all hidden activations, a
complete old-KV snapshot or a formal proof for arbitrary inputs.

Cold comparisons used engine SHA-256
`57c870d5e2f6d8594b9d80ea6daf90099728a5ef8ba80080532105969d02d02f`.
Warm comparisons and all performance runs used final engine SHA-256
`9fe81de70efd702b1cc77b1885029d595b47440cead3449d04a0c32f22400af3`.
The sole engine-source difference is the opt-in saved-state fingerprint marker;
the numerical producer/consumer are identical. The kernel-test build preceded
that host-only marker. The final source inventory was frozen across all timings.
The recorded base commit is `5fea699439dd5190b7693f067b89fcf849586103` plus
the R15 patch documented here; per-file hashes are published with the results.

The loaded app-local HIP module was inspected on the actual isolated process:
`amdhip64_7.dll` SHA-256
`546fb3d6e2d2194a9526fb94ec2fd3aa5b92a48a7595f04efece80162047ef69`.
Other loaded ROCm module hashes are included in the evidence. This verifies one
observed runtime, not portability to a different ROCm build or Linux device.

## Complete-request performance

Six fresh engine launches, three pairs: off/on, on/off, off/on. Each launch
processed 32768 and 131072 input tokens, with that length order reversed in the
second pair. No samples were discarded. Each request generated 128 greedy
tokens with EOS ignored, MTP mode 1 and fixed gamma 2. Context capacity 262144,
chunk 8192, one slot, BF16 KV, grouped/native HT and the preceding R13 research
flags were fixed. Only the dual-output switch differed between arms. Weights:
`qwen38-flash-next-v2.hgn` plus matching PLE; no overlay, external draft or vision.
Actual cached tokens were zero. Audit/profile/trace instrumentation was off.

All paired input IDs and complete output token arrays matched, including across
repetitions. This is a throughput experiment with fixed synthetic token inputs,
not an office-task quality evaluation. There is no serial condition in this run.

Each rate/time column reports the median of its three samples, **off → on**.
Time reduction is the median of the three paired reductions, `100*(off-on)/off`;
negative means slower. It need not equal the ratio of the independent medians.
Request wall time excludes startup, loading, client input construction and cleanup.

| Input tokens | Prefill token/s | Decode token/s | Request seconds | Paired request time reduction | Faster request pairs |
|---|---:|---:|---:|---:|---:|
| 32768 | 1237.55 → 1230.50 | 30.05 → 29.48 | 30.736 → 30.983 | -0.471% | 1/3 |
| 131072 | 1184.61 → 1185.32 | 26.33 → 26.53 | 115.542 → 115.510 | +0.028% | 2/3 |

The local 33.3% norm-plus-cast reduction from the earlier microbenchmark is not
an engine speedup. Moreover, indexer auto-tuning selected different solutions
(-711/-712) across launches. This and ordinary device/run variation limit causal
attribution of small deltas. The three-pair campaign neither demonstrates a
stable overall gain nor proves that R15 alone caused an observed slowdown.
The switch therefore remains off by default.

The slow third-pair 128K off-arm decode (7178.7 ms versus 4825.3 ms on) is
retained, as is the slow third-pair 32K on-arm prefill. Speculative round/proposal
counts were unchanged across arms and repeats (32K: 60/119, 128K: 63/126).
The cause of these timing excursions was not isolated; neither is counted as
evidence that R15 improves decode arithmetic or guarantees a regression.

All measurements, **off / on**, are retained below:

| Input tokens | Pair | Prefill ms | Decode ms | Request seconds |
|---|---:|---:|---:|---:|
| 32768 | 1 | 26329.3 / 26456.2 | 4258.9 / 4274.8 | 30.598060 / 30.742070 |
| 32768 | 2 | 26591.4 / 26629.8 | 4401.0 / 4341.8 | 31.003782 / 30.982933 |
| 32768 | 3 | 26478.1 / 28362.6 | 4246.4 / 4366.9 | 30.736000 / 32.740966 |
| 131072 | 1 | 109240.5 / 110914.2 | 4766.6 / 4818.2 | 114.044329 / 115.770249 |
| 131072 | 2 | 110645.9 / 110579.8 | 4861.2 / 4884.8 | 115.542189 / 115.509647 |
| 131072 | 3 | 111099.3 / 109819.3 | 7178.7 / 4825.3 | 118.317365 / 114.683842 |

The [JSON evidence](benchmarks/r15-integration.json) includes all 12 measurements,
flags, hashes, activation/cleanup checks, exact-output results and per-launch
indexer selections. A separate CPU calculation re-read the sealed raw results,
checked full ID arrays and recomputed these statistics. Raw model-state captures,
input captures and the local process-ownership driver remain local; the published
data allow recalculation, not one-command reproduction of the complete campaign.

## Why this was a small target, and what remains

An earlier instrumented R12c profile attributed about 0.8% of long-input prefill
to the **entire** MTP prompt-ingest path (859.724 ms / 104977.5 ms at 131072).
Normalization is only part of that work. This historical profile is explanatory
evidence, not a new R15 timing or an additive speedup estimate.

Further throughput research should first re-profile the current binary and
prioritize expert gate/up/down, GDN and QSA. The older profile showed much larger
costs there. Parent/child event timings overlap, so they must not be summed as
independent opportunities. Better control of automatic kernel selection is also
needed before assigning sub-percent effects to a patch.

Outstanding coverage includes persistent checkpoint restore, multiple slots,
vision, arbitrary sampling, Linux, older-weight full regression, and stable MTP
benefit on long-input/short-output requests. R15 tests are relative to the existing
grouped/native-HT R13 configuration; they do not repair its earlier numerical
differences from exact/reference arithmetic or establish model task accuracy.
The R14 office tool-protocol failures remain unresolved.
