# Experimental Flash-Next v2 HGN support

This branch adds a **correctness-first** path for `qwen38-flash-next-v2.hgn`
and its separate `qwen38-flash-next-ngram.hgn` PLE table. It is not a claim
of parity with Halogen's optimized kernels or of production readiness.
The original weights are read-only; no conversion file or requantized copy
is produced.

## Implementation

* Dense HT (storage 16, parameter `0x1208`) and Q6 (storage 24, parameter 64)
  are decoded once on the GPU to BF16. This adds about **7.83 GiB** for this
  model, while the compressed source tensors also remain in the arena.
  BF16 rounding is an explicit numerical difference from a native compressed
  matrix multiply. The indexer reference projection still uses FP32 weights.
* Routed experts (storage 23, parameter 128) remain compressed on the GPU.
  The default `exact` batch kernel shares each expert row across eight tokens
  while preserving the original FP32 accumulation and router-rank order.
  Its packed weight reads and small-batch kernel preserve the same per-lane
  column order and FP32 reduction tree, without FP16 matrix operands.
  Explicit `wmma` and `compensated` modes instead reuse the tiled matrix-core
  pipeline, with rotations around the nonlinear activation. These faster
  modes change floating-point rounding and are not output-equivalent modes.
* Optional native HT projection kernels read the compressed trellis weights
  directly for single-token and small-batch generation. Dense BF16 copies
  remain available for prefill; this option does not remove the 7.83 GiB cost.
* The PLE direct-I/O handle is selected from the mapping that owns the table,
  including a separate sidecar. The sidecar is part of the existing KV-cache
  file identity. `NGRAM_FILE` is understood by the Windows and HGN launchers.
* Unknown variants, inconsistent payload sizes and missing rotation tensors
  fail before allocating the model arena. Missing PLE sidecar reports an
  explicit error. Old Q4CP models keep their existing dispatch.

For an initial Windows test, use a separate checkout/build and a short
context. Do not replace a working installation until end-to-end checks pass:

```ini
MODEL_DIR="C:/models/halogen-qwen3.8-flash-next"
MODEL_FILE="$MODEL_DIR/qwen38-flash-next-v2.hgn"
NGRAM_FILE="$MODEL_DIR/qwen38-flash-next-ngram.hgn"
OVERLAY_FILE=""
MTP_FILE=""
VISION_FILE=""
TOKENIZER_DIR="$MODEL_DIR/tokenizer"
MAX_CONTEXT="4096"
PARALLEL="1"
ENGINE_HOST="127.0.0.1"
ENGINE_PORT="18870"
API_HOST="127.0.0.1"
API_PORT="18871"
KVSNAP_MAX_GB="0"
```

The external MTP sidecar and vision tower are omitted from this recipe.
The v2 main file itself already contains an MTP head in the older supported
formats. Empty `MTP_FILE` therefore does **not** disable drafting, and
`MTP_GAMMA=0` means adaptive drafting. Start with the engine CLI `--gen N`
serial path before testing service-side MTP verification (`--spec-gen N`
is the explicit speculative CLI path). Do not apply an old HQ overlay when measuring v2:
that would replace some of its weights and produce a mixed checkpoint.

## Observed layouts

These are the variants present in the tested Flash-Next v2 file, not a generic
implementation of every Halogen HT variant. All integer fields are little
endian. The upper 32 bits of HGN `Record.extra` contain the quantization
parameter; the lower 32 bits are not part of that parameter.

### Storage 24: grouped affine Q6

A row of K values uses `align16(13*K/16)` bytes: K/2 low-nibble bytes, K/4
high-two-bit bytes, then K/64 `(FP16 scale, FP16 minimum)` pairs. Within each
byte the earlier value occupies the lower bits. Decode `q*scale+minimum`.
The row padding matters, e.g. K=320 uses 272 bytes, not 260.

### Storage 23: rotated grouped Q4

The payload is a flat low-nibble-first code plane (N*K/2 bytes), followed
by one **signed FP16 scale per 128 values**. Decode `(nibble-8)*scale` in the
rotated basis. Apply normalized 128-wide Walsh-Hadamard transforms along both
matrix axes and multiply by input/output rotation vectors to recover weights.
The vectors are FP16 and shared across experts. Gate/up uses `.experts.su`
and `.gate_up_proj.svh`; down uses `.down_proj.suh` and `.down_proj.svh`.

### Storage 16: 4-bit trellis, parameter 0x1208

