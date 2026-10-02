"""Extract production reference kernels used by the standalone HIP benchmarks.

Run before compiling bf16_exact_gemv_bench.cpp or v2_exact_wave_bench.cpp.
No generated source is checked in, avoiding drift between tests and reference.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build'


def between(source, begin, end):
    start = source.index(begin)
    return source[start:source.index(end, start)]


def main():
    BUILD.mkdir(exist_ok=True)
    source = (ROOT / 'src/gpu/parts/20_kernels_gemv.inc').read_text(encoding='utf8')
    reference = between(source, '// --- bf16-weight multi-row GEMV', '// --- fp32-weight multi-row GEMV')
    reference += between(source, '// --- BF16 GEMV: one warp per row', '// --- q8g32 P=1 GEMV')
    (BUILD / 'bf16_reference.inc').write_text(reference, encoding='utf8')
    source = (ROOT / 'src/gpu/parts/24_kernels_moe_lt.inc').read_text(encoding='utf8')
    source = source[source.index('// Weighted deterministic reduce:'):]
    reference = between(source, 'template <int K>', '// k_moe_reduce_pw +')
    (BUILD / 'reduce_reference.inc').write_text(reference, encoding='utf8')


if __name__ == '__main__':
    main()
