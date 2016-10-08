#/usr/bin/env python3
import os
import re
import time

def readlog(path):
    with open(path) as fd:
        for line in fd:
            m = re.match(r'^\[(\d+)\|(\d+)\] (\d+): (.*)$', line.rstrip('\n'))
            if not m:
                print('warning: runtime error in {}:'.format(path), line)
                continue
            yield tuple(map(int, m.group(1, 2, 3))) + (m.group(4),)

def readlogs(root):
    data = {}
    for file in os.listdir(root):
        if file.endswith('.stderr'):
            data[int(file[:-7])] = list(readlog(os.path.join(root, file)))
    return data

if __name__ == '__main__':
    try:
        logs = readlogs('obj/tests/deck')
    except FileNotFoundError:
        print('obj/tests/deck not found; did you run `make tests/deck CFLAGS="-O3 -DDECK_DEBUG=1"`?')
        exit(1)
    unified = {}
    for pid, log in logs.items():
        if not log:
            print('warning: process', pid, 'has empty log; did you specify -DDECK_DEBUG=1?')
        for rtc, ltc, rqpid, kind in log:
            c = unified.setdefault(ltc, {})
            assert pid not in c, 'multiple events at time {} in process {}'.format(ltc, pid)
            c[pid] = rtc, rqpid, kind
    held_by = None
    requested = {pid: set() for pid in logs}
    for ltc in sorted(unified):
        for pid, (rtc, rqpid, kind) in unified[ltc].items():
            t = time.strftime("%H:%M:%S", time.gmtime(rtc / 1000000)) + '|' + str(ltc)
            if kind == 'acquire':
                if pid != rqpid:
                    print(t, pid, 'somehow knows about', rqpid, 'taking the lock')
                if requested[pid] != set(logs):
                    print(t, pid, 'took the lock without asking', ', '.join(map(str, set(logs) - requested[pid])))
                if held_by is not None:
                    print(t, pid, 'took the lock held by', held_by)
                requested[pid].clear()
                held_by = pid
            elif kind == 'release':
                if pid == rqpid:
                    if held_by != pid:
                        print(t, pid, 'released the lock held by', held_by)
                    held_by = None
            elif kind == 'request':
                requested[rqpid].add(pid)
            elif kind == 'cancel':
                requested[rqpid].discard(pid)
                print(t, rqpid, 'relinquished its request from', pid)
            else:
                assert False, '{}?'.format(kind)
    print('consistency check finished; if there is no other output, all is fine')
