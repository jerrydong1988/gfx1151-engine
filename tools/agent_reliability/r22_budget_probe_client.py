"""Secondary diagnostic only: both profiles get 8192 tokens on instance A.

Registered after the two crossed length failures in the 4096-token matrix.
This probe never replaces primary attempts or changes their success denominator.
"""
import hashlib
import json
from pathlib import Path
import sys
import r22_client

original_execute = r22_client.client.execute


def extended(args):
    assert args.max_tokens == 4096 and args.fixture_variant == 'r22-a'
    args.max_tokens = 8192
    return original_execute(args)


if __name__ == '__main__':
    r22_client.client.execute = extended
    try:
        r22_client.main()
    finally:
        output = Path(sys.argv[sys.argv.index('--output') + 1]) if '--output' in sys.argv else None
        if output is not None and output.exists():
            result = json.loads(output.read_text(encoding='utf-8'))
            result['r22_budget_probe'] = dict(secondary=True, max_tokens_per_turn=8192,
                source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                selection='Instance A, both task families and both profiles; selected after primary length failures')
            r22_client.client.update_owned(output, result)
