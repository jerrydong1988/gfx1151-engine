# Windows hybrid mainline deployment

Selected on 2026-10-02, diagnostics updated on 2026-10-03: HGN v2 hybrid arithmetic,
aligned MTP, gamma 2.
The native launcher reads explicit `GDEC_*` configuration entries from
`service.conf`, in addition to the existing high-level options. It applies them
after built-in flags; the existing context, KV, gamma and arithmetic configuration
keys retain their final authority. Existing parent-environment precedence is
preserved. `--check` prints the effective child `GDEC_*` environment without
starting children. Normal startup records the same settings in its launcher log.

For a Windows installation, copy `tools/windows_hybrid.conf.example` to the
installation's `service.conf` and set `MODEL_DIR` to the existing weight directory.
Use the matching `start_win.exe`, `build/gdec-win.exe`, `build/gdec-api-win.exe`
and validated ROCm runtime files. Start only `start_win.exe`; stop the service
with its tray menu. The source repository retains alternative computation paths
for research and numerical regression, but they need no separately installed
services or shortcuts.

The selected installation uses:

- `qwen38-flash-next-v2.hgn` plus `qwen38-flash-next-ngram.hgn`.
- No legacy w4b overlay or external MTP file; v2 provides its own draft head.
- Existing `qwen38-flash-next-vision.hgn` and tokenizer, unchanged.
- Context capacity 262144, one slot, BF16 paged KV, Prefill chunk 8192.
- MTP gamma 2, aligned verification and serial accumulator order.
- Upstream dense backend with ordered HT reduction; upstream expert Prefill
  for large batches, retaining the fork's short verification path.
- RAM/SSD checkpoints disabled; one-request image limit 16.
- Loopback engine/API ports 18860/18861; model ID `qwen3.8-flash-next`.
- Request/chunk host timing enabled; experimental PLE page I/O and double-read
  verification disabled. Numerical settings and weights are unchanged by R24.

The R24 inference engine SHA-256 is
`fe91c4f38050e4b274c20350c2eeb0ff80049ab877796f3d4b25e2f107d9b577`;
the API remains
`a4b38906fe189454670929271a37af5f5d0dfb884148af89b77433e5e60f7da0`.
The API source remains `2e68d6845d38bc5e22d8dbdc1877509c310ee427`.
The engine includes the R24 diagnostics and default-off experimental reader,
built on the `93b801a` source baseline. The installed manifest records the exact
local source revision. The native launcher is unchanged from the prior deployment.

Prior deployment validation: five native runtime-configuration checks, 32 launcher/host
checks, real API smoke (Chat, Completions, Responses, streaming, stop and sampled
logprobs), a streamed tool/result round trip, red-image recognition and rejection
of 17 images. No 256K performance or all-input numerical-equivalence claim is
made by these deployment checks. R24 separately checks the effective configuration,
engine identity, an actual tool/result/final-answer exchange, and red-image
recognition; it does not claim the entire earlier deployment suite was repeated.

See [R24 conditions and limits](RESEARCH_WORKLOAD_R24.md). The optional source-ID
report helper is client-side and is not automatically integrated into Octop.

Output budgets remain client-controlled. A fixed 8192-token output allowance was
preferable in the bounded R23 tasks; the server does not silently cap or override
Octop requests. R23's schema-specific independent delivery checker is **not**
installed into Octop. Hybrid remains numerically different from the exact
reference, and a successful API/tool smoke does not prove business-data accuracy.
