"""Record button activity from the live tester's event stream for N seconds and
summarise which inputs fired. Usage: python3 record.py SECONDS"""
import json, sys, time, urllib.request
dur = float(sys.argv[1])
r = urllib.request.urlopen('http://localhost:8765/events', timeout=dur + 5)
end = time.time() + dur
seen, mode_seen, prev, conn = {}, False, 0, set()
r.fp.raw._sock.settimeout(0.5)
while time.time() < end:
    try: line = r.readline().decode().strip()
    except Exception: continue
    if not line.startswith('data:'): continue
    s = json.loads(line[5:]); conn.add(s['connected'])
    rising = s['bits'] & ~prev
    for i in range(16):
        if rising >> i & 1: seen[i + 1] = seen.get(i + 1, 0) + 1
    combo = bin(s['bits']).count('1')
    if combo > 1: seen.setdefault('multi', set()).add(s['bits'])
    mode_seen |= bool(s['mode']); prev = s['bits']
multi = seen.pop('multi', set())
print('connected states:', conn)
print('bars that fired (bar: presses):', dict(sorted(seen.items())) or 'none')
print('never fired:', [i for i in range(1, 17) if i not in seen])
print('mode fired:', mode_seen)
if multi: print('several bars on at once:', [format(m, '016b')[::-1] for m in multi][:6])
