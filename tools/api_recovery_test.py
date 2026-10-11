"""CPU-only late-engine/invalid-INFO recovery and Windows diagnostic CLI checks."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from api_disconnect_test import free_port, make_tokenizer, wait_for
from api_regression_test import get, request


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--api', required=True)
    parser.add_argument('--eng-cli', required=True)
    args = parser.parse_args()
    api, cli = str(Path(args.api).resolve()), str(Path(args.eng_cli).resolve())
    fake = str(Path(__file__).with_name('api_fake_engine.py'))
    env = {k: v for k, v in os.environ.items() if not k.startswith('GDEC_')}
    env.update(GDEC_API_TOKCACHE_FILE='', GDEC_REQSTAT='0')
    flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
    with tempfile.TemporaryDirectory(prefix='gdec-recovery-') as temp:
        root = Path(temp)
        make_tokenizer(root/'tokenizer')
        ep, ap = free_port(), free_port()
        while ep == ap:
            ap = free_port()
        base = f'http://127.0.0.1:{ap}'
        info_file = root/'info.txt'
        valid = 'I 1 0 131072 8 1 1 0 0 0 2 131072 0 1 262144'
        children = []
        with (root/'test.log').open('w+') as log:
            def start(command):
                child = subprocess.Popen(command, cwd=root, env=env, stdout=log,
                                         stderr=log, creationflags=flags)
                children.append(child)
                return child

            def health():
                return json.loads(get(base, '/health')[2])

            def api_up():
                try:
                    return get(base, '/health')[0] == 200
                except OSError:
                    return False

            def launch_api():
                child = start([api, '--tokenizer', str(root/'tokenizer'), '--engine',
                               f'127.0.0.1:{ep}', '--host', '127.0.0.1', '--port',
                               str(ap), '--context', '262144', '--overrides', ''])
                wait_for(api_up, 'API startup', timeout=6)
                return child

            def complete(seed=None):
                body = {'prompt': 'hi', 'temperature': 0, 'max_tokens': 4}
                if seed is not None:
                    body.update(temperature=1, seed=seed)
                return request(base, '/v1/completions', body)

            try:
                started = time.monotonic()
                api_proc = launch_api()
                assert time.monotonic()-started < 5, 'API blocked on absent engine'
                assert health()['slots'] == 1 and not health()['engine_info_ready']
                for _ in range(3):
                    assert complete()[0] == 502, 'cold requests must fail safely'
                assert api_proc.poll() is None
                if os.name == 'nt':
                    # Windows may take about two seconds to report a refusal,
                    # beyond these 0.3/1-second budgets. A timeout is then valid.
                    log.flush()
                    trace = (root/'test.log').read_text(errors='replace')
                    assert 'connection timed out' in trace or 'WSA error 10061' in trace, trace
                    assert 'WSA error 0' not in trace and 'WSA error 10035' not in trace, trace
                print('PASS absent engine: bounded startup and repeated safe 502', flush=True)

                info_file.write_text('HANG', encoding='ascii')
                start([sys.executable, fake, '--port', str(ep), '--info-file', str(info_file)])
                time.sleep(3.5)
                assert not health()['engine_info_ready']
                assert complete()[0] == 200
                print('PASS stalled INFO does not block generation/control recovery', flush=True)

                for bad in ['BROKEN 1 0 131072', 'I 1 0 -1', 'I 1 0 999999999999999999999',
                            valid.replace(' 2 131072', ' 65 131072'),
                            valid.replace(' 262144', ' 2147483648')]:
                    info_file.write_text(bad, encoding='ascii')
                    time.sleep(2.3)
                    h = health()
                    assert not h['engine_info_ready'] and h['slots'] == 1 and h['context'] == 262144, h
                print('PASS malformed INFO, integer overflow and invalid slot/pool widths', flush=True)

                # Expand while requests are queued and one client is leased.
                with ThreadPoolExecutor(max_workers=3) as pool:
                    futures = [pool.submit(complete, 616163) for _ in range(3)]
                    wait_for(lambda: health()['queued'] >= 1, 'queued requests')
                    info_file.write_text(valid, encoding='ascii')
                    wait_for(lambda: health()['engine_info_ready'], 'INFO recovery', timeout=8)
                    h = health()
                    assert (h['slots'], h['context'], h['slot_ctx'], h['kv_pool']) == (2, 131072, 131072, 262144), h
                    wait_for(lambda: health()['in_flight'] == 2, 'actual concurrent leases')
                    assert all(f.result()[0] == 200 for f in futures)
                assert complete()[0] == 200
                print('PASS late engine: two concurrent leases, queue wakeup and updated capacity', flush=True)

                result = subprocess.run([cli, '--host', f'127.0.0.1:{ep}',
                                         '--tokenizer', str(root/'tokenizer'),
                                         json.dumps({'ids': [1024], 'max_tokens': 2})],
                                        env=env, cwd=root, capture_output=True, text=True,
                                        timeout=10, creationflags=flags)
                assert result.returncode == 0, result.stderr
                assert json.loads(result.stdout)['reason'] == 'done', result.stdout
                print('PASS diagnostic CLI connects and receives generation on Windows', flush=True)

                # A restarted API must still accept legacy INFO without optional fields.
                api_proc.terminate(); api_proc.wait(timeout=10)
                info_file.write_text('I 1 0 65536', encoding='ascii')
                launch_api()
                h = health()
                assert h['engine_info_ready'] and (h['slots'], h['context'], h['kv_pool']) == (1, 65536, 0), h
                assert complete()[0] == 200
                print('PASS legacy INFO and generation', flush=True)
            except BaseException:
                log.flush(); log.seek(0); print(log.read())
                raise
            finally:
                for child in reversed(children):
                    if child.poll() is None:
                        child.terminate()
                    child.wait(timeout=10)
    print('RESULT PASS')


if __name__ == '__main__':
    main()
