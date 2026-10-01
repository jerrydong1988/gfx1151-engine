"""Read-only comparison of two MTPAUD01 diagnostic files.

Prints JSON to stdout. Exit codes: 0 = identical, 1 = valid but different,
2 = malformed/unreadable input. No model, tokenizer, network or GPU is used.
Raw bit comparisons include NaN payloads and signed zero; this is not a
numerical-tolerance test or a performance measurement.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct
import sys

MAGIC = b"MTPAUD01"
WIDTHS = {"f32le": 4, "bf16le": 2, "i32le": 4}
MAX_HEADER = 1024 * 1024
MAX_PAYLOAD = 1024 * 1024 * 1024


class AuditError(ValueError):
    pass


def integer(value, name):
    if type(value) is not int or value < 0:
        raise AuditError(f"{name} must be a nonnegative integer")
    return value


def exact(stream, size, label):
    data = stream.read(size)
    if len(data) != size:
        raise AuditError(f"truncated {label}: expected {size}, got {len(data)}")
    return data


def validate_header(header, payload_size):
    if not isinstance(header, dict) or header.get("event") not in ("ingest", "step"):
        raise AuditError("header must describe an ingest or step event")
    integer(header.get("sequence"), "sequence")
    integer(header.get("vocab"), "vocab")
    integer(header.get("P"), "P")
    tensors = header.get("tensors")
    if not isinstance(tensors, list):
        raise AuditError("tensors must be a list")
    offset, names = 0, set()
    for tensor in tensors:
        if not isinstance(tensor, dict):
            raise AuditError("tensor descriptor must be an object")
        name, dtype = tensor.get("name"), tensor.get("dtype")
        if not isinstance(name, str) or not name or name in names:
            raise AuditError("tensor names must be nonempty and unique within an event")
        names.add(name)
        if not isinstance(dtype, str) or dtype not in WIDTHS:
            raise AuditError(f"unsupported dtype: {dtype!r}")
        shape = tensor.get("shape")
        if not isinstance(shape, list) or len(shape) != 2:
            raise AuditError(f"{name}: expected two-dimensional shape")
        rows, cols = (integer(v, f"{name}.shape") for v in shape)
        count = integer(tensor.get("nbytes"), f"{name}.nbytes")
        if count != rows * cols * WIDTHS[dtype]:
            raise AuditError(f"{name}: dtype/shape/nbytes disagree")
        if integer(tensor.get("offset"), f"{name}.offset") != offset:
            raise AuditError(f"{name}: noncontiguous, overlapping or invalid offset")
        offset += count
    if offset != payload_size:
        raise AuditError("tensor lengths do not equal framed payload length")


class Reader:
    def __init__(self, path):
        self.path = Path(path)
        self.stream = self.path.open("rb")
        self.hash = hashlib.sha256()
        self.events = 0
        self.types = Counter()
        self.tensor_counts = Counter()
        self.tensor_payload_bytes = Counter()
        self.payload_bytes = 0
        self.framed_bytes = 0
        self.full_vocab_steps = 0
        self.non_full_vocab_steps = 0
        self.ingest_P_counts = Counter()
        self.position_state_mismatches = 0
        self.vocabs = set()
        try:
            magic = self.read_exact(8, "magic")
            if magic != MAGIC:
                raise AuditError("unsupported magic, expected MTPAUD01")
        except Exception:
            self.stream.close()
            raise

    def read_exact(self, n, label):
        data = exact(self.stream, n, label)
        self.hash.update(data)
        self.framed_bytes += len(data)
        return data

    def next(self):
        framing = self.stream.read(16)
        if not framing:
            return None
        if len(framing) != 16:
            raise AuditError("truncated event framing")
        self.hash.update(framing)
        self.framed_bytes += 16
        header_n, payload_n = struct.unpack("<QQ", framing)
        if not (0 < header_n <= MAX_HEADER) or payload_n > MAX_PAYLOAD:
            raise AuditError("event exceeds parser safety bounds")
        header_raw = self.read_exact(header_n, "header")
        try:
            header = json.loads(header_raw.decode("utf-8"))
        except (UnicodeError, json.JSONDecodeError) as exc:
            raise AuditError(f"invalid JSON header: {exc}") from exc
        validate_header(header, payload_n)
        if header["sequence"] != self.events:
            raise AuditError("noncontiguous event sequence")
        payload = self.read_exact(payload_n, "payload")
        self.events += 1
        self.types[header["event"]] += 1
        self.payload_bytes += payload_n
        self.vocabs.add(header.get("vocab"))
        if header.get("device_mtp_pos") != header.get("mtp_pos"):
            self.position_state_mismatches += 1
        if header["event"] == "ingest":
            self.ingest_P_counts[header.get("P")] += 1
        for tensor in header["tensors"]:
            self.tensor_counts[tensor["name"]] += 1
            self.tensor_payload_bytes[tensor["name"]] += tensor["nbytes"]
        if header["event"] == "step":
            # A step may also include diagnostic tensors such as selected_blocks.
            # Completeness describes the named logits tensor, not tensor count.
            logits = next((tensor for tensor in header["tensors"]
                           if tensor["name"] == "logits"), None)
            full = (header.get("logits_n") == header.get("vocab")
                    and logits is not None
                    and logits["dtype"] == "f32le"
                    and logits["shape"] == [1, header.get("vocab")])
            self.full_vocab_steps += int(full)
            self.non_full_vocab_steps += int(not full)
        return header, header_raw, payload

    def summary(self):
        return {"path": str(self.path.resolve()), "sha256": self.hash.hexdigest(),
                "file_bytes": self.framed_bytes, "events": self.events,
                "event_types": dict(self.types), "tensor_records": sum(self.tensor_counts.values()),
                "tensor_counts": dict(self.tensor_counts), "payload_bytes": self.payload_bytes,
                "tensor_payload_bytes": dict(self.tensor_payload_bytes),
                "full_vocab_step_events": self.full_vocab_steps,
                "non_full_vocab_step_events": self.non_full_vocab_steps,
                "vocabs": sorted(self.vocabs, key=str),
                "ingest_P_counts": dict(self.ingest_P_counts),
                "host_device_mtp_position_mismatches": self.position_state_mismatches}


def raw_diff(a, b, width):
    common = min(len(a), len(b))
    same = a == b
    if same:
        return {"overlap_bytes": common, "different_overlap_bytes": 0,
                "different_overlap_bits": 0, "different_overlap_elements": 0,
                "unpaired_bytes": 0}
    byte_count = sum(x != y for x, y in zip(a, b))
    bit_count = 0
    for start in range(0, common, 65536):
        end = min(start + 65536, common)
        bit_count += (int.from_bytes(a[start:end], "little") ^
                      int.from_bytes(b[start:end], "little")).bit_count()
    elements = sum(a[i:i + width] != b[i:i + width]
                   for i in range(0, common - common % width, width))
    return {"overlap_bytes": common, "different_overlap_bytes": byte_count,
            "different_overlap_bits": bit_count, "different_overlap_elements": elements,
            "unpaired_bytes": abs(len(a) - len(b))}


def compare(left_path, right_path):
    left, right = Reader(left_path), None
    try:
        right = Reader(right_path)
        stats = {}
        headers_bad = frames_bad = missing_events = 0
        first = None
        while True:
            a, b = left.next(), right.next()
            if a is None and b is None:
                break
            if a is None or b is None:
                missing_events += 1
                first = first or {"sequence": (a or b)[0]["sequence"], "reason": "unpaired event"}
                continue
            ah, ar, ap = a
            bh, br, bp = b
            if ah != bh:
                headers_bad += 1
            if ar != br:
                frames_bad += 1
            if ar != br or ap != bp:
                first = first or {"sequence": ah["sequence"], "reason": "header or payload differs"}
            at = {t["name"]: t for t in ah["tensors"]}
            bt = {t["name"]: t for t in bh["tensors"]}
            for name in sorted(at.keys() | bt.keys()):
                entry = stats.setdefault(name, Counter())
                entry["event_pairs"] += 1
                if name not in at or name not in bt:
                    entry["missing_tensor_records"] += 1
                    continue
                x, y = at[name], bt[name]
                av = ap[x["offset"]:x["offset"] + x["nbytes"]]
                bv = bp[y["offset"]:y["offset"] + y["nbytes"]]
                entry["descriptor_mismatches"] += int(x != y)
                width = WIDTHS[x["dtype"]]
                if x["dtype"] != y["dtype"]:
                    entry["dtype_mismatches"] += 1
                    width = 1  # byte elements when dtypes cannot be paired
                entry.update(raw_diff(av, bv, width))
        ls, rs = left.summary(), right.summary()
        return {"format": "MTPAUD01", "read_only": True,
                "all_bytes_equal": first is None and ls["sha256"] == rs["sha256"],
                "left": ls, "right": rs, "header_object_mismatches": headers_bad,
                "header_raw_byte_mismatches": frames_bad, "unpaired_events": missing_events,
                "tensor_raw_differences": {k: dict(v) for k, v in stats.items()},
                "first_mismatch": first,
                "scope": "Only recorded event metadata and tensor bytes are compared; not unrecorded model state, performance, or an arbitrary-input equivalence proof."}
    finally:
        left.stream.close()
        if right is not None:
            right.stream.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("left", type=Path)
    parser.add_argument("right", type=Path)
    args = parser.parse_args()
    try:
        result = compare(args.left, args.right)
    except (OSError, AuditError, OverflowError) as exc:
        print(json.dumps({"error": str(exc), "valid": False, "read_only": True}))
        return 2
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result["all_bytes_equal"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
