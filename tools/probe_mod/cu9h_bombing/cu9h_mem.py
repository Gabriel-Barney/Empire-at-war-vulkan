#!/usr/bin/env python3
"""cu9h_mem.py - the retail memory layout the cu9h bombing-run probe reads, and one snapshot of it (roadmap group cu9h
t02; docs/re/HUD_GAPS.md sec. 9). Shared by the runner's reader (cu9h_run.py), the debugger's per-hit snapshot
(cu9h_dbg.py) and the driver's mark tool (cu9h_mark.py). Read-only: `read(addr, n)` is any ReadProcessMemory wrapper.

Layout (RVAs in StarWarsG.exe FoC x64 1.121; [H] from docs/re unless tagged):
  GMM 0xb153e0: +0x1c mode type (0 galactic, 1 land, 2 space), +0x38 current mode, +0x50 mode stack depth
  frame sync 0xb0a320: +0 global frame; mode +0x10 = mode frame (the bombing-run "now")
  player list 0xa16fd0: +0 vector begin, +0x30 local index; player +0x4c id (0x294a70: local id = record index)
  HUD object 0xb27f60 (a static object, not a pointer; UI.md sec. 0):
    +0x211c HUD mode (3 land, 2 space, 1 galactic; HUD_GAPS sec. 9.0)
    +0x20b2 bombing-run targeting flag (byte), +0x20c0 targeting blob (alTerrainShadowBlob*), blob +0xb8 colour
    (4 floats; 0x30c3b0 writes (0,1,0,1) or, when 0x494540 fails, (1,0,0,1) [M: RGBA order])
    +0x26dc / +0x26e0 / +0x26e4 ground point x, y, z under the cursor (the blob position; HUD_GAPS sec. 9.9 item 4)
  cursor pixel 0xb0a158 (x) / 0xb0a15c (y) (ints, the 0x3bb2d0 pick input)
  land mode +0x38 BombingRunManagerClass: +0 enabled, +8 mode, +0x14 default interval, +0x20 records, +0x28 count;
    record (0x78 bytes): +0 enabled, +1 available, +4 ready frame, +8 interval, +0xc last start, +0x28 in progress,
    +0x40 bomber count (L1_S4_REMAINING sec. 3.7)
  land mode +0x1f0 -> +0x30: controls locked (0x35f790)
  component table 0xa174e0 (72-byte entries, +8 = component*): b_special_weapon = enum 0xe5 (HUD_GAPS sec. 9.1);
    ButtonComponentClass (vftable RVA 0x863b60): +0xc0 bit 4 = disabled (vf +0x80 = 0x34b7e0), +0x5f0 state (4 while
    disabled), +0x43c dial = int(fraction x 100) (vf +0xf0 = 0x3515d0, its first dial)  [read from the decompile for
    this kit, cu9h t02]
"""
import struct

RVA_GMM = 0xB153E0
RVA_FRAME_SYNC = 0xB0A320
RVA_PLAYER_LIST = 0xA16FD0
RVA_HUD = 0xB27F60
RVA_CURSOR_X = 0xB0A158
RVA_CURSOR_Y = 0xB0A15C
RVA_COMPONENTS = 0xA174E0
COMP_ENTRY = 72
COMP_SPECIAL_WEAPON = 0xE5
VFT_BUTTON = 0x863B60

HUD_MODE = 0x211C
HUD_FLAG = 0x20B2
HUD_BLOB = 0x20C0
HUD_GROUND = 0x26DC          # x; y = +0x26e0, z = +0x26e4
BLOB_COLOUR = 0xB8

MODE_FRAME = 0x10
MODE_BOMBING = 0x38
MODE_LOCK = 0x1F0
REC_SIZE = 0x78
COMP_FLAGS = 0xC0
COMP_STATE = 0x5F0
COMP_DIAL = 0x43C

FPS = 30


def _u8(b, o):
    return b[o]


def colour_name(c):
    """'GREEN' / 'RED' for the two values 0x30c3b0 writes, 'OTHER' otherwise, None without a colour."""
    if not c:
        return None
    r = tuple(round(x, 3) for x in c)
    if r == (0.0, 1.0, 0.0, 1.0):
        return 'GREEN'
    if r == (1.0, 0.0, 0.0, 1.0):
        return 'RED'
    return 'OTHER'


