"""R14 deterministic CPU fixtures. No shell, subprocess, network, GPU or arbitrary paths.

Canonical API: SCENARIOS, TOOLS, new_run(root, scenario),
dispatch(name, arguments, root), validate_final(final, root).
Also supports create_run(scenario, run_dir), four-argument dispatch, and evaluate.
Tool outputs are real local SQLite/limited-AST results, never canned successes.
"""
from __future__ import annotations

import ast
from collections import defaultdict, Counter
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import re
import sqlite3
import time

HOME = Path(__file__).resolve().parent
CUTOFF = '2026-09-30'
INVOICE_COLS = ['row_id','invoice_id','revision','department','issued_year','due_date','amount_cents','status','currency']
PAYMENT_COLS = ['row_id','event_id','invoice_id','paid_on','amount_cents','status','currency']
INITIAL_CODE = '''def parse_amount_cents(text):
    cleaned = text.strip().replace("¥", "").replace("￥", "").replace(",", "")
    return round(float(cleaned) * 100)
'''
CODE_CONTRACT = '''修复单个纯函数 parse_amount_cents(text)，返回精确整数分，非法输入统一 raise ValueError。
仅接受str；先去除首尾ASCII空白（空格、\\t、\\r、\\n），内部空白不允许。
格式：可选一个前置+或-，其后可选一个¥或￥，然后非空ASCII数字整数部分；可选小数点及1或2位ASCII数字。
无逗号整数可有前导0。有逗号时首组1至3位、后续每组恰3位；不得在小数部分放逗号。
绝对值上限999999999.99元；负零返回0。禁止指数、NaN/Inf、括号、下划线、Unicode数字及额外符号。
不得通过binary float近似金额。调用者仍传原始表单字符串，不能改变签名/调用者。
执行器明确是CPU受限Python AST函数解释器，不是完整仓库或shell：仅一个def；可用赋值、if、for、return、raise ValueError、break/continue，整数/字符串/list/tuple，单层列表或生成式，常见算术比较。
内置函数：len,int,str,float,round,abs,all,any,min,max,sum,range,enumerate,isinstance；str方法strip/split/startswith/endswith/replace/join/isdigit/isdecimal/count。
不支持import、while、try、类、递归、任意属性、文件/网络/进程；每测试有步数和对象大小上限。float仅用于重现旧实现，提交修复不得调用float或round。
'''
OFFICE_POLICY = '''这些均为确定性合成的业务记录，无真实客户数据。金额一律为整数分，不做汇率换算；截点2026-09-30。
发票：按invoice_id取最高revision。忽略row_id后完全相同的最高版本行视作重复；同一最高版本若有不同内容，整张发票排除为conflicting_latest_revision。否则仅posted、CNY、非空且非负amount_cents纳入。
发票排除分类按以下顺序且互斥：conflicting_latest_revision → non_posted → non_cny → missing_amount。
付款：按event_id去重（忽略row_id）。不同内容的同event_id排除为conflicting_event；余下按not_settled → non_cny → after_cutoff → ineligible_or_missing_invoice顺序排除。after_cutoff是paid_on晚于截点；最后一类是没有合格发票。其余付款按发票累计。排除数按唯一invoice_id/event_id计，不按原始行数计。
账面余额=合格发票金额−已确认纳入付款，允许负数（预收/超收），不得截为0。仅余额>0且due_date严格早于截点才算逾期；逾期Top5按balance_cents降序、invoice_id升序。
按department和issued_year分别汇总invoice_count、receivable_cents、applied_payment_cents、balance_cents。分组只能来源于合格发票，无合格发票的组不列出。
最终提交字段：kind='office'、eligible_invoice_count、receivable_cents、applied_payment_cents、balance_cents、by_department、by_year、overdue_top5、invoice_exclusions、payment_exclusions。
by_department每项含department及上述4个分组指标；by_year每项含issued_year及4指标；Top5每项含invoice_id/department/due_date/balance_cents。两个exclusions对象须包含上述全部类别（含0）。
'''


def tool(name, description, properties, required):
    return {'type':'function','function':{'name':name,'description':description,
            'parameters':{'type':'object','properties':properties,'required':required,'additionalProperties':False}}}


