# Raw GEN speculative-decoding validation

Requires Python 3.11+ (the audit reader uses `hashlib.file_digest`).

These are opt-in clients for an **already running, isolated test engine**. They
never start/restart an engine, change its settings, download a tokenizer, or
grade answer quality. Python's standard library and this checkout's
`tools/qwentok.py` are used. Set `TOK_DIR` to an existing tokenizer directory
containing `tokenizer.json`, or pass `--tokenizer`.

- `protocol_validation.py`: short greedy/sample comparisons, EOS, cancellation,
  warm continuation, disconnect recovery, and opt-in multi-slot flows.
- `long_context.py`: exact-length synthetic inputs, capacity checks before any
  generation, and serial/MTP/chain comparisons. Inputs are retained with hashes.
- `long_codegen.py`: the same client with a sustained Python-code prompt suffix;
  use `--tokens 512` for the R7 code-generation output budget.
- `long_sampling.py`: cold long-input sampling repeats in serial, MTP and chain
  modes, checking same-mode/same-seed tokens, completion and finite logprobs.
- `mixed_concurrency.py`: two-slot mixed long-prefill/short-chain work, sequential
  references, simultaneous dispatch, and first-token-gated overlap.
- `inspect_mtp_audit.py`: read-only framing, tensor-length and raw-bit comparison
  of two completed diagnostic files; no engine, tokenizer or GPU is used.

## Setup and offline checks

Run from the repository root; replace the tokenizer placeholder with your own
directory. `SOURCE` is derived from the client location in this checkout.

```powershell
$env:TOK_DIR = 'path/to/existing/tokenizer'
$env:GDEC_TEST_OUT = 'spec-validation-results'
python tools/spec_validation/protocol_validation.py --selftest
python tools/spec_validation/protocol_validation.py --suite plan
python tools/spec_validation/long_context.py --selftest
python tools/spec_validation/long_context.py --sizes 2050 2052 8192 --dry-run
python tools/spec_validation/long_codegen.py --selftest
python tools/spec_validation/long_codegen.py --sizes 131072 --tokens 512 --modes 0 1 --dry-run
python tools/spec_validation/long_sampling.py --selftest
python tools/spec_validation/long_sampling.py --size 128K
python tools/spec_validation/mixed_concurrency.py --selftest
python tools/spec_validation/mixed_concurrency.py
```

The default actions are offline plans. Plans/selftests open no engine socket and
do not create a results directory. Live results go to `GDEC_TEST_OUT`, or
`cwd/spec-validation-results` when unset. Relative output paths are relative to
the calling working directory. Live entry points create missing parent
directories and refuse an existing result label; use distinct labels for runs.
`long_context.py` token files live in `<label>.inputs` beside `<label>.json`;
`long_sampling.py` retains its input in `<label>.input.json` beside the result.
`long_context.py` and `long_codegen.py` accept output budgets 64, 128 and 512;
`--plan` is an alias of their `--dry-run` option. A 512-token output may end at
the budget before the requested program is complete; these clients do not run
or grade generated code.

## Explicit live examples

The operator must first start an idle engine with matching weights, numerical
flags, context capacity, slots and gamma. Retain that launch manifest and the
binary hash. `--gamma` is a declaration: GEN cannot set it and INFO cannot
independently verify it. Do not run these suites on a service doing other work.

