# Windows hybrid mainline deployment

Selected on 2026-10-02: HGN v2 hybrid arithmetic, aligned MTP, gamma 2.
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

The installed inference engine SHA-256 remains
`0db12c2073baf550267519ad0ceec6d275f6a7653c9ebffb76681508ccc23728`;
the API remains
`a4b38906fe189454670929271a37af5f5d0dfb884148af89b77433e5e60f7da0`.
Their inference source is `2e68d6845d38bc5e22d8dbdc1877509c310ee427`.
Only the native launcher was rebuilt for persistent configuration support.

Validation: five native runtime-configuration checks, 32 existing launcher/host
checks, real API smoke (Chat, Completions, Responses, streaming, stop and sampled
logprobs), a streamed tool/result round trip, red-image recognition and rejection
of 17 images. No 256K performance or all-input numerical-equivalence claim is
made by these deployment checks.

Output budgets remain client-controlled. A fixed 8192-token output allowance was
preferable in the bounded R23 tasks; the server does not silently cap or override
Octop requests. R23's schema-specific independent delivery checker is **not**
installed into Octop. Hybrid remains numerically different from the exact
reference, and a successful API/tool smoke does not prove business-data accuracy.
