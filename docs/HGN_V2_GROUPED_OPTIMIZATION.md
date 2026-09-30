# HGN v2 group-scale matrix experiments — 2026-09-30

This follows [the order-preserving work](HGN_V2_EXACT_OPTIMIZATION.md).
The goal is faster execution with small arithmetic error, without assuming
that either the original FP32 implementation or the upstream experimental
engine is a model-quality oracle. The default remains `exact`; the two new
modes are opt-in experiments. The daily installation was not replaced.

## Algorithm

For each 128-column weight group, the new kernel computes a dot product
using the **exact signed Q4 codes** (-8 through 7), then applies the original
signed FP16 scale in FP32. It does not round `code * scale` to FP16 first.
Packed arithmetic decodes nibbles into exact FP16 integers, eliminating the
random LDS codebook reads. No model file is rewritten or requantized.

* `grouped`: split the FP32 activation into `hi = half(x)` and
  `lo = half((x - float(hi)) * 256)`. Use two matrix products, combine them
  in FP32, and scale each 128-column dot product before accumulating groups.
* `grouped-f16`: use only `hi`, exploring the faster arithmetic boundary.

The rotations/SiLU kernel packs hi/lo once into row-wise half planes. These
planes occupy the existing FP32 scratch allocation. Both modes use the
existing optimized FP32 small-batch GEMV; the measurements below also enable
`GDEC_V2_NATIVE_HT=1` for dense decode projections. Thus the decode gain is
not attributable to the new batched kernel alone.

The grouped path uses 32-token tiles below 6144 input tokens and 64-token
tiles for larger batches. Tests at 4096/8192 tokens supported the wider
choice for long batches. 64-row output tiles and direct LDS scale loads
did not provide a consistent gain and were not retained.

The split does not extend FP16's exponent range. This path requires finite
activations representable in FP16. Group scaling and expert reduction also
change the FP32 summation order; neither mode promises bitwise equivalence.

## Numerical checks

Synthetic complete-MoE tests use randomized expert routing, positive,
negative and zero scales, input/output Hadamard transforms, SiLU, and
weighted reduction. At P=1024, E=512, D=2560 and intermediate size 640:

| Expert implementation | Relative L2 vs original FP32 | Device time |
|---|---:|---:|
| Previous order-preserving exact | 0 | 59.48 ms |
| Previous compensated WMMA, BN=32 | 1.65e-5 | 14.83 ms |
| Previous FP16 WMMA, BN=64 | 4.78e-4 | 9.20 ms |
| New grouped, BN=32 | 3.85e-7 | 10.88 ms |
| New grouped-f16, BN=64 | 3.62e-4 | 8.91 ms |

These are synthetic kernel timings, not end-to-end generation rates.
The order-preserving implementation continues to match its FP32 reference
exactly. Independent CPU double-precision dot products at K=128/640/2560,
including odd token tile tails, measured grouped relative L2 between
1.16e-7 and 1.72e-7; the original FP32 implementation ranged from 7.42e-8
to 2.43e-7. This checks arithmetic against the stored quantized values,
not model accuracy against unquantized weights.

Diagnostic layer captures showed equal inputs and an approximately 7.8e-8
relative L2 change in layer 0's expert output, followed by larger later-layer
differences and changed routes. Restoring expert-reduction order or making
an initial layer prefix exact did not consistently recover model outputs.
This supports checking whole-model behavior separately from local error;
it does not establish the unique cause of the amplification.

## Whole-model measurements

Windows, Ryzen AI Max+395 / Radeon 8060S, one resident model at a time.
Service tests used 16384 context, one slot, BF16 KV, vision off, and no reused
prompt tokens. Greedy decoding, thinking disabled. Each speed fixture has
three repetitions; the table reports medians and includes the first request.
Short fixtures consequently include a visibly slower first request.

The exact baseline is `18259059920f412fb5711e328d0c6fe8d8de5af4`, using the
same v2 files. The upstream experimental comparison is the installed w4b
path at `78a41cc87289dc759ace2f9c030ad5483b30673c`, with the quality overlay
and external MTP. The latter changes both weights and execution path;
it is not a controlled test of kernel performance alone.

