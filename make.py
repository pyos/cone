#!/usr/bin/env python3
import re
import os
import sys
import shlex

git_modules = {
    'libcno': ['obj/libcno.a'],
}

incpaths = ['libcno']
libpaths = ['libcno/obj']
libs = ['dl', 'cno']

templates = {
    'libco/generic/vec~T~.h': [
        {'T': 'struct co_closure'},
        {'T': 'struct co_call_at'},
    ]
}

bins = {
    'test_simple':      ['libco/coro.c', 'tests/simple.c'],
    'test_time_switch': ['libco/coro.c', 'tests/time_switch.c'],
    'test_cno':         ['libco/coro.c', 'tests/cno.c'],
}

def scandeps(files, deps):
    for file in files:
        cdeps = set()
        try:
            with open(file) as fd:
                for line in fd:
                    line = line.strip()
                    if not line.startswith('#'):
                        continue
                    line = line[1:].lstrip()
                    if not line.startswith('include '):
                        continue
                    line = line[7:].lstrip()
                    if not line.startswith('"'):
                        continue
                    cdeps.add(os.path.normpath(os.path.join(os.path.dirname(file), line[1:-1])))
        except IOError:
            pass
        deps.update(scandeps({c for c in cdeps if c not in deps}, deps))
        deps[file] = cdeps.union(*map(deps.__getitem__, cdeps))
    return {k: v for k, v in deps.items() if k in files}

def template(data, params, paramre=re.compile('~(\w+)~'), join='_'):
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

if len(sys.argv) == 1:
    sources = scandeps(set().union(*bins.values()), {})
    temporaries = {'obj'}
    with open('Makefile', 'w') as m:
        print('.PHONY: all clean', file=m)
        print('all:', *map('obj/{}'.format, bins), file=m)

        for mod, targets in git_modules.items():
            print('{0}/.git: .gitmodules\n\tgit submodule update --init {0}'.format(mod), file=m)
            for target in targets:
                print('{0}/{1}: {0}/.git\n\t$(MAKE) -C {0} {1}'.format(mod, target), file=m)

        for bin, srcs in bins.items():
            objs = ' '.join('obj/' + src.rpartition('.')[0] + '.o' for src in srcs)
            print('obj/{0}: {1}\n\t$(CC) {2} {1} -o obj/{0} {3}'.format(bin, objs,
                ' '.join(map('-L{}'.format, libpaths)),
                ' '.join(map('-l{}'.format, libs))), file=m)

        for src, deps in sources.items():
            obj = 'obj/' + src.rpartition('.')[0] + '.o'
            print('{}: {} {}'.format(obj, src, ' '.join(deps)), file=m)
            print('\t@mkdir -p {}'.format(os.path.dirname(obj)), file=m)
            print('\t$(CC) -std=c11 -I. -Wall -Wextra -fPIC $(CFLAGS) {2} -D_GNU_SOURCE -c {0} -o {1}'.format(src, obj,
                ' '.join(map('-I{}'.format, incpaths))), file=m)

        for name, paramsets in templates.items():
            for params in paramsets:
                outname = template(name, params, join='~~')
                encoded = ' '.join(shlex.quote(p) for ps in params.items() for p in ps)
                temporaries.add(outname)
                print('{2}: {1} make.py\n\t{0} make.py template {3} < {1} > {2}'.format(sys.executable, name, outname, encoded), file=m)

        print('clean:\n\trm -rf', *temporaries, file=m)
    os.execl('/usr/bin/env', '/usr/bin/env', 'make')
elif sys.argv[1] == 'template':
    sys.stdout.write(template(sys.stdin.read(), dict(zip(*[iter(sys.argv[2:])] * 2))))
