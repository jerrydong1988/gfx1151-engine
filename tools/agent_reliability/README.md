# Agent reliability fixtures (R18–R23)

These tests execute real, bounded local tools against a synthetic 1,002-row ledger
or a small Python-function exercise. They are not an Octop integration, a benchmark
of private customer records, or a general-purpose tool executor.

The fixture and client descend from R14/R17. R18 fixes SQLite authorization for
CTE `count(*)`, declares the submission shape explicitly, and adds an optional
bounded recovery policy for `invalid_tool_call`. The statistical acceptance
oracle is unchanged. `amounts-v2` changes invoice and payment amounts and has a
separate recomputed oracle. No reference answer is returned to the model.
Each new ledger run stores its variant in `run.json` so later rescoring uses the
saved dataset rather than the evaluator process's current variant setting.

## Offline checks

Python 3.10+ and the standard library are sufficient. From the repository root:

```text
python -B tools/agent_reliability/test_fixture_cpu.py
python -B tools/agent_reliability/test_cte.py
python -B tools/agent_reliability/test_variant.py
python -B tools/agent_reliability/test_r14_agent_client_cpu.py
python -B tools/agent_reliability/test_recovery.py
python -B tools/agent_reliability/test_recovery_loop.py
```

These commands do not contact a model. Temporary fixture files are confined to
this directory and cleaned up; reports go to stdout. Keep the reference parser
and test oracle out of an actual agent's tool-accessible resources.

## Isolated model run

Start a separate engine/API on loopback engine port 18870 and API port 18871.
Use the matching API with request statistics enabled, no admin overrides, and
no other clients. Select serial or MTP in the engine before the run. The client
checks the actual drafter and token accounting in each completed request's
statistics; `--mode` is an expected mode, not a server reconfiguration request.
The Windows machine-specific process ownership/launch driver is not included.

Example for an already-running MTP instance:

```text
python -B tools/agent_reliability/r14_agent_client.py --run --mode 1 --identity local-r18-mtp --scenario all --repeat 1 --reasoning true --max-turns 24 --max-tokens 4096 --tool-repair 2 --fixture-variant base --output tools/agent_reliability/runs/mtp-base.json --work-dir tools/agent_reliability/runs/mtp-base
```

Omit `--run` for an offline argument preview. Both output paths must be new.
Use `--fixture-variant amounts-v2` for the altered ledger, and `--mode 0` only
after launching a serial instance. Requests use greedy sampling. Reasoning and
recovery are explicit options; their defaults remain off. The 24-round limit
includes failed generation attempts; each call has its own output budget.

## What recovery guarantees

`tool_recovery.py` only produces feedback after an explicit `invalid_tool_call`
response. The client discards that response's partial calls and never executes
them. It allows at most two distinct-error regenerations per task, stops on a
repeated diagnostic, and does not reinterpret tool names or modify arguments.
Network failures, timeouts and `length` are not automatically retried. Successful
earlier tool results remain in the conversation; no prior side effect is replayed.

This policy must be implemented by a consumer to apply to that consumer. Changing
the engine API alone does not add this recovery loop to Octop or another client.
Streaming consumers must wait for successful completion of the whole response
before executing calls: the API still sends argument deltas before validation.

The API validator handles common schema types, required/properties/items,
enum/const, additionalProperties, and fully supported allOf/anyOf/oneOf branches.
It is output validation, not constrained decoding or full JSON Schema support;
unresolved references and unsupported constraints still need consumer validation.

## Acceptance and timing

Successful lifecycle completion is not business success. The client requires a
natural final response, actual required tool use, a stored submission and an
independent exact-value/type check for the ledger. Code repair additionally runs
private cases. The ledger scorer rejects extra fields, wrong integers and
incorrect exclusion counts; both submitted shapes are also checked by the API.

Task wall time includes all turns, tool execution and recovery; it excludes model
loading and independent scoring. Phase throughput uses uncached prompt tokens
and engine phase times, not accumulated context/cache-hit totals. Failed attempts
are retained under `failed_reqstat` and must be included when aggregating cost.
See [the R18 report](../../docs/R18_TASK_RELIABILITY.md) for the tested scope and
limits; a few fixed tasks are not a broad success-rate estimate.

## R23 bounded output recovery and source verification

R23 adds an opt-in evaluation wrapper. The legacy client keeps its original
behavior unless the wrapper supplies the new budget options. The inference
engine/API binaries, model weights and daily deployment are unchanged.

For an already running isolated MTP instance on the ports above, each command
below performs actual model calls (there is no offline-preview mode in this
wrapper). Give every run new output and work paths:

```text
python -B tools/agent_reliability/r23_client.py --variant r23-d --scenario office --policy fixed8192 --delivery-guard --identity local-r23-d-office-fixed --output tools/agent_reliability/runs/local-r23-d-office-fixed.json --work-dir tools/agent_reliability/runs/local-r23-d-office-fixed
```

Use `--policy adaptive4096` and fresh paths to start with a 4096-token cap, then
allow one verified `length` regeneration at up to 8192 tokens. It reuses the valid
message history, discards the incomplete assistant response without executing
any of its tools, and retains the larger cap afterward. A larger cap permits
more output; it does not force that amount to be generated. Regeneration costs
extra and may produce different text, even with greedy sampling. Context or
total-budget exhaustion, unclear `length`, and transport failures do not retry.

`--delivery-guard` applies only to `office`: an independent SQL computation checks
the submission against the declared source contract before accepted storage.
Rejected attempts remain in the audit files, with only failed field names
returned to the agent. Two rejections stop the task; source mutation or checker
failure blocks delivery immediately. The original Python scorer still runs
independently afterward. This checker does not apply to arbitrary XLSX files,
does not validate HTML/PPTX layouts and is not installed into Octop by these
changes. Code tasks retain public tests and separate final private grading.

Offline checks for these additions:

```text
python -B tools/agent_reliability/test_budget_recovery.py
python -B tools/agent_reliability/test_ledger_verifier.py
python -B tools/agent_reliability/test_r23_report.py
```

The R23 wrapper fixes greedy sampling, thinking enabled, 24 API attempts, 65536
generated tokens per task, and a 1800-second request-admission limit. Already
running requests are not interrupted by that admission limit. For reproducible
paired tests, use the same arithmetic profile, fresh engine/API processes, no
overrides and no other clients. The Windows process-ownership driver remains
machine-specific and is not included. `r23_report.py` reads that driver's frozen
evidence manifest; it makes no model calls and never reruns failed cases.

See [R23 protocol and results](../../docs/TASK_RELIABILITY_R23.md). Primary policy
pairs, selected exact regressions and timing repeats have separate denominators.
