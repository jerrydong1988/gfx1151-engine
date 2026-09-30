# HGN v2 development validation — 2026-09-30

Platform: Windows 11, Radeon 8060S / gfx1151, TheRock 10.0.0 toolchain.
Fork base: upstream `52e0aad9c4c612c57db38541b3d1a3ada61e7717`.

## Passed

| Check | Observed result |
|---|---|
| CPU synthetic format checks | Passed, including Q6 row padding and unknown HT variant rejection |
| All 65,536 procedural codebook states | CPU and GPU exactly equal |
| HT dense decode, 256 x 384 | Relative L2 0.00167821 vs FP32 CPU result; includes BF16 output rounding |
| Q6 dense decode, 7 x 320 | Exact on the synthetic representable-value fixture |
| Rotated expert GEMV, per-token input | Relative L2 1.59342e-7 vs independently materialized CPU matrix multiply |
| Rotated expert GEMV, per-slot input | Relative L2 1.54112e-7 |
| Weighted expert reduction | Passed |
| Existing kernel test suite | 187 PASS lines, final `ALL PASS` |
| Windows engine and launcher builds | Passed |
| Actual v2 metadata and sample decode | All 699 new-format matrices checked (313 HT, 96 expert, 290 Q6), finite samples |
| Actual v2 vs v1 Q4 sample comparison | 699 matrices, mean cosine 0.993526, minimum 0.987619 |
| Missing separate PLE table | Rejected explicitly before model allocation |

The comparison samples the first 128 rows of each rotated matrix and four
rows of each Q6 matrix. Its minimum occurred at
`layers.0.mlp.experts.gate_up_proj.weight`. It is a useful layout check, but
v1 and v2 use different quantizers: these cosines are **not** accuracy scores
against full-precision weights. File sizes checked were 66,687,678,432 bytes
for the main v2 file and 51,200,246,144 bytes for its PLE sidecar. This does
not substitute for a complete file hash/checksum verification.

## Not yet established

* Full v2 model load, generated-text coherence and end-to-end logits checks.
* Actual prompt processing / generation throughput and memory high-water mark.
* Long context, MTP verification, vision and concurrent requests with v2.
* Application/Agent tool-call quality and statistical-analysis quality.
* Linux runtime behavior.

The existing working installation was left running and unchanged. The
development build is isolated. A full-load test requires releasing the old
instance's GPU allocation first; no second full model was started alongside it.
