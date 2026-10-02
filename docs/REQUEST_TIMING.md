# Prompt latency diagnostics

Normal serve requests now log low-overhead **host elapsed intervals**. No new GPU
event, stream synchronization, weight conversion, precision change or kernel
selection is introduced by these records. `GDEC_REQUEST_TIMING=0` disables the
request/chunk wall records. Existing detailed GPU profiling stays opt-in.

- `[request-prefill-wall]`: request ID, prompt/cache lengths, setup, prompt
  ingest and first-token readback/sampling time. Setup includes the existing
  continuation boundary work; it is not an HTTP queue timer.
- `[prefill-wall]`: chunk base/length, preparation, main prefill, MTP submission,
  scheduler yield and total. Async GPU work can cross these boundaries. A long
  `main_ms` is a location for further investigation, not proof of a slow kernel.
- `[iproj-pinned] query_ms`: host duration of first-shape solution enumeration.
- `[mtp-branch-strided] support_ms`: host duration of descriptor/support setup.

The chunk records are **inside** request ingest: never add both to obtain task
time. Model loading, HTTP/tokenization, client tools and other requests are
separate. Sampled output differences across gamma or serial/MTP modes are not
an equality oracle, even with the same random seed.

Summarize a preserved log, without modifying it:

```text
python -B tools/request_timing_report.py engine.log --out timings.json
python -B -m unittest discover -s tools -p test_request_timing_report.py
```

For a targeted reproduction, use `GDEC_PREFILL_PROFILE=1` and optionally
`GDEC_MTP_INGEST_PROFILE=1`. These diagnostic runs are not directly comparable
to uninstrumented throughput measurements; the MTP event diagnostic can add
synchronization. Detailed profiler child intervals overlap their parents.

Preserve the exact binary, runtime, weights, effective environment, prompt token
hash, cache boundary and sampling policy. Record process I/O/page faults plus
system disk latency and memory alongside a slow request. Page faults include
soft faults; Windows commit headroom is not GPU residency headroom. A run that
recovers after restart does not by itself establish a fix, and one slow request
must not be removed from reliability results merely because it lowers throughput.

## Experimental Windows PLE page reads

`GDEC_PLE_PAGE_IO=1` opts into sorted, deduplicated 4 KiB page reads for gathers
of at least 512 rows. Each page is scattered back to the original row order.
This changes I/O scheduling and host copying, not the PLE hash, FP8 decoding,
weights, KV precision or GPU arithmetic. Small Decode gathers retain row reads.
It is default off; normal hot Prefill performance and long-tail robustness must
be measured separately before choosing it for a workload.

`[ple-page-io]` records row/page counts, CPU planning time and overlapped-read plus
scatter time. `[ple-row-io]` records legacy large-gather time. With lookahead these
intervals overlap model execution: **do not add them to chunk or request time**.
This is buffered Windows I/O; byte counts do not measure physical SSD traffic.

For correctness diagnostics only, set `GDEC_PLE_PAGE_VERIFY=1` together with
page I/O. Every eligible gather is also read through the row path and compared
byte for byte before use; a mismatch aborts. This doubles reads and is excluded
from throughput comparisons. Never leave verification enabled in normal use.

The independent planner test compares against direct file slices, including
90/160/4096-byte rows, arbitrary completion order, duplicates and partial EOF:

```text
cl /EHsc /std:c++17 tools/test_ple_page_plan.cpp /Fe:build/test-ple-page-plan.exe
build/test-ple-page-plan.exe
```

The existing reader assumes a single pump owns each completion port at a time.
The page mode does not add concurrent-reader support or establish multislotted,
multimodal or other-weight-format performance.

## Windows slow-read localization (R25)

With request timing enabled, `[ple-row-wait]` separates cumulative time inside
`ReadFile` from time inside `GetQueuedCompletionStatus`, and reports each call's
maximum. The clock is deliberately coarse (`GetTickCount64`); use it for second-
scale stalls, not kernel microbenchmarks. These include descheduling and are not
physical-disk-only timings. Correlate with disk latency/queue, page reads and
available memory. Windows page reads can load file-backed pages, not just the
pagefile; they do not by themselves prove commit exhaustion.

An aligned unbuffered page-read prototype passed byte checks but regressed the
16,384-token probe from 12.84 s to 39.19 s. It was rejected and is not included in
this runtime. This does not rule out other storage/data-flow approaches.

## Bounded same-prompt retry checkpoint (R25)

`GDEC_RETRY_CHECKPOINT=1` with `GDEC_RCKPT_MAX>0` inserts a snapshot at the last
ordinary Prefill chunk boundary strictly before prompt end. A repeated request
can then recompute at most one chunk, instead of the whole prompt. Existing GDN,
MTP and pinned-KV state restoration is used; changing KV length alone is unsafe.
Use the same fixed `GDEC_PREFILL_CHUNK` as the measured configuration. A full
prompt-end checkpoint has no saved logits and is not itself a resumable match.

Two RAM entries with `GDEC_CKPT_TOKENS=0` retain this boundary and prompt end for
a typical immediate retry. Other conversations, pool pressure and additional
API snapshot cuts can evict them; reuse is never guaranteed. RAM checkpoints do
not survive a process restart. Token equality must be checked against cold runs
with the same numerical profile; this feature does not establish general MTP
equivalence or semantic correctness.

API terminal parser failures now log the response ID and `invalid_tool_call`
after generation. An engine `finish=stop` line alone is not evidence of successful
API delivery. Terminal logs omit tool arguments and document contents.
