// alamo_render_particles_tests - R4 particle systems (no framework; non-zero exit on failure).
//   * proxy names: _ALTn / _LODn indices and the cut name (ANIM_PARTICLES.md B.3), texture directory stripping
//   * pool size (B.4) incl. the 5,003 cap and link multiplication; Particle Detail slot spread
//   * spawn timing vs sim ticks: d = 0.03 s per tick, rate / burst / initial delay / burst count / stop time / pre-warm,
//     back-filled event pre-ages, dropped spawns on a full pool, spawn-on-death / during-life links, release rules
//   * proxy attachment: stripping, alt filter, hidden mask, one-shot systems not recreated, owners released
//   * real data (ALAMO_FOC_DATA / ALAMO_EAW_DATA): every shipped particle file loads (FoC 179 files / 471 emitters,
//     EaW 409 / 1019) and the proxies of every shipped model resolve as in B.3
//   * GPU (skipped without a Vulkan 1.3 device): the synthetic systems simulated + drawn through Renderer::render's scene
//     hook (buckets 7 / 8), 0 validation messages, PNG logs/particles_synthetic.png
//   * sim (alamo_render_sim present): the synthetic sim map with particle proxies on every object, particles on vs off
//     -> identical per-tick CRC logs
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "alamo/assets/model.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/log.hpp"
#include "alamo/core/test_support.hpp"
#include "alamo/render/particles/particle_attach.hpp"
#include "alamo/render/particles/particle_data.hpp"
#include "alamo/render/particles/particle_gpu.hpp"
#include "alamo/render/particles/particle_sim.hpp"
#include "alamo/render/particles/particle_synthetic.hpp"
#include "alamo/render/png.hpp"
#include "alamo/render/renderer.hpp"
#include "alamo/rhi/rhi.hpp"
#include "alamo/rhi/test_fixture.hpp"
#include "alamo/vfs/vfs.hpp"
#if defined(ALAMO_PARTICLES_HAVE_SIM)
#include "alamo/core/sim_crc.hpp"
#include "alamo/jobs/crc_recorder.hpp"
#include "alamo/jobs/job_system.hpp"
#include "alamo/render/particles/particle_sim_attach.hpp"
#include "alamo/render/sim_session.hpp"
#include "alamo/render/sim_snapshot.hpp"
#include "alamo/render/sim_view.hpp"
#include "../../tests/test_fakes.hpp"
#endif

using namespace alamo;
using alamo::testing::Offscreen;
using alamo::testing::env_or_empty;

using namespace alamo::render::particles;
namespace fs = std::filesystem;

static SystemPtr one(EmitterDef e, const char* name = "T", bool leave = false) {
    auto s = std::make_shared<SystemDef>();
    s->name = name;
    s->leave_particles = leave;
    s->emitters.push_back(std::move(e));
    finalize_system(*s);
    return s;
}

// ------------------------------------------------------------------------------------------ names, pools
static void test_names_pools() {
    ProxyName p = parse_proxy_name("P_EXPLOSION_MED01_ALT3");
    CHECK(p.base == "P_EXPLOSION_MED01" && p.alt == 3 && p.lod == -1);
    p = parse_proxy_name("p_engine_glow_lod1_alt12");
    CHECK(p.base == "P_ENGINE_GLOW" && p.alt == 12 && p.lod == 1);
    p = parse_proxy_name("P_SMOKE");
    CHECK(p.base == "P_SMOKE" && p.alt == -1 && p.lod == -1);
    p = parse_proxy_name("P_X_ALT");  // no digits: index -1, still cut
    CHECK(p.base == "P_X" && p.alt == -1);
    CHECK(texture_base_name("C:\\PROJECTS\\Art\\p_particle_master.tga") == "P_PARTICLE_MASTER.TGA");
    CHECK(texture_base_name("p_asteroids.dds") == "P_ASTEROIDS.DDS");

    EmitterDef e = basic_emitter(100, 2.0f);
    CHECK(emitter_pool(e) == 200);           // rate x lifetime
    e.particles_per_second = 3000;
    e.lifetime = 30.0f;
    CHECK(emitter_pool(e) == kPoolCap);      // capped at 5,003
    e = basic_emitter(1, 0.25f);             // interval 1 s > lifetime: one event's worth
    CHECK(emitter_pool(e) == 1);
    e.use_bursts = true;
    e.particles_per_burst = 10;
    e.burst_delay = 0.5f;
    e.lifetime = 1.0f;
    CHECK(emitter_pool(e) == 20);            // 10 / 0.5 x 1
    e.burst_delay = 4.0f;
    CHECK(emitter_pool(e) == 10);            // interval > lifetime: one burst
    // Links: the child's pool x the parent's pool.
    auto s = std::make_shared<SystemDef>();
    EmitterDef par = basic_emitter(10, 1.0f), ch = basic_emitter(5, 1.0f);
    par.spawn_during_life = 1;
    s->emitters = {par, ch};
    finalize_system(*s);
    CHECK(s->emitters[1].parent == 0 && !s->emitters[1].parent_on_death && s->emitters[1].pool == 50);
    // Particle Detail spread: a fixed RNG-free subset of ceil(detail x pool) slots.
    CHECK(detail_active_count(100, 0.5f, 1.0f) == 50 && detail_active_count(100, 1.0f, 0.25f) == 25 && detail_active_count(7, 0.5f, 1.0f) == 4);
    uint32_t n = 0;
    for (uint32_t i = 0; i < 100; ++i) n += detail_slot_active(i, 100, 50) ? 1u : 0u;
    CHECK(n == 50);
    CHECK(ParticleWorld::tick_delta(1) == 0.03f && ParticleWorld::tick_delta(0) == 0.0f && ParticleWorld::tick_delta(1000) == 1.0f);
}

// ------------------------------------------------------------------------------------------ spawn timing
static uint64_t spawned(const ParticleWorld& w, ParticleWorld::Handle h, size_t e = 0) {
    const SystemState* s = w.system(h);
    return s ? s->emitters[e].spawned : 0;
}
static uint32_t live(const ParticleWorld& w, ParticleWorld::Handle h, size_t e = 0) {
    const SystemState* s = w.system(h);
    return s ? s->emitters[e].live : 0;
}

