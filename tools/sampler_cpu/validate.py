#!/usr/bin/env python3
"""CPU-only sampler regressions using verbatim engine source excerpts.

Default execution prints a plan. --run requires an unused --outdir and a C++17
compiler, detected from PATH or selected by --compiler. No HIP/GPU code runs.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
DEFAULT_SOURCE = HERE.parents[1]


def digest(text):
    return hashlib.sha256(text.encode("utf8")).hexdigest()


def extract(source, relative, start, end, after=""):
    path = source / relative
    text = path.read_text(encoding="utf8")
    minimum = text.index(after) if after else 0
    begin = text.index(start, minimum)
    begin = text.rfind("\n", 0, begin) + 1
    finish = text.index(end, begin) + len(end)
    value = text[begin:finish] + "\n"
    return value, {"source": relative, "line_start": text[:begin].count("\n") + 1, "line_end": text[:finish].count("\n") + 1, "sha256": digest(value)}


def compiler_path(explicit):
    if explicit:
        found = shutil.which(explicit)
        if found:
            return str(Path(found).resolve())
        supplied = Path(explicit)
        if supplied.is_file():
            return str(supplied.resolve())
        raise ValueError(f"Compiler not found: {explicit}")
    for name in ("clang++", "g++", "clang-cl", "cl"):
        found = shutil.which(name)
        if found:
            return str(Path(found).resolve())
    raise ValueError("No C++17 compiler on PATH; select one with --compiler")


def compile_command(compiler, output, executable):
    template = HERE / "cpu_test.cpp"
    if Path(compiler).stem.lower() in ("cl", "clang-cl"):
        return [compiler, "/nologo", "/std:c++17", "/O2", "/EHsc", "/fp:strict",
                "/I" + str(output), str(template), "/Fe:" + str(executable),
                "/Fo:" + str(output / "cpu_test.obj")]
    return [compiler, "-std=c++17", "-O2", "-fno-fast-math", "-ffp-contract=off",
            "-I", str(output), str(template), "-o", str(executable)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE,
                        help="Engine repository root (default: this tool's repository)")
    parser.add_argument("--outdir", type=Path,
                        help="New or empty directory for generated sources, executable and evidence")
    parser.add_argument("--compiler", help="C++17 compiler path/name; default: detect on PATH")
    parser.add_argument("--run", action="store_true", help="Compile and run CPU tests; otherwise print a plan")
    parser.add_argument("--suite", choices=("all", "filters", "conditional"), default="all")
    parser.add_argument("--trials", type=int, default=10000)
    args = parser.parse_args()
    if args.trials < 10000:
        parser.error("Use at least 10000 trials for the documented statistical thresholds")
    source = args.source.resolve()
    host, host_meta = extract(source, "src/gpu/parts/51_host_cfg.inc", "struct HostSampler {", "\n};")
    baseline_meta = json.loads((HERE / "fixtures/baseline.json").read_text(encoding="utf8"))
    baseline_text = (HERE / "fixtures/host_sampler_before_alignment.inc").read_text(encoding="utf8")
    baseline_begin = baseline_text.index("struct HostSampler {")
    baseline_excerpt = baseline_text[baseline_begin:]
    if digest(baseline_excerpt) != baseline_meta["excerpt_sha256"]:
        parser.error("Regression-only baseline fixture hash changed")
    if not args.run:
        print(json.dumps({"plan_only": True, "gpu": False, "current_host_sampler": host_meta,
                          "baseline": baseline_meta, "suite": args.suite, "trials": args.trials,
                          "scope": "Verbatim HostSampler and pure/chain MTP selection excerpts; synthetic CPU logits. No GPU model/state validation."}, indent=2))
        return
    if args.outdir is None:
        parser.error("--run requires --outdir")
    output = args.outdir.resolve()
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        parser.error("--outdir must be new or empty; existing evidence is never overwritten")
    compiler = compiler_path(args.compiler)
    output.mkdir(parents=True, exist_ok=True)
    metadata = {
        "scope": "CPU-only verbatim excerpts; no GPU kernels, real logits, KV/rollback, or complete serving loop",
        "suite": args.suite, "trials": args.trials, "compiler": compiler,
        "excerpts": {"host": host_meta, "baseline": baseline_meta},
        "test_template_sha256": hashlib.sha256((HERE / "cpu_test.cpp").read_bytes()).hexdigest(),
    }
    (output / "current.inc").write_text(host, encoding="utf8")
    (output / "baseline.inc").write_text(baseline_text, encoding="utf8")
    for name, marker in (("pure", "std::vector<int> spec_loop_sample("),
                         ("chain", "std::vector<int> spec_loop_chain_sample(")):
        block, meta = extract(source, "src/gpu/parts/40_model.inc", "Sampler target_view = sampler;",
                              "sampler.rng = target_view.rng;", after=marker)
        (output / f"{name}.inc").write_text(block, encoding="utf8")
        metadata["excerpts"][name] = meta
    order, order_meta = extract(source, "src/gpu/parts/40_model.inc",
                                 "std::sort(ord.begin(), ord.end(),", "      });",
                                 after="bool sms_topk_rows(")
    (output / "gpu-order.inc").write_text(order, encoding="utf8")
    metadata["excerpts"]["gpu_final_order"] = order_meta
    executable = output / ("sampler_cpu_test.exe" if sys.platform == "win32" else "sampler_cpu_test")
    command = compile_command(compiler, output, executable)
    metadata["compile_command"] = command
    metadata_path = output / "metadata.json"
    metadata_path.write_text(json.dumps(metadata, indent=2), encoding="utf8")
    compilation = subprocess.run(command, cwd=output, text=True, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, encoding="utf8", errors="replace")
    (output / "compile.log").write_text(compilation.stdout, encoding="utf8")
    metadata["compile_exit_code"] = compilation.returncode
    if compilation.returncode:
        metadata_path.write_text(json.dumps(metadata, indent=2), encoding="utf8")
        print(compilation.stdout)
        raise SystemExit(compilation.returncode)
    completed = subprocess.run([str(executable), str(args.trials), args.suite], cwd=output,
                               text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               encoding="utf8", errors="replace")
    result_path = output / "results.jsonl"
    result_path.write_text(completed.stdout, encoding="utf8")
    metadata["exit_code"] = completed.returncode
    metadata["exe_sha256"] = hashlib.sha256(executable.read_bytes()).hexdigest()
    metadata_path.write_text(json.dumps(metadata, indent=2), encoding="utf8")
    rows = [json.loads(line) for line in completed.stdout.splitlines() if line.startswith("{")]
    summary = next((row for row in reversed(rows) if row.get("summary")), None)
    print(json.dumps({"result_file": str(result_path), "metadata_file": str(metadata_path),
                      "summary": summary, "exit_code": completed.returncode}, indent=2))
    if completed.returncode:
        for row in rows:
            if row.get("pass") is False and row.get("implementation") != "baseline":
                print(json.dumps(row))
        if summary is None:
            print(completed.stdout)
    raise SystemExit(completed.returncode)


if __name__ == "__main__":
    main()
