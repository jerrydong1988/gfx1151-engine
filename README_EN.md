# gfx1151-engine: Windows HGN v2 inference research

![Strix Halo — Qwen3.8-Flash-Next](media/strix_banner_21x9_v2.png)

*中文：[README.md](README.md)*

This is a research fork of the experimental
[upstream gfx1151-engine](https://github.com/IIIIIllllIIIIIlllll/gfx1151-engine),
on branch `codex/halogen-v2-support`. Work targets **Windows 11 / Ryzen AI Max+ 395 /
Radeon 8060S (gfx1151) / 128 GiB unified memory**: Halogen Flash-Next v2 HGN loading,
expert and prefill kernels, and numerical alignment between serial and MTP decoding.
The target remains Qwen3.8-Flash-Next (`qwen4_exp`) and compatible architectures,
not arbitrary language models.

**The older w4b / Q4CP path is retained. V2 support is additive, not a replacement.**
Format support, measured coverage, and which optimizations apply are separate questions;
retaining a format does not promise bitwise-identical output across engine versions.

As of **2026-10-02**, the cumulative engine contains R1–R13, default-off R15–R17
candidates and R16 timing guards. R14/R17 include real tool-task evaluation.
Bounded numerical checks and local speedups do not establish stable overall gains
across input lengths.
These are local research stages, not upstream releases.
Results do not establish production readiness, universal losslessness, or an overall
performance lead over upstream.

As of the R17 checkpoint (2026-10-02), two paged QSA candidates are available
but default off. Tool-output failures after normal stopping now report
`invalid_tool_call` rather than falsely claiming token-budget exhaustion.
This interface correction does not fix model tool selection or SQL correctness.

## What this fork changes

| Area | Additions and improvements | Current boundary |
|---|---|---|
| HGN v2 loading | Observed HT / rotated grouped Q4 / Q6 layouts, separate PLE sidecar in Windows loading, layout and missing-tensor checks | Not every HGN variant; older Q4CP and GGUF dispatch remain |
| V2 experts | FP32-order-preserving exact batch/small-batch kernels; optional group-scale matrix kernels, high/low activation splitting, native HT projections | Grouped arithmetic changes floating-point evaluation; not bitwise equivalent |
| MTP numerics and state | Optional aligned residual/GDN/QSA/short-verification policy, a reproduced multi-row accumulation fix, slot/checkpoint guards and sampler corrections | Finite passing cases do not establish arbitrary sampling, cache or multi-slot equivalence |
| Ordinary prefill | Deferred expert scaling, permuted Q4 reads, PLE lookahead, gate/up plus activation fusion, reduction plus Hadamard fusion | R10/R12 have same-weight, same-binary switch comparisons |
| MTP preparation | Optional KV-only ingest, fused reads, skipping an unused verifier head, tap/RMSNorm fusion, four original-shape projections and dual-output normalization | Shape/slot/runtime guards and fallbacks remain; complete requests are not always faster |
| Evidence | Format/kernel checks, numerical comparisons, long-input timing, real tool loops and failure records | Kernel speed or successful loading is not treated as task-quality evidence |

The upstream Windows port, OpenAI-style API, Q4CP/GGUF, MTP, PLE, vision and caching
provide the foundation; these are not features invented by this fork.
See the [cumulative research checkpoint](docs/WINDOWS_V2_RESEARCH_CHECKPOINT.md).

## Weight compatibility: older formats remain

| Weight path | Current implementation | Validation in this fork |
|---|---|---|
| `qwen38-flash-next-w4b.hgn` + older overlay / external MTP | Older HGN/Q4CP decoding and serial/batched expert dispatch retained; still the repository's default service configuration | An early v2-fork Windows EXE loaded and generated with it; **the latest cumulative engine has not rerun the complete older-weight regression** |
| Other older HQ / overlay combinations | Existing container and overlay paths retained, subject to their tensor layouts and model architecture | Not every HQ file/combination tested; a file extension is insufficient evidence |
| `qwen38-flash-next-v2.hgn` + `qwen38-flash-next-ngram.hgn` | Observed storage 16 / 23 / 24 layouts added; main file includes MTP | Main Windows research path, with kernel, whole-model, long-input and tool-loop records |
| Same-architecture GGUF | Inherited loader, expert path and Linux entry point retained | No Windows GGUF end-to-end regression in this v2 research; not a claim of complete Windows GGUF support |

Code: [HGN decoding](src/hgn.h), [model dispatch](src/gpu/parts/40_model.inc),
[model loading](src/gpu/parts/52_main.inc).
V2 expert kernels are selected by tensor type: older Q4CP does not automatically gain
the v2-specific speedups below. Shared sampler and MTP code also changed, so retained
compatibility does not mean all older-path behavior is unchanged.

V2's `NGRAM_FILE` holds **PLE lookup weights**; it is not an ngram speculation switch.
Clear the old `OVERLAY_FILE` when measuring v2 to avoid replacing its weights.
Empty `MTP_FILE` omits an external draft sidecar but **does not disable embedded MTP**.
See [formats and isolated configuration](docs/HGN_V2.md).

## Measurements and comparison rules

Tests used the Windows 11 / 395 / 8060S / 128 GiB machine above. These are historical
results from individual stages, not a fresh ranking of every setting at the newest
commit. Model files were not rewritten or requantized; that alone does not establish
equal arithmetic or model quality.

### Earlier expert optimization versus upstream with older weights

Medians of three repetitions; context capacity 16384, one slot, BF16 KV, greedy,
thinking/vision off, no reused prompt tokens. Rates are phase tokens/second.

| Workload | Earlier v2 exact | V2 grouped + native HT | Upstream experimental: older w4b + quality overlay + external MTP |
|---|---:|---:|---:|
| Prefill, 8138 input tokens | 358.45 | 1164.40 | 1397.20 |
| Serial decode, 128 output tokens | 15.37 | 26.57 | 32.75 |
| MTP decode, 128 output tokens | 15.60 | 27.38 | 32.69 |

Grouped prefill reached **3.25x** the earlier exact throughput and **83.3%** of that
upstream configuration. The upstream revision was `78a41cc`, earlier exact `1825905`.
Weights, MTP and execution paths differ; this is not an engine-only controlled
comparison or a new R13-versus-upstream measurement.
The more heavily rounded `grouped-f16` experiment reached 1351.85 tok/s (96.8% of
that upstream configuration), but is neither an equivalent acceleration nor the default.
[Conditions and numerical checks](docs/HGN_V2_GROUPED_OPTIMIZATION.md)

### Later controlled comparisons with the same v2 configuration

One binary with switches off/on; fixed input IDs, serial greedy, 128 outputs, one
slot, BF16 KV, context capacity 262144, chunks of 8192 and actual cached tokens zero.
These later experiments retain grouped/native HT research settings, not default exact.
Three pairs per length, retaining slow samples. Percentages are **medians of paired
time reductions**, not throughput increases.

| Stage / input tokens | Prefill time reduction | Complete request time reduction |
|---|---:|---:|
| R10 / 32768 | 5.34% | 4.69% |
| R10 / 131072 | 6.65% | 6.37% |
| R10 / 260000 | 6.25% | 6.18% |
| R12c / 32768 | 2.264% | 1.861% |
| R12c / 131072 | 2.387% | 2.269% |

All listed prefill comparisons were faster in 3/3 pairs; decode was not uniformly
faster. R10's 260K complete-request medians were **247.659 → 232.758 seconds**.
Request time excludes process startup, model loading, tokenization and cleanup.
Do not add stage gains or multiply them into the earlier historical table.
[Configuration, methods and boundaries](docs/WINDOWS_V2_RESEARCH_CHECKPOINT.md)

### MTP: faster decode does not always mean a faster task

R13's two additional preparation switches reduced paired complete-request time by
only 0.1667% / 0.1734% at 131072 / 260000 inputs relative to MTP with them off.
Variation and automatic kernel selection limit attribution.
**At 260000 inputs and 128 outputs, enabled MTP still took 0.2585% longer than
serial, with all three pairs slower.** Stable overall gains for long-input,
short-output requests remain unproven.

R14 ran four isolated instances in serial/MTP/MTP/serial order, each executing one
office aggregation and one code-repair fixture: 64 model requests. Tools actually
executed; evaluation did not merely inspect generated JSON.

| Scenario | Serial decode | MTP decode | Completion |
|---|---:|---:|---|
| Code repair | 22.28 tok/s | 40.72 tok/s | 2/2 successful per mode; mean task time 96.01 → 51.91 s |
| Office aggregation | 23.05 tok/s | 40.55 tok/s | 0/2 successful per mode; both hit a tool-protocol failure |

Code task mean time fell 45.9%, but the first serial prefill had substantial variation;
this is not evidence of a stable MTP prefill gain.
Office runs generated unknown `query_ledser` instead of `query_ledger`, with invalid
SQL. The API also mapped the parser failure to `length` although the output budget
was not exhausted. R17 fixes the error classification; model/tool-task reliability remains a separate limitation.

These are two repetitions per mode of fixed tasks, not a full Octop UI test or broad
agent ranking. Maximum actual prompts were 45496 office and 4114 code tokens,
not a 256K agent test.
[Research record and timing definitions](docs/WINDOWS_V2_RESEARCH_CHECKPOINT.md)

### R15: dual-output normalization integrated, default off

One norm now optionally writes FP32 and BF16 while retaining FP32 fallback and
the original rounding. All 241 kernel cases passed, including 54 new cases.
Cold and prefix-reuse comparisons matched all 448 corresponding output tokens,
recorded MTP cache state and draft logits. This is bounded coverage, not proof
for arbitrary inputs or every model state.

Six fresh launches, three complete-request pairs each at 32K/128K, **did not
establish stable overall acceleration**. Median paired request time increased
**0.471% at 32K** and decreased **0.028% at 128K**. The earlier 33.3% local microkernel
reduction is not a prefill/task speedup; automatic kernel selection also differed
between launches. `GDEC_MTP_TAP_NORM_DUAL` therefore remains off by default.
[Integration, request timings and numerical boundaries](docs/R15_INTEGRATION.md) /
[Earlier microbenchmark](docs/R15_NORM_OUTPUT_PROBE.md)

### R16: profiling, accumulated micro gains and timing guards

Expert/GDN/QSA profiling led to default-off GU double buffering and down traversal
candidates. Approximately 2.2%–2.4% / 1.5%–2.0% local kernel-time reductions did not
reliably accumulate: eight fresh four-arm launches gave paired median complete-
request reductions of just **0.009% at 32K** and **0.031% at 128K** with both on,
one faster and one slower pair at each length. No stable overall gain is claimed.

Twenty captured chunk-boundary residuals/full target logits and 256 corresponding
tokens matched; all 2048 formal timing-run tokens also matched across arms. This
is not an all-state/input proof. Final kernel and policy tests produced 251 PASS
lines. A validated indexer-solution pin and invalid-duration guards improve
measurement control; the scheduling candidates remain off by default.
[All samples, numerical limits and build boundaries](docs/R16_PREFILL_RESEARCH.md)

### R17: captured workloads, QSA dataflow, GDN and tool loops

R17 captured 144 model-routing samples, replayed actual QSA tensors, scanned
32 GDN projection shapes, and tested a dual-projection dispatch at eight sizes.
Small expert tiles, LDS address caching and broad GDN replacements were rejected.
Two QSA environment flags remain default off: `GDEC_QSA_HEAD_INTERLEAVE` and
`GDEC_QSA_KV_FUSED`. Head dispatch reduced local QSA time by 10.6%–18.2%; two
128K requests were 0.98%/3.02% faster, but short-input results were mixed.
KV fusion and the combination did not establish stable general end-to-end gains.
All slower samples and a model-loading timeout remain recorded.

All 3,072 output tokens across 24 formal requests matched the same binary with
the new flags off. Separate chunk hidden/logit audits and 252 kernel plus 72 KV
dataflow cases passed; these are bounded same-quantization checks. Chat/Responses
JSON/SSE now distinguish invalid tool calls from actual budget exhaustion.
The existing serial/MTP paths each passed 2/2 code tasks: weighted decode was
24.18/40.71 tokens/s and mean task time was 83.96/51.96 seconds. Office tasks
passed 0/2 per mode; one error-feedback message still produced no delivery within
12 turns. These MTP comparisons are not gains from the new QSA switches.
[Full four-track results and inclusive Agent timings (Chinese)](docs/R17_REAL_WORKLOAD_RESEARCH.md).

## Precision, defaults and unfinished work

- Defaults remain `GDEC_V2_MOE=exact`, native HT off and `GDEC_SPEC_PRECISION=legacy`.
  Exact means expert arithmetic preserves its FP32 reference order, not unquantized
  model-quality equivalence. Default dense HT/Q6 conversion also retains about
  7.83 GiB of BF16 copies.
- Grouped, grouped-f16, native HT, aligned and subsequent optional fusions require
  explicit selection. High-speed results here are not factory defaults.
  Shared sampler fixes are not all gated by aligned.
- In an earlier 1020-position teacher-forced comparison, exact top-token agreement
  with stored reference logits was 100%; grouped was 87.843%.
  This is behavioral agreement, **not answer accuracy**. Small local error or similar
  PPL does not guarantee whole-model output or statistical-task quality.
- Some reproduced serial/MTP differences have been repaired and finite tested
  configurations align. Arbitrary sampling, warm/cache operation, multiple slots,
  all long inputs and multimodal requests are not universally verified equivalent.
- Remaining work includes a complete older-weight regression on the cumulative
  engine, Linux validation of new paths, office tool failures, stable MTP gains for
  long-input/short-output requests and R15 persistent-cache/multi-slot validation.

[Precision policy](docs/SPEC_NUMERIC_ALIGNMENT.md) /
[Expert numerical experiments](docs/HGN_V2_GROUPED_OPTIMIZATION.md) /
[Evidence boundaries](docs/WINDOWS_V2_RESEARCH_CHECKPOINT.md)

## Windows build and first run

Use a separate checkout of this branch, Git Bash, the TheRock ROCm toolchain and a
compatible driver. Set `THEROCK` to your local toolchain directory in Git Bash, then:

```bash
git clone --branch codex/halogen-v2-support https://github.com/jerrydong1988/gfx1151-engine.git
cd gfx1151-engine
bash build_win.sh
bash build_win.sh api
bash build_win.sh launcher
bash build_win.sh v2-test
bash build_win.sh test
```

1. Adapt `service.conf` using the [isolated v2 configuration](docs/HGN_V2.md):
   main weights, matching PLE sidecar and tokenizer; begin with a short context
   and one slot. The repository configuration still defaults to older w4b.
2. In PowerShell, run `.\start_win.exe --check`, then `.\start_win.exe`.
   Successful full loading and requests must be verified separately from unit tests.
3. For later research settings, use the [R13 parameters and limits](docs/WINDOWS_V2_RESEARCH_CHECKPOINT.md)
   as process environment variables before launch, not additional `service.conf`
   keys. Empty external MTP or gamma zero does not mean serial decoding.

The current Windows launcher sets `GDEC_GEMM_WMMA` and `GDEC_GDN_FUSED`; the older
README statement that these were disabled on Windows no longer applies.
Actual runtime identity, memory/commit headroom, context and concurrency still matter;
128 GiB capacity alone does not establish 256K multi-request feasibility.

See [BUILD_EN.md](BUILD_EN.md) for build details. Some inherited documents retain
historical parameters, performance or platform conclusions; use the linked research
records for this fork's new paths and measured scope. Not all local manifests, input
captures and task harnesses are distributed; these build commands do not reproduce
every table here automatically.

## Research records and documentation

| Record | Contents |
|---|---|
| [HGN v2](docs/HGN_V2.md) / [Initial validation](docs/HGN_V2_VALIDATION.md) | Formats, loading, configuration, initial Windows results and older-weight checks |
| [First kernel optimization](docs/HGN_V2_OPTIMIZATION.md) | Exact / WMMA exploration and rejected precision tradeoffs |
| [Order-preserving optimization](docs/HGN_V2_EXACT_OPTIMIZATION.md) | Same-v2 exact speedups and token comparisons |
| [Grouped matrix kernels](docs/HGN_V2_GROUPED_OPTIMIZATION.md) | Grouped/native HT, historical upstream comparison, numerical and task failures |
| [MTP numerical alignment](docs/SPEC_NUMERIC_ALIGNMENT.md) | Alignment policy, usage and unverified scope |
| [R1–R14 checkpoint](docs/WINDOWS_V2_RESEARCH_CHECKPOINT.md) | Committed implementation, long-input timing, tool loops and reproduction conditions |
| [R15 integration](docs/R15_INTEGRATION.md) / [Earlier probe](docs/R15_NORM_OUTPUT_PROBE.md) | Default-off implementation, numerical checks and complete-request measurements without stable overall gains |
| [R16 Prefill research](docs/R16_PREFILL_RESEARCH.md) | Hotspots, four-arm accumulation test and timing guards; no stable whole-request gain |
| [R17 real-workload research](docs/R17_REAL_WORKLOAD_RESEARCH.md) | Captured routes, QSA dataflow, GDN rejections, complete requests and real Agent checks |

Inherited documentation: [Quick start](QUICKSTART_EN.md), [GGUF](GGUF.md),
[Older HGN HQ](HGN-HQ.md), [HGN container](HGN-FORMAT_EN.md), [MTP](MTP_EN.md),
[Ngram](NGRAM_EN.md), [Concurrency](CONCURRENCY.md), [Conversion](CONVERT_EN.md),
[KLD](KLD.md).

## Acknowledgements

This project's implementation borrows from [halogen-flash-server](https://github.com/peonist-ai/halogen-flash-server) by peonist-ai. The `.hgn` weight container format is halogen's checkpoint container format — see [HGN-FORMAT_EN.md](HGN-FORMAT_EN.md). Many thanks to the halogen authors.

GGUF support heavily references [gufo](https://github.com/gufo-org/gufo) (MIT license): the routed-expert F16 WMMA GEMM kernel is ported from its RoutedF16GEMMKernel (`src/gpu/parts/26_kernels_moe_gguf.inc`); the LUT-decoded variants for hgn q4cp / GGUF IQ4 follow the same pipeline (`27_kernels_moe_lut.inc`); the GGUF↔engine tensor transform semantics reference its reference.cpp (`src/gguf_map.h`). Prefill optimizations such as HC gate fusion and producer epilogues writing the next GEMM's input directly also borrow from gufo's approach (comparison analysis in [GUFO-GAP.md](GUFO-GAP.md), Chinese).

The ngram speculative drafting approach and the two-tier prompt cache borrow ideas from the open-source [llama.cpp](https://github.com/ggml-org/llama.cpp) (MIT license); GGUF weights and the vision tower (mmproj) reuse the very same files as llama.cpp. See the "Attribution" section of [NGRAM_EN.md](NGRAM_EN.md).

## License

[AGPL-3.0](LICENSE)