static void test_timing() {
    const float d = ParticleWorld::tick_delta(1);
    {   // Rate mode, 10 / s: events at 0, 0.1, 0.2, ... (the first at emitter time 0), counted per tick.
        ParticleWorld w(1u << 16);
        w.set_throttle(false);  // per-update stepping (the throttle: test_throttle)
        const auto h = w.create(one(basic_emitter(10, 1.0f)), {});
        float age = 0.0f;
        bool ok = true;
        uint64_t expect = 0;
        const double interval = static_cast<double>(1.0f / 10.0f);
        for (int t = 1; t <= 200; ++t) {
            w.update(d);
            age += d;
            expect = 0;
            while (interval * static_cast<double>(expect) <= static_cast<double>(age)) ++expect;
            ok = ok && spawned(w, h) + w.system(h)->emitters[0].dropped == expect;  // exact-size pool: a death one update late drops
        }
        CHECK(ok && expect >= 60);
        CHECK(live(w, h) == 10);  // steady state = rate x lifetime = the pool
        w.update(0.0f);           // paused: nothing happens
        CHECK(spawned(w, h) + w.system(h)->emitters[0].dropped == expect);
    }
    {   // Bursts: 5 every 0.5 s, burst count 3, initial delay 0.2 -> nothing before 0.2 s, 15 in total, then the system
        // dies once its particles are dead.
        EmitterDef e = basic_emitter(0, 0.4f);
        e.use_bursts = true;
        e.particles_per_burst = 5;
        e.burst_delay = 0.5f;
        e.burst_count = 3;
        e.initial_delay = 0.2f;
        ParticleWorld w(1u << 16);
        w.set_throttle(false);  // per-update stepping (the throttle: test_throttle)
        const auto h = w.create(one(e), {});
        for (int t = 0; t < 6; ++t) w.update(d);  // 0.18 s
        CHECK(spawned(w, h) == 0);
        w.update(d);  // 0.21 s
        CHECK(spawned(w, h) == 5 && live(w, h) == 5);
        for (int t = 0; t < 60; ++t) w.update(d);
        CHECK(!w.valid(h));  // three bursts, all dead: killed itself
        CHECK(w.stats().spawned == 15);
    }
    {   // Back-filling: one 0.55 s update of a 10 / s emitter spawns 6 particles pre-aged 0.55, 0.45, ... 0.05.
        ParticleWorld w(1u << 16);
        w.set_throttle(false);  // per-update stepping (the throttle: test_throttle)
        w.set_record_packets(true);
        const auto h = w.create(one(basic_emitter(10, 1.0f)), {});
        w.update(0.55f);
        const std::vector<UpdatePacket> pk = w.take_packets();
        CHECK(spawned(w, h) == 6);
        bool ok = !pk.empty() && pk.back().spawns.size() == 6;
        if (ok) {
            const std::vector<SpawnRecord>& sp = pk.back().spawns;
            for (size_t i = 0; i < sp.size(); ++i) ok = ok && std::fabs(sp[i].pre_age - (0.55f - 0.1f * static_cast<float>(i))) < 1e-5f;
            ok = ok && sp.front().lerp == 0.0f && sp.back().lerp > 0.9f;  // interpolated emitter transform
        }
        CHECK(ok);
        CHECK(pk.front().clears.size() == 1 && pk.front().clears[0].count == 10);  // the new range is cleared first
    }
    {   // Stop time 0.25 s: events at 0, 0.1, 0.2 only.
        EmitterDef e = basic_emitter(10, 1.0f);
        e.stop_time = 0.25f;
        ParticleWorld w(1u << 16);
        w.set_throttle(false);  // per-update stepping (the throttle: test_throttle)
        const auto h = w.create(one(e), {});
        for (int t = 0; t < 20; ++t) w.update(d);
        CHECK(spawned(w, h) == 3);
    }
    {   // Pre-warm (skip 1 s): the first update steps 0.1 s until age > 1 (11 steps) + d; events older than the
        // lifetime (0.5 s) are skipped, so only the last ones exist.
        EmitterDef e = basic_emitter(10, 0.5f);
        e.skip_time = 1.0f;
        ParticleWorld w(1u << 16);
        w.set_throttle(false);  // per-update stepping (the throttle: test_throttle)
        const auto h = w.create(one(e), {});
        w.update(d);
        CHECK(live(w, h) == 5 || live(w, h) == 6);
        CHECK(spawned(w, h) == live(w, h));
    }
    {   // Pool cap and dropped spawns: 3000 / s x 30 s -> 5,003 slots; a full pool drops.
        EmitterDef e = basic_emitter(3000, 30.0f);
        ParticleWorld w(1u << 16);
        w.set_throttle(false);  // per-update stepping (the throttle: test_throttle)
        const auto h = w.create(one(e), {});
        for (int t = 0; t < 70; ++t) w.update(d);
        const SystemState* s = w.system(h);
        CHECK(s && s->emitters[0].pool == kPoolCap && s->emitters[0].live == kPoolCap && s->emitters[0].dropped > 0);
    }
    {   // Particle Detail 0.5: only half the slots take particles.
        ParticleWorld w(1u << 16);
        w.set_throttle(false);  // per-update stepping (the throttle: test_throttle)
        w.set_detail(0.5f);
        const auto h = w.create(one(basic_emitter(100, 1.0f)), {});
        for (int t = 0; t < 100; ++t) w.update(d);
        CHECK(live(w, h) == 50);
        w.set_detail(1.0f);
        for (int t = 0; t < 100; ++t) w.update(d);
        CHECK(live(w, h) >= 99);
    }
    {   // Links: 8 shells (0.6 s) -> each death starts one 12-particle POP burst; each live shell runs a TRAIL.
        const std::vector<SystemPtr> syn = synthetic_systems();
        SystemPtr chain;
        for (const SystemPtr& s : syn)
            if (s->name == "SYN_CHAIN") chain = s;
        CHECK(chain && chain->emitters[1].parent == 0 && chain->emitters[1].parent_on_death && chain->emitters[2].parent == 0);
        ParticleWorld w(1u << 18);
        w.set_throttle(false);  // per-update stepping (the throttle: test_throttle)
        const auto h = w.create(chain, {});
        for (int t = 0; t < 30; ++t) w.update(d);  // 0.9 s: shells died at 0.6 s
        CHECK(spawned(w, h, 0) == 8 && spawned(w, h, 1) == 96 && spawned(w, h, 2) > 8);
        // Leave particles: released systems stop spawning and live out their particles.
        w.release(h);
        CHECK(w.valid(h));
        const uint64_t before = spawned(w, h, 0);
        for (int t = 0; t < 60; ++t) w.update(d);
        CHECK(!w.valid(h) && before == 8);
    }
    {   // No leave-particles flag: released = removed at once; its range is reusable.
        ParticleWorld w(64);
        w.set_throttle(false);  // per-update stepping (the throttle: test_throttle)
        const auto a = w.create(one(basic_emitter(10, 3.0f)), {});  // pool 30
        const auto b = w.create(one(basic_emitter(10, 3.0f)), {});  // pool 30
        const auto c = w.create(one(basic_emitter(10, 3.0f)), {});  // no room: 4 left
        CHECK(w.system(c)->emitters[0].base == UINT32_MAX && w.stats().pool_exhausted == 1);
        w.release(a);
        CHECK(!w.valid(a) && w.valid(b));
        const auto e2 = w.create(one(basic_emitter(10, 3.0f)), {});
        CHECK(w.system(e2)->emitters[0].base == 0);
    }
}

