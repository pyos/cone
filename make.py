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
    ],
}

bins = {
    'test_simple':      ['libco/coro.c', 'tests/simple.c'],
    'test_time_switch': ['libco/coro.c', 'tests/time_switch.c'],
    'test_cno':         ['libco/coro.c', 'tests/cno.c'],
}

def scandeps(files, deps):
    for file in files - deps.keys():
        deps[file] = cdeps = set()
        with open(file) as fd:
            for line in filter(None, map(re.compile(r'^\s*#\s*include\s+"(.+?)"\s*$').match, fd)):
                cdeps.add(os.path.normpath(os.path.join(os.path.dirname(file), line.group(1))))
        scandeps(cdeps, deps)
        cdeps |= set().union(*map(deps.__getitem__, cdeps))
    return {k: deps[k] for k in files}

def template(data, params, isalnum=re.compile('\w').match):
    paramre = re.compile(r'(?P<L>\w?)(~(?P<name>\w+)~)(?P<R>\w?)')
    def subst(match):
        try:
            value = params[match.group('name')]
        except KeyError:
            raise Exception('unknown parameter {}'.format(match.group()))
        if match.group('L'):
            value = re.sub(r'^(_|._)_?(?:struct\s+)?(?:co_)?', r'\1', match.group('L') + '_' + value)
        if match.group('R'):
            value = re.sub(r'^(?:struct\s+)?(.+?)_?(_.|_)$', r'\1\2', value + '_' + match.group('R'))
        return value
    return paramre.sub(subst, data)

if len(sys.argv) > 1 and sys.argv[1] == 'template':
    sys.stdout.write(template(sys.stdin.read(), dict(zip(*[iter(sys.argv[2:])] * 2))))
else:
    temporaries = {'obj'}
    with open('Makefile', 'w') as m:
        print('.PHONY: all clean clean_all', file=m)
        print('all:', *map('obj/{}'.format, bins), file=m)

        for mod, targets in git_modules.items():
            print('{0}/.git: .gitmodules\n\tgit submodule update --init {0}'.format(mod), file=m)
            for target in targets:
                print('{0}/{1}: {0}/.git\n\t$(MAKE) -C {0} {1}'.format(mod, target), file=m)

        for bin, srcs in bins.items():
            objs = ' '.join('obj/' + src.rpartition('.')[0] + '.o' for src in srcs)
            print('obj/{0}: {1}\n\t$(CC) {2} {1} $(CFLAGS) -o obj/{0} {3}'.format(bin, objs,
                ' '.join(map('-L{}'.format, libpaths)),
                ' '.join(map('-l{}'.format, libs))), file=m)

        for name, paramsets in templates.items():
            for params in paramsets:
                outname = template(name, params)
                encoded = ' '.join(shlex.quote(p) for ps in params.items() for p in ps)
                temporaries.add(outname)
                print('{2}: {1} make.py\n\t{0} make.py template {3} < {1} > {2}'.format(sys.executable, name, outname, encoded), file=m)
                with open(name) as inp, open(outname, 'w') as out:
                    out.write(template(inp.read(), params))

        for src, deps in scandeps(set().union(*bins.values()), {}).items():
            obj = 'obj/' + src.rpartition('.')[0] + '.o'
            print('{}: {} {}'.format(obj, src, ' '.join(deps)), file=m)
            print('\t@mkdir -p {}'.format(os.path.dirname(obj)), file=m)
            print('\t$(CC) -std=c11 -I. -Wall -Wextra -fPIC $(CFLAGS) {2} -D_GNU_SOURCE -c {0} -o {1}'.format(src, obj,
                ' '.join(map('-I{}'.format, incpaths))), file=m)

        print('clean:\n\trm -rf', *temporaries, file=m)
        print('clean_all: clean\n\trm Makefile', file=m)
    os.execl('/usr/bin/env', '/usr/bin/env', 'make', *sys.argv[1:])
