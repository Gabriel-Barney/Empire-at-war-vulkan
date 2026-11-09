#!/usr/bin/env python3
"""cu9h_dbg.py - write watchpoints on the HUD bombing-run fields of the running retail game (roadmap group cu9h t02;
docs/re/HUD_GAPS.md sec. 9.9 items 3 and 4).

  cu9h_dbg.py --out LOG [--pid N | --wait-process-sec 600] [--ground-budget 240] [--ground-refills 20]
              [--max-hits 20000] [--stop-file PATH] [--min-stack 2] [--frames 16] [--allow-unknown-exe]

The same project-local debugger technique as ../cu8h_probes/cu8h_dbg.py (Windows debug API through ctypes; its
structures and kernel32 / dbghelp bindings are imported from there), with four hardware WRITE watchpoints on the HUD
object (a static object at RVA 0xb27f60):
  Dr0  +0x26dc (4 bytes)  ground point x    } item 4: who writes the blob position (budgeted: --ground-budget hits,
  Dr3  +0x26e4 (4 bytes)  ground point z    } refilled each time targeting turns on, up to --ground-refills times)
  Dr1  +0x20b2 (1 byte)   targeting flag      item 3: does a click on the DISABLED button run its callback (0x346a80 ->
                                              0x2e7950 sets the flag to 1)? every write, always on
  Dr2  +0x20c0 (8 bytes)  targeting blob ptr  blob created / destroyed, always on
Each hit logs the slot, the thread, the writer (RIP, as an RVA), a stack walk (dbghelp StackWalk64) and a snapshot of
the bombing-run state at that moment (cu9h_mem.snapshot: flag, blob colour, ground point, cursor, the local record,
the button's disabled bit and dial). A data watchpoint traps after the write, so the snapshot holds the new value.

Arming: only in a campaign land battle (GMM mode type 1 with a mode stack depth >= --min-stack, 2 = land over the
galaxy), so the main-menu background battle never arms; disarmed when the battle mode goes. The game's memory is
never written; only the threads' debug registers are set, and they are cleared before the debugger detaches.
Stops: game exit (EXIT), --stop-file appears, --max-hits. DebugSetProcessKillOnExit(FALSE) keeps the game alive.
Never kill this process while it is attached: armed debug registers without a debugger crash the game. Create the
stop file instead (cu9h_run.py does).

Log lines: CU9HDBG 1, ATTACH, BATTLE, ARM mask= why=, HIT n= slot= what= val= ... walk=rva,rva,.. (the stack walk, last on
the line), BUDGET, BATTLE_END, STOP why=, DETACH ok=, EXIT.
Offline check: cu9h_dbg.py --selftest (a fake process writes the four fields in its own memory).
"""
import argparse
import ctypes
import os
import struct
import subprocess
import sys
import tempfile
import threading
import time

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'cu8h_probes'))
import cu8h_dbg as D  # noqa: E402  (k32, dbghelp, Ctx, STACKFRAME64, Log, kv, constants, find_game)
import cu9h_mem as M  # noqa: E402

k32, dbghelp, kv = D.k32, D.dbghelp, D.kv
OFF_DR = (0x48, 0x50, 0x58, 0x60)
OFF_DR6, OFF_DR7, OFF_RIP, OFF_RSP = D.OFF_DR6, D.OFF_DR7, D.OFF_RIP, D.OFF_RSP
LEN_BITS = {1: 0, 2: 1, 8: 2, 4: 3}
# slot -> (HUD offset, length, name, budgeted)
SLOTS = {0: (M.HUD_GROUND, 4, 'ground_x', True), 1: (M.HUD_FLAG, 1, 'flag', False),
         2: (M.HUD_BLOB, 8, 'blob', False), 3: (M.HUD_GROUND + 8, 4, 'ground_z', True)}
ALWAYS = frozenset((1, 2))
GROUND = frozenset((0, 3))


def dr7_for(mask, old):
    v = old & ~0xFF & ~(0xFFFF << 16)
    for s in mask:
        v |= 1 << (2 * s)
        v |= (0x1 | (LEN_BITS[SLOTS[s][1]] << 2)) << (16 + 4 * s)
    return v


