"""Native launcher GDEC_* configuration regression; no GPU/model is loaded."""
import argparse
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--launcher', required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='gdec-runtime-config-') as temporary:
        root = Path(temporary)
        for directory in ('build/rocblas/library', 'build/hipblaslt/library', 'models/tokenizer'):
            (root / directory).mkdir(parents=True)
        for name in ('build/gdec-win.exe', 'build/gdec-api-win.exe', 'models/test.hgn',
                     'models/tokenizer/tokenizer.json'):
            (root / name).touch()
        shutil.copy2(args.launcher, root / 'start_win.exe')
        engine_port, api_port = free_port(), free_port()
        while api_port == engine_port:
            api_port = free_port()
        base = (f'MODEL_DIR="{root.as_posix()}/models"\n'
                'MODEL_FILE="$MODEL_DIR/test.hgn"\n'
                'TOKENIZER_DIR="$MODEL_DIR/tokenizer"\n'
                'MTP_FILE=""\nVISION_FILE=""\nOVERLAY_FILE=""\n'
                f'ENGINE_PORT="{engine_port}"\nAPI_PORT="{api_port}"\n')
        cases = [
            ('configured', 'GDEC_DRAFTER="mtp"\nGDEC_MTP_INGEST_FUSED="1"\n'
             'GDEC_IPROJ_SOLUTION="-711"\n', {},
             {'GDEC_DRAFTER': 'mtp', 'GDEC_MTP_INGEST_FUSED': '1', 'GDEC_IPROJ_SOLUTION': '-711'}),
            ('environment-precedence', 'GDEC_DRAFTER="mtp"\n', {'GDEC_DRAFTER': 'serial'},
             {'GDEC_DRAFTER': 'serial'}),
            ('explicit-default-override', 'GDEC_QSA_WMMA="0"\n', {}, {'GDEC_QSA_WMMA': '0'}),
            ('standard-policy-final', 'GDEC_SPEC_GAMMA="8"\nMTP_GAMMA="2"\n', {},
             {'GDEC_SPEC_GAMMA': '2'}),
            ('no-runtime-config', '', {}, {'GDEC_QSA_WMMA': '1'}),
        ]
        env = {k: v for k, v in os.environ.items() if not k.startswith('GDEC_')}
        for key in ('MODEL_DIR', 'MODEL_FILE', 'NGRAM_FILE', 'MTP_FILE', 'VISION_FILE',
                    'OVERLAY_FILE', 'TOKENIZER_DIR', 'ENGINE_PORT', 'API_PORT', 'MTP_GAMMA'):
            env.pop(key, None)
        for name, config, override, expected in cases:
            (root / 'service.conf').write_text(base + config, encoding='utf-8')
            result = subprocess.run([str(root / 'start_win.exe'), '--check'], cwd=root,
                                    env=env | override, capture_output=True, text=True,
                                    encoding='utf-8', errors='replace', timeout=20,
                                    creationflags=subprocess.CREATE_NO_WINDOW)
            assert result.returncode == 0, (name, result.stdout, result.stderr)
            actual = dict(line[8:].split('=', 1) for line in result.stdout.splitlines()
                          if line.startswith('runtime '))
            assert all(actual.get(k) == v for k, v in expected.items()), (name, actual)
            assert not (root / 'logs').exists(), 'Dry run must not start children or create logs'
            print('PASS', name)


if __name__ == '__main__':
    main()
