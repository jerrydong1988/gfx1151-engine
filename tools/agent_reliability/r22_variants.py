"""Fresh paired instances; answers and private tests never enter model messages.

Reuses the bounded fixture tools and independent integer/regex oracles. The
variants change business identities, amounts, dates, exclusions and initial code,
not the published task contract. They are instances of two task families, not six
different benchmarks. configure() must receive a freshly imported fixture module.
"""
from datetime import date, timedelta
import hashlib
import json
from pathlib import Path
import random

VARIANTS = ('r22-a', 'r22-b', 'r22-c')


def fingerprint(value):
    return hashlib.sha256(json.dumps(value, ensure_ascii=False, sort_keys=True,
        separators=(',', ':'), allow_nan=False).encode()).hexdigest()


def office_rows(original, variant):
    if variant not in VARIANTS:
        raise ValueError('Unknown R22 variant')
    seed = 22001 + VARIANTS.index(variant) * 101
    rng = random.Random(seed)
    invoices, payments = original('base')
    identities = sorted({r[1] for r in invoices} | {r[2] for r in payments})
    numbers = rng.sample(range(10000, 99000), len(identities))
    mapping = {key: f'R{seed}-{number}' for key, number in zip(identities, numbers)}
    departments = ['研发甲', '运营乙', '财务丙', '销售丁', '交付戊', '综合己']
    info = {}
    for key in identities:
        info[key] = dict(mult=rng.choice([1, 2, 3]), shift=rng.randrange(97, 9907),
            dept=rng.choice(departments), year=rng.choice([2023, 2024, 2025, 2026]),
            due=(date(2025, 1, 1) + timedelta(days=rng.randrange(760))).isoformat(),
            zero=rng.randrange(53) == 0, negative=rng.randrange(67) == 0,
            foreign=rng.randrange(37) == 0, draft=rng.randrange(31) == 0)
    for row in invoices:
        key = row[1]
        item = info[key]
        row[1] = mapping[key]
        row[3:6] = [item['dept'], item['year'], item['due']]
        if row[6] is not None:
            row[6] = row[6] * item['mult'] + item['shift']
            if item['zero']:
                row[6] = 0
            if item['negative']:
                row[6] = -abs(row[6]) - 1
        if item['foreign']:
            row[8] = 'USD'
        if item['draft']:
            row[7] = 'draft'
    event_info = {}
    for key in sorted({r[1] for r in payments}):
        event_info[key] = (rng.randrange(20000, 99000), rng.choice([1, 2, 4]),
            rng.randrange(11, 991), rng.randrange(29) == 0,
            rng.randrange(41) == 0, rng.randrange(23) == 0)
    for row in payments:
        key = row[1]
        number, mult, shift, pending, foreign, late = event_info[key]
        # Include the original ID to guarantee no random event-ID collisions.
        row[1] = f'E{seed}-{number}-{key}'
        row[2] = mapping[row[2]]
        row[4] = row[4] * mult + shift
        if pending:
            row[5] = 'pending'
        if foreign:
            row[6] = 'USD'
        if late:
            row[3] = '2026-10-01'
    for rows in (invoices, payments):
        rng.shuffle(rows)
        for i, row in enumerate(rows, 1):
            row[0] = i
    assert (len(invoices), len(payments)) == (652, 350)
    return invoices, payments


def configure(fixture, variant):
    if variant not in VARIANTS:
        raise ValueError('Unknown R22 variant')
    original_office = fixture.office_data
    original_hidden = fixture.hidden_inputs
    fixture.DATA_VARIANT = variant
    fixture.office_data = lambda value=None: office_rows(original_office, value or fixture.DATA_VARIANT)
    reference = (Path(__file__).parent / 'selftest_reference_parser.py').read_text(encoding='utf-8')
    if variant == 'r22-b':
        fixture.INITIAL_CODE = reference.replace("value = text.strip(' \\t\\r\\n')", 'value = text.strip()').replace(
            "if cents > 99999999999:", 'if cents > 999999999999:')
    elif variant == 'r22-c':
        fixture.INITIAL_CODE = reference.replace('1 <= len(fraction) <= 2', 'len(fraction) == 2').replace(
            "return sign * cents", 'return cents')
    rng = random.Random(42001 + VARIANTS.index(variant) * 137)
    public = list(fixture.PUBLIC_INPUTS)
    public.extend(['\u00a012.34', '-0.10', '1.5'])
    rng.shuffle(public)
    fixture.PUBLIC_INPUTS = public
    private = list(original_hidden())
    for _ in range(40):
        major, minor = rng.randrange(999999999), rng.randrange(100)
        private.extend([f'-￥{major:,}.{minor:02d}', f' +{major}.{minor:02d}\r\n',
                        f'{major}.{minor:02d}0', f'{major}e2'])
    fixture.hidden_inputs = lambda: list(private)
    return dict(variant=variant, office_input_sha256=fingerprint(fixture.office_data()),
        office_expected_sha256=fingerprint(fixture.office_truth()),
        initial_code_sha256=hashlib.sha256(fixture.INITIAL_CODE.encode()).hexdigest(),
        public_code_inputs_sha256=fingerprint(public), private_code_inputs_sha256=fingerprint(private),
        private_case_count=len(private), records=1002)