def expected_dial(s):
    """The dial value (int(f x 100)) HUD_GAPS sec. 9.3 predicts from the record, or None when it cannot be computed
    (no record, or a run in progress: the remaining flight time is not read)."""
    if s.get('rec_en') is None or s.get('mf') is None:
        return None
    if s.get('ready'):
        return 100
    if not s['rec_en']:
        return 100                        # rule 1 -> 0, shown as 1.0
    if s['rec_inp']:
        return None
    now = s['mf']
    r = now if s['rec_av'] else s['rec_ready']       # 0x4950f0 for "not in progress"
    if now >= r:
        return 100
    st = s['rec_last']
    if r <= st or st == -1 or now <= st:
        return 100                        # rule 4 -> 0, shown as 1.0
    f = struct.unpack('<f', struct.pack('<f', float(now - st) / float(r - st)))[0]   # in (0, 1): not shown as 1.0
    return int(struct.unpack('<f', struct.pack('<f', f * 100.0))[0])                  # 0x3515d0: int(f x 100)


def snapshot(read, base):
    """One read of everything the probe needs. Missing parts are None. `read(addr, n)` -> bytes or None."""
    s = {'mt': None, 'stack': None, 'mode': None, 'mf': None, 'gf': None}

    def u64(a):
        b = read(a, 8)
        return struct.unpack('<Q', b)[0] if b else None

    def i32(a):
        b = read(a, 4)
        return struct.unpack('<i', b)[0] if b else None

    g = read(base + RVA_GMM, 0x58)
    if g:
        s['mt'] = struct.unpack_from('<i', g, 0x1C)[0]
        s['mode'] = struct.unpack_from('<Q', g, 0x38)[0]
        s['stack'] = struct.unpack_from('<i', g, 0x50)[0]
    if s['mode']:
        s['mf'] = i32(s['mode'] + MODE_FRAME)
    s['gf'] = i32(base + RVA_FRAME_SYNC)
    # HUD
    h1 = read(base + RVA_HUD + HUD_FLAG, 0x18)
    if h1:
        s['flag'] = h1[0]
        s['blob'] = struct.unpack_from('<Q', h1, HUD_BLOB - HUD_FLAG)[0]
    else:
        s['flag'] = s['blob'] = None
    hm = i32(base + RVA_HUD + HUD_MODE)
    s['hud_mode'] = hm
    gp = read(base + RVA_HUD + HUD_GROUND, 12)
    s['ground'] = struct.unpack('<fff', gp) if gp else None
    s['colour'] = None
    if s['blob']:
        cb = read(s['blob'] + BLOB_COLOUR, 16)
        s['colour'] = struct.unpack('<ffff', cb) if cb else None
    s['blob_col'] = colour_name(s['colour'])
    cx, cy = i32(base + RVA_CURSOR_X), i32(base + RVA_CURSOR_Y)
    s['cursor'] = (cx, cy) if cx is not None and cy is not None else None
    # local player
    pl = read(base + RVA_PLAYER_LIST, 0x38)
    s['local_id'] = None
    if pl:
        vb, ve = struct.unpack_from('<QQ', pl, 0)
        li = struct.unpack_from('<i', pl, 0x30)[0]
        if vb and ve and 0 <= li < (ve - vb) // 8 <= 64:
            pp = u64(vb + 8 * li)
            s['local_id'] = i32(pp + 0x4C) if pp else None
    # bombing-run manager / record (land only)
    for k in ('mgr', 'mgr_en', 'mgr_interval', 'rec_count', 'rec_en', 'rec_av', 'rec_inp', 'rec_ready', 'rec_int',
              'rec_last', 'rec_bombers', 'locked', 'recs_enabled'):
        s[k] = None
    if s['mt'] == 1 and s['mode']:
        mgr = u64(s['mode'] + MODE_BOMBING)
        s['mgr'] = mgr
        lk = u64(s['mode'] + MODE_LOCK)
        lb = read(lk + 0x30, 1) if lk else None
        s['locked'] = (lb[0] if lb else None) if lk else 0
        if mgr:
            mb = read(mgr, 0x30)
            if mb:
                s['mgr_en'] = mb[0]
                s['mgr_interval'] = struct.unpack_from('<i', mb, 0x14)[0]
                arr = struct.unpack_from('<Q', mb, 0x20)[0]
                cnt = struct.unpack_from('<i', mb, 0x28)[0]
                s['rec_count'] = cnt
                if arr and 0 < cnt <= 16:
                    recs = read(arr, REC_SIZE * cnt)
                    if recs:
                        s['recs_enabled'] = ','.join(str(i) for i in range(cnt) if recs[REC_SIZE * i])
                        lid = s['local_id']
                        if lid is not None and 0 <= lid < cnt:
                            o = REC_SIZE * lid
                            s['rec_en'] = recs[o]
                            s['rec_av'] = recs[o + 1]
                            s['rec_inp'] = recs[o + 0x28]
                            s['rec_ready'], s['rec_int'], s['rec_last'] = struct.unpack_from('<iii', recs, o + 4)
                            s['rec_bombers'] = struct.unpack_from('<i', recs, o + 0x40)[0]
    # 0x494410 for the local player (no skirmish: the campaign probe; 0x140b15490 is always clear)
    s['ready'] = None
    if s['rec_en'] is not None and s['mgr_en'] is not None:
        s['ready'] = int(bool(s['mgr_en'] and s['rec_en'] and s['rec_av'] and not s['rec_inp'] and not s['locked']))
    # the b_special_weapon component
    s['comp'] = u64(base + RVA_COMPONENTS + COMP_ENTRY * COMP_SPECIAL_WEAPON + 8)
    s['comp_vft'] = s['comp_disabled'] = s['comp_state'] = s['comp_dial'] = None
    if s['comp']:
        vft = u64(s['comp'])
        s['comp_vft'] = (vft - base) & 0xFFFFFFFF if vft else None
        cb = read(s['comp'] + COMP_FLAGS, 1)
        s['comp_disabled'] = (cb[0] >> 2) & 1 if cb else None
        s['comp_state'] = i32(s['comp'] + COMP_STATE)
        s['comp_dial'] = i32(s['comp'] + COMP_DIAL)
    s['dial_expected'] = expected_dial(s)
    s['phase'] = phase(s)
    return s


