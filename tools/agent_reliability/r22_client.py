"""R22 entry point: fixed budgets, fresh instances, existing strict SSE client."""
import argparse
import hashlib
import json
from pathlib import Path
import r14_agent_client as client
from r22_variants import VARIANTS, configure


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--variant', choices=VARIANTS, required=True)
    p.add_argument('--scenario', choices=['office', 'code'], required=True)
    p.add_argument('--identity', required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--work-dir', type=Path, required=True)
    a = p.parse_args()
    original_load = client.load_fixture
    provenance = {}

    def load():
        fixture, digest = original_load()
        provenance.update(configure(fixture, a.variant))
        provenance['base_fixture_sha256'] = digest
        provenance['variant_source_sha256'] = hashlib.sha256(
            (Path(__file__).parent / 'r22_variants.py').read_bytes()).hexdigest()
        provenance['wrapper_sha256'] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
        return fixture, digest

    client.load_fixture = load
    args = argparse.Namespace(base_url='http://127.0.0.1:18871/v1', scenario=a.scenario,
        repeat=1, mode=1, identity=a.identity, output=a.output, work_dir=a.work_dir,
        max_turns=24, max_tokens=4096, reasoning='true', tool_repair=2,
        fixture_variant=a.variant)
    try:
        result = client.execute(args)
    finally:
        if a.output.exists():
            current = json.loads(a.output.read_text(encoding='utf-8'))
            current['r22_provenance'] = provenance
            client.update_owned(a.output, current)
    print(json.dumps(dict(status=result['status'], success=result['all_tasks_success'],
        turns=len(result['records']), variant=a.variant, scenario=a.scenario), ensure_ascii=False), flush=True)


if __name__ == '__main__':
    main()
