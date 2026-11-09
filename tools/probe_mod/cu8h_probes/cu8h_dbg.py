#!/usr/bin/env python3
"""cu8h_dbg.py - synced-draw tracer for the running retail game (roadmap group cu8h, t01; CU8G_T02_ENV_PICK_RE sec. 7 P1).

  cu8h_dbg.py --out LOG [--pid N | --wait-process-sec 600] [--arm-file PATH | --arm-now] [--stop-file PATH]
              [--stop-mode-frame 90] [--stop-after-frames N] [--max-hits 300000] [--state-rva 0xa13e24 | --state-addr 0xADDR]
              [--frames 24] [--allow-unknown-exe]

A small project-local debugger (Windows debug API through ctypes, nothing installed). It attaches to StarWarsG.exe,
puts a hardware WRITE watchpoint (Dr0, 4 bytes) on the synchronized LCG state (RVA 0xa13e24 = 0x140a13e24 at the
preferred base; MAIN_LOOP sec. 4.1) in every thread, and logs each write: thread, RIP, the new state, the number of
LCG steps since the previous write (or a reseed), the game mode (GMM mode type / stack / current mode / mode frame /
global frame) and a stack walk (dbghelp StackWalk64, return addresses as RVAs). A watchpoint sees every step of the
state, whichever function makes it (int 0x1ffb40, float 0x1ffbb0, 0x532480, 0x5324d0, percent 0x1ffdb0, inlined code,
the seed write in Start_Game), so no code breakpoint and no code patch is needed. The game's memory is never written;
only the debug registers of its threads are set, and they are cleared again before the debugger detaches.

Arming (the main-menu background battle draws all the time, so the watchpoint is off until asked for):
  --arm-now          arm at once;
  --arm-file PATH    arm when PATH exists, disarm when it is removed (touch it just before Start / Begin).
Stopping: game exit; --stop-file PATH appears; --max-hits; or --stop-mode-frame N: a tactical mode (type 1 / 2)
that was not the current mode when the watchpoint was armed reaches mode frame N (default 90 = 3 s of battle);
or --stop-after-frames N: the mode that was current when it armed is N mode frames past its frame at arming (for a
trace armed inside a battle, e.g. just before an order; 0 = off).
On a stop the watchpoint is cleared in every thread (inside a debug event, all threads frozen), then
DebugActiveProcessStop detaches; DebugSetProcessKillOnExit(FALSE) keeps the game alive if this script dies.

Log lines (key=value): CU8HDBG 1, ATTACH pid= base= state=, THREAD tid= (new), ARM / DISARM why=, HIT n= tid= rip=
state= steps= mt= st= mode= mf= gf= stack=rva,rva,..., STOP why=, DETACH ok=, EXIT.
RVAs are image offsets (0x1ffb40 style, as in docs/re); an address outside the exe is written as m:<hex>.

Offline check: cu8h_dbg.py --selftest (attaches to a fake Python process that steps an LCG in its own memory).
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import os
import struct
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'a1_tactical'))

LCG_MUL, LCG_ADD = 0x41C64E6D, 0xBDF
STATE_RVA = 0xA13E24
RVA_FRAME_SYNC = 0xB0A320     # a1mem / MAIN_LOOP
RVA_GMM = 0xB153E0

# ---- Win32 -----------------------------------------------------------------------------------------------------------
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
dbghelp = ctypes.WinDLL('dbghelp', use_last_error=True)

PROCESS_ALL_ACCESS = 0x1F0FFF
THREAD_ACCESS = 0x0002 | 0x0008 | 0x0010 | 0x0040   # SUSPEND_RESUME, GET_CONTEXT, SET_CONTEXT, QUERY_INFORMATION
DBG_CONTINUE = 0x00010002
DBG_EXCEPTION_NOT_HANDLED = 0x80010001
EXCEPTION_DEBUG_EVENT, CREATE_THREAD_DEBUG_EVENT, CREATE_PROCESS_DEBUG_EVENT = 1, 2, 3
EXIT_THREAD_DEBUG_EVENT, EXIT_PROCESS_DEBUG_EVENT, LOAD_DLL_DEBUG_EVENT = 4, 5, 6
EXCEPTION_BREAKPOINT, EXCEPTION_SINGLE_STEP = 0x80000003, 0x80000004
STATUS_WX86_BREAKPOINT = 0x4000001F
CONTEXT_ALL = 0x10001F
CONTEXT_DEBUG_REGISTERS = 0x100010
CTX_SIZE = 0x4D0
OFF_FLAGS, OFF_DR0, OFF_DR6, OFF_DR7, OFF_RSP, OFF_RIP = 0x30, 0x48, 0x68, 0x70, 0x98, 0xF8
DR7_WATCH = 0x1 | (0x1 << 16) | (0x3 << 18)          # L0, RW0 = write, LEN0 = 4 bytes
INFINITE = 0xFFFFFFFF

k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.OpenThread.restype = wt.HANDLE
k32.OpenThread.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.DebugActiveProcess.argtypes = [wt.DWORD]
k32.DebugActiveProcessStop.argtypes = [wt.DWORD]
k32.DebugSetProcessKillOnExit.argtypes = [wt.BOOL]
k32.DebugBreakProcess.argtypes = [wt.HANDLE]
k32.WaitForDebugEvent.argtypes = [ctypes.c_void_p, wt.DWORD]
k32.ContinueDebugEvent.argtypes = [wt.DWORD, wt.DWORD, wt.DWORD]
k32.GetThreadContext.argtypes = [wt.HANDLE, ctypes.c_void_p]
k32.SetThreadContext.argtypes = [wt.HANDLE, ctypes.c_void_p]
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]
k32.CloseHandle.argtypes = [wt.HANDLE]
k32.GetProcessId.argtypes = [wt.HANDLE]


class ADDRESS64(ctypes.Structure):
    _fields_ = [('Offset', ctypes.c_uint64), ('Segment', ctypes.c_uint16), ('Mode', ctypes.c_uint32)]


class KDHELP64(ctypes.Structure):
    _fields_ = [('Thread', ctypes.c_uint64), ('ThCallbackStack', ctypes.c_uint32),
                ('ThCallbackBStore', ctypes.c_uint32), ('NextCallback', ctypes.c_uint32),
                ('FramePointer', ctypes.c_uint32), ('KiCallUserMode', ctypes.c_uint64),
                ('KeUserCallbackDispatcher', ctypes.c_uint64), ('SystemRangeStart', ctypes.c_uint64),
                ('KiUserExceptionDispatcher', ctypes.c_uint64), ('StackBase', ctypes.c_uint64),
                ('StackLimit', ctypes.c_uint64), ('BuildVersion', ctypes.c_uint32),
                ('RetpolineStubFunctionTableSize', ctypes.c_uint32), ('RetpolineStubFunctionTable', ctypes.c_uint64),
                ('RetpolineStubOffset', ctypes.c_uint32), ('RetpolineStubSize', ctypes.c_uint32),
                ('Reserved0', ctypes.c_uint64 * 2)]


class STACKFRAME64(ctypes.Structure):
    _fields_ = [('AddrPC', ADDRESS64), ('AddrReturn', ADDRESS64), ('AddrFrame', ADDRESS64), ('AddrStack', ADDRESS64),
                ('AddrBStore', ADDRESS64), ('FuncTableEntry', ctypes.c_void_p), ('Params', ctypes.c_uint64 * 4),
                ('Far', wt.BOOL), ('Virtual', wt.BOOL), ('Reserved', ctypes.c_uint64 * 3), ('KdHelp', KDHELP64)]


dbghelp.SymSetOptions.argtypes = [wt.DWORD]
dbghelp.SymInitialize.argtypes = [wt.HANDLE, ctypes.c_char_p, wt.BOOL]
dbghelp.SymCleanup.argtypes = [wt.HANDLE]
dbghelp.SymRefreshModuleList.argtypes = [wt.HANDLE]
dbghelp.StackWalk64.argtypes = [wt.DWORD, wt.HANDLE, wt.HANDLE, ctypes.POINTER(STACKFRAME64), ctypes.c_void_p,
                                ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
FN_TABLE_ACCESS = ctypes.cast(dbghelp.SymFunctionTableAccess64, ctypes.c_void_p)
FN_MODULE_BASE = ctypes.cast(dbghelp.SymGetModuleBase64, ctypes.c_void_p)
AddrModeFlat = 3


def lcg(s):
    return (s * LCG_MUL + LCG_ADD) & 0xFFFFFFFF


def lcg_steps(prev, new, limit=4096):
    """Number of LCG steps from prev to new (0 when equal), or None when new is not within `limit` steps (a seed)."""
    if prev is None:
        return None
    s = prev
    for k in range(limit + 1):
        if s == new:
            return k
        s = lcg(s)
    return None


class Ctx:
    """A 16-byte aligned x64 CONTEXT buffer."""

    def __init__(self):
        self.raw = ctypes.create_string_buffer(CTX_SIZE + 16)
        self.addr = (ctypes.addressof(self.raw) + 15) & ~15

    def get(self, h, flags=CONTEXT_ALL):
        ctypes.memset(self.addr, 0, CTX_SIZE)
        ctypes.c_uint32.from_address(self.addr + OFF_FLAGS).value = flags
        return bool(k32.GetThreadContext(h, ctypes.c_void_p(self.addr)))

    def set(self, h):
        return bool(k32.SetThreadContext(h, ctypes.c_void_p(self.addr)))

    def q(self, off):
        return ctypes.c_uint64.from_address(self.addr + off).value

    def setq(self, off, v):
        ctypes.c_uint64.from_address(self.addr + off).value = v & 0xFFFFFFFFFFFFFFFF

    def setflags(self, v):
        ctypes.c_uint32.from_address(self.addr + OFF_FLAGS).value = v


class Log:
    """Buffered, but flushed at least once a second and on every non-HIT line, so the file shows progress."""
    def __init__(self, path):
        self.f = open(path, 'w', buffering=1 << 16)
        self.last = time.monotonic()

    def __call__(self, line):
        self.f.write(line + '\n')
        now = time.monotonic()
        if not line.startswith('HIT ') or now - self.last >= 1.0:
            self.f.flush()
            self.last = now

    def close(self):
        self.f.flush()
        self.f.close()


def kv(**k):
    return ' '.join('%s=%s' % (a, b) for a, b in k.items())


# ---- the debugger ------------------------------------------------------------------------------------------------------

class Tracer:
    def __init__(self, a, log):
        self.a, self.log = a, log
        self.pid = a.pid
        self.hp = None
        self.base = None
        self.state_addr = None
        self.threads = {}            # tid -> handle (handles from the debug events; the system closes them)
        self.armed = False
        self.want_armed = False
        self.break_pending = False
        self.stop_why = None
        self.hits = 0
        self.prev_state = None
        self.arm_mode = None
        self.arm_mf = None
        self.ctx = Ctx()
        self.ctx2 = Ctx()
        self.ev = ctypes.create_string_buffer(512)
        self.sym = False
        self.image_end = None
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

    def u32(self, addr):
        b = self.read(addr, 4)
        return struct.unpack('<I', b)[0] if b else None

    def u64(self, addr):
        b = self.read(addr, 8)
        return struct.unpack('<Q', b)[0] if b else None

    def rva(self, x):
        if self.base and self.image_end and self.base <= x < self.image_end:
            return '%x' % (x - self.base)
        return 'm:%x' % x

    def mode(self):
        """(mode type, stack, mode ptr, mode frame, global frame) or Nones (the fake game has no GMM)."""
        if self.a.no_gmm:
            return None, None, None, None, None
        b = self.read(self.base + RVA_GMM, 0x58)
        if not b:
            return None, None, None, None, None
        mt = struct.unpack_from('<i', b, 0x1C)[0]
        cur = struct.unpack_from('<Q', b, 0x38)[0]
        st = struct.unpack_from('<i', b, 0x50)[0]
        mf = self.u32(cur + 0x10) if cur else None
        gf = self.u32(self.base + RVA_FRAME_SYNC)
        return mt, st, cur, mf, gf

    # debug registers
    def set_dr(self, h, on):
        c = self.ctx2
        if not c.get(h, CONTEXT_DEBUG_REGISTERS):
            return False
        c.setflags(CONTEXT_DEBUG_REGISTERS)
        c.setq(OFF_DR0, self.state_addr if on else 0)
        c.setq(OFF_DR6, 0)
        dr7 = c.q(OFF_DR7) & ~(0x3 | (0xF << 16))
        c.setq(OFF_DR7, dr7 | DR7_WATCH if on else dr7)
        return c.set(h)

    def apply(self, on, why):
        """Call only inside a debug event (all threads frozen)."""
        ok = bad = 0
        for tid, h in list(self.threads.items()):
            if self.set_dr(h, on):
                ok += 1
            else:
                bad += 1
        self.armed = on
        if on:
            m = self.mode()
            self.arm_mode = m[2]
            self.arm_mf = m[3]
            self.prev_state = self.u32(self.state_addr)
        self.log('%s %s' % ('ARM' if on else 'DISARM', kv(why=why, threads=ok, failed=bad, t='%.3f' % time.time(),
                                                            state='%08x' % (self.prev_state or 0))))

    def request_break(self):
        if not self.break_pending:
            self.break_pending = True
            if not k32.DebugBreakProcess(self.hp):
                self.break_pending = False
                self.log('BREAK_FAILED ' + kv(err=ctypes.get_last_error()))

    # stack walk
    def walk(self, h):
        c = self.ctx
        rip, rsp = c.q(OFF_RIP), c.q(OFF_RSP)
        if not self.sym:
            return [rip]
        cc = self.ctx2
        ctypes.memmove(cc.addr, c.addr, CTX_SIZE)
        fr = STACKFRAME64()
        fr.AddrPC.Offset, fr.AddrPC.Mode = rip, AddrModeFlat
        fr.AddrStack.Offset, fr.AddrStack.Mode = rsp, AddrModeFlat
        fr.AddrFrame.Offset, fr.AddrFrame.Mode = cc.q(0xA0), AddrModeFlat
        out = []
        for _ in range(self.a.frames):
            if not dbghelp.StackWalk64(0x8664, self.hp, h, ctypes.byref(fr), ctypes.c_void_p(cc.addr), None,
                                       FN_TABLE_ACCESS, FN_MODULE_BASE, None):
                break
            pc = fr.AddrPC.Offset
            if not pc:
                break
            out.append(pc)
        return out or [rip]

    def on_hit(self, tid, h):
        c = self.ctx
        if not c.get(h, CONTEXT_ALL):
            self.log('CTX_FAIL ' + kv(tid=tid, err=ctypes.get_last_error()))
            return
        dr6 = c.q(OFF_DR6)
        st = self.u32(self.state_addr)
        steps = lcg_steps(self.prev_state, st) if st is not None else None
        self.prev_state = st
        self.hits += 1
        mt, sk, cur, mf, gf = self.mode()
        stack = self.walk(h)
        self.log('HIT ' + kv(n=self.hits, tid=tid, rip=self.rva(c.q(OFF_RIP)), state='%08x' % (st or 0),
                             steps='seed' if steps is None else steps, dr6='%x' % (dr6 & 0xF), mt=mt, st=sk,
                             mode='%x' % (cur or 0), mf=mf, gf=gf, stack=','.join(self.rva(x) for x in stack)))
        # clear Dr6 in this thread
        cc = self.ctx2
        if cc.get(h, CONTEXT_DEBUG_REGISTERS):
            cc.setflags(CONTEXT_DEBUG_REGISTERS)
            cc.setq(OFF_DR6, 0)
            cc.set(h)
        if self.a.max_hits and self.hits >= self.a.max_hits:
            self.stop_why = 'max_hits'
        if self.a.stop_mode_frame and mt in (1, 2) and cur and cur != self.arm_mode and mf is not None \
                and mf >= self.a.stop_mode_frame:
            self.stop_why = 'mode_frame_%d' % mf
        after = getattr(self.a, 'stop_after_frames', 0)
        if after and cur and cur == self.arm_mode and self.arm_mf is not None and mf is not None \
                and mf >= self.arm_mf + after:
            self.stop_why = 'after_frames_%d' % (mf - self.arm_mf)

    # main loop
    def attach(self):
        self.hp = k32.OpenProcess(PROCESS_ALL_ACCESS, False, self.pid)
        if not self.hp:
            raise OSError('OpenProcess(%d) failed: %d' % (self.pid, ctypes.get_last_error()))
        if not k32.DebugActiveProcess(self.pid):
            raise OSError('DebugActiveProcess(%d) failed: %d' % (self.pid, ctypes.get_last_error()))
        k32.DebugSetProcessKillOnExit(False)

    def poll_controls(self):
        now = time.time()
        if now - self.last_poll < 0.05:
            return
        self.last_poll = now
        if self.a.stop_file and os.path.exists(self.a.stop_file):
            self.stop_why = self.stop_why or 'stop_file'
        if self.a.arm_file:
            self.want_armed = os.path.exists(self.a.arm_file)

    def run(self):
        self.attach()
        detached = False
        exited = False
        try:
            while True:
                self.poll_controls()
                need_break = (self.stop_why is not None) or (self.want_armed != self.armed and self.base is not None)
                if need_break and self.base is not None:
                    self.request_break()
                if not k32.WaitForDebugEvent(self.ev, 100):
                    continue
                code, pid, tid = struct.unpack_from('<III', self.ev.raw, 0)
                u = 16
                cont = DBG_CONTINUE
                if code == CREATE_PROCESS_DEBUG_EVENT:
                    hfile, _hproc, hthread, base = struct.unpack_from('<QQQQ', self.ev.raw, u)
                    if hfile:
                        k32.CloseHandle(hfile)
                    self.base = base
                    lfanew = self.u32(base + 0x3C)
                    soi = self.u32(base + lfanew + 24 + 56) if lfanew else None
                    self.image_end = base + (soi or 0)
                    self.state_addr = self.a.state_addr if self.a.state_addr else base + self.a.state_rva
                    self.threads[tid] = hthread
                    dbghelp.SymSetOptions(0x2 | 0x4 | 0x10)    # UNDNAME | DEFERRED_LOADS | LOAD_LINES
                    self.sym = bool(dbghelp.SymInitialize(self.hp, None, True))
                    self.log('ATTACH ' + kv(pid=self.pid, base='%x' % base, state='%x' % self.state_addr,
                                            size_of_image='%x' % (soi or 0), sym=int(self.sym)))
                    if self.a.arm_now:
                        self.want_armed = True
                elif code == CREATE_THREAD_DEBUG_EVENT:
                    hthread = struct.unpack_from('<Q', self.ev.raw, u)[0]
                    self.threads[tid] = hthread
                    if self.armed:
                        self.set_dr(hthread, True)
                    if self.a.verbose:
                        self.log('THREAD ' + kv(tid=tid, armed=int(self.armed)))
                elif code == EXIT_THREAD_DEBUG_EVENT:
                    self.threads.pop(tid, None)
                elif code == LOAD_DLL_DEBUG_EVENT:
                    hfile = struct.unpack_from('<Q', self.ev.raw, u)[0]
                    if hfile:
                        k32.CloseHandle(hfile)
                    if self.sym:
                        dbghelp.SymRefreshModuleList(self.hp)
                elif code == EXIT_PROCESS_DEBUG_EVENT:
                    self.log('EXIT ' + kv(code=struct.unpack_from('<I', self.ev.raw, u)[0], hits=self.hits))
                    k32.ContinueDebugEvent(pid, tid, DBG_CONTINUE)
                    exited = True
                    break
                elif code == EXCEPTION_DEBUG_EVENT:
                    exc = struct.unpack_from('<I', self.ev.raw, u)[0]
                    if exc == EXCEPTION_SINGLE_STEP:
                        h = self.threads.get(tid)
                        if h is None:
                            h = k32.OpenThread(THREAD_ACCESS, False, tid)
                            self.threads[tid] = h
                        c = self.ctx
                        if self.armed and c.get(h, CONTEXT_DEBUG_REGISTERS) and (c.q(OFF_DR6) & 0x1):
                            self.on_hit(tid, h)
                        else:
                            cont = DBG_EXCEPTION_NOT_HANDLED
                    elif exc in (EXCEPTION_BREAKPOINT, STATUS_WX86_BREAKPOINT):
                        # the attach breakpoint, or ours from DebugBreakProcess: all threads are frozen here
                        self.break_pending = False
                        if self.stop_why is None and self.want_armed != self.armed:
                            self.apply(self.want_armed, 'arm_file' if self.a.arm_file else 'arm_now')
                    else:
                        cont = DBG_EXCEPTION_NOT_HANDLED
                    if self.stop_why is not None:
                        # stop inside this event: clear every thread, then detach
                        if self.armed:
                            self.apply(False, 'stop')
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
        """After disarming: continue the events still queued (a watchpoint hit another thread took at the same moment
        must not reach the game unhandled after the detach). True when the process exited meanwhile."""
        t0 = time.time()
        while time.time() - t0 < seconds:
            if not k32.WaitForDebugEvent(self.ev, 50):
                continue
            code, pid, tid = struct.unpack_from('<III', self.ev.raw, 0)
            cont = DBG_CONTINUE
            if code == EXIT_PROCESS_DEBUG_EVENT:
                k32.ContinueDebugEvent(pid, tid, DBG_CONTINUE)
                self.log('EXIT ' + kv(hits=self.hits, during='drain'))
                return True
            if code == LOAD_DLL_DEBUG_EVENT or code == CREATE_PROCESS_DEBUG_EVENT:
                hfile = struct.unpack_from('<Q', self.ev.raw, 16)[0]
                if hfile:
                    k32.CloseHandle(hfile)
            if code == EXCEPTION_DEBUG_EVENT:
                exc = struct.unpack_from('<I', self.ev.raw, 16)[0]
                if exc == EXCEPTION_SINGLE_STEP:
                    self.log('DRAIN_STEP ' + kv(tid=tid))
                elif exc not in (EXCEPTION_BREAKPOINT, STATUS_WX86_BREAKPOINT):
                    cont = DBG_EXCEPTION_NOT_HANDLED
            k32.ContinueDebugEvent(pid, tid, cont)
        return False

    def detach(self):
        ok = bool(k32.DebugActiveProcessStop(self.pid))
        self.log('DETACH ' + kv(ok=int(ok), err=0 if ok else ctypes.get_last_error()))
        return ok

    def emergency_stop(self):
        """After an error: get one debug event with every thread frozen, clear the watchpoint, detach."""
        try:
            if self.armed:
                self.request_break()
                t0 = time.time()
                while time.time() - t0 < 10:
                    if not k32.WaitForDebugEvent(self.ev, 200):
                        continue
                    code, pid, tid = struct.unpack_from('<III', self.ev.raw, 0)
                    if code == EXIT_PROCESS_DEBUG_EVENT:
                        k32.ContinueDebugEvent(pid, tid, DBG_CONTINUE)
                        return
                    cont = DBG_CONTINUE
                    if code == EXCEPTION_DEBUG_EVENT:
                        exc = struct.unpack_from('<I', self.ev.raw, 16)[0]
                        if exc not in (EXCEPTION_BREAKPOINT, STATUS_WX86_BREAKPOINT, EXCEPTION_SINGLE_STEP):
                            cont = DBG_EXCEPTION_NOT_HANDLED
                    self.apply(False, 'emergency')
                    k32.ContinueDebugEvent(pid, tid, cont)
                    break
            self.detach()
        except Exception as e:  # noqa: BLE001
            self.log('EMERGENCY_FAIL ' + kv(err=str(e).replace(' ', '_')))


def find_game(a, log):
    import a1mem
    t0 = time.time()
    while time.time() - t0 < a.wait_process_sec:
        for pid in a1mem.find_pids():
            try:
                p = a1mem.Proc(pid)
            except OSError:
                continue
            ok = p.pe_identity()
            p.close()
            if ok or a.allow_unknown_exe:
                return pid
            log('IGNORED ' + kv(pid=pid))
        time.sleep(1.0)
    return None


# ---- selftest ------------------------------------------------------------------------------------------------------------

FAKE = r'''
import ctypes, os, sys, time
buf = ctypes.create_string_buffer(64)
addr = (ctypes.addressof(buf) + 3) & ~3
cell = ctypes.c_uint32.from_address(addr)
cell.value = 12345
open(sys.argv[1], 'w').write('%x\n' % addr)
while not os.path.exists(sys.argv[2]):
    time.sleep(0.01)
s = cell.value
for i in range(int(sys.argv[3])):
    s = (s * 0x41C64E6D + 0xBDF) & 0xFFFFFFFF
    cell.value = s
    time.sleep(0.002)
open(sys.argv[2] + '.done', 'w').write('%08x\n' % s)
time.sleep(float(sys.argv[4]))
sys.exit(7)
'''


def selftest():
    tmp = tempfile.mkdtemp(prefix='cu8h_dbg_')
    fake = os.path.join(tmp, 'fake.py')
    open(fake, 'w').write(FAKE)
    fails = []
    for mode, steps, linger in (('exit', 40, 0.5), ('detach', 30, 3.0)):
        addr_f, go_f = os.path.join(tmp, mode + '.addr'), os.path.join(tmp, mode + '.go')
        p = subprocess.Popen([getattr(sys, "_base_executable", sys.executable), '-I', fake, addr_f, go_f, str(steps), str(linger)])
        t0 = time.time()
        while not os.path.exists(addr_f) and time.time() - t0 < 20:
            time.sleep(0.02)
        time.sleep(0.1)
        addr = int(open(addr_f).read().strip(), 16)
        out = os.path.join(tmp, mode + '.log')
        a = argparse.Namespace(pid=p.pid, state_addr=addr, state_rva=0, arm_now=True, arm_file=None, stop_file=None,
                               stop_mode_frame=0, max_hits=steps if mode == 'detach' else 0, frames=8, no_gmm=True,
                               verbose=False)
        log = Log(out)
        tr = Tracer(a, log)
        import threading
        threading.Timer(1.0, lambda: open(go_f, 'w').write('go')).start()
        hits = tr.run()
        log.close()
        rc = p.wait(30)
        lines = open(out).read().splitlines()
        # the fake's buffer is freed at interpreter exit, so writes after its last step can follow: compare the first
        hl = [ln for ln in lines if ln.startswith('HIT ')][:steps]
        st = [int(ln.split('state=')[1].split()[0], 16) for ln in hl]
        exp, s = [], 12345
        for _ in range(steps):
            s = lcg(s)
            exp.append(s)
        stp = [ln.split('steps=')[1].split()[0] for ln in hl]
        if len(hl) != steps or st != exp or (mode == 'detach' and hits != steps):
            fails.append('%s: %d hits (want %d), states match %s' % (mode, hits, steps, st == exp))
        if stp and any(x != '1' for x in stp):
            fails.append('%s: steps %s' % (mode, sorted(set(stp))))
        if rc != 7:
            fails.append('%s: fake exit code %s (want 7: the target must survive the debugger)' % (mode, rc))
        if mode == 'detach' and not any(ln.startswith('DETACH ok=1') for ln in lines):
            fails.append('detach: no DETACH ok=1')
        if not all(',' in ln.split('stack=')[1] for ln in hl[:3]):
            fails.append('%s: stack walk gave one frame only' % mode)
        print('selftest %s: hits=%d rc=%s %s' % (mode, hits, rc, 'ok' if not fails else ''))
    if lcg_steps(1, lcg(lcg(lcg(1)))) != 3 or lcg_steps(1, 1) != 0 or lcg_steps(1, 0xdeadbeef, 50) is not None:
        fails.append('lcg_steps')
    for f in fails:
        print('FAIL', f)
    print('CU8H DBG SELFTEST', 'OK' if not fails else 'FAILED')
    return 0 if not fails else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out')
    ap.add_argument('--pid', type=int)
    ap.add_argument('--wait-process-sec', type=float, default=600)
    ap.add_argument('--arm-file')
    ap.add_argument('--arm-now', action='store_true')
    ap.add_argument('--stop-file')
    ap.add_argument('--stop-mode-frame', type=int, default=90)
    ap.add_argument('--stop-after-frames', type=int, default=0)
    ap.add_argument('--max-hits', type=int, default=300000)
    ap.add_argument('--state-rva', type=lambda s: int(s, 0), default=STATE_RVA)
    ap.add_argument('--state-addr', type=lambda s: int(s, 0))
    ap.add_argument('--frames', type=int, default=24)
    ap.add_argument('--allow-unknown-exe', action='store_true')
    ap.add_argument('--no-gmm', action='store_true', help=argparse.SUPPRESS)
    ap.add_argument('--verbose', action='store_true')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.out:
        ap.error('--out is required')
    if not a.arm_now and not a.arm_file:
        ap.error('give --arm-now or --arm-file')
    log = Log(a.out)
    log('CU8HDBG 1 ' + kv(t=time.strftime('%Y-%m-%dT%H:%M:%S'), state_rva='%x' % a.state_rva,
                          stop_mode_frame=a.stop_mode_frame, stop_after_frames=a.stop_after_frames))
    if a.pid is None:
        a.pid = find_game(a, log)
        if a.pid is None:
            log('NO_GAME')
            log.close()
            print('no StarWarsG.exe found')
            return 2
    try:
        hits = Tracer(a, log).run()
    finally:
        log.close()
    print('cu8h_dbg: %d hits, log %s' % (hits, a.out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
