"""One bounded regeneration after a verified output-limit response.

The caller retains the request messages, discards the truncated assistant turn,
and dispatches no tools from that response. Transport failures are not retries.
After escalation the larger cap remains in effect for the rest of the task.
"""


class BudgetRecovery:
    def __init__(self, initial_tokens, maximum_tokens, context_capacity):
        for value in (initial_tokens, maximum_tokens, context_capacity):
            if type(value) is not int or value <= 0:
                raise ValueError('Positive integer token limits are required')
        if maximum_tokens < initial_tokens:
            raise ValueError('Maximum cannot be below the initial request cap')
        self.current_tokens = initial_tokens
        self.maximum_tokens = maximum_tokens
        self.context_capacity = context_capacity
        self.used = 0

    def on_length(self, record, remaining_task_tokens):
        prompt, output = record['prompt_tokens'], record['output_tokens']
        if any(type(x) is not int or x < 0 for x in (prompt, output, remaining_task_tokens)):
            raise ValueError('Invalid verified token accounting')
        room = max(0, self.context_capacity - prompt)
        result = dict(retry=False, previous_cap=self.current_tokens,
                      context_remaining=room, completed_tools_replayed=False,
                      truncated_response_discarded=True)
        if room <= self.current_tokens:
            return result | {'reason': 'context_limit'}
        if output != self.current_tokens:
            return result | {'reason': 'unexplained_length_stop'}
        if self.used or self.current_tokens == self.maximum_tokens:
            return result | {'reason': 'output_limit_no_retry_remaining'}
        proposed = min(self.maximum_tokens, room)
        if remaining_task_tokens < proposed:
            return result | {'reason': 'task_output_limit'}
        self.used += 1
        self.current_tokens = proposed
        return result | dict(retry=True, reason='output_limit_retry', next_cap=proposed,
                             retry_number=self.used)
