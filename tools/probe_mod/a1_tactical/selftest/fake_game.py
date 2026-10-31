#!/usr/bin/env python3
"""fake_game.py - test target for a1_reader.py (never used in real runs).

  fake_game.py <scenario.json>      prints "BASE 0x... PID n", then plays the scenario and exits

Lays out the documented StarWarsG.exe structures (a1mem.py) in its own memory: an image span with the
GameModeManager, FrameSynchronizer and PlayerList statics, mode objects, an object store (MSVC unordered_map list),
object / type / player / faction records. It plays the scenario's modes frame by frame (galactic, then the land /
space battles) and applies its events (owner changes, population capacity changes, objects added at a frame), and
it keeps the scenario's Lua records as bytes objects from their frame on, so the heap scan can find them.

Scenario JSON: {"fps": 300, "players": [{"id", "faction", "human", "neutral", "popcap"}],
  "modes": [{"type": 0|1|2, "stack": 1|2, "frames": N, "slow_frames": n (first n frames at 30 fps), "objects": [{"type", "owner", "x", "y", "frame"}],
             "events": [{"frame", "kind": "owner"|"popcap", "obj"|"player", "value"}],
             "records": [{"frame", "parts": ["A1P1", "FSS", ...]}]}]}
Records are given as parts and joined here, so no whole record ever appears in this file or the scenario text.
"""
import ctypes
import json
import os
import struct
import sys
import time

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import a1mem as L  # noqa: E402

k32 = ctypes.WinDLL('kernel32')
k32.VirtualAlloc.restype = ctypes.c_void_p
k32.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32]
KEEP = []


def alloc(n):
    b = ctypes.create_string_buffer(n)
    KEEP.append(b)
    return ctypes.addressof(b)


def w(addr, fmt, *v):
    data = struct.pack('<' + fmt, *v)
    ctypes.memmove(addr, data, len(data))


def std_string(addr, s):
    raw = s.encode('ascii')
    if len(raw) <= 15:
        ctypes.memmove(addr, raw + b'\0' * (16 - len(raw)), 16)
        w(addr + 0x10, 'QQ', len(raw), 15)
    else:
        p = alloc(len(raw) + 1)
        ctypes.memmove(p, raw, len(raw))
        w(addr, 'Q', p)
        w(addr + 0x10, 'QQ', len(raw), len(raw))


def main():
    sc = json.load(open(sys.argv[1]))
    img = k32.VirtualAlloc(None, 0xCC0000, 0x3000, 0x04)
    gmm = img + L.RVA_GMM
    fs = img + L.RVA_FRAME_SYNC
    pl = img + L.RVA_PLAYER_LIST
    # players
    factions = {}
    by_id = {}
    for p in sc['players']:
        f = factions.get(p['faction'])
        if f is None:
            f = alloc(0x200)
            std_string(f + 0x40, p['faction'].capitalize())
            w(f + L.FAC_NEUTRAL, 'B', 1 if p.get('neutral') else 0)
            factions[p['faction']] = f
        pa = alloc(L.P_END)
        w(pa + L.P_INDEX, 'ii', p['index'] if 'index' in p else len(by_id), p['id'])
        w(pa + L.P_HUMAN, 'B', 1 if p.get('human') else 0)
        w(pa + L.P_FACTION, 'Q', f)
        w(pa + L.P_POPCAP, 'i', p.get('popcap', 0))
        by_id[p['id']] = pa
    count = max(by_id) + 1
    arr = alloc(8 * count)
    for pid, pa in by_id.items():
        w(arr + 8 * pid, 'Q', pa)
    vec = alloc(8 * len(by_id))
    for i, pid in enumerate(sorted(by_id)):
        w(vec + 8 * i, 'Q', by_id[pid])
    w(pl + L.PL_VEC, 'QQQ', vec, vec + 8 * len(by_id), vec + 8 * len(by_id))
    w(pl + L.PL_BY_ID, 'Q', arr)
    w(pl + L.PL_COUNT, 'i', count)
    print('BASE 0x%x PID %d' % (img, os.getpid()), flush=True)
    types = {}
    shown = []  # keeps record bytes objects alive
    gframe = 0
    dt = 1.0 / sc.get('fps', 300)
    for mode in sc['modes']:
        mb = alloc(0x400)
        gom = alloc(0x200)
        sentinel = alloc(0x20)
        w(sentinel, 'QQ', sentinel, sentinel)
        w(gom + L.GOM_ID_HEAD, 'QQ', sentinel, 0)
        w(mb + L.MODE_PRIMARY, 'Q', gom)
        state = {'last': sentinel, 'size': 0, 'next_id': 100}
        objs = {}

        def add_object(o):
            t = types.get(o['type'])
            if t is None:
                t = alloc(0x200)
                std_string(t + L.TYPE_NAME, o['type'])
                types[o['type']] = t
            oa = alloc(0x400)
            oid = o.get('id', state['next_id'])
            state['next_id'] = max(state['next_id'], oid) + 1
            w(oa + L.OBJ_ID, 'i', oid)
            w(oa + L.OBJ_OWNER, 'i', o['owner'])
            w(oa + L.OBJ_POS, 'fff', o.get('x', 0.0), o.get('y', 0.0), 0.0)
            w(oa + L.OBJ_TYPE, 'Q', t)
            node = alloc(0x20)
            w(node, 'QQiiQ', sentinel, state['last'], oid, 0, oa)
            w(state['last'], 'Q', node)
            state['last'] = node
            state['size'] += 1
            w(gom + L.GOM_ID_SIZE, 'Q', state['size'])
            objs[oid] = oa

        for o in mode.get('objects', []):
            if o.get('frame', 0) == 0:
                add_object(o)
        w(gmm + L.GMM_STACK, 'i', mode.get('stack', 1))
        w(gmm + L.GMM_GAME_TYPE, 'ii', mode.get('game_type', 0), mode['type'])
        w(mb + L.MODE_FRAME, 'I', 0)
        w(gmm + L.GMM_CUR_MODE, 'Q', mb)
        for F in range(mode['frames']):
            w(mb + L.MODE_FRAME, 'I', F)
            w(fs + L.FS_FRAME, 'I', gframe)
            for o in mode.get('objects', []):
                if o.get('frame', 0) == F and F:
                    add_object(o)
            for e in mode.get('events', []):
                if e['frame'] != F:
                    continue
                if e['kind'] == 'owner':
                    w(objs[e['obj']] + L.OBJ_OWNER, 'i', e['value'])
                elif e['kind'] == 'popcap':
                    w(by_id[e['player']] + L.P_POPCAP, 'i', e['value'])
            for r in mode.get('records', []):
                if r['frame'] == F:
                    shown.append(('|'.join(r['parts'])).encode('ascii') + b'\0')
            gframe += 1
            time.sleep(1.0 / 30 if F < mode.get('slow_frames', 0) else dt)  # real speed where the test needs it
        shown.clear()  # the mode's Lua states are gone; their strings may still linger in freed memory
    return 0


if __name__ == '__main__':
    sys.exit(main())
