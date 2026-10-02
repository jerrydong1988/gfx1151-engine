"""Retain R22 regression inputs and add two not-yet-model-run instances."""
from datetime import date, timedelta
import hashlib
from pathlib import Path
import random
import r22_variants as old

VARIANTS = old.VARIANTS + ('r23-d', 'r23-e')


def configure(fixture, variant):
    if variant in old.VARIANTS:
        return old.configure(fixture, variant)
    if variant not in VARIANTS:
        raise ValueError('Unknown R23 instance')
    original = fixture.office_data
    base = 'r22-a' if variant == 'r23-d' else 'r22-c'
    old.configure(fixture, base)
    seed = 23017 if variant == 'r23-d' else 23089

    def rows(unused=None):
        inv, pay = old.office_rows(original, base)
        rng = random.Random(seed)
        keys = sorted({r[1] for r in inv} | {r[2] for r in pay})
        order = rng.sample(range(100000,999999),len(keys))
        info = {key:(f'{variant}-I{number}',rng.randrange(101,8101),rng.choice((1,2)),
                     rng.choice(('采购甲','服务乙','产品丙','华北丁','华南戊')),
                     rng.choice((2022,2023,2024,2025,2026)),rng.randrange(-120,121))
                for key,number in zip(keys,order)}
        for r in inv:
            key,shift,scale,dept,year,days = info[r[1]]
            r[1],r[3],r[4] = key,dept,year
            r[5] = (date.fromisoformat(r[5])+timedelta(days=days)).isoformat()
            if r[6] is not None and r[6] > 0:
                r[6] = r[6]*scale+shift
        for r in pay:
            r[1] = variant+'-'+r[1]
            r[2] = info[r[2]][0]
            r[4] = r[4]*3 + 137
        for table in (inv,pay):
            rng.shuffle(table)
            for i,r in enumerate(table,1):
                r[0]=i
        return inv,pay

    fixture.office_data = rows
    fixture.DATA_VARIANT = variant
    reference = (Path(__file__).parent/'selftest_reference_parser.py').read_text(encoding='utf-8')
    if variant == 'r23-d':
        fixture.INITIAL_CODE = reference.replace("any(c not in '0123456789' for c in digits + fraction)",
                                                'not (digits + fraction).isdigit()')
    else:
        fixture.INITIAL_CODE = reference.replace('len(group) != 3', 'len(group) < 2 or len(group) > 3')
    # Explain an existing VM restriction on new instances; historical prompts
    # remain untouched. Both policies see the same declared constraint.
    fixture.CODE_CONTRACT += '\n执行器不允许以下划线开头的局部变量名。\n'
    public = list(fixture.PUBLIC_INPUTS)
    public += ['１２.３４','1,23','12,34.5']
    random.Random(seed).shuffle(public)
    fixture.PUBLIC_INPUTS = public
    hidden = fixture.hidden_inputs() + ['１２.３４','1,23','12,34.5']
    for i in range(1,33):
        hidden += [f'{i},12.34',f'-￥{seed*i}.07',f'\u2003{seed*i}.10']
    fixture.hidden_inputs = lambda: list(hidden)
    return dict(variant=variant,office_input_sha256=old.fingerprint(rows()),
        office_expected_sha256=old.fingerprint(fixture.office_truth()),
        initial_code_sha256=hashlib.sha256(fixture.INITIAL_CODE.encode()).hexdigest(),
        public_code_inputs_sha256=old.fingerprint(public),private_code_inputs_sha256=old.fingerprint(hidden),
        private_case_count=len(hidden),records=1002)
