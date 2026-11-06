"""a2g t02 / t04 (docs/re/A2G_T02_T04_PORT_VS_RETAIL.md): env-gated stderr traces for an EXPORTED copy of the engine.
Never run it on a checkout: it rewrites engine/ai/src/galactic_ai.cpp and ai_goals.cpp in place.
  git archive <commit> CMakeLists.txt engine tools/lint tools/test_env.cmd tools/cache_env.cmd tools/meg tools/formats tools/shaders/cpp | tar -x -C <dir>
  python tools/analysis/a2g_trace_patch.py <dir>        (the ActiveGoals experiment patch is gone: it became the A2g F1 fix)
  junction <dir>/toolchain -> the project toolchain, configure + build alamo_headless (modules core;vfs;data;jobs;galactic;objects;anim;commands;tactical;lua;ai;assets;terrain;apps)
Run variables:
ALAMO_A2G_TRACE=1 : budget rows at init / every fiscal tick (and reweights with =2), goal drops with reason, accepts.
ALAMO_A2G_T09OFF=1: galactic nominal_allowed_time = false (pre-t09 AllowedTime base, for the t04 comparison).
ALAMO_A2G_TRACE=3 : also one budget row per AI player per frame up to frame 10200; ALAMO_A2G_EQS=a,b,c: equation values at each budget reweight.
Lines: A2G BUD (init / tick / rew / frame), A2G ACC (accept: eta, nominal, tolerance), A2G DROP (maintenance drop + reason), A2G RREL (Resolve release).
The traces draw nothing and change no state: the HotM-human golden still MATCHes with the patched exe.
"""
import sys, pathlib
root = pathlib.Path(sys.argv[1]) / 'engine' / 'ai' / 'src'

def sub(path, old, new, count=1):
    p = root / path
    s = p.read_text(encoding='utf-8')
    if s.count(old) != count:
        raise SystemExit('%s: expected %d of %r, got %d' % (path, count, old[:60], s.count(old)))
    p.write_text(s.replace(old, new), encoding='utf-8', newline='\n')

