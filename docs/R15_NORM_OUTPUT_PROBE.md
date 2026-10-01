# R15: dual-output normalization candidate

2026-10-01. **Independent microkernel research; not integrated into the engine.**

本记录补充最新候选的实际数据，不把局部微核收益计入当前分支引擎性能。
以下原始计时来自固定应用目录 HIP 运行库的第二轮，未与第一轮系统 HIP 7.2 的结果混合。

## Baseline and candidate

The baseline was extracted from engine commit
`08f0df010fbbb4284fa90dd4dc5c25194e7190b2`:
R13 `r13tap::norm_bf16_to_f32` followed by `k_f32_to_bf16_v4`.
The candidate writes both the original FP32 value and its BF16 conversion from
one normalization kernel. It retains 1024 threads, the original FP32 summation
and reduction order, final expression and conversion rounding.

No model weights were changed. This removes one FP32 read pass and one cast
launch, not the FP32 write. At P=8192 it avoids about 320 MiB of reads.
That traffic reduction does not establish a whole-model speedup.

## Correctness checks and runtime

Hardware: Windows 11, Ryzen AI Max+ 395, Radeon 8060S, 128 GiB UMA.

- 88 distinct finite-input cases: 80 small, eight large. Each arm was checked
  three times; repetition does not create new independent cases.
- Full FP32/BF16 bit comparisons, finite-value checks, guards, read-only input
  checks and independent round-to-nearest-even conversion checks passed.
  Tested P values include 1/2/7/16/17/31/32/33/1024/8192/8193, two group counts,
  and random, wide-exponent, zero, tiny and rounding-boundary inputs.
- An earlier process loaded System32 HIP 7.2. Its record was retained locally,
  but its timing is not included here.
- This run captured HIP runtime version **71526333** and the app-local HIP DLL.
  Its SHA-256 matched the checked toolchain and engine copies. Merely setting
  PATH was not used as proof of the loaded library.
- ISA inspection found baseline norm/candidate VGPR counts 10/12, both with
  4096 bytes LDS and zero scratch, without a change to the inspected FP32
  reduction, final arithmetic expression or rounding mode.

These checks concern the tested finite input domain. They do not prove
NaN/Inf behavior, whole-model states or arbitrary sampling equivalence.

## Timings

Each shape ran nine alternating ABBA/BAAB blocks, 18 observations per arm.
All 216 samples are positive and finite. All nine block comparisons per shape
favored the candidate in this run. These are repeated timings **within one
process**, not repeated independent process/device experiments.

| P / groups | Baseline median (ms) | Candidate median (ms) | Local time reduction |
|---|---:|---:|---:|
| 1024 / 1 | 0.755750 | 0.442850 | 41.4% |
| 1024 / 4 | 0.792060 | 0.467925 | 40.9% |
| 8192 / 1 | 5.130200 | 3.607350 | 29.7% |
| 8192 / 4 | 5.470750 | 3.647000 | 33.3% |
| 8193 / 1 | 5.159650 | 3.653000 | 29.2% |
| 8193 / 4 | 5.483900 | 3.636950 | 33.7% |

Here reduction is `1 - candidate_median / baseline_median`. This differs
from the median-of-paired-reductions convention in the R10/R12 engine tables.
Timings cover the complete norm-plus-cast baseline versus dual-output norm.
They exclude downstream Lt projections, full prefill, decode and tool tasks.

## Evidence and next integration gate

[Sanitized measurement data](benchmarks/r15-norm-output.json) contains all
216 raw timing observations, correctness counts, runtime identity and hashes.
Private machine paths and unrelated local metadata are omitted.

- Probe EXE SHA-256:
  `b71aabd06ee46f226c6b7e7ba93ef0b744a9df3b9d203e185d55d547765adc38`.
- Retained source log SHA-256:
  `8e69108d1aca2462db16af920452177495a59de999483c2a75a489497653de86`.
- Loaded `amdhip64_7.dll` SHA-256:
  `546fb3d6e2d2194a9526fb94ec2fd3aa5b92a48a7595f04efece80162047ef69`.

The independent probe implementation, full local logs/build capture and
complete reproduction package are not shipped in this checkpoint. Published
timing samples allow recalculating summaries, not independently reproducing
the kernel experiment.

Integration still needs an explicit per-call BF16-ready output, invalidation
of stale conversion caches before scratch writes, FP32 fallbacks, projection
comparisons and cold/warm MTP KV/index/ring/draft-logit checks. Only after those
checks and repeated complete-request measurements can this candidate count
toward engine performance. No such end-to-end claim is made here.

See the [committed R1–R14 checkpoint](WINDOWS_V2_RESEARCH_CHECKPOINT.md) for
the engine work that precedes this candidate.
