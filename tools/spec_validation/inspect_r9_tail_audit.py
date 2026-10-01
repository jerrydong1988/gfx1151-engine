"""Read-only full-vs-tail MTP audit, standard-library-only, no GPU.

The proposal q is intentionally different. Only retained prompt ingest tensors
must be byte-identical; decode proposal logits are checked for shape/finiteness.
"""
from __future__ import annotations
import argparse
from array import array
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import struct
import sys

WIDTHS = {"f32le": 4, "bf16le": 2, "i32le": 4}
PROMPT_NAMES = ("K", "V", "pooled_keys", "raw_ring", "tokens")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    with Path(path).open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def inspect(path, is_tail, prompt_tokens, vocab):
    counts, ps = Counter(), Counter()
    retained = {}
    ranges = set()
    selected_min, selected_max = None, None
    payload_bytes = step_count = finite_logits = sequence = 0
    with Path(path).open("rb") as f:
        require(f.read(8) == b"MTPAUD01", "bad audit magic")
        while framing := f.read(16):
            require(len(framing) == 16, "truncated framing")
            hn, pn = struct.unpack("<QQ", framing)
            require(0 < hn <= 1048576 and pn <= 1073741824, "invalid framing sizes")
            head_raw = f.read(hn)
            require(len(head_raw) == hn, "truncated header")
            h = json.loads(head_raw)
            payload_offset = f.tell()
            raw = f.read(pn)
            require(len(raw) == pn, "truncated payload")
            require(h["sequence"] == sequence, "noncontiguous event sequence")
            sequence += 1
            require(h["event"] in ("ingest", "step"), "unknown event")
            require(h["mtp_pos"] == h["device_mtp_pos"], "host/device position mismatch")
            require(h["vocab"] == vocab, "unexpected vocabulary")
            require(h["P"] > 0 and h["base"] >= 0, "invalid base/P")
            tensors, cursor = {}, 0
            for t in h["tensors"]:
                require(t["name"] not in tensors, "duplicate tensor name")
                require(t["dtype"] in WIDTHS and len(t["shape"]) == 2, "invalid tensor type")
                require(all(type(v) is int and v >= 0 for v in t["shape"]), "invalid tensor shape")
                require(t["offset"] == cursor, "invalid tensor offset")
                require(t["nbytes"] == math.prod(t["shape"]) * WIDTHS[t["dtype"]], "invalid tensor size")
                tensors[t["name"]] = (t, raw[cursor:cursor + t["nbytes"]])
                cursor += t["nbytes"]
            require(cursor == pn, "tensor payload length mismatch")
            counts[h["event"]] += 1
            payload_bytes += pn
            begin = h.get("tail_begin", 0)
            end = h.get("tail_prompt_end", prompt_tokens - 1)
            if is_tail:
                require(begin > 0 and begin % 4 == 0, "tail mode was not active/aligned")
                require(end == prompt_tokens - 1 and begin < end, "tail prompt range mismatch")
                require(h["base"] >= begin, "access before initialized tail")
                ranges.add((begin, end))
                position_end = h["base"] + h["P"] if h["event"] == "ingest" else h["device_mtp_pos"] + 1
                require(h["valid_block_begin"] == begin // 4, "invalid valid-block start")
                require(h["valid_block_end"] == position_end // 4, "invalid valid-block end")
                require(h["recent_begin"] == position_end // 4 * 4, "invalid recent start")
                require(h["recent_end"] == position_end, "invalid recent end")
                require(begin <= h["recent_begin"] <= h["recent_end"], "recent rows outside initialized tail")
            else:
                require(begin == 0, "full audit unexpectedly enabled tail")
            if h["event"] == "step":
                require(h["P"] == 1 and h["base"] == h["device_mtp_pos"], "invalid step position")
                require("logits" in tensors, "step missing logits")
                t, b = tensors["logits"]
                require(t["shape"] == [1, vocab] and t["dtype"] == "f32le" and h["logits_n"] == vocab,
                        "incomplete/full-vocab logits descriptor")
                values = array("f")
                values.frombytes(b)
                if sys.byteorder != "little":
                    values.byteswap()
                require(len(values) == vocab and all(math.isfinite(x) for x in values), "nonfinite/truncated logits")
                finite_logits += len(values)
                step_count += 1
                if is_tail:
                    require("selected_blocks" in tensors, "tail step missing actual selected block dump")
                    t, b = tensors["selected_blocks"]
                    require(t["shape"] == [1, 512] and t["dtype"] == "i32le", "bad selected block tensor")
                    ids = struct.unpack("<512i", b)
                    require(all(x < y for x, y in zip(ids, ids[1:])), "selected ids not unique/increasing")
                    require(all(h["valid_block_begin"] <= i < h["valid_block_end"] for i in ids),
                            "selected block outside initialized valid range")
                    selected_min = ids[0] if selected_min is None else min(selected_min, ids[0])
                    selected_max = ids[-1] if selected_max is None else max(selected_max, ids[-1])
            else:
                require(h["mtp_pos"] == h["base"] + h["P"], "ingest position mismatch")
                require(h["block_base"] == h["base"] // 4, "ingest block base mismatch")
                require(h["block_count"] == (h["base"] + h["P"]) // 4 - h["base"] // 4,
                        "ingest pooled block count mismatch")
                ps[h["P"]] += 1
                if h["base"] < prompt_tokens - 1:
                    require(h["base"] + h["P"] <= prompt_tokens - 1, "prompt ingest crosses into decode")
                    key = (h["base"], h["P"])
                    require(key not in retained, "duplicate prompt chunk; expected one audited request")
                    require(set(tensors) == set(PROMPT_NAMES), "unexpected prompt tensor set")
                    require(tensors["K"][0]["dtype"] == tensors["V"][0]["dtype"]
                            and tensors["K"][0]["dtype"] in ("f32le", "bf16le"), "invalid KV dtype")
                    for name in ("K", "V"):
                        require(tensors[name][0]["shape"] == [h["P"], 512], "invalid KV shape")
                    for name, shape, dtype in (("pooled_keys", [h["block_count"], 128], "f32le"),
                                               ("raw_ring", [4, 128], "f32le"),
                                               ("tokens", [h["P"], 1], "i32le")):
                        require(tensors[name][0]["shape"] == shape and tensors[name][0]["dtype"] == dtype,
                                "invalid retained " + name + " tensor")
                    retained[key] = {name: {"dtype": tensors[name][0]["dtype"],
                                           "shape": tensors[name][0]["shape"],
                                           "nbytes": tensors[name][0]["nbytes"],
                                           "_file_offset": payload_offset + tensors[name][0]["offset"],
                                           "sha256": hashlib.sha256(tensors[name][1]).hexdigest()}
                                     for name in PROMPT_NAMES}
    require(sequence > 0 and step_count > 0 and retained, "audit does not cover ingest and steps")
    require(not is_tail or len(ranges) == 1, "tail range changed within request")
    return {"path": str(Path(path).resolve()), "sha256": digest(path), "file_bytes": Path(path).stat().st_size,
            "events": sequence, "event_types": dict(counts), "payload_bytes": payload_bytes,
            "full_finite_logits_events": step_count, "finite_logit_elements": finite_logits,
            "ingest_P_counts": dict(ps), "host_device_positions_equal": True,
            "tail_range": list(next(iter(ranges))) if ranges else None,
            "selected_blocks_checked": step_count * 512 if is_tail else 0,
            "selected_min": selected_min, "selected_max": selected_max}, retained


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("full", type=Path)
    p.add_argument("tail", type=Path)
    p.add_argument("--prompt-tokens", type=int, default=32768)
    p.add_argument("--vocab", type=int, default=248320)
    p.add_argument("--output", type=Path)
    args = p.parse_args()
    try:
        full, full_chunks = inspect(args.full, False, args.prompt_tokens, args.vocab)
        tail, tail_chunks = inspect(args.tail, True, args.prompt_tokens, args.vocab)
        begin, end = tail["tail_range"]
        expected = {k for k in full_chunks if begin <= k[0] < end}
        require(expected == set(tail_chunks), "retained prompt chunk coverage mismatch")
        cursor, comparisons = begin, []
        for key in sorted(expected):
            require(key[0] == cursor, "gap/overlap in retained prompt chunks")
            cursor += key[1]
            for name in PROMPT_NAMES:
                left, right = full_chunks[key][name], tail_chunks[key][name]
                public_left = {k: v for k, v in left.items() if not k.startswith("_")}
                public_right = {k: v for k, v in right.items() if not k.startswith("_")}
                require(public_left == public_right,
                        f"retained prompt tensor differs: base={key[0]} P={key[1]} {name}")
                with args.full.open("rb") as a, args.tail.open("rb") as b:
                    a.seek(left["_file_offset"])
                    b.seek(right["_file_offset"])
                    remaining = left["nbytes"]
                    while remaining:
                        count = min(remaining, 1048576)
                        av, bv = a.read(count), b.read(count)
                        require(len(av) == len(bv) == count and av == bv,
                                f"raw retained bytes differ: base={key[0]} P={key[1]} {name}")
                        remaining -= count
                comparisons.append({"base": key[0], "P": key[1], "tensor": name, **public_right,
                                    "raw_bytes_equal": True})
        require(cursor == end, "retained chunks do not cover tail prompt end")
        result = {"passed": True, "read_only": True, "full": full, "tail": tail,
                  "retained_prompt_tensor_comparisons": comparisons,
                  "all_retained_prompt_tensors_equal": True,
                  "scope": "Only the proposal q has truncated prompt context. Decode proposal logits may intentionally differ; no whole-file equivalence assertion. This audit alone does not prove target-distribution fidelity or answer correctness."}
        code = 0
    except (OSError, ValueError, KeyError, TypeError, OverflowError, struct.error) as exc:
        result = {"passed": False, "read_only": True, "error": f"{type(exc).__name__}: {exc}"}
        code = 1
    encoded = json.dumps(result, ensure_ascii=False, indent=2)
    if args.output:
        with args.output.open("x", encoding="utf8") as f:
            f.write(encoded + "\n")
    print(encoded)
    return code


if __name__ == "__main__":
    sys.exit(main())
