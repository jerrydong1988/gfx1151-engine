# Windows HGN v2 research checkpoint

2026-10-01. R1–R14 are local research stages, not upstream releases. The tested
machine ran Windows 11 on a Ryzen AI Max+ 395 / Radeon 8060S (`gfx1151`), with
128 GiB UMA. The cumulative R13b engine was also used for the R14 tool-task
experiment. This checkpoint records bounded results, not production readiness.

See [v2 formats and configuration](HGN_V2.md),
[grouped expert arithmetic](HGN_V2_GROUPED_OPTIMIZATION.md), and
[speculative numerical alignment](SPEC_NUMERIC_ALIGNMENT.md) for implementation
details. Model files were not rewritten or requantized by these experiments.

## Implemented paths and defaults

The conservative defaults remain `GDEC_V2_MOE=exact`, native HT off, and
`GDEC_SPEC_PRECISION=legacy`. Grouped matrix-core experts and native HT are
explicit choices; they change floating-point arithmetic and are not globally
output-equivalent to `exact`.

- **R1–R7:** the opt-in `aligned` policy brings speculative residuals, GDN,
  short verification and QSA arithmetic closer to serial decode. Slot-local
  guards and checkpoint transaction boundaries protect scheduling state.
  Optional QSA row batching and single-layer KV-only MTP ingest reduce work.
  Shared sampler fixes cover raw-logit top-k/ties, signed zero and dynamic
  prefix-penalty ordering; these fixes are not all gated by `aligned`.
- **R8:** optional GR-read ingest fusion and removal of an unused verifier
  head retain the state needed by subsequent verification.
- **R9:** optional tail-only draft initialization remains a research path. It
  changes the draft distribution and has restrictive cache/slot eligibility;
  it is disabled in the later configurations reported below.
- **R10:** deferred expert scaling, permuted Q4 reads and PLE I/O lookahead
  reduce ordinary prefill work and waiting.
- **R11:** ordered BF16 multi-row accumulation repairs a reproducible greedy
  serial/MTP divergence while retaining batched computation. It requires the
  aligned policy; it does not establish equality for every input.
- **R12:** grouped gate/up plus activation fusion and ordered reduction plus
  Hadamard fusion reduce repeated reads and intermediate transfers. GU mode
  `1` retains the earlier BN32 path; mode `2` selects the later BN64 path.
- **R13:** BF16 tap expansion/RMSNorm fusion and four original-shape strided
  branch projections remove intermediate traffic. Eligibility includes a
  single slot and normal prompt ingestion; repair/bridge and unsupported
  shapes retain fallback paths. Both switches default off. The branch path
  retains four separate original-size GEMMs and additionally requires matching
  dtype/dimensions, sufficient scratch and supported Lt algorithm index 1251
  in the tested runtime; it does not substitute one larger 4P GEMM. The
  proposed flat-4P GEMM and key-only
  projection replacements did not pass the chosen numerical gate and were
  not integrated.

The optional switches described above are off unless explicitly selected;
`aligned` itself enables its documented serial index/attention stages. Runtime
eligibility still matters after a switch is set. Consult
[the dispatch and guards](../src/gpu/parts/40_model.inc) and
[the R13 branch implementation](../src/gpu/parts/41_mtp_branch_strided.inc).

## Controlled same-v2 measurements

Each R10/R12 comparison used one binary, the same v2 weights, fixed input IDs,
serial greedy decoding, 128 output tokens, one slot, context capacity 262144,
8192-token prefill chunks and actual cached-token count zero. Audit/profile
instrumentation was disabled. Three pairs per length were retained, including
slow samples. Percentages below are medians of paired **time reductions**,
not throughput increases or ratios of separately computed medians.

| Stage and input length | Prefill time reduction | Complete request time reduction |
|---|---:|---:|
| R10, 32768 | 5.34% | 4.69% |
| R10, 131072 | 6.65% | 6.37% |
| R10, 260000 | 6.25% | 6.18% |
| R12c, 32768 | 2.264% | 1.861% |
| R12c, 131072 | 2.387% | 2.269% |

R10 compares its three switches off/on. R12c retains the prior optimizations
and compares GU/reduction fusion `0/0` with `2/1`. Every listed prefill pair was
faster, but decode was not uniformly faster. The R10 260K request-time medians
were 247.659 and 232.758 seconds. Do not add or multiply gains from different
stages to claim a measured cumulative speedup. Request time excludes process
startup, model loading, tokenization and cleanup.

R13's additional two switches produced only 0.1667% / 0.1734% median paired
request-time reductions at 131072 / 260000 inputs, relative to MTP with those
switches off. Variation and automatic input-projection kernel selection limit
causal attribution. At 260000 input / 128 output tokens, the enabled MTP path
still took 0.2585% longer than serial at the paired median, with all three
pairs slower. Faster decode alone does not establish faster complete requests.

## R14: real local tool loops