TOOLS = [
    tool('read_resource','读取该场景固定资源。overview给规则与资源目录；invoice/payment全量返回columns+rows。',
         {'resource':{'type':'string','enum':['overview','invoices','payments','parser','tests']}},['resource']),
    tool('query_ledger','对当前隔离账库执行单条只读SELECT或WITH查询；仅发票与付款表，无文件或网络能力。最多200结果行。',
         {'sql':{'type':'string'}},['sql']),
    tool('code_action','仅对当前隔离练习函数执行真实测试，或用source完整替换该函数。无shell/网络/其他路径。',
         {'action':{'type':'string','enum':['test','replace']},'source':{'type':'string'}},['action']),
    tool('submit_result','保存本场景结构化最终结果，不计算或透露ground truth。office按overview字段；code用kind/summary/tests_passed。',
         {'result':{'type':'object','additionalProperties':True}},['result']),
]
SYSTEM_PROMPT = '''你在隔离CPU工具环境完成真实可核验的合成办公或函数修复任务。按需要调用工具并依据实际结果处理错误，不能假造读表、查询、修改或测试成功。
先读取overview明确规则；完成后必须用submit_result提交结构化结果，再给简短中文结论。工具是本地固定fixture能力，不是任意终端。无需询问批准。'''
SCENARIOS = {
    'office':{'system_prompt':SYSTEM_PROMPT,'user_prompt':'''请完成截至2026-09-30的合成应收台账复核。先实际读取规则及invoices/payments两张全量表，检查版本、重复和排除原因，然后使用只读SQL计算总额、部门/年度分组、逾期Top5和互斥异常数量。金额保持整数分，保留超收负余额。用submit_result提交overview要求的完整结构化结论，并简述异常口径。不要只凭部分样本估算。''','tools':TOOLS},
    'code':{'system_prompt':SYSTEM_PROMPT,'user_prompt':'''表单金额解析函数错误接受了畸形金额，有时金额分也不精确。请实际读取规范和当前函数/测试，先跑测试确认缺陷，修复固定parse_amount_cents函数，再运行测试验证。只修改该练习函数，不能改测试或规则。通过后以submit_result提交{"kind":"code","summary":"简述修复","tests_passed":true}；若未通过则如实提交false。''','tools':TOOLS},
}



from task_contracts import office_schema, code_schema
for _scenario, _schema in [('office', office_schema()), ('code', code_schema())]:
    _tools=json.loads(json.dumps(TOOLS))
    for _tool in _tools:
        if _tool['function']['name']=='submit_result':
            _tool['function']['parameters']['properties']['result']=_schema
    SCENARIOS[_scenario]['tools']=_tools

DATA_VARIANT='base'

def office_data(variant=None):
    variant=DATA_VARIANT if variant is None else variant
    invoices, payments = [], []
    depts = ['OPS','ENG','FIN','SALES','HR','ADMIN']
    nominal = {}
    for i in range(1,601):
        amount = 100000+(i*7919)%4900000
        row = [0,f'INV{i:04d}',1,depts[(i-1)%6],2024+i%3,
               f'{2024+i%3}-{1+i%12:02d}-{1+(i*7)%27:02d}',
               None if i%29 == 0 else amount,
               'void' if i%37 == 0 else 'draft' if i%31 == 0 else 'posted',
               'USD' if i%41 == 0 else 'CNY']
        invoices.append(row.copy())
        if i%15 == 0:
            row = row.copy(); row[2] = 2
            if row[6] is not None: row[6] += 33333
            invoices.append(row.copy())
        nominal[i] = row[6] if row[6] is not None else amount
        if i%50 == 0:
            dup = row.copy()
            if i%100 == 0 and dup[6] is not None: dup[6] += 101
            invoices.append(dup)
    for j in range(1,301):
        invoice = 2*j
        row = [0,f'PAY{j:04d}',f'INV{invoice:04d}',
               f'2026-{10 if j%61 == 0 else 1+j%9:02d}-{1+j%27:02d}',
               nominal[invoice]*(35+15*(j%5))//100,
               'pending' if j%17 == 0 else 'settled','USD' if j%47 == 0 else 'CNY']
        payments.append(row.copy())
        if j%10 == 0:
            extra = row.copy(); extra[1] += 'B'; extra[4] = nominal[invoice]//2
            payments.append(extra)
        if j%20 == 0:
            dup = row.copy()
            if j%60 == 0: dup[4] += 77
            payments.append(dup)
    for j in range(1,6):
        payments.append([0,f'ORPH{j:03d}',f'INV{700+j:04d}','2026-09-01',10000*j,'settled','CNY'])
    for rows in (invoices,payments):
        for i,row in enumerate(rows,1): row[0] = i
    assert (len(invoices),len(payments)) == (652,350)
    if variant=='amounts-v2':
        for row in invoices:
            if row[6] is not None: row[6]=row[6]*2+(17 if row[6]>=0 else -17)
        for row in payments: row[4]=row[4]*3+(31 if row[4]>=0 else -31)
    elif variant!='base':raise ValueError('unknown data variant')
    return invoices,payments