```powershell
python tools/spec_validation/protocol_validation.py --suite greedy --label g4-greedy --gamma 4 --identity 'BUILD+WEIGHTS+PRECISION+CONTEXT+SLOTS' --engine-log 'engine.log'
python tools/spec_validation/protocol_validation.py --suite sampling --label g4-sample --gamma 4 --identity 'BUILD+WEIGHTS+PRECISION+CONTEXT+SLOTS'
python tools/spec_validation/protocol_validation.py --suite lifecycle --label g4-life --gamma 4 --identity 'BUILD+WEIGHTS+PRECISION+CONTEXT+SLOTS'
python tools/spec_validation/long_context.py --run --label g4-long --gamma 4 --identity 'BUILD+WEIGHTS+PRECISION+CONTEXT+SLOTS' --sizes 2050 2052 8192
python tools/spec_validation/long_codegen.py --run --label r7-code128k-g2 --gamma 2 --identity 'BUILD+WEIGHTS+R7-BOTH+CONTEXT+SLOTS+CACHE0' --sizes 131072 --modes 0 1 --tokens 512 --engine-log 'engine.log'
python tools/spec_validation/long_sampling.py --run --label g4-long-sample --gamma 4 --identity 'BUILD+WEIGHTS+PRECISION+CONTEXT+SLOTS' --size 32768 --tokens 64 --engine-log 'engine.log'
# Requires a compatible slot-safe build and at least two reported slots:
python tools/spec_validation/protocol_validation.py --suite concurrent --allow-concurrency --label g4-concurrent --gamma 4 --identity 'SLOT_SAFE_BUILD+WEIGHTS+FLAGS+SLOTS'
# Requires exactly two slots and the process settings printed by its plan:
python tools/spec_validation/mixed_concurrency.py --run --label mixed-g8 --gamma 8 --identity 'SLOT_SAFE_BUILD+WEIGHTS+FLAGS' --engine-log 'engine.log'
```

After external gamma restarts, `protocol_validation.py --compare-files FILE1
FILE2` compares matching greedy cases and refuses differing identity strings.
Exclude gamma from the identity, but include every other material configuration.
No Windows runtime or launch scripts are included here.

## R7 environment variants and state audit

`configs/r7-{off,qsa,kv,both}.json` are portable **engine environment** values,
not client options or complete launch configurations. All four select aligned
precision and disable adaptive gamma. They set both experimental switches
explicitly, so prior environment values cannot silently select another variant:

| Variant | `GDEC_SPEC_QSA_BATCH` | `GDEC_MTP_INGEST_KV_ONLY` |
| --- | ---: | ---: |
| off | 0 | 0 |
| qsa | 1 | 0 |
| kv | 0 | 1 |
| both | 1 | 1 |

For example, in PowerShell 7, load the values **before externally starting a
dedicated engine from that shell**. These commands do not change an existing
engine process. Retain the complete launch settings, config hash, binary hash,
model identity, context, gamma, slot count and cache settings with each result.

```powershell
$r7Config = Get-Content -Raw tools/spec_validation/configs/r7-both.json | ConvertFrom-Json -AsHashtable
foreach ($entry in $r7Config.GetEnumerator()) { Set-Item -LiteralPath ('Env:' + $entry.Key) -Value $entry.Value }
# Performance runs must omit state diagnostics from the new engine environment.
Remove-Item Env:GDEC_MTP_STATE_AUDIT -ErrorAction SilentlyContinue
```

For an independent state comparison, instead load `r7-off.json`, set
`GDEC_MTP_STATE_AUDIT` to a new absolute filename such as the following, then
externally start a dedicated one-slot engine with gamma 2 and context 262144.
The diagnostic refuses an existing file. Use the same weights, tokenizer,
process settings and prompts for each variant.

```powershell
$env:GDEC_MTP_STATE_AUDIT = Join-Path (Get-Location) 'audit-off.bin'
# After the dedicated engine is ready:
python tools/spec_validation/long_context.py --run --label audit-off --gamma 2 --identity 'BUILD+WEIGHTS+R7-OFF+CTX262144+SLOTS1+CACHE0+AUDIT' --sizes 2050 2052 8192 --modes 1 --tokens 64
```

After that engine has stopped and closed its file, repeat using `r7-kv.json`
and then `r7-both.json`, with distinct audit filenames, labels and identities.
Compare the completed files offline:

```powershell
python tools/spec_validation/inspect_mtp_audit.py audit-off.bin audit-kv.bin
python tools/spec_validation/inspect_mtp_audit.py audit-off.bin audit-both.bin
```

