#!/usr/bin/env python3
"""Fault injection: a missing engine cancel acknowledgement must not pin a slot.

Runs only owned CPU children with a synthetic tokenizer. No GPU/model required.
The pre-fix API fails the 14-second bound; the fixed API reconnects after 10 s.
"""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
from api_disconnect_test import free_port, make_tokenizer, wait_for
from api_regression_test import get, request


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--api', required=True)
    a = p.parse_args()
    ep, ap = free_port(), free_port()
    while ep == ap:
        ap = free_port()
    base = f'http://127.0.0.1:{ap}'
    children = []
    flags = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
    env = dict(os.environ, GDEC_API_TOKCACHE_FILE='', GDEC_API_TOKCACHE='0',
               GDEC_API_ADMIN_KEY='', GDEC_REQSTAT='0')
    with tempfile.TemporaryDirectory(prefix='cancel-timeout-') as temporary:
        root = Path(temporary)
        make_tokenizer(root/'tokenizer')
        with (root/'children.log').open('w+') as log:
            try:
                children.append(subprocess.Popen([sys.executable, str(Path(__file__).with_name('api_fake_engine.py')),
                    '--port', str(ep)], stdout=log, stderr=log, env=env, creationflags=flags))
                def state():
                    with socket.create_connection(('127.0.0.1', ep), timeout=1) as s:
                        s.sendall(b'TESTSTATE\n')
                        with s.makefile('rb') as f:
                            return json.loads(f.readline())
                def ready():
                    try:
                        state()
                        return True
                    except OSError:
                        return False
                wait_for(ready, 'fake ready')
                children.append(subprocess.Popen([str(Path(a.api).resolve()), '--tokenizer', str(root/'tokenizer'),
                    '--engine', f'127.0.0.1:{ep}', '--host', '127.0.0.1', '--port', str(ap), '--overrides', ''],
                    cwd=root, stdout=log, stderr=log, env=env, creationflags=flags))
                def health():
                    try:
                        return json.loads(get(base, '/health')[2])
                    except OSError:
                        return {}
                wait_for(lambda: bool(health()), 'API ready', timeout=10)
                payload = json.dumps({'messages':[{'role':'user','content':'hi'}], 'stream':True,
                    'temperature':1, 'seed':616164, 'max_tokens':128, 'enable_thinking':False}).encode()
                with socket.create_connection(('127.0.0.1', ap), timeout=2) as s:
                    s.sendall((f'POST /v1/chat/completions HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n'
                        f'Content-Length: {len(payload)}\r\nConnection: close\r\n\r\n').encode()+payload)
                    wait_for(lambda: len(state()) == 1, 'stalled generation started')
                started = time.monotonic()
                wait_for(lambda: state()[0]['cancel'] == 1, 'cancel sent', timeout=3)
                wait_for(lambda: health().get('in_flight') == 0, 'slot released without D-ack', timeout=14)
                elapsed = time.monotonic()-started
                assert 9 <= elapsed < 14, elapsed
                assert state()[0]['cancel'] == 1 and not state()[0]['done']
                code, _, raw = request(base, '/v1/chat/completions', {
                    'messages':[{'role':'user','content':'hi'}], 'temperature':0,
                    'enable_thinking':False, 'max_tokens':4})
                assert code == 200 and json.loads(raw)['choices'][0]['message']['content'] == 'Hi', raw
                print(f'PASS missing D-ack: released in {elapsed:.3f}s; next request completed on recovered connection')
            except BaseException:
                log.flush(); log.seek(0); print(log.read(), file=sys.stderr)
                raise
            finally:
                for child in reversed(children):
                    if child.poll() is None:
                        child.terminate()
                    child.wait(timeout=5)


if __name__ == '__main__':
    main()