| Prompt / workload | Previous exact | Grouped + native HT | Grouped-f16 + native HT | Upstream experimental |
|---|---:|---:|---:|---:|
| Prefill 197 tokens | 201.31 | 532.58 | 562.70 | 500.25 |
| Prefill 978 tokens | 301.88 | 864.95 | 983.11 | 916.50 |
| Prefill 3018 tokens | 339.30 | 1024.51 | 1193.45 | 1210.54 |
| Prefill 8138 tokens | 358.45 | 1164.40 | 1351.85 | 1397.20 |
| Serial prose decode, 128 tokens | 15.37 | 26.57 | 26.54 | 32.75 |
| Chained prose decode, 128 tokens | 15.60 | 27.38 | 27.30 | 32.69 |

Rates are engine-reported phase token/s. The 8138-token client TTFT medians
are 22.72 / 6.99 / 6.02 / 5.85 seconds respectively. Grouped is 3.25x the
previous exact prefill throughput and 83.3% of the upstream comparison;
grouped-f16 reaches 96.8%. Generation is still slower than upstream.
The speculative rows are timing measurements, **not lossless-speed claims**.

## Behavior and limitations

Same-v2 teacher-forced comparisons against stored FP32-reference logits:

| Corpus / mode | Positions | Same top token | Mean KLD | Candidate PPL | Stored-base PPL |
|---|---:|---:|---:|---:|---:|
| Earlier corpus, exact regression | 1020 | 100.000% | rounding floor | 2.314203 | 2.314203 |
| Earlier corpus, grouped | 1020 | 87.843% | 0.333371 | 2.316223 | 2.314203 |
| Earlier corpus, grouped-f16 | 1020 | 87.255% | 0.341994 | 2.459999 | 2.314203 |
| New Chinese office/code corpus, grouped | 2044 | 93.297% | 0.121409 | 1.723618 | 1.875702 |
| New Chinese office/code corpus, grouped-f16 | 2044 | 94.031% | 0.123367 | 1.945937 | 1.875702 |

The second corpus was not used to tune the kernel. It is synthetic and
repetitive, not a representative benchmark. Its reference generation PPL
was 1.878793; reconstructed base PPL is 1.875702 because the logits-file
format clips/quantizes log probabilities. Lower PPL on these samples is
useful evidence, not a general-quality guarantee. Same-top agreement is
behavioral agreement, not answer accuracy. Similar aggregate PPL can hide
large changes at individual positions.

Sixteen deterministic, direct-answer fixtures cover last-record deduplication,
cross-year people counts, units, missing evidence, top-3 ranking, split
contracts, updates, tool-result trust, and 3K/12K-context ledger retrieval.
Exact, grouped, and grouped-f16 each passed 8/16, with the same pass/fail
pattern; upstream passed 7/16. All v2 paths failed seven of eight arithmetic
ledger variants and the unit-conversion case. These tests disable thinking
and tools: they expose a weakness, but are not an Octop workbook evaluation.

The grouped packed-input pilot passed an OpenAI-style tool round trip,
generated Python function fixtures, the smaller ledger fixture and retrieval
checks. Native-HT serial comparison matched 125/126 reference top tokens
(PPL 1.866196 vs 1.858167). Serial and chained generation produced different
prose on a fixed prompt, including on a pilot with native HT off. The test
does not uniquely attribute the difference to native HT or the expert kernel.
Do not present MTP as verified lossless on these new paths.

The priority candidate is `grouped` for further quality testing. `grouped-f16`
is a measured speed/rounding tradeoff, not an equivalent default. Neither
has been certified for real workbook accuracy, 256K context, multimodal
requests, multi-client load, Linux, or parity with an authoritative Halogen
v2 implementation. Existing dense BF16 conversions also remain outside the
new expert-kernel precision claim.

Reproduce kernel checks with `bash build_win.sh v2-test`; the MoE test accepts
an optional argument for the larger 4096/8192-token synthetic runs.