class Tracer:
    def __init__(self, a, log):
        self.a, self.log = a, log
        self.pid = a.pid
        self.hp = None
        self.base = None
        self.hud = None
        self.image_end = None
        self.threads = {}
        self.applied = frozenset()
        self.want = frozenset()
        self.break_pending = False
        self.stop_why = None
        self.hits = 0
        self.ground_hits = 0
        self.budget = a.ground_budget
        self.refills = 0
        self.battle_mode = None
        self.prev_flag = None
        self.ctx = D.Ctx()
        self.ctx2 = D.Ctx()
        self.ev = ctypes.create_string_buffer(512)
        self.sym = False
        self.last_poll = 0.0

    # memory
    def read(self, addr, n):
        if not addr:
            return None
        buf = ctypes.create_string_buffer(n)
        got = ctypes.c_size_t()
        if not k32.ReadProcessMemory(self.hp, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)) or got.value != n:
            return None
        return buf.raw

    def rva(self, x):
        if self.base and self.image_end and self.base <= x < self.image_end:
            return '%x' % (x - self.base)
        return 'm:%x' % x

    def snap(self):
        if self.a.no_gmm:  # selftest: only the HUD fields exist in the fake
            s = {k: None for k in M.KEYS}
            h = self.read(self.hud + M.HUD_FLAG, 0x18)
            if h:
                s['flag'] = h[0]
                s['blob'] = struct.unpack_from('<Q', h, M.HUD_BLOB - M.HUD_FLAG)[0]
            g = self.read(self.hud + M.HUD_GROUND, 12)
            s['ground'] = struct.unpack('<fff', g) if g else None
            return s
        return M.snapshot(self.read, self.base)

    # debug registers
    def set_dr(self, h, mask):
        c = self.ctx2
        if not c.get(h, D.CONTEXT_DEBUG_REGISTERS):
            return False
        c.setflags(D.CONTEXT_DEBUG_REGISTERS)
        for s in range(4):
            c.setq(OFF_DR[s], self.hud + SLOTS[s][0] if s in mask else 0)
        c.setq(OFF_DR6, 0)
        c.setq(OFF_DR7, dr7_for(mask, c.q(OFF_DR7)))
        return c.set(h)

    def apply(self, mask, why):
        """Only inside a debug event (all threads frozen)."""
        ok = bad = 0
        for tid, h in list(self.threads.items()):
            if self.set_dr(h, mask):
                ok += 1
            else:
                bad += 1
        self.applied = frozenset(mask)
        self.log('ARM ' + kv(mask=''.join(str(s) for s in sorted(mask)) or 'none', why=why, threads=ok, failed=bad,
                             budget=self.budget, refills=self.refills, t='%.3f' % time.time()))

    def request_break(self):
        if not self.break_pending:
            self.break_pending = True
            if not k32.DebugBreakProcess(self.hp):
                self.break_pending = False
                self.log('BREAK_FAILED ' + kv(err=ctypes.get_last_error()))

    def desired(self):
        """The mask the game state asks for (polled outside debug events)."""
        if self.a.no_gmm:
            battle = True
            mode = 1
        else:
            g = self.read(self.base + M.RVA_GMM, 0x58)
            if not g:
                return self.want
            mt = struct.unpack_from('<i', g, 0x1C)[0]
            mode = struct.unpack_from('<Q', g, 0x38)[0]
            st = struct.unpack_from('<i', g, 0x50)[0]
            battle = mt == 1 and st >= self.a.min_stack and bool(mode)
        if not battle:
            if self.battle_mode is not None:
                self.log('BATTLE_END ' + kv(mode='%x' % self.battle_mode))
            self.battle_mode = None
            return frozenset()
        if mode != self.battle_mode:
            self.battle_mode = mode
            self.budget, self.refills, self.prev_flag = self.a.ground_budget, 0, None
            self.log('BATTLE ' + kv(mode='%x' % mode, t='%.3f' % time.time()))
        return ALWAYS | (GROUND if self.budget > 0 else frozenset())

    # stack walk
    def walk(self, h):
        c = self.ctx
        rip, rsp = c.q(OFF_RIP), c.q(OFF_RSP)
        if not self.sym:
            return [rip]
        cc = self.ctx2
        ctypes.memmove(cc.addr, c.addr, D.CTX_SIZE)
        fr = D.STACKFRAME64()
        fr.AddrPC.Offset, fr.AddrPC.Mode = rip, D.AddrModeFlat
        fr.AddrStack.Offset, fr.AddrStack.Mode = rsp, D.AddrModeFlat
        fr.AddrFrame.Offset, fr.AddrFrame.Mode = cc.q(0xA0), D.AddrModeFlat
        out = []
        for _ in range(self.a.frames):
            if not dbghelp.StackWalk64(0x8664, self.hp, h, ctypes.byref(fr), ctypes.c_void_p(cc.addr), None,
                                       D.FN_TABLE_ACCESS, D.FN_MODULE_BASE, None):
                break
            pc = fr.AddrPC.Offset
            if not pc:
                break
            out.append(pc)
        return out or [rip]

    def on_hit(self, tid, h, fired):
        c = self.ctx
        if not c.get(h, D.CONTEXT_ALL):
            self.log('CTX_FAIL ' + kv(tid=tid, err=ctypes.get_last_error()))
            return
        stack = ','.join(self.rva(x) for x in self.walk(h))
        s = self.snap()
        for slot in sorted(fired):
            off, n, name, budgeted = SLOTS[slot]
            raw = self.read(self.hud + off, n)
            if name == 'flag':
                val = raw[0] if raw else '-'
            elif name == 'blob':
                val = '%x' % struct.unpack('<Q', raw)[0] if raw else '-'
            else:
                val = '%.3f' % struct.unpack('<f', raw)[0] if raw else '-'
            self.hits += 1
            self.log('HIT ' + kv(n=self.hits, slot=slot, what=name, val=val, tid=tid, rip=self.rva(c.q(OFF_RIP)),
                                 t='%.3f' % time.time()) + ' ' + M.line(s) + ' walk=' + stack)
            if budgeted:
                self.ground_hits += 1
                self.budget -= 1
        # Dr6 clear in this thread
        cc = self.ctx2
        if cc.get(h, D.CONTEXT_DEBUG_REGISTERS):
            cc.setflags(D.CONTEXT_DEBUG_REGISTERS)
            cc.setq(OFF_DR6, 0)
            cc.set(h)
        new = set(self.applied)
        if 1 in fired:
            fl = s.get('flag')
            if fl == 1 and self.prev_flag != 1 and self.refills < self.a.ground_refills:
                self.refills += 1
                self.budget = self.a.ground_budget
                self.log('BUDGET ' + kv(why='targeting_on', budget=self.budget, refills=self.refills))
            self.prev_flag = fl
        if self.budget <= 0:
            new -= GROUND
        elif self.applied:
            new |= GROUND
        new = frozenset(new)
        if new != self.applied:
            self.apply(new, 'budget' if self.budget <= 0 else 'refill')
        self.want = new
        if self.a.max_hits and self.hits >= self.a.max_hits:
            self.stop_why = 'max_hits'

    # main loop
    def attach(self):
        self.hp = k32.OpenProcess(D.PROCESS_ALL_ACCESS, False, self.pid)
        if not self.hp:
            raise OSError('OpenProcess(%d) failed: %d' % (self.pid, ctypes.get_last_error()))
        if not k32.DebugActiveProcess(self.pid):
            raise OSError('DebugActiveProcess(%d) failed: %d' % (self.pid, ctypes.get_last_error()))
        k32.DebugSetProcessKillOnExit(False)

    def poll(self):
        now = time.time()
        if now - self.last_poll < 0.05 or self.base is None:
            return
        self.last_poll = now
        if self.a.stop_file and os.path.exists(self.a.stop_file):
            self.stop_why = self.stop_why or 'stop_file'
        w = self.desired()
        # keep a budget-trimmed mask while in the same battle
        if w and self.applied and (self.budget <= 0):
            w = w - GROUND
        self.want = w

    def run(self):
        self.attach()
        detached = exited = False
        try:
            while True:
                self.poll()
                if self.base is not None and (self.stop_why is not None or self.want != self.applied):
                    self.request_break()
                if not k32.WaitForDebugEvent(self.ev, 100):
                    continue
                code, pid, tid = struct.unpack_from('<III', self.ev.raw, 0)
                u = 16
                cont = D.DBG_CONTINUE
                if code == D.CREATE_PROCESS_DEBUG_EVENT:
                    hfile, _hproc, hthread, base = struct.unpack_from('<QQQQ', self.ev.raw, u)
                    if hfile:
                        k32.CloseHandle(hfile)
                    self.base = base
                    lfanew = struct.unpack('<I', self.read(base + 0x3C, 4))[0]
                    soi = struct.unpack('<I', self.read(base + lfanew + 24 + 56, 4))[0]
                    self.image_end = base + soi
                    self.hud = self.a.hud_addr if self.a.hud_addr else base + M.RVA_HUD
                    self.threads[tid] = hthread
                    dbghelp.SymSetOptions(0x2 | 0x4 | 0x10)
                    self.sym = bool(dbghelp.SymInitialize(self.hp, None, True))
                    self.log('ATTACH ' + kv(pid=self.pid, base='%x' % base, hud='%x' % self.hud, size_of_image='%x' % soi,
                                            sym=int(self.sym)))
                elif code == D.CREATE_THREAD_DEBUG_EVENT:
                    hthread = struct.unpack_from('<Q', self.ev.raw, u)[0]
                    self.threads[tid] = hthread
                    if self.applied:
                        self.set_dr(hthread, self.applied)
                elif code == D.EXIT_THREAD_DEBUG_EVENT:
                    self.threads.pop(tid, None)
                elif code == D.LOAD_DLL_DEBUG_EVENT:
                    hfile = struct.unpack_from('<Q', self.ev.raw, u)[0]
                    if hfile:
                        k32.CloseHandle(hfile)
                    if self.sym:
                        dbghelp.SymRefreshModuleList(self.hp)
                elif code == D.EXIT_PROCESS_DEBUG_EVENT:
                    self.log('EXIT ' + kv(code=struct.unpack_from('<I', self.ev.raw, u)[0], hits=self.hits))
                    k32.ContinueDebugEvent(pid, tid, D.DBG_CONTINUE)
                    exited = True
                    break
                elif code == D.EXCEPTION_DEBUG_EVENT:
                    exc = struct.unpack_from('<I', self.ev.raw, u)[0]
                    if exc == D.EXCEPTION_SINGLE_STEP:
                        h = self.threads.get(tid)
                        if h is None:
                            h = k32.OpenThread(D.THREAD_ACCESS, False, tid)
                            self.threads[tid] = h
                        c = self.ctx
                        fired = set()
                        if self.applied and c.get(h, D.CONTEXT_DEBUG_REGISTERS):
                            fired = {s for s in range(4) if c.q(OFF_DR6) & (1 << s)} & set(self.applied)
                        if fired:
                            self.on_hit(tid, h, fired)
                        else:
                            cont = D.DBG_EXCEPTION_NOT_HANDLED
                    elif exc in (D.EXCEPTION_BREAKPOINT, D.STATUS_WX86_BREAKPOINT):
                        self.break_pending = False
                        if self.stop_why is None and self.want != self.applied:
                            self.apply(self.want, 'battle' if self.want else 'no_battle')
                    else:
                        cont = D.DBG_EXCEPTION_NOT_HANDLED
                    if self.stop_why is not None:
                        if self.applied:
                            self.apply(frozenset(), 'stop')
                        self.log('STOP ' + kv(why=self.stop_why, hits=self.hits))
                        k32.ContinueDebugEvent(pid, tid, cont)
                        if self.drain(0.5):
                            exited = True
                        else:
                            detached = self.detach()
                        break
                k32.ContinueDebugEvent(pid, tid, cont)
        finally:
            if not detached and not exited:
                self.emergency_stop()
            if self.sym:
                dbghelp.SymCleanup(self.hp)
            if self.hp:
                k32.CloseHandle(self.hp)
        return self.hits

    def drain(self, seconds):
        t0 = time.time()
        while time.time() - t0 < seconds:
            if not k32.WaitForDebugEvent(self.ev, 50):
                continue
            code, pid, tid = struct.unpack_from('<III', self.ev.raw, 0)
            cont = D.DBG_CONTINUE
            if code == D.EXIT_PROCESS_DEBUG_EVENT:
                k32.ContinueDebugEvent(pid, tid, D.DBG_CONTINUE)
                self.log('EXIT ' + kv(hits=self.hits, during='drain'))
                return True
            if code in (D.LOAD_DLL_DEBUG_EVENT, D.CREATE_PROCESS_DEBUG_EVENT):
                hfile = struct.unpack_from('<Q', self.ev.raw, 16)[0]
                if hfile:
                    k32.CloseHandle(hfile)
            if code == D.EXCEPTION_DEBUG_EVENT:
                exc = struct.unpack_from('<I', self.ev.raw, 16)[0]
                if exc == D.EXCEPTION_SINGLE_STEP:
                    self.log('DRAIN_STEP ' + kv(tid=tid))
                elif exc not in (D.EXCEPTION_BREAKPOINT, D.STATUS_WX86_BREAKPOINT):
                    cont = D.DBG_EXCEPTION_NOT_HANDLED
            k32.ContinueDebugEvent(pid, tid, cont)
        return False

    def detach(self):
        ok = bool(k32.DebugActiveProcessStop(self.pid))
        self.log('DETACH ' + kv(ok=int(ok), err=0 if ok else ctypes.get_last_error()))
        return ok

    def emergency_stop(self):
        try:
            if self.applied:
                self.request_break()
                t0 = time.time()
                while time.time() - t0 < 10:
                    if not k32.WaitForDebugEvent(self.ev, 200):
                        continue
                    code, pid, tid = struct.unpack_from('<III', self.ev.raw, 0)
                    if code == D.EXIT_PROCESS_DEBUG_EVENT:
                        k32.ContinueDebugEvent(pid, tid, D.DBG_CONTINUE)
                        return
                    cont = D.DBG_CONTINUE
                    if code == D.EXCEPTION_DEBUG_EVENT:
                        exc = struct.unpack_from('<I', self.ev.raw, 16)[0]
                        if exc not in (D.EXCEPTION_BREAKPOINT, D.STATUS_WX86_BREAKPOINT, D.EXCEPTION_SINGLE_STEP):
                            cont = D.DBG_EXCEPTION_NOT_HANDLED
                    self.apply(frozenset(), 'emergency')
                    k32.ContinueDebugEvent(pid, tid, cont)
                    break
            self.detach()
        except Exception as e:  # noqa: BLE001
            self.log('EMERGENCY_FAIL ' + kv(err=str(e).replace(' ', '_')))


