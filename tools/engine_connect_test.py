"""Compile a host-only connection probe and check timeout/error preservation."""
import argparse
import os
from pathlib import Path
import socket
import subprocess
import tempfile

SOURCE = r'''
#include "engine_client.h"
#ifdef _WIN32
#include "os_win32.h"
#endif
#include <chrono>
#include <cstdio>
#include <cstdlib>
int main(int argc, char** argv) {
#ifdef _WIN32
    if (!wsa_init()) return 2;
#endif
    if (argc != 3) return 2;
    gdec::EngineClient client;
    std::string error;
    const auto start = std::chrono::steady_clock::now();
    const bool ok = client.connect(argv[1], &error, std::atof(argv[2]));
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    std::printf("%.6f\n%s\n", seconds, error.c_str());
    return ok ? 0 : 1;
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cxx', required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='gdec-connect-') as directory:
        temp = Path(directory)
        source = temp/'probe.cpp'
        source.write_text(SOURCE)
        binary = temp/('probe.exe' if os.name == 'nt' else 'probe')
        command = [args.cxx, '-std=c++17', '-O2', '-D_CRT_SECURE_NO_WARNINGS',
                   '-I'+str(root/'src/api'), '-I'+str(root/'src/gpu'),
                   str(source), str(root/'src/api/engine_client.cpp'), '-o', str(binary)]
        if os.name == 'nt':
            command.append('-lws2_32')
        subprocess.run(command, check=True)

        def probe(port, timeout):
            result = subprocess.run([str(binary), f'127.0.0.1:{port}', str(timeout)],
                                    capture_output=True, text=True, timeout=timeout+3)
            assert result.returncode in (0, 1), result
            lines = result.stdout.splitlines()
            elapsed = float(lines[0])
            assert elapsed < timeout+1, (elapsed, result.stdout)
            return result.returncode, elapsed, '\n'.join(lines[1:])

        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            port = listener.getsockname()[1]
            listener.listen()
            assert probe(port, 1)[0] == 0
            accepted, _ = listener.accept()
            accepted.close()
        print('PASS bounded connection succeeds', flush=True)

        code, elapsed, error = probe(port, .15)
        assert code == 1 and error and 'WSA error 0' not in error and 'WSA error 10035' not in error, error
        # Immediate refusal is also valid on operating systems without TCP retry delay.
        print(f'PASS short budget fails safely in {elapsed:.3f}s: {error}', flush=True)
        code, elapsed, error = probe(port, 5)
        assert code == 1 and 'timed out' not in error, (elapsed, error)
        if os.name == 'nt':
            assert 'WSA error 10061' in error, error
        print(f'PASS refused connection preserves actual error in {elapsed:.3f}s: {error}', flush=True)
    print('RESULT PASS')


if __name__ == '__main__':
    main()
