#!/usr/bin/env python3
"""a1mem.py - read-only access to a running FoC StarWarsG.exe (Steam x64 1.121) for the A1 tactical-AI probes.

Pure Python (ctypes, kernel32 / psapi): OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION | SYNCHRONIZE),
ReadProcessMemory, VirtualQueryEx. Never writes the game's memory, never injects, never suspends.

Layout: the same values as tools/probe_mod/q2_traces/reader/q2_layout.hpp
(validated by the 2026-10-29 S10 runs and the 2026-10-30 Q2 runs), plus the player fields the land cap needs:
  PlayerList (RVA 0xA16FD0): +0x00 std::vector<PlayerClass*> in index order (Q2, observed); +0x20 PlayerClass** by
  player id, +0x28 i32 count (the id lookup the cap code uses, AI_GALACTIC_3 §3.3 / docs/re).
  PlayerClass: +0x48 i32 index, +0x4C i32 id, +0x62 u8 human, +0x68 faction*, +0x404 i32 land population capacity
  (AI_GALACTIC_3 §3.3: Base_Population_Capacity + sum of Additional_Population_Capacity of held objects, refreshed
  on object add / remove in the current mode). Faction +0x108 u8 Is_Neutral (TACTICAL_COMBAT / AI_TACTICAL).
  The faction's name offset is not documented: find_faction_name() searches the faction object for a std::string
  holding a known faction name and reports the offset it used.

Heap scan (the Lua output channel): retail _CustomScriptMessage / _OuputDebug write nothing (S10 and Q2 runs,
2026-10-29 / 30), so the probe Lua keeps its records as Lua strings ("A1P1|...|END") and Proc.scan() finds them in
the game's committed private memory.
"""
import ctypes
import ctypes.wintypes as wt
import math
import re
import struct
import subprocess
import threading
import time

# ---- exe identity (PE_OVERVIEW.md) ---------------------------------------------------------------------------------
FOC_TIMESTAMP = 0x67002393
FOC_SIZE_OF_IMAGE = 0x00CC0000

# ---- statics (MAIN_LOOP.md) ----------------------------------------------------------------------------------------
RVA_FRAME_SYNC = 0xB0A320
RVA_GMM = 0xB153E0
RVA_PLAYER_LIST = 0xA16FD0

FS_FRAME = 0x00          # u32 global logic frame
GMM_GAME_TYPE = 0x18     # i32
GMM_MODE_TYPE = 0x1C     # i32 0 galactic, 1 land, 2 space
GMM_CUR_MODE = 0x38      # GameModeClass*
GMM_STACK = 0x50         # i32 mode stack depth
MODE_FRAME = 0x10        # u32 per-mode frame
MODE_PRIMARY = 0x18      # GameObjectManagerClass*
GOM_ID_HEAD = 0x88       # unordered_map<int, GameObject*> list sentinel
GOM_ID_SIZE = 0x90
OBJ_BASE = 0x50          # object block read from +0x50 .. +0x3A8
OBJ_ID = 0x50
OBJ_OWNER = 0x58
OBJ_POS = 0x78
OBJ_TYPE = 0x298
OBJ_FLAGS = 0x3A0        # & 0x42: dead / removing
OBJ_END = 0x3A8
TYPE_NAME = 0xF8         # MSVC std::string (16-byte buffer / pointer, +0x10 size, +0x18 capacity)

PL_VEC = 0x00
PL_BY_ID = 0x20
PL_COUNT = 0x28
P_INDEX = 0x48
P_ID = 0x4C
P_HUMAN = 0x62
P_FACTION = 0x68
P_POPCAP = 0x404
P_END = 0x408
FAC_NEUTRAL = 0x108

FACTIONS = ('REBEL', 'EMPIRE', 'PIRATES', 'NEUTRAL', 'HOSTILE', 'SARLACC', 'UNDERWORLD', 'HUTTS')

# ---- kernel32 / psapi ----------------------------------------------------------------------------------------------
PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
SYNCHRONIZE = 0x00100000
MEM_COMMIT = 0x1000
MEM_PRIVATE = 0x20000
PAGE_GUARD = 0x100
PAGE_NOACCESS = 0x01
READABLE = {0x02, 0x04, 0x08, 0x20, 0x40, 0x80}  # R, RW, WC, XR, XRW, XWC

_k32 = ctypes.WinDLL('kernel32', use_last_error=True)
_psapi = ctypes.WinDLL('psapi', use_last_error=True)


