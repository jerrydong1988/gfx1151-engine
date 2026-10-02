import tempfile, pathlib, json, hashlib
import fixture as f
O=pathlib.Path(__file__).resolve().parent
checks=[]
with tempfile.TemporaryDirectory(prefix='r18-cte-',dir=O)as temp:
    root=pathlib.Path(temp)/'run';f.create_run('office',root)
    before=hashlib.sha256((root/'ledger.sqlite').read_bytes()).hexdigest()
    queries={
      'SELECT count(*) FROM invoices':652,
      'WITH d AS (SELECT DISTINCT invoice_id FROM invoices) SELECT count(*) FROM d':600,
      'WITH d AS (SELECT 1 AS x) SELECT count(*) FROM d':1,
      'WITH d AS (SELECT * FROM invoices), e AS (SELECT * FROM d) SELECT (SELECT count(*) FROM e)':652,
    }
    for sql,count in queries.items():
        assert f.query(root,sql)['rows']==[[count]],sql
        checks.append({'sql':sql,'allowed':True})
    for sql in ["SELECT count(*) FROM sqlite_master", "WITH d AS (SELECT * FROM sqlite_master) SELECT count(*) FROM d",
                "WITH invoices AS (SELECT * FROM sqlite_master) SELECT count(*) FROM invoices", "DELETE FROM invoices",
                "CREATE TABLE extra(x)", "ATTACH DATABASE ':memory:' AS extra", "PRAGMA table_info(invoices)",
                "SELECT load_extension('missing')"]:
        try:f.query(root,sql)
        except Exception:checks.append({'sql':sql,'allowed':False})
        else:raise AssertionError(sql)
    assert hashlib.sha256((root/'ledger.sqlite').read_bytes()).hexdigest()==before
print(json.dumps({'passed':True,'checks':checks,'database_unchanged':True},indent=2))
print('PASS',len(checks),'CTE/denial checks; database unchanged')
