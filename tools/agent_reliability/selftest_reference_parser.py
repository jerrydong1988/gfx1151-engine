def parse_amount_cents(text):
    if not isinstance(text, str):
        raise ValueError('text')
    value = text.strip(' \t\r\n')
    if not value:
        raise ValueError('empty')
    sign = 1
    if value[0] in ('+', '-'):
        if value[0] == '-':
            sign = -1
        value = value[1:]
    if value[:1] in ('¥', '￥'):
        value = value[1:]
    parts = value.split('.')
    if len(parts) > 2:
        raise ValueError('points')
    whole = parts[0]
    fraction = parts[1] if len(parts) == 2 else ''
    if len(parts) == 2 and not (1 <= len(fraction) <= 2):
        raise ValueError('fraction length')
    groups = whole.split(',')
    if len(groups) > 1:
        if not (1 <= len(groups[0]) <= 3):
            raise ValueError('first group')
        for group in groups[1:]:
            if len(group) != 3:
                raise ValueError('group')
    digits = ''.join(groups)
    if not digits or any(c not in '0123456789' for c in digits + fraction):
        raise ValueError('digits')
    cents = int(digits) * 100 + int((fraction + '00')[:2])
    if cents > 99999999999:
        raise ValueError('range')
    return sign * cents