class MBI(ctypes.Structure):
    _fields_ = [('BaseAddress', ctypes.c_uint64), ('AllocationBase', ctypes.c_uint64),
                ('AllocationProtect', wt.DWORD), ('PartitionId', wt.WORD), ('RegionSize', ctypes.c_uint64),
                ('State', wt.DWORD), ('Protect', wt.DWORD), ('Type', wt.DWORD)]


_k32.OpenProcess.restype = wt.HANDLE
_k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
_k32.ReadProcessMemory.restype = wt.BOOL
_k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                   ctypes.POINTER(ctypes.c_size_t)]
_k32.VirtualQueryEx.restype = ctypes.c_size_t
_k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
_k32.WaitForSingleObject.restype = wt.DWORD
_k32.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
_k32.CloseHandle.argtypes = [wt.HANDLE]
_psapi.EnumProcessModulesEx.restype = wt.BOOL
_psapi.EnumProcessModulesEx.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_void_p), wt.DWORD,
                                        ctypes.POINTER(wt.DWORD), wt.DWORD]


def find_pids(exe='StarWarsG.exe'):
    r = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq %s' % exe, '/FO', 'CSV', '/NH'], capture_output=True,
                       text=True)
    pids = []
    for line in r.stdout.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) >= 2 and parts[0].lower() == exe.lower():
            try:
                pids.append(int(parts[1].strip('"')))
            except ValueError:
                pass
    return pids