The parser checks framing, contiguous offsets, dtype/shape lengths and event
sequence, then reports per-tensor raw byte/bit/element differences and event
counts. Exit codes are 0 for identical files, 1 for valid differences and 2 for
malformed/unreadable files. It does not modify its inputs. These snapshots cover
newly ingested K/V ranges, newly completed pooled-key blocks, the raw ring,
ingest token IDs and valid step logits, not all historical model state. Check
the reported vocabulary and full-vocabulary event counts before claiming full
logits coverage. State auditing synchronizes GPU work and copies data to the
host; its timings must not be used as performance evidence. See
`docs/SPEC_NUMERIC_ALIGNMENT.md` for the experimental switch and format details.

`long_sampling.py` defaults to 32768 input tokens, 64 output tokens, modes
0/1/4, seed 777 and two repeats per mode (six requests). Its sampling parameters
match the short suite: temperature 0.8, top-k 20, top-p 0.95, min-p 0.0, with
target token logprobs requested. Add `--seeds 777 20260930` for both seeds used
by that suite. `--size 128K` means 131072 input tokens and needs at least 131136
context tokens with the default output budget. Disable RAM/SSD prefix caches
externally: the client requires zero cached tokens and does not apply settings.
It retains every token/logprob, input hash, identity, timings, actual drafter and
cache counters. Same-mode repeat or request-check failures are nonzero exits;
missing actual-drafter coverage is incomplete, not a pass. Cross-mode token
equality is observational only, and finite target logprobs do not establish
distribution fidelity.

## Interpret the evidence

- **Completion is not an all-pass result.** Inspect record checks, comparisons,
  skipped cases, actual drafter, cache state and branch coverage. `done` and
  `length` are legitimate endings; a capped answer has not been fully evaluated.
- Greedy token equality is a regression target under matched prompts, weights,
  precision, cache state and output budgets; it is not an answer-quality score.
- Sampling: same-mode/same-seed repeats require matched cold state and numerical
  paths. Serial and speculative modes may consume RNG draws differently, so
  cross-mode same-seed inequality is **not** a failure. Mode 3 sampled fallback
  should report actual drafter 0 and match serial. A few seeds do not establish
  distribution fidelity.
- Cancellation's abbreviated `D ... cancel 0 0 0.0 0.0` is not a full token
  counter. Previously emitted and buffered tokens are allowed; the count check
  is not applicable. A cancellation racing completion is inconclusive. Check
  cancellation acknowledgement, latency and connection reuse. Disconnect
  recovery proves liveness, not immediate cessation of GPU work.
- Warm continuation needs `cached_tokens > 0`; a fresh connection is not assumed
  cold without checking its counter. Different warm/cold prefill paths can
  produce numerical differences; mismatches are unresolved regression evidence,
  not automatically request contamination or a pass. Turn 1 depends on turn 0.
- A retained two-slot concurrency run had **three turn-1 mismatches** (serial,
  MTP and chain) with identical input IDs but warm sequential / cold concurrent
  prefixes (101/102 cached tokens versus 0). All turn-0 flows matched. This is
  **not an all-pass concurrency result**. A follow-up sequential cold replay
  matched all six concurrent cold turn-1 outputs exactly, including those three
  divergences. Thus concurrency is not required to reproduce these examples;
  the warm/cold numerical discrepancy remains and is not discarded as a pass.
- Actual drafter and nonzero proposal/round counters establish speculation;
  mode selection alone does not. Chain logs must show positive ngram and MTP
  rounds to establish both paths. Untagged log windows can include a preceding
  cancelled/disconnected request; do not attribute them blindly. `committed`
  can include more than accepted draft tokens, so `committed/proposed` is not a
  reliable acceptance-rate definition.
- Mixed-request wall overlap does not prove a scheduler branch executed. Inspect
  lending and drafter logs. Only explicit early-yield evidence confirms that
  exact branch; absent logging means unknown, not zero activity.
- Compare decode as emitted/generated tokens divided by decode time, separate
  from prefill and end-to-end latency. Prefill needs the actually evaluated,
  uncached token count. Match input/output lengths and cache state; isolate
  startup effects and repeat measurements. Synthetic repetitive long inputs do
  not establish real-document speed, retrieval quality or production stability.

