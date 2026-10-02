# R22: paired task evaluation of hybrid and exact arithmetic

## Protocol frozen before execution

This experiment compares the R19 hybrid arithmetic profile with the R21 exact
profile **inside the same R21 engine binary**, using the same API executable.
It is not a comparison of every source change between two releases. No weight,
KV precision, inference code, task budget or grading rule changes during the run.

- Windows 11, Ryzen AI Max+ 395 / Radeon 8060S, 128 GiB UMA.
- Flash-Next v2 HGN with the corresponding PLE n-gram file.
- Single slot, BF16 KV, context capacity 131072, Prefill chunk 8192.
- MTP gamma 2, aligned verification, greedy sampling, seed 20261001.
- Thinking enabled; 4096 output tokens per turn; 24 turns per task.
- Strict tool validation plus the same bounded recovery client on both sides:
  at most two distinct protocol-error repairs. No manual hints or result-based
  reruns. A failed task is retained in the denominator.
- Three fresh variants of each of two task families, paired across both profiles:
  12 task attempts, six pairs. Half the pairs start with each profile.
- Each task starts a new engine and API. Its first prompt must have zero cached
  tokens; within-task prefix cache reuse is allowed and measured separately.
- Model loading and independent scoring are excluded from task wall time.
  The cold first API request, including any lazy initialization, is included.

### Task families and independent grading

1. Synthetic receivables: 652 invoice rows and 350 payment rows. Read both tables,
   choose highest revisions, reject conflicting identities, apply mutually
   exclusive exclusion priorities, preserve negative balances, aggregate by
   department and year, and produce overdue Top 5. Each variant changes amounts,
   identifiers, departments, years/dates, ordering and abnormal records. The
   Python integer oracle is cross-checked by an independently written SQL path
   before model requests. Submission does not reveal its grading result.
2. Repair `parse_amount_cents`: three different starting defects under the same
   explicit contract. Actual bounded-AST execution, baseline test before edit,
   final public tests, and 446-case private grading suite (including public
   cases). There is no shell, arbitrary filesystem, network or access to the
   hidden test/oracle implementation through model tools.

These are different instances of **two** task families, not six independent
domains. Different amounts alone do not establish broad benchmark diversity.
No Octop memory, previous task response, original business workbook, or previous
generated report is supplied. HTML/PPTX appearance is outside this experiment.

### Profiles

Hybrid uses grouped/native HT, upstream dense HT, upstream-prefill expert backend
and ordered HT reduction. The four R20/R21 exact optimization switches are off.

Exact uses the original exact arithmetic mode with native HT disabled and enables
`GDEC_V2_EXACT_REUSE`, `GDEC_V2_EXACT_ORDER`, `GDEC_V2_EXACT_DATAFLOW` and
`GDEC_V2_BF16_GEMV`. Both preserve the same aligned MTP infrastructure and API.
The exact profile is a numerical reference implementation of quantized weights,
not ground truth for model answer quality.

Engine source was pinned at `2e68d6845d38bc5e22d8dbdc1877509c310ee427`.
The engine SHA-256 was
`0db12c2073baf550267519ad0ceec6d275f6a7653c9ebffb76681508ccc23728`, and the API
SHA-256 was `a4b38906fe189454670929271a37af5f5d0dfb884148af89b77433e5e60f7da0`.
Both campaigns check weight file size, modification time and the SHA-256 of
sampled first/last MiB before and after running. This is a change-detection
check, not a full-file checksum of the large model files. No process in this
experiment writes the weights. Raw process identities and request logs remain
local; the benchmark summaries contain configuration and artifact fingerprints.

### Reusable tools

- `tools/agent_reliability/test_r22_variants.py`: no-model SQL/oracle checks,
  correct-parser verification and rejection of deliberately wrong submissions.
- `tools/agent_reliability/r22_client.py`: runs one new scenario/variant against
  the isolated loopback API on port 18871. The caller owns engine lifecycle.
- `tools/agent_reliability/r22_report.py`: aggregates recorded task evidence.

Raw request/SSE/reqstat logs remain local. Published results must distinguish
first-pass/protocol/tool failures, expected baseline code failures, failures
after edits, task completion, independent correctness, and weighted fresh-input
Prefill/Decode. Different model plans and output lengths mean task-wall changes
are not pure kernel-speed measurements. A finite all-pass sample cannot establish
equal population accuracy.

## Primary results (4096 output tokens per turn)

All 12 registered attempts completed. Hybrid passed 4/6 and exact passed 3/6.
These small counts do not estimate population accuracy. Four failures exhausted
the per-turn output budget; one naturally completed but submitted wrong statistics.
There were no engine crashes, connection failures or protocol-format rejections.
Three ordinary tool execution errors occurred on the exact profile and were
recovered within the task (two SQL errors, one bounded-executor name restriction).

| Family / profile | Independently passed | Weighted fresh Prefill, t/s | Weighted Decode, t/s |
|---|---:|---:|---:|
| Office / hybrid | 2/3 | 1051.52 | 45.79 |
| Office / exact | 1/3 | 652.21 | 25.47 |
| Code / hybrid | 2/3 | 574.81 | 45.65 |
| Code / exact | 2/3 | 417.77 | 24.33 |

Phase rates include all recorded requests, including failed task attempts.
They are ratios of total new/output tokens to total phase time, not averages of
per-request rates and not full-prompt counts that include cached tokens.

