"""probe_run.py - helpers every probe-kit runner (<kit>_run.py) shares (cu18 t02, 2026-11-07; they were copied into
each runner)."""
import os
import subprocess
import time


def say(*a):
    print(*a, flush=True)


def running(name):
    """True while a process with this image name (e.g. StarWarsG.exe) runs."""
    r = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq %s' % name, '/NH'], capture_output=True, text=True)
    return name.lower() in r.stdout.lower()


def set_state(where, **kv):
    """Write <where>\\run_status.txt (k=v lines plus the time) for a background caller."""
    with open(os.path.join(where, 'run_status.txt'), 'w') as f:
        for k, v in kv.items():
            f.write('%s=%s\n' % (k, v))
        f.write('time=%s\n' % time.strftime('%Y-%m-%d %H:%M:%S'))


def scan_once(proc, needle, record_re, seen, out):
    """One heap scan of `proc` (an a1mem process) for `needle`; appends the records `record_re` matches that were not
    seen before to `out`. Returns (new records, scan stats)."""
    hits, stats = proc.scan([needle])
    new = 0
    for _, tail in hits[needle]:
        m = record_re.match(tail)
        if not m:
            continue
        try:
            text = m.group(0).decode('ascii')
        except UnicodeDecodeError:
            continue  # a stale heap fragment with garbage bytes (a2f's record_re allows any byte in one field; run 191238)
        if text in seen:
            continue
        seen.add(text)
        out.write(text + '\n')
        new += 1
    out.flush()
    return new, stats
