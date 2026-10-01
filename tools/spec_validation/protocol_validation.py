"""Opt-in raw GEN validation; never starts an engine or scores answer quality.

Default --suite plan only prints the test plan, without a socket connection.
The gamma label records an externally configured process setting; GEN has no
per-request gamma setting. See README.md for prerequisites and valid oracles.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import re
import socket
import sys
import threading
import time
from datetime import datetime, timezone

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1]
TOKENIZER = Path(os.environ["TOK_DIR"]).expanduser() if os.environ.get("TOK_DIR") else None
REQS = itertools.count(time.time_ns() // 1000)
INFO_FIELDS = "mtp draft_head ctx spec_rows default_drafter drafter_weights dflash2 cache_mb cache_align kv_slots slot_ctx cache_mode sampling".split()
DONE_FIELDS = "prompt_tokens generated_tokens prefill_ms decode_ms actual_drafter rounds committed cached_tokens proposed".split()
MODES = (0, 1, 3, 4)


def result_directory():
    """Resolve output placement without creating anything in offline modes."""
    configured = os.environ.get("GDEC_TEST_OUT")
    return (Path(configured).expanduser() if configured else Path.cwd() / "spec-validation-results").resolve()


def require_tokenizer(path):
    if path is None:
        raise ValueError("Set TOK_DIR or pass --tokenizer to an existing tokenizer directory")
    path = Path(path).expanduser().resolve()
    if not (path / "tokenizer.json").is_file():
        raise FileNotFoundError(f"No tokenizer.json in {path}; no files are downloaded")
    return path


def now():
    return datetime.now(timezone.utc).isoformat()


def digest(value):
    return hashlib.sha256(json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode()).hexdigest()


def parse_done(line):
    parts = line.split()
    if len(parts) < 7 or parts[0] != "D":
        raise ValueError(f"Invalid D line: {line}")
    out = {"req": int(parts[1]), "reason": parts[2], "raw": line}
    for key, value in zip(DONE_FIELDS, parts[3:]):
        out[key] = float(value) if key.endswith("_ms") else int(value)
    return out


def parse_info(line):
    parts = line.split()
    if not parts or parts[0] != "I":
        raise ValueError(f"Invalid INFO response: {line}")
    return {"raw": line, **dict(zip(INFO_FIELDS, map(int, parts[1:])))}


def compare(left, right):
    a, b = left["tokens"], right["tokens"]
    common = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y), min(len(a), len(b)))
    return {
        "tokens_equal": a == b,
        "common_prefix_tokens": common,
        "first_difference_1based": None if a == b else common + 1,
        "lengths": [len(a), len(b)],
        "finish_reasons": [left.get("done", {}).get("reason"), right.get("done", {}).get("reason")],
        "cached_tokens": [left.get("done", {}).get("cached_tokens"), right.get("done", {}).get("cached_tokens")],
    }


def wrap(text):
    return "<|im_start|>user\n" + text + "<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n"


def prompts():
    phrase = "核对编号，统一单位，按项汇总，保留依据。"
    return {
        "office": "请拟一份发给项目经理的中文工作通知：本周五17点前提交尚未签署合同的核对结果。需检查合同编号、客户名称、金额单位和责任部门，无法确认的数据单独列明。约200字，直接给出通知正文。",
        "code": "只输出Python函数summarize(rows)：按contract_id去重；同号金额不同抛出ValueError；按department累计amount。不要导入模块。",
        "ledger": "合同号,部门,金额（元）\nA01,研发,125\nA02,运营,375\nA01,研发,125\nA03,研发,240\nA04,运营,160\nA05,采购,80\n按合同号去重，列出唯一合同数、金额合计和各部门金额，不补充台账外信息。",
        "repeat": "以下材料用于复制测试：\n" + (phrase + "\n") * 14 + "请原样输出下面这一句40次，每次单独一行，不加标题、编号或解释：\n" + phrase,
    }


class Wire:
    """One active GEN per connection; cancellation is sent on that connection."""
    def __init__(self, host, port, timeout):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.file = self.sock.makefile("r", encoding="utf8")

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.file.close()
        self.sock.close()

    def send(self, line):
        self.sock.sendall((line + "\n").encode("ascii"))

    def read(self):
        line = self.file.readline()
        if not line:
            raise EOFError("Engine disconnected before final response")
        return line.strip()

    def control(self, verb):
        self.send(verb)
        return self.read()

    def generate(self, ids, mode, budget, eos=(), sample=None, cancel_after=None, disconnect_after=None, penalty=None):
        req = next(REQS)
        fields = ["GEN", req, budget, len(eos), *eos, len(ids), *ids, mode]
        if sample:
            fields += ["SAMPLE", *sample, "LOGPROBS"]
            if penalty:
                fields += ["PENALTY", *penalty]
        started = time.perf_counter()
        self.send(" ".join(map(str, fields)))
        result = {"req": req, "mode": mode, "budget": budget, "prompt_tokens": len(ids), "prompt_ids_sha256": digest(ids), "eos": list(eos), "sample": sample, "penalty": penalty, "tokens": [], "logprobs": [], "started_utc": now(), "done": None, "cancel_sent": False}
        first = cancel_at = None
        while True:
            line = self.read()
            parts = line.split()
            if parts[0] not in ("T", "D") or len(parts) < 3 or int(parts[1]) != req:
                raise ValueError(f"Unexpected/cross-request protocol line for {req}: {line}")
            if parts[0] == "D":
                result["done"] = parse_done(line)
                break
            first = first or time.perf_counter()
            result["tokens"].append(int(parts[2]))
            result["logprobs"].append(float(parts[3]) if len(parts) > 3 else None)
            if disconnect_after and len(result["tokens"]) >= disconnect_after:
                # The context manager closes the read file too, so no duplicate FD remains.
                result["intentional_disconnect"] = True
                break
            if cancel_after and len(result["tokens"]) >= cancel_after and not result["cancel_sent"]:
                cancel_at = time.perf_counter()
                self.send(f"X {req}")
                result["cancel_sent"] = True
                result["tokens_when_cancel_sent"] = len(result["tokens"])
        ended = time.perf_counter()
        result.update(elapsed_s=ended - started, ttft_s=None if first is None else first - started, cancel_response_s=None if cancel_at is None else ended - cancel_at, tokens_sha256=digest(result["tokens"]))
        d = result["done"] or {}
        result["checks"] = {
            "valid_token_ids": all(0 <= t < 248320 for t in result["tokens"]),
            "wire_count_matches_done": None if not d or d.get("reason") == "cancel" else d.get("generated_tokens") == len(result["tokens"]),
            "within_output_budget": len(result["tokens"]) <= budget,
            "finite_timings": all(math.isfinite(d[k]) and d[k] >= 0 for k in ("prefill_ms", "decode_ms") if k in d),
            "sample_logprobs_finite_nonpositive": None if not sample else all(lp is not None and math.isfinite(lp) and lp <= 1e-4 for lp in result["logprobs"]),
            "eos_not_emitted": not set(eos).intersection(result["tokens"]),
        }
        return result


class Harness:
    def __init__(self, args, tokenizer):
        self.args, self.tk = args, tokenizer
        self.path = result_directory() / (args.label + ".json")
        if self.path.exists():
            raise FileExistsError(f"Refusing to overwrite evidence: {self.path}")
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.lock = threading.Lock()
        self.data = {"schema": 1, "label": args.label, "status": "running", "started_utc": now(), "identity": args.identity, "gamma_declared": args.gamma, "gamma_verified_from_protocol": False, "suite": args.suite, "host": args.host, "port": args.port, "records": [], "comparisons": [], "notes": ["No answer correctness score. Same-seed serial versus speculative sampled outputs need not match.", "No automatic server startup, restart, or configuration change."]}
        self.eos = self.tk.encode("<|im_end|>")

    def conn(self):
        return Wire(self.args.host, self.args.port, self.args.timeout)

    def flush(self):
        # Called with lock held when worker threads write.
        temporary = self.path.with_suffix(".writing.json")
        temporary.write_text(json.dumps(self.data, ensure_ascii=False, indent=2), encoding="utf8")
        # Windows readers/AV can briefly deny rename even after our handle closed.
        # Retry the same evidence file; never drop a record or replace a new run.
        for attempt in range(30):
            try:
                os.replace(temporary, self.path)
                break
            except PermissionError:
                if attempt == 29:
                    raise
                time.sleep(min(0.05 * (attempt + 1), 0.5))

    def note(self, value):
        with self.lock:
            self.data["comparisons"].append(value)
            self.flush()
        print(json.dumps(value, ensure_ascii=False), flush=True)

    def record(self, case, ids, mode, *, conn=None, budget=None, eos=None, sample=None, cancel_after=None, disconnect_after=None, capture_log=True, penalty=None):
        log = Path(self.args.engine_log) if self.args.engine_log and capture_log else None
        offset = log.stat().st_size if log and log.exists() else None
        if conn is None:
            with self.conn() as connection:
                result = connection.generate(ids, mode, budget or self.args.tokens, self.eos if eos is None else eos, sample, cancel_after, disconnect_after, penalty)
        else:
            result = conn.generate(ids, mode, budget or self.args.tokens, self.eos if eos is None else eos, sample, cancel_after, disconnect_after, penalty)
        result["case"] = case
        result["text"] = self.tk.decode(result["tokens"])
        if offset is not None:
            with log.open("rb") as handle:
                handle.seek(offset)
                lines = handle.read().decode("utf8", "replace").splitlines()
            selected = [line for line in lines if "chain:" in line or f"req {result['req']} " in line]
            result["engine_log_window"] = {"path": str(log), "start_byte": offset, "selected_lines": selected, "attribution": "Time window only; requires isolated server with no unrelated traffic."}
            stats = [tuple(map(int, match)) for line in lines for match in re.findall(r"rounds=(\d+) \(ngram=(\d+) mtp=(\d+)\)", line)]
            result["chain_rounds_window"] = [{"total": a, "ngram": b, "mtp": c} for a, b, c in stats]
        done = result["done"] or {}
        actual = done.get("actual_drafter")
        chain_stats = result.get("chain_rounds_window", [])
        result["coverage"] = {
            "requested_drafter": mode,
            "actual_drafter": actual,
            "expected_drafter": 0 if mode == 3 and sample else mode,
            "speculative_proposals_observed": (done.get("proposed") or 0) > 0,
            "pure_ngram_proposals_observed": actual == 3 and (done.get("proposed") or 0) > 0,
            "chain_ngram_rounds_in_isolated_log_window": None if not chain_stats else sum(row["ngram"] for row in chain_stats),
            "chain_mtp_rounds_in_isolated_log_window": None if not chain_stats else sum(row["mtp"] for row in chain_stats),
            "note": "Chain coverage is attributable only with an isolated log window; missing evidence is not zero rounds.",
        }
        with self.lock:
            self.data["records"].append(result)
            self.flush()
        print(json.dumps({"case": case, "mode": mode, "tokens": len(result["tokens"]), "done": result["done"], "checks": result["checks"]}, ensure_ascii=False), flush=True)
        return result

    def greedy(self):
        for case, text in prompts().items():
            if self.args.cases and case not in self.args.cases.split(","):
                continue
            ids = self.tk.encode(wrap(text))
            records = [self.record("greedy/" + case, ids, mode) for mode in MODES]
            for other in records[1:]:
                self.note({"test": "greedy_token_parity", "case": case, "modes": [0, other["mode"]], **compare(records[0], other), "actual_drafter": other["done"].get("actual_drafter"), "oracle": "Exact emitted token equality is the intended greedy regression target, not an answer quality assessment."})

    def sampling(self):
        ids = self.tk.encode(wrap("写一小段Python代码处理合同编号重复、金额冲突与部门汇总，接着用中文解释边界情况。"))
        for seed in self.args.seeds:
            sample = [0.8, 20, 0.95, 0.0, seed]
            firsts = {}
            for mode in MODES:
                records = [self.record(f"sample/seed{seed}/repeat{index}", ids, mode, sample=sample) for index in range(self.args.repeats)]
                firsts[mode] = records[0]
                for other in records[1:]:
                    cold = all(r["done"].get("cached_tokens") == 0 for r in (records[0], other))
                    self.note({"test": "same_mode_seed_repeat", "seed": seed, "mode": mode, "cold_precondition": cold, **compare(records[0], other), "oracle": "Deterministic regression only under identical state, kernel/config and cold-cache preconditions; otherwise inconclusive."})
            self.note({"test": "sampled_ngram_serial_fallback", "seed": seed, **compare(firsts[0], firsts[3]), "actual_drafter": firsts[3]["done"].get("actual_drafter"), "expected_actual_drafter": 0})
            for mode in (1, 4):
                self.note({"test": "cross_mode_sample_observation_not_equality_oracle", "seed": seed, "modes": [0, mode], **compare(firsts[0], firsts[mode]), "oracle": "Difference is allowed; RNG draw schedules differ. Distribution fidelity needs a separate multi-seed statistical test."})

    def lifecycle(self):
        ids = self.tk.encode(wrap(prompts()["repeat"]))
        for mode in MODES:
            reference = self.record("eos/reference", ids, mode, budget=32, eos=[])
            # First occurrence selected explicitly; EOS is excluded from emitted tokens.
            chosen_index = min(7, len(reference["tokens"]) - 1)
            if chosen_index < 0:
                self.note({"test": "custom_eos", "mode": mode, "status": "inconclusive", "reason": "No reference tokens"})
                continue
            eos_token = reference["tokens"][chosen_index]
            expected = reference["tokens"][:reference["tokens"].index(eos_token)]
            stopped = self.record("eos/custom", ids, mode, budget=32, eos=[eos_token])
            self.note({"test": "custom_eos", "mode": mode, "eos_token": eos_token, "expected_prefix": expected, "tokens_equal_expected_prefix": stopped["tokens"] == expected, "finish_is_done": stopped["done"]["reason"] == "done"})
            with self.conn() as connection:
                cancelled = self.record("cancel/request", ids, mode, conn=connection, budget=max(256, self.args.tokens), eos=[], cancel_after=8)
                pong = connection.control("PING")
                after = self.record("cancel/same_connection_reuse", self.tk.encode(wrap("只回答：连接正常。")), mode, conn=connection, budget=32)
            self.note({"test": "cancel_and_reuse", "mode": mode, "cancel_observed": cancelled["done"]["reason"] == "cancel", "cancel_sent": cancelled["cancel_sent"], "cancel_response_s": cancelled["cancel_response_s"], "latency_within_budget": cancelled["cancel_response_s"] is not None and cancelled["cancel_response_s"] <= self.args.cancel_timeout, "ping_ok": pong == "PONG", "reuse_finished": after["done"]["reason"] in ("done", "length"), "note": "Buffered tokens after X are allowed; cancellation racing a completed request is inconclusive."})
            with self.conn() as connection:
                first = self.record("continuation/first", ids, mode, conn=connection, budget=48, eos=[])
                extension = self.tk.encode("\n" + "核对编号，统一单位，按项汇总，保留依据。\n" * 5)
                continued_ids = ids + first["tokens"] + extension
                warm = self.record("continuation/warm", continued_ids, mode, conn=connection, budget=64, eos=[])
            cold = self.record("continuation/fresh_connection", continued_ids, mode, budget=64, eos=[])
            self.note({"test": "continuation_warm_vs_cold", "mode": mode, "warm_cached_positive": (warm["done"].get("cached_tokens") or 0) > 0, "fresh_cached_zero": cold["done"].get("cached_tokens") == 0, **compare(warm, cold), "oracle": "Token parity is a regression goal; failure can be numeric cache/prefill path variation, not necessarily cross-request contamination."})
        with self.conn() as connection:
            dropped = self.record("disconnect/active", ids, 4, conn=connection, budget=max(256, self.args.tokens), eos=[], disconnect_after=8)
        with self.conn() as connection:
            pong = connection.control("PING")
            recovery = self.record("disconnect/recovery", self.tk.encode(wrap("只回答：服务正常。")), 0, conn=connection, budget=32)
        self.note({"test": "disconnect_recovery", "dropped_req": dropped["req"], "ping_ok": pong == "PONG", "recovery_finished": recovery["done"]["reason"] in ("done", "length"), "note": "Recovery checks liveness, not proof that GPU work stopped immediately after disconnect."})

    def concurrent(self):
        slots = self.data["info"].get("kv_slots", 1)
        if not self.args.allow_concurrency or slots < 2:
            self.note({"test": "concurrent", "status": "skipped", "slots": slots, "reason": "Requires --allow-concurrency, slots >= 2 and a build whose precision guards support yield safely."})
            return
        flows = [("alpha", 0, None), ("beta", 1, None), ("gamma", 3, None), ("delta", 4, None), ("sample_mtp", 1, [0.8, 20, 0.95, 0, 777]), ("sample_chain", 4, [0.8, 20, 0.95, 0, 777])]
        def flow(spec, phase, barrier=None):
            name, mode, sample = spec
            ids = self.tk.encode(wrap(f"[隔离测试 {name}] 只重复本行标识 {name} 并用中文说明编号核对、单位统一和保留依据的做法。"))
            records = []
            with self.conn() as connection:
                if barrier:
                    barrier.wait(timeout=self.args.timeout)
                records.append(self.record(f"concurrent/{phase}/{name}/turn0", ids, mode, conn=connection, budget=64, eos=[], sample=sample, capture_log=False))
                ids += records[0]["tokens"] + self.tk.encode(f"\n请继续说明标识 {name} 对应的检查步骤。\n")
                records.append(self.record(f"concurrent/{phase}/{name}/turn1", ids, mode, conn=connection, budget=64, eos=[], sample=sample, capture_log=False))
            return records
        baselines = [flow(spec, "sequential") for spec in flows]
        # All connections released together. Requests beyond slot capacity test queuing.
        barrier = threading.Barrier(len(flows))
        with concurrent.futures.ThreadPoolExecutor(max_workers=len(flows)) as pool:
            futures = [pool.submit(flow, spec, "parallel", barrier) for spec in flows]
            parallel = [future.result() for future in futures]
        for spec, seq, par in zip(flows, baselines, parallel):
            for turn, (a, b) in enumerate(zip(seq, par)):
                self.note({"test": "concurrent_vs_sequential", "flow": spec[0], "mode": spec[1], "turn": turn, **compare(a, b), "prompts_equal": a["prompt_ids_sha256"] == b["prompt_ids_sha256"], "oracle": "Same per-flow mode/seed exact token parity is the target. Each received line is also checked for its request ID. Turn 1 depends on turn 0 equality."})

    def execute(self):
        try:
            with self.conn() as connection:
                self.data["info"] = parse_info(connection.control("INFO"))
                self.data["ping_ok"] = connection.control("PING") == "PONG"
            self.flush()
            for name in ("greedy", "sampling", "lifecycle", "concurrent"):
                if self.args.suite in (name, "all"):
                    getattr(self, name)()
            self.data["status"] = "complete"
        except Exception as error:
            self.data.update(status="error", error=f"{type(error).__name__}: {error}")
            raise
        finally:
            self.data["finished_utc"] = now()
            self.flush()


def compare_files(paths):
    datasets = [json.loads(Path(p).read_text(encoding="utf8")) for p in paths]
    reference = datasets[0]
    def index(data):
        return {(r["case"], r["mode"], r["prompt_ids_sha256"], r["budget"], tuple(r["eos"])): r for r in data["records"] if r["case"].startswith("greedy/") and r.get("done")}
    first = index(reference)
    output = []
    for data in datasets[1:]:
        if data["identity"] != reference["identity"]:
            raise ValueError("Identity changed: cannot attribute differences to gamma")
        for key, other in index(data).items():
            if key in first:
                output.append({"case": key[0], "mode": key[1], "gammas_declared": [reference["gamma_declared"], data["gamma_declared"]], "labels": [reference["label"], data["label"]], **compare(first[key], other)})
    print(json.dumps(output, ensure_ascii=False, indent=2))


def selftest():
    assert parse_done("D 12 cancel 0 0 0.0 0.0")["reason"] == "cancel"
    assert parse_done("D 3 length 10 8 2.1 12.5 4 2 7 0 8")["proposed"] == 8
    assert parse_info("I 1 0 16384 8 4 1 0 0 0 2 16384 1 1")["kv_slots"] == 2
    assert compare({"tokens": [1, 2]}, {"tokens": [1, 3]})["first_difference_1based"] == 2
    assert compare({"tokens": [1]}, {"tokens": [1, 2]})["common_prefix_tokens"] == 1
    assert re.findall(r"rounds=(\d+) \(ngram=(\d+) mtp=(\d+)\)", "chain: rounds=9 (ngram=4 mtp=5)") == [("9", "4", "5")]
    class FakeWire(Wire):
        def __init__(self):
            self.lines = []
            self.sent = []

        def send(self, line):
            self.sent.append(line)
            parts = line.split()
            if parts[0] == "GEN":
                rid = int(parts[1])
                n_eos = int(parts[3])
                n_ids = int(parts[4 + n_eos])
                mode = int(parts[5 + n_eos + n_ids])
                lp = " -0.3" if "LOGPROBS" in parts else ""
                self.lines = [f"T {rid} {t}{lp}" for t in (11, 12, 13)]
                self.lines.append(f"D {rid} length {n_ids} 3 1.2 2.3 {mode} 1 2 0 4")
            elif parts[0] == "X":
                self.lines[-1] = self.lines[-1].replace(" length ", " cancel ")

        def read(self):
            return self.lines.pop(0)

    fake = FakeWire()
    output = fake.generate([101, 102], 4, 3, [999], [0.8, 20, 0.95, 0, 777], cancel_after=1)
    assert output["tokens"] == [11, 12, 13]
    assert output["done"]["reason"] == "cancel"
    assert output["tokens_when_cancel_sent"] == 1  # two already-buffered tokens allowed
    assert output["checks"]["wire_count_matches_done"] is None  # abbreviated cancel D line
    assert output["checks"]["sample_logprobs_finite_nonpositive"]
    assert "SAMPLE 0.8 20 0.95 0 777 LOGPROBS" in fake.sent[0]

    class WrongRequest(FakeWire):
        def read(self):
            return "T -9999 11"
    try:
        WrongRequest().generate([101], 0, 3)
    except ValueError as error:
        assert "cross-request" in str(error)
    else:
        raise AssertionError("Cross-request token was not rejected")
    print("PASS parser, request encoding, buffered cancellation and request-isolation selftest; no socket opened")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=["plan", "info", "greedy", "sampling", "lifecycle", "concurrent", "all"], default="plan")
    parser.add_argument("--label")
    parser.add_argument("--identity", default="", help="Exact build/model/precision/config identity, excluding gamma; required for live suites")
    parser.add_argument("--gamma", type=int, choices=range(1, 9), help="Declared EXTERNALLY set GDEC_SPEC_GAMMA; cannot set via GEN")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18870)
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument("--cancel-timeout", type=float, default=30)
    parser.add_argument("--tokens", type=int, default=128)
    parser.add_argument("--repeats", type=int, default=2)
    parser.add_argument("--seeds", type=int, nargs="+", default=[777, 20260930])
    parser.add_argument("--cases", default="")
    parser.add_argument("--engine-log")
    parser.add_argument("--tokenizer", type=Path, default=TOKENIZER)
    parser.add_argument("--allow-concurrency", action="store_true")
    parser.add_argument("--compare-files", nargs="+")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        selftest()
        return
    if args.compare_files:
        compare_files(args.compare_files)
        return
    if args.tokens < 1 or args.repeats < 2 or any(seed < 0 or seed >= 2**64 for seed in args.seeds):
        parser.error("Positive token budget, repeats >= 2 and uint64 seeds required")
    if set(filter(None, args.cases.split(","))) - prompts().keys():
        parser.error("Unknown --cases; choose office,code,ledger,repeat")
    try:
        args.tokenizer = require_tokenizer(args.tokenizer)
    except (ValueError, FileNotFoundError) as error:
        parser.error(str(error))
    os.environ["TOK_DIR"] = str(args.tokenizer)
    sys.path.insert(0, str(SOURCE / "tools"))
    from qwentok import Tokenizer
    tokenizer = Tokenizer()
    if args.suite == "plan":
        print(json.dumps({"no_socket_opened": True, "modes": {0: "serial", 1: "pure MTP", 3: "pure ngram greedy / serial sampling fallback", 4: "ngram then MTP chain"}, "gamma_sweep_recommendation": [1, 2, 4, 7, 8], "prompts": [{"case": key, "tokens": len(tokenizer.encode(wrap(value))), "sha256": digest(tokenizer.encode(wrap(value)))} for key, value in prompts().items()], "suites": ["greedy", "sampling", "lifecycle", "concurrent"], "sampling_oracle": "Same-mode/seed repeats only; cross-mode identical-seed equality is not required."}, ensure_ascii=False, indent=2))
        return
    if not args.label or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", args.label) or not args.identity or args.gamma is None:
        parser.error("Live suites require a simple --label, nonempty --identity and declared --gamma")
    Harness(args, tokenizer).execute()


if __name__ == "__main__":
    main()
