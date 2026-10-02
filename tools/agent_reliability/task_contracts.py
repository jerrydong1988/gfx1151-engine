"""Submission shape only. No dataset values, SQL solution, or reference answer."""
def obj(properties):
    return {'type':'object','properties':properties,'required':list(properties),'additionalProperties':False}

def integers(*names):
    return {name:{'type':'integer'} for name in names}

def office_schema():
    totals=integers('invoice_count','receivable_cents','applied_payment_cents','balance_cents')
    return obj({
        'kind':{'type':'string','const':'office'},
        **integers('eligible_invoice_count','receivable_cents','applied_payment_cents','balance_cents'),
        'by_department':{'type':'array','items':obj({'department':{'type':'string'},**totals})},
        'by_year':{'type':'array','items':obj({'issued_year':{'type':'integer'},**totals})},
        'overdue_top5':{'type':'array','items':obj({'invoice_id':{'type':'string'},'department':{'type':'string'},
            'due_date':{'type':'string'},'balance_cents':{'type':'integer'}})},
        'invoice_exclusions':obj(integers('conflicting_latest_revision','non_posted','non_cny','missing_amount')),
        'payment_exclusions':obj(integers('conflicting_event','not_settled','non_cny','after_cutoff','ineligible_or_missing_invoice')),
    })

def code_schema():
    return obj({'kind':{'type':'string','const':'code'},'summary':{'type':'string'},'tests_passed':{'type':'boolean'}})
