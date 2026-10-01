"""Long-input sampling regression against an externally started engine.

Default is an offline plan: no sockets, tokenizer, or GPU work. --run sends
sequential GEN requests only; never starts/stops services or changes settings.
Same-mode/same-seed repeated tokens are the regression oracle. Cross-mode
equality is explicitly NOT required and this is not a distribution benchmark.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import re
import sys
import time

sys.dont_write_bytecode = True
from protocol_validation import TOKENIZER, Wire, compare, digest, now, parse_info, require_tokenizer, result_directory
from long_context import exact_input, tokenizer_for, capacity_check, save_json, log_start, log_window


class RecordedWire(Wire):
    def __init__(self, *args):
        super().__init__(*args)
        self.request_id = None
        self.observed_tokens = []
        self.observed_logprobs = []

    def send(self, line):
        if line.startswith("GEN "):
            self.request_id = int(line.split(" ", 2)[1])
        super().send(line)

    def read(self):
        line = super().read()
        parts = line.split()
        if len(parts) >= 3 and parts[0] == "T" and int(parts[1]) == self.request_id:
            self.observed_tokens.append(int(parts[2]))
            self.observed_logprobs.append(float(parts[3]) if len(parts) > 3 else None)
        return line


def size_value(value):
    found = re.fullmatch(r"([1-9][0-9]*)([KkMm]?)", value)
    if not found:
        raise argparse.ArgumentTypeError("Use a positive token count, e.g. 32768 or 128K")
    return int(found[1]) * {"": 1, "k": 1024, "m": 1024**2}[found[2].lower()]


def record_checks(record):
    done = record.get("done") or {}
    tokens, logprobs = record.get("tokens", []), record.get("logprobs", [])
    checks = {
        "request_completed_normally": not record.get("error") and done.get("reason") in ("done", "length"),
        "nonempty_output": bool(tokens),
        "uncached": done.get("cached_tokens") == 0,
        "wire_count_matches_output": done.get("generated_tokens") == len(tokens),
        "within_budget": len(tokens) <= record["budget"],
        "target_logprobs_finite_nonpositive": len(logprobs) == len(tokens) and bool(tokens)
            and all(lp is not None and math.isfinite(lp) and lp <= 1e-4 for lp in logprobs),
        "wire_client_checks": all(value is not False for value in record.get("checks", {}).values()),
    }
    actual, requested = done.get("actual_drafter"), record["mode"]
    if requested == 0:
        actual_ok = actual == 0
    else:
        actual_ok = actual == requested and done.get("rounds", 0) > 0 and done.get("proposed", 0) > 0
    return {"checks": checks, "all_checks_passed": all(checks.values()),
            "requested_mode": requested, "actual_mode": actual,
            "requested_drafter_observed": actual_ok,
            "drafter_note": "Mode 4 is ngram+MTP chain, not pure MTP. A too-short/EOS response may not enter any speculative round; actual mode is recorded and missing coverage is not promoted to a pass."}


def sampling_plan(args):
    return {"status": "offline_plan", "no_socket_opened": True,
            "input_tokens": args.size, "output_budget": args.tokens,
            "modes": args.modes, "mode_names": {0: "serial", 1: "pure MTP", 4: "ngram+MTP chain"},
            "repeats_per_mode_seed": args.repeats, "seeds": args.seeds,
            "sampling": {"temperature": 0.8, "top_k": 20, "top_p": 0.95, "min_p": 0.0},
            "request_count": len(args.modes) * args.repeats * len(args.seeds),
            "externally_declared_gamma": args.gamma,
            "requirements": "Dedicated already-started engine; input+output fits context; disable RAM/SSD prefix caching; preserve binary/model/flags identity. No settings are applied by this script.",
            "oracle": "Repeated same-mode/same-seed token and finish-reason equality with zero cached tokens. Cross-mode equality is not required. Finite logprobs are protocol checks, not proof of matching the serial target distribution."}


def execute(args):
    output_dir = result_directory()
    destination = output_dir / (args.label + ".json")
    input_path = output_dir / (args.label + ".input.json")
    if destination.exists() or input_path.exists():
        raise FileExistsError("Refusing to replace existing result/input evidence; use a new label")
    tokenizer = tokenizer_for(require_tokenizer(args.tokenizer))
    ids, metadata = exact_input(tokenizer, args.size)
    eos = tokenizer.encode("<|im_end|>")
    with Wire(args.host, args.port, args.timeout) as wire:
        info = parse_info(wire.control("INFO"))
    capacity = capacity_check(info, [args.size], args.tokens)
    if not info.get("sampling"):
        raise RuntimeError("INFO does not advertise sampling support; no GEN sent")
    if set(args.modes).intersection({1, 4}) and not info.get("mtp"):
        raise RuntimeError("Requested MTP modes require loaded MTP weights; no GEN sent")
    output_dir.mkdir(parents=True, exist_ok=True)
    save_json(input_path, {"input_ids": ids, **metadata})
    result = {"status": "running", "started_utc": now(), "label": args.label,
              "identity": args.identity, "info": info, "capacity": capacity,
              "gamma_declared_externally": args.gamma, "plan": sampling_plan(args),
              "input_file": str(input_path), "input_ids_sha256": digest(ids),
              "input_tokens": len(ids), "eos": eos, "records": [], "repeat_checks": [],
              "cross_mode_observations": [], "answer_quality_scored": False,
              "scope_note": "Synthetic repeated input; task correctness and sampled-distribution fidelity are not evaluated. LOGPROBS values are engine-reported target token logprobs, not residual draw logprobs."}
    save_json(destination, result)
    for seed in args.seeds:
        firsts = {}
        for mode in args.modes:
            for repetition in range(args.repeats):
                offset = log_start(args.engine_log)
                wire = None
                record = {"mode": mode, "seed": seed, "repeat": repetition + 1,
                          "budget": args.tokens, "dispatch_utc": now(), "done": None,
                          "tokens": [], "logprobs": [], "input_ids_sha256": digest(ids)}
                started = time.perf_counter()
                try:
                    with RecordedWire(args.host, args.port, args.timeout) as wire:
                        record.update(wire.generate(ids, mode, args.tokens, eos=eos,
                                                   sample=[0.8, 20, 0.95, 0.0, seed]))
                    record["text"] = tokenizer.decode(record["tokens"])
                except Exception as exc:
                    record["error"] = f"{type(exc).__name__}: {exc}"
                    if wire is not None:
                        record.update(req=wire.request_id, tokens=wire.observed_tokens,
                                      logprobs=wire.observed_logprobs)
                record.update(finished_utc=now(), wall_s=time.perf_counter() - started)
                record["validation"] = record_checks(record)
                record["engine_log_window"] = log_window(args.engine_log, offset, record.get("req"))
                result["records"].append(record)
                if mode not in firsts:
                    firsts[mode] = record
                else:
                    baseline = firsts[mode]
                    compared = compare(baseline, record)
                    cold = all(r["validation"]["all_checks_passed"] for r in (baseline, record))
                    result["repeat_checks"].append({"seed": seed, "mode": mode,
                        "repeat_pair": [1, repetition + 1], **compared,
                        "preconditions_met": cold,
                        "logprobs_exactly_equal": baseline["logprobs"] == record["logprobs"],
                        "passed": cold and compared["tokens_equal"]
                            and compared["finish_reasons"][0] == compared["finish_reasons"][1]})
                save_json(destination, result)
                print(json.dumps({"seed": seed, "mode": mode, "repeat": repetition + 1,
                                  "req": record.get("req"), "tokens": len(record["tokens"]),
                                  "validation": record["validation"], "elapsed_s": record["wall_s"]},
                                 ensure_ascii=False), flush=True)
                if record.get("error") or not record["validation"]["all_checks_passed"]:
                    result.update(status="failed_request", finished_utc=now())
                    save_json(destination, result)
                    return 1
        if 0 in firsts:
            for mode in args.modes:
                if mode == 0:
                    continue
                result["cross_mode_observations"].append({"seed": seed, "modes": [0, mode],
                    **compare(firsts[0], firsts[mode]), "equality_required": False,
                    "note": "Different RNG schedules; this observation is not a pass/fail oracle."})
        save_json(destination, result)
    repeat_ok = bool(result["repeat_checks"]) and all(c["passed"] for c in result["repeat_checks"])
    modes_observed = all(r["validation"]["requested_drafter_observed"] for r in result["records"])
    result.update(status="complete" if repeat_ok and modes_observed else "failed_repeat" if not repeat_ok else "incomplete_drafter_coverage",
                  finished_utc=now(), all_same_mode_seed_repeats_equal=repeat_ok,
                  requested_drafter_observed_for_every_record=modes_observed,
                  all_target_logprobs_finite_nonpositive=all(r["validation"]["checks"]["target_logprobs_finite_nonpositive"] for r in result["records"]))
    save_json(destination, result)
    print(json.dumps({"result": result["status"], "path": str(destination),
                      "records": len(result["records"]), "same_mode_repeats_equal": repeat_ok,
                      "actual_drafter_coverage": modes_observed}), flush=True)
    return 0 if repeat_ok and modes_observed else 2


def selftest():
    assert size_value("128K") == 131072 and size_value("32768") == 32768
    base = {"budget": 64, "mode": 1, "tokens": [17], "logprobs": [-0.2],
            "done": {"reason": "length", "generated_tokens": 1, "cached_tokens": 0,
                     "actual_drafter": 1, "rounds": 1, "proposed": 4}, "checks": {"count": True}}
    assert record_checks(base)["all_checks_passed"]
    assert record_checks(base)["requested_drafter_observed"]
    assert not record_checks({**base, "logprobs": [float("nan")]})["all_checks_passed"]
    assert not record_checks({**base, "done": {**base["done"], "cached_tokens": 4}})["all_checks_passed"]
    fallback = record_checks({**base, "done": {**base["done"], "actual_drafter": 0}})
    assert fallback["all_checks_passed"] and not fallback["requested_drafter_observed"]
    print("SELFTEST PASS (offline, no sockets)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--label", default="long-sampling-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
    parser.add_argument("--identity", default="")
    parser.add_argument("--gamma", type=int, choices=range(1, 9))
    parser.add_argument("--size", type=size_value, default=32768)
    parser.add_argument("--tokens", type=int, default=64)
    parser.add_argument("--modes", type=int, choices=(0, 1, 4), nargs="+", default=[0, 1, 4])
    parser.add_argument("--seeds", type=int, nargs="+", default=[777])
    parser.add_argument("--repeats", type=int, default=2)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18870)
    parser.add_argument("--timeout", type=float, default=900)
    parser.add_argument("--tokenizer", type=Path, default=TOKENIZER)
    parser.add_argument("--engine-log", type=Path)
    args = parser.parse_args()
    if args.selftest:
        selftest()
        return 0
    if args.tokens < 1 or args.repeats < 2 or any(seed < 0 or seed >= 2**64 for seed in args.seeds):
        parser.error("Positive output budget, repeats>=2, and uint64 seeds required")
    if len(set(args.modes)) != len(args.modes) or len(set(args.seeds)) != len(args.seeds):
        parser.error("Modes/seeds must not contain duplicates")
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", args.label):
        parser.error("Label must be a simple filename component")
    if args.run:
        if not args.identity or args.gamma is None:
            parser.error("--run requires exact --identity and externally configured --gamma")
        if args.engine_log and not args.engine_log.is_file():
            parser.error("Supplied --engine-log does not exist")
        return execute(args)
    print(json.dumps(sampling_plan(args), ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