HELP = r'''
#include <cstdio>
#include <cstdlib>
namespace alamo::ai {
inline int a2g_level() { static const int v = [] { const char* e = std::getenv("ALAMO_A2G_TRACE"); return e ? std::atoi(e) : 0; }(); return v; }
inline bool a2g_t09off() { static const bool v = std::getenv("ALAMO_A2G_T09OFF") != nullptr; return v; }
}
'''
# galactic_ai.cpp: budget dumps, accept trace, t09 toggle
sub('galactic_ai.cpp', '#include "galactic_ai_impl.hpp"\n', '#include "galactic_ai_impl.hpp"\n' + HELP + r'''
namespace alamo::ai {
static void a2g_dump(const char* tag, int64_t now, int32_t player, const AiBudget& b, const AiDefinitions& d, float credits, float gain) {
    std::fprintf(stderr, "A2G BUD %s f=%lld p=%d cr=%.4f gain=%.4f inc68=%.4f T=%.2f sumP=%.4f |", tag, static_cast<long long>(now), player,
                 static_cast<double>(credits), static_cast<double>(gain), static_cast<double>(b.income_per_second()),
                 static_cast<double>(b.period()), static_cast<double>(b.pools_total()));
    for (size_t c = 0; c < b.size(); ++c) {
        const auto& k = b.category(static_cast<int32_t>(c));
        if (k.weight == 0.0f && k.A == 0.0f && k.P == 0.0f && k.goals.empty()) continue;
        std::fprintf(stderr, " %s A=%.4f R=%.4f P=%.4f w=%.4f n=%zu;", c < d.categories.size() ? d.categories[c].c_str() : "?",
                     static_cast<double>(k.A), static_cast<double>(k.R), static_cast<double>(k.P), static_cast<double>(k.weight), k.goals.size());
    }
    std::fprintf(stderr, "\n");
}
}
''')
sub('galactic_ai.cpp', '''    if (!ctx.budget.initialised()) ctx.budget.init(raw, w->sim.credits(player));
    else ctx.budget.reweight(raw);
''', '''    const bool a2g_first = !ctx.budget.initialised();
    if (!ctx.budget.initialised()) ctx.budget.init(raw, w->sim.credits(player));
    else ctx.budget.reweight(raw);
    if (a2g_level() >= (a2g_first ? 1 : 2)) a2g_dump(a2g_first ? "init" : "rew", w->now, player, ctx.budget, w->defs, w->sim.credits(player), 0.0f);
    if (const char* eqs = std::getenv("ALAMO_A2G_EQS"); eqs && w->now <= 10200) {  // comma-separated equation names
        std::string all(eqs);
        std::fprintf(stderr, "A2G EQ f=%lld p=%d", static_cast<long long>(w->now), player);
        size_t s = 0;
        while (s <= all.size()) {
            size_t e = all.find(',', s);
            if (e == std::string::npos) e = all.size();
            const std::string name = all.substr(s, e - s);
            double v = 0.0;
            const bool ok = !name.empty() && evaluate(name, -1, v);
            std::fprintf(stderr, " %s=%s%.4f", name.c_str(), ok ? "" : "FAIL:", v);
            s = e + 1;
        }
        for (size_t c = 0; c < raw.size(); ++c) std::fprintf(stderr, " raw%zu=%.4f", c, static_cast<double>(raw[c]));
        std::fprintf(stderr, "\\n");
    }
''')
sub('galactic_ai.cpp', '''    pa->ctx.budget.tick(t.gain, sim.rules().constants.fiscal_cycle_seconds, sim.credits(t.player));
''', '''    pa->ctx.budget.tick(t.gain, sim.rules().constants.fiscal_cycle_seconds, sim.credits(t.player));
    if (a2g_level() >= 1) a2g_dump("tick", now, t.player, pa->ctx.budget, defs, sim.credits(t.player), t.gain);
''')
sub('galactic_ai.cpp', '''    g.nominal_time = w->pp_nominal_time(pp);  // A2f t09: AllowedTime base 0x6c0450(PP) [H]
''', '''    g.nominal_time = w->pp_nominal_time(pp);  // A2f t09: AllowedTime base 0x6c0450(PP) [H]
    if (a2g_level() >= 1)
        std::fprintf(stderr, "A2G ACC f=%lld p=%d goal=%s plan=%s eta=%.3f nominal=%.3f tol=%.3f tl=%.1f nsel=%zu net=%.1f\\n",
                     static_cast<long long>(w->now), player, gname.c_str(), w->library.plans[plan].name.c_str(), static_cast<double>(g.eta),
                     static_cast<double>(g.nominal_time), static_cast<double>(w->defs.goals[g.type].build_time_delay_tolerance),
                     static_cast<double>(w->defs.goals[g.type].time_limit), pp.sel.size(), static_cast<double>(pp.net));
''')
sub('galactic_ai.cpp', '''        gcfg.nominal_allowed_time = true;''', '''        gcfg.nominal_allowed_time = !a2g_t09off();''')
# =3: one row per AI player per frame after its service, frames <= 10200 (the retail probe window)
sub('galactic_ai.cpp', '''    pa->ctx.service(now, enabled, auto_resolve_busy);
}''', '''    pa->ctx.service(now, enabled, auto_resolve_busy);
    if (a2g_level() >= 3 && now <= 10200 && pa->ctx.budget.initialised())
        a2g_dump("frame", now, player, pa->ctx.budget, w.defs, w.sim.credits(player), 0.0f);
}''')