## R8 independent environment variants

`configs/r8-{off,fused,head,both}.json` keep the R7 row-batched QSA and KV-only
ingest experiments **enabled**, then vary only the two R8 changes. Thus
`r8-off` means R8-off/R7-both, not the legacy engine or all experiments off.
All four select aligned precision, disable adaptive gamma, explicitly set both
new switches, and set ingest profiling to 0.

| Variant | `GDEC_MTP_INGEST_FUSED` | `GDEC_SPEC_SKIP_UNUSED_FINAL` |
| --- | ---: | ---: |
| off | 0 | 0 |
| fused | 1 | 0 |
| head | 0 | 1 |
| both | 1 | 1 |

These files are engine environment values, not live client options or complete
service configurations. Load one before externally starting the isolated engine.
Explicitly clear inherited diagnostic switches for performance runs; some older
diagnostics use presence semantics, so assigning 0 is not equivalent to removing
them.

```powershell
$r8Config = Get-Content -Raw tools/spec_validation/configs/r8-both.json | ConvertFrom-Json -AsHashtable
foreach ($entry in $r8Config.GetEnumerator()) { Set-Item -LiteralPath ('Env:' + $entry.Key) -Value $entry.Value }
foreach ($diagName in @('GDEC_MTP_STATE_AUDIT', 'GDEC_MTP_INGEST_PROFILE', 'GDEC_PHASE', 'GDEC_PHASE_PROF', 'GDEC_PROF', 'GDEC_PERF', 'GDEC_TIME')) {
    Remove-Item -LiteralPath ('Env:' + $diagName) -ErrorAction SilentlyContinue
}
```

Before speed comparisons, perform a separate one-slot raw audit for each variant
with unique absolute audit paths and the same cold inputs. Reuse the R7 audit
commands above with R8 identities; compare complete files using
`inspect_mtp_audit.py`. Include sparse-boundary prompts, full prompt chunks and
accepted-token repairs. Audit agreement is bounded by recorded tensors and
prompts; it is not complete model-state or arbitrary-input proof.

For one dedicated diagnostic run, load a variant and set
`GDEC_MTP_INGEST_PROFILE=1` before starting its engine. Sum records by prompt
chunk or repair batch using `base` and `P`; do not mix these groups or attribute
the whole prefill difference to the existing decode-only `spec time: ingest`
line. The new event diagnostic synchronizes each ingest and cannot supply the
timings used to claim end-to-end speed. See the R8 section of
`docs/SPEC_NUMERIC_ALIGNMENT.md` for interval boundaries.

After all diagnostics are disabled, compare identical input IDs, output limits,
sampling settings, gamma, caches and slots with alternating/reversed order and
repeats. Keep serial and MTP requests for each variant; report TTFT, generation
rate and complete request wall time separately. The head change affects the
MTP verifier, not common prompt prefill. A small positive wall-time difference
from one pair is not a stable-speedup result.

Head-skip uses the normal existing batched readback on successful requests and
an exception-only stream drain before freeing caller-owned token storage. Its
guard refuses undersized borrowed scratch while snapshots are live, so test
mixed concurrency explicitly. Keep the flags independently controllable for
regression isolation; they remain experimental and default off.

## R9 cold-request tail-proposer variants

`configs/r9-{full,tail8k,tail32k}.json` all keep the R8 `both` settings: aligned
precision, row-batched QSA, KV-only ingest, fused ingest and skipped unused final
head, with adaptive gamma and ingest profiling off. They additionally set one
slot and disable both RAM/SSD checkpoint tiers. Only the requested tail window
differs:

| Variant | `GDEC_MTP_PREFILL_TAIL` | Initialization requested |
| --- | ---: | --- |
| full | 0 | Original complete MTP prefix |
| tail8k | 8192 | At least 8192 ingest rows, rounded to original chunk boundary |
| tail32k | 32768 | At least 32768 ingest rows, rounded to original chunk boundary |

