"""Extract the real prefix-correction implementation and build its HIP probe.

Never executes a GPU test. --cpu-check only runs uploader/reference checks with
host-memory transport; invoke the resulting executable explicitly for GPU tests.
"""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, default=HERE.parents[1])
    ap.add_argument('--outdir', type=Path, required=True)
    ap.add_argument('--compiler', required=True, help='HIP-capable hipcc executable')
    ap.add_argument('--arch', default='gfx1151')
    ap.add_argument('--cpu-check', action='store_true')
    ap.add_argument('--legacy-corrections-ref', help='Read ONLY the two correction functions from this Git ref for a before/after probe')
    args = ap.parse_args()
    src = args.source.resolve() / 'src/gpu/parts'
    out = args.outdir.resolve()
    if out.exists():
        ap.error(f'Refusing to overwrite existing evidence: {out}')
    out.mkdir(parents=True)
    meta = {'scope': 'Real uploader, GPU correction/top-k dispatch, and HostSampler; synthetic logits, no model/KV',
            'gpu_executed': False, 'excerpts': {}}

    def extract(name, file, start, end, include_end=False):
        raw = (src / file).read_bytes()
        text = raw.decode('utf8').replace('\r\n', '\n')
        a = text.index(start)
        b = text.index(end, a) + (len(end) if include_end else 0)
        excerpt = text[a:b].rstrip() + '\n'
        (out / name).write_text(excerpt, encoding='utf8', newline='\n')
        meta['excerpts'][name] = {
            'source': str((src / file).resolve()),
            'line_start': text[:a].count('\n') + 1,
            'line_end': text[:b].count('\n') + 1,
            'sha256': hashlib.sha256(excerpt.encode()).hexdigest(),
            'source_file_sha256': hashlib.sha256(raw).hexdigest(),
            'normalization': 'CRLF to LF; trailing whitespace at excerpt end stripped'}

    extract('gpu-kernels.inc', '21_kernels_ple.inc', '__global__ void k_corr_apply', '// [tgk-end]')
    extract('gpu-dispatch.inc', '40_model.inc', '  bool sms_topk_rows(', '  // prepare_sparse on candidate row')
    extract('round-upload.inc', '40_model.inc', '  template <typename Sampler>\n  bool sms_round_shared(', '  // Per-row draft-prefix penalties.')
    extract('prefix-upload.inc', '40_model.inc', '  template <typename Sampler>\n  bool sms_prefix_upload(', '  // Corrections + two-pass radix top-k')
    extract('host-sampler.inc', '51_host_cfg.inc', 'struct HostSampler {', '\n};', True)
    # The pre-filter correction statements are verbatim, not a reimplementation.
    extract('host-correction-body.inc', '51_host_cfg.inc', '    const int n = (int)logits.size();', '    float lmax = -INFINITY;')
    if args.legacy_corrections_ref:
        repo = args.source.resolve()
        commit = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', args.legacy_corrections_ref], text=True).strip()
        meta['legacy_corrections_commit'] = commit
        for name, file, start, end in [
            ('gpu-kernels.inc', '21_kernels_ple.inc', '__global__ void k_corr_apply', '// Radix top-k'),
            ('prefix-upload.inc', '40_model.inc', '  template <typename Sampler>\n  bool sms_prefix_upload(', '  // Corrections + two-pass radix top-k'),
        ]:
            old = subprocess.check_output(['git', '-C', str(repo), 'show', commit + ':src/gpu/parts/' + file]).decode('utf8').replace('\r\n', '\n')
            a = old.index(start)
            b = old.index(end, a)
            legacy = old[a:b]
            current = (out / name).read_text(encoding='utf8')
            replacement = legacy + (current[current.index(end):] if name == 'gpu-kernels.inc' else '')
            (out / name).write_text(replacement, encoding='utf8', newline='\n')
            meta['excerpts'][name]['legacy_replacement'] = {
                'commit': commit, 'path': 'src/gpu/parts/' + file,
                'line_start': old[:a].count('\n') + 1, 'line_end': old[:b].count('\n') + 1,
                'sha256': hashlib.sha256(legacy.encode()).hexdigest()}
            meta['excerpts'][name]['effective_sha256'] = hashlib.sha256(replacement.encode()).hexdigest()
    exe = out / 'prefix-correction-probe.exe'
    cmd = [args.compiler, '-O3', '-std=c++17', '--offload-arch=' + args.arch,
           '-x', 'hip', '-I', str(out), str(HERE / 'probe.cpp'), '-o', str(exe)]
    meta['compile_command'] = cmd
    meta['test_sha256'] = hashlib.sha256((HERE / 'probe.cpp').read_bytes()).hexdigest()
    meta['extractor_sha256'] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    result = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / 'compile.log').write_text(result.stdout, encoding='utf8')
    meta['compile_exit_code'] = result.returncode
    if result.returncode == 0:
        meta['executable_sha256'] = hashlib.sha256(exe.read_bytes()).hexdigest()
        if args.cpu_check:
            check = subprocess.run([str(exe), '--cpu-only'], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            (out / 'cpu-results.jsonl').write_text(check.stdout, encoding='utf8')
            meta['cpu_exit_code'] = check.returncode
    (out / 'metadata.json').write_text(json.dumps(meta, ensure_ascii=False, indent=2), encoding='utf8')
    print(json.dumps({'executable': str(exe), 'compile_exit_code': result.returncode,
                      'cpu_exit_code': meta.get('cpu_exit_code'), 'gpu_executed': False}))
    raise SystemExit(result.returncode or meta.get('cpu_exit_code', 0))


if __name__ == '__main__':
    main()