# ---- selftest ------------------------------------------------------------------------------------------------------------

FAKE = r'''
import ctypes, os, struct, sys, time
buf = ctypes.create_string_buffer(0x3000)
hud = (ctypes.addressof(buf) + 15) & ~15
CT = {'<f': ctypes.c_float, '<B': ctypes.c_uint8, '<Q': ctypes.c_uint64}
def put(off, fmt, v):
    CT[fmt].from_address(hud + off).value = v    # one store of the field's size, like the game's
open(sys.argv[1], 'w').write('%x\n' % hud)
while not os.path.exists(sys.argv[2]):
    time.sleep(0.01)
def frames(n, t0):
    for i in range(n):
        put(0x26DC, '<f', t0 + i); put(0x26E0, '<f', 2.0 * i); put(0x26E4, '<f', 7.0)
        time.sleep(0.002)
frames(30, 0.0)              # 30 x/z pairs: the budget of 20 runs out
put(0x20B2, '<B', 1)         # targeting on: refill
put(0x20C0, '<Q', 0x1234)    # blob created
frames(30, 100.0)
put(0x20C0, '<Q', 0)
put(0x20B2, '<B', 0)         # targeting off
frames(5, 200.0)
open(sys.argv[2] + '.done', 'w').write('done\n')
time.sleep(float(sys.argv[3]))
sys.exit(7)
'''