def office_truth(variant=None):
    invoices,payments = office_data(variant)
    grouped = defaultdict(list)
    for values in invoices:
        r = dict(zip(INVOICE_COLS,values)); grouped[r['invoice_id']].append(r)
    exclusions = dict.fromkeys(['conflicting_latest_revision','non_posted','non_cny','missing_amount'],0)
    eligible = {}
    for key,records in grouped.items():
        rev = max(r['revision'] for r in records)
        latest = {tuple(r[c] for c in INVOICE_COLS[1:]) for r in records if r['revision'] == rev}
        r = dict(zip(INVOICE_COLS[1:],next(iter(latest))))
        reason = ('conflicting_latest_revision' if len(latest)>1 else 'non_posted' if r['status']!='posted'
                  else 'non_cny' if r['currency']!='CNY' else 'missing_amount' if r['amount_cents'] is None or r['amount_cents']<0 else None)
        if reason: exclusions[reason] += 1
        else: eligible[key] = r
    pe = dict.fromkeys(['conflicting_event','not_settled','non_cny','after_cutoff','ineligible_or_missing_invoice'],0)
    events = defaultdict(set)
    for row in payments: events[row[1]].add(tuple(row[1:]))
    paid = defaultdict(int)
    for values in events.values():
        r = dict(zip(PAYMENT_COLS[1:],next(iter(values))))
        reason = ('conflicting_event' if len(values)>1 else 'not_settled' if r['status']!='settled' else 'non_cny' if r['currency']!='CNY'
                  else 'after_cutoff' if r['paid_on']>CUTOFF else 'ineligible_or_missing_invoice' if r['invoice_id'] not in eligible else None)
        if reason: pe[reason] += 1
        else: paid[r['invoice_id']] += r['amount_cents']
    def aggregate(rows):
        due = sum(r['amount_cents'] for r in rows); received = sum(paid[r['invoice_id']] for r in rows)
        return {'invoice_count':len(rows),'receivable_cents':due,'applied_payment_cents':received,'balance_cents':due-received}
    total = aggregate(list(eligible.values())); total['eligible_invoice_count'] = total.pop('invoice_count')
    result = {'kind':'office',**total,'invoice_exclusions':exclusions,'payment_exclusions':pe}
    for field,outkey in [('department','by_department'),('issued_year','by_year')]:
        result[outkey] = [{field:k,**aggregate([r for r in eligible.values() if r[field]==k])} for k in sorted({r[field] for r in eligible.values()})]
    late = [{'invoice_id':r['invoice_id'],'department':r['department'],'due_date':r['due_date'],'balance_cents':r['amount_cents']-paid[r['invoice_id']]}
            for r in eligible.values() if r['due_date']<CUTOFF and r['amount_cents']>paid[r['invoice_id']]]
    result['overdue_top5'] = sorted(late,key=lambda r:(-r['balance_cents'],r['invoice_id']))[:5]
    return result


def amount_oracle(value):
    if type(value) is not str: raise ValueError('text required')
    value = value.strip(' \t\r\n')
    match = re.fullmatch(r'([+-]?)[¥￥]?([0-9]+|[0-9]{1,3}(?:,[0-9]{3})+)(?:\.([0-9]{1,2}))?',value)
    if not match: raise ValueError('format')
    sign,whole,frac = match.groups()
    amount = int(whole.replace(',',''))*100+int((frac or '').ljust(2,'0'))
    if amount>99999999999: raise ValueError('range')
    return -amount if sign=='-' else amount


PUBLIC_INPUTS = ['12.34','1,234.5','-¥0.01',' +￥001.20\t','0.29','9,99.00','1.234','1e3','NaN','１２.３４','1_000.00','¥-12.00','1,000,000,000.00','999,999,999.99',None,'  ','12.','1 2.00']


def hidden_inputs():
    values = list(PUBLIC_INPUTS)
    for i in range(1,81):
        major = (i*123457)%999999999
        sign = '-' if i%3==0 else '+' if i%3==1 else ''
        unit = '¥' if i%2 else '￥'
        frac = f'{i%100:02d}'
        values += [f'{sign}{unit}{major:,}.{frac}',str(major),f'{major}.0{i%10}']
    values += ['.1','-.1','++1','--1','1,23','1234,567','1,,234','1,234,','12,345.0,1','1\n2','1\u00a0','\u00a01','+','¥','0x10','-Inf','(12.00)',True,10,[],{},'1000000000','-1000000000','-0.00','000.01']
    return values


class FixtureExecutionError(Exception): pass
class BudgetError(FixtureExecutionError): pass
class Returned(Exception):
    def __init__(self,value): self.value=value
class BreakFlow(Exception): pass
class ContinueFlow(Exception): pass


