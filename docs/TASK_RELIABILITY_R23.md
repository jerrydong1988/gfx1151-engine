# R23: bounded output recovery and independently checked delivery

## Registered protocol

The R21 engine/API binaries and both arithmetic profiles remain unchanged.
R23 changes the evaluation client and a contract-specific delivery workflow.
Nothing here automatically installs recovery or ledger checking in Octop.

The primary matrix has **16 hybrid tasks**: four instances (old R22 A/B and new
R23 D/E), two task families and two output policies. D/E have not been run through
the model before this experiment; their data/initial defects differ, but they
remain instances of the same two synthetic task families, not new domains.
The policies are fixed 8192 tokens and adaptive 4096 with at most one escalation
to 8192 per task. The larger cap persists after escalation. Order is balanced
across the eight pairs. All tasks use fresh engine/API processes and zero initial
prefix-cache hits; within-task reuse, including regeneration reuse, is measured.

Two exact B-office cases are a separate regression check, not a population quality
comparison. Four additional A-code cases complete three runs per policy for a
limited timing-repeat check; these repeat outcomes do not enter the primary
success denominator. All 22 attempts are retained, including failures.

Conditions: context 131072, chunk 8192, BF16 KV, MTP gamma 2 / aligned, greedy,
seed 20261001, thinking enabled. No change to weights or sampling during retries.
Maximum 24 API attempts, 65536 generated tokens per task and two distinct format
repairs. A 1800-second limit stops admission of further requests; it does not
interrupt a request already running. The owned runner has a 5400-second client
timeout. Model loading and final hidden scoring are separate from task time;
runtime verification, failed requests and retry costs count toward task time.

## Recovery semantics

The client only considers regeneration after complete SSE EOF, `[DONE]`, a
terminal `length` reason, and validated usage/reqstat. It distinguishes context
capacity from an output cap and refuses unexplained early `length` stops.
Transport failures and uncertain completion are not automatic retries.

The truncated assistant response is preserved in evidence but is not added to
valid history or dispatched. The next request has identical valid messages and
a larger cap. Earlier completed tool results stay in that history; the recovery
mechanism does not replay their actions. This is a bounded regeneration, not a
claim of generic exactly-once execution for every agent/tool system. Retry stops
when context, total output, escalation count or attempt limits prevent it.

## Independent delivery check

For the office task, `ledger_verifier.py` recomputes totals, department/year
groups, overdue Top 5 and exclusion identities from the immutable source SQLite
tables using independently written SQL. The hidden final grader continues to use
the original Python grouping/integer implementation. The runtime checker neither
imports that oracle nor evaluates the model's own SQL as its reference.

Rejected submissions are retained as attempts but not saved as accepted delivery.
The model receives only failed field names and whether another attempt is allowed:
no expected amounts, corrected answer, SQL solution or hidden grading examples.
At two rejections, changed source data or checker failure, delivery is blocked.
A later rejected submission cannot inherit acceptance from an earlier good one.
This is an assisted workflow and is not a measurement of raw model accuracy.

The checker is explicitly limited to the declared synthetic schema and business
contract. Adapting it to the user's workbook requires a separately reviewed data
mapping and business rules. Code tasks retain public tests plus separate final
private grading; no hidden code answers are supplied to the agent.
The guard validates the structured submission, not every claim in the final
natural-language reply or a chart's visual mapping.

## Offline checks

Before the first GPU case, the existing 35 client/recovery tests and 10 new
budget/delivery tests passed. The later hand-calculated and full-loop checks
below were added while the frozen campaign ran; they did not change its client,
fixture inputs or engine settings.

- Existing SSE/metrics/loop and format recovery tests pass with default behavior.
- Injected output truncation does not dispatch even a complete first call in an
  otherwise incomplete response; successful earlier actions run once.
- Repeated truncation, context exhaustion, unclear length and total task budget
  all stop without an unbounded retry.
- Independent SQL matches the Python oracle on all five available A–E instances.
- Initial parser defects fail public/private tests and the reference parser passes.
- Self-balancing wrong totals, wrong exclusion counts, float values, changed
  ordering/duplicates, source mutation and unavailable verification cannot pass.
- A hand-calculated miniature ledger separately covers cutoff equality, refund,
  overpayment, tied ranks and superseded conflicts; an empty ledger returns zero
  totals and no groups. The full client loop stops after two rejected deliveries.

Engine speed claims require
recorded phase metrics and repeat variation; raising a cap is not a kernel
optimization, and a stopped wrong delivery is not a successfully completed task.

## Campaign results

