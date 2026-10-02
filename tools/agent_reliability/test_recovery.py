from tool_recovery import ToolRecovery
T=[{'function':{'name':'read_data'}}]
p=ToolRecovery(2)
assert p.feedback('timeout','uncertain',T) is None and p.used==0
assert p.feedback('length','budget',T) is None and p.used==0
x=p.feedback('invalid_tool_call','Unknown function: fake',T)
assert x and 'fake' in x['content'] and 'read_data' in x['content'] and p.used==1
assert p.feedback('invalid_tool_call','Unknown function: fake',T) is None
assert p.feedback('invalid_tool_call','Missing field: path',T) and p.used==2
assert p.feedback('invalid_tool_call','third distinct failure',T) is None
assert ToolRecovery(0).feedback('invalid_tool_call','x',T) is None
for value in (-1,3,True):
    try:ToolRecovery(value)
    except ValueError:pass
    else:raise AssertionError(value)
print('PASS recovery boundaries, duplicate-error stop, no timeout/length replay')