class FunctionVM:
    """Bounded AST interpreter: no exec/eval/compile and no arbitrary Python objects."""
    FUNCTIONS = {'len':len,'int':int,'str':str,'float':float,'round':round,'abs':abs,
                 'all':all,'any':any,'min':min,'max':max,'sum':sum,'enumerate':enumerate,'isinstance':isinstance}
    METHODS = {'strip','split','startswith','endswith','replace','join','isdigit','isdecimal','count'}
    def __init__(self,source):
        if type(source) is not str or len(source)>6000: raise FixtureExecutionError('source must be <=6000 chars')
        tree = ast.parse(source)
        if len(list(ast.walk(tree)))>1000 or len(tree.body)!=1 or not isinstance(tree.body[0],ast.FunctionDef): raise FixtureExecutionError('exactly one bounded function required')
        self.fn=tree.body[0]; args=self.fn.args
        if self.fn.name!='parse_amount_cents' or self.fn.decorator_list or len(args.args)!=1 or args.args[0].arg!='text' or args.posonlyargs or args.kwonlyargs or args.vararg or args.kwarg or args.defaults or self.fn.returns: raise FixtureExecutionError('signature must be def parse_amount_cents(text)')
        forbidden=(ast.Import,ast.ImportFrom,ast.While,ast.Try,ast.With,ast.AsyncFunctionDef,ast.ClassDef,ast.Lambda,ast.Global,ast.Nonlocal,ast.Yield,ast.YieldFrom,ast.Await,ast.Delete,ast.NamedExpr)
        for node in ast.walk(self.fn):
            if isinstance(node,forbidden) or (isinstance(node,ast.FunctionDef) and node is not self.fn): raise FixtureExecutionError('unsupported syntax: '+type(node).__name__)
            if isinstance(node,ast.Name) and node.id.startswith('_'): raise FixtureExecutionError('private names forbidden')
            if isinstance(node,ast.Attribute) and node.attr not in self.METHODS: raise FixtureExecutionError('only allowlisted str methods')
            if isinstance(node,ast.Call) and isinstance(node.func,ast.Name) and node.func.id not in set(self.FUNCTIONS)|{'range','ValueError'}: raise FixtureExecutionError('function call not allowed')
        self.steps=0
    def bound(self,x):
        if type(x) is int and abs(x)>10**15: raise BudgetError('integer bound')
        if type(x) is float and (not math.isfinite(x) or abs(x)>10**15): raise FixtureExecutionError('nonfinite/out-of-range float')
        if isinstance(x,(str,list,tuple,dict,set)) and len(x)>4096: raise BudgetError('object length bound')
        return x
    def tick(self):
        self.steps+=1
        if self.steps>20000: raise BudgetError('step budget')
    def assign(self,target,value,env):
        if isinstance(target,ast.Name):
            if target.id in self.FUNCTIONS or target.id in ('range','ValueError'): raise FixtureExecutionError('cannot shadow builtins')
            env[target.id]=self.bound(value)
        elif isinstance(target,(ast.Tuple,ast.List)):
            if len(target.elts)!=len(value): raise FixtureExecutionError('unpack length')
            for t,v in zip(target.elts,value): self.assign(t,v,env)
        else: raise FixtureExecutionError('only local names/unpacking assignable')
    def expr(self,n,e):
        self.tick()
        if isinstance(n,ast.Constant): return self.bound(n.value)
        if isinstance(n,ast.Name):
            if n.id in e: return e[n.id]
            if n.id in self.FUNCTIONS: return self.FUNCTIONS[n.id]
            raise FixtureExecutionError('unknown name: '+n.id)
        if isinstance(n,(ast.List,ast.Tuple,ast.Set)):
            v=[self.expr(x,e) for x in n.elts];return self.bound(tuple(v) if isinstance(n,ast.Tuple) else set(v) if isinstance(n,ast.Set) else v)
        if isinstance(n,ast.Subscript): return self.bound(self.expr(n.value,e)[self.expr(n.slice,e)])
        if isinstance(n,ast.Slice): return slice(*(self.expr(x,e) if x else None for x in (n.lower,n.upper,n.step)))
        if isinstance(n,ast.UnaryOp):
            x=self.expr(n.operand,e)
            if isinstance(n.op,ast.Not):return not x
            if isinstance(n.op,ast.USub):return self.bound(-x)
            if isinstance(n.op,ast.UAdd):return self.bound(+x)
        if isinstance(n,ast.BinOp):
            a,b=self.expr(n.left,e),self.expr(n.right,e)
            if isinstance(n.op,ast.Add):
                if isinstance(a,(str,list,tuple)) and len(a)+len(b)>4096: raise BudgetError('concat size')
                return self.bound(a+b)
            if isinstance(n.op,ast.Sub):return self.bound(a-b)
            if isinstance(n.op,ast.Mult):
                if isinstance(a,(str,list,tuple)) and type(b)is int and len(a)*max(b,0)>4096:raise BudgetError('repeat size')
                if isinstance(b,(str,list,tuple)) and type(a)is int and len(b)*max(a,0)>4096:raise BudgetError('repeat size')
                return self.bound(a*b)
            if isinstance(n.op,ast.FloorDiv):return self.bound(a//b)
            if isinstance(n.op,ast.Div):return self.bound(a/b)
            if isinstance(n.op,ast.Mod):
                if isinstance(a,str):raise FixtureExecutionError('format operator unsupported')
                return self.bound(a%b)
        if isinstance(n,ast.BoolOp):
            value=self.expr(n.values[0],e)
            for item in n.values[1:]:
                if (isinstance(n.op,ast.And) and not value) or (isinstance(n.op,ast.Or) and value):break
                value=self.expr(item,e)
            return value
        if isinstance(n,ast.Compare):
            a=self.expr(n.left,e)
            for op,other in zip(n.ops,n.comparators):
                b=self.expr(other,e)
                if isinstance(op,ast.Eq):ok=a==b
                elif isinstance(op,ast.NotEq):ok=a!=b
                elif isinstance(op,ast.Lt):ok=a<b
                elif isinstance(op,ast.LtE):ok=a<=b
                elif isinstance(op,ast.Gt):ok=a>b
                elif isinstance(op,ast.GtE):ok=a>=b
                elif isinstance(op,ast.In):ok=a in b
                elif isinstance(op,ast.NotIn):ok=a not in b
                elif isinstance(op,ast.Is):ok=a is b
                elif isinstance(op,ast.IsNot):ok=a is not b
                else:raise FixtureExecutionError('comparison unsupported')
                if not ok:return False
                a=b
            return True
        if isinstance(n,ast.IfExp):return self.expr(n.body if self.expr(n.test,e) else n.orelse,e)
        if isinstance(n,(ast.ListComp,ast.GeneratorExp)):
            if len(n.generators)!=1 or n.generators[0].is_async:raise FixtureExecutionError('single bounded comprehension only')
            g=n.generators[0]; values=self.expr(g.iter,e); need_size(values); out=[]
            for v in values:
                self.tick(); local=e.copy(); self.assign(g.target,v,local)
                if all(self.expr(cond,local) for cond in g.ifs):out.append(self.expr(n.elt,local))
            return self.bound(out)
        if isinstance(n,ast.Call):
            if any(k.arg is None for k in n.keywords) or any(isinstance(a,ast.Starred) for a in n.args):raise FixtureExecutionError('no argument expansion')
            args=[self.expr(a,e) for a in n.args];kwargs={k.arg:self.expr(k.value,e) for k in n.keywords}
            if isinstance(n.func,ast.Name):
                name=n.func.id
                if name=='range':
                    value=range(*args,**kwargs);need_size(value);return value
                if name=='ValueError':return ValueError(*args)
                if name not in self.FUNCTIONS:raise FixtureExecutionError('call not allowed')
                if name in ('sum','all','any','min','max','enumerate') and args and not isinstance(args[0],(int,float)):need_size(args[0])
                if name=='str' and any(type(x) not in (str,int,float,bool,type(None)) for x in args):raise FixtureExecutionError('str conversion accepts primitive inputs only')
                value=self.FUNCTIONS[name](*args,**kwargs)
                if name=='enumerate':value=list(value)
                return self.bound(value)
            if isinstance(n.func,ast.Attribute):
                obj=self.expr(n.func.value,e)
                if type(obj)is not str or n.func.attr not in self.METHODS:raise FixtureExecutionError('only actual string methods')
                if n.func.attr=='join':
                    need_size(args[0])
                    if sum(len(x) for x in args[0])+len(obj)*len(args[0])>4096:raise BudgetError('join size')
                if n.func.attr=='replace' and len(args)>1 and len(obj)*(len(args[1])+1)>100000:raise BudgetError('replace size')
                return self.bound(getattr(obj,n.func.attr)(*args,**kwargs))
        raise FixtureExecutionError('unsupported expression: '+type(n).__name__)
    def block(self,body,e):
        for n in body:
            self.tick()
            if isinstance(n,ast.Return):raise Returned(self.expr(n.value,e) if n.value else None)
            elif isinstance(n,ast.Assign):
                value=self.expr(n.value,e)
                for t in n.targets:self.assign(t,value,e)
            elif isinstance(n,ast.AugAssign):self.assign(n.target,self.expr(ast.BinOp(left=n.target,op=n.op,right=n.value),e),e)
            elif isinstance(n,ast.If):self.block(n.body if self.expr(n.test,e) else n.orelse,e)
            elif isinstance(n,ast.For):
                values=self.expr(n.iter,e);need_size(values);broken=False
                for v in values:
                    self.tick();self.assign(n.target,v,e)
                    try:self.block(n.body,e)
                    except ContinueFlow:continue
                    except BreakFlow:broken=True;break
                if not broken:self.block(n.orelse,e)
            elif isinstance(n,ast.Raise):
                if n.cause is not None:raise FixtureExecutionError('raise-from unsupported')
                if isinstance(n.exc,ast.Name) and n.exc.id=='ValueError':raise ValueError
                value=self.expr(n.exc,e)
                if type(value)is not ValueError:raise FixtureExecutionError('only ValueError may be raised')
                raise value
            elif isinstance(n,ast.Expr):self.expr(n.value,e)
            elif isinstance(n,ast.Pass):pass
            elif isinstance(n,ast.Break):raise BreakFlow()
            elif isinstance(n,ast.Continue):raise ContinueFlow()
            else:raise FixtureExecutionError('unsupported statement: '+type(n).__name__)
    def run(self,value):
        self.steps=0
        try:self.block(self.fn.body,{'text':value})
        except Returned as r:return r.value
        return None


def need_size(value):
    if not isinstance(value,(str,list,tuple,set,range)) or len(value)>1024:raise BudgetError('iteration bound')


def check_tests(source,private=False):
    vm=FunctionVM(source); inputs=hidden_inputs() if private else PUBLIC_INPUTS; failures=[]
    forbidden_float=any(isinstance(n,ast.Call) and isinstance(n.func,ast.Name) and n.func.id in ('float','round') for n in ast.walk(vm.fn))
    for i,value in enumerate(inputs,1):
        try:expect=amount_oracle(value); expected={'value':expect}
        except ValueError:expected={'error':'ValueError'}
        try:
            result=vm.run(value); actual={'type':type(result).__name__}
            if type(result) in (int,float,str,bool,type(None)):actual['value']=result
            else:actual['non_integer_return']=True
        except Exception as error:actual={'error':type(error).__name__}
        passed=(actual.get('error')=='ValueError') if 'error'in expected else (actual.get('type')=='int' and actual.get('value')==expected['value'])
        if not passed:failures.append({'case_id':f'T{i:03d}','input':value,'expected':expected,'actual':actual})
    return {'executor':'bounded_python_ast_fixture_cpu','suite':'private' if private else 'public','passed':len(inputs)-len(failures),'total':len(inputs),'failures':failures,'float_or_round_present':forbidden_float,'success':not failures and not forbidden_float}


def resolved_run(root):
    path=Path(root).resolve()
    if path==HOME or not path.is_relative_to(HOME):raise ValueError('run directory must be inside the R14 fixture directory')
    return path


def save_new(path,value):
    with Path(path).open('x',encoding='utf8') as f:json.dump(value,f,ensure_ascii=False,separators=(',',':'))


def new_run(root,scenario='office'):
    if scenario not in SCENARIOS:raise ValueError('unknown scenario')
    root=resolved_run(root);root.mkdir(parents=True,exist_ok=False)
    info={'schema':1,'scenario':scenario,'fixture_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'created_utc':datetime.now(timezone.utc).isoformat()}
    if scenario=='office':info['data_variant']=DATA_VARIANT
    save_new(root/'run.json',info)
    if scenario=='office':
        inv,pay=office_data(); db=sqlite3.connect(root/'ledger.sqlite')
        db.execute('CREATE TABLE invoices(row_id INTEGER,invoice_id TEXT,revision INTEGER,department TEXT,issued_year INTEGER,due_date TEXT,amount_cents INTEGER,status TEXT,currency TEXT)')
        db.execute('CREATE TABLE payments(row_id INTEGER,event_id TEXT,invoice_id TEXT,paid_on TEXT,amount_cents INTEGER,status TEXT,currency TEXT)')
        db.executemany('INSERT INTO invoices VALUES(?,?,?,?,?,?,?,?,?)',inv);db.executemany('INSERT INTO payments VALUES(?,?,?,?,?,?,?)',pay);db.commit();db.close()
    else:(root/'parser.py').write_text(INITIAL_CODE,encoding='utf8')
    (root/'trace.jsonl').touch(exist_ok=False)
    return {'scenario':scenario,'run_dir':str(root),'fixture_sha256':info['fixture_sha256']}


def create_run(scenario,run_dir):return new_run(run_dir,scenario)


def read_json(path):return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def query(root,sql):
    if type(sql)is not str or not sql.strip() or len(sql)>16000:raise ValueError('SQL must be a nonempty bounded string')
    uri=(root/'ledger.sqlite').as_uri()+'?mode=ro'
    conn=sqlite3.connect(uri,uri=True);conn.execute('PRAGMA query_only=ON')
    safe_functions={'count','sum','min','max','avg','total','coalesce','ifnull','nullif','typeof','length','lower','upper','substr','substring','replace','round','abs','group_concat','row_number','rank','dense_rank','first_value','last_value'}
    real_relations = {r[0] for r in conn.execute('SELECT name FROM sqlite_schema UNION SELECT name FROM sqlite_temp_schema')}
    real_relations.update({'sqlite_schema','sqlite_master','sqlite_temp_schema','sqlite_temp_master'})
    def authorize(action,a,b,db,trigger):
        if action in (sqlite3.SQLITE_SELECT,sqlite3.SQLITE_RECURSIVE):return sqlite3.SQLITE_OK
        if action==sqlite3.SQLITE_READ:
            if db=='main' and a in ('invoices','payments'):return sqlite3.SQLITE_OK
            # SQLite reports count(*) on a CTE with no database/column.
            # Underlying real tables still pass through the allowlist above.
            if db is None and b=='' and (a in ('invoices','payments') or a not in real_relations):return sqlite3.SQLITE_OK
        if action==sqlite3.SQLITE_FUNCTION and (b or '').lower() in safe_functions:return sqlite3.SQLITE_OK
        return sqlite3.SQLITE_DENY
    conn.set_authorizer(authorize);deadline=time.monotonic()+2
    conn.set_progress_handler(lambda:int(time.monotonic()>deadline),1000)
    try:
        cursor=conn.execute(sql);rows=cursor.fetchmany(201)
        if len(rows)>200:raise ValueError('query exceeds 200 returned rows; aggregate or narrow the result')
        return {'columns':[c[0] for c in cursor.description],'rows':[list(r) for r in rows],'row_count':len(rows),'read_only':True}
    finally:conn.close()


def _dispatch(scenario,name,args,root):
    if type(args)is not dict:raise ValueError('arguments must be an object')
    allowed={'read_resource':{'resource'},'query_ledger':{'sql'},'code_action':{'action','source'},'submit_result':{'result'}}
    if name not in allowed or set(args)-allowed[name]:raise ValueError('unknown tool or argument')
    if name=='read_resource':
        resource=args.get('resource')
        if resource=='overview':
            if scenario=='office':return {'scenario':'office','policy':OFFICE_POLICY,'resources':{'invoices':{'rows':652,'columns':INVOICE_COLS},'payments':{'rows':350,'columns':PAYMENT_COLS}},'query_tables':['invoices','payments'],'record_count':1002,'final_schema_source':'policy'}
            return {'scenario':'code','contract':CODE_CONTRACT,'resources':['parser','tests'],'final_result':{'kind':'code','summary':'brief description','tests_passed':True}}
        if scenario=='office' and resource in ('invoices','payments'):
            db=sqlite3.connect((root/'ledger.sqlite').as_uri()+'?mode=ro',uri=True)
            try:
                cursor=db.execute('SELECT * FROM '+resource+' ORDER BY row_id');rows=cursor.fetchall()
                return {'resource':resource,'columns':[c[0]for c in cursor.description],'rows':[list(r)for r in rows],'row_count':len(rows),'complete':True}
            finally:db.close()
        if scenario=='code' and resource=='parser':return {'resource':'parser','editable_function':'parse_amount_cents','source':(root/'parser.py').read_text(encoding='utf8')}
        if scenario=='code' and resource=='tests':return {'resource':'tests','public_cases':[{'case_id':f'T{i:03d}','input':value}for i,value in enumerate(PUBLIC_INPUTS,1)],'test_rule':'Expected values/errors are derived from the contract; code_action(test) runs the current function. Additional private cases use the same contract.'}
        raise ValueError('resource unavailable in this scenario')
    if name=='query_ledger':
        if scenario!='office':raise ValueError('ledger absent in code scenario')
        return query(root,args.get('sql'))
    if name=='code_action':
        if scenario!='code':raise ValueError('editable function absent in office scenario')
        action=args.get('action')
        if action=='test':
            if 'source'in args:raise ValueError('test executes saved source only')
            source=(root/'parser.py').read_text(encoding='utf8');result=check_tests(source)
            result['source_sha256']=hashlib.sha256(source.encode()).hexdigest();return result
        if action=='replace':
            source=args.get('source');FunctionVM(source)
            count=len(list(root.glob('parser-version-*.py')))
            with (root/f'parser-version-{count:03d}.py').open('x',encoding='utf8')as f:f.write((root/'parser.py').read_text(encoding='utf8'))
            (root/'parser.py').write_text(source,encoding='utf8')
            return {'saved':True,'function':'parse_amount_cents','source_sha256':hashlib.sha256(source.encode()).hexdigest(),'tests_executed':False}
        raise ValueError('unknown code action')
    if name=='submit_result':
        result=args.get('result')
        if type(result)is not dict or result.get('kind')!=scenario:raise ValueError('result.kind must match scenario')
        if len(json.dumps(result,ensure_ascii=False))>30000:raise ValueError('submission too large')
        number=len(list(root.glob('submission-*.json')))
        save_new(root/f'submission-{number:03d}.json',result)
        return {'stored':True,'submission_number':number,'note':'Stored for independent validation; this is not a correctness verdict.'}
    raise ValueError('unknown tool')


def dispatch(*args):
    if len(args)==3:name,arguments,root=args;scenario=None
    elif len(args)==4:scenario,name,arguments,root=args
    else:raise TypeError('dispatch(name,args,root) or dispatch(scenario,name,args,root)')
    root=resolved_run(root);info=read_json(root/'run.json')
    if scenario is not None and scenario!=info['scenario']:raise ValueError('scenario mismatch')
    scenario=info['scenario'];start=time.perf_counter_ns()
    try:result={'ok':True,'result':_dispatch(scenario,name,arguments,root)}
    except Exception as error:result={'ok':False,'error':{'type':type(error).__name__,'message':str(error)}}
    event={'tool':name,'arguments':arguments,'output':result,'elapsed_ns':time.perf_counter_ns()-start}
    with (root/'trace.jsonl').open('a',encoding='utf8')as f:f.write(json.dumps(event,ensure_ascii=False,separators=(',',':'))+'\n')
    return result


def normalize_office(result):
    result=json.loads(json.dumps(result))
    for key,field in [('by_department','department'),('by_year','issued_year')]:
        if key in result:result[key]=sorted(result[key],key=lambda r:r[field])
    return result


def exact_typed(left,right):
    if type(left) is not type(right):return False
    if isinstance(right,dict):return left.keys()==right.keys() and all(exact_typed(left[k],v)for k,v in right.items())
    if isinstance(right,list):return len(left)==len(right)and all(exact_typed(a,b)for a,b in zip(left,right))
    return left==right


def validate_final(final,root):
    root=resolved_run(root);info=read_json(root/'run.json');scenario=info['scenario']
    trace=[json.loads(line)for line in (root/'trace.jsonl').read_text(encoding='utf8').splitlines()]
    completed=[x for x in trace if x['output'].get('ok')]
    readset={x['arguments']['resource']for x in completed if x['tool']=='read_resource'}
    submissions=sorted(root.glob('submission-*.json'))
    checks={'overview_read':'overview'in readset,'structured_submission_stored':bool(submissions),'fixture_unchanged':info['fixture_sha256']==hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    submitted=read_json(submissions[-1])if submissions else None
    details={}
    if scenario=='office':
        checks['complete_invoices_and_payments_read']={'invoices','payments'}<=readset
        checks['actual_read_only_SQL_executed']=any(x['tool']=='query_ledger'for x in completed)
        expected=office_truth(info.get('data_variant','base'))
        try:checks['exact_structured_ground_truth']=exact_typed(normalize_office(submitted),normalize_office(expected))
        except Exception:checks['exact_structured_ground_truth']=False
        details={'expected':expected,'submitted':submitted,'input_records':1002,'ground_truth_method':'Independent Python grouping and integer arithmetic, not the agent SQL.'}
    else:
        source=(root/'parser.py').read_text(encoding='utf8');source_sha=hashlib.sha256(source.encode()).hexdigest()
        tests=[x for x in completed if x['tool']=='code_action'and x['arguments'].get('action')=='test']
        replaces=[i for i,x in enumerate(trace)if x['tool']=='code_action'and x['arguments'].get('action')=='replace'and x['output'].get('ok')]
        checks['parser_and_tests_read']={'parser','tests'}<=readset
        checks['baseline_test_before_first_edit']=bool(replaces)and any(x['tool']=='code_action'and x['arguments'].get('action')=='test'and x['output'].get('ok')for x in trace[:replaces[0]])
        checks['function_actually_changed']=source!=INITIAL_CODE
        checks['current_source_public_test_passed']=any(x['output']['result']['source_sha256']==source_sha and x['output']['result']['success']for x in tests)
        try:private=check_tests(source,True)
        except Exception as error:private={'success':False,'error':str(error)}
        checks['private_contract_cases_pass']=private['success']
        checks['truthful_structured_submission']=type(submitted)is dict and submitted.get('kind')=='code'and submitted.get('tests_passed')is True and type(submitted.get('summary'))is str and bool(submitted['summary'].strip())
        details={'private_test':private,'source_sha256':source_sha,'submission':submitted,'execution_scope':'Fixed function only; bounded Python AST interpreter, not a full repository or shell.'}
    return {'success':all(checks.values()),'scenario':scenario,'checks':checks,'details':details,'tool_calls':len(trace),'successful_tool_calls':len(completed),'final_text_used_for_scoring':False}


def evaluate(scenario,run_dir,messages):
    if read_json(resolved_run(run_dir)/'run.json')['scenario']!=scenario:raise ValueError('scenario mismatch')
    return validate_final(messages,run_dir)


def ground_truth(scenario):
    if scenario=='office':return office_truth()
    if scenario=='code':return {'contract':CODE_CONTRACT,'private_case_count':len(hidden_inputs()),'success_rule':'Changed function, actual passing current public tests, private exact integer/error contract tests, truthful structured submission.'}
    raise ValueError('unknown scenario')
