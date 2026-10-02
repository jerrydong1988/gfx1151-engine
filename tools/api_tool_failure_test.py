#!/usr/bin/env python3
"""CPU integration: malformed calls must not masquerade as exhausted budgets."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from api_disconnect_test import free_port, make_tokenizer, wait_for
from api_regression_test import check, get, request, parse_sse


def checks(base):
    fn = {"name": "read_file", "parameters": {"type": "object", "properties": {
        "path": {"type": "string"}, "count": {"type": "integer"},
        "action": {"type": "string", "enum": ["read", "write"]},
        "payload": {"type": "object", "properties": {"n": {"type": "integer"}}, "required": ["n"]}},
        "required": ["path"], "additionalProperties": False}}
    for responses in [False, True]:
        path = "/v1/responses" if responses else "/v1/chat/completions"
        for stream in [False, True]:
            for case in range(14):
                body = {"stream": stream, "enable_thinking": False, "seed": 717100 + case,
                        "temperature": 0.7, "max_output_tokens": 2048,
                        "tools": [{"type": "function", **fn}] if responses else
                                 [{"type": "function", "function": fn}]}
                body.update({"input": "test"} if responses else
                            {"messages": [{"role": "user", "content": "test"}]})
                status, _, raw = request(base, path, body)
                if case in (7, 8, 9, 10, 12):
                    frames = [json.loads(line[6:]) for line in raw.decode().splitlines()
                              if line.startswith('data: {')] if stream else [json.loads(raw)]
                    errors = [e.get('error') or e.get('response', {}).get('error') for e in frames]
                    check(all('secret-value' not in json.dumps(e) for e in errors if e),
                          'argument values are not repeated in diagnostics', raw)
                invalid = case in [1, 2, 3, 6, 7, 8, 9, 10, 12]
                name = f"{'responses' if responses else 'chat'}-{'sse' if stream else 'json'}-{case}"
                if not responses:
                    if invalid:
                        if status == 502:
                            value = json.loads(raw)
                            check(value["error"]["code"] == "invalid_tool_call", name, raw)
                        else:
                            check(stream and status == 200, name + "-status", raw)
                            events = parse_sse(raw)
                            check(any(isinstance(e, dict) and e.get("error", {}).get("code") ==
                                      "invalid_tool_call" for e in events), name + "-error", raw)
                            check(not any(isinstance(e, dict) and any(c.get("finish_reason") for c in
                                      e.get("choices", [])) for e in events), name + "-no-false-finish", raw)
                    else:
                        check(status == 200, name + "-status", raw)
                        values = parse_sse(raw) if stream else [json.loads(raw)]
                        finishes = [c["finish_reason"] for e in values if isinstance(e, dict)
                                    for c in e.get("choices", []) if c.get("finish_reason")]
                        check(finishes == ["tool_calls" if case in [0, 11, 13] else "length" if case == 4 else "stop"],
                              name, raw)
                else:
                    check(status == 200, name + "-status", raw)
                    if stream:
                        frames = [json.loads(line[6:]) for line in raw.decode().splitlines()
                                  if line.startswith("data: {")]
                        finals = [e for e in frames if e.get("type") in
                                  ("response.failed", "response.incomplete", "response.completed")]
                        check(len(finals) == 1, name + "-one-final", raw)
                        value = finals[0]["response"]
                    else:
                        value = json.loads(raw)
                    check(value["status"] == ("failed" if invalid else "incomplete" if case == 4 else "completed"), name, raw)
                    if invalid:
                        check(value["error"]["code"] == "invalid_tool_call" and
                              "incomplete_details" not in value, name + "-cause", raw)
                    if case == 4:
                        check(value["incomplete_details"]["reason"] == "max_output_tokens", name + "-budget", raw)
    print("RESULT PASS")


def main():
    p = argparse.ArgumentParser(); p.add_argument("--api", required=True); a = p.parse_args()
    api = str(Path(a.api).resolve()); engine = str(Path(__file__).with_name("api_fake_engine.py"))
    ep, ap = free_port(), free_port()
    while ep == ap: ap = free_port()
    base = f"http://127.0.0.1:{ap}"; children = []
    with tempfile.TemporaryDirectory(prefix="gdec-tool-failure-") as temporary:
        root = Path(temporary); make_tokenizer(root / "tokenizer")
        env = dict(os.environ, GDEC_API_TOKCACHE_FILE="", GDEC_API_ADMIN_KEY="", GDEC_API_TOKCACHE="0", GDEC_REQSTAT="0", ROPE_FACTOR="1")
        flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
        with (root / "test.log").open("w+") as log:
            try:
                children.append(subprocess.Popen([sys.executable, engine, "--port", str(ep)], stdout=log, stderr=log, env=env, creationflags=flags))
                import socket
                def ready():
                    try:
                        with socket.create_connection(("127.0.0.1", ep), timeout=.1): return True
                    except OSError: return False
                wait_for(ready, "engine")
                children.append(subprocess.Popen([api, "--tokenizer", str(root / "tokenizer"), "--engine", f"127.0.0.1:{ep}", "--port", str(ap)], stdout=log, stderr=log, env=env, creationflags=flags))
                def health():
                    try: return get(base, "/health")[0] == 200
                    except OSError: return False
                wait_for(health, "API"); checks(base)
            except BaseException:
                log.flush(); log.seek(0); print(log.read(), file=sys.stderr); raise
            finally:
                for child in reversed(children):
                    if child.poll() is None:
                        child.terminate()
                        try: child.wait(timeout=5)
                        except subprocess.TimeoutExpired: child.kill(); child.wait(timeout=5)


if __name__ == "__main__": main()
