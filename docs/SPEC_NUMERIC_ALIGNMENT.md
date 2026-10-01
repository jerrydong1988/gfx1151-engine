# Experimental speculative numerical alignment

`GDEC_SPEC_PRECISION=aligned` makes target-side speculative verification use
arithmetic closer to serial decode. The default remains `legacy`; the policy is
opt-in and does not change model weights or claim universal bitwise equivalence.
Invalid policy values fail at startup.

## Scope

The policy covers greedy MTP, sampled MTP, ngram replay, and their chain modes.
During a speculative transaction it keeps residuals and the GDN recurrence in
FP32, uses the serial convolution accumulation/activation order, and avoids the
extra BF16 normalization boundary. Small verifier GEMMs use supported batches of
at most eight rows, falling back to serial GEMV where no matching kernel exists.
V2 expert verification uses the direct small-batch path; large prompt prefill
retains the configured optimized kernels. QSA indexer projection uses the serial
quantized projection arithmetic during verification.

Policy version 2 also aligns two QSA stages while the precision guard is active:

- Trunk index scores and top-512 selection run one row at a time through the
  existing serial kernels, with the full `index_blocks` score stride, each row's
  absolute position, and the original paged-key addressing. Projection, pooled
  key construction, and the rest of the batch remain unchanged.
- Attention runs one row at a time through the existing serial split-attention
  kernel and FP32 partial merge, including dense rows. QKV projection, preparation,
  and cache writes retain their batch implementations. This shared attention
  helper also covers MTP ingest performed inside the precision guard; its scope
  is broader than trunk verification alone.

Both stages are enabled by default under `aligned`. For independent ablations,
set `GDEC_SPEC_INDEX_SERIAL=0` or `GDEC_SPEC_ATTN_SERIAL=0` to restore the respective
batch stage. These switches do not enable alignment under `legacy`. The main
prompt prefill runs outside the guard and keeps its configured optimized paths.

This is an arithmetic-path alignment, not an answer-quality guarantee or global
bitwise-equivalence claim. Other kernels, state histories, and configurations
still need validation, and small remaining logit differences can matter at a
near tie. Passing finite greedy test sets does not prove all prompts will emit
identical tokens.

The new per-row QSA branches use host-selected `base + row` position pointers.
They are intended for the current host-driven speculative rounds and are not
validated for graph capture/replay across changing positions. A future replay
path must supply current positions rather than retain capture-time pointers.

## Scheduling and cache state

Precision state is stored per sequence slot and reset with the slot. The guard
spans verification, acceptance, rollback/replay, result reads, and MTP ingest.
Phase flags are restored on exceptions. Cooperative scheduling happens only
before a shared speculative checkpoint is saved; a large verifier requeues
before borrowing an undersized workspace. A verifier does not lend the GPU in
the middle of its checkpoint transaction.

The aligned policy has a separate model-cache fingerprint (policy version 2),
including both QSA ablation switches. Legacy cache identity is unchanged. Do not
reuse old experimental cache files manually or infer correctness from an HTTP
health response alone.

## Configuration boundaries

Do not combine `aligned` with `GDEC_GDN_LOOP` or `GDEC_QSA_LOOP` (presence,
including `=0`, enables those older diagnostic fallbacks), or active
`GDEC_V2_SMALL_EXACT`, `GDEC_V2_SMALL_DOT_EXACT`, or `GDEC_V2_SMALL_ORDERED`.
Startup rejects these combinations: GDN_LOOP omits needed speculative snapshots,
QSA_LOOP bypasses serial split attention, and the small-MoE switches change
arithmetic according to total batch size. The older `GDEC_SPEC_*` diagnostic
flags remain for ablation, not as the recommended configuration.

Use the same checkpoint, expert mode, native-HT choice, token IDs, context size,
and sampling settings when comparing paths. Fix `GDEC_SPEC_GAMMA` and disable
adaptive draft depth when attributing a test to a particular depth. The Windows
launcher obtains gamma from `MTP_GAMMA` in its service configuration.

## Valid comparisons

For greedy decoding, exact token equality against serial output is a useful
regression target. Include actual ngram proposals, MTP proposals, early EOS,
length limits, cancellation, reused slots, simultaneous requests, and contexts
crossing sparse-attention boundaries. A requested drafter that proposes no
tokens is not evidence that its verifier was exercised.

For sampling, identical seed does not require identical serial/speculative
text: the two algorithms consume random draws differently. Test the target and
proposal distributions, rejection correction, log-probability validity,
same-mode reproducibility, and request isolation separately. Distribution
correctness for computed probabilities does not prove that two floating-point
forward paths compute exactly the same probabilities.

