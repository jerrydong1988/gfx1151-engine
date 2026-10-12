#!/usr/bin/env python3
"""Build the pinned, static WebP decoder for Windows (Python 3.12+, CMake, Ninja).

Run in the same MSVC/Windows SDK environment used by the API compiler. Sources
are cached below build/deps; no system packages or runtime DLLs are installed.
"""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

VERSION = '1.6.0'
SHA256 = 'e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564'
URL = f'https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-{VERSION}.tar.gz'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    deps = root / 'build/deps'
    deps.mkdir(parents=True, exist_ok=True)
    archive = deps / f'libwebp-{VERSION}.tar.gz'
    if not archive.exists():
        with urllib.request.urlopen(URL, timeout=60) as response:
            data = response.read()
        if hashlib.sha256(data).hexdigest() != SHA256:
            raise RuntimeError('libwebp download checksum mismatch')
        archive.write_bytes(data)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise RuntimeError('libwebp cached archive checksum mismatch')
    source = deps / f'libwebp-{VERSION}'
    if not source.exists():
        with tarfile.open(archive) as package:
            package.extractall(deps, filter='data')
    output = deps / f'libwebp-{VERSION}-build'
    subprocess.run(['cmake', '-S', str(source), '-B', str(output), '-G', 'Ninja',
                    '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_POLICY_DEFAULT_CMP0091=NEW',
                    '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
                    '-DCMAKE_C_COMPILER=' + str(Path(args.cc).resolve()),
                    '-DBUILD_SHARED_LIBS=OFF', '-DWEBP_USE_THREAD=OFF'] +
                   ['-DWEBP_BUILD_' + option + '=OFF' for option in (
                       'ANIM_UTILS', 'CWEBP', 'DWEBP', 'GIF2WEBP', 'IMG2WEBP',
                       'VWEBP', 'WEBPINFO', 'LIBWEBPMUX', 'WEBPMUX', 'EXTRAS')], check=True)
    subprocess.run(['cmake', '--build', str(output), '--target', 'webpdecoder',
                    '--parallel', '4'], check=True)
    if not (output / 'webpdecoder.lib').is_file():
        raise RuntimeError('static webpdecoder.lib was not produced')
    license_dir = root / 'build/licenses/libwebp'
    license_dir.mkdir(parents=True, exist_ok=True)
    for name in ('COPYING', 'PATENTS', 'AUTHORS'):
        shutil.copy2(source / name, license_dir / name)
    print('WebP decoder ready:', output / 'webpdecoder.lib')


if __name__ == '__main__':
    main()