Four isolated instances ran serial/MTP/MTP/serial, each executing an office
fixture and a code-repair fixture: 64 model requests and eight tasks in total.
Tools actually read data, executed bounded SQL or tested the fixture function;
the code test is not an unrestricted shell or a full-repository repair task.
Only random tool-call IDs were normalized when comparing visible trajectories;
all four trajectories per scenario matched, including failed steps.

| Scenario | Serial decode | MTP decode | Task result |
|---|---:|---:|---|
| Code repair | 22.28 tok/s | 40.72 tok/s | 2/2 successful in each mode |
| Office aggregation | 23.05 tok/s | 40.55 tok/s | 0/2 successful in each mode |

Code task mean wall time fell from 96.01 to 51.91 seconds (45.9%); observed
ranges were 85.26–106.76 and 51.90–51.92 seconds. The first serial task had
substantial prefill variation, so its aggregate prefill difference is not
evidence of a stable MTP prefill improvement. Office runs ended with a
tool-protocol failure without submitting an answer. Cache recovery identified
a complete 26-token XML call to unknown `query_ledser` (instead of
`query_ledger`), with SQL `x`; the API mapped the parser failure to `length`,
although the 2048-token budget was not exhausted. Faster decoding did not
solve task completion. These are two repetitions of each fixed task,
not a broad agent benchmark or a business-quality guarantee.

Rates use total output tokens / total decode seconds. Prefill rates use only
new tokens (`prompt_tokens - cached_tokens`). Tool and HTTP time are retained
in task wall time; model loading and final independent scoring are excluded.
API/reqstat-derived draft-acceptance counters can disagree with engine live
counters at completion boundaries and are not an unambiguous acceptance oracle.

## Parameters for an isolated reproduction

Start from the [separate v2 configuration](HGN_V2.md), using the original v2
main file, matching PLE sidecar and tokenizer, with no old weight overlay or
external MTP sidecar. The following are **process environment variables**, not
additional `service.conf` keys. Set them before launch in a clean environment:

```text
GDEC_V2_MOE=grouped
GDEC_V2_NATIVE_HT=1
GDEC_SPEC_PRECISION=aligned
GDEC_SPEC_QSA_BATCH=1
GDEC_SPEC_SERIAL_ORDER_MR=1
GDEC_MTP_INGEST_KV_ONLY=1
GDEC_MTP_INGEST_FUSED=1
GDEC_SPEC_SKIP_UNUSED_FINAL=1
GDEC_V2_MOE_DEFER_SCALE=1
GDEC_V2_MOE_PERMUTE_Q4=1
GDEC_PLE_LOOKAHEAD=1
GDEC_V2_MOE_REDUCE_HAD=1
GDEC_V2_MOE_FUSED_GU=2
GDEC_MTP_TAP_NORM_FUSED=1
GDEC_MTP_BRANCH_STRIDED=1
GDEC_MTP_PREFILL_TAIL=0
GDEC_PREFILL_SKIP_INTERMEDIATE_HEAD=0
GDEC_SPEC_ADAPT=0
GDEC_SPEC_ADAPT_GREEDY=0
GDEC_SPEC_TRACE=0
```

For the R13 long-input comparison, use single-slot BF16 paged KV, context
262144, chunk 8192, fixed gamma 2, greedy decoding, ignore EOS and exactly 128
outputs. Disable RAM/SSD checkpoint reuse (`RCKPT_MAX=0`, `KVSNAP_MAX_GB=0`)
and vision. Use a new connection for each request and verify actual cached=0;
that does not imply cold file caches or a cold GPU. Compare serial with MTP
off/on for only the two R13 flags, using the same binary and full input IDs.
Disable all audit/profile/diagnostic captures for timing, verify the actual
route and activation/fallback messages, and keep every completed sample.
An empty `MTP_FILE` does not disable the embedded MTP head; gamma zero is
adaptive, not serial. See the [protocol and precision notes](SPEC_NUMERIC_ALIGNMENT.md).

This parameter example is not a turnkey reproduction of every research run.
The complete local manifests, raw captures and task harnesses are not all
distributed here. Building the repository or running its available tests does
not by itself reproduce the tables above.

## Evidence boundaries

Historical upstream comparisons used revision `78a41cc` with **w4b + quality
overlay + external MTP**, while this work uses v2 and embedded MTP. There is
no same-v2 upstream-binary performance baseline in this checkpoint. Those
older figures are whole-configuration comparisons, not isolated engine gains;
see [the historical benchmark conditions](HGN_V2_OPTIMIZATION.md).

Finite cold/warm state captures and complete greedy-token comparisons passed
for the tested configurations. They do not prove all hidden states, arbitrary
sampling distributions, multi-slot/checkpoint behavior, every input or task
quality equivalent. Model identity in later runs used path/size/mtime rather
than rehashing all weights. The remaining 260K short-output negative result,
office failures and rejected numerical candidates are part of this checkpoint.