The CPU sampler now chooses top-k on corrected raw logits and resolves ties by
token ID, matching the GPU candidate path. Earlier CPU code selected after
`expf` using unspecified tie order, which could change the retained support
when distinct logits rounded to equal probabilities. This correction affects
sampling in both policies and may change old fixed-seed text; greedy behavior
is unaffected. It does not make serial and speculative RNG draw schedules equal.
GPU radix top-k now canonicalizes positive and negative zero to the same bucket,
so candidate collection preserves the same token-ID tie rule for signed zero.

Dynamic prefix penalties also use the CPU correction order: accumulate integer
occurrence counts, compute the complete merged penalty once, and subtract it
once from the post-bias logit. The GPU preserves those post-bias values in small
shared scratch rather than subtracting history and prefix penalties separately.
The original logits remain untouched for dense fallback. The current prefix
capacity is 64; this follows the existing MTP/ngram round limits. See
`tools/sampler_prefix/README.md` for the real-kernel before/after probe. This
correction is independent of the forward precision policy and does not make
the two forward paths or RNG schedules identical.

Long synthetic repeated prompts measure capacity and numerical regressions;
their throughput and output agreement should not be generalized to arbitrary
documents or business-data accuracy.

## Optional long-input experiments

These switches are off by default and retain separate cache identities.

- `GDEC_SPEC_QSA_BATCH=1` batches independent rows of the aligned index-score,
  selector and split-attention/combine launches. Each row retains the serial
  kernel's arithmetic, block shape and selector dispatch based on the full
  score stride. It only applies while the aligned guard and the corresponding
  `GDEC_SPEC_INDEX_SERIAL` / `GDEC_SPEC_ATTN_SERIAL` stage are enabled. Partial
  attention scratch holds at most 65 rows (up to 48.375 MiB extra); larger
  ranges are chunked. The arena estimate includes the extra allocation.
- `GDEC_MTP_INGEST_KV_ONLY=1` builds the current single MTP layer's K/V and
  indexer state without computing unused query/attention-output/MLP results.
  The real draft step is unchanged. Input mixing, K/V projection, normalization,
  RoPE, page mapping, index pool/ring order and position updates are retained.
  This is specific to the current single-layer MTP state dependencies, not a
  generic optimization for arbitrary multi-layer draft models.

For read-only raw-state diagnostics, `GDEC_MTP_STATE_AUDIT` may name an absolute
new file. It records ingest K/V/indexer/ring tensors and each draft step's valid
raw logits (respecting a reduced draft vocabulary). The format starts with
`MTPAUD01`, then repeats little-endian uint64 JSON-length and payload-length,
UTF-8 JSON tensor descriptors, and raw tensor bytes. Existing files are refused.
The diagnostic synchronizes and reads back the GPU and must **not** be enabled
in performance measurements. Validate with one slot; it is not a concurrent
audit facility. It is disabled without a nonempty environment value.

Adaptive gamma currently optimizes a fixed cost model from acceptance rates;
it does not measure the complete real round cost or select serial execution.
A larger maximum context or deeper draft is not evidence of an acceleration.
Compare complete request time and decode time separately, with the actual
input length, cache state and output budget held constant.

## R8 ingest fusion and unused verifier head experiments

All switches in this section are **off by default**. They add no model-weight
conversion and do not establish universal bitwise equivalence or a guaranteed
end-to-end speedup. Compare each change independently on the same binary.

- `GDEC_MTP_INGEST_FUSED=1` only takes effect together with
  `GDEC_MTP_INGEST_KV_ONLY=1`. In the attention GR-read used by ingest, the
  existing RMSNorm kernel emits both its FP32 output and BF16 conversion; the
  existing fused combine emits sigmoid, branch combination and BF16 conversion
  without the separate intermediate passes. Projection shapes, intended FP32
  branch order and reduction width remain unchanged. The real `mtp_step` is
  unchanged. This applies to prompt and accepted-token repair ingests; it is
  not a different KV precision. The temporary sigmoid buffer is no longer
  materialized because the KV-only remainder does not consume it. Normalized
  FP32 values remain available, and the BF16 conversion-cache metadata is
  updated for the new output. Raw-state/logit equality must still be tested.
