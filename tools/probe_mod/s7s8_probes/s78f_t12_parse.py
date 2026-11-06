"""s78f_t12_parse.py - report for the s78f t12 follow-up scenarios of the S7/S8 probe (2026-11-05).

  s78f_t12_parse.py <reader.log>

- SPMAT_EDGE: per window (one held T4B at d from the deployed SPMAT, explicit Attack_Target) the grenades thrown and
  the SPMAT's targeting state / target histogram.
- IDLE47V: per phase (P Pod_Walker without ATTACKIDLE, I Rebel_Infiltrator with it, V victims only) the median step
  of the synced draws per frame at the "on" (victims spawned + attack) and "off" (victims despawned) edges, plus the
  attackers' targeting state / clip at on + 60. Slot 47 redraws per service when P's step exceeds V's by ~6.
- REL: Is_Enemy between the AI player and the other players (why AI units ignored Hostile-owned victims).
- ACTC without a matching ACT: an Activate_Ability call that never returned (the AT-AT stall).
GUARD_MOVE is in s78f_extra_parse.py.
"""
import collections
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # run with python -I
import s78f_extra_parse as P  # noqa: E402


def at(m, oid, f):
    best = None
    for d in m['st'].get(oid, []):
        if int(d['frame']) <= f:
            best = d
        else:
            break
    return best


def main():
    modes = P.load(sys.argv[1])
    for n in sorted(modes, key=int):
        m = modes[n]
        recs = [P.parse_rec(r) for r in m['lua']]
        scns = {d.get('scn') for d in recs if d['kind'] == 'SCN'}
        if not scns & {'SPMAT_EDGE', 'IDLE47V', 'ATAT', 'REINF', 'GUARD_MOVE', 'SQUAD', 'ESCORT'}:
            continue
        print('== mode %s: scenarios %s' % (n, sorted(x for x in scns if x)))
        for d in recs:
            if d['kind'] == 'REL':
                print('   REL other=%s id=%s ai_enemy_of_other=%s other_enemy_of_ai=%s' % (
                    d.get('other'), d.get('other_id'), d.get('ai_enemy_of_other'), d.get('other_enemy_of_ai')))
        calls = collections.Counter((d.get('scn'), d.get('blk')) for d in recs if d['kind'] == 'ACTC')
        rets = collections.Counter((d.get('scn'), d.get('blk')) for d in recs if d['kind'] == 'ACT')
        for k, c in sorted(calls.items(), key=lambda kv: str(kv[0])):
            if rets.get(k, 0) < c:
                fr = [d['F'] for d in recs if d['kind'] == 'ACTC' and (d.get('scn'), d.get('blk')) == k]
                ab = [d.get('ab') for d in recs if d['kind'] == 'ACTC' and (d.get('scn'), d.get('blk')) == k]
                print('   NO RETURN: %s %s ability %s at F=%s' % (k[0], k[1], ab[0], fr))
        last = max((d['F'] for d in recs if d['F'] is not None), default=None)
        print('   last Lua record F=%s' % last)

        spawns = [d for d in recs if d['kind'] == 'SPAWN' and d.get('ok') == '1' and d.get('id') in m['st']]
        marks = sorted((d for d in recs if d['kind'] == 'MARK' and d['F'] is not None), key=lambda d: d['F'])

        if 'SPMAT_EDGE' in scns:
            print('\n-- SPMAT_EDGE')
            sp = [d['id'] for d in spawns if d.get('scn') == 'SPMAT_EDGE' and d.get('tag') == 'spmat']
            sid = sp[0] if sp else None
            gren = sorted(int(o['frame']) for o in m['obj'].values() if 'SPMAT_GRENADE' in o.get('type', ''))
            wins = [d for d in marks if d.get('scn') == 'SPMAT_EDGE' and d.get('mark', '').startswith('win')]
            for i, d in enumerate(wins):
                a = d['F']
                b = wins[i + 1]['F'] if i + 1 < len(wins) else a + 240
                h = collections.Counter()
                for x in m['st'].get(sid, []):
                    if a <= int(x['frame']) < b:
                        h['ts=%s loco=%s tgt=%s' % (x.get('ts'), x.get('loco'), 'y' if x.get('tgt') not in ('none', '-') else 'n')] += 1
                g = [x for x in gren if a <= x < b]
                print('   d=%-4s F=%d..%d grenades %2d first +%s  %s' % (d.get('d'), a, b, len(g), (g[0] - a) if g else '-',
                                                                       dict(h.most_common(4))))

        if 'IDLE47V' in scns:
            print('\n-- IDLE47V')
            per, unknown = P.draws_per_frame(m['rng'])
            fr = sorted(per)
            single = {f: per[f] for i, f in enumerate(fr) if i and fr[i - 1] == f - 1}

            def med(a, b):
                v = [single[f] for f in range(a, b) if f in single]
                return statistics.median(v) if v else None

            for ph in ('P', 'I', 'V'):
                ups, downs, st = [], [], []
                ids = None
                for d in marks:
                    mk = d.get('mark', '')
                    if d.get('scn') != 'IDLE47V' or not mk.startswith(ph + '_'):
                        continue
                    f = d['F']
                    b, a = med(f - 140, f - 10), med(f + 20, f + 140)
                    if b is None or a is None:
                        continue
                    (ups if '_on' in mk else downs).append(a - b)
                    if '_on' in mk:
                        if ids is None:
                            ids = [x['id'] for x in spawns if x.get('scn') == 'IDLE47V' and x.get('tag', '')[:1] == 'i'
                                   and not x.get('tag', '').startswith('iv') and x['F'] <= f and
                                   x['F'] >= f - 1800 and x.get('type') != 'T4B_Tank']
                        st.append(' '.join('%s/%s' % ((at(m, i, f + 60) or {}).get('ts'),
                                                      ((at(m, i, f + 60) or {}).get('anim') or '-').split(':')[0])
                                           for i in (ids or [])))
                if ups or downs:
                    print('   %s: on steps %s (median %+.1f); off steps %s (median %+.1f)' % (
                        ph, ' '.join('%+.0f' % x for x in ups), statistics.median(ups) if ups else float('nan'),
                        ' '.join('%+.0f' % x for x in downs), statistics.median(downs) if downs else float('nan')))
                    for s in st[:2]:
                        print('      ts/clip at on+60: %s' % s)
    return 0


if __name__ == '__main__':
    sys.exit(main())