The payload is N*K/2 bytes, ordered as `[N/128, K/16, 8, 32 uint32 words]`.
Each word supplies eight MSB-first 4-bit transitions. A 16-bit state ends at
the current nibble, wrapping within each 32-word tile. Lane L maps to row
`L%16`, with eight columns starting at `(L/16)*8`.

The procedural codebook multiplies the state by `0x83dcd12d` modulo 2^32,
sums its four bytes and evaluates
`half_round(fma(1024+sum, half(0x1eee), half(0xc931)))`.
Two normalized Hadamard transforms and `.suh` / `.svh` recover the original
basis. Output rotation vectors also carry scales; they must not be treated
as signs alone.

## Reproducible tests

```sh
bash build_win.sh v2-test
bash build_win.sh
# Optional, read-only checks using files supplied by the model owner:
build/hgn-v2-test.exe /path/v2.hgn /path/ngram.hgn
build/hgn-v2-test.exe /path/v2.hgn /path/ngram.hgn --compare-v1 /path/w4b.hgn
```

Synthetic tests cover row padding, both Q6 code planes, signed scales,
Hadamard inversion, trellis tile boundaries, GPU/CPU dense decoding,
per-token/per-expert grouped GEMVs, complete routed MoE batches, weighted
reduction, and strided FP32/BF16 inputs to native HT projections. The exact
batch test requires numerically identical FP32 outputs to its reference.
CPU inspection
validates metadata for every v2 tensor and samples 128 rows of each rotated
matrix (four rows for Q6). Optional cosine comparison to v1 is a structural
cross-check against a **different quantization**, not a full-precision oracle.

Windows gfx1151 validation results are recorded in `HGN_V2_VALIDATION.md`.
The subsequent kernel work and its numerical limits are recorded in
`HGN_V2_OPTIMIZATION.md`; further order-preserving improvements are in
`HGN_V2_EXACT_OPTIMIZATION.md`.
Group-scale matrix kernels and their separate numerical/behavioral tests are
recorded in `HGN_V2_GROUPED_OPTIMIZATION.md`.
Linux execution, long context, concurrent requests and quality/performance
parity require separate evidence.

## Kernel modes

Set process environment variables before starting the engine or launcher:

| Variable | Values | Default |
|---|---|---|
| `GDEC_V2_MOE` | `exact`, `reference`, `wmma`, `compensated`, `grouped`, `grouped-f16` | `exact` |
| `GDEC_V2_NATIVE_HT` | `1` enables compressed HT projections | off |
| `GDEC_KLD_SERIAL` | present: teacher-forced serial logits for KLD testing | off |

`reference` retains the original slow expert loop and disables native HT.
`exact` accelerates batches above 64 tokens and uses an order-preserving
packed GEMV for smaller batches. `wmma` uses FP16 matrix operands with FP32 accumulation; `compensated`
adds scaled residual operands to reduce local rounding error, at extra cost.
`grouped` multiplies exact Q4 integer codes by high/low FP16 activation parts,
then applies each original 128-column scale in FP32. It avoids the additional
weight rounding in the older WMMA path. `grouped-f16` uses only the high part
to explore a faster, less accurate arithmetic boundary. These modes do not
requantize or rewrite the model files. They change floating-point arithmetic
and are not guaranteed to preserve model outputs or greedy MTP token parity.
All matrix-core modes use the optimized FP32 small-batch expert GEMV.
`GDEC_V2_REFERENCE=1` remains a diagnostic alias for `reference`.

For speed experiments on Windows PowerShell:

```powershell
$env:GDEC_V2_MOE = 'grouped'
$env:GDEC_V2_NATIVE_HT = '1'
.\start_win.exe
```

To return to the conservative default, remove both environment variables
before restarting. Environment changes do not alter a running engine.
These are engine environment variables, not new `service.conf` keys.

## Format research provenance

The implementation was written for this repository. Container/older-format
references: [jtsylve/hgn-spec](https://github.com/jtsylve/hgn-spec), commit
`cc77a36142abe2ba0392ea50d906281664e437eb` (does not document these three new
storage types). The procedural codebook's mathematical reference is
[ExLlamaV3](https://github.com/turboderp-org/exllamav3), commit
`d3739fd393337b1ff4d6c2a342b12f0c87a9592f`, under MIT.
The associated notice is retained in `third_party/exllamav3-LICENSE`.
V2 byte layouts were checked using supplied model records, numerical
comparisons and inspection of the publicly distributed Halogen 0.15.1
decoder. No Halogen executable, disassembly, model bytes or upstream kernel
implementation is distributed in this branch.