- `GDEC_SPEC_SKIP_UNUSED_FINAL=1` removes an unused single-row mixer/head/argmax
  from the four MTP verifier paths (pure/chain, greedy/sampling). The following
  all-row logits remain the source of acceptance and continuation decisions.
  It does not remove the real prompt-final head or change ordinary prompt
  prefill. The fast path requires an active MTP verify phase, no prompt tap
  capture or progress session, and one batch fitting `maxbatch` and its capacity;
  other configurations use the original final-head path.

The head-skip helper calls `prefill_chunk` with the caller-owned token vector,
which remains alive until the following existing batched readback. It must not
simply use `prefill_batch(final=false)`: that wrapper creates a temporary token
vector whose lifetime would end before an asynchronous H2D necessarily drains.
All trunk residual, position, K/V, GDN, convolution and snapshot writes remain.
An exception-only guard drains the captured stream before destroying the token
vector during C++ unwinding; successful execution adds no synchronization.
That guard is not a recovery mechanism for a failed GPU or a terminated process.

Workspace ownership must be acquired by `spec_round_yield` **before** saving
shared snapshots. If head-skip discovers a borrowed workspace smaller than its
verify batch, it reports an error instead of yielding with live snapshots or
continuing into undersized scratch. Retry with head-skip disabled, or use a
tested `GDEC_CONC_RMAX` large enough for the verifier. This boundary needs mixed
request tests; a single-slot pass does not validate concurrency.

Fused ingest has an additional cache salt only when both ingest flags are
enabled. Head-skip has its own cache salt when enabled. Leaving these switches
off preserves the prior fingerprints. Keep variants in their own caches; finite
raw-state equality does not justify manually mixing experimental cache files.

### Ingest timing diagnostic

`GDEC_MTP_INGEST_PROFILE=1` enables HIP-event timing for each ingest. Lines have
the prefix `[mtp-ingest-prof]` and include `base`, `P`, `kv_only`, `fused`,
`input_ms`, `gr_ms`, `state_ms`, and `total_ms`:

- Input: token H2D plus input mixing.
- GR: the attention GR-read interval.
- State: the remaining ingest path and device position update. With KV-only
  this is K/V and indexer work; without KV-only it also includes the old unused
  attention-output/MLP computation.
- Total: first-to-last event interval, excluding `kv_prepare`/COW, CPU setup
  and raw-state-audit readback. GPU intervals may include host enqueue gaps.

The diagnostic allocates four events per ingest and deliberately synchronizes
the last event before printing and destroying them. Normal operation adds no
GPU event, allocation or synchronization from this diagnostic. **Disable it for
all end-to-end performance comparisons**; use `base`/`P` to distinguish prompt
chunks from small decode-repair batches. It does not replace a verifier-wide
GDN/QSA/MoE/head profile and does not alter cache identity.

For short answers after a very long uncached prompt, most request time is
shared trunk prefill. Removing all MTP initialization can at most recover the
observed generation-time saving if generation speed is held fixed. Therefore
small net improvements require repeated paired measurements; a higher decode
token rate alone is not evidence of a stable overall advantage.

## R9 bounded MTP tail-prefill experiment

`GDEC_MTP_PREFILL_TAIL=8192` (or `32768`) is an **off-by-default** proposal-only
experiment. Unset/0 retains full MTP initialization. Other values must be an
integer >=8192; invalid values log and fall back to full initialization. This
section describes the implementation and its required validation, not a measured
performance improvement or a universal output-equivalence result.

The target model still prefills the entire prompt with its original weights,
context, positions and verifier. The MTP proposal layer initializes a fixed
suffix of its own K/V and indexer state. Full-trunk hidden taps are used for each
retained position. Its draft distribution q, draft tokens, acceptance rate and
subsequent verifier batching can change. The actual q produced by each draft
step is still used by the existing p/q acceptance and residual-sampling paths.
This does not inherit a bit-identical-logit/output promise from the R7/R8
arithmetic/scheduling experiments; existing serial/verify numerical limitations
remain independently relevant.

### Eligibility and fallback

The first implementation enables tail initialization only for a **serve-mode
pure-MTP cold request**, after live/RAM/SSD reuse decisions, with all of:

- One sequence slot; text input without active M-RoPE.
- Sparse QSA and `GDEC_MTP_INGEST_KV_ONLY=1`.
- Both actual checkpoint tiers disabled: `!rckpt::enabled()` and an empty SSD
  snapshot directory configuration. Use `GDEC_RCKPT=0` and `GDEC_KVSNAP=0`;
  neither clearing snapshot hints nor `GDEC_CKPT_TOKENS=0` disables those tiers.