// ------------------------------------------------------------------------------------------ R4 throttle / scalars
// STRATEGIC_GAPS §6.2: the throttle global is statically 1. An undrawn system (updated before) gathers d and steps once
// >= 0.1 s gathered (4 ticks of 0.03 s: 0.12 s), with that d; a drawn one steps every update. The scalar setter springs
// the emission / detail / colour values toward their targets (s = 0 snaps).
static void test_throttle() {
    const float d = ParticleWorld::tick_delta(1);
    ParticleWorld w(1u << 16);
    CHECK(w.throttle());  // on by default (the original's initialised data)
    w.set_record_packets(true);
    const auto hidden = w.create(one(basic_emitter(10, 1.0f)), {});
    const auto drawn = w.create(one(basic_emitter(10, 1.0f)), {});
    const uint32_t drawn_index = static_cast<uint32_t>(drawn & 0xFFFFFFFFu);
    uint32_t hidden_steps = 0, drawn_steps = 0;
    float hidden_d = 0.0f, drawn_d = 0.0f;
    for (int t = 0; t < 12; ++t) {
        w.mark_drawn(drawn_index);  // the renderer submitted it last frame
        w.update(d);
        for (const UpdatePacket& p : w.take_packets())
            for (const PacketEmitter& e : p.emitters) {
                const bool is_hidden = e.base == w.system(hidden)->emitters[0].base;
                (is_hidden ? hidden_steps : drawn_steps) += 1;
                (is_hidden ? hidden_d : drawn_d) += e.d;
            }
    }
    // Hidden: the first update (never updated before), then every 4th tick (0.12 s gathered): ticks 1, 5, 9.
    CHECK(drawn_steps == 12 && hidden_steps == 3);
    CHECK(std::fabs(drawn_d - 12 * d) < 1e-5f && std::fabs(hidden_d - 9 * d) < 1e-5f);
    // Event back-filling keeps the spawn count: the hidden system spawned what it would have by its last step.
    CHECK(w.stats().throttled == 9 && w.stats().system_steps == 15);
    // Off (tests / tools): every system steps every update.
    w.set_throttle(false);
    w.update(d);
    uint32_t n = 0;
    for (const UpdatePacket& p : w.take_packets()) n += static_cast<uint32_t>(p.emitters.size());
    CHECK(n == 2);
    // Scalars: emission snapped to 0 -> nothing spawns; smoothed colour moves toward the target over updates.
    ParticleWorld v(1u << 16);
    v.set_throttle(false);
    const auto h = v.create(one(basic_emitter(10, 1.0f)), {});
    const float zero = 0.0f;
    v.set_scalar(h, SystemScalar::Emission, std::span<const float>(&zero, 1), 0.0f);
    for (int t = 0; t < 10; ++t) v.update(d);
    CHECK(v.system(h)->emitters[0].spawned == 0 && v.system(h)->emission_scale == 0.0f);
    const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    v.set_scalar(h, SystemScalar::Color, red, 0.5f);
    v.update(d);
    const float g1 = v.system(h)->color[1];
    for (int t = 0; t < 400; ++t) v.update(d);
    CHECK(g1 < 1.0f && g1 > 0.9f && v.system(h)->color[1] == 0.0f && v.system(h)->color[0] == 1.0f);
    // Detail 0.5 snapped: half the slots active (re-evaluated at once).
    const float half = 0.5f;
    v.set_scalar(h, SystemScalar::Detail, std::span<const float>(&half, 1), 0.0f);
    CHECK(v.system(h)->emitters[0].active == 5);
}

// ------------------------------------------------------------------------------------------ attachment
static render::GpuModel fake_model(std::shared_ptr<assets::ModelAsset>& asset, const std::vector<std::pair<std::string, bool>>& proxies) {
    asset = std::make_shared<assets::ModelAsset>();
    asset->name = "FAKE";
    asset->skeleton.names = {"ROOT", "B1"};
    asset->skeleton.parents = {-1, 0};
    asset->skeleton.visible = {1, 1};
    asset->skeleton.billboard_mode = {0, 0};
    assets::Mat3x4 b1;
    b1.m[0][3] = 10.0f;
    asset->skeleton.bind_local = {assets::Mat3x4{}, b1};
    asset->skeleton.finalize();
    assets::MeshInfo mi;
    mi.name = "HULL";
    asset->meshes.push_back(mi);
    for (const auto& [n, hidden] : proxies) asset->proxies.push_back({n, 1, hidden});
    render::GpuModel m;
    m.uid = 77;
    m.asset = asset;
    return m;
}