The files contain process environment values, not complete service/client
configuration. Select **pure MTP** in the client; the new tail path is serve-only.
Tail defaults off in the engine. For an effective experiment, send a cold text
request and inspect `[mtp-tail] enabled`/`ready` with the actual B, row count and
pooled-block interval. Merely loading a preset does not establish activation.

```powershell
$r9Config = Get-Content -Raw tools/spec_validation/configs/r9-tail8k.json | ConvertFrom-Json -AsHashtable
foreach ($entry in $r9Config.GetEnumerator()) { Set-Item -LiteralPath ('Env:' + $entry.Key) -Value $entry.Value }
# These older settings use presence semantics or are diagnostic-only.
foreach ($clearName in @('GDEC_QSA_DENSE', 'GDEC_NOSPEC', 'GDEC_NOSPEC_SAMPLE', 'GDEC_MTP_STATE_AUDIT', 'GDEC_MTP_INGEST_PROFILE', 'GDEC_PHASE', 'GDEC_PHASE_PROF', 'GDEC_PROF', 'GDEC_PERF', 'GDEC_TIME')) {
    Remove-Item -LiteralPath ('Env:' + $clearName) -ErrorAction SilentlyContinue
}
```

Start an isolated engine externally after applying the environment. Never change
the preset underneath a running process. Keep model, input ids, batch/context
size, sampling, gamma and output budget fixed across full/tail runs. The full
preset also disables the caches so it is a matched cold-request reference.

The bounded implementation requires cold, one-slot, text, sparse QSA, pure MTP,
KV-only and both caches off. Uncovered cases explicitly use full initialization.
An old-tail same-connection warm continuation forces a full target+MTP recompute
and disables tail for that request. Check that fallback and its timing cost;
the candidate does not provide a tail-aware warm cache. Ngram/chain, multi-slot,
dense, multimodal, short input and RAM/SSD-enabled cases also require fallback
checks, not a claim that those paths used tail initialization.

Do not use full-versus-tail complete audit-file equality or raw draft-logit
equality as the R9 gate. The target remains full-context, but proposal q changes
by design. In separate diagnostic runs, enable a unique absolute
`GDEC_MTP_STATE_AUDIT` path and inspect optional tail metadata and the 512 absolute
`selected_blocks`. The engine asserts their valid range and strict ordering.
Compare retained ingest tensors by their absolute base/row/block positions,
and separately establish identical target prompt state/initial logits. Test all
modulo-four endings, reversed paging, poisoned missing-prefix diagnostics,
accepted/rejected repair, greedy output, sampled repetition and proposal-q
correction. Existing finite-precision serial/verify caveats still apply.

Disable all diagnostics before measuring performance. Report actual retained
rows, request cache/activation/fallback state, proposal acceptance, TTFT, decode
and whole-request duration. Alternate/reverse order and repeat. Changing q can
hurt acceptance and offset skipped initialization. These presets and instructions
describe an experiment; no performance or universal precision conclusion is
implied. See `docs/SPEC_NUMERIC_ALIGNMENT.md` for state and cache boundaries.

### Inspect a completed R9 full/tail raw-audit pair

The standard-library-only analyzer can run from any working directory and never
contacts the engine. Use one cold pure-MTP request per file, with the same full
input token IDs and a positive tail window in the second run:

```powershell
python tools/spec_validation/inspect_r9_tail_audit.py C:/audit/full.bin C:/audit/tail.bin --prompt-tokens 32768 --vocab 248320 --output C:/audit/tail-review.json
```

The output path must be new. The analyzer checks all recorded full-vocabulary
logits for finiteness, host/device positions, actual 512 selected blocks for
strict order/uniqueness and initialized bounds, and every retained prompt tensor
(K, V, pooled keys, raw ring and token IDs) against its full-prefix counterpart
by exact bytes. Inspect `tail_range` for the actual rounded window. Decode draft
logits may intentionally differ because proposal q changed. This bounded audit
does not prove target-state equivalence, arbitrary-input output quality or a
speedup; run those gates separately and disable auditing for timing tests.