- Enough prompt rows for a positive, four-aligned original chunk boundary that
  leaves at least the requested number of ingest rows.

Warm/checkpoint reuse, enabled caches, dense QSA, multiple slots, multimodal
input, chain/ngram modes and short/unaligned chunk cases use the original full
initialization path and log `[mtp-tail] full-prefill fallback` with a reason.
The switch is not activated by CLI prefill or serve startup warmup. Several older
engine switches, including `GDEC_QSA_DENSE`, use presence semantics: remove that
variable rather than setting it to 0 when testing sparse QSA.

If a preceding request used a tail and the next same-connection request would
reuse that live prefix, this first version **forces a full target and MTP
recompute** and does not enable tail for that new request. It cannot just clear
the lower bound and continue, since the old MTP prefix is absent. This is an
explicit warm-request performance cost; the log reports the discarded cached
prefix. Ordinary continuation from a complete MTP state retains its old behavior.

### Absolute state and actual window

For N prompt tokens, initial MTP ingestion covers [B,N-1); the last prompt row
is consumed by the first drafting step. B is rounded down to an original prefill
chunk boundary, requiring a multiple of four. The target and retained MTP chunk
shapes remain unchanged; the actual suffix can therefore exceed the configured
window. For example, with N=260000 and batch=8192, window=8192 retains 14239 ingest
rows from B=245760, while window=32768 retains 38815 rows from B=221184. Check the
activation/ready logs instead of assuming exactly the configured row count.

B remains fixed for that request while generated rows extend the valid suffix;
this is not a sliding-window or reduced-target-context implementation. The
absolute MTP position and raw index ring are explicitly initialized. Four-aligned
start and at least four first-chunk rows prevent pooling an absent left-hand
history row. Tail fields follow slot binding and are cleared by reset_state.

The MTP score kernel enumerates only initialized pooled blocks, reading each
key through `ik_blk(ptab, relative_block + B/4)`. The existing selector receives
compact relative candidate ids; selected ids are shifted back to absolute blocks
before unchanged sparse flash attention. RoPE, K/V stores and page positions
stay absolute. At least 512 complete valid blocks are available, so attention
uses 512 selected blocks plus the absolute 0..3-token incomplete tail. Masking
absent-prefix scores with -Inf would be insufficient because selector ReLU can
turn them into zero-score candidates; this implementation excludes those ids.

The target still allocates its complete shared KV pages. Tail mode does not
promise a reduction in the full target's KV pool size. The normal tail path adds
no diagnostic readback or forced stream synchronization. It skips old prompt tap
captures and MTP ingests and changes only MTP candidate enumeration.

### Cache isolation and diagnostic scope

The requested nonzero window has a separate versioned cache-fingerprint salt,
even when a request falls back. Effective tail requests require RAM/SSD caches
off because their current formats cannot represent a missing MTP prefix. Do not
share experimental snapshots or toggle policy in a running process. Environment
values are read once, matching existing process-level engine settings.

With `GDEC_MTP_STATE_AUDIT=<new absolute path>`, an effective-tail record adds
`tail_begin`, `tail_prompt_end`, valid pooled-block bounds and recent-token bounds.
Each draft-step record also contains `selected_blocks` (512 int32 absolute ids).
The diagnostic asserts that ids are strictly increasing and all fall within
[B/4, floor((device_mtp_pos+1)/4)); it also checks the recent-token lower bound.
Without effective tail, the previous audit record layout remains unchanged.
These checks run in the existing synchronized audit path. Disable diagnostics
for performance measurements.

Required validation includes target prompt state/initial logits, corresponding
retained K/V/index/ring rows, all modulo-four endings, paged/reversed layouts,
rejection repair, greedy output, sampling repeatability and actual-q correction,
successive requests, and every documented fallback. Raw full-versus-tail draft
logits and complete audit files are expected to differ and are not an equality
gate. Runtime range checks do not by themselves prove target output equivalence,
sampling-distribution equality, absence of all invalid reads, or stable speedup.
Benchmark acceptance, repair/verifier costs, TTFT, decode and whole-request time
separately with repeated matched runs; a worse proposer can erase any setup saving.

## BF16 multi-row accumulator order experiment (R11)

`GDEC_SPEC_SERIAL_ORDER_MR=1` enables the ordered BF16 multi-row kernel only
inside the aligned speculative FP32 guard. The default is off; legacy policy,
ordinary prompt prefill and unsupported/non-BF16 dispatch retain their existing
paths. The flag has a separate aligned cache-fingerprint salt. Set it before
starting a process, not while a request is running.