static void test_attach() {
    ParticleLibrary lib;
    for (const SystemPtr& s : synthetic_systems()) lib.add(s);
    ParticleWorld w(1u << 18);
    w.set_throttle(false);  // per-update stepping (the throttle: test_throttle)
    ProxyAttachments at(w, lib);
    std::shared_ptr<assets::ModelAsset> asset;
    render::GpuModel m = fake_model(asset, {{"SYN_FIRE", false}, {"SYN_SMOKE_ALT1", false}, {"SYN_SPARKS_ALT0", false},
                                            {"LENSFLARE0", false}, {"SYN_HEAT", true}});
    render::Instance in;
    in.model = &m;
    in.transform.m[0][3] = 100.0f;
    at.begin();
    at.sync(5, in);
    at.end();
    CHECK(at.stats().particle_proxies == 4 && at.stats().unresolved == 1 && at.stats().stripped == 2);
    CHECK(at.stats().hidden == 2);  // SYN_SMOKE_ALT1 (alt 0 current) and SYN_HEAT (ALO hidden flag)
    uint32_t visible = 0, systems = 0;
    float x = 0;
    w.for_each([&](uint32_t, const SystemState& s) {
        ++systems;
        visible += s.visible ? 1u : 0u;
        x = s.cur.translation().x;
    });
    CHECK(systems == 4 && visible == 2 && x == 110.0f);  // owner x bone B1 (+10)
    // Hardpoint-style mask over meshes + proxies: proxy 0 hidden by its sub-object bit.
    const std::vector<uint8_t> mask = {0, 1, 0, 0, 0, 0};
    in.mesh_hidden = mask;
    at.begin();
    at.sync(5, in);
    at.end();
    CHECK(at.stats().hidden == 2);  // proxy 0 now hidden, proxy 4 (index 5) visible by the mask
    // The owner leaves: its systems are released (SYN_* have no leave-particles flag: removed).
    at.begin();
    at.end();
    systems = 0;
    w.for_each([&](uint32_t, const SystemState&) { ++systems; });
    CHECK(at.owners() == 0 && systems == 0);
    // One-shot systems are not recreated while the owner stays.
    EmitterDef b = basic_emitter(0, 0.1f);
    b.use_bursts = true;
    b.particles_per_burst = 3;
    b.burst_delay = 1.0f;
    b.burst_count = 1;
    lib.add(one(b, "ONE_SHOT"));
    std::shared_ptr<assets::ModelAsset> a2;
    render::GpuModel m2 = fake_model(a2, {{"ONE_SHOT", false}});
    m2.uid = 78;
    in.model = &m2;
    in.mesh_hidden = {};
    for (int f = 0; f < 20; ++f) {
        at.begin();
        at.sync(9, in);
        at.end();
        w.update(0.03f);
    }
    CHECK(at.stats().finished == 1 && w.stats().spawned == 3);
    // A particle-file owner (sim PARTICLE object): one system per key, moved with the owner, released when it leaves.
    at.clear();
    assets::Mat3x4 t;
    for (int f = 0; f < 3; ++f) {
        t.m[2][3] = static_cast<float>(f);
        at.begin();
        at.sync_effect(1234, lib.get("syn_smoke"), t, 0.5f);
        at.end();
    }
    systems = 0;
    float z = -1.0f, fade = 0.0f;
    w.for_each([&](uint32_t, const SystemState& s) {
        ++systems;
        z = s.cur.translation().z;
        fade = s.fade;
    });
    CHECK(systems == 1 && z == 2.0f && fade == 0.5f && at.stats().systems == 1);
    // Galactic corruption scalars (GRC 4.5): snapped colour RGBA / emission / detail; unknown keys ignored.
    at.set_effect_scalars(1234, 0.65f, 0.5f, 0.75f);
    at.set_effect_scalars(999, 0.1f, 0.1f, 0.1f);
    float col[4] = {0, 0, 0, 0}, emis = 0.0f, det = 0.0f;
    w.for_each([&](uint32_t, const SystemState& s) {
        std::copy(std::begin(s.color), std::end(s.color), col);
        emis = s.emission_scale;
        det = s.detail_scale;
    });
    CHECK(col[0] == 0.65f && col[3] == 0.65f && emis == 0.5f && det == 0.75f);
    at.begin();
    at.end();
    systems = 0;
    w.for_each([&](uint32_t, const SystemState&) { ++systems; });
    CHECK(systems == 0);
}

// ------------------------------------------------------------------------------------------ real data
static void scan_corpus(const char* what, const std::string& root, bool eaw, uint32_t want_files, uint32_t want_emitters) {
    vfs::VfsConfig cfg;
    cfg.foc_root = root;
    cfg.eaw_root = std::string();  // this corpus only
    std::string err;
    auto v = vfs::Vfs::create(cfg, &err);
    CHECK(v != nullptr);
    if (!v) return;
    const std::vector<std::string> alos = v->list("Data\\Art\\Models\\*.alo");
    uint32_t files = 0, emitters = 0, failed = 0, heat = 0, sorted = 0, linked = 0, capped = 0, maxpool = 0;
    uint32_t blend[16] = {};
    ParticleLibrary lib(v.get());
    std::vector<std::string> models;
    for (const std::string& a : alos) {
        std::vector<uint8_t> b;
        if (!v->read(a, b)) continue;
        if (!is_particle_file(b)) {
            models.push_back(a);
            continue;
        }
        SystemDef s;
        if (!load_system(b, a, s, &err)) {
            ++failed;
            std::printf("  %s: %s\n", a.c_str(), err.c_str());
            continue;
        }
        ++files;
        for (const EmitterDef& e : s.emitters) {
            ++emitters;
            ++blend[std::min<uint32_t>(e.blend_mode, 15)];
            heat += e.heat ? 1u : 0u;
            sorted += e.sorted() ? 1u : 0u;
            linked += e.parent >= 0 ? 1u : 0u;
            capped += e.pool == kPoolCap ? 1u : 0u;
            maxpool = std::max(maxpool, e.pool);
            CHECK(e.color_texture.find('\\') == std::string::npos);
        }
    }
    std::printf("%s: %u particle files, %u emitters (%u failed); blend 1:%u 2:%u 11:%u, heat %u, sorted %u, linked %u, "
                "pool max %u (%u capped)\n",
                what, files, emitters, failed, blend[1], blend[2], blend[11], heat, sorted, linked, maxpool, capped);
    CHECK(files == want_files && emitters == want_emitters && failed == 0);
    // Proxies of every model (B.3): exact name, after _ALT / _LOD stripping, or nothing.
    uint32_t proxies = 0, exact = 0, stripped = 0, none = 0;
    for (const std::string& mp : models) {
        std::vector<uint8_t> b;
        assets::ModelAsset m;
        if (!v->read(mp, b) || !assets::build_model(b, mp, m)) continue;
        for (const assets::Proxy& p : m.proxies) {
            ++proxies;
            std::string up = p.name;
            for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (lib.get(up)) ++exact;
            else if (lib.get(parse_proxy_name(p.name).base)) ++stripped;
            else ++none;
        }
    }
    std::printf("%s: %u proxies in %zu models: %u name a particle file, %u only after stripping _ALT/_LOD, %u nothing\n", what,
                proxies, models.size(), exact, stripped, none);
    CHECK(proxies > 0 && stripped > 0);
    (void)eaw;
}

