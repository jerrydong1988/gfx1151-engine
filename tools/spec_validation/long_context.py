"""Exact-token long-input validation over the existing raw GEN client.

Default execution is an OFFLINE plan. Only --run opens a socket and sends GEN.
Examples, after the operator starts a dedicated engine with the intended gamma:

  python long_context.py --selftest
  python long_context.py --sizes 2050 2052 8192 --dry-run
  python long_context.py --run --label long-g4-small --gamma 4 \
      --identity BUILD_WEIGHTS_FLAGS --sizes 2050 2052 8192 --modes 0 1 4
  python long_context.py --run --label long-g4-large --gamma 4 \
      --identity BUILD_WEIGHTS_FLAGS --sizes 32768 131072 260000 --tokens 128

The program does not modify engine configuration, restart services, download
anything, or grade answers. Gamma is externally configured and only declared
here. Exact input token IDs are saved once per size beside the result JSON.
This repeated synthetic input is a numerical/context-capacity regression test;
its speed must not be generalized to arbitrary documents or answer quality.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import time

sys.dont_write_bytecode = True
from protocol_validation import SOURCE, TOKENIZER, Wire, compare, digest, now, parse_info, require_tokenizer, result_directory

DEFAULT_SIZES = [2050, 2052, 8192, 32768, 131072, 260000]
PREFIX = (
    "<|im_start|>user\n"
    "以下为合成的合同核对记录，用于验证长输入的稳定性。数据段会重复，"
    "不代表实际业务统计。请等待数据结束后的指令。\n[DATA BEGIN]\n"
)
# ASCII-only filler ensures that cutting at any token boundary cannot leave an
# incomplete multibyte character before the complete Chinese/chat suffix.
SEGMENT = "\n".join(
    f"REC-{i:04d} department=OPS contract=CN-{i:04d} amount_yuan={i * 17} status=REVIEW source=LEDGER"
    for i in range(1, 33)
) + "\n"
SUFFIX = (
    "\n[DATA END]\n"
    "请写一份约250字的中文合同数据核对工作说明，分三段说明合同编号去重、"
    "金额单位统一、无法确认的数据留待复核。只说明方法，不计算上述合成数据的总额，"
    "不编造客户、期限或统计结果。直接给出正文。"
    "<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n"
)


def tokenizer_for(path):
    os.environ["TOK_DIR"] = str(require_tokenizer(path))
    sys.path.insert(0, str(SOURCE / "tools"))
    from qwentok import Tokenizer
    return Tokenizer()


def exact_input(tk, size):
    head, block, tail = tk.encode(PREFIX), tk.encode(SEGMENT), tk.encode(SUFFIX)
    if not block:
        raise ValueError("Filler encoded to an empty sequence")
    remaining = size - len(head) - len(tail)
    if remaining < 1:
        raise ValueError(f"Input {size} is too short: need at least {len(head) + len(tail) + 1} tokens")
    copies, partial = divmod(remaining, len(block))
    ids = head + block * copies + block[:partial] + tail
    assert len(ids) == size and ids[-len(tail):] == tail
    metadata = {
        "construction": "Separately encoded complete chat prefix + repeated ASCII data block token IDs + partial block token prefix + separately encoded complete chat/instruction suffix.",
        "input_tokens": size,
        "input_ids_sha256": digest(ids),
        "hash_encoding": "SHA256 of UTF-8 compact JSON token-ID array, ensure_ascii=False, separators=(',', ':')",
        "prefix_tokens": len(head),
        "suffix_tokens": len(tail),
        "block_tokens": len(block),
        "full_block_copies": copies,
        "partial_block_tokens": partial,
        "prefix_text": PREFIX,
        "block_text": SEGMENT,
        "suffix_text": SUFFIX,
        "chat_suffix_preserved": True,
        "canonical_representation": "The saved token IDs are authoritative; re-encoding decoded text can change segment-boundary tokenization.",
    }
    return ids, metadata


def capacity_check(info, sizes, budget):
    ctx = info.get("ctx")
    if not isinstance(ctx, int) or ctx <= 0:
        raise ValueError("INFO did not report a valid positive ctx; refusing generation")
    # This engine currently reports the same ctx/slot_ctx. Honor the smaller
    # positive limit should a compatible implementation expose different ones.
    slot_ctx = info.get("slot_ctx")
    limit = min(ctx, slot_ctx) if isinstance(slot_ctx, int) and slot_ctx > 0 else ctx
    violations = [size for size in sizes if size + budget > limit]
    if violations:
        raise ValueError(f"Input+output exceeds INFO capacity {limit}: sizes={violations}, budget={budget}; no GEN sent")
    return {"ctx": ctx, "slot_ctx": slot_ctx, "effective_limit": limit, "output_budget": budget, "all_sizes_fit": True, "remaining_tokens": {str(size): limit - size - budget for size in sizes}}


def summary_stats(record):
    done = record.get("done") or {}
    pre_ms, dec_ms = done.get("prefill_ms", 0), done.get("decode_ms", 0)
    cached = done.get("cached_tokens")
    processed = None if cached is None else record["prompt_tokens"] - cached
    return {
        "emitted_tokens": len(record["tokens"]),
        "finish_reason": done.get("reason"),
        "actual_drafter": done.get("actual_drafter"),
        "cold_input_confirmed": cached == 0,
        "cached_tokens": cached,
        "processed_prompt_tokens": processed,
        "prefill_tokens_per_s": None if processed is None or pre_ms <= 0 else processed * 1000 / pre_ms,
        "decode_tokens_per_s_engine_convention": None if dec_ms <= 0 else len(record["tokens"]) * 1000 / dec_ms,
        "ttft_s": record["ttft_s"],
        "request_elapsed_s": record["elapsed_s"],
        "speculative_proposals_observed": (done.get("proposed") or 0) > 0,
        "note": "Throughput follows raw D timing fields; decode rate uses emitted token count as in engine logs. No answer-quality score.",
    }


def log_start(path):
    return path.stat().st_size if path and path.exists() else None


def log_window(path, offset, req):
    if path is None or offset is None:
        return None
    with path.open("rb") as handle:
        handle.seek(offset)
        lines = handle.read().decode("utf8", "replace").splitlines()
    counts = [tuple(map(int, found)) for line in lines for found in re.findall(r"rounds=(\d+) \(ngram=(\d+) mtp=(\d+)\)", line)]
    return {
        "path": str(path), "start_byte": offset,
        "selected_lines": [line for line in lines if "chain:" in line or f"req {req} " in line],
        "chain_rounds": [{"total": total, "ngram": ngram, "mtp": mtp} for total, ngram, mtp in counts],
        "attribution": "Time-window attribution requires an isolated engine and no unrelated requests; absent counters are unknown, not zero.",
    }


def save_json(path, data):
    temporary = path.with_suffix(".writing.json")
    temporary.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf8")
    for attempt in range(30):
        try:
            os.replace(temporary, path)
            break
        except PermissionError:
            if attempt == 29:
                raise
            time.sleep(min(0.05 * (attempt + 1), 0.5))


def execute(args, tk, planned):
    result_path = result_directory() / (args.label + ".json")
    inputs_path = result_path.with_suffix(".inputs")
    if result_path.exists() or inputs_path.exists():
        raise FileExistsError(f"Refusing to overwrite existing evidence label: {args.label}")
    result_path.parent.mkdir(parents=True, exist_ok=True)
    data = {
        "schema": 1, "label": args.label, "identity": args.identity,
        "gamma_declared": args.gamma, "gamma_verified_by_protocol": False,
        "started_utc": now(), "status": "running", "sampling": "greedy",
        "sizes": args.sizes, "modes": args.modes, "output_budget": args.tokens,
        "eos_enabled": not args.ignore_eos,
        "inputs": [], "records": [], "comparisons": [],
        "notes": ["Synthetic repeated filler; no arithmetic or answer-quality score.", "New connection per request. Record cached_tokens rather than assuming a cold start.", "Completion status describes harness execution, not all checks passing.", "No per-request gamma exists; gamma declaration must match the externally configured process."],
    }
    log_path = Path(args.engine_log) if args.engine_log else None
    try:
        # INFO capacity is checked for ALL requested sizes before the first GEN.
        with Wire(args.host, args.port, args.timeout) as connection:
            data["info"] = parse_info(connection.control("INFO"))
            data["capacity_check"] = capacity_check(data["info"], args.sizes, args.tokens)
            data["ping_ok"] = connection.control("PING") == "PONG"
            if not data["ping_ok"]:
                raise ValueError("PING response invalid; refusing generation")
            if any(mode in (1, 4) for mode in args.modes) and data["info"].get("mtp") != 1:
                raise ValueError("INFO reports no MTP; requested speculative modes would be unsupported or fallback")
        inputs_path.mkdir()
        save_json(result_path, data)
        eos = [] if args.ignore_eos else tk.encode("<|im_end|>")
        for size, metadata in planned:
            ids, current = exact_input(tk, size)
            if metadata["input_ids_sha256"] != current["input_ids_sha256"]:
                raise ValueError("Local tokenizer construction changed since planning")
            input_path = inputs_path / f"input-{size}.json"
            with input_path.open("x", encoding="utf8") as handle:
                json.dump({**metadata, "token_ids": ids}, handle, ensure_ascii=False, separators=(",", ":"))
            data["inputs"].append({**metadata, "input_file": str(input_path), "input_file_sha256": hashlib.sha256(input_path.read_bytes()).hexdigest()})
            save_json(result_path, data)
            size_records = []
            for mode in args.modes:
                print(json.dumps({"event": "request_start", "input_tokens": size, "mode": mode, "output_budget": args.tokens, "input_ids_sha256": metadata["input_ids_sha256"]}), flush=True)
                offset = log_start(log_path)
                with Wire(args.host, args.port, args.timeout) as connection:
                    record = connection.generate(ids, mode, args.tokens, eos=eos)
                record["input_file"] = str(input_path)
                record["text"] = tk.decode(record["tokens"])
                record["input_hash_verified"] = record["prompt_ids_sha256"] == metadata["input_ids_sha256"]
                record["summary"] = summary_stats(record)
                record["engine_log_window"] = log_window(log_path, offset, record["req"])
                size_records.append(record)
                data["records"].append(record)
                save_json(result_path, data)
                print(json.dumps({"event": "request_complete", "input_tokens": size, "mode": mode, **record["summary"]}, ensure_ascii=False), flush=True)
                if not record["done"] or record["done"].get("reason") not in ("done", "length"):
                    raise RuntimeError(f"Generation did not finish normally: {record['done']}")
            reference = next((record for record in size_records if record["mode"] == 0), size_records[0])
            for other in size_records:
                if other is reference:
                    continue
                comparison = {
                    "input_tokens": size, "modes": [reference["mode"], other["mode"]],
                    "input_ids_sha256": metadata["input_ids_sha256"],
                    "reference_is_serial": reference["mode"] == 0,
                    **compare(reference, other),
                    "oracle": "Exact greedy token consistency is a regression target; this does not score answer correctness. EOS and length are both legitimate finishes.",
                }
                data["comparisons"].append(comparison)
                print(json.dumps(comparison, ensure_ascii=False), flush=True)
            save_json(result_path, data)
        data["status"] = "complete"
    except BaseException as error:
        data["status"] = "interrupted" if isinstance(error, KeyboardInterrupt) else "error"
        data["error"] = f"{type(error).__name__}: {error}"
        raise
    finally:
        data["finished_utc"] = now()
        save_json(result_path, data)


def selftest(tk):
    tail = tk.encode(SUFFIX)
    head = tk.encode(PREFIX)
    for size in DEFAULT_SIZES:
        ids, metadata = exact_input(tk, size)
        assert len(ids) == size
        assert ids[:len(head)] == head and ids[-len(tail):] == tail
        assert metadata["input_ids_sha256"] == digest(ids)
        assert metadata["input_ids_sha256"] == exact_input(tk, size)[1]["input_ids_sha256"]
        assert "\ufffd" not in tk.decode(ids), "Filler truncation caused an invalid Unicode sequence"
    good = capacity_check({"ctx": 262144, "slot_ctx": 262144}, DEFAULT_SIZES, 128)
    assert good["remaining_tokens"]["260000"] == 2016
    assert capacity_check({"ctx": 262144, "slot_ctx": 8192}, [8000], 128)["effective_limit"] == 8192
    for info, sizes, budget in [({}, [2050], 64), ({"ctx": 8192}, [8192], 1), ({"ctx": 262144}, [262017], 128)]:
        try:
            capacity_check(info, sizes, budget)
        except ValueError:
            pass
        else:
            raise AssertionError("Capacity failure was not rejected")
    print(json.dumps({"selftest": "passed", "sizes": DEFAULT_SIZES, "exact_lengths_and_hashes": True, "complete_chat_suffix": True, "unicode_boundaries": True, "capacity_rejections": True, "no_socket_opened": True}, ensure_ascii=False))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--run", action="store_true", help="Explicitly contact the externally started test engine")
    action.add_argument("--dry-run", "--plan", action="store_true", help="Offline plan (the default)")
    action.add_argument("--selftest", action="store_true", help="Local tokenizer/capacity tests only; no socket")
    parser.add_argument("--sizes", type=int, nargs="+", default=DEFAULT_SIZES)
    parser.add_argument("--modes", type=int, nargs="+", choices=(0, 1, 4), default=[0, 1, 4])
    parser.add_argument("--tokens", type=int, choices=(64, 128, 512), default=64)
    parser.add_argument("--label")
    parser.add_argument("--identity", default="", help="Build, weights, precision flags and process configuration evidence")
    parser.add_argument("--gamma", type=int, choices=range(1, 9), help="Externally configured gamma; metadata only")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18870)
    parser.add_argument("--timeout", type=float, default=1800)
    parser.add_argument("--tokenizer", type=Path, default=TOKENIZER)
    parser.add_argument("--engine-log")
    parser.add_argument("--ignore-eos", action="store_true", help="Force fixed budget even across EOS; optional diagnostic, not natural chat semantics")
    args = parser.parse_args()
    if len(set(args.sizes)) != len(args.sizes) or len(set(args.modes)) != len(args.modes):
        parser.error("Sizes and modes must not contain duplicates")
    if any(size < 1 for size in args.sizes) or args.timeout <= 0:
        parser.error("Sizes and timeout must be positive")
    if args.run and (not args.label or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", args.label) or not args.identity or args.gamma is None):
        parser.error("--run requires a simple --label, nonempty --identity and declared --gamma")
    try:
        tk = tokenizer_for(args.tokenizer)
    except (ValueError, FileNotFoundError) as error:
        parser.error(str(error))
    if args.selftest:
        selftest(tk)
        return
    planned = [(size, exact_input(tk, size)[1]) for size in args.sizes]
    if not args.run:
        print(json.dumps({"offline_plan": True, "no_socket_opened": True, "capacity_not_yet_verified": "A live run first checks INFO.ctx for ALL sizes before any GEN.", "modes": args.modes, "output_budget": args.tokens, "prompts": [{key: value for key, value in metadata.items() if not key.endswith("_text")} for _, metadata in planned]}, ensure_ascii=False, indent=2))
        return
    execute(args, tk, planned)


if __name__ == "__main__":
    main()