The ordinary BF16 MR kernel first sums eight products, then adds that subtotal
to the lane accumulator. Serial GEMV adds each product to its persistent lane
accumulator. Floating-point arithmetic makes these different operations even
when both use FP32. The ordered specialization retains weight sharing across
rows but copies serial's BF16 expansion, per-lane accumulation chain and lane
reduction. It does not round weights or activations to a lower precision.

The guard includes target verification and accepted-token MTP rebuilding, and
also covers chain/ngram speculative drivers. Consequently this switch can affect
eligible MTP ingest projections as well as target verification. It is not a
promise that draft proposals remain identical. Large prompt prefill lies outside
the guard and retains the existing optimization settings.

The initial HGN v2 regression uses 32,768 identical input IDs and 128 greedy
output IDs. Disabling all MR kernels removes the prior token-83 divergence;
enabling only ordered BF16 MR does so as well. Turning this flag off in the same
new executable restores that divergence. Keep this bounded causal evidence
separate from a claim of equivalence for every model, context or sampler.

In particular, the independent GGUF dtype-8 dispatch at some K dimensions uses
different lane-reduction widths in serial and MR; this BF16 repair does not
change that path. Same-seed sampled output equality is not the sampling oracle.
Use exact same-prefix numerical comparisons and distribution-specific tests
when extending coverage to random sampling.

## R12 opt-in grouped Prefill fusion

Two independent, default-off switches target HGN v2 precise Group128 experts:

- `GDEC_V2_MOE_REDUCE_HAD=1` combines the existing rank-ordered weighted
  reduction with the inverse 128-point Hadamard. It retains rank order,
  operand order, normalization and the final sign multiplication.
- `GDEC_V2_MOE_FUSED_GU=1` replaces GU plus activation with a BM256/BN32
  kernel. Gate and up retain the existing grouped high/low WMMA and scale
  accumulation chains. The epilogue transposes to LDS, then each wave handles
  128 channels using four values per lane. The seven-stage Hadamard tree is
  unchanged; the last two stages use registers instead of cross-wave barriers.
  The result has the same split-FP16 layout consumed by the unchanged down
  kernel. This does not requantize weights or drop the low activation plane.
- `GDEC_V2_MOE_FUSED_GU=2` selects the R12c BM256/BN64 variant with512
  threads (16 wave32 waves) per block. Waves0-7 own gate rows and waves8-15
  own the matching up rows; every lane retains four token fragments with the
  same per-output WMMA, low-plane compensation and scale-accumulation order.
  The wave4 Hadamard/activation/split-half epilogue is unchanged. This restores
  weight reuse across64 slots while retaining the fused intermediate layout.

The GU selector accepts only the exact strings `1` and `2`; missing, `0`, and
all invalid values disable GU fusion. Value `1` retains the R12b kernel,
256-thread/grid-Z2 launch, marker and cache salt without alteration. Value `2`
uses a separate namespaced kernel and cache salt. Its marker is
`[v2-moe] experimental grouped GU/activation BN64 fusion active (mode=2)`.

The common router continues to produce tiles of at most64 expert-sorted
slots, with the same `(P*k+63)/64+experts` allocation/launch upper bound. GU=2
consumes each tile once (grid-Z1); it does not change the tile list or the
following down projection. Down retains BN32/grid-Z2 below its existing
threshold and BN64/grid-Z1 above it, reading the same slot-major split-FP16
buffer. The GU=2 kernel declares26880bytes of LDS, reusing it for its16384-byte
transpose after the K loop. It adds no workspace allocation or host sync.

Both switches are gated to precise grouped batches with P > 16, k = 10,
aligned-spec inactive and dimensions divisible by 128. Unqualified paths and
the small serial/verification kernels retain their previous dispatch. Each
enabled switch gets a separate cache fingerprint salt; default-off identity
is unchanged. Workspace allocations are unchanged, since the down projection
still requires the original large scratch buffer.

The profiler reports `expert_reduce_rotate` and `expert_gu_activation` as
replacement intervals. They must not be added to the old component intervals
as though they were additional work. Event capacity remains within the old
six-scope expert upper bound.

Synthetic microprobes check finite bitwise outputs, tails, guards and immutable
inputs. The standalone wave4 activation probe preserves NaN classification
but observes NaN payload differences; it is not a claim of all-bit-pattern
equivalence. Full-model raw state/logit checks and profiling-off request
comparisons remain separate acceptance evidence. Source transformations or
isolated kernel speedups alone do not establish end-to-end gains.
