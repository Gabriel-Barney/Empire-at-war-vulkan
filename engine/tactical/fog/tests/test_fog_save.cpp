// alamo_tactical_fog_save_tests (no framework; non-zero exit on failure).
// N1 stage 2 (saves during a tactical battle): FogGrid / FogOfWar write / read.
//   * FogGrid: write -> read -> write identical, hash equal, bad geometry / overlong packets / truncation rejected
//     (unchanged);
//   * scenarios (land with the setup phase, ally sharing, owner changes, garrisons, fired flashes, Lua reveal / undo,
//     Reveal_All, reveal-range modifiers, battle scale, overrun, Disable_Rendering; space with a dense nebula mask):
//     saved at several frames (during setup, mid-fade, after undo...), a fresh fog is built the way the host builds it
//     (start + relations + dense mask + refog hook) and loaded; both run on and must have identical hashes, save bytes
//     and refog callbacks every frame;
//   * recounting the Lua snapshots after a load (the old rebuild contract) double-counts: shown, so hosts don't;
//   * random single-bit corruption never crashes and a rejected load leaves the fog unchanged.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/tactical/fog/fog_of_war.hpp"

using namespace alamo;
using namespace alamo::tactical::fog;

namespace {

int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

using Bytes = std::vector<uint8_t>;

template <class T>
Bytes bytes_of(const T& t) {
    ByteWriter w;
    t.write(w);
    return w.bytes();
}
template <class T>
bool load(T& t, std::span<const uint8_t> b) {
    ByteReader r(b);
    return t.read(r) && r.ok() && r.done();
}
template <class T>
uint64_t hash_of(const T& t) {
    StateHasher h;
    t.hash(h);
    return h.digest();
}
template <class T>
bool rejects(T& target, const Bytes& bad) {
    const Bytes before = bytes_of(target);
    ByteReader r(bad);
    return !target.read(r) && bytes_of(target) == before;
}
template <class T>
bool rejects_truncation(T& target, const Bytes& good, size_t stride) {
    const Bytes before = bytes_of(target);
    for (size_t n = 0; n < good.size();) {
        ByteReader r(std::span<const uint8_t>(good.data(), n));
        if (target.read(r) || bytes_of(target) != before) {
            std::printf("  truncation at %zu of %zu accepted or changed the object\n", n, good.size());
            return false;
        }
        n += (n < 400 || n + 96 > good.size()) ? 1 : stride;
    }
    return true;
}
void put_u32(Bytes& b, size_t at, uint32_t v) {
    for (size_t i = 0; i < 4; ++i) b[at + i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFFu);
}

FogObject unit(int32_t owner, float x, float y, float sight) {
    FogObject o;
    o.owner = owner;
    o.x = x;
    o.y = y;
    o.sight_range = sight;
    return o;
}

// ================================================================ FogGrid
void test_grid() {
    FogGrid a;
    a.init({0.0f, 0.0f, 600.0f, 450.0f}, 15.0f, 15.0f, 3);
    CellList keep;
    const RefogFn none;
    for (uint32_t f = 1; f <= 200; ++f) {
        if (f % 16 == 3) a.decay(f, 21, none);
        if (f % 9 == 0) {
            CellList l;
            a.reveal(static_cast<float>(f * 7 % 600), static_cast<float>(f * 13 % 450), 40.0f, &l, RevealFlags{}, f);
            if (f % 27 == 0) a.undo(l);
            else if (keep.empty()) keep = l;
        }
        if (f == 50) a.flash(300.0f, 200.0f, 70.0f);
        if (f == 120) a.reveal_all(f);
        if (f == 150) a.undo_reveal_all();
    }
    const Bytes good = bytes_of(a);
    FogGrid b;
    b.init({0.0f, 0.0f, 100.0f, 100.0f}, 10.0f, 10.0f, 1);  // replaced
    CHECK(load(b, good) && bytes_of(b) == good && hash_of(b) == hash_of(a));
    // Both continue identically (decay table, last decay, fade bytes).
    bool same = true;
    for (uint32_t f = 201; f <= 420; ++f) {
        if (f % 16 == 3) {
            a.decay(f, 21, none);
            b.decay(f, 21, none);
        }
        if (f == 260) {
            a.undo(keep);
            b.undo(keep);
        }
        same = same && hash_of(a) == hash_of(b);
    }
    CHECK(same);
    // Empty grid (no arrays) round trip.
    FogGrid e, e2;
    e.init({0.0f, 0.0f, 100.0f, 100.0f}, 0.0f, 15.0f, 0);
    const Bytes eb = bytes_of(e);
    CHECK(load(e2, eb) && !e2.valid() && bytes_of(e2) == eb);
    FogGrid t;
    CHECK(rejects_truncation(t, good, 3));
    // Layout: u32 version, i32 player, 4 x f32 box, i32 W @24, i32 H @28, u32 N @32, f32 cw, f32 ch, u32 last decay,
    // i32 step, bool, 256-byte table, packets @309.
    Bytes bad = good;
    put_u32(bad, 24, 513);
    CHECK(rejects(t, bad));
    bad = good;
    put_u32(bad, 32, 7);  // N != W x H
    CHECK(rejects(t, bad));
    bad = good;
    put_u32(bad, 16, 0xC2C80000u);  // box x1 = -100 < x0
    CHECK(rejects(t, bad));
    // A 1 x 2 grid: each plane is one literal packet (0x01, v0, v1); a longer packet runs past N.
    FogGrid tiny, tiny2;
    tiny.init({0.0f, 0.0f, 10.0f, 30.0f}, 15.0f, 15.0f, 0);
    CHECK(tiny.width() == 1 && tiny.height() == 2);
    const Bytes tb = bytes_of(tiny);
    CHECK(tb.size() == 309 + 4 * 3 && tb[309] == 0x01 && load(tiny2, tb));
    bad = tb;
    bad[309] = 0x02;  // 3 literal bytes for 2 cells
    CHECK(rejects(tiny2, bad));
    bad = tb;
    bad[309] = 0x80;  // repeat 3 for 2 cells
    CHECK(rejects(tiny2, bad));
    std::printf("fog grid: %u cells, %zu save bytes (raw planes %u)\n", a.cells(), good.size(), a.cells() * 4u);
}

// ================================================================ scenarios
struct FWorld {
    FogOfWar fog;
    std::vector<FogCells> lua;  // script-held snapshots (the Lua module saves them in its own chunk)
    uint64_t refog = 0;         // digest of the refog callbacks (player, cell) in call order
    uint32_t refogs = 0;
};

struct SrcDef {
    int32_t id = 0, owner = 0;
    float cx = 0.0f, cy = 0.0f, rx = 0.0f, ry = 0.0f, w = 0.0f, phase = 0.0f, sight = 0.0f;
    uint32_t die = 0;
};

struct FogScenario {
    const char* name = "";
    FogSetup setup;
    std::vector<FogOfWar::DenseCircle> dense;
    std::vector<SrcDef> src;
    std::function<void(FWorld&, uint32_t)> extra;  // scenario-specific host / script calls before the sources
    uint32_t frames = 0;
};

FogObject src_obj(const SrcDef& s, uint32_t f, const FogBox& box) {
    const float t = static_cast<float>(f) * s.w + s.phase;
    FogObject o = unit(s.owner, s.cx + s.rx * std::cos(t), s.cy + s.ry * std::sin(t), s.sight);
    if (s.id % 7 == 0 && f >= 150) o.owner = (s.owner + 1) % 3;  // captured at 150
    o.x = std::clamp(o.x, box.x0, box.x1);
    o.y = std::clamp(o.y, box.y0, box.y1);
    o.dense_multiplier = 0.5f;
    if (s.id % 4 == 0) {
        o.has_model = true;
        o.box_hx = 12.0f;
        o.box_hy = 20.0f;
    }
    return o;
}

// The battle host at battle start (and the loader before read()).
std::unique_ptr<FWorld> build(const FogScenario& sc) {
    auto w = std::make_unique<FWorld>();
    w->fog.start(sc.setup);
    w->fog.set_relation(0, 2, 0);
    w->fog.set_relation(2, 0, 0);
    if (!sc.dense.empty()) w->fog.set_dense_obstacles(sc.dense);
    FWorld* p = w.get();
    w->fog.set_refog([p](int32_t player, uint32_t cell) {
        p->refog = (p->refog ^ ((static_cast<uint64_t>(static_cast<uint32_t>(player)) << 32) | cell)) * 0x100000001B3ull;
        ++p->refogs;
    });
    return w;
}

void step(const FogScenario& sc, FWorld& w, uint32_t f) {
    FogOfWar& fog = w.fog;
    fog.decay_step(f);
    sc.extra(w, f);
    for (const SrcDef& s : sc.src) {
        if (f == s.die) {
            fog.source_remove(s.id);
            continue;
        }
        if (f > s.die) continue;
        const FogObject o = src_obj(s, f, sc.setup.box);
        if (s.id % 7 == 0 && f == 150) {
            fog.source_change_owner(s.id, s.owner, o);
            continue;
        }
        const bool contained = s.id % 5 == 0 && f >= 100 && f < 140;
        fog.source_service(s.id, o, contained);
        if (f % 23 == static_cast<uint32_t>(s.id) % 23) fog.source_fired(o, (o.owner + 1) % 3);
    }
}

std::vector<SrcDef> make_sources(uint32_t n, float w, float h, float sight, uint32_t seed) {
    std::vector<SrcDef> v;
    auto rnd = [&seed](uint32_t m) {
        seed = seed * 1103515245u + 12345u;
        return static_cast<float>((seed >> 8) % m);
    };
    for (uint32_t i = 0; i < n; ++i) {
        SrcDef s;
        s.id = static_cast<int32_t>(10 + i * 3);
        s.owner = static_cast<int32_t>(i % 3);
        s.cx = rnd(static_cast<uint32_t>(w));
        s.cy = rnd(static_cast<uint32_t>(h));
        s.rx = 20.0f + rnd(static_cast<uint32_t>(w / 4.0f));
        s.ry = 20.0f + rnd(static_cast<uint32_t>(h / 4.0f));
        s.w = 0.004f + rnd(100) * 0.0003f;
        s.phase = rnd(628) * 0.01f;
        s.sight = sight * (0.5f + rnd(100) * 0.01f);
        s.die = i % 6 == 0 ? 80 + static_cast<uint32_t>(rnd(250)) : 100000u;
        v.push_back(s);
    }
    return v;
}

std::vector<FogSetupObject> setup_objects(const std::vector<SrcDef>& src, uint32_t f, const FogBox& box) {
    std::vector<FogSetupObject> objs;
    for (const SrcDef& s : src) {
        FogSetupObject so;
        so.obj = src_obj(s, f, box);
        so.has_locomotor = s.id % 2 == 0;
        so.has_reveal = true;
        so.reveal_for_attacker = s.id % 11 == 0;
        so.reveal_during_setup = s.id % 13 == 0;
        so.reinforcement_prevention_radius = s.id % 17 == 0 ? 60.0f : 0.0f;
        objs.push_back(so);
    }
    return objs;
}

FogScenario land_scenario() {
    FogScenario sc;
    sc.name = "land fog";
    sc.setup.mode = FogMode::Land;
    sc.setup.box = {0.0f, 0.0f, 900.0f, 750.0f};
    sc.setup.players = {{0, true, true}, {1, true, false}, {2, true, true}, {3, false, false}};
    sc.setup.constants.setup_phase_enabled = true;
    sc.setup.constants.min_overrun_seconds = 6.0f;  // overrun possible from frame 180
    sc.src = make_sources(36, 900.0f, 750.0f, 90.0f, 777);
    sc.frames = 380;
    sc.extra = [src = sc.src, box = sc.setup.box](FWorld& w, uint32_t f) {
        FogOfWar& fog = w.fog;
        switch (f) {
            case 3: fog.begin_setup(setup_objects(src, f, box), 0, 1); break;
            case 30: fog.setup_reveal(setup_objects(src, f, box), 0, 1); break;
            case 52: fog.end_setup(setup_objects(src, f, box), 0, 1); break;
            case 60: w.lua.push_back(fog.lua_reveal_position(0, 450.0f, 380.0f, 120.0f, 60.0f)); break;
            case 61: w.lua.push_back(fog.lua_reveal_object(1, unit(0, 200.0f, 600.0f, 150.0f))); break;
            case 70: fog.reveal_all(1); break;
            case 90: fog.undo_reveal(w.lua[0]); break;
            case 120: fog.add_player_reveal_multiplier(0, 3, 77u, 1.5f); break;
            case 125: fog.add_player_reveal_modifier(0, 1, 78u, -0.25f); break;
            case 130: fog.set_battle_scale(2, 0.75f); break;
            case 180: fog.set_human(1, true); break;
            case 200: fog.undo_reveal_all(1); break;
            case 220: fog.remove_player_reveal_modifier(0, 77u); break;
            case 240: fog.undo_reveal(w.lua[1]); break;
            case 260: fog.disable_rendering(true); break;
            case 270: fog.disable_rendering(false); break;
            case 300: fog.mark_sources_dirty(); break;
            default: break;
        }
        if (f % 31 == 0) fog.temporary_reveal(static_cast<int32_t>(f % 3), unit(1, 40.0f + static_cast<float>(f), 260.0f, 0.0f), 30.0f);
        if (f >= 180) fog.service_overrun(f, 2, true, 0, 100.0f, 1, 20.0f);
    };
    return sc;
}

FogScenario space_scenario() {
    FogScenario sc;
    sc.name = "space fog";
    sc.setup.mode = FogMode::Space;
    sc.setup.box = {-3000.0f, -2500.0f, 3000.0f, 2500.0f};
    sc.setup.players = {{0, true, true}, {1, true, false}, {2, true, false}};
    sc.setup.fow_active = true;
    sc.dense = {{0.0f, 0.0f, 900.0f}, {-1800.0f, 1200.0f, 500.0f}, {2100.0f, -900.0f, 650.0f}};
    sc.src = make_sources(24, 6000.0f, 5000.0f, 900.0f, 4242);
    for (SrcDef& s : sc.src) {
        s.cx -= 3000.0f;
        s.cy -= 2500.0f;
    }
    sc.frames = 300;
    sc.extra = [](FWorld& w, uint32_t f) {
        if (f == 40) w.lua.push_back(w.fog.lua_reveal_position(1, 0.0f, 0.0f, 1200.0f, 400.0f));
        if (f == 160) w.fog.undo_reveal(w.lua[0]);
        if (f == 100) w.fog.reveal_all(2);
    };
    return sc;
}

bool same_state(const FWorld& a, const FWorld& b) {
    if (hash_of(a.fog) != hash_of(b.fog) || bytes_of(a.fog) != bytes_of(b.fog)) return false;
    if (a.refog != b.refog || a.refogs != b.refogs || a.lua.size() != b.lua.size()) return false;
    for (size_t i = 0; i < a.lua.size(); ++i)
        if (a.lua[i].player != b.lua[i].player || a.lua[i].cells != b.lua[i].cells) return false;
    return true;
}

// The host's load: build the battle's fog as at battle start, read the chunk; the Lua module restores its snapshots.
std::unique_ptr<FWorld> load_copy(const FogScenario& sc, const FWorld& a) {
    std::unique_ptr<FWorld> b = build(sc);
    const Bytes saved = bytes_of(a.fog);
    if (!load(b->fog, saved) || bytes_of(b->fog) != saved) return nullptr;  // write -> read -> write
    b->lua = a.lua;
    b->refog = a.refog;  // (the test's accumulator, not fog state)
    b->refogs = a.refogs;
    return b;
}

void check_resume(const FogScenario& sc, uint32_t save_at, bool recount_check) {
    std::unique_ptr<FWorld> a = build(sc);
    for (uint32_t f = 1; f < save_at; ++f) step(sc, *a, f);
    std::unique_ptr<FWorld> b = load_copy(sc, *a);
    CHECK(b != nullptr);
    if (!b) return;
    CHECK(same_state(*a, *b));
    const size_t saved = bytes_of(a->fog).size();
    const bool in_setup = a->fog.setup_phase();
    if (sc.setup.constants.setup_phase_enabled && save_at > 3 && save_at <= 52) CHECK(in_setup);  // saved during setup
    // The old contract (rebuild + recount the Lua snapshots) is wrong once the grids are saved: it double-counts.
    if (recount_check) {
        std::unique_ptr<FWorld> c = load_copy(sc, *a);
        bool any = false;
        for (const FogCells& cells : c->lua) {
            c->fog.recount(cells);
            any = any || !cells.cells.empty();
        }
        CHECK(any && hash_of(c->fog) != hash_of(a->fog));
    }
    uint32_t bad = 0, ticks = 0;
    const uint32_t refogs0 = a->refogs;
    for (uint32_t f = save_at; f <= sc.frames; ++f, ++ticks) {
        step(sc, *a, f);
        step(sc, *b, f);
        if (!same_state(*a, *b)) {
            bad = f;
            break;
        }
    }
    CHECK(bad == 0);
    if (bad) std::printf("  %s: resumed run from %u diverged at frame %u\n", sc.name, save_at, bad);
    std::printf("%s: saved at %u (%zu bytes, setup %d), %u frames identical, %u refogs after the load\n", sc.name, save_at,
                saved, in_setup ? 1 : 0, ticks, a->refogs - refogs0);
}

// ================================================================ rejection and corruption
void test_rejections() {
    const FogScenario sc = land_scenario();
    std::unique_ptr<FWorld> a = build(sc);
    for (uint32_t f = 1; f < 140; ++f) step(sc, *a, f);
    const Bytes good = bytes_of(a->fog);
    std::unique_ptr<FWorld> t = build(sc);
    CHECK(rejects_truncation(t->fog, good, 97));
    Bytes bad = good;
    put_u32(bad, 0, 2);
    CHECK(rejects(t->fog, bad));
    bad = good;
    bad[5] = 3;  // mode
    CHECK(rejects(t->fog, bad));
    // Space (dense mask): truncation.
    const FogScenario ssc = space_scenario();
    std::unique_ptr<FWorld> s = build(ssc);
    for (uint32_t f = 1; f < 60; ++f) step(ssc, *s, f);
    const Bytes sgood = bytes_of(s->fog);
    std::unique_ptr<FWorld> u = build(ssc);
    CHECK(rejects_truncation(u->fog, sgood, 13));
    // Random single-bit flips on both: never a crash; rejected -> unchanged; accepted -> the fog still runs.
    uint32_t rejected = 0, accepted = 0, broken = 0;
    uint32_t seed = 99;
    for (const Bytes* src : {&good, &sgood}) {
        std::unique_ptr<FWorld> x = src == &good ? build(sc) : build(ssc);
        const Bytes before = bytes_of(x->fog);
        for (uint32_t k = 0; k < 300; ++k) {
            Bytes y = *src;
            seed = seed * 1664525u + 1013904223u;
            const size_t at = (seed >> 4) % y.size();
            y[at] = static_cast<uint8_t>(y[at] ^ (1u << ((seed >> 28) & 7u)));
            ByteReader r(y);
            if (x->fog.read(r)) {
                ++accepted;
                for (uint32_t f = 200; f < 216; ++f) x->fog.decay_step(f);
                x->fog.source_service(10, unit(0, 100.0f, 100.0f, 80.0f));
                FogCells c = x->fog.lua_reveal_position(0, 300.0f, 300.0f, 90.0f, 45.0f);
                x->fog.undo_reveal(c);
                (void)hash_of(x->fog);
                if (!load(x->fog, before)) ++broken;
            } else {
                ++rejected;
                if (bytes_of(x->fog) != before) ++broken;
            }
        }
    }
    CHECK(broken == 0 && rejected > 0);
    std::printf("fog corruption: %u flips rejected, %u accepted (valid values), saves %zu / %zu bytes\n", rejected, accepted,
                good.size(), sgood.size());
}

}  // namespace

int main() {
    test_grid();
    const FogScenario land = land_scenario();
    for (uint32_t at : {1u, 2u, 4u, 20u, 31u, 53u, 75u, 97u, 141u, 151u, 233u, 300u}) check_resume(land, at, at == 75u);
    const FogScenario space = space_scenario();
    for (uint32_t at : {1u, 17u, 45u, 99u, 170u, 250u}) check_resume(space, at, at == 45u);
    test_rejections();
    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("all fog save tests passed\n");
    return 0;
}
