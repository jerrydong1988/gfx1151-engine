# R24: research workload latency and report workflow

Date: 2026-10-03. Windows 11, Ryzen AI Max+ 395 / Radeon 8060S,
128 GiB UMA, HGN v2 hybrid, BF16 KV, one slot, context capacity 262144.
This is a bounded local study, not another upstream comparison or a full Octop
task rerun. The weights and numerical kernels were not changed.

## Adopted outcome

- Add host request/chunk/setup/PLE I/O diagnostics, without extra GPU events or
  synchronization. See [timing semantics](REQUEST_TIMING.md).
- Keep MTP gamma **2**. Gamma 3 won the initial 512-output sweep, but regressed
  on a longer output test. There is no demonstrated universal Decode gain.
- Keep experimental Windows PLE page I/O **off** by default. Byte equality passed;
  hot Prefill improvement and reliable removal of the long tail did not.
- Provide a client-side citation renderer. On a frozen research corpus, concise
  writing plus source IDs cut one completed report-generation request from
  **373.413 to 197.867 seconds (47.0%)**, with the same baseline binary/gamma 2.
  Generated tokens fell **10681 to 6175 (42.2%)**. This is a workflow observation,
  not a 47% engine-throughput improvement or full-task latency guarantee.

## Prefill: recovered normal timing is not a proven fix

The original slow continuation reused 59639 tokens and added 15065, taking
130.942 seconds. A teacher-forced historical-token replay at the exact cache
boundary took 11.472 seconds. The original RNG and every intermediate session
state were not recreated.

Other runs still stalled: a cold 126954-token serial Prefill took 437.497 seconds
and later 92.953; a 152384-token MTP Prefill took 203.311 seconds. Disk read latency
and system page reads rose during the latter, while commit headroom remained
about 47 GiB. System-wide counters cannot identify PLE cold reads, WDDM residency,
other processes or SSD conditions individually. Root cause remains unresolved.
First-shape projection queries observed in diagnostic replays were at most
0.932 seconds, not enough to explain those long pauses in those replays.

The opt-in page reader sorts/deduplicates 4 KiB pages then scatters the exact row
bytes back to the original order. Verification covered 26 real gathers / 382484480
bytes, 11 replay output checkpoints, and three 512-token sampled output streams
with identical tokens and logprobs. Independent tests also compare direct byte
slices for 90/160/4096-byte rows, duplicate offsets, partial EOF and arbitrary
completion order; a real Windows IOCP fixture checks slot reuse.

Page-reader normal Prefill times at 74704/126954/152384 input tokens were
52.900/91.952/111.622 seconds, close to normal row-reader timings. A recovered
203-to-112-second run is not credited as a causal fix. Small Decode gathers still
use row reads. Linux, multi-slot and other-format performance were not established.

## Decode: draft depth is workload dependent

Initial fixed-512-output sweep, temperature .7, top-k 20, top-p .8, seed 1729:

| Input tokens | Serial tok/s | Gamma 2 | Gamma 3 | Gamma 4 |
|---:|---:|---:|---:|---:|
| 20366 | 30.59 | 40.29 | 40.41 | 37.98 |
| 74704 | 26.44 | 40.90 | 48.50 | 46.87 |
| 126954 | 26.83 | 38.79 | 42.77 | 43.69 |
| 152384 | Not measured | 36.71 | 39.50 | 26.06 |

The last gamma-4 case also had I/O-related stalls; it is retained, not silently
removed. The initial gamma-3 aggregate Decode gain was about 8.7%.

Counterexample: same 126954-token input, seed 991, 2048 output tokens, reversed
gamma order, same candidate binary/page-I/O setting: gamma 3 produced 37.83 tok/s
versus gamma 2 at 40.52 tok/s. Request times were 145.135 versus 141.341 seconds.
At temperature 1/top-p .95, seed 3163, 1024 outputs, gamma 3 was 43.12 versus
41.77 tok/s, but saved only about 1.08 seconds of the entire cold request.

Changing gamma changes sampled trajectories even with the same seed. These are
exploratory workload observations, not universal rankings or proof of new
precision loss. Fixed-budget runs intentionally finish `length`.

## Frozen-corpus report experiment

132 unique retrieved URLs and their original snippets were frozen. No historical
memory, fresh search, or audit corrections were added. All complete reports used
temperature .7, top-k 20, top-p .8, seed 1729 and a 32768-token hard allowance.
They ended naturally. The soft policy asks for a concise, evidence-scoped report.

The first compact policy made the model repeat long percent-encoded URLs. It
reduced prose, yet increased total output from 10681 to 10935 tokens, and one
copied URL was corrupted. Source-ID-only output plus deterministic link rendering
resolved all 168 citations in the gamma-2 candidate without rewriting its prose.
Unknown IDs and links differing from the manifest are rejected.

Final revalidation caught a hole in the original renderer: a gamma-3 report's
`[S58]` escaped a matcher requiring three digits. The matcher now recognizes all
digit lengths and rejects IDs absent from the manifest, with a regression test.
That candidate fails the strict citation gate; its original report and earlier
check are preserved alongside a corrected validation record. The selected
gamma-2 report still passes all 168 mappings with identical rendered Markdown.

Human review scores on the previous 30/20/20/15/10/5 rubric were baseline 74,
initial compact 76, and ID-only gamma-2 report 78. These are bounded editorial
judgments, not accuracy percentages or general model rankings. The selected
report still misstated one guide's publication month and repeated an unsupported
conversion of a standards-development period into an enterprise compliance
deadline. Citations do not establish factual correctness. It remains a draft
requiring revision for formal compliance use.

The complete local record includes other candidates and negative findings.
Sanitized timing rows are in [r24-research-performance.json](benchmarks/r24-research-performance.json).
Private prompts, output tokens and source documents are not committed.

## Using the workflow helper

1. Freeze search results into a JSON source manifest with stable IDs and exact
   URLs. Extract this programmatically from tool results, not by asking the
   model to retype URLs.
2. Ask for adjacent `[S001]` citations, scope-qualified claims, and an appropriate
   soft prose budget. Keep enough hard output allowance to finish naturally.
3. Preserve the original model Markdown, then run:

   ```text
   python -B tools/research_citations.py raw-report.md sources.json report.md
   ```

4. Independently verify important claims against primary sources. A valid source
   ID only proves a mapping, not that the source supports the statement.

This helper is not automatically installed into Octop's agent orchestration.
It does not change global reasoning limits, input capacity or output budgets.

## Validation limits

Windows full GPU build, byte tests, parser/citation unit tests and a real
three-request API tool/result/final-answer loop passed. The latter independently
checks a small deduplicated synthetic ledger; it is not a general task-accuracy
test. Existing hybrid-versus-exact numerical limits remain. No new 256K-full-load,
concurrent or exhaustive multimodal claim is made.