Completed **2026-10-02 22:56:24 UTC+8**, following the registered order.
The primary hybrid matrix passes **8/8 with fixed 8192**
and **7/8 with adaptive 4096→8192**. These selected
instances do not estimate a general success rate or establish a model-quality
ranking. Seven primary pairs have identical executed tool actions and output
token counts; their adaptive-vs-fixed wall changes range from -0.91% to +3.22%.

| Instance / task | Fixed 8192 success / s | Adaptive success / s |
|---|---|---|
| r22-a / office | True / 274.40 | True / 283.23 |
| r22-a / code | True / 140.31 | False / 236.14 |
| r22-b / office | True / 298.99 | True / 302.19 |
| r22-b / code | True / 184.88 | True / 185.01 |
| r23-d / office | True / 248.27 | True / 250.34 |
| r23-d / code | True / 126.73 | True / 125.57 |
| r23-e / office | True / 225.24 | True / 224.07 |
| r23-e / code | True / 83.32 | True / 82.80 |

### A-code failure and recovery cost

The adaptive A-code attempt discards a 4096-token response without executing its
calls and regenerates from identical valid history. It then passes all 21 public
tests but only **364/446** private-suite entries: amounts without a decimal point
are not multiplied by 100. `123457` incorrectly returns 123457 instead of
12345700. This is one boundary defect with many related failing inputs, not 82
independent task failures. Fixed 8192 produces a different program that passes
446/446. The private suite includes public cases.

The recorded regeneration does **not** preserve the truncated reasoning prefix.
The truncated request uses 1872 cached prompt tokens; regeneration uses zero.
The valid request except its cap is identical, but regeneration is not a
continuation of the discarded generation. Cache/batch state differs; this
campaign does not isolate the numerical cause of the changed text.

An offline diagnostic that supplies `00` for the empty fraction makes the bad
program pass 446/446. It is not sent to the model, does not edit its saved source,
and does not convert the failed attempt into a benchmark success.

The additional two runs per policy were registered in advance. Across all three
A-code attempts, fixed succeeds 3/3 and adaptive
succeeds 0/3:

| Policy | Attempt seconds | Median seconds | All succeeded |
|---|---|---|---|
| fixed8192 | 140.31 / 140.38 / 140.44 | 140.38 | True |
| adaptive4096 | 236.14 / 207.69 / 210.02 | 210.02 | False |

Failed-attempt times are not times to a correct answer. All discarded generation
and regeneration costs remain in task totals. Fixed 8192 is a ceiling, not a
forced output length. These data support fixed 8192 for this test workflow;
adaptive recovery remains an optional bounded fallback, not a speed improvement.

### Separate exact B-office regression

| Policy | Success | Attempt seconds | Length retries | Rejected deliveries |
|---|---|---|---|---|
| adaptive4096 | True | 500.04 | 0 | 0 |
| fixed8192 | True | 506.58 | 0 | 0 |

These two selected regressions do not enter the primary denominator and do not
rank hybrid vs exact task quality. Both profiles use the unchanged R21 engine.
The office submission tool now announces independent checking. R22-to-R23 is
therefore not a one-variable budget experiment: a first-correct submission is
not evidence of a runtime checker repairing the model's answer.

### Delivery verification and runtime

All 10 live ledger tasks that reached submission had correct fields on their first delivery; there was no live ledger rejection/repair to credit. Rejection and stopping are supported by the previous incorrect R22 B submission replay and injected CPU cases.
The guard records 0 final accepted ledger submissions that
disagree with the independent Python grader. That is limited evidence for the
declared contract, not a guarantee for arbitrary data or narrative claims.
All 22 attempts account for 204 API requests,
265 dispatched tool calls and 188,110 generated tokens.
There are 0 recorded protocol errors and
8 ordinary tool errors; details remain in the data file.
The latter are not engine crashes. Delivery checking consumes
1.243 seconds total inside the measured task clock.

Weight loading is separate from inference. The slowest readiness takes
322.50 seconds, while the median is
21.08 seconds. After one slow
load a resource sample shows about 53.97 GiB commit headroom; it was not sampled
during the slow load and does not establish an I/O, paging or thermal cause.
The full campaign lasts 103.89 minutes including launches.

The completed offline suite has **51 passing unit tests**, plus standalone format
recovery assertions. The frozen client/fixture hashes and both binary hashes are
checked throughout; paired initial inputs differ only in `max_tokens`. Weight
metadata and first/last MiB samples match before/after, not full-file weight hashes.
This work changes the test client, delivery workflow, cases and reports. It does
not modify `src`, change arithmetic defaults, deploy a new engine, or install the
workflow into Octop. R19 hybrid's existing numerical differences remain.

Machine-readable outcomes and phase metrics: [R23 evidence](benchmarks/r23-task-reliability.json).
Usage and test commands: [fixture README](../tools/agent_reliability/README.md).
