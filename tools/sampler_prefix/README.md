# Dynamic prefix penalty regression probe

The probe extracts the actual `sms_round_shared`, `sms_prefix_upload`,
`k_corr_apply`, top-k dispatch and `HostSampler` source at build time. A separate
correction oracle uses the verbatim pre-filter statements of `HostSampler::prepare`.
It does not replace the implementation with a rewritten sampler. Excerpt hashes,
line ranges, build flags and executable hashes are saved beside the executable.

From a configured HIP/C++ compiler environment:

```text
python tools/sampler_prefix/validate.py --source . --outdir PATH_TO_NEW_EVIDENCE_DIR --compiler PATH_TO_HIPCC --cpu-check
PATH_TO_NEW_EVIDENCE_DIR/prefix-correction-probe.exe
```

The Python command builds only; `--cpu-check` runs the actual uploader against
host-memory copy shims and the CPU correction oracle without launching GPU work.
Only the second command runs the actual GPU correction and top-k kernels. Use
`--case NAME` for one case or `--cpu-only` to avoid all HIP runtime operations.
Existing evidence directories are never overwritten.

For a historical control, add `--legacy-corrections-ref COMMIT` to the build.
Only the two changed correction functions are extracted from that Git commit;
the test, sampler, top-k and dispatch remain current. For this control, the CPU
uploader-contract check intentionally fails because it expects merged penalties;
use actual GPU `correction_bit_mismatches` to measure the old arithmetic result.

Cases include non-binary-exact and negative penalties, repeated and unique IDs,
base-history membership, overlapping bias, prefix lengths 1/8/64, offset chunks,
zero/presence-only/frequency-only penalties, row zero, and top-k overflow fallback.
The first case passes null prefix buffers with only bias/base corrections active.
GPU checks require bitwise corrected logits, top-k IDs and final probabilities to
match the CPU reference; original raw logits must remain pristine. The probability
checks also exercise top-p/min-p filters.

The fix stores merged prefix penalties and subtracts them once from post-bias
logits; CPU-only success is not GPU validation or proof for arbitrary floating
point/compiler settings. No model quality or same-seed serial/MTP identity claim
is made by this synthetic test.