class Proc:
    """One opened process. base = exe image base (or the test base of the fake game)."""

    def __init__(self, pid, test_base=None):
        self.pid = pid
        self.h = _k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION | SYNCHRONIZE, False, pid)
        if not self.h:
            raise OSError('OpenProcess(%d) failed, error %d' % (pid, ctypes.get_last_error()))
        self.base = test_base if test_base is not None else self._exe_base()
        self.timestamp = self.size_of_image = None
        self.reads = self.failed = 0

    def _exe_base(self):
        mods = (ctypes.c_void_p * 1)()
        need = wt.DWORD()
        if not _psapi.EnumProcessModulesEx(self.h, mods, ctypes.sizeof(mods), ctypes.byref(need), 3):
            raise OSError('EnumProcessModulesEx failed, error %d' % ctypes.get_last_error())
        return mods[0]

    def close(self):
        if self.h:
            _k32.CloseHandle(self.h)
            self.h = None

    def alive(self):
        return _k32.WaitForSingleObject(self.h, 0) == 0x102  # WAIT_TIMEOUT

    def read(self, addr, n):
        if not addr or n <= 0:
            return None
        buf = ctypes.create_string_buffer(n)
        got = ctypes.c_size_t()
        self.reads += 1
        if not _k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)) or got.value != n:
            self.failed += 1
            return None
        return buf.raw

    def u64(self, addr):
        b = self.read(addr, 8)
        return struct.unpack('<Q', b)[0] if b else None

    def i32(self, addr):
        b = self.read(addr, 4)
        return struct.unpack('<i', b)[0] if b else None

    def u32(self, addr):
        b = self.read(addr, 4)
        return struct.unpack('<I', b)[0] if b else None

    def pe_identity(self):
        lfanew = self.u32(self.base + 0x3C)
        if lfanew is None or self.u32(self.base + lfanew) != 0x4550:
            return False
        self.timestamp = self.u32(self.base + lfanew + 8)
        self.size_of_image = self.u32(self.base + lfanew + 24 + 56)
        return self.timestamp == FOC_TIMESTAMP and self.size_of_image == FOC_SIZE_OF_IMAGE

    def std_string(self, addr):
        b = self.read(addr, 0x20)
        if not b:
            return None
        size, cap = struct.unpack_from('<QQ', b, 0x10)
        if size > 256 or cap < size:
            return None
        if cap <= 15:
            raw = b[:size]
        else:
            raw = self.read(struct.unpack_from('<Q', b, 0)[0], size) if size else b''
            if raw is None:
                return None
        try:
            return raw.decode('ascii')
        except UnicodeDecodeError:
            return None

    # ---- heap scan ----------------------------------------------------------------------------------------------
    def regions(self, private_only=True):
        mbi = MBI()
        addr = 0x10000
        while addr < 0x7FFFFFFF0000:
            if not _k32.VirtualQueryEx(self.h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
                break
            nxt = mbi.BaseAddress + mbi.RegionSize
            if (mbi.State == MEM_COMMIT and (mbi.Protect & 0xFF) in READABLE and not (mbi.Protect & PAGE_GUARD)
                    and (mbi.Type == MEM_PRIVATE or not private_only)):
                yield mbi.BaseAddress, mbi.RegionSize
            if nxt <= addr:
                break
            addr = nxt

    def scan(self, needles, chunk=8 << 20, private_only=True):
        """Find every occurrence of each needle (bytes) in readable committed memory. Returns
        ({needle: [(addr, bytes after the needle up to 400 bytes)]}, stats)."""
        hits = {n: [] for n in needles}
        stats = {'regions': 0, 'bytes': 0, 'unreadable': 0}
        ov = 512  # chunks overlap so a record crossing a chunk border is still whole in one of them
        for base, size in self.regions(private_only):
            stats['regions'] += 1
            off = 0
            while off < size:
                n = min(chunk + ov, size - off)
                b = self.read(base + off, n)
                if b is None:  # freed or re-protected between VirtualQueryEx and the read
                    stats['unreadable'] += 1
                    off += chunk
                    continue
                stats['bytes'] += n
                for nd in needles:
                    i = b.find(nd)
                    while i != -1:
                        if i < chunk or n < chunk + ov:  # a hit inside the overlap is reported by the next chunk
                            hits[nd].append((base + off + i, b[i:i + 400]))
                        i = b.find(nd, i + 1)
                off += chunk
        return hits, stats


RECORD_RE = re.compile(rb'A1P1\|([A-Z]+)\|([ -~]{0,360}?)\|END')


def lua_records(hits, record_re=RECORD_RE):
    """Full probe records from scan hits for a record needle (default b'A1P1|'): [(addr, text)], dedup by text."""
    out = []
    seen = set()
    for addr, tail in hits:
        m = record_re.match(tail)
        if not m:
            continue
        text = m.group(0).decode('ascii')
        if text in seen:
            continue
        seen.add(text)
        out.append((addr, text))
    return out


def parse_record(text):
    """A probe record (A1P1, kind, k=v fields, END joined by '|') -> (kind, {k: v})."""
    parts = text.split('|')
    kind = parts[1]
    kv = {}
    for p in parts[2:-1]:
        if '=' in p:
            k, v = p.split('=', 1)
            kv[k] = v
    return kind, kv


# ---- reader scaffolding shared by the kits' memory readers (cu18 t03, 2026-11-07) ------------------------------------

def kv(**k):
    return ' '.join('%s=%s' % (a, b) for a, b in k.items())


class Log:
    """Thread-safe line log (append, flushed per line)."""
    def __init__(self, path):
        self.f = open(path, 'a', encoding='ascii', errors='replace')
        self.lock = threading.Lock()
        self.t0 = time.time()

    def __call__(self, line):
        with self.lock:
            self.f.write(line + '\n')
            self.f.flush()


def wait_for_game(a, log):
    """The first StarWarsG.exe that opens and matches the FoC PE identity (or any with a.allow_unknown_exe), waiting
    up to a.wait_process_sec; None on timeout. Logs OPEN_FAILED / IGNORED lines."""
    t0 = time.time()
    ignored = set()
    open_fails = {}
    while time.time() - t0 < a.wait_process_sec:
        for pid in find_pids():
            if pid in ignored:
                continue
            try:
                p = Proc(pid)
            except OSError as e:
                # s78g 2026-11-05: a process caught while still starting fails EnumProcessModulesEx (error 299,
                # partial copy); retry instead of ignoring it for good, give up after 60 failures.
                n = open_fails.get(pid, 0) + 1
                open_fails[pid] = n
                if n == 1 or n == 60:
                    log('OPEN_FAILED ' + kv(pid=pid, n=n, error=str(e).replace(' ', '_')))
                if n >= 60:
                    ignored.add(pid)
                continue
            if p.pe_identity() or a.allow_unknown_exe:
                return p
            log('IGNORED ' + kv(pid=pid, timestamp=hex(p.timestamp or 0), size_of_image=hex(p.size_of_image or 0)))
            ignored.add(pid)
            p.close()
        time.sleep(1.0)
    return None


# ---- model pointer search (cu18 t04: was in oi_reader.py and s7s8_reader.py; selftest in oi_reader.py) ---------------
MODEL_SLOT = 0xB8          # primary animation slot in the model
MODEL_TIME = 0x148         # sim time of the model's last advance (f32 or f64)
ANIM_TYPES = 119


def plausible_ptr(q):
    return q is not None and 0x10000 <= q < 0x7FFF00000000 and q % 8 == 0


def slot_ok(b, off=MODEL_SLOT):
    """The primary animation slot at b[off:off+0x30] looks valid -> (type, mode, frame, speed) or None."""
    if len(b) < off + 0x20:
        return None
    clip, crc, typ, mode, var, fr, sp = struct.unpack_from('<QIiIiff', b, off)
    if not plausible_ptr(clip) or crc == 0:
        return None
    if not (0 <= typ < ANIM_TYPES) or mode not in (0, 1, 2) or not (0 <= var < 16):
        return None
    if not (math.isfinite(fr) and 0.0 <= fr < 5000.0 and math.isfinite(sp) and 0.0 < sp <= 20.0):
        return None
    return typ, mode, fr, sp


def time_ok(b, sim_t):
    """+0x148 holds the sim time of the last advance (global frame / 30), as f32 or f64."""
    if sim_t is None or len(b) < MODEL_TIME + 8:
        return False
    f32 = struct.unpack_from('<f', b, MODEL_TIME)[0]
    f64 = struct.unpack_from('<d', b, MODEL_TIME)[0]
    return (math.isfinite(f32) and abs(f32 - sim_t) < 2.0) or (math.isfinite(f64) and abs(f64 - sim_t) < 2.0)


def model_candidates(read, obj, sim_t, span=0x400, span2=0x200):
    """{(off, off2): score} for one object; off2 = None for a direct pointer. read(addr, n) -> bytes | None."""
    out = {}
    ob = read(obj, span)
    if not ob:
        return out
    firsts = []
    for off in range(0, span, 8):
        q = struct.unpack_from('<Q', ob, off)[0]
        if not plausible_ptr(q):
            continue
        firsts.append((off, q))
        b = read(q, MODEL_TIME + 8)
        if b and slot_ok(b):
            out[(off, None)] = 2 + (2 if time_ok(b, sim_t) else 0)
    if any(v >= 4 for v in out.values()):
        return out
    for off, q in firsts:
        qb = read(q, span2)
        if not qb:
            continue
        for off2 in range(0, span2, 8):
            r = struct.unpack_from('<Q', qb, off2)[0]
            if not plausible_ptr(r):
                continue
            b = read(r, MODEL_TIME + 8)
            if b and slot_ok(b):
                out[(off, off2)] = 1 + (2 if time_ok(b, sim_t) else 0)
    return out


def choose_model_offset(per_object):
    """per_object: [ {(off, off2): score} ] -> (key, objects, score) of the best offset, or None."""
    tally = {}
    for cands in per_object:
        for k, s in cands.items():
            t = tally.setdefault(k, [0, 0])
            t[0] += 1
            t[1] += s
    if not tally:
        return None
    k, (n, s) = max(tally.items(), key=lambda it: (it[1][0], it[1][1], -(it[0][0]), -(it[0][1] or 0)))
    need = 2 if len(per_object) >= 2 else 1
    if n < need or (n == 1 and s < 4):
        return None
    return k, n, s


# ---- game state ----------------------------------------------------------------------------------------------------

class Game:
    """Typed reads of the documented structures; every getter returns None on a failed read."""

    def __init__(self, proc):
        self.p = proc
        self.type_names = {}
        self.faction_names = {}
        self.faction_off = None

    def mode(self):
        gmm = self.p.base + RVA_GMM
        b = self.p.read(gmm, 0x58)
        if not b:
            return None
        game_type, mode_type = struct.unpack_from('<ii', b, GMM_GAME_TYPE)
        cur = struct.unpack_from('<Q', b, GMM_CUR_MODE)[0]
        stack = struct.unpack_from('<i', b, GMM_STACK)[0]
        frame = self.p.u32(cur + MODE_FRAME) if cur else None
        gframe = self.p.u32(self.p.base + RVA_FRAME_SYNC + FS_FRAME)
        return {'ptr': cur, 'type': mode_type, 'game_type': game_type, 'stack': stack, 'frame': frame,
                'gframe': gframe}

    def type_name(self, tptr):
        if not tptr:
            return '?'
        n = self.type_names.get(tptr)
        if n is None:
            n = self.p.std_string(tptr + TYPE_NAME) or '?'
            n = ''.join(c if 0x20 < ord(c) < 0x7F and c != '|' else '_' for c in n).upper() or '?'
            self.type_names[tptr] = n
        return n

    def objects(self, mode_ptr, limit=200000):
        """[(id, owner, type_name, x, y, z, flags, obj_addr)] of the mode's synchronised store."""
        gom = self.p.u64(mode_ptr + MODE_PRIMARY)
        if not gom:
            return None
        head = self.p.u64(gom + GOM_ID_HEAD)
        size = self.p.u64(gom + GOM_ID_SIZE)
        if not head or size is None or size > limit:
            return None
        out = []
        node = self.p.u64(head)
        n = 0
        while node and node != head and n <= size + 16:
            nb = self.p.read(node, 0x20)
            if not nb:
                return out
            nxt, = struct.unpack_from('<Q', nb, 0x00)
            val, = struct.unpack_from('<Q', nb, 0x18)
            if val:
                ob = self.p.read(val + OBJ_BASE, OBJ_END - OBJ_BASE)
                if ob:
                    oid, = struct.unpack_from('<i', ob, OBJ_ID - OBJ_BASE)
                    owner, = struct.unpack_from('<i', ob, OBJ_OWNER - OBJ_BASE)
                    x, y, z = struct.unpack_from('<fff', ob, OBJ_POS - OBJ_BASE)
                    tptr, = struct.unpack_from('<Q', ob, OBJ_TYPE - OBJ_BASE)
                    flags = ob[OBJ_FLAGS - OBJ_BASE]
                    out.append((oid, owner, self.type_name(tptr), x, y, z, flags, val))
            node = nxt
            n += 1
        return out

    def find_faction_name(self, fptr):
        """Faction name by search: the first std::string in the faction object's first 0x400 bytes whose text is a
        known faction name (the offset is then reused for every faction)."""
        if not fptr:
            return None
        if fptr in self.faction_names:
            return self.faction_names[fptr]
        name = None
        offs = [self.faction_off] if self.faction_off is not None else range(0, 0x400, 8)
        for off in offs:
            s = self.p.std_string(fptr + off)
            if s and s.upper() in FACTIONS:
                name = s.upper()
                self.faction_off = off
                break
        if name is None and self.faction_off is not None:
            s = self.p.std_string(fptr + self.faction_off)
            name = s.upper() if s else None
        self.faction_names[fptr] = name
        return name

    def players(self):
        """[{'id', 'index', 'human', 'popcap', 'faction', 'neutral', 'addr', 'slot'}] from the by-id array, plus
        'vec' = the index-order vector's (index, id) pairs for a cross-check."""
        pl = self.p.base + RVA_PLAYER_LIST
        b = self.p.read(pl, 0x30)
        if not b:
            return None
        vb, ve = struct.unpack_from('<QQ', b, PL_VEC)
        arr, = struct.unpack_from('<Q', b, PL_BY_ID)
        cnt, = struct.unpack_from('<i', b, PL_COUNT)
        res = []
        if arr and 0 < cnt <= 64:
            ptrs = self.p.read(arr, 8 * cnt)
            for i in range(cnt if ptrs else 0):
                pp, = struct.unpack_from('<Q', ptrs, 8 * i)
                if not pp:
                    continue
                pb = self.p.read(pp, P_END)
                if not pb:
                    continue
                fptr, = struct.unpack_from('<Q', pb, P_FACTION)
                neutral = None
                if fptr:
                    nb = self.p.read(fptr + FAC_NEUTRAL, 1)
                    neutral = nb[0] if nb else None
                res.append({'slot': i, 'addr': pp, 'index': struct.unpack_from('<i', pb, P_INDEX)[0],
                            'id': struct.unpack_from('<i', pb, P_ID)[0], 'human': pb[P_HUMAN],
                            'popcap': struct.unpack_from('<i', pb, P_POPCAP)[0],
                            'faction': self.find_faction_name(fptr), 'neutral': neutral})
        vec = []
        if vb and ve and 0 < ve - vb <= 8 * 64 and (ve - vb) % 8 == 0:
            vp = self.p.read(vb, ve - vb)
            for i in range((ve - vb) // 8 if vp else 0):
                pp, = struct.unpack_from('<Q', vp, 8 * i)
                pb = self.p.read(pp + P_INDEX, 8) if pp else None
                if pb:
                    vec.append(struct.unpack('<ii', pb))
        return {'by_id': res, 'count': cnt, 'vec': vec}
