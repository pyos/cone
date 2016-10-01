#!/usr/bin/env python3
import re
import sys


def generate(data, params, paramre=re.compile('~(\w+)~'), join='_'):
    def subst(match):
        start, end = match.span()
        try:
            value = params[match.group(1)]
        except KeyError:
            raise Exception('unknown parameter {}'.format(match.group()))
        if start > 0 and match.string[start - 1].isalnum():
            if value.startswith('struct '):
                value = value[7:]
            if value.startswith('co_'):
                value = value[3:]
            value = join + value
        if end < len(match.string) and match.string[end].isalnum():
            value = value + join
        return value
    return paramre.sub(subst, data)


_, name, *params = sys.argv
params = {name: value for p in params for name, _, value in [p.partition('=')]}
with open(name) as fd:
    code = generate(fd.read(), params)
with open(generate(name, params, join='~~'), 'w') as fd:
    fd.write(code)