# ai_goals.cpp: drop reason in maintenance
sub('ai_goals.cpp', '#include "alamo/core/sim_rng.hpp"\n', '#include "alamo/core/sim_rng.hpp"\n' + HELP)
sub('ai_goals.cpp', '''        bool drop = g.state == GoalState::Released || g.running < 0;
        if (!drop && hooks_->uses_budget() && !hooks_->can_fund(g)) drop = true;
        if (!drop && effective_desire(g) <= 0.0f && hooks_->replaceable(g)) drop = true;
        if (!drop && hooks_->plan_invalid(g)) drop = true;  // template flag +0x60->+0xa8 not modelled
        if (!drop && hooks_->uses_budget()) {
            const float at = allowed_time(g);
            if (at > 0.0f && at < hooks_->elapsed_eta(g) && hooks_->replaceable(g)) drop = true;
        }
''', '''        bool drop = g.state == GoalState::Released || g.running < 0;
        const char* why = drop ? "notrunning" : "";
        if (!drop && hooks_->uses_budget() && !hooks_->can_fund(g)) { drop = true; why = "fund"; }
        if (!drop && effective_desire(g) <= 0.0f && hooks_->replaceable(g)) { drop = true; why = "desire"; }
        if (!drop && hooks_->plan_invalid(g)) { drop = true; why = "invalid"; }  // template flag +0x60->+0xa8 not modelled
        float a2g_at = 0.0f, a2g_eta = 0.0f;
        if (!drop && hooks_->uses_budget()) {
            const float at = allowed_time(g);
            a2g_at = at;
            a2g_eta = hooks_->elapsed_eta(g);
            if (at > 0.0f && at < a2g_eta && hooks_->replaceable(g)) { drop = true; why = "time"; }
        }
        if (drop && a2g_level() >= 1)
            std::fprintf(stderr, "A2G DROP f=%lld goal=%s why=%s running=%d at=%.3f eta_el=%.3f eta=%.3f nominal=%.3f created=%lld\\n",
                         static_cast<long long>(now), defs_->goals[g.type].name.c_str(), why, g.running, static_cast<double>(a2g_at),
                         static_cast<double>(a2g_eta), static_cast<double>(g.eta), static_cast<double>(g.nominal_time),
                         static_cast<long long>(g.created));
''')
# Resolve_Category releases: incumbent with an invalid plan, random-phase failure (which test), displaced (not re-taken)
RREL = '''std::fprintf(stderr, "A2G RREL f=%lld goal=%s why=%s running=%d eta=%.3f nominal=%.3f at=%.3f\\n", static_cast<long long>(now),
                             defs_->goals[g.type].name.c_str(), WHY, g.running, static_cast<double>(g.eta), static_cast<double>(g.nominal_time),
                             static_cast<double>(allowed_time(g)));'''
sub('ai_goals.cpp', '''            if (hooks_->plan_invalid(g)) {
                release(g, true);''', '''            if (hooks_->plan_invalid(g)) {
                if (a2g_level() >= 1) ''' + RREL.replace('WHY', '"invalid"') + '''
                release(g, true);''')
sub('ai_goals.cpp', '''        if (!hooks_->plan_activatable(g) || !fundable(g) || !time_ok(g)) {
            learning_->record_activation(g.type, g.target, false, def.activation_tracking_duration, now);
            taken[i] = 1;
            release(g, true);''', '''        if (!hooks_->plan_activatable(g) || !fundable(g) || !time_ok(g)) {
            learning_->record_activation(g.type, g.target, false, def.activation_tracking_duration, now);
            taken[i] = 1;
            if (a2g_level() >= 1 && g.state != GoalState::Proposed) {
                const char* why2 = !fundable(g) ? "random-fund" : !time_ok(g) ? "random-time" : "random-activatable";
                ''' + RREL.replace('WHY', 'why2') + '''
            }
            release(g, true);''')
sub('ai_goals.cpp', '''        if (g.state != GoalState::Proposed) release(g, true);
        else hooks_->drop_potential_plan(g);''', '''        if (g.state != GoalState::Proposed) {
            if (a2g_level() >= 1) ''' + RREL.replace('WHY', '"displaced"') + '''
            release(g, true);
        } else hooks_->drop_potential_plan(g);''')
print('patched')