static void test_real_data() {
    const std::string foc = env_or_empty("ALAMO_FOC_DATA"), eaw = env_or_empty("ALAMO_EAW_DATA");
    if (foc.empty() && eaw.empty()) {
        std::printf("real data: ALAMO_FOC_DATA / ALAMO_EAW_DATA not set, skipped\n");
        return;
    }
    if (!foc.empty()) scan_corpus("FoC", foc, false, 179, 471);
    if (!eaw.empty()) scan_corpus("EaW", eaw, true, 409, 1019);
    if (foc.empty()) return;
    // The game's view (FoC with the EaW fallback mounted): every model the VFS lists, proxies resolved as at load.
    vfs::VfsConfig cfg;
    cfg.foc_root = foc;
    std::string err;
    auto v = vfs::Vfs::create(cfg, &err);
    CHECK(v != nullptr);
    if (!v) return;
    ParticleLibrary lib(v.get());
    uint32_t models = 0, proxies = 0, exact = 0, stripped = 0, none = 0;
    for (const std::string& a : v->list("Data\\Art\\Models\\*.alo")) {
        std::vector<uint8_t> b;
        assets::ModelAsset m;
        if (!v->read(a, b) || is_particle_file(b) || !assets::build_model(b, a, m)) continue;
        ++models;
        for (const assets::Proxy& p : m.proxies) {
            ++proxies;
            if (lib.get(p.name)) ++exact;
            else if (lib.get(parse_proxy_name(p.name).base)) ++stripped;
            else ++none;
        }
    }
    const ParticleLibrary::Stats ls = lib.stats();
    std::printf("FoC+EaW: %u proxies in %u models: %u name a particle file, %u only after stripping, %u nothing "
                "(library: %u loaded, %u missing, %u not particle)\n",
                proxies, models, exact, stripped, none, ls.loaded, ls.missing, ls.not_particle);
    // ANIM_PARTICLES.md B.3: 5,685 proxies, 2,206 resolve only after stripping (3,345 exact / 134 nothing there; this
    // scan finds 3,347 / 132: two names resolve as written here).
    CHECK(proxies == 5685 && stripped == 2206 && exact + none == 3479);
}

// ------------------------------------------------------------------------------------------ GPU
struct Gpu : alamo::testing::Gpu {
    render::Renderer renderer;
};

