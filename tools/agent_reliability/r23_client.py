"""R23 bounded budget policy and optional pre-delivery source reconciliation."""
import argparse
import hashlib
import json
from pathlib import Path
import r14_agent_client as client
from r23_variants import VARIANTS, configure
from delivery_guard import install


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--variant', choices=VARIANTS, required=True)
    p.add_argument('--scenario', choices=('office','code'), required=True)
    p.add_argument('--policy', choices=('fixed8192','adaptive4096'), required=True)
    p.add_argument('--delivery-guard', action='store_true')
    p.add_argument('--identity', required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--work-dir', type=Path, required=True)
    a=p.parse_args()
    original=client.load_fixture
    provenance={}

    def load():
        f, sha=original()
        provenance.update(configure(f,a.variant))
        provenance['base_fixture_sha256']=sha
        provenance['implementation_sha256']={name:hashlib.sha256((Path(__file__).parent/name).read_bytes()).hexdigest()
            for name in ('r23_client.py','r23_variants.py','r22_variants.py','r14_agent_client.py',
                         'budget_recovery.py','ledger_verifier.py','delivery_guard.py')}
        if a.delivery_guard:
            install(f)
        return f,sha

    client.load_fixture=load
    args=argparse.Namespace(base_url='http://127.0.0.1:18871/v1',scenario=a.scenario,repeat=1,
        mode=1,identity=a.identity,output=a.output,work_dir=a.work_dir,max_turns=24,
        max_tokens=8192 if a.policy=='fixed8192' else 4096,reasoning='true',tool_repair=2,
        fixture_variant=a.variant,budget_monitor=True,
        length_retry_max_tokens=8192 if a.policy=='adaptive4096' else 0,
        max_task_output_tokens=65536,task_wall_limit_s=1800,delivery_guard=a.delivery_guard,
        policy=a.policy)
    try:
        result=client.execute(args)
    finally:
        if a.output.exists():
            current=json.loads(a.output.read_text(encoding='utf-8'))
            current['r23_provenance']=provenance
            client.update_owned(a.output,current)
    print(json.dumps(dict(success=result['all_tasks_success'],status=result['status'],
        records=len(result['records']),variant=a.variant,policy=a.policy),ensure_ascii=False),flush=True)


if __name__=='__main__':
    main()