| Paired instance | Hybrid result / attempt seconds | Exact result / attempt seconds |
|---|---|---|
| A office | Pass / 367.75 | Output limit / 247.09 |
| A code | Output limit / 105.66 | Pass / 367.69 |
| B office | Output limit / 128.28 | Wrong statistics / 604.17 |
| B code | Pass / 179.27 | Output limit / 194.75 |
| C office | Pass / 258.02 | Pass / 413.30 |
| C code | Pass / 104.61 | Pass / 234.47 |

Failed attempt time is **not** time to a correct answer. Only the two C pairs
support a completed-task wall comparison: hybrid reduced wall time by 37.57%
for office and 55.38% for code. Office hybrid generated 10335 tokens versus
8097 for exact; code hybrid generated 4742 versus 5552. These are task outcomes,
not identical-output kernel benchmarks. Every paired initial request and fixture
fingerprint matched, every first request had zero cached tokens, and no profile
environment changed during the campaign.

### Reproduced statistical error

Exact office B retained `row_id` inside `SELECT DISTINCT` for highest-revision
invoice rows. Unique row IDs prevented otherwise identical content from being
deduplicated; those rows were then treated as conflicts. It submitted 477 eligible
invoices versus the correct 483, understated receivables by 251875.02 yuan,
applied payments by 139584.23 yuan and balance by 112290.79 yuan. Conflict counts
used rows instead of identities, and negative amounts were omitted from the
specified `missing_amount` exclusion count.

A read-only diagnostic reran the saved SQL and reproduced the submitted totals.
Removing only `row_id` from its DISTINCT columns recovered all four independent
reference totals. This diagnosed the mistake; it did not alter the original
submission, rescore the task as successful, or send the model a correction.
Thus numerical agreement with the exact reference is not a guarantee of correct
business logic. This experiment does not establish unquantized model fidelity.

### Startup variation

Exact office B took 301.90 seconds to become ready during an observed interval
of roughly 60 MiB/s C-drive reads and a high disk queue. At the sampled instant,
Windows still had about 46 GiB commit headroom and zero page-in/page-out counters.
Other observed starts returned to roughly 21–30 seconds. The storage temperature
was unavailable, so no thermal or other unverified root cause is asserted.
Startup is recorded separately and excluded from the task wall figures above.

Fresh processes also do not flush the operating system file cache, driver state,
or control clocks/temperature. A later A-office hybrid control reproduced every
visible request/response message after ignoring generated tool-call IDs, and
the same 12130 output tokens, yet its task time changed from 367.75 to 306.04 s.
Prefill phase totals changed from 82.96 to 47.23 s and Decode from 283.55 to
257.69 s. The output budget was raised but was never reached on either run;
this timing difference cannot be credited as a budget or kernel optimization.
The cause was not isolated. Keep both runs rather than selecting the fastest,
and require controlled repeated timing before claiming stable small speed gains.

### Separate budget diagnostic

After crossed `finish_reason=length` failures in instance A (exact office and
hybrid code), a **separate** four-task diagnostic was registered. Both profiles
run both instance A tasks from fresh state with 8192 output tokens per turn.
It changes no other task setting and does not replace primary failures or enter
the primary success denominator. `r22_budget_probe_client.py` records this
secondary status in each result. This is an outcome-selected diagnostic of a
specific failure mechanism, not an unbiased second accuracy sample.


All four secondary tasks completed and independently passed. The two primary
length failures (exact office A and hybrid code A) continued the same recorded
reasoning prefixes after raising the cap and passed. The two already-successful
controls retained the complete visible transcripts and output token counts after
ignoring generated tool-call IDs. This diagnoses these two truncations; it does
not repair the wrong-statistics case, test instance B at 8192, or revise the
primary denominator.

The hybrid code truncation was not unfinished reasoning: both runs had identical
reasoning content in that turn. At 4096 tokens the tool arguments were partial;
the larger-budget run completed that call at 4345 tokens and continued to a
correct result. The request output budget covers reasoning and tool arguments,
so preserving enough space for a complete actionable output matters. The client
did not execute the truncated call.

| Instance / profile | 4096 result | 8192 result | 8192 task seconds | 8192 output tokens |
|---|---|---|---:|---:|
| A office / hybrid | Pass | Pass | 306.04 | 12130 |
| A office / exact | Output limit | Pass | 503.27 | 11000 |
| A code / hybrid | Output limit | Pass | 141.91 | 6484 |
| A code / exact | Pass | Pass | 375.26 | 9210 |


## Decision and boundaries

Retain hybrid as the practical speed route, with its documented arithmetic
differences, and R21 exact as a controlled numerical reference for future kernel
changes. Neither is an oracle for task answers. Keep exact-output regression,
independent task grading and whole-request timing as separate checks.

The concrete reliability priorities exposed here are output-budget handling and
independent reconciliation of business results. This round adds evaluation tools
and evidence; it does not implement an engine-side thinking/final-answer reserve,
an Octop retry policy, or a new kernel. The successful 8192 probe is a bounded
configuration diagnostic, not a universal recommended budget or an automatic
deployment change. Statistical failures must not be hidden by retrying until a
pass or by switching to a slower arithmetic profile without independent grading.

[Machine-readable primary and secondary outcomes](benchmarks/r22-route-quality.json)
include hashes, fixed profiles, all task results, metrics and SQL-error evidence.
Raw SSE/tool/result files are preserved locally. Numerical-fidelity conclusions
remain those in R19–R21: this task study neither establishes hybrid/exact
equivalence nor an unquantized-model quality ranking.
