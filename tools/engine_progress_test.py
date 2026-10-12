"""Run engine_progress_test.cpp against progress-only first/next-token stalls."""
import argparse
import socket
import subprocess
import threading
import time

parser = argparse.ArgumentParser()
parser.add_argument('--probe', required=True)
args = parser.parse_args()
for mode in (0, 1):
    with socket.socket() as server:
        server.bind(('127.0.0.1', 0))
        server.listen()
        def serve():
            try:
                with server.accept()[0] as conn:
                    with conn.makefile('rb') as reader:
                        assert b'PROGRESS' in reader.readline()
                        if mode:
                            conn.sendall(b'T 7 1\n')
                        for _ in range(40):
                            conn.sendall(b'P 7 0 0 1 8 0 100\n')
                            time.sleep(.03)
            except OSError:
                pass
        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        subprocess.run([args.probe, f'127.0.0.1:{server.getsockname()[1]}', str(mode)], check=True, timeout=3)
        thread.join(timeout=2)
