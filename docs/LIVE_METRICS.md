# Live inference metrics

The dashboard's **实时运行** section polls `GET /metrics/live` once per second
while visible. The existing Prefill/Decode cards and chart remain completed
request statistics for the selected date range, now labelled **历史平均**.

The live endpoint reads a small mutex-protected API memory snapshot. It does
not acquire a generation slot, query the GPU, scan reqstat files or persist
partial requests. Counters start at API process launch. The `session` identity
and monotonic `sample_ms` reset browser sampling after a restart. A failed poll
clears current rates and resets the sampling baseline. Pause/background and
long sampling gaps also reset that baseline.

## Measurement contract

- `prefill_completed_tokens_total`: uncached prompt rows actually completed at
  existing chunk completion synchronization points. Cached rows are excluded.
- `prefill_estimated_tokens_total`: completed rows plus the current chunk's
  fraction of layers observed complete by existing nonblocking HIP event probes.
  This is an **estimate**, not additional fully processed tokens. The dashboard
  labels its delta/elapsed rate and progress bar accordingly. At Prefill end,
  both counters reconcile to the actual uncached prompt count.
- `output_tokens_total`: committed engine `T` tokens, including reasoning and
  tool text, excluding rejected speculative drafts. The dashboard computes
  delta/elapsed across consecutive polls, covering all requests. This is recent
  wall-clock throughput, not the historical ratio of tokens to request time.
- `active`: request IDs, phases, counts, elapsed time and update age. Cleanup is
  scoped to generation lifetime, including exceptions, cancellation and failed
  engine connections. Queued requests remain in the existing health display.
- `last_prefill`: latest completed Prefill with its final engine duration and
  rate; published before Decode completes and retained while idle. Per-request
  `prefill_done` and rate similarly become available at that phase transition.

Large batches can produce uneven one-second rates. Zero recent throughput can
mean a batch is still computing, not that a request has failed. Lack of progress
is labelled by update age. UI polling does not change MTP or arithmetic settings.

## Engine protocol

`GEN ... PROGRESS` opts into:

```
P <req> <prefill_done:0|1> <completed> <estimated> <uncached_total> <cached> <prefill_ms>
```

The initial message marks prompt ingest start. Existing per-layer reports and
chunk completion report progress; the final message marks the completed Prefill
phase before token generation. No new GPU synchronization is added. The sink is
thread-local to the GEN worker, so interleaved sequence slots cannot send each
other's progress. Generation teardown releases it, including exception paths.

The final `D` and committed `T` protocol meanings remain unchanged. Existing
clients that omit `PROGRESS` receive no `P` messages. Older engines ignore the
bare suffix under their existing lenient parser; the new API still reports
committed output counts but leaves live Prefill unavailable. Malformed,
wrong-request or regressing progress is ignored, and progress messages never
extend first-token/idle-token deadlines.

## Focused validation

- `python tools/api_live_metrics_test.py --api <candidate>`: two simultaneous
  generations; phase result before response completion; exact counters/cache
  exclusion; invalid progress; legacy, cancellation and failure cleanup.
- `tools/engine_progress_test.cpp` plus `tools/engine_progress_test.py`: progress
  messages cannot hide a first-token or Decode stall.
- Existing disconnect, cancellation-timeout and capacity negotiation tests.
- `node tools/dashboard_ui_test.cjs`: live states, restart/error recovery,
  historical statistics separation, desktop and mobile layout, existing flows.
