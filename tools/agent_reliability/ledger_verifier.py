"""Read-only reconciliation for the declared synthetic ledger contract.

This derives results from source tables using SQL. It never imports the hidden
Python grading oracle or model-generated SQL. It is schema/contract-specific,
not a validator for arbitrary business spreadsheets.
"""
import hashlib
import json
from pathlib import Path
import sqlite3
import time

CTE = '''WITH
latest AS (
 SELECT DISTINCT invoice_id,revision,department,issued_year,due_date,amount_cents,status,currency
 FROM invoices i WHERE revision=(SELECT MAX(revision) FROM invoices j WHERE i.invoice_id=j.invoice_id)
), ir AS (
 SELECT invoice_id,CASE WHEN COUNT(*)>1 THEN 'conflicting_latest_revision'
 WHEN MIN(status)<>'posted' THEN 'non_posted' WHEN MIN(currency)<>'CNY' THEN 'non_cny'
 WHEN MIN(amount_cents) IS NULL OR MIN(amount_cents)<0 THEN 'missing_amount' ELSE 'eligible' END reason
 FROM latest GROUP BY invoice_id
), eligible AS (SELECT l.* FROM latest l JOIN ir USING(invoice_id) WHERE ir.reason='eligible'),
payments_distinct AS (
 SELECT DISTINCT event_id,invoice_id,paid_on,amount_cents,status,currency FROM payments
), pr AS (
 SELECT event_id,CASE WHEN COUNT(*)>1 THEN 'conflicting_event'
 WHEN MIN(status)<>'settled' THEN 'not_settled' WHEN MIN(currency)<>'CNY' THEN 'non_cny'
 WHEN MIN(paid_on)>:cutoff THEN 'after_cutoff'
 WHEN MIN(invoice_id) NOT IN (SELECT invoice_id FROM eligible) THEN 'ineligible_or_missing_invoice'
 ELSE 'eligible' END reason FROM payments_distinct GROUP BY event_id
), paid AS (
 SELECT p.invoice_id,SUM(p.amount_cents) amount FROM payments_distinct p JOIN pr USING(event_id)
 WHERE pr.reason='eligible' GROUP BY p.invoice_id
), balances AS (
 SELECT e.*,COALESCE(p.amount,0) paid FROM eligible e LEFT JOIN paid p USING(invoice_id)
) '''
AGG = 'COUNT(*),COALESCE(SUM(amount_cents),0),COALESCE(SUM(paid),0),COALESCE(SUM(amount_cents-paid),0)'
GROUP_KEYS = ('invoice_count','receivable_cents','applied_payment_cents','balance_cents')


def file_digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, ensure_ascii=False, allow_nan=False, separators=(',', ':'))


def exact(left, right):
    if type(left) is not type(right):
        return False
    if isinstance(right, dict):
        return left.keys() == right.keys() and all(exact(left[k],v) for k,v in right.items())
    if isinstance(right, list):
        return len(left) == len(right) and all(exact(a,b) for a,b in zip(left,right))
    return left == right


def normalized(value):
    value = json.loads(canonical(value))
    for key, field in (('by_department','department'), ('by_year','issued_year')):
        value[key] = sorted(value[key], key=lambda row: row[field])
    return value


def recompute(database, cutoff='2026-09-30'):
    # Path comes from the owned fixture, never a model-supplied argument.
    con = sqlite3.connect(Path(database).resolve().as_uri() + '?mode=ro', uri=True)
    try:
        con.execute('PRAGMA query_only=ON')
        con.execute('BEGIN')
        deadline = time.monotonic() + 5
        con.set_progress_handler(lambda: int(time.monotonic() > deadline), 1000)
        def rows(sql):
            return con.execute(CTE + sql, {'cutoff':cutoff}).fetchall()
        values = rows('SELECT '+AGG+' FROM balances')[0]
        result = dict(kind='office', **dict(zip(
            ('eligible_invoice_count',)+GROUP_KEYS[1:], values)))
        for field, key in (('department','by_department'), ('issued_year','by_year')):
            result[key] = [dict(zip((field,)+GROUP_KEYS,row)) for row in
                rows(f'SELECT {field},{AGG} FROM balances GROUP BY {field} ORDER BY {field}')]
        result['overdue_top5'] = [dict(zip(('invoice_id','department','due_date','balance_cents'), row))
            for row in rows("SELECT invoice_id,department,due_date,amount_cents-paid AS balance "
                            "FROM balances WHERE due_date<:cutoff AND balance>0 "
                            "ORDER BY balance DESC,invoice_id ASC LIMIT 5")]
        for table,key,categories in (
            ('ir','invoice_exclusions',('conflicting_latest_revision','non_posted','non_cny','missing_amount')),
            ('pr','payment_exclusions',('conflicting_event','not_settled','non_cny','after_cutoff','ineligible_or_missing_invoice'))):
            counts = dict(rows(f"SELECT reason,COUNT(*) FROM {table} WHERE reason<>'eligible' GROUP BY reason"))
            result[key] = {k:counts.get(k,0) for k in categories}
        return result
    finally:
        con.close()


class LedgerVerifier:
    def __init__(self, database, cutoff='2026-09-30', max_rejections=2):
        self.database = Path(database)
        self.cutoff = cutoff
        self.input_sha256 = file_digest(self.database)
        self.max_rejections = max_rejections
        self.rejections = 0
        self.accepted_sha256 = None
        self.blocked = False
        self.events = []

    def check(self, submission):
        start = time.perf_counter()
        self.accepted_sha256 = None
        failures = []
        try:
            if self.blocked:
                failures = ['delivery_guard_limit']
            elif file_digest(self.database) != self.input_sha256:
                failures = ['source_changed']
                self.blocked = True
            else:
                expected = normalized(recompute(self.database, self.cutoff))
                try:
                    actual = normalized(submission)
                    if type(actual) is not dict or actual.keys() != expected.keys():
                        failures.append('submission_fields')
                    failures += [key for key in expected if not exact(actual.get(key),expected[key])]
                except (ValueError,TypeError,KeyError,AttributeError):
                    failures = ['submission_shape']
                if file_digest(self.database) != self.input_sha256:
                    failures.append('source_changed')
                    self.blocked = True
        except (OSError,sqlite3.Error):
            failures = ['independent_verifier_unavailable']
            self.blocked = True
        if failures:
            self.rejections += 1
            self.blocked = self.blocked or self.rejections >= self.max_rejections
        else:
            self.accepted_sha256 = hashlib.sha256(canonical(submission).encode()).hexdigest()
        event = dict(accepted=not failures, failed_fields=sorted(set(failures)),
                     recoverable=bool(failures) and not self.blocked,
                     rejection_count=self.rejections, input_sha256=self.input_sha256,
                     verifier_wall_s=time.perf_counter()-start)
        self.events.append(event)
        # No expected amount, corrected output, private-case answer or SQL solution
        # is returned to the agent. Each attempt is independently recorded.
        return event