static bool frame(Gpu& g, Offscreen& o, const render::ViewMatrices& vm, std::span<const render::Instance> inst, render::RecordOptions ro) {
    rhi::Frame* f = g.ring.begin_frame();
    if (!f) return false;
    VkCommandBuffer cmd = f->cmd();
    const rhi::DeviceFns& fn = g.ctx.vkd();
    const rhi::AcquireResult acq = g.up.record_acquires(cmd);
    render::PassTarget pt;
    pt.color = o.color.handle;
    pt.color_view = o.color.view;
    pt.extent = {o.w, o.h};
    pt.clear[0] = 0.02f;
    pt.clear[1] = 0.02f;
    pt.clear[2] = 0.06f;
    pt.color_after = rhi::ImageState::TransferSrc;
    render::DrawStats st;
    const bool drawn = g.renderer.render(*f, cmd, pt, vm, inst, render::DrawMode::Unlit, ro, &st);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {o.w, o.h, 1};
    fn.vkCmdCopyImageToBuffer(cmd, o.color.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, o.readback.handle, 1, &copy);
    const VkBufferMemoryBarrier2 hb = rhi::buffer_barrier(o.readback.handle, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                                          VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    rhi::cmd_barrier(fn, cmd, {}, std::span<const VkBufferMemoryBarrier2>(&hb, 1));
    VkSemaphoreSubmitInfo wait = rhi::semaphore_info(g.up.timeline(), acq.wait_value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    rhi::FrameSubmit fs;
    if (acq.wait_value) fs.waits = {&wait, 1};
    if (!g.ring.end_frame(*f, fs)) return false;
    g.ring.wait_idle();
    g.alloc.invalidate(o.readback.alloc);
    return drawn;
}

static void test_gpu(const std::string& src_dir) {
    const bool strict = render::setup_validation_env(src_dir);
    {
        rhi::Loader probe;
        if (!probe.open()) {
            std::printf("SKIP gpu: no Vulkan loader\n");
            return;
        }
    }
    Gpu g;
    rhi::ContextDesc cd;
    cd.app_name = "alamo_render_particles_tests";
    cd.validation = strict ? rhi::Validation::Required : rhi::Validation::IfAvailable;
    if (!g.ctx.init(cd)) {
        CHECK(!strict);
        std::printf("SKIP gpu: no suitable Vulkan 1.3 device\n");
        return;
    }
    std::printf("gpu: %s, validation %s\n", g.ctx.caps().name.c_str(), g.ctx.validation_enabled() ? "on" : "off");
    rhi::FrameRingDesc fd;
    fd.frames_in_flight = 2;
    fd.uniform_bytes = 16ull << 20;
    CHECK(g.alloc.init(g.ctx) && g.up.init(g.ctx, g.alloc) && g.ring.init(g.ctx, g.alloc, fd));
    render::RendererDesc rd;
    rd.color_format = VK_FORMAT_R8G8B8A8_UNORM;
    rd.translated = false;
    const bool rinit = g.renderer.init(g.ctx, g.alloc, g.up, rd);
    CHECK(rinit);
    g.renderer.set_frame_ring(&g.ring);
    ParticleWorld world(1u << 18);
    world.set_throttle(false);  // per-update stepping (the throttle: test_throttle)
    ParticleRenderer pr;
    const bool pinit = rinit && pr.init(g.ctx, g.alloc, g.up, world, nullptr);
    CHECK(pinit);
    Offscreen o;
    CHECK(o.create(g, 640, 360, false));
    if (pinit) {
        g.renderer.set_scene_hook(&pr);
        const std::vector<SystemPtr> syn = synthetic_systems();
        std::vector<ParticleWorld::Handle> hs;
        for (size_t i = 0; i < syn.size(); ++i) {
            assets::Mat3x4 t;
            t.m[0][3] = (static_cast<float>(i) - 2.0f) * 45.0f;
            hs.push_back(world.create(syn[i], t));
        }
        world.set_water(hs[1], true);  // smoke in bucket 7
        render::OrbitCamera cam;
        cam.target = {0, 0, 15};
        cam.yaw = 1.5707963f;
        cam.pitch = 0.25f;
        cam.distance = 230.0f;
        const render::ViewMatrices vm = render::make_view(cam, o.w, o.h, 1.0f, 5000.0f);
        render::RecordOptions ro;
        ro.post = false;
        ro.shadows = false;
        bool ok = true;
        uint32_t max_draws = 0, sorted = 0, water = 0;
        for (int f = 0; f < 45; ++f) {
            // Systems move: the fire orbits (interpolated spawns, inherit velocity path).
            assets::Mat3x4 t;
            t.m[0][3] = -90.0f + 10.0f * std::sin(0.2f * static_cast<float>(f));
            t.m[1][3] = 10.0f * std::cos(0.2f * static_cast<float>(f));
            world.set_transform(hs[0], t);
            world.update(ParticleWorld::tick_delta(1));
            ok = frame(g, o, vm, {}, ro) && ok;
            max_draws = std::max(max_draws, pr.last_stats().draw_calls);
            sorted = std::max(sorted, pr.last_stats().sorted_emitters);
            water = std::max(water, pr.last_stats().water_draws);
        }
        const ParticleFrameStats& ps = pr.last_stats();
        std::printf("gpu: %u systems, %u drawn, %u draw calls (%u water, %u transparent), %u sorted emitters, %u live particles\n",
                    ps.systems, ps.systems_drawn, ps.draw_calls, ps.water_draws, ps.transparent_draws, ps.sorted_emitters,
                    world.stats().live_particles);
        CHECK(ok && max_draws >= 5 && water >= 1 && (sorted >= 1 || !pr.sort_supported()));
        const auto* px = static_cast<const uint8_t*>(o.readback.mapped());
        uint32_t lit = 0;
        for (size_t i = 0; i < static_cast<size_t>(o.w) * o.h; ++i)
            if (px[i * 4] > 60 || px[i * 4 + 1] > 60) ++lit;
        std::printf("gpu: %u particle pixels\n", lit);
        CHECK(lit > 2000);
        std::vector<uint8_t> rgba(px, px + static_cast<size_t>(o.w) * o.h * 4);
        for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
        const std::string png = src_dir + "/logs/particles_synthetic.png";
        CHECK(render::write_png_rgba(png, o.w, o.h, rgba));
        std::printf("wrote %s\n", png.c_str());
        // Hidden systems keep updating but draw nothing.
        world.for_each([&](uint32_t, const SystemState&) {});
        for (auto h : hs) world.set_visible(h, false);
        world.update(0.03f);
        CHECK(frame(g, o, vm, {}, ro) && pr.last_stats().draw_calls == 0 && pr.last_stats().systems_hidden >= 1);
        g.renderer.set_scene_hook(nullptr);
    }
    g.ring.wait_idle();
    pr.shutdown();
    o.destroy(g);
    g.renderer.shutdown();
    g.ring.shutdown();
    g.up.shutdown();
    g.alloc.shutdown();
    const uint32_t ve = g.ctx.validation_errors(), vw = g.ctx.validation_warnings();
    std::printf("gpu: validation %u errors, %u warnings\n", ve, vw);
    CHECK(ve == 0 && vw == 0);
    g.ctx.shutdown();
}

// ------------------------------------------------------------------------------------------ sim CRC
#if defined(ALAMO_PARTICLES_HAVE_SIM)
struct ProxyBoxModels final : render::SimModelSource {
    const render::GpuModel* model = nullptr;
    const render::SnapshotTypeTable* table = nullptr;
    const render::GpuModel* type_model(objects::TypeIndex t) override {
        return table && table->valid(t) && !(*table)[t].model.empty() ? model : nullptr;
    }
    const render::GpuModel* named_model(const std::string&) override { return nullptr; }
};

static void test_sim() {
    std::string err;
    const fs::path dir = fs::temp_directory_path() / "alamo_render_particles_tests";
    std::error_code ec;
    remove_all_long(dir, ec);
    const auto w = render::make_synthetic_sim_world(path_to_utf8(dir), &err);
    CHECK(w != nullptr);
    if (!w) return;
    render::SnapshotTypeTable table;
    table.build(w->types, objects::ModeKind::Space);
    // The debug box with particle proxies (engine, damage smoke with an alt index, an unresolved name).
    auto box = render::debug_box_model(render::kSyntheticModel);
    box->proxies = {{"SYN_FIRE", 0, false}, {"SYN_SMOKE_ALT0", 0, false}, {"P_MISSING_EFFECT", 0, false}};
    render::GpuModel model;
    model.uid = 4242;
    model.asset = box;
    ProxyBoxModels models;
    models.model = &model;
    models.table = &table;
    auto run = [&](bool particles, uint64_t ticks, WorldStats* out) {
        jobs::JobSystem js(jobs::JobSystemConfig{-1});
        render::SimSessionSpec ss;
        ss.seed = 11;
        render::SimSession s(w->types, js, w->map, *w->index, ss);
        render::SnapshotBuffer buf;
        render::InterpolatedFrame frame;
        render::SimInstanceBuilder builder(table);
        std::vector<render::Instance> fg, bg;
        ParticleLibrary lib;
        for (const SystemPtr& p : synthetic_systems()) lib.add(p);
        ParticleWorld pw(1u << 20);
        pw.set_throttle(false);  // per-update stepping (the throttle: test_throttle)
        pw.set_record_packets(true);
        ProxyAttachments at(pw, lib);
        for (uint64_t t = 0; t < ticks && s.tick(); ++t) {
            if (!particles) continue;
            auto snap = std::make_shared<render::SimSnapshot>();
            render::capture_snapshot(s.host(), table, *snap, jobs::make_parallel_for(js));
            buf.publish(std::move(snap));
            const render::SnapshotBuffer::Pair p = buf.latest();
            render::interpolate(p.prev.get(), *p.curr, 1.0f, render::InterpMode::Smooth, frame);
            builder.build(frame, models, fg, bg);
            sync_sim_frame(at, frame, table, models, builder);
            pw.update(ParticleWorld::tick_delta(1));
            (void)pw.take_packets();
        }
        if (out) *out = pw.stats();
        return s.host().crc_log();
    };
    const CrcLog off = run(false, 300, nullptr);
    WorldStats ps;
    const CrcLog on = run(true, 300, &ps);
    const CrcDivergence dv = first_divergence(off, on);
    std::printf("sim: particles off vs on (300 ticks): %zu vs %zu records: %s; %u systems, %u live particles, %llu spawned\n",
                off.records.size(), on.records.size(), dv.message.c_str(), ps.systems, ps.live_particles,
                static_cast<unsigned long long>(ps.spawned));
    CHECK(!dv.diverged && off.records.size() == on.records.size() && !off.records.empty());
    CHECK(ps.systems > 10 && ps.spawned > 1000);
}

// Int-B: sim-attached effects (InterpolatedFrame::effects, damaged smoke) become systems per (object, percent) and are
// released with their object.
static void test_sim_effects() {
    std::string err;
    const fs::path dir = fs::temp_directory_path() / "alamo_render_particles_tests_fx";
    std::error_code ec;
    remove_all_long(dir, ec);
    const auto w = render::make_synthetic_sim_world(path_to_utf8(dir), &err);
    CHECK(w != nullptr);
    if (!w) return;
    render::SnapshotTypeTable table;
    table.build(w->types, objects::ModeKind::Space);
    const objects::TypeIndex frig = w->types.resolve_name("Sim_Frigate");
    auto box = render::debug_box_model(render::kSyntheticModel);  // no proxies: only the attached effects make systems
    render::GpuModel model;
    model.uid = 4343;
    model.asset = box;
    ProxyBoxModels models;
    models.model = &model;
    models.table = &table;
    render::SimInstanceBuilder builder(table);
    ParticleLibrary lib;
    for (const SystemPtr& p : synthetic_systems()) lib.add(p);
    ParticleWorld pw(1u << 18);
    pw.set_throttle(false);  // per-update stepping (the throttle: test_throttle)
    ProxyAttachments at(pw, lib);
    render::InterpolatedFrame f;
    for (objects::ObjectId id : {10, 11}) {
        render::SnapshotObject o;
        o.id = id;
        o.type = frig;
        o.position = {static_cast<float>(id) * 100.0f, 0, 0};
        f.objects.push_back(o);
    }
    f.effects = {{10, "SYN_SMOKE", 75, 1}, {10, "SYN_SMOKE", 50, 2}, {11, "P_NOT_A_SYSTEM", 75, 3}, {12, "SYN_FIRE", 75, 4}};
    std::vector<render::Instance> fg, bg;
    builder.build(f, models, fg, bg);
    SimAttachStats st = sync_sim_frame(at, f, table, models, builder);
    pw.update(ParticleWorld::tick_delta(1));
    std::printf("sim effects: %u attached, %u missing, %u systems\n", st.attached, st.attached_missing, pw.stats().systems);
    CHECK(st.attached == 2 && st.attached_missing == 1 && st.synced == 2 && pw.stats().systems == 2);
    // Object 10 leaves: its two effect systems are released.
    f.objects.erase(f.objects.begin());
    builder.build(f, models, fg, bg);
    st = sync_sim_frame(at, f, table, models, builder);
    CHECK(st.attached == 0 && at.stats().released == 2);
    // R5 world effects: one at a world point, one following object 11, one whose owner is gone, one unknown asset.
    std::vector<render::SimWorldEffect> wfx(4);
    wfx[0].key = 1;
    wfx[0].asset = "SYN_SPARKS";
    wfx[0].position = {5, 6, 7};
    wfx[1].key = 2;
    wfx[1].asset = "SYN_FIRE";
    wfx[1].owner = 11;
    wfx[2].key = 3;
    wfx[2].asset = "SYN_FIRE";
    wfx[2].owner = 99;
    wfx[3].key = 4;
    wfx[3].asset = "P_NOT_A_SYSTEM";
    builder.build(f, models, fg, bg);
    st = sync_sim_frame(at, f, table, models, builder, nullptr, nullptr, wfx);
    CHECK(st.world == 2 && st.world_missing == 1);
    // R3: a contained object (garrison) gets no systems; its world effect is skipped too.
    f.objects[0].flags = static_cast<uint8_t>(f.objects[0].flags | render::kSnapContained);
    builder.build(f, models, fg, bg);
    st = sync_sim_frame(at, f, table, models, builder, nullptr, nullptr, wfx);
    CHECK(st.contained == 1 && st.world == 1 && st.synced == 0 && fg.empty());
    // Listed no more: released.
    st = sync_sim_frame(at, f, table, models, builder);
    CHECK(st.world == 0 && at.stats().released >= 1);
    std::printf("sim world effects: OK\n");
}

// OI render / Int-final: Lua Attach_Particle_Effect requests (lua::ParticleEffectRequest-shaped records) -> snapshot_lua_effects
// -> SimSnapshot -> interpolate -> sync_sim_frame: one system per request, at the named bone, kept while the request and the
// object stay listed, released with the object. Render-side only (the sim CRC with particles on / off: test_sim).
using alamo::testing::FakeLuaRequest;

static void test_sim_lua_effects() {
    std::string err;
    const fs::path dir = fs::temp_directory_path() / "alamo_render_particles_tests_lua";
    std::error_code ec;
    remove_all_long(dir, ec);
    const auto w = render::make_synthetic_sim_world(path_to_utf8(dir), &err);
    CHECK(w != nullptr);
    if (!w) return;
    render::SnapshotTypeTable table;
    table.build(w->types, objects::ModeKind::Space);
    const objects::TypeIndex frig = w->types.resolve_name("Sim_Frigate");
    std::shared_ptr<assets::ModelAsset> asset;
    render::GpuModel model = fake_model(asset, {});  // bones ROOT, B1 (+10 on x), no proxies: only the Lua effects make systems
    ProxyBoxModels models;
    models.model = &model;
    models.table = &table;
    render::SimInstanceBuilder builder(table);
    ParticleLibrary lib;
    for (const SystemPtr& p : synthetic_systems()) lib.add(p);
    ParticleWorld pw(1u << 18);
    pw.set_throttle(false);
    ProxyAttachments at(pw, lib);
    std::vector<render::Instance> fg, bg;
    auto make_snapshot = [&](std::initializer_list<objects::ObjectId> ids, const std::vector<FakeLuaRequest>& reqs) {
        render::SimSnapshot s;
        for (objects::ObjectId id : ids) {
            render::SnapshotObject o;
            o.id = id;
            o.type = frig;
            o.position = {static_cast<float>(id) * 100.0f, 0, 0};
            s.objects.push_back(o);
        }
        render::append_snapshot_effects(s, render::snapshot_lua_effects(reqs, table, 2));  // since 2: frame 1 belongs to another mode
        render::InterpolatedFrame f;
        render::interpolate(nullptr, s, 1.0f, render::InterpMode::Smooth, f);
        return f;
    };
    auto live_systems = [&]() {
        uint32_t n = 0;
        pw.for_each([&](uint32_t, const SystemState&) { ++n; });
        return n;
    };
    auto sync = [&](const render::InterpolatedFrame& f) {
        builder.build(f, models, fg, bg);
        return sync_sim_frame(at, f, table, models, builder);
    };
    std::vector<FakeLuaRequest> reqs = {{1, 0, 10, -1, "SYN_SMOKE", ""},      // before the battle: dropped
                                        {5, 1, 10, -1, "SYN_SMOKE", ""},      // object 10, root
                                        {6, 2, 10, -1, "SYN_FIRE", "B1"},     // object 10, bone B1
                                        {7, 3, 11, -1, "SYN_SMOKE", "NOPE"},  // unknown bone: the root
                                        {8, 4, 12, -1, "SYN_SMOKE", ""},      // object 12 is not in the snapshot
                                        {9, 5, 10, -1, "P_MISSING", ""}};     // not a particle file
    render::InterpolatedFrame f = make_snapshot({10, 11}, reqs);
    CHECK(f.effects.size() == 4 && f.effects[0].lua_seq == 2 && f.effects[0].bone.empty());  // seq + 1; object 12 and frame 1 dropped
    SimAttachStats st = sync(f);
    pw.update(ParticleWorld::tick_delta(1));
    std::printf("sim lua effects: %u lua, %u at a bone, %u missing, %u systems\n", st.lua, st.lua_bones, st.attached_missing,
                pw.stats().systems);
    CHECK(st.lua == 3 && st.lua_bones == 1 && st.attached == 3 && st.attached_missing == 1 && pw.stats().systems == 3);
    CHECK(live_systems() == 3 && st.synced == 2);
    // Positions in creation order: object 10 root, object 10 at B1 (the bone offset = 10 x type scale), object 11 root.
    std::vector<assets::Float3> pos;
    pw.for_each([&](uint32_t, const SystemState& s) { pos.push_back(s.cur.translation()); });
    CHECK(pos.size() == 3);
    if (pos.size() == 3) {
        const float scale = table[frig].scale;
        const float dx = pos[1].x - pos[0].x, dy = pos[1].y - pos[0].y, dz = pos[1].z - pos[0].z;
        CHECK(std::fabs(std::sqrt(dx * dx + dy * dy + dz * dz) - 10.0f * scale) < 1e-3f);
        CHECK(pos[0].x == 1000.0f && pos[2].x == 1100.0f);
    }
    // Same requests next frame: the systems are kept (not recreated, nothing released).
    st = sync(make_snapshot({10, 11}, reqs));
    CHECK(st.lua == 3 && at.stats().released == 0 && live_systems() == 3);
    // A request that leaves the list releases only its system.
    reqs.erase(reqs.begin() + 2);
    st = sync(make_snapshot({10, 11}, reqs));
    CHECK(st.lua == 2 && at.stats().released == 1 && live_systems() == 2);
    // Object 10 dies (leaves the snapshot): its Lua systems go with it, object 11's stays.
    st = sync(make_snapshot({11}, reqs));
    CHECK(st.lua == 1 && at.stats().released == 1 && live_systems() == 1);
    // A garrisoned object (kSnapContained) keeps no systems.
    f = make_snapshot({11}, reqs);
    f.objects[0].flags = static_cast<uint8_t>(f.objects[0].flags | render::kSnapContained);
    st = sync(f);
    CHECK(st.lua == 0 && live_systems() == 0);
    // Nothing left in the world after the last object is gone.
    st = sync(make_snapshot({}, reqs));
    CHECK(st.lua == 0 && live_systems() == 0);
    std::printf("sim lua effects: OK\n");
}
#endif

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    log::set_min_level(log::Level::Error);
    const std::string src_dir = ALAMO_RENDER_SOURCE_DIR;
    test_names_pools();
    test_timing();
    test_throttle();
    test_attach();
    test_real_data();
    test_gpu(src_dir);
#if defined(ALAMO_PARTICLES_HAVE_SIM)
    test_sim();
    test_sim_effects();
    test_sim_lua_effects();
#else
    std::printf("sim: alamo_render_sim not configured, skipped\n");
#endif
    if (g_fail) std::printf("%d FAILED\n", g_fail);
    else std::printf("all particle tests passed\n");
    return g_fail ? 1 : 0;
}
