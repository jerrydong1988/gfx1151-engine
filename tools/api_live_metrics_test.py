"""CPU integration: progress before completion, concurrency, legacy and cleanup."""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import queue
import socket
import socketserver
import subprocess
import tempfile
import threading
import time
from api_disconnect_test import free_port, make_tokenizer, wait_for
from api_regression_test import get, request

incoming = queue.Queue()


class Engine(socketserver.StreamRequestHandler):
    def handle(self):
        try:
            self.serve()
        except (ConnectionError, OSError):
            pass  # API shutdown closes owned idle control connections.

    def serve(self):
        while raw := self.rfile.readline():
            fields = raw.decode().split()
            if fields[0] == 'INFO':
                self.wfile.write(b'I 1 0 262144 8 1 1 0 0 0 2 262144 0 1 524288\n')
            elif fields[0] == 'PING':
                self.wfile.write(b'PONG\n')
            elif fields[0] == 'GEN':
                assert 'PROGRESS' in fields
                req = int(fields[1])
                at = 4 + int(fields[3])
                n = int(fields[at])
                commands = queue.Queue()
                incoming.put((req, n, commands))
                while True:
                    line = commands.get(timeout=15)
                    if line == 'DROP':
                        return
                    self.wfile.write((line.format(req=req) + '\n').encode())
                    if line.startswith('D '):
                        break


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--api', required=True)
    args = parser.parse_args()
    with Server(('127.0.0.1', 0), Engine) as engine, tempfile.TemporaryDirectory() as tmp:
        threading.Thread(target=engine.serve_forever, daemon=True).start()
        root = Path(tmp)
        make_tokenizer(root/'tokenizer')
        port = free_port()
        base = f'http://127.0.0.1:{port}'
        env = {k: v for k, v in os.environ.items() if not k.startswith('GDEC_')}
        env.update(GDEC_API_TOKCACHE_FILE='', GDEC_API_TOKCACHE='0')
        with (root/'api.log').open('w+') as log:
            child = subprocess.Popen([str(Path(args.api).resolve()), '--tokenizer', str(root/'tokenizer'),
                '--engine', f'127.0.0.1:{engine.server_address[1]}', '--host', '127.0.0.1',
                '--port', str(port), '--overrides', ''], cwd=root, env=env,
                stdout=log, stderr=log, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            def read(path):
                return json.loads(get(base, path)[2])
            def ready():
                try:
                    return read('/health')['engine_info_ready']
                except (OSError, KeyError):
                    return False
            def live():
                status, headers, body = get(base, '/metrics/live')
                assert status == 200
                return json.loads(body)
            def generate():
                return request(base, '/v1/completions', {'prompt': 'abcdefghijkl',
                    'temperature': 0, 'max_tokens': 8})
            try:
                wait_for(ready, 'API', timeout=10)
                assert live()['active'] == []
                with concurrent.futures.ThreadPoolExecutor(2) as workers:
                    a, b = workers.submit(generate), workers.submit(generate)
                    ra, na, qa = incoming.get(timeout=3)
                    rb, nb, qb = incoming.get(timeout=3)
                    assert na == nb == 12
                    wait_for(lambda: len(live()['active']) == 2, 'two active')
                    qa.put('P {req} 0 0 4 8 4 1000')
                    qb.put('P {req} 1 8 8 8 4 2000')
                    wait_for(lambda: live()['last_prefill'] is not None, 'prefill done')
                    state = live()
                    assert state['prefill_completed_tokens_total'] == 8, state
                    assert state['prefill_estimated_tokens_total'] == 12, state
                    assert state['last_prefill']['tok_per_s'] == 4, state
                    assert not a.done() and not b.done()
                    assert read('/reqstat/summary')['requests'] == 0
                    qa.put('P {req} 0 0 2 8 4 1001')  # regression: ignored
                    qa.put('P {req} 0 0 99 8 4 1001')  # invalid: ignored
                    qa.put('P 99999 0 0 8 8 4 1001')  # wrong request: ignored
                    qa.put('P {req} 1 8 8 8 4 3000')
                    for q in (qa, qb):
                        q.put('T {req} 12675')
                        q.put('T {req} 12675')
                    wait_for(lambda: live()['output_tokens_total'] == 4, 'tokens before D')
                    state = live()
                    assert all(r['phase'] == 'decode' for r in state['active']), state
                    assert state['prefill_completed_tokens_total'] == 16, state
                    assert state['prefill_estimated_tokens_total'] == 16, state
                    assert read('/reqstat/summary')['requests'] == 0
                    for q in (qa, qb):
                        q.put('D {req} done 12 2 3000 500 1 1 2 4 2')
                    assert a.result()[0] == b.result()[0] == 200
                    wait_for(lambda: live()['active'] == [], 'completion cleanup')
                    assert read('/reqstat/summary')['requests'] == 2
                    print('PASS concurrent Prefill/Decode visible before either request finishes')
                    print('PASS cached tokens excluded, invalid/regressing progress ignored, final totals exact')
                    for mode in ('legacy', 'cancel', 'error', 'drop'):
                        future = workers.submit(generate)
                        req, n, q = incoming.get(timeout=3)
                        if mode == 'legacy':
                            q.put('T {req} 12675')
                            wait_for(lambda: any(r['output_tokens'] for r in live()['active']), 'legacy token')
                            assert live()['active'][0]['prefill_reported'] is False
                            q.put('D {req} done 12 1 100 0 0 0 0 0 0')
                        elif mode == 'drop':
                            q.put('DROP')
                        else:
                            q.put(f'D {{req}} {mode} 0 0 0 0')
                        result = future.result(timeout=5)
                        assert result[0] == {'legacy': 200, 'cancel': 200, 'error': 400, 'drop': 502}[mode], result
                        wait_for(lambda: not live()['active'], mode+' cleanup')
                        print('PASS '+mode+' cleanup and fallback')
                print('RESULT PASS')
            except BaseException:
                log.flush(); log.seek(0); print(log.read()); raise
            finally:
                child.terminate(); child.wait(timeout=10); engine.shutdown()


if __name__ == '__main__':
    main()