def phase(s):
    if s['mt'] is None:
        return 'NO_GAME'
    if s['mt'] == 0:
        return 'GALAXY'
    if s['mt'] != 1:
        return 'SPACE' if s['mt'] == 2 else 'OTHER'
    if (s['stack'] or 0) < 2:
        return 'MENU_OR_SKIRMISH'
    if s['rec_en'] is None or not s['rec_en']:
        return 'BATTLE_NO_RECORD'
    if s.get('flag'):
        return 'TARGETING'
    if s['locked']:
        return 'LOCKED'
    if s['rec_inp']:
        return 'RUN_IN_PROGRESS'
    if s['ready']:
        return 'READY'
    return 'COOLDOWN'


def secs_to_ready(s):
    if s.get('rec_en') and not s.get('rec_inp') and not s.get('rec_av') and s.get('mf') is not None \
            and s.get('rec_ready') is not None:
        return max(0.0, (s['rec_ready'] - s['mf']) / float(FPS))
    return None


def fmt(v):
    if v is None:
        return '-'
    if isinstance(v, float):
        return '%.2f' % v
    if isinstance(v, tuple):
        return ','.join(fmt(x) for x in v)
    if isinstance(v, int) and v > 0xFFFFFFFF:
        return '%x' % v
    return str(v)


KEYS = ('phase', 'mt', 'stack', 'mf', 'gf', 'hud_mode', 'flag', 'blob', 'blob_col', 'colour', 'ground', 'cursor',
        'local_id', 'mgr_en', 'mgr_interval', 'rec_count', 'recs_enabled', 'rec_en', 'rec_av', 'rec_inp', 'rec_ready',
        'rec_int', 'rec_last', 'rec_bombers', 'locked', 'ready', 'comp_vft', 'comp_disabled', 'comp_state', 'comp_dial',
        'dial_expected')


def line(s, keys=KEYS):
    """key=value tokens (no spaces inside a value)."""
    out = []
    for k in keys:
        v = s.get(k)
        if k in ('blob', 'comp_vft') and isinstance(v, int):
            out.append('%s=%x' % (k, v))
        else:
            out.append('%s=%s' % (k, fmt(v)))
    return ' '.join(out)


def summary(s):
    """One line for the driver."""
    t = secs_to_ready(s)
    parts = ['phase=%s' % s['phase'],
             'button=%s' % ('-' if s.get('comp_disabled') is None else ('DISABLED' if s['comp_disabled'] else 'ENABLED')),
             'dial=%s' % fmt(s.get('comp_dial')), 'dial_expected=%s' % fmt(s.get('dial_expected')),
             'targeting=%s' % fmt(s.get('flag')), 'blob=%s' % (s.get('blob_col') or 'none')]
    if t is not None:
        parts.append('ready_in=%.1fs' % t)
    parts += ['ground=%s' % fmt(s.get('ground')), 'cursor=%s' % fmt(s.get('cursor')), 'mf=%s' % fmt(s.get('mf'))]
    return ' '.join(parts)


def parse_line(text):
    """key=value tokens back into a dict of strings (for the report)."""
    d = {}
    for tok in text.split():
        if '=' in tok:
            k, v = tok.split('=', 1)
            d[k] = v
    return d