def selftest():
    tmp = tempfile.mkdtemp(prefix='cu9h_dbg_')
    fake = os.path.join(tmp, 'fake.py')
    open(fake, 'w').write(FAKE)
    fails = []
    for mode, linger in (('exit', 0.5), ('stopfile', 4.0)):
        addr_f, go_f = os.path.join(tmp, mode + '.addr'), os.path.join(tmp, mode + '.go')
        stop_f = os.path.join(tmp, mode + '.stop')
        p = subprocess.Popen([getattr(sys, '_base_executable', sys.executable), '-I', fake, addr_f, go_f, str(linger)])
        t0 = time.time()
        while not os.path.exists(addr_f) and time.time() - t0 < 20:
            time.sleep(0.02)
        time.sleep(0.1)
        hud = int(open(addr_f).read().strip(), 16)
        out = os.path.join(tmp, mode + '.log')
        a = argparse.Namespace(pid=p.pid, hud_addr=hud, ground_budget=20, ground_refills=5, max_hits=0, frames=8,
                               no_gmm=True, min_stack=2, stop_file=stop_f if mode == 'stopfile' else None)
        log = D.Log(out)
        tr = Tracer(a, log)
        threading.Timer(1.0, lambda g=go_f: open(g, 'w').write('go')).start()
        if mode == 'stopfile':
            def stopper(done=go_f + '.done', sf=stop_f):
                while not os.path.exists(done):
                    time.sleep(0.05)
                time.sleep(0.3)
                open(sf, 'w').write('stop')
            threading.Thread(target=stopper, daemon=True).start()
        tr.run()
        log.close()
        rc = p.wait(30)
        lines = open(out).read().splitlines()
        hits = [D_parse(ln) for ln in lines if ln.startswith('HIT ')]
        by = {}
        for h in hits:
            by.setdefault(h['what'], []).append(h)
        flags = [h['val'] for h in by.get('flag', [])]
        blobs = [h['val'] for h in by.get('blob', [])]
        nx, nz = len(by.get('ground_x', [])), len(by.get('ground_z', []))
        # budget 20 before targeting (x and z alternate: 10 + 10), refill on flag=1: 20 more, then off again
        if flags[:2] != ['1', '0']:
            fails.append('%s: flag writes %s (want 1, 0)' % (mode, flags))
        if blobs[:2] != ['1234', '0']:
            fails.append('%s: blob writes %s (want 1234, 0)' % (mode, blobs))
        if nx + nz != 40:
            fails.append('%s: %d ground hits (want 40: budget 20 + one refill of 20)' % (mode, nx + nz))
        gx = [float(h['val']) for h in by.get('ground_x', [])]
        if gx[:10] != [float(i) for i in range(10)] or gx[10:20] != [100.0 + i for i in range(10)]:
            fails.append('%s: ground x values %s' % (mode, gx[:24]))
        if any(h.get('flag') != h['val'] for h in by.get('flag', [])):
            fails.append('%s: the snapshot flag differs from the written value' % mode)
        if not all(',' in h['walk'] for h in hits[:3]):
            fails.append('%s: stack walk gave one frame only' % mode)
        if rc != 7:
            fails.append('%s: fake exit code %s (want 7: the target must survive the debugger)' % (mode, rc))
        if mode == 'stopfile' and not any(ln.startswith('DETACH ok=1') for ln in lines):
            fails.append('stopfile: no DETACH ok=1')
        if not any(ln.startswith('ARM mask=0123') for ln in lines) or not any(ln.startswith('ARM mask=12 ') for ln in lines):
            fails.append('%s: no arm 0123 / budget trim to 12' % mode)
        print('selftest %s: %d hits (ground %d, flag %s, blob %s) rc=%s' % (mode, len(hits), nx + nz, flags, blobs, rc))
    if dr7_for({0}, 0) != (0x1 | (0xD << 16)) or dr7_for({1}, 0) != (0x4 | (0x1 << 20)) or \
            dr7_for({2}, 0) != (0x10 | (0x9 << 24)):
        fails.append('dr7_for bits')
    for f in fails:
        print('FAIL', f)
    print('CU9H DBG SELFTEST', 'OK' if not fails else 'FAILED')
    return 0 if not fails else 1


