#!/usr/bin/env python3
"""port_a2j_patch.py - env-gated stderr traces for an EXPORTED copy of the engine (a2j t02 / t04 port reference).

Never run it on a checkout: it rewrites engine/ai/src/galactic_ai.cpp and engine/apps/headless/ai_run.cpp in place.
Run it after tools/analysis/a2g_trace_patch.py (it extends that patch's per-frame budget row). a2j_port.py does the
whole export / patch / build / run; this file only patches.

  python port_a2j_patch.py <export dir>

Run variables (all read-only traces; nothing draws from the RNG or changes state):
  ALAMO_A2H_EXTRA_EQS=FILE  one "name|expression" per line, added to the equation set after the shipped files (the
                            a2j perception token equations A2FB_D4_*, written by a2j_port.py from a2fb_spec.py)
  ALAMO_A2J_PERC=Underworld:Kashyyyk,Empire:Felucia   faction:planet pairs, sampled right after that player's AI
                            service at frames 0, 30, .. 600, then every 300 frames up to ALAMO_A2J_MAXF (default 3000)
  ALAMO_A2J_EQS=KEY:Name,KEY:Name,...   the equations evaluated per sample (a2j_port.py passes the kit's D4 keys)
  ALAMO_A2G_TRACE=3          (a2g patch) the per-frame budget row; this patch keeps it up to frame 10200 and then every
                            30th frame for the rest of the run (30-minute runs), for the per-cycle P / I table
Line: A2J PERC f=<frame> p=<player> fac=<FACTION> tgt=<planet> t=<planet index> own=<owner faction> KEY=value ...
      (KEY=FAIL: the evaluation failed; in retail that would be nil and the calling script would stop)
"""
import pathlib
import sys

NL = chr(92) + 'n'


def sub(root, path, old, new, count=1):
    p = root / path
    s = p.read_text(encoding='utf-8')
    if s.count(old) != count:
        raise SystemExit('%s: expected %d of %r, got %d' % (path, count, old[:70], s.count(old)))
    p.write_text(s.replace(old, new), encoding='utf-8', newline='\n')


EXTRA_EQS = r'''
    if (const char* xf = std::getenv("ALAMO_A2H_EXTRA_EQS")) {  // scratch (a2j): name|expression per line
        std::FILE* f = std::fopen(xf, "rb");
        if (f) {
            char line[4096];
            while (std::fgets(line, sizeof line, f)) {
                std::string l(line);
                while (!l.empty() && (l.back() == 10 || l.back() == 13)) l.pop_back();
                const size_t bar = l.find('|');
                if (bar == std::string::npos) continue;
                ai::LoadReport rep;
                eqs.add(l.substr(0, bar), l.substr(bar + 1), &rep);
                for (const auto& m : rep.messages) std::fprintf(stderr, "A2H EQADD %s\n", m.c_str());
            }
            std::fclose(f);
            eqs.link(nullptr);
        }
    }
'''.replace('\\n', NL)

PERC = r'''
    if (const char* pe = std::getenv("ALAMO_A2J_PERC")) {  // scratch (a2j t02): D4 perception reference
        static const int64_t a2j_maxf = [] { const char* e = std::getenv("ALAMO_A2J_MAXF"); return e ? std::atoll(e) : 3000; }();
        if (now >= 0 && now <= a2j_maxf && ((now <= 600 && now % 30 == 0) || now % 300 == 0)) {
            auto upper = [](std::string s) { for (char& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch))); return s; };
            auto fac_of = [&w, &upper](int32_t pl) -> std::string {
                if (pl < 0 || static_cast<size_t>(pl) >= w.sim.players().size()) return "-";
                const uint32_t f = w.sim.players()[static_cast<size_t>(pl)].faction;
                return f < w.sim.rules().factions.size() ? upper(w.sim.rules().factions[f].name) : std::string("?");
            };
            const std::string me = fac_of(player);
            std::string all(pe);
            size_t s0 = 0;
            while (s0 < all.size()) {
                size_t e0 = all.find(',', s0);
                if (e0 == std::string::npos) e0 = all.size();
                const std::string pair = all.substr(s0, e0 - s0);
                s0 = e0 + 1;
                const size_t colon = pair.find(':');
                if (colon == std::string::npos || upper(pair.substr(0, colon)) != me) continue;
                const std::string want = upper(pair.substr(colon + 1));
                int32_t tgt = -1;
                const auto& names = w.sim.map().planets.name;
                for (size_t i = 0; i < names.size(); ++i)
                    if (upper(names[i]) == want) tgt = static_cast<int32_t>(i);
                if (tgt < 0) { std::fprintf(stderr, "A2J PERC f=%lld p=%d fac=%s tgt=%s none\n", static_cast<long long>(now), player, me.c_str(), want.c_str()); continue; }
                std::fprintf(stderr, "A2J PERC f=%lld p=%d fac=%s tgt=%s t=%d own=%s", static_cast<long long>(now), player, me.c_str(),
                             w.planet_name(static_cast<PlanetIndex>(tgt)).c_str(), tgt, fac_of(w.sim.planets()[static_cast<size_t>(tgt)].owner).c_str());
                if (const char* ql = std::getenv("ALAMO_A2J_EQS")) {
                    std::string q(ql);
                    size_t q0 = 0;
                    while (q0 < q.size()) {
                        size_t q1 = q.find(',', q0);
                        if (q1 == std::string::npos) q1 = q.size();
                        const std::string kv = q.substr(q0, q1 - q0);
                        q0 = q1 + 1;
                        const size_t c2 = kv.find(':');
                        const std::string key = c2 == std::string::npos ? kv : kv.substr(0, c2);
                        const std::string nm = c2 == std::string::npos ? kv : kv.substr(c2 + 1);
                        double x = 0.0;
                        if (!nm.empty() && pa->evaluate(nm, tgt, x)) std::fprintf(stderr, " %s=%.6g", key.c_str(), x);
                        else std::fprintf(stderr, " %s=FAIL", key.c_str());
                    }
                }
                std::fprintf(stderr, "\n");
            }
        }
    }
'''.replace('\\n', NL)


def main():
    root = pathlib.Path(sys.argv[1])
    A = 'engine/apps/headless/ai_run.cpp'
    old = '    ai::load_equation_files(*files, files->list(ai::kPerceptionEquationGlob), eqs);\n'
    sub(root, A, old, old + EXTRA_EQS)
    G = 'engine/ai/src/galactic_ai.cpp'
    # a2g patch's per-frame row: keep it to 10200, then every 30th frame (a 30-minute run stays readable)
    sub(root, G, 'if (a2g_level() >= 3 && now <= 10200 && pa->ctx.budget.initialised())',
        'if (a2g_level() >= 3 && (now <= 10200 || now % 30 == 0) && pa->ctx.budget.initialised())')
    old = '        a2g_dump("frame", now, player, pa->ctx.budget, w.defs, w.sim.credits(player), 0.0f);\n}'
    sub(root, G, old, '        a2g_dump("frame", now, player, pa->ctx.budget, w.defs, w.sim.credits(player), 0.0f);\n' + PERC + '}')
    s = (root / G).read_text(encoding='utf-8')
    if '#include <cctype>' not in s:
        (root / G).write_text('#include <cctype>\n' + s, encoding='utf-8', newline='\n')
    print('a2j patched')


if __name__ == '__main__':
    main()
