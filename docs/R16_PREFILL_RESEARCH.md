# R16: Prefill bottlenecks, scheduling experiments and measurement control

2026-10-01/02, Windows 11 / Ryzen AI Max+ 395 / Radeon 8060S (gfx1151), 128 GiB UMA.
Base: `79cab18b5078b8caecfeba1c3874a724c4af017d` on `codex/halogen-v2-support`.

This round re-profiled the cumulative v2 engine and tested whether small expert
kernel improvements accumulate. **The four-arm model campaign did not establish
a stable overall gain.** The two scheduling experiments remain default off.
A separate timing-validity fix prevents invalid event durations from participating
in indexer auto-selection and diagnostic aggregation. The daily installation and
model weights were not modified.

中文结论：本轮完成热点复测、三类微核探索、两项候选的独立与叠加实测。
微核收益未稳定转化为整机收益，不提升为默认设置；计时异常防护单独修复并验证。
这不是新的上游速度比较，也没有解决已有办公工具协议失败或全场景 MTP 等价问题。

## Where time is spent

The initial diagnostic used 32768 / 131072 cold input tokens, 8192-row chunks,
128 greedy outputs, MTP mode 1 / gamma 2, one slot, BF16 KV, grouped/native HT
and the R13 research flags. R15 dual-output normalization was off. At 128K,
positive aggregate event spans attributed approximately:

| Parent scope | Share of chunk event intervals |
|---|---:|
| MoE, including routing and shared expert | 41.48% |
| GDN attention | 22.47% |
| QSA attention | 21.62% |
| GR | 12.84% |
| PLE | 1.16% |

The diagnostic chunk total was 110533.06 ms. Within it, expert GU/activation
accounted for 23448.94 ms and down projection 12954.37 ms. GDN input/output
projections together were about 15900 ms, versus about 5430 ms for recurrence.
QSA flash was about 13100 ms and score/select about 4117 ms. These are nested
scopes: **do not add parent and child percentages**. They include event-interval
effects and instrumentation, and do not measure pure GPU busy time, bandwidth
saturation or a hardware ceiling.

The historical profiler checked API status but did not reject invalid durations.
The above aggregates passed the analysis checks; individual spans were not all
retained. Treat them as hotspot guidance, not precision performance claims.

## Candidates and microbenchmark findings

| Candidate | Change | Observation and decision |
|---|---|---|
| Fused expert GU double buffering | Two LDS staging buffers; keep per-output math and WMMA order | Large synthetic case: 2.429% / 2.215% paired median kernel-time reduction in two process runs. Integrated for opt-in whole-model testing. |
| Expert down traversal | Group four existing BN64 tiles in launch order; same kernel arithmetic | Large synthetic case: 1.470% / 1.972% paired median reduction. Integrated for opt-in whole-model testing. |
| GDN GEMM grouped-M traversal | Change CTA order, retain tile and arithmetic | Current grouping was best or near best at P8191/8192. P1025 QKV/Z improved with gm16, but no short/warm model validation; not integrated. |

The initial down prototype used BN32, while normal P8192 prefill dispatches BN64.
The seven-arm follow-up explicitly tested BN64. The earlier BN32 results are
retained in the data, not presented as the production long-prefill improvement.
GDN warp-grid and core staging ideas already rejected in R10 were not retested.

For GU at K2560, the staging loop has 41 rather than 80 barriers; LDS increases
from 26880 to 53760 bytes. Compiler-reported registers were 182 versus 183, with
no local spills and a predicted one resident CTA in both variants. This is not a
measured occupancy/stall explanation for the observed timing difference.

The seven microbenchmark process runs contain 48 complete shape/check records
and 2142 timing observations. Each timing observation averages three launches;
case order rotates across blocks. Full output bytes, finite values, guard regions
and unchanged inputs were checked twice with poisoned output storage. Synthetic
expert distributions are not captures of the actual model's router.

Three averaged timing observations were negative despite successful HIP event calls:
`-5.411046505`, `-10.635443687`, and `-0.17859` ms. Raw observations are retained.
For a candidate's paired statistic, a block is excluded if its baseline or
candidate duration is nonpositive. The data retain the valid/excluded block IDs.
The cause inside the driver/timer stack is not established.

## Controlling indexer selection and invalid timing

`GDEC_IPROJ_SOLUTION=<signed integer>` pins only the large FP32 indexer projection
(P >= 1024). Each actual shape must list the requested rocBLAS solution. Invalid
syntax, unsupported IDs or failed pinned launches stop rather than silently
substitute a kernel. IDs are runtime-specific; `-711` is the measured local ID,
not a portable recommendation. Missing/empty/`auto` retains auto-selection.
A pinned ID conflicts with the **presence** of `GDEC_IPROJ_SGEMM`, even if its
string value is `0`. Pinned checkpoint fingerprints are separated from auto.