def D_parse(line):
    d = {}
    head, _, stack = line.partition(' walk=')
    for tok in head.split()[1:]:
        if '=' in tok:
            k, v = tok.split('=', 1)
            d[k] = v
    d['walk'] = stack
    return d


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out')
    ap.add_argument('--pid', type=int)
    ap.add_argument('--wait-process-sec', type=float, default=600)
    ap.add_argument('--ground-budget', type=int, default=240)
    ap.add_argument('--ground-refills', type=int, default=20)
    ap.add_argument('--max-hits', type=int, default=20000)
    ap.add_argument('--stop-file')
    ap.add_argument('--min-stack', type=int, default=2)
    ap.add_argument('--frames', type=int, default=16)
    ap.add_argument('--hud-addr', type=lambda s: int(s, 0), help=argparse.SUPPRESS)
    ap.add_argument('--no-gmm', action='store_true', help=argparse.SUPPRESS)
    ap.add_argument('--allow-unknown-exe', action='store_true')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.out:
        ap.error('--out is required')
    log = D.Log(a.out)
    log('CU9HDBG 1 ' + kv(t=time.strftime('%Y-%m-%dT%H:%M:%S'), hud_rva='%x' % M.RVA_HUD,
                          ground_budget=a.ground_budget, min_stack=a.min_stack))
    if a.pid is None:
        a.pid = D.find_game(a, log)
        if a.pid is None:
            log('NO_GAME')
            log.close()
            print('no StarWarsG.exe found')
            return 2
    try:
        hits = Tracer(a, log).run()
    finally:
        log.close()
    print('cu9h_dbg: %d hits, log %s' % (hits, a.out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
