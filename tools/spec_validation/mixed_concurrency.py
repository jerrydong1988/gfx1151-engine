"""Opt-in mixed prefill/decode validation. No service/configuration mutations.

By default prints an offline plan. --run sends two sequential reference GENs,
then tests simultaneous submission and an already-decoding short request while
a long prefill starts. Imports the existing wire client and tokenizer helpers.
Numerical/token consistency only: no answer-quality or distribution claims.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import sys
import threading
import time

sys.dont_write_bytecode = True
from protocol_validation import TOKENIZER, Wire, compare, digest, now, parse_info, prompts, wrap, result_directory
from long_context import exact_input, tokenizer_for, capacity_check, save_json


class ObservedWire(Wire):
    def __init__(self, host, port, timeout, first_token=None):
        super().__init__(host, port, timeout)
        self.first_token = first_token
        self.request_id = None
        self.observed_tokens = []

    def send(self, line):
        if line.startswith("GEN "):
            self.request_id = int(line.split(" ", 2)[1])
        super().send(line)

    def read(self):
        line = super().read()
        if line.startswith("T "):
            fields = line.split()
            if len(fields) >= 3 and int(fields[1]) == self.request_id:
                self.observed_tokens.append(int(fields[2]))
                if self.first_token is not None:
                    self.first_token.set()
        return line


def window_start(path):
    return path.stat().st_size


def parse_log_text(text):
    lines = text.splitlines()
    lends = [line for line in lines if "[conc] prefill lends GPU" in line]
    # Explicit requeue logging is optional. Never infer that exact branch from
    # unrelated borrow/capacity messages; retain its absence as unknown.
    early = [line for line in lines if "[spec-early-yield]" in line]
    chain = []
    for line in lines:
        found = re.search(r"\bchain(?:-sample)?: .*rounds=(\d+) \(ngram=(\d+) mtp=(\d+)(?: serial=\d+)?\)", line)
        if found:
            chain.append({"rounds": int(found[1]), "ngram": int(found[2]), "mtp": int(found[3]), "raw": line})
    return {"lends": lends, "lend_count": len(lends), "explicit_early_yields": early,
            "chain_rounds": chain,
            "selected_lines": [line for line in lines if "[conc]" in line or "[spec-early-yield]" in line or "chain:" in line or "-> slot" in line or "failed:" in line]}


def log_window(path, offset):
    with path.open("rb") as handle:
        handle.seek(offset)
        raw = handle.read()
    return {"path": str(path), "start_byte": offset, "end_byte": offset + len(raw),
            **parse_log_text(raw.decode("utf8", "replace")),
            "attribution": "Isolated-engine phase window. The long request uses mode 0, so chain summary lines can only belong to this phase's short mode-4 request. Lend lines have no request ID."}


def check_record(record):
    if record.get("error"):
        return False
    done = record.get("done") or {}
    return (done.get("reason") in ("done", "length")
            and done.get("cached_tokens") == 0
            and len(record.get("tokens", [])) > 0
            and all(value is not False for value in record.get("checks", {}).values()))


def run_one(args, tk, case, ids, mode, budget, *, phase, barrier=None,
            wait_first=None, signal_first=None):
    out = {"case": case, "phase": phase, "mode": mode, "budget": budget,
           "prompt_tokens": len(ids), "prompt_ids_sha256": digest(ids),
           "connected_utc": None, "dispatch_utc": None, "finished_utc": None}
    wire = None
    try:
        with ObservedWire(args.host, args.port, args.timeout, signal_first) as wire:
            out["connected_utc"] = now()
            if barrier is not None:
                barrier.wait(timeout=20)
            if wait_first is not None:
                if not wait_first.wait(min(60, args.timeout)):
                    raise TimeoutError("Short request did not emit its first token; long request was not sent")
                out["short_first_token_observed_before_dispatch"] = True
            out["dispatch_utc"] = now()
            started = time.perf_counter()
            record = wire.generate(ids, mode, budget, eos=tk.encode("<|im_end|>"))
            out.update(record)
            out["roundtrip_wall_s"] = time.perf_counter() - started
            out["text"] = tk.decode(record["tokens"])
    except Exception as exc:
        out["error"] = f"{type(exc).__name__}: {exc}"
        if wire is not None:
            out["req"] = wire.request_id
            out["tokens"] = wire.observed_tokens
    out["finished_utc"] = now()
    out["record_valid_and_uncached"] = check_record(out)
    return out


def overlap_seconds(left, right):
    try:
        starts = [datetime.fromisoformat(x["dispatch_utc"]).timestamp() for x in (left, right)]
        ends = [datetime.fromisoformat(x["finished_utc"]).timestamp() for x in (left, right)]
        return max(0, min(ends) - max(starts))
    except (KeyError, TypeError, ValueError):
        return None


def execute(args, tk, long_ids, short_ids, meta):
    path = result_directory() / (args.label + ".json")
    if path.exists():
        raise FileExistsError(f"Refusing to overwrite existing evidence: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    with Wire(args.host, args.port, args.timeout) as wire:
        info = parse_info(wire.control("INFO"))
    if info.get("kv_slots") != 2:
        raise RuntimeError(f"Requires exactly two reported slots, got {info}")
    capacity = capacity_check(info, [len(long_ids), len(short_ids)], max(args.long_tokens, args.short_tokens))
    aggregate = len(long_ids) + args.long_tokens + len(short_ids) + args.short_tokens
    if aggregate > capacity["effective_limit"]:
        raise RuntimeError("Aggregate two-request token demand exceeds the reported shared pool capacity")
    startup = args.engine_log.read_text(encoding="utf8", errors="replace")
    actual_rmax = re.findall(r"prefill borrow armed \(R<=(\d+) rows", startup)
    data = {"status": "running", "started_utc": now(), "identity": args.identity,
            "label": args.label, "info": info, "capacity": capacity,
            "declared_process_flags": {"gamma": args.gamma, "rmax": args.rmax,
                                       "ngram_max": args.ngram_max, "ngram_min": args.ngram_min,
                                       "ngram_match": args.ngram_match},
            "engine_reported_borrow_rmax": int(actual_rmax[-1]) if actual_rmax else None,
            "flag_evidence_note": "GEN/INFO do not report gamma or ngram parameters. The caller must retain its launch/environment manifest; CLI values here are declarations, not independent verification.",
            "aggregate_reserved_tokens": aggregate, "long_input": {"token_ids": long_ids, **meta},
            "short_input": {"text": prompts()["repeat"], "token_ids": short_ids, "token_ids_sha256": digest(short_ids)},
            "reference": [], "phases": [], "quality_scoring": False,
            "sampler": "greedy, no SAMPLE suffix", "thinking": False}
    save_json(path, data)
    definitions = [("long_prefill", long_ids, 0, args.long_tokens),
                   ("short_chain", short_ids, 4, args.short_tokens)]
    for definition in definitions:
        rec = run_one(args, tk, *definition, phase="sequential_reference")
        data["reference"].append(rec)
        save_json(path, data)
        print(json.dumps({"phase": "reference", "case": rec["case"], "req": rec.get("req"),
                          "tokens": len(rec.get("tokens", [])), "valid": rec["record_valid_and_uncached"]}), flush=True)
        if not rec["record_valid_and_uncached"]:
            data.update(status="failed_reference", finished_utc=now())
            save_json(path, data)
            return 1
    references = {rec["case"]: rec for rec in data["reference"]}
    for repetition in range(args.repeats):
        for kind in ("simultaneous", "decode_then_prefill"):
            phase_name = f"{kind}-{repetition + 1}"
            phase = {"phase": phase_name, "started_utc": now(), "records": []}
            data["phases"].append(phase)
            offset = window_start(args.engine_log)
            barrier, first_token = threading.Barrier(2), threading.Event()
            with ThreadPoolExecutor(max_workers=2) as pool:
                jobs = []
                for definition in definitions:
                    case = definition[0]
                    jobs.append(pool.submit(run_one, args, tk, *definition, phase=phase_name,
                                            barrier=barrier,
                                            wait_first=first_token if kind == "decode_then_prefill" and case == "long_prefill" else None,
                                            signal_first=first_token if case == "short_chain" else None))
                for future in as_completed(jobs):
                    rec = future.result()
                    phase["records"].append(rec)
                    save_json(path, data)
            phase["records"].sort(key=lambda rec: rec["case"])
            phase["finished_utc"] = now()
            phase["engine_evidence"] = evidence = log_window(args.engine_log, offset)
            phase["comparisons"] = [{"case": rec["case"], **compare(references[rec["case"]], rec)}
                                    for rec in phase["records"] if "tokens" in rec]
            phase["wall_overlap_s"] = overlap_seconds(*phase["records"])
            phase["all_equal_to_same_mode_reference"] = (len(phase["comparisons"]) == 2
                and all(check_record(rec) for rec in phase["records"])
                and all(cmp["tokens_equal"] for cmp in phase["comparisons"]))
            chain_ng = sum(x["ngram"] for x in evidence["chain_rounds"])
            chain_mtp = sum(x["mtp"] for x in evidence["chain_rounds"])
            phase["coverage"] = {
                "overlap_observed": bool(phase["wall_overlap_s"] and phase["wall_overlap_s"] > 0),
                "prefill_lending_observed": evidence["lend_count"] > 0,
                "ngram_rounds_observed": chain_ng,
                "mtp_rounds_observed": chain_mtp,
                "declared_mtp_rows_exceed_rmax": args.gamma + 1 > args.rmax,
                "declared_small_ngram_rows_fit_rmax": args.ngram_max + 1 <= args.rmax,
                "explicit_early_yield_observed": len(evidence["explicit_early_yields"]) > 0,
                "boundary_note": "Overlap alone is not scheduler-branch coverage. Lend+MTP evidence demonstrates contention under the declared row boundary; the exact early-yield branch is confirmed only by an explicit [spec-early-yield] log, otherwise unknown.",
            }
            save_json(path, data)
            print(json.dumps({"phase": phase_name, "equal": phase["all_equal_to_same_mode_reference"],
                              "coverage": phase["coverage"]}, ensure_ascii=False), flush=True)
    all_equal = all(x["all_equal_to_same_mode_reference"] for x in data["phases"])
    contention = any(x["coverage"]["prefill_lending_observed"] and x["coverage"]["mtp_rounds_observed"] > 0 for x in data["phases"])
    both_drafters = any(x["coverage"]["ngram_rounds_observed"] > 0 and x["coverage"]["mtp_rounds_observed"] > 0 for x in data["phases"])
    data.update(status="complete" if all_equal else "failed_consistency", finished_utc=now(),
                all_same_mode_reference_sequences_equal=all_equal,
                lending_and_mtp_observed=contention, both_drafters_observed=both_drafters,
                explicit_early_yield_observed=any(x["coverage"]["explicit_early_yield_observed"] for x in data["phases"]))
    save_json(path, data)
    print(json.dumps({"result": data["status"], "path": str(path), "lending": contention,
                      "both_drafters": both_drafters, "early_yield": data["explicit_early_yield_observed"]}), flush=True)
    return 0 if all_equal and contention and both_drafters else 2


def selftest():
    evidence = parse_log_text("[conc] prefill lends GPU at L2\nchain: 12 tokens | rounds=5 (ngram=2 mtp=3)\n[spec-early-yield] rows=9 capacity=4\n")
    assert evidence["lend_count"] == 1 and evidence["chain_rounds"][0]["ngram"] == 2
    assert evidence["chain_rounds"][0]["mtp"] == 3 and len(evidence["explicit_early_yields"]) == 1
    assert not check_record({"error": "timeout"})
    assert not check_record({"tokens": [1], "done": {"reason": "length", "cached_tokens": 9}})
    assert overlap_seconds({"dispatch_utc": "2026-01-01T00:00:00+00:00", "finished_utc": "2026-01-01T00:00:04+00:00"},
                           {"dispatch_utc": "2026-01-01T00:00:02+00:00", "finished_utc": "2026-01-01T00:00:06+00:00"}) == 2
    print("SELFTEST PASS (offline; no sockets)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18870)
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument("--tokenizer", type=Path, default=TOKENIZER)
    parser.add_argument("--label", default="mixed-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
    parser.add_argument("--identity", default="")
    parser.add_argument("--engine-log", type=Path)
    parser.add_argument("--long-size", type=int, choices=(8192, 32768), default=8192)
    parser.add_argument("--long-tokens", type=int, default=64)
    parser.add_argument("--short-tokens", type=int, default=512)
    parser.add_argument("--gamma", type=int, default=8)
    parser.add_argument("--rmax", type=int, choices=(4, 8), default=4)
    parser.add_argument("--ngram-max", type=int, default=2)
    parser.add_argument("--ngram-min", type=int, default=1)
    parser.add_argument("--ngram-match", type=int, default=4)
    parser.add_argument("--repeats", type=int, default=1)
    args = parser.parse_args()
    if args.selftest:
        selftest()
        return 0
    if args.run and (not args.identity or not args.engine_log or not args.engine_log.is_file()):
        parser.error("--run requires --identity and an existing --engine-log")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", args.label):
        parser.error("label must be a simple filename component")
    if args.repeats < 1 or min(args.long_tokens, args.short_tokens) < 1:
        parser.error("repeats and output budgets must be positive")
    if not args.run:
        print(json.dumps({"status": "offline_plan", "long_input_tokens": args.long_size,
                          "long_mode": 0, "short_mode": 4, "short_prompt": "repeat",
                          "sequence": "2 sequential references; simultaneous 2-connection phase; short-first-token gated mixed phase",
                          "required_process_settings": {"MAX_CONTEXT": 65536, "PARALLEL": 2,
                              "GDEC_SPEC_PRECISION": "aligned", "GDEC_SPEC_GAMMA": args.gamma,
                              "GDEC_CONC_PREFILL": "1", "GDEC_CONC_RMAX": args.rmax,
                              "GDEC_CONC_PREFILL_QUANTUM_MS": "1", "GDEC_NGRAM_MAX": args.ngram_max,
                              "GDEC_NGRAM_MIN": args.ngram_min, "GDEC_NGRAM_MATCH": args.ngram_match,
                              "GDEC_RCKPT_MAX": "0", "GDEC_KVSNAP": "0"},
                          "note": "Flags are not applied by this script. No network/GPU calls in plan mode."}, ensure_ascii=False, indent=2))
        return 0
    try:
        tk = tokenizer_for(args.tokenizer)
    except (ValueError, FileNotFoundError) as error:
        parser.error(str(error))
    long_ids, metadata = exact_input(tk, args.long_size)
    short_ids = tk.encode(wrap(prompts()["repeat"]))
    return execute(args, tk, long_ids, short_ids, metadata)


if __name__ == "__main__":
    raise SystemExit(main())