This removes one observed source of comparison variability. It does not pin
all kernels or control GPU clocks, temperature and background Windows activity.

After the frozen R16b campaign, R16c adds a separate validity guard:

- Timed GEMM intervals must be finite and positive. Retry an invalid interval
  at most three times, and check both timed launch return statuses.
- An unusable candidate cannot win. Without a usable plain-SGEMM baseline,
  retain plain SGEMM rather than inventing a speedup comparison.
- Diagnostic profile spans reject nonfinite/negative durations and increment
  the invalid count; zero remains allowed for empty diagnostic scopes.

This repairs a demonstrated robustness gap. Negative events were observed in
probes and the profiler, **not captured inside a winning production auto-tune**;
it is not proof that they caused previous selection variability.

## Numerical checks and the bounded dispatch

The optional flags are `GDEC_V2_MOE_GU_DOUBLE_BUFFER=1` and
`GDEC_V2_MOE_DOWN_GROUP4=1`. They apply only to eligible normal v2 grouped precise
prefill: P >= 6144, d2560/mid640, one unborrowed slot, deferred scale/permuted Q4,
and the existing eligible GU mode or BN64 down path. Serial/aligned verification,
other shapes and unsupported arithmetic combinations use existing paths.
The opt-in checkpoint identities are separate; this does not claim checkpoint
restore coverage. These are process environment variables, not service.conf keys.

R16b's 250 PASS lines include eight new production-kernel scheduling cases and
one parser result covering 18 inputs. Ragged tiles, empty experts, counts above
64, direct/mapped inputs and padded launch grids are exercised. R16c adds one
duration-policy result covering eight values: **251 PASS lines** in the final run.

Two actual-model diagnostic launches compare both candidates off versus both on.
Twenty matching chunk boundaries captured the last-row residual state (10240
FP32 elements each) and complete target logits (248320 FP32 elements each).
All captured values were finite and bitwise equal; all 256 corresponding
generated tokens matched. This is not every hidden row, KV/GDN/MTP state, every
input or proof of task-quality equivalence. It is relative to the existing
grouped/native-HT baseline, not a restoration of exact/reference arithmetic.

The first off/32K profile chunk contained a negative aggregate GDN-preparation
interval. That entire chunk was excluded from **both** sides of the paired
diagnostic comparison. All captures remain usable for numerical comparison.
No complete-request performance samples were discarded.

## Whole-model four-arm test

Eight fresh launches, ordered off / GU / down / both, then both / down / GU / off.
Each launch processed 32K then 128K; the length order was not randomized. There
are only two independently launched samples per arm/length. All formal profiling,
audit and trace switches were off, actual cached tokens were zero, and both
candidate activation markers were checked. Indexer solution -711 was checked for
the actual P8192 and P8191 shapes. Context capacity was 262144. Weights were the
same v2 main file and PLE sidecar, with no overlay, external draft or vision.

All full input arrays and corresponding output token arrays match across arms
and repetitions (2048 generated tokens total). Request time is client wall time,
excluding startup, loading, tokenization and cleanup. Prefill/decode are engine
steady-clock phase times, independent of the anomalous HIP event intervals.
This is a fixed synthetic-input throughput comparison, not an Octop office audit,
a serial-versus-MTP test, or a new upstream comparison.

Positive means time saved; negative means slower. Values are medians of
the two paired time reductions (with two samples, this equals their mean).
They are not throughput increases or statistical confidence intervals.

| Inputs | Candidate | Prefill reduction | Request reduction | Faster request pairs |
|---|---|---:|---:|---:|
| 32768 | gu | +0.126% | +0.199% | 2/2 |
| 32768 | down | +0.398% | +0.437% | 2/2 |
| 32768 | both | +0.079% | +0.009% | 1/2 |
| 131072 | gu | -0.314% | -0.315% | 0/2 |
| 131072 | down | -0.105% | -0.100% | 1/2 |
| 131072 | both | +0.087% | +0.031% | 1/2 |

Both-arm request reductions in chronological pairs were **-0.402%, +0.420%**
at 32K and **-0.666%, +0.727%** at 128K: no consistent combined advantage.
Down alone was modestly faster in both 32K pairs, but not consistently at 128K.
Two repeats do not establish general stability. No percentages are summed.

All formal measurements:

| Round | Arm | Inputs | Prefill ms | Decode ms | Request seconds |
|---|---|---:|---:|---:|---:|
| 1 | off | 32768 | 26248.7 | 4210.7 | 30.477222 |
| 1 | off | 131072 | 109780.3 | 4828.8 | 114.654350 |
| 1 | gu | 32768 | 26240.9 | 4221.0 | 30.473806 |
| 1 | gu | 131072 | 110323.6 | 4891.4 | 115.257189 |
| 1 | down | 32768 | 26107.7 | 4217.5 | 30.339105 |
| 1 | down | 131072 | 110384.3 | 4861.7 | 115.301080 |
| 1 | both | 32768 | 26372.0 | 4216.7 | 30.599776 |
| 1 | both | 131072 | 110464.6 | 4902.2 | 115.417896 |
| 2 | off | 32768 | 26257.6 | 4260.8 | 30.529010 |
| 2 | off | 131072 | 111246.2 | 4796.3 | 116.092482 |
| 2 | gu | 32768 | 26199.3 | 4199.5 | 30.410801 |
| 2 | gu | 131072 | 111393.8 | 4775.5 | 116.213996 |
| 2 | down | 32768 | 26189.4 | 4199.3 | 30.400258 |
| 2 | down | 131072 | 110867.8 | 4763.4 | 115.669789 |
| 2 | both | 32768 | 26092.8 | 4294.6 | 30.400758 |
| 2 | both | 131072 | 110358.7 | 4847.3 | 115.248419 |

## Why small kernel gains did not add up here

GU/activation and down together cover only part of Prefill. Applying their
large-case micro reductions to their diagnostic shares suggests roughly 0.7%
potential Prefill savings **if those savings transfer unchanged**. It does not
justify adding 2.3% and 1.7% and claiming a 4% task improvement.

In the paired 128K profile, GU/activation changed from 23351.75 to 23266.49 ms,
whereas down changed from 12859.18 to 13064.82 ms. These diagnostic spans do not
establish causality, but show that synthetic micro gains did not simply carry
over to the model. Actual router distributions, surrounding work and memory
behavior require further investigation before assigning a cause.

Small gains remain worth testing. This round demonstrates the need to measure
single and combined changes with all slower samples retained, rather than
assuming independent, additive improvements. Neither candidate becomes default.

The evidence also records `both - GU - down + off` in the original time units:
negative means an extra saving relative to adding the two individual savings.
This descriptive interaction varies with length and round; it is not a causal
interaction estimate or a statistical proof from this two-round campaign.

The next stronger hypotheses are model-captured expert tile distributions and
QSA flash/select-KV reuse, followed by GDN projection work at relevant shapes.
The profiler makes these reasonable targets; no unimplemented route is claimed
as a speedup. Warm reuse, Agent task quality, concurrency, vision, Linux and the
latest full older-weight regression remain outside this round.

## Build and evidence boundaries

The initial hotspot diagnostic used **R16a**:
`0c2659a35ad331534bf2d641226def14caa3cead3348f4599102315f7425a1c2`.
Its model run and cleanup completed; the local adapter's final printing statement
then failed. That reporting-only failure is retained in the initial-profile note,
and the adapter was corrected before subsequent runs.

The complete four-arm timings and paired model captures used frozen **R16b**:
`50e8b6ffc313e081249e2a85ccbcea3a77a146f456f78b09426c1d017ec660e1`.
All 250 kernel cases also passed for that source.

The final timing-guard build **R16c** is:
`32de78f5bba494432d1cdb46a9ddc16b37bb498efa972154f37bab0dba01ef55`.
All 251 kernel cases passed. A separate 8192-input / 128-output request with
automatic indexer selection completed and the owned runtime stopped cleanly.
It selected `-711` in that run. This is a smoke check,
not a new performance comparison or a live injected-negative-event test.
Invalid/zero/infinite/NaN duration policy inputs are covered by the host policy
test. R16b timing numbers are **not relabeled as R16c measurements**.

Source differences between these builds are the host timing-validity guard,
its test and comments/indentation. Per-file source hashes and build/test log
hashes are included in the JSON. File size/mtime checks verified unchanged model
and PLE inputs across launches; this round did not fully rehash those large files.
The app-local HIP DLL was rehashed on disk in R16c and matches the module
previously inspected in the same isolated setup during R15:
`546fb3d6e2d2194a9526fb94ec2fd3aa5b92a48a7595f04efece80162047ef69`.
No live module inventory was captured in this R16c smoke run. This does not
establish behavior on a different ROCm runtime or Linux.


The [sanitized JSON evidence](benchmarks/r16-prefill-research.json) retains all
formal request measurements, micro observations (including invalid ones),
source/binary hashes, flags, numerical summaries and lifecycle checks. Raw model
captures, synthetic input files and the local ownership-aware campaign driver
remain local. Published evidence supports recalculation, not one-command
reproduction of every whole-model capture. Production-kernel tests are in
`tools/ktest.cu`; run `bash build_win.sh test` with the Windows TheRock toolchain.
