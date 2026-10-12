#!/usr/bin/env python3
"""Generate independent Pillow fixtures and run the CPU WebP/API regression test."""
import argparse
import base64
import json
from pathlib import Path
import struct
import subprocess
import tempfile
from PIL import Image


def fixtures(root):
    image = Image.new('RGBA', (64, 48))
    image.putdata([(x * 4, y * 5, (x + y) * 2, (x * 7 + y * 3) % 256)
                   for y in range(48) for x in range(64)])
    for name, fmt, options in [('lossless', 'WEBP', {'lossless': True, 'exact': True}),
                               ('lossy', 'WEBP', {'quality': 75}),
                               ('alpha', 'WEBP', {'lossless': True, 'exact': True}),
                               ('jpeg', 'JPEG', {'quality': 90}), ('png', 'PNG', {})]:
        source = image if name == 'alpha' else image.convert('RGB')
        if name == 'jpeg':
            # Solid color avoids comparing unrelated stb/libjpeg chroma upsamplers.
            source = Image.new('RGB', image.size, (27, 83, 191))
        source.save(root / (name + '.img'), format=fmt, **options)
        with Image.open(root / (name + '.img')) as decoded:
            decoded.convert('RGB').save(root / (name + '.png'))
    data = (root / 'lossless.img').read_bytes()
    (root / 'truncated.webp').write_bytes(data[:25])
    (root / 'corrupt.webp').write_bytes(b'RIFF\x10\x00\x00\x00WEBPVP8 garbage')
    image.save(root / 'animated.webp', save_all=True,
               append_images=[Image.new('RGBA', image.size, 'red')], duration=100, lossless=True)
    # Valid VP8L dimensions, deliberately over the 100M-pixel allocation cap.
    payload = b'\x2f' + struct.pack('<I', 16382 | (16382 << 14))
    chunk = b'VP8L' + struct.pack('<I', len(payload)) + payload + b'\0'
    (root / 'oversized.webp').write_bytes(b'RIFF' + struct.pack('<I', len(chunk) + 4) + b'WEBP' + chunk)
    url = 'data:image/webp;base64,' + base64.b64encode(data).decode()
    body = {'input': [{'role': 'user', 'content': 'Read image'},
                     {'type': 'function_call', 'call_id': 'read1', 'name': 'read_image', 'arguments': '{}'},
                     {'type': 'function_call_output', 'call_id': 'read1', 'output': [
                         {'type': 'input_text', 'text': 'before'},
                         {'type': 'input_image', 'image_url': url},
                         {'type': 'input_text', 'text': 'after'}]}]}
    (root / 'messages.json').write_text(json.dumps(body), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--test-bin', required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='gdec-webp-test-') as tmp:
        fixtures(Path(tmp))
        subprocess.run([str(Path(args.test_bin).resolve()), tmp], check=True)
