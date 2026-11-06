// alamo_render_r5_tests - Phase 4 R3 / R4 / R5 render-side additions (no framework; non-zero exit on failure).
//   * R5 fog: state -> intensity rules (f, r, fade-in, regrow), FogRenderer border / blur / palettes (land, setup,
//     space), upload-on-change, disabled / invalid inputs; a real tactical FogOfWar grid (reveal, decay window)
//     -> render view -> texels; snapshot plumbing (fog copy, digest, kSnapFogHidden, hardpoint events)
//   * R5 SimInstanceBuilder: HideWhenFogged fade over ticks, hardpoint events in event order, R4 alt passed, R3
//     per-bone visibility from an AnimPoseCache of a synthetic clip with visibility bits
//   * R5 decorations: SurfaceFX / TerrainDecalFX parsing (synthetic + shipped data), surface types, runtime decal
//     spawn rules (height, coverage, fade, cap, determinism), SimDecorations tracks + scorch marks from frames
//   * GPU (skipped without a Vulkan 1.3 device; 0 validation messages): R4 _ALT swap per instance (pixels);
//     real data (ALAMO_FOC_DATA): _MP_LAND_ENDOR with a sim FogOfWar grid, scorch decals and a unit track,
//     PNGs written to the build directory (r5_endor_fog.png, r5_endor_nofog.png, r5_endor_decals.png)
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/assets/asset_cache.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/log.hpp"
#include "alamo/jobs/job_system.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/render/fog_view.hpp"
#include "alamo/render/lod.hpp"
#include "alamo/render/map_scene.hpp"
#include "alamo/render/pipelines.hpp"
#include "alamo/render/png.hpp"
#include "alamo/render/scene_anim.hpp"
#include "alamo/render/sim_decorations.hpp"
#include "alamo/render/sim_session.hpp"
#include "alamo/render/sim_snapshot.hpp"
#include "alamo/render/sim_view.hpp"
#include "alamo/render/surface_fx.hpp"
#include "alamo/rhi/rhi.hpp"
#include "alamo/vfs/vfs.hpp"
#ifdef ALAMO_R5_HAVE_FOG
#include "alamo/tactical/fog/fog_of_war.hpp"
#endif
#ifdef ALAMO_R5_HAVE_ANIM
#include "alamo/anim/anim_behaviors.hpp"
#include "alamo/anim/anim_state.hpp"
#include "alamo/render/anim/anim_pose_cache.hpp"
#endif

using namespace alamo;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

static std::string env_or_empty(const char* name) {
#ifdef _MSC_VER
    char* v = nullptr;
    size_t len = 0;
    std::string out;
    if (_dupenv_s(&v, &len, name) == 0 && v) {
        out = v;
        std::free(v);
    }
    return out;
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

static std::span<const uint8_t> bytes_of(std::string_view s) { return {reinterpret_cast<const uint8_t*>(s.data()), s.size()}; }

// ------------------------------------------------------------------------------------------- fog (CPU)
static render::FogFrameInput fog_input(uint32_t w, uint32_t h, uint8_t state, uint8_t fade = 0) {
    render::FogFrameInput f;
    f.valid = true;
    f.width = w;
    f.height = h;
    f.x0 = 0;
    f.y0 = 0;
    f.x1 = 15.0f * static_cast<float>(w);
    f.y1 = 15.0f * static_cast<float>(h);
    f.cell_w = f.cell_h = 15.0f;
    f.state.assign(static_cast<size_t>(w) * h, state);
    f.fade.assign(static_cast<size_t>(w) * h, fade);
    f.frame = 100;
    f.last_decay = 99;  // f = 0
    f.regrow_step = 21;
    return f;
}

static void test_fog_rules() {
    CHECK(render::fog_frames_since_decay(100, 99) == 0);
    CHECK(render::fog_frames_since_decay(100, 95) == 4);
    CHECK(render::fog_frames_since_decay(200, 10) == 15);
    CHECK(render::fog_frames_since_decay(5, 5) == 0 && render::fog_frames_since_decay(3, 9) == 0);
    CHECK(render::fog_regrow_per_frame(21) == 1 && render::fog_regrow_per_frame(-1) == 0 && render::fog_regrow_per_frame(40) == 2);
    const auto& table = terrain::fow_intensity_table();
    // Clear (16..238) stays 255, fogged 0 stays 0; the last regrow steps fade; fresh reveals fade in.
    std::vector<uint8_t> out;
    render::FogFrameInput in = fog_input(4, 4, 100);
    CHECK(render::fog_intensity(in, out) && out.size() == 16 && out[0] == 255);
    in = fog_input(4, 4, 0);
    CHECK(render::fog_intensity(in, out) && out[5] == 0);
    in = fog_input(4, 4, 8);
    in.last_decay = 95;  // f = 4, r = 1 -> 8 - 4 = 4
    CHECK(render::fog_intensity(in, out) && out[0] == table[4]);
    in = fog_input(4, 4, 255, 10);  // counted after a decay: 255 - 10 + f(0) = 245
    CHECK(render::fog_intensity(in, out) && out[0] == table[245]);
    in.last_decay = 90;  // f = 9: 254
    CHECK(render::fog_intensity(in, out) && out[0] == table[254]);
    in = fog_input(4, 4, 239, 0);  // first reveal
    CHECK(render::fog_intensity(in, out) && out[0] == table[239] && table[239] < 64);
    // Unusable inputs.
    in = fog_input(4, 4, 100);
    in.state.pop_back();
    CHECK(!in.usable() && !render::fog_intensity(in, out) && out.empty());
    in = fog_input(4, 4, 100);
    in.valid = false;
    CHECK(!in.usable());
    in = fog_input(4, 4, 100);
    in.x1 = in.x0;
    CHECK(!in.usable());
    in = fog_input(4, 4, 100);
    in.render_option = false;
    CHECK(in.usable() && !in.drawn());

    // FogRenderer CPU build: border fogged, blur, land palette (fogged texel = 129 grey, alpha 0 / clear = 254 white).
    render::FogRenderer fr;
    in = fog_input(8, 6, 100);
    CHECK(fr.build(in, false));
    const terrain::FowGrid& g = fr.grid();
    CHECK(g.width() == 8 && g.height() == 6 && fr.processed().size() == 48);
    CHECK(fr.processed()[0] == 0 && fr.processed()[8 * 3 + 4] == 255 && fr.processed()[8 + 1] < 255);  // border / centre / blurred edge
    CHECK(g.texels()[0] == 129 && g.texels()[3] == 0);                                                     // fogged: LandFOWColor 130 -> 129
    const size_t c = (8 * 3 + 4) * 4;
    CHECK(g.texels()[c] == 254 && g.texels()[c + 3] == 254);  // clear: white (i = 255 -> 254)
    CHECK(fr.value_at(15.0f * 4.5f, 15.0f * 6 - 15.0f * 3.5f) == 255 && fr.value_at(1.0f, 1.0f) == 0);
    CHECK(fr.value_at(-100.0f, 0.0f) == 255);  // outside the grid
    CHECK(fr.stats().fogged_cells > 0 && fr.stats().clear_cells > 0);
    // Setup phase palette (SetupPhaseFOWColor 0,130,130,130 -> fog A 130, R 0, G 130, B 130).
    in.setup_phase = true;
    CHECK(fr.build(in, false) && fr.grid().texels()[0] == 0 && fr.grid().texels()[1] == 129 && fr.grid().texels()[3] == 129);
    // Space palette: visible transparent black, fog = SpaceFOWColor (255,255,255,255) -> fogged white opaque-ish.
    in.setup_phase = false;
    CHECK(fr.build(in, true) && fr.grid().texels()[0] == 254 && fr.grid().texels()[3] == 0 && fr.grid().texels()[c + 3] == 0);  // [port] edge fade: border alpha 0
    // GameConstants colours.
    render::FogColors cols;
    const char* gc = "<GameConstants><LandFOWColor>10,20,30,40</LandFOWColor><SpaceFOWColor>1, 2, 3</SpaceFOWColor></GameConstants>";
    CHECK(render::parse_fog_colors(bytes_of(gc), cols));
    CHECK(cols.land_fog == terrain::pack_fow_color(10, 20, 30, 40) && cols.space_fog == terrain::kSpaceFowColor);
    CHECK(!render::parse_fog_colors(bytes_of("not xml"), cols));
    std::printf("fog rules: OK (fogged texel %u, clear %u)\n", g.texels()[0], g.texels()[c]);
}

#ifdef ALAMO_R5_HAVE_FOG
// A real sim grid: reveal, decay window, render view -> FogFrameInput -> texels.
static void test_fog_sim_grid() {
    tactical::fog::FogOfWar fog;
    tactical::fog::FogSetup setup;
    setup.mode = tactical::fog::FogMode::Land;
    setup.box = {0.0f, 0.0f, 1500.0f, 1200.0f};
    setup.players = {{0, true, true}, {1, true, false}};
    fog.start(setup);
    tactical::fog::FogObject unit;
    unit.owner = 0;
    unit.x = 750.0f;
    unit.y = 600.0f;
    unit.sight_range = 200.0f;
    uint32_t frame = 1;
    fog.set_frame(frame);
    fog.source_service(7, unit);
    auto view_at = [&](uint32_t f) {
        fog.decay_step(f);
        return render::fog_frame_input(fog.render_view(0));
    };
    std::shared_ptr<const render::FogFrameInput> in;
    for (; frame <= 32; ++frame) {
        in = view_at(frame);
        fog.source_service(7, unit);
    }
    CHECK(in && in->usable() && in->width == 100 && in->height == 80 && in->player == 0);
    render::FogRenderer fr;
    CHECK(fr.build(*in, false));
    // Right after a decay the fresh cells render at table[255 - fade] (fade-in over the next frames, DECORATIONS 2.3).
    CHECK(fr.value_at(750.0f, 600.0f) >= 200 && fr.value_at(100.0f, 100.0f) == 0);
    uint32_t clear0 = 0;
    for (uint8_t v : fr.processed()) clear0 += v >= 200 ? 1u : 0u;
    std::printf("fog sim grid: %ux%u, regrow step %d, %u clear cells while revealed\n", in->width, in->height, in->regrow_step, clear0);
    CHECK(clear0 > 400);
    // The source leaves: the cells stay clear for the 6.4-6.9 s regrow window, then fog (render follows the state).
    fog.source_remove(7);
    uint32_t clear_until = 0;
    for (uint32_t k = 1; k <= 260; ++k) {
        in = view_at(frame + k);
        fr.build(*in, false);
        if (fr.value_at(750.0f, 600.0f) > 0) clear_until = k;
    }
    std::printf("fog sim grid: centre visible for %u frames after the source left\n", clear_until);
    CHECK(clear_until >= 180 && clear_until <= 215);
    CHECK(fr.value_at(750.0f, 600.0f) == 0);
    // Snapshot plumbing: the copy is independent of the grid; digest covers it.
    render::SimSnapshot s;
    const uint64_t d0 = s.digest();
    s.fog = in;
    CHECK(s.digest() != d0);
    render::InterpolatedFrame f;
    render::interpolate(nullptr, s, 1.0f, render::InterpMode::Original, f);
    CHECK(f.fog == s.fog);
}
#endif

// ------------------------------------------------------------------------------------------- snapshot plumbing
static render::SnapshotObject sobj(int32_t id, float x, uint32_t hp_first = 0, uint32_t hp_count = 0) {
    render::SnapshotObject o;
    o.id = id;
    o.type = 0;
    o.position = {x, 0, 0};
    o.facing = {1, 0, 0};
    o.hardpoint_first = hp_first;
    o.hardpoint_count = hp_count;
    return o;
}

static void test_snapshot_plumbing() {
    render::SimSnapshot s;
    s.objects = {sobj(1, 0), sobj(3, 1, 0, 3), sobj(5, 2, 3, 2)};
    s.hardpoint_destroyed.assign(5, 0);
    const uint64_t d0 = s.digest();
    const std::vector<render::SnapshotHardpointEvent> ev = {{3, 2}, {5, 0}, {3, 0}, {4, 0}, {5, 7}};
    render::set_snapshot_hardpoint_events(s, ev);
    CHECK(s.hardpoint_events.size() == 3 && s.hardpoint_events[0].index == 2 && s.hardpoint_events[2].id == 3);
    CHECK((s.hardpoint_destroyed == std::vector<uint8_t>{1, 0, 1, 1, 0}));
    CHECK(s.digest() != d0);
    render::apply_fog_hidden(s, [](const render::SnapshotObject& o) { return o.id == 3; });
    CHECK((s.objects[1].flags & render::kSnapFogHidden) && !(s.objects[0].flags & render::kSnapFogHidden));
    render::apply_fog_hidden(s, nullptr);
    CHECK(!(s.objects[1].flags & render::kSnapFogHidden));
    render::InterpolatedFrame f;
    render::interpolate(nullptr, s, 1.0f, render::InterpMode::Smooth, f);
    CHECK(f.hardpoint_events.size() == 3);
}

// ------------------------------------------------------------------------------------------- OI render (R1 / R5)
// Request record shaped like lua::ParticleEffectRequest (snapshot_lua_effects is a template over its fields).
struct FakeLuaRequest {
    uint32_t frame = 0, seq = 0;
    int32_t object = 0, type = -1;
    std::string type_name, bone;
};

static void test_oi_snapshot() {
    // R1 _ALT rule (TACTICAL_GAPS §6.3): last match of every threshold in list order; the data quirk 1 / 3 kept.
    render::SnapshotTypeInfo ti;
    ti.damage_thresholds = {1.0f, 0.66f, 0.33f};
    ti.damage_alternates = {0, 1, 2};
    CHECK(render::damage_alt_index(ti, 1.0f) == 0 && render::damage_alt_index(ti, 0.5f) == 1 && render::damage_alt_index(ti, 0.2f) == 2);
    render::SnapshotTypeInfo quirk;
    quirk.damage_thresholds = {1.0f};
    quirk.damage_alternates = {3};
    CHECK(render::damage_alt_index(quirk, 1.0f) == 3);
    // R1 authoritative combat source: its states replace the provisional bits of the objects it tracks.
    render::SimSnapshot s;
    s.objects = {sobj(3, 1, 0, 3), sobj(5, 2, 3, 2)};
    s.hardpoint_destroyed = {1, 0, 1, 1, 0};
    render::apply_hardpoint_source(
        s, [](objects::ObjectId id) { return id == 3 ? 3u : 0u; }, [](objects::ObjectId, uint32_t k) { return k == 1; });
    CHECK((s.hardpoint_destroyed == std::vector<uint8_t>{0, 1, 0, 1, 0}));
    // R5 ability events: state changes against the previous snapshot (type order, then stealth).
    render::SimSnapshot prev, cur;
    prev.objects = {sobj(1, 0)};
    prev.objects[0].ability_bits[0] = uint64_t{1} << 5;
    cur.objects = {sobj(1, 0), sobj(3, 1)};
    cur.objects[0].ability_bits[0] = uint64_t{1} << 7;
    cur.objects[1].ability_bits[0] = uint64_t{1} << render::kAbilityStealth;
    cur.objects[1].flags = render::kSnapStealthMeshes;
    render::set_snapshot_ability_events(cur, &prev);
    CHECK(cur.ability_events.size() == 4);
    if (cur.ability_events.size() == 4) {
        CHECK(cur.ability_events[0].id == 1 && cur.ability_events[0].type == 5 && cur.ability_events[0].kind == render::SnapshotAbilityKind::Deactivated);
        CHECK(cur.ability_events[1].type == 7 && cur.ability_events[1].kind == render::SnapshotAbilityKind::Activated);
        CHECK(cur.ability_events[2].id == 3 && cur.ability_events[2].type == render::kAbilityStealth);
        CHECK(cur.ability_events[3].kind == render::SnapshotAbilityKind::StealthOn);
    }
    render::InterpolatedFrame f;
    render::interpolate(&prev, cur, 1.0f, render::InterpMode::Original, f);
    CHECK(f.ability_events.size() == 4);
    const uint64_t d0 = cur.digest();
    render::set_snapshot_ability_events(cur, &cur);  // no change against itself
    CHECK(cur.ability_events.empty() && cur.digest() != d0);
    // R5 Lua Attach_Particle_Effect requests -> attached effects (frame filter, seq key, bone, objects in the snapshot).
    const std::string dir = (fs::temp_directory_path() / "alamo_render_r5_tests").string();
    std::string err;
    const auto w = render::make_synthetic_sim_world(dir, &err);
    CHECK(w != nullptr);
    if (w) {
        render::SnapshotTypeTable table;
        table.build(w->types, objects::ModeKind::Space);
        const std::vector<FakeLuaRequest> reqs = {
            {5, 0, 1, -1, "P_Old_Effect", ""}, {20, 3, 1, -1, "P_Smoke_Test", "B1"}, {21, 4, 9, -1, "P_Other", ""}};
        const std::vector<render::SnapshotEffect> fx = render::snapshot_lua_effects(reqs, table, 10);
        CHECK(fx.size() == 2 && fx[0].lua_seq == 4 && fx[0].bone == "B1" && fx[0].percent == render::kEffectLua && fx[0].asset == "P_Smoke_Test");
        render::SimSnapshot ls;
        ls.objects = {sobj(1, 0)};
        render::append_snapshot_effects(ls, fx);
        CHECK(ls.effects.size() == 1 && ls.effects[0].id == 1);  // object 9 is not in the snapshot
    }
    // R5 reinforcement overlay: the sampler's cells replace the player's grid in palette 3 (space only).
    render::FogFrameInput in = fog_input(8, 8, 255);
    in.reinforce.assign(in.cells(), 0xFF);
    for (uint32_t y = 3; y < 5; ++y)
        for (uint32_t x = 3; x < 5; ++x) in.reinforce[y * 8 + x] = 0;
    render::FogRenderer fr;
    CHECK(fr.build(in, true) && !fr.stats().reinforce_sampled);
    const uint8_t own = fr.processed()[3 * 8 + 3];
    fr.set_space_reinforce(true);
    CHECK(fr.build(in, true) && fr.stats().reinforce_sampled);
    const uint8_t blocked = fr.processed()[3 * 8 + 3];
    CHECK(blocked < own);
    CHECK(fr.build(in, false) && !fr.stats().reinforce_sampled);  // land: never the space overlay
}

// ------------------------------------------------------------------------------------------- builder
struct FakeModels final : render::SimModelSource {
    const render::GpuModel* model = nullptr;
    const render::GpuModel* type_model(objects::TypeIndex) override { return model; }
    const render::GpuModel* named_model(const std::string&) override { return nullptr; }
};

// Three bones (root, B1, B2), meshes HULL (B1), FLAG (B2), HULL_ALT1 (B1), one proxy on B2.
static std::shared_ptr<assets::ModelAsset> three_bone_model() {
    auto m = std::make_shared<assets::ModelAsset>();
    m->name = "R5_TEST.ALO";
    assets::Skeleton& sk = m->skeleton;
    sk.names = {"ROOT", "B1", "B2"};
    sk.parents = {-1, 0, 0};
    sk.visible = {1, 1, 1};
    sk.billboard_mode = {0, 0, 0};
    sk.bind_local = {assets::Mat3x4{}, assets::Mat3x4{}, assets::Mat3x4{}};
    sk.finalize();
    for (const char* n : {"HULL", "FLAG", "HULL_ALT1"}) {
        assets::MeshInfo mi;
        mi.name = n;
        mi.attach_bone = std::string_view(n) == "FLAG" ? 2u : 1u;
        m->meshes.push_back(mi);
    }
    assets::Proxy p;
    p.name = "P_TEST";
    p.bone = 2;
    m->proxies.push_back(p);
    return m;
}

// Independent double-precision reference of the critically damped spring (ABILITY_VISUALS 1.1): `n` ticks of dt = 1/30.
static double ref_spring(double v, double target, double T, int n, double* vel_out = nullptr) {
    const double dt = 1.0 / 30.0, w = 2.0 / T, x = w * dt, k = 1.0 / (1.0 + x + 0.48 * x * x + 0.235 * x * x * x);
    double vel = 0.0;
    for (int i = 0; i < n; ++i) {
        const double c = v - target, t = (vel + w * c) * dt;
        vel = (vel - w * t) * k;
        v = target + (c + t) * k;
    }
    if (vel_out) *vel_out = vel;
    return v;
}

static void test_builder() {
    const std::string dir = (fs::temp_directory_path() / "alamo_render_r5_tests").string();
    std::string err;
    const auto w = render::make_synthetic_sim_world(dir, &err);
    CHECK(w != nullptr);
    if (!w) return;
    render::SnapshotTypeTable table;
    table.build(w->types, objects::ModeKind::Land);
    render::GpuModel gm;
    gm.asset = three_bone_model();
    gm.max_alt = 1;  // HULL_ALT1 (Renderer::upload_model computes it)
    gm.mesh_hidden.assign(3, 0);
    FakeModels models;
    models.model = &gm;
    render::SimInstanceBuilder b(table);
    // (default HideWhenFogged spring, smooth time 0.25 s, dt 1/30 per sim tick)
    render::InterpolatedFrame f;
    f.objects = {sobj(1, 0), sobj(2, 10)};
    f.objects[1].flags = render::kSnapFogHidden;  // hidden from its first frame: starts at 0
    f.objects[0].alt = 1;
    std::vector<render::Instance> main, bg;
    render::SimInstanceStats st;
    f.tick = 1;
    b.build(f, models, main, bg, &st);
    CHECK(main.size() == 1 && main[0].alt == 1 && st.alt_nonzero == 1 && st.fog_hidden == 1 && st.fog_faded_out == 1);
    CHECK(b.fog_alpha(2) == 0.0f && b.fog_alpha(1) == 1.0f);
    // Object 1 becomes hidden: the 0.25 s critically damped spring (3 ticks later: reference below), drawn while alpha >= 0.025.
    f.objects[0].flags = render::kSnapFogHidden;
    f.objects[1].flags = 0;
    f.tick = 4;
    b.build(f, models, main, bg, &st);
    const double ref_down = ref_spring(1.0, 0.0, 0.25, 3), ref_up = ref_spring(0.0, 1.0, 0.25, 3);
    CHECK(std::fabs(b.fog_alpha(1) - ref_down) < 1e-4 && std::fabs(b.fog_alpha(2) - ref_up) < 1e-4 && main.size() == 2);
    CHECK(ref_down > 0.5 && ref_down < 0.9);  // sanity: well inside the fade
    CHECK(std::fabs(main[0].light_scale.w - ref_down) < 1e-4);
    const float a_tick4 = b.fog_alpha(1);
    f.tick = 4;  // same tick again: no step
    b.build(f, models, main, bg, &st);
    CHECK(b.fog_alpha(1) == a_tick4);
    f.tick = 40;  // 36 more ticks: settled
    b.build(f, models, main, bg, &st);
    CHECK(b.fog_alpha(1) < render::SimInstanceBuilder::kFogHideAlpha && b.fog_alpha(2) > 0.99f && main.size() == 1 &&
          st.fog_faded_out == 1);
    // Hardpoint events in event order (the sticky bits follow).
    render::InterpolatedFrame h;
    h.tick = 21;
    h.objects = {sobj(9, 0, 0, 3)};
    h.hardpoint_destroyed = {0, 1, 1};
    h.hardpoint_events = {{9, 2}, {9, 1}};
    b.build(h, models, main, bg, &st);
    const render::HardpointVisibility* hv = b.hardpoint_state(9);
    CHECK(hv && hv->destroyed(1) && hv->destroyed(2) && !hv->destroyed(0) && st.hardpoint_events == 2);
    // R3 per-bone visibility: B2 hidden -> FLAG (mesh 1) and the proxy hidden, HULL kept.
    std::vector<uint8_t> vis = {1, 1, 0};
    b.set_visibility_source([&](objects::ObjectId id, const render::GpuModel&) {
        return id == 9 ? std::span<const uint8_t>(vis) : std::span<const uint8_t>();
    });
    h.tick = 22;
    b.build(h, models, main, bg, &st);
    CHECK(main.size() == 1 && main[0].mesh_hidden.size() >= 3 && main[0].mesh_hidden[0] == 0 && main[0].mesh_hidden[1] == 1);
    CHECK(st.bone_hidden_meshes == 1 && b.mesh_mask(9).size() == main[0].mesh_hidden.size());
    vis = {1, 1, 1};
    b.build(h, models, main, bg, &st);
    CHECK(st.bone_hidden_meshes == 0 && (main[0].mesh_hidden.empty() || main[0].mesh_hidden[1] == 0));
    // R4 ALT selection: every alternate uploaded (kAnyAlt), the default look keeps alt 0.
    const render::MeshSelection all = render::select_meshes_all_lods(*gm.asset, render::kAnyAlt, true, nullptr);
    CHECK(all.mesh_visible == std::vector<uint8_t>({1, 1, 1}) && all.alt == 0);
    const render::MeshSelection a0 = render::select_meshes_all_lods(*gm.asset, 0, true, nullptr);
    CHECK(a0.mesh_visible == std::vector<uint8_t>({1, 1, 0}));
    // R3 detached corpses: added to the snapshot as kSnapCorpse objects, drawn faded with the pose of the same id.
    {
        objects::TypeIndex drawable = objects::kNone;
        for (objects::TypeIndex t = 0; t < table.size() && drawable == objects::kNone; ++t)
            if (!table[t].model.empty()) drawable = t;
        CHECK(drawable != objects::kNone);
        render::SimSnapshot s;
        s.mode = objects::ModeKind::Land;
        s.objects = {sobj(1, 0), sobj(5, 1)};
        s.objects[0].type = s.objects[1].type = drawable;
        struct FakeCorpse {  // anim::Corpse field names
            int32_t id;
            bool detached;
            struct {
                uint32_t object_type;
                int32_t owner;
                float position[3];
                float facing[3];
            } info;
            float a;
            float alpha(uint64_t) const { return a; }
        };
        const std::vector<FakeCorpse> raw = {{3, true, {drawable, 0, {7, 8, 9}, {0, 1, 0}}, 0.5f},
                                             {4, false, {drawable, 0, {0, 0, 0}, {1, 0, 0}}, 1.0f},  // not detached
                                             {5, true, {drawable, 0, {0, 0, 0}, {1, 0, 0}}, 1.0f},   // store object exists
                                             {7, true, {drawable, 0, {0, 0, 0}, {1, 0, 0}}, 0.0f},   // faded out
                                             {9, true, {0xFFFFFFFFu, 0, {0, 0, 0}, {1, 0, 0}}, 1.0f}};
        const std::vector<render::SnapshotCorpse> sc = render::snapshot_corpses(raw, 100);
        CHECK(sc.size() == 4 && sc[0].id == 3 && sc[0].position.z == 9.0f && sc[0].alpha == 0.5f);
        render::add_snapshot_corpses(s, table, sc);
        CHECK(s.objects.size() == 3 && s.objects[1].id == 3 && (s.objects[1].flags & render::kSnapCorpse) && s.objects[1].fade == 0.5f &&
              s.objects[1].health == 0.0f && s.objects[1].facing.y == 1.0f);
        render::InterpolatedFrame cf;
        render::interpolate(nullptr, s, 1.0f, render::InterpMode::Original, cf);
        cf.tick = 100;
        render::SimInstanceBuilder cb(table);
        std::vector<objects::ObjectId> posed;
        cb.set_pose_source([&](objects::ObjectId id, const render::GpuModel&) {
            posed.push_back(id);
            return std::span<const render::Mat3x4>();
        });
        cb.build(cf, models, main, bg, &st);
        CHECK(main.size() == 3 && posed.size() == 3 && posed[1] == 3 && std::fabs(main[1].light_scale.w - 0.5f) < 1e-6f);
    }
    std::printf("builder: OK\n");
}

#ifdef ALAMO_R5_HAVE_ANIM
// AnimPoseCache visibility: a 2-frame clip whose track for B2 is invisible on frame 0.
// R1 applied _ALT (0x12dca0: only up to the model's highest ALT index) and the R5 stealth mesh swap.
static void test_oi_builder() {
    const std::string dir = (fs::temp_directory_path() / "alamo_render_r5_tests").string();
    std::string err;
    const auto w = render::make_synthetic_sim_world(dir, &err);
    CHECK(w != nullptr);
    if (!w) return;
    render::SnapshotTypeTable table;
    table.build(w->types, objects::ModeKind::Space);
    render::GpuModel gm;
    auto ma = three_bone_model();
    for (const char* n : {"BASEMESH_HULL", "STEALTH_SHELL"}) {
        assets::MeshInfo mi;
        mi.name = n;
        mi.attach_bone = 1;
        ma->meshes.push_back(mi);
    }
    gm.asset = ma;
    gm.mesh_hidden = {0, 0, 0, 0, 1};  // the STEALTH mesh hidden by default (name rule)
    gm.max_alt = 1;
    FakeModels models;
    models.model = &gm;
    render::SimInstanceBuilder b(table);
    render::InterpolatedFrame f;
    f.objects = {sobj(1, 0)};
    std::vector<render::Instance> main, bg;
    render::SimInstanceStats st;
    const int8_t alts[4] = {3, 1, 3, 0};  // 3 > max ALT: not applied (keeps the previous one)
    const int expect[4] = {0, 1, 1, 0};
    for (int k = 0; k < 4; ++k) {
        f.objects[0].alt = alts[k];
        f.tick = static_cast<uint64_t>(k + 1);
        b.build(f, models, main, bg, &st);
        CHECK(main.size() == 1 && main[0].alt == expect[k] && b.applied_alt(1, -9) == expect[k]);
    }
    f.objects[0].flags = render::kSnapStealthMeshes;
    f.tick = 9;
    b.build(f, models, main, bg, &st);
    CHECK(main.size() == 1 && st.stealthed == 1);
    if (main.size() == 1 && main[0].mesh_hidden.size() >= 5) CHECK(main[0].mesh_hidden[3] == 1 && main[0].mesh_hidden[4] == 0);
}

static void test_anim_visibility() {
    anim::AnimModel model;
    model.model = "R5_TEST.ALO";
    const std::vector<std::string> names = {"ROOT", "B1", "B2"};
    const std::vector<int32_t> parents = {-1, 0, 0};
    const std::vector<uint32_t> bb = {0, 0, 0};
    const std::vector<anim::Mat34> bind(3, anim::Mat34{});
    model.skeleton = anim::skeleton_from_arrays(names, parents, bb, bind);
    model.has_skeleton = true;
    auto clip = std::make_shared<anim::Clip>();
    clip->name = "R5_TEST_IDLE_00.ALA";
    clip->frame_count = 2;
    clip->fps = 30.0f;
    clip->bone_count = 2;
    clip->rot_raw = {0, 0, 0, 32767, 0, 0, 0, 32767};
    for (uint32_t t = 0; t < 2; ++t) {
        anim::ClipTrack tr;
        tr.bone_name = names[t + 1];
        tr.file_bone_index = t + 1;
        tr.rot_first = t;
        tr.has_visibility = t == 1;
        tr.vis_first = 0;
        clip->tracks.push_back(tr);
    }
    clip->vis_bits = {0x00};  // B2 hidden on both frames
    anim::ClipRef ref;
    ref.clip = clip;
    model.clips[static_cast<size_t>(anim::kAnimIdle)].push_back(ref);
    anim::bind_clips(model);
    CHECK(model.clips[0][0].bound);
    anim::AnimWorld world;
    anim::AnimCreateParams cp;
    cp.loop_idle = true;
    world.create(4, &model, cp, nullptr, 0);
    render::AnimPoseCache cache;
    cache.update(&world);
    const std::span<const uint8_t> v = cache.visibility(4, 3);
    CHECK(v.size() == 3 && v[0] == 1 && v[1] == 1 && v[2] == 0);
    CHECK(cache.visibility(4, 4).empty() && cache.visibility(5, 3).empty() && cache.pose(4, 3).size() == 3);
    clip->vis_bits = {0x03};
    cache.update(&world);
    CHECK(cache.visibility(4, 3).empty());  // everything visible: nothing to hide
    // The real anim::Corpse converts through the render template (field names checked by the compiler).
    anim::Corpse corpse;
    corpse.id = 4;
    corpse.detached = true;
    corpse.info.object_type = 2;
    corpse.info.position[2] = 5.0f;
    const std::vector<anim::Corpse> corpses = {corpse};
    const std::vector<render::SnapshotCorpse> sc = render::snapshot_corpses(corpses, 0);
    CHECK(sc.size() == 1 && sc[0].id == 4 && sc[0].type == 2 && sc[0].position.z == 5.0f && sc[0].alpha == 1.0f);
    std::printf("anim visibility: OK\n");
}
#endif

// ------------------------------------------------------------------------------------------- R3 / R5 stage 2
// fog_point_visible, ghosts, contained, roll, muzzles, scene-model frame rule.
static void test_stage2_builder() {
    // fog_point_visible: §1.2 cell rules on the copy.
    render::FogFrameInput fin = fog_input(10, 10, 0);  // 150 x 150 box, all fogged
    fin.state[2 * 10 + 3] = 7;                         // row 2 (from the top), column 3 visible
    CHECK(render::fog_point_visible(fin, 3 * 15 + 1.0f, 150.0f - 2 * 15 - 1.0f));
    CHECK(!render::fog_point_visible(fin, 1.0f, 1.0f) && !render::fog_point_visible(fin, -5.0f, 10.0f));
    render::FogFrameInput off = fin;
    off.fow_active = false;
    CHECK(render::fog_point_visible(off, 1.0f, 1.0f));
    CHECK(render::fog_point_visible(render::FogFrameInput{}, 1.0f, 1.0f));
    // Roll: 0 = the plain transform; pi/2 about the facing (+X, the model's nose -Y) turns the model's +X (left, +Y
    // unrolled) onto +Z and its up onto -Y.
    const render::Mat3x4 xa = render::object_transform({1, 2, 3}, {1, 0, 0}, 2.0f), xb = render::object_transform({1, 2, 3}, {1, 0, 0}, 2.0f, 0.0f);
    CHECK(std::memcmp(&xa, &xb, sizeof xa) == 0);
    const render::Mat3x4 r = render::object_transform({0, 0, 0}, {1, 0, 0}, 1.0f, 1.5707963f);
    CHECK(std::fabs(r.m[2][0] - 1.0f) < 1e-5f && std::fabs(r.m[1][0]) < 1e-5f && std::fabs(r.m[0][1] + 1.0f) < 1e-6f &&
          std::fabs(r.m[1][2] + 1.0f) < 1e-5f);
    CHECK(render::SceneModelAnimator::frame_position(0.0, 30.0f, 10) == 0.0f);
    CHECK(std::fabs(render::SceneModelAnimator::frame_position(1.0, 30.0f, 11) - 0.0f) < 1e-4f);  // 30 frames wrap at 10
    CHECK(std::fabs(render::SceneModelAnimator::frame_position(0.5, 30.0f, 11) - 5.0f) < 1e-4f);
    CHECK(render::SceneModelAnimator::frame_position(2.0, 30.0f, 1) == 0.0f);
    render::SceneModelAnimator noanim(nullptr);
    render::GpuModel nm;
    CHECK(noanim.pose(nm, 3).empty());

    const std::string dir = (fs::temp_directory_path() / "alamo_render_r5_tests").string();
    std::string err;
    const auto w = render::make_synthetic_sim_world(dir, &err);
    CHECK(w != nullptr);
    if (!w) return;
    render::SnapshotTypeTable table;
    table.build(w->types, objects::ModeKind::Land);
    table.set_ghosts(0, true, false);   // Last_State_Visible_Under_FOW
    table.set_ghosts(1, false, true);   // Initial_State_Visible_Under_FOW
    render::GpuModel gm;
    {
        auto ma = three_bone_model();
        ma->meshes[1].hidden = true;  // FLAG (bone B2) ALO-hidden: the "muzzle flash"
        gm.asset = ma;
    }
    gm.mesh_hidden = {0, 1, 0};
    gm.max_alt = 1;
    FakeModels models;
    models.model = &gm;
    render::SimInstanceBuilder b(table);
    b.set_fog_smooth_time(0.0f);
    auto fogp = std::make_shared<render::FogFrameInput>(fog_input(10, 10, 255));  // everything visible
    render::InterpolatedFrame f;
    f.fog = fogp;
    render::SnapshotObject u = sobj(1, 0);
    u.type = 0;
    u.position = {20, 130, 0};  // cell (1, 1)
    u.alt = 1;
    f.objects = {u};
    std::vector<render::Instance> main, bg;
    render::SimInstanceStats st;
    f.tick = 1;
    b.build(f, models, main, bg, &st);
    CHECK(main.size() == 1 && b.ghost_count() == 0);
    // Hidden: the ghost stands at the last visible state while the real object moved away.
    auto fog2 = std::make_shared<render::FogFrameInput>(fog_input(10, 10, 0));
    f.fog = fog2;
    f.objects[0].flags = render::kSnapFogHidden;
    f.objects[0].position = {100, 30, 0};
    f.objects[0].alt = 0;
    f.tick = 2;
    b.build(f, models, main, bg, &st);
    CHECK(b.has_ghost(1) && st.ghosts == 1 && main.size() == 1 && main[0].transform.m[0][3] == 20.0f && main[0].alt == 1);
    // The object dies (gone from the frame): the ghost stays while its spot is fogged.
    f.objects.clear();
    f.tick = 3;
    b.build(f, models, main, bg, &st);
    CHECK(b.has_ghost(1) && main.size() == 1);
    // The spot is seen again: the ghost goes.
    auto fog3 = std::make_shared<render::FogFrameInput>(fog_input(10, 10, 0));
    fog3->state[1 * 10 + 1] = 200;
    f.fog = fog3;
    f.tick = 4;
    b.build(f, models, main, bg, &st);
    CHECK(!b.has_ghost(1) && main.empty());
    // Initial_State_Visible_Under_FOW: a ghost from the first (hidden) frame; no fog in the frame -> no ghosts.
    render::SnapshotObject s2 = sobj(2, 0);
    s2.type = 1;
    s2.position = {140, 10, 0};
    s2.flags = render::kSnapFogHidden;
    f.fog = fog2;
    f.objects = {s2};
    f.tick = 5;
    b.build(f, models, main, bg, &st);
    CHECK(b.has_ghost(2) && st.ghosts == 1);
    f.fog = nullptr;
    f.tick = 6;
    b.build(f, models, main, bg, &st);
    CHECK(b.ghost_count() == 0 && main.empty());
    // Contained: not drawn.
    render::SnapshotObject c = sobj(3, 0);
    c.type = 2;
    c.flags = render::kSnapContained;
    f.objects = {c};
    f.tick = 7;
    b.build(f, models, main, bg, &st);
    CHECK(main.empty() && st.contained == 1);
    // q6 t19: Lua-hidden (script_hidden): not drawn, counted apart.
    render::SnapshotObject hid = sobj(3, 0);
    hid.type = 2;
    hid.script_hidden = true;
    f.objects = {hid};
    f.tick = 7;
    b.build(f, models, main, bg, &st);
    CHECK(main.empty() && st.script_hidden == 1 && st.contained == 0);
    // Muzzle: bone B2 shows the ALO-hidden FLAG mesh this tick only.
    f.objects = {sobj(4, 0)};
    f.objects[0].type = 2;
    f.muzzles = {{4, "b2", -1}};
    f.tick = 8;
    b.build(f, models, main, bg, &st);
    CHECK(main.size() == 1 && main[0].mesh_hidden.size() >= 3 && main[0].mesh_hidden[1] == 0 && st.muzzle_meshes >= 1);
    f.muzzles.clear();
    f.tick = 9;
    b.build(f, models, main, bg, &st);
    CHECK(main.size() == 1 && (main[0].mesh_hidden.empty() || main[0].mesh_hidden[1] == 1) && st.muzzle_meshes == 0);
    // Snapshot plumbing for muzzles / foot plants / contained.
    render::SimSnapshot s;
    s.objects = {sobj(1, 0), sobj(5, 0)};
    const uint64_t d0 = s.digest();
    const std::vector<render::SnapshotMuzzle> mz = {{5, "MuzzleA_00", -1}, {1, "MuzzleA_01", 2}, {7, "X", -1}, {1, "", -1}};
    render::set_snapshot_muzzles(s, mz);
    CHECK(s.muzzles.size() == 3 && s.muzzles[0].id == 1 && s.muzzles[0].hardpoint == 2 && s.muzzles[1].bone.empty());  // own weapon kept
    const std::vector<render::SnapshotFootPlant> fp = {{5, 1, {1, 2, 3}, ""}, {6, 0, {0, 0, 0}, ""}};
    render::set_snapshot_foot_plants(s, fp);
    CHECK(s.foot_plants.size() == 1 && s.foot_plants[0].foot == 1);
    render::apply_contained(s, [](const render::SnapshotObject& o) { return o.id == 5; });
    CHECK((s.objects[1].flags & render::kSnapContained) && !(s.objects[0].flags & render::kSnapContained));
    CHECK(s.digest() != d0);
    const uint64_t d1 = s.digest();
    render::apply_script_hidden(s, [](const render::SnapshotObject& o) { return o.id == 1; });  // q6 t19
    CHECK(s.objects[0].script_hidden && !s.objects[1].script_hidden && s.digest() == d1);  // presentation only
    render::InterpolatedFrame fi;
    render::interpolate(nullptr, s, 1.0f, render::InterpMode::Original, fi);
    CHECK(fi.muzzles.size() == 3 && fi.foot_plants.size() == 1);
    CHECK(fi.objects[0].script_hidden && !fi.objects[1].script_hidden);
    render::apply_script_hidden(s, nullptr);
    CHECK(!s.objects[0].script_hidden);
    // Corpse roll through the template (a record with info.roll).
    struct RollCorpse {
        int32_t id;
        bool detached;
        struct {
            uint32_t object_type;
            int32_t owner;
            float position[3];
            float facing[3];
            float roll;
        } info;
        float alpha(uint64_t) const { return 1.0f; }
    };
    const std::vector<RollCorpse> rc = {{9, true, {0, 0, {0, 0, 0}, {1, 0, 0}, 0.75f}}};
    CHECK(render::snapshot_corpses(rc, 0)[0].roll == 0.75f);
    // Muzzles by bone index / own weapon (SimAnimation::Muzzle: id, bone, until).
    struct SimMuzzle {
        int32_t id;
        int32_t bone;
        uint64_t until;
    };
    const std::vector<SimMuzzle> sm = {{4, 2, 10}, {5, -1, 9}, {6, 1, 8}};
    const std::vector<render::SnapshotMuzzle> smz = render::snapshot_muzzles(sm, 8);
    CHECK(smz.size() == 2 && smz[0].bone_index == 2 && smz[1].bone_index == -1 && smz[1].bone.empty());
    f.objects = {sobj(4, 0)};
    f.objects[0].type = 2;
    f.muzzles = {smz[0]};
    f.tick = 10;
    b.build(f, models, main, bg, &st);
    CHECK(main.size() == 1 && main[0].mesh_hidden.size() >= 2 && main[0].mesh_hidden[1] == 0);
    f.muzzles = {{4, "", -1, -1}};  // own weapon: bones named Muzzle* (none on this model) -> nothing shown
    f.tick = 11;
    b.build(f, models, main, bg, &st);
    CHECK(st.muzzle_meshes == 0);
    f.muzzles.clear();
    // Foot plants from SimAnimation::FrameEvent-like records (SURFACE kind 1 only; foot from the bone name).
    struct EvDef {
        uint8_t kind;
        std::string name, bone;
    };
    struct Ev {
        int32_t id;
        const EvDef* def;
        Vec3 position;
    };
    const EvDef surf{1, "Generic_Footprint", "B_Foot_R"}, snd{0, "Unit_Fire", ""};
    const std::vector<Ev> evs = {{4, &surf, {1, 2, 3}}, {4, &snd, {0, 0, 0}}, {4, nullptr, {0, 0, 0}}};
    const std::vector<render::SnapshotFootPlant> fps = render::snapshot_foot_plants(evs);
    CHECK(fps.size() == 1 && fps[0].foot == 1 && fps[0].trigger == "Generic_Footprint" && fps[0].position.z == 3.0f);
    // Spin-away corpse motion: drifts along the death facing, yaws and rolls (render-side rates).
    render::SnapshotCorpse sc;
    sc.id = 40;
    sc.type = 2;
    sc.position = {100, 0, 0};
    sc.facing = {1, 0, 0};
    sc.spin_away = true;
    sc.spin_seconds = 2.0f;
    render::SimSnapshot ss;
    render::add_snapshot_corpses(ss, table, std::span<const render::SnapshotCorpse>(&sc, 1));
    if (!table[2].model.empty()) {
        CHECK(ss.objects.size() == 1 && (ss.objects[0].flags & render::kSnapSpinAway) &&
              std::fabs(ss.objects[0].position.x - (100.0f + 2.0f * render::kSpinAwayDrift)) < 1e-3f &&
              std::fabs(ss.objects[0].roll - 2.0f * render::kSpinAwayRollRate) < 1e-5f && std::fabs(ss.objects[0].facing.y) > 0.5f);
    }
    std::printf("stage 2 builder: OK\n");
}

static const char* kSurfaceXml = R"(<?xml version="1.0"?>
<SurfaceEffects>
  <SurfaceFX name="Generic_Track">
    <Surface_Settings name="default">
      <Dynamic_Track_Name>Track_A</Dynamic_Track_Name>
      <Dynamic_Track_Left_Bone>Tread_L</Dynamic_Track_Left_Bone>
      <Dynamic_Track_Right_Bone>Tread_R</Dynamic_Track_Right_Bone>
    </Surface_Settings>
    <Surface_Settings name="concrete"><Dynamic_Track_Name>NONE</Dynamic_Track_Name></Surface_Settings>
  </SurfaceFX>
  <SurfaceFX name="EXPLOSION_SCORCH_MARK_SMALL">
    <Surface_Settings name="default">
      <Decal_Name>SCORCH</Decal_Name>
      <Surface_Settings name="water"><Decal_Name>NONE</Decal_Name></Surface_Settings>
    </Surface_Settings>
  </SurfaceFX>
</SurfaceEffects>)";
static const char* kDecalXml = R"(<?xml version="1.0"?>
<TerrainDecals>
  <Decal Name="SCORCH">
    <Render_Mode>DIFFUSE_ALPHA</Render_Mode><Intensity>0.5</Intensity><Scale>20, 30</Scale>
    <UV_Slot>0,3,   1,3</UV_Slot><Random_Size_Offset>0.2</Random_Size_Offset><Fadeout_Time>10</Fadeout_Time>
    <Permanent>false</Permanent><Random_Rotate>Yes</Random_Rotate>
  </Decal>
  <Decal Name="Plain"><Scale>10</Scale><Slot_Scale>2</Slot_Scale></Decal>
</TerrainDecals>)";
static const char* kTrackXml = R"(<?xml version="1.0"?>
<DynamicTracks><DynamicTrack name="Track_A"><width>5</width><texture_name>w_tank_track_01.tga</texture_name>
<segment_length>7</segment_length><fade_begin_distance>200</fade_begin_distance><fade_end_distance>280</fade_end_distance>
<Render_Mode>DECAL_BUMP_ALPHA</Render_Mode><min_geometry_lod>0.25</min_geometry_lod></DynamicTrack></DynamicTracks>)";

// Flat 63 x 63 terrain (1240 units), material 0 = dirt, material 1 = concrete in the x > 620 half.
static terrain::Terrain flat_terrain() {
    terrain::Terrain t;
    t.present = true;
    t.width = t.height = 63;
    t.heights.assign(63 * 63, 0);
    t.material.assign(63 * 63, 0);
    for (uint32_t y = 0; y < 63; ++y)
        for (uint32_t x = 32; x < 63; ++x) t.material[y * 63 + x] = 1;
    t.materials[0].surface_type = 2;
    t.materials[1].surface_type = 5;
    return t;
}

static void test_surface_data(render::SurfaceFxLibrary& lib) {
    std::vector<render::SurfaceFxDef>& sfx = lib.surface_fx();
    CHECK(render::parse_surface_fx(bytes_of(kSurfaceXml), sfx) && sfx.size() == 2);
    CHECK(render::parse_terrain_decal_fx(bytes_of(kDecalXml), lib.decals()) && lib.decals().size() == 2);
    CHECK(terrain::parse_dynamic_track_fx(bytes_of(kTrackXml), lib.tracks()) && lib.tracks().size() == 1);
    const render::SurfaceFxDef* gt = lib.find_surface_fx("generic_track");
    CHECK(gt && gt->settings.size() == 2 && gt->find("DIRT") == &gt->settings[0] && gt->find("concrete")->dynamic_track.empty());
    CHECK(gt->settings[0].left_bone == "Tread_L" && gt->settings[0].dynamic_track == "Track_A");
    const render::SurfaceFxDef* sc = lib.find_surface_fx("EXPLOSION_SCORCH_MARK_SMALL");
    CHECK(sc && sc->settings.size() == 2 && sc->find("water")->decal.empty() && sc->find("grass")->decal == "SCORCH");  // nested block flattened
    const render::DecalFxDef* d = lib.find_decal("scorch");
    CHECK(d && d->mode == terrain::RenderMode::DiffuseAlpha && d->intensity == 0.5f && d->scale_w == 20.0f && d->scale_h == 30.0f);
    CHECK(d->uv_slots.size() == 2 && d->uv_slots[1][0] == 1 && d->uv_slots[1][1] == 3 && d->random_rotate && d->fadeout_time == 10.0f);
    const render::DecalFxDef* p = lib.find_decal("Plain");
    CHECK(p && p->scale_h == 10.0f && p->slot_scale == 2 && p->mode == terrain::RenderMode::Modulate && !lib.find_decal("NONE"));
    CHECK(render::is_none_name(" none ") && !render::is_none_name("x") && lib.track_textures().size() == 1);
    const terrain::Terrain t = flat_terrain();
    CHECK(render::terrain_surface_type(t, 100, 100) == 2 && render::terrain_surface_type(t, 1000, 100) == 5);
    CHECK(render::surface_type_name(2) == "dirt" && render::surface_type_name(5) == "concrete" && render::surface_type_name(40) == "default");
    terrain::Terrain wet = t;
    wet.header.water_type = terrain::WaterType::Lava;
    wet.header.water_height = 5.0f;
    CHECK(render::terrain_surface_type(wet, 100, 100) == 8);
}

static void test_runtime_decals(const render::SurfaceFxLibrary& lib) {
    const terrain::Terrain t = flat_terrain();
    const render::DecalFxDef& d = *lib.find_decal("SCORCH");
    render::RuntimeDecals a(7), b(7);
    CHECK(a.spawn(t, d, 300, 300, 100.0f, 0, 0) == render::DecalSpawn::HeightMismatch);  // 100 > 0.53 x max(w, h)
    CHECK(a.spawn(t, d, 300, 300, 0, 0, 0) == render::DecalSpawn::Spawned);
    CHECK(a.spawn(t, d, 302, 301, 0, 0, 0) == render::DecalSpawn::Spawned);
    CHECK(a.spawn(t, d, 301, 302, 0, 0, 0) == render::DecalSpawn::Covered);  // two live decals already cover it
    CHECK(a.spawn(t, d, 600, 300, 0, 0, 0) == render::DecalSpawn::Spawned);
    CHECK(a.size() == 3 && a.stats().rejected_covered == 1 && a.stats().rejected_height == 1);
    // Same seed, same calls -> identical records (render RNG only).
    b.spawn(t, d, 300, 300, 100.0f, 0, 0);
    b.spawn(t, d, 300, 300, 0, 0, 0);
    CHECK(b.size() == 1 && b.record(0).rotation == a.record(0).rotation && b.record(0).width == a.record(0).width);
    // Exactly Scale: Random_Size_Offset 0.2 is parsed but not applied (captured factor 1, DECORATIONS 4.2).
    CHECK(d.random_size_offset == 0.2f && a.record(0).width == 20.0f && a.record(0).height == 30.0f && a.record(1).width == 20.0f);
    CHECK(a.record(0).slot_row == 3 && a.record(0).render_mode == static_cast<uint32_t>(terrain::RenderMode::DiffuseAlpha));
    // Fade: intensity (L - age) / L, dead past L.
    a.update(5.0f);
    CHECK(std::fabs(a.alpha(0) - 0.25f) < 1e-5f && a.size() == 3);
    const std::vector<terrain::DecalGeometry> geo = a.geometry();
    CHECK(geo.size() == 3 && !geo[0].mesh.empty() && std::abs(int(geo[0].mesh.vertices[0].color[3]) - 64) <= 1);
    a.update(10.5f);
    CHECK(a.size() == 0 && a.stats().expired == 3);
    // Cap: oldest non-permanent dropped first.
    render::RuntimeDecals c(1);
    c.max_decals = 2;
    render::DecalFxDef perm = d;
    perm.permanent = true;
    perm.random_rotate = false;
    CHECK(c.spawn(t, perm, 100, 100, 0, 0, 0) == render::DecalSpawn::Spawned);
    CHECK(c.spawn(t, d, 200, 100, 0, 0, 0) == render::DecalSpawn::Spawned);
    CHECK(c.spawn(t, d, 300, 100, 0, 0, 0) == render::DecalSpawn::Spawned);
    CHECK(c.size() == 2 && c.record(0).permanent && c.record(1).position[0] == 300.0f && c.stats().evicted == 1);
    terrain::Terrain none;
    CHECK(c.spawn(none, d, 0, 0, 0, 0, 0) == render::DecalSpawn::NoTerrain);
}

static void test_sim_decorations(const render::SurfaceFxLibrary& lib) {
    const std::string dir = (fs::temp_directory_path() / "alamo_render_r5_tests").string();
    std::string err;
    const auto w = render::make_synthetic_sim_world(dir, &err);
    CHECK(w && w->types.size() >= 2);
    if (!w) return;
    render::SnapshotTypeTable table;
    table.build(w->types, objects::ModeKind::Land);
    table.set_decorations(0, {"Missing_Trigger", "Generic_Track"}, "");
    table.set_decorations(1, {}, "EXPLOSION_SCORCH_MARK_SMALL");
    CHECK(table.find(table[1].name) == 1 && table.find("NO_SUCH_TYPE_R5") == objects::kNone);
    const terrain::Terrain t = flat_terrain();
    render::SimDecorations deco(lib, table, &t, 11);
    render::InterpolatedFrame f;
    render::SnapshotObject tank = sobj(1, 100);
    tank.type = 0;
    tank.position = {100, 300, 0};
    for (uint64_t tick = 1; tick <= 60; ++tick) {
        f.tick = tick;
        tank.position.x = 100.0f + 4.0f * static_cast<float>(tick);  // 4 units / tick
        f.objects = {tank};
        if (tick >= 30) {
            render::SnapshotObject boom = sobj(2, 0);
            boom.type = 1;
            boom.position = {300, 500, 0};
            f.objects.push_back(boom);
        }
        deco.update(f);
        deco.update(f);  // same tick: no-op
    }
    CHECK(deco.stats().ticks == 60);
    CHECK(deco.generator_count() == 1 && deco.stats().track_objects == 1);  // no bone source: one centre track
    const std::vector<render::DynamicTrackMesh> tm = deco.track_meshes();
    CHECK(tm.size() == 1 && !tm[0].mesh.empty() && tm[0].texture == "w_tank_track_01.tga" && tm[0].mode == terrain::RenderMode::DecalBumpAlpha);
    CHECK(deco.stats().scorch_seen == 1 && deco.stats().scorch_spawned == 1 && deco.decals().size() == 1);  // only when it appears
    CHECK(deco.decal_geometry().size() == 1);
    // Onto concrete ("NONE"): the trail only fades.
    for (uint64_t tick = 61; tick <= 200; ++tick) {
        f.tick = tick;
        tank.position.x = 700.0f + static_cast<float>(tick);
        f.objects = {tank};
        deco.update(f);
    }
    CHECK(deco.generator_count() == 1 && deco.stats().faded_only == 1);
    // Bone source: two tracks.
    deco.set_bone_source([](const render::SnapshotObject& o, std::string_view bone, Vec3& out) {
        out = {o.position.x, o.position.y + (bone == "Tread_L" ? 3.0f : -3.0f), 0.0f};
        return true;
    });
    for (uint64_t tick = 201; tick <= 230; ++tick) {
        f.tick = tick;
        tank.position = {200.0f + 3.0f * static_cast<float>(tick - 200), 200.0f, 0.0f};
        f.objects = {tank};
        deco.update(f);
    }
    CHECK(deco.generator_count() == 3 && deco.track_meshes().size() >= 2);
    // Geometry detail below min_geometry_lod: no new tracks; the object leaves: generators dropped.
    f.tick = 231;
    f.objects.clear();
    deco.update(f);
    CHECK(deco.generator_count() == 0);
    render::SimDecorations space(lib, table, nullptr);
    f.tick = 1;
    f.objects = {tank};
    space.update(f);
    CHECK(space.track_meshes().empty() && space.decal_geometry().empty());
    std::printf("sim decorations: OK (decal clock %.1f s, %u live decals)\n", 231.0 / 30.0, deco.stats().live_decals);
}

// R5 stage 2: SurfaceFX wakes (energy gate) and foot plants (decal + puff, foot side, expiry).
static void test_sim_decorations_stage2(render::SurfaceFxLibrary& lib) {
    const char* xml = R"(<SurfaceEffects>
  <SurfaceFX name="Wake_Test">
    <Surface_Settings name="default"><Dynamic_Track_Name>NONE</Dynamic_Track_Name></Surface_Settings>
    <Surface_Settings name="water"><Particle_Name>P_WAKE</Particle_Name><Particle_Min_Energy>0.2</Particle_Min_Energy></Surface_Settings>
  </SurfaceFX>
  <SurfaceFX name="Foot_Test_Left"><Surface_Settings name="default"><Decal_Name>SCORCH</Decal_Name><Particle_Name>P_PUFF_L</Particle_Name></Surface_Settings></SurfaceFX>
  <SurfaceFX name="Foot_Test_Right"><Surface_Settings name="default"><Particle_Name>P_PUFF_R</Particle_Name></Surface_Settings></SurfaceFX>
</SurfaceEffects>)";
    CHECK(render::parse_surface_fx(bytes_of(xml), lib.surface_fx()));
    const std::string dir = (fs::temp_directory_path() / "alamo_render_r5_tests").string();
    std::string err;
    const auto w = render::make_synthetic_sim_world(dir, &err);
    if (!w || w->types.size() < 5) {
        CHECK(false);
        return;
    }
    render::SnapshotTypeTable table;
    table.build(w->types, objects::ModeKind::Land);
    table.set_decorations(3, {"Wake_Test"}, "");
    table.set_max_speed(3, 10.0f);
    table.set_decorations(4, {}, "");  // foot triggers come with the events (AnimationSFXMaps SURFACE rows)
    terrain::Terrain wet = flat_terrain();
    wet.header.water_type = terrain::WaterType::Water;
    wet.header.water_height = 5.0f;
    render::SimDecorations deco(lib, table, &wet, 3);
    render::InterpolatedFrame f;
    render::SnapshotObject boat = sobj(1, 0);
    boat.type = 3;
    render::SnapshotObject walker = sobj(2, 0);
    walker.type = 4;
    walker.position = {400, 400, 0};
    // Tick 1: first sight (no speed yet); tick 2: 1 unit / tick = 30 / s -> energy 3 >= 0.2: wake on.
    for (uint64_t tick = 1; tick <= 2; ++tick) {
        f.tick = tick;
        boat.position = {300.0f + static_cast<float>(tick), 300.0f, 0.0f};
        f.objects = {boat, walker};
        deco.update(f);
    }
    std::printf("sim decorations stage 2: %u surface particles, %zu effects\n", deco.stats().surface_particles, deco.effects().size());
    CHECK(deco.stats().surface_particles == 1 && deco.effects().size() == 1 && deco.effects()[0].owner == 1 &&
          deco.effects()[0].asset == "P_WAKE");
    // Still: energy 0 < 0.2: off.
    f.tick = 3;
    deco.update(f);
    CHECK(deco.stats().surface_particles == 0 && deco.effects().empty());
    // Foot plants: left -> SCORCH decal + P_PUFF_L at the foot; right -> P_PUFF_R only; unknown object ignored.
    f.tick = 4;
    f.foot_plants = {{2, 0, {410, 400, 0}, "Foot_Test_Left"}, {2, 1, {390, 400, 0}, "Foot_Test_Right"}, {77, 0, {0, 0, 0}, ""}};
    deco.update(f);
    CHECK(deco.stats().foot_plants == 3 && deco.stats().foot_puffs == 2 && deco.stats().foot_decals == 1 && deco.stats().foot_unmatched == 1);
    CHECK(deco.effects().size() == 2 && deco.effects()[0].owner == objects::kNoObjectId && deco.effects()[0].asset == "P_PUFF_L" &&
          deco.effects()[0].position.x == 410.0f && deco.effects()[1].asset == "P_PUFF_R");
    // An explicit trigger wins.
    f.tick = 5;
    f.foot_plants = {{2, 0, {420, 400, 0}, "Foot_Test_Right"}};
    deco.update(f);
    CHECK(deco.effects().size() == 3 && deco.effects()[2].asset == "P_PUFF_R");
    // Puffs expire after kFootEffectSeconds.
    f.foot_plants.clear();
    f.tick = 5 + static_cast<uint64_t>(render::kFootEffectSeconds * 30.0f) + 1;
    deco.update(f);
    CHECK(deco.effects().empty());
    // Spin-away explosion when the spinning corpse leaves (space: no terrain).
    table.set_spin_away_explosion(3, "P_BOOM");
    render::SimDecorations space(lib, table, nullptr);
    render::InterpolatedFrame sf;
    render::SnapshotObject spin = sobj(9, 0);
    spin.type = 3;
    spin.flags = render::kSnapSpinAway | render::kSnapCorpse;
    spin.position = {7, 8, 9};
    sf.tick = 1;
    sf.objects = {spin};
    space.update(sf);
    CHECK(space.effects().empty());
    sf.tick = 2;
    sf.objects.clear();
    space.update(sf);
    CHECK(space.effects().size() == 1 && space.effects()[0].asset == "P_BOOM" && space.effects()[0].position.z == 9.0f &&
          space.stats().spin_explosions == 1);
    std::printf("sim decorations stage 2: OK\n");
}

static void test_real_surface_data() {
    const std::string root = env_or_empty("ALAMO_FOC_DATA");
    if (root.empty()) {
        std::printf("real surface data: ALAMO_FOC_DATA not set, skipped\n");
        return;
    }
    vfs::VfsConfig cfg;
    cfg.foc_root = root;
    std::string err;
    auto files = vfs::Vfs::create(cfg, &err);
    CHECK(files != nullptr);
    if (!files) return;
    render::SurfaceFxLibrary lib;
    CHECK(lib.load(*files));
    std::printf("real surface data: %zu SurfaceFX, %zu TerrainDecalFX, %zu DynamicTrackFX\n", lib.surface_fx().size(), lib.decals().size(),
                lib.tracks().size());
    CHECK(lib.surface_fx().size() >= 30 && lib.decals().size() >= 18 && lib.tracks().size() >= 11);
    // Every decal / track a SurfaceFX names resolves (NONE excluded).
    uint32_t refs = 0, missing = 0;
    for (const render::SurfaceFxDef& d : lib.surface_fx())
        for (const render::SurfaceSettings& s : d.settings) {
            if (!s.decal.empty()) {
                ++refs;
                missing += lib.find_decal(s.decal) ? 0u : 1u;
            }
            if (!s.dynamic_track.empty()) {
                ++refs;
                missing += lib.find_track(s.dynamic_track) ? 0u : 1u;
            }
        }
    std::printf("real surface data: %u decal / track references, %u unresolved\n", refs, missing);
    CHECK(refs > 20 && missing == 0);
    const render::SurfaceFxDef* large = lib.find_surface_fx("EXPLOSION_SCORCH_MARK_LARGE");
    CHECK(large && large->find("dirt") && large->find("dirt")->decal == "EXPLOSION_SCORCH_LARGE" && large->find("water") &&
          large->find("water")->decal.empty());
    const render::SurfaceFxDef* gt = lib.find_surface_fx("Generic_Track");
    CHECK(gt && gt->find("grass")->dynamic_track == "Track_Bump" && gt->find("grass")->left_bone == "Tread_L");
    const render::DecalFxDef* massive = lib.find_decal("EXPLOSION_SCORCH_MASSIVE");
    CHECK(massive && massive->slot_scale == 2 && massive->mode == terrain::RenderMode::DecalBumpAlpha && massive->scale_w == 450.0f);
}

// ------------------------------------------------------------------------------------------- GPU
struct Gpu {
    rhi::Context ctx;
    rhi::Allocator alloc;
    rhi::Uploader up;
    rhi::FrameRing ring;
};

struct Offscreen {
    rhi::Image color, depth;
    rhi::Buffer readback;
    uint32_t w = 0, h = 0;
    bool create(Gpu& g, uint32_t width, uint32_t height) {
        w = width;
        h = height;
        rhi::ImageDesc cd;
        cd.format = rhi::Format::RGBA8;
        cd.width = w;
        cd.height = h;
        cd.mips = 1;
        cd.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        rhi::ImageDesc dd = cd;
        dd.format = rhi::Format::D32F;
        dd.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        return g.alloc.create_image(cd, color) && g.alloc.create_image(dd, depth) &&
               g.alloc.create_buffer(static_cast<VkDeviceSize>(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::MemoryUsage::Readback,
                                     readback);
    }
    void destroy(Gpu& g) {
        g.alloc.destroy(color);
        g.alloc.destroy(depth);
        g.alloc.destroy(readback);
    }
    std::vector<uint8_t> pixels() const {
        const auto* p = static_cast<const uint8_t*>(readback.mapped());
        return std::vector<uint8_t>(p, p + static_cast<size_t>(w) * h * 4);
    }
};

static bool render_frame(Gpu& g, render::Renderer& r, Offscreen& o, const render::ViewMatrices& vm, std::span<const render::Instance> inst,
                         render::GpuModel* resident, render::DrawStats& stats, render::DrawMode mode, const render::RecordOptions& ro,
                         std::span<const render::Instance> sky = {}, const float* clear = nullptr,
                         const std::function<void()>& mid_frame = {}) {
    rhi::Frame* f = g.ring.begin_frame();
    if (!f) return false;
    VkCommandBuffer cmd = f->cmd();
    const rhi::DeviceFns& fn = g.ctx.vkd();
    const rhi::AcquireResult acq = g.up.record_acquires(cmd);
    if (mid_frame) mid_frame();  // integration fixes: work done after begin_frame + this frame's acquires

    if (resident) r.make_resident(*resident);
    render::PassTarget pt;
    pt.color = o.color.handle;
    pt.color_view = o.color.view;
    pt.depth = o.depth.handle;
    pt.depth_view = o.depth.view;
    pt.extent = {o.w, o.h};
    pt.clear[0] = 0.0f;
    pt.clear[1] = 0.0f;
    pt.clear[2] = 0.25f;
    pt.clear[3] = 1.0f;
    if (clear) std::memcpy(pt.clear, clear, sizeof pt.clear);
    pt.color_after = rhi::ImageState::TransferSrc;
    const bool drawn = r.render(*f, cmd, pt, vm, inst, mode, ro, &stats, nullptr, sky);
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

// Two unit boxes: mesh BOX_ALT0 at x = -1.5, mesh BOX_ALT1 at x = +1.5.
static std::shared_ptr<assets::ModelAsset> alt_boxes() {
    auto m = render::debug_box_model("R5_ALT_BOXES.ALO", 1.0f);
    assets::Submesh second = m->submeshes[0];
    for (int pass = 0; pass < 2; ++pass) {
        assets::Submesh& sm = pass == 0 ? m->submeshes[0] : second;
        const float dx = pass == 0 ? -1.5f : 1.5f;
        for (uint32_t v = 0; v < sm.vertex_count; ++v) {
            assets::StaticVertex st{};
            std::memcpy(&st, sm.vertices.data() + static_cast<size_t>(v) * 64, sizeof st);
            st.position[0] += dx;
            std::memcpy(sm.vertices.data() + static_cast<size_t>(v) * 64, &st, sizeof st);
        }
        sm.bounds = {{dx - 1, -1, -1}, {dx + 1, 1, 1}};
    }
    second.mesh = 1;
    m->submeshes.push_back(second);
    m->meshes[0].name = "BOX_ALT0";
    assets::MeshInfo mi = m->meshes[0];
    mi.name = "BOX_ALT1";
    mi.first_submesh = 1;
    m->meshes.push_back(mi);
    m->bounds = {{-2.5f, -1, -1}, {2.5f, 1, 1}};
    m->sphere = {{0, 0, 0}, 3.0f};
    return m;
}

static double luma(const uint8_t* p) { return 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]; }

// Mean luma of a w x h image region.
static double region_luma(const std::vector<uint8_t>& px, uint32_t w, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
    double s = 0;
    for (uint32_t y = y0; y < y1; ++y)
        for (uint32_t x = x0; x < x1; ++x) s += luma(&px[(static_cast<size_t>(y) * w + x) * 4]);
    return s / static_cast<double>((x1 - x0) * (y1 - y0));
}

static void write_png(const std::string& path, uint32_t w, uint32_t h, std::vector<uint8_t> rgba) {
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    CHECK(render::write_png_rgba(path, w, h, rgba));
    std::printf("  wrote %s\n", path.c_str());
}

static void test_gpu_alt(Gpu& g, render::Renderer& r) {
    Offscreen o;
    CHECK(o.create(g, 256, 128));
    render::GpuModel model;
    std::string err;
    const bool up = r.upload_model(alt_boxes(), [](std::string_view, assets::TextureRole) { return std::shared_ptr<const assets::TextureAsset>(); },
                                   model, nullptr, &err);
    CHECK(up && model.submeshes.size() == 2);
    if (up) {
        CHECK(model.submeshes[0].alt == 0 && model.submeshes[1].alt == 1);
        const assets::Aabb db = model.drawn_bounds();
        CHECK(db.valid() && db.max.x < 0.0f);  // the default look is alt 0 only
        render::OrbitCamera cam;
        cam.target = {0, 0, 0};
        cam.yaw = -1.5707963f;
        cam.pitch = 0.0f;
        cam.distance = 12.0f;
        cam.fov_x = 0.9f;
        const render::ViewMatrices vm = render::make_view(cam, o.w, o.h, 1.0f, 100.0f);
        render::Instance in;
        in.model = &model;
        render::RecordOptions ro;
        ro.post = false;
        ro.shadows = false;
        ro.frustum_cull = false;
        double left[2] = {0, 0}, right[2] = {0, 0};
        for (int alt = 0; alt < 2; ++alt) {
            in.alt = alt;
            render::DrawStats st;
            // Integration fixes (flake seen under full ctest: the first frame ran before the model's upload was resident,
            // nothing drawn): redraw until the model is resident and drawn with compiled pipelines.
            for (int k = 0; k < 200; ++k) {
                CHECK(render_frame(g, r, o, vm, std::span<const render::Instance>(&in, 1), &model, st, render::DrawMode::Unlit, ro));
                if (model.resident() && st.draws > 0 && st.pending_pipeline == 0 && st.translation_pending == 0) break;
                r.wait_translations();
                r.pipelines().wait_idle();
            }
            const std::vector<uint8_t> px = o.pixels();
            left[alt] = region_luma(px, o.w, 0, 0, o.w / 2, o.h);
            right[alt] = region_luma(px, o.w, o.w / 2, 0, o.w, o.h);
        }
        std::printf("gpu alt: alt 0 -> left %.1f / right %.1f, alt 1 -> left %.1f / right %.1f\n", left[0], right[0], left[1], right[1]);
        // Each alternate shows only its own box: one screen half lit, the other half lit for the other alt.
        const double d0 = left[0] - right[0], d1 = left[1] - right[1];
        CHECK(std::fabs(d0) > 20.0 && std::fabs(d1) > 20.0 && d0 * d1 < 0.0);
    }
    g.ring.wait_idle();
    if (up) r.destroy_model(model);
    o.destroy(g);
}

static void test_gpu_real(Gpu& g, const std::string& out_dir) {
    const std::string root = env_or_empty("ALAMO_FOC_DATA");
    if (root.empty()) {
        std::printf("gpu real data: ALAMO_FOC_DATA not set, skipped\n");
        return;
    }
    vfs::VfsConfig cfg;
    cfg.foc_root = root;
    std::string err;
    auto files = vfs::Vfs::create(cfg, &err);
    CHECK(files != nullptr);
    if (!files) return;
    render::EffectCache effects(files.get());
    assets::AssetCache cache(*files, effects.model_load_options());
    jobs::JobSystem js;
    render::Renderer rr;
    render::RendererDesc rd;
    rd.color_format = VK_FORMAT_R8G8B8A8_UNORM;
    rd.effects = &effects;
    rd.js = &js;
    CHECK(rr.init(g.ctx, g.alloc, g.up, rd));
    rr.set_frame_ring(&g.ring);
    render::SurfaceFxLibrary lib;
    CHECK(lib.load(*files));
    render::FogColors colors;
    {
        std::vector<uint8_t> xml;
        if (files->read("Data\\XML\\GameConstants.xml", xml)) CHECK(render::parse_fog_colors(xml, colors));
    }
    CHECK(colors.land_fog == terrain::kLandFowColor);
    Offscreen o;
    CHECK(o.create(g, 960, 540));
    {
        render::ModelStreamer streamer(rr, cache, js);
        render::MapScene scene;
        scene.set_objects_enabled(false);
        scene.set_runtime_decals_enabled(true);
        CHECK(scene.load(*files, "_MP_LAND_ENDOR", nullptr, serial_for, &err));
        CHECK(scene.start(rr, streamer, cache, {}, &err));
        const render::OrbitCamera cam = scene.tactical_camera(960.0f / 540.0f);
        const render::ViewMatrices vm = render::make_view(cam, o.w, o.h, render::MapScene::kNear, render::MapScene::kFar);
        render::RecordOptions ro;
        scene.apply(ro);
        const render::Float3 cc = scene.clear_color();
        const float clear[4] = {cc.x, cc.y, cc.z, 1.0f};
        // Sim fog: one revealing unit at the camera target (land grid over the playable box, 15-unit cells).
        render::FogRenderer fog_r;
        fog_r.set_colors(colors);
        std::shared_ptr<const render::FogFrameInput> fin;
#ifdef ALAMO_R5_HAVE_FOG
        {
            float mn[2], mx[2];
            CHECK(scene.map().playable_box(mn, mx));
            tactical::fog::FogOfWar fog;
            tactical::fog::FogSetup setup;
            setup.mode = tactical::fog::FogMode::Land;
            setup.box = {mn[0], mn[1], mx[0], mx[1]};
            setup.players = {{0, true, true}, {1, true, false}};
            fog.start(setup);
            tactical::fog::FogObject unit;
            unit.owner = 0;
            unit.x = cam.target.x;
            unit.y = cam.target.y;
            unit.sight_range = 180.0f;
            for (uint32_t frame = 1; frame <= 32; ++frame) {
                fog.decay_step(frame);
                fog.source_service(1, unit);
            }
            fin = render::fog_frame_input(fog.render_view(0));
            CHECK(fin && fin->usable());
        }
#else
        {
            const terrain::FowGridDesc d = terrain::make_fow_grid(scene.map());
            auto f = std::make_shared<render::FogFrameInput>();
            f->valid = true;
            f->width = d.width;
            f->height = d.height;
            f->x0 = d.x0;
            f->y0 = d.y0;
            f->x1 = d.x1;
            f->y1 = d.y1;
            f->cell_w = d.cell_x;
            f->cell_h = d.cell_y;
            f->state.assign(f->cells(), 0);
            f->fade.assign(f->cells(), 0);
            for (uint32_t cy = 0; cy < d.height; ++cy)
                for (uint32_t cx = 0; cx < d.width; ++cx) {
                    const std::array<float, 2> c = d.cell_center(cx, cy);
                    if (std::hypot(c[0] - cam.target.x, c[1] - cam.target.y) < 180.0f) f->state[cy * d.width + cx] = 255;
                }
            fin = f;
        }
#endif
        // Decorations: a scorch mark next to the target, a tank driving past it.
        std::string dir = (fs::temp_directory_path() / "alamo_render_r5_tests").string();
        const auto w = render::make_synthetic_sim_world(dir, &err);
        CHECK(w != nullptr);
        render::SnapshotTypeTable table;
        if (w) table.build(w->types, objects::ModeKind::Land);
        if (table.size() >= 2) {
            table.set_decorations(0, {"Generic_Track"}, "");
            table.set_decorations(1, {}, "EXPLOSION_SCORCH_MARK_LARGE");
        }
        render::SimDecorations deco(lib, table, &scene.map().terrain, 5);
        render::InterpolatedFrame f;
        for (uint64_t tick = 1; tick <= 90 && table.size() >= 2; ++tick) {
            f.tick = tick;
            render::SnapshotObject tank = sobj(1, 0);
            tank.type = 0;
            tank.position = {cam.target.x - 150.0f + 3.0f * static_cast<float>(tick), cam.target.y - 60.0f, cam.target.z};
            f.objects = {tank};
            if (tick >= 10) {
                render::SnapshotObject boom = sobj(2, 0);
                boom.type = 1;
                boom.position = {cam.target.x + 40.0f, cam.target.y + 20.0f,
                                 scene.map().terrain.sample_height(cam.target.x + 40.0f, cam.target.y + 20.0f)};
                f.objects.push_back(boom);
            }
            deco.update(f);
        }
        std::printf("gpu real: decorations: %u generators, %llu scorch spawned, %u live decals, %zu track meshes\n",
                    static_cast<uint32_t>(deco.generator_count()), static_cast<unsigned long long>(deco.stats().scorch_spawned),
                    deco.stats().live_decals, deco.track_meshes().size());
        CHECK(deco.stats().scorch_spawned == 1 && deco.track_meshes().size() == 1);
        // Settle (models / pipelines / translations), fog + decorations on.
        render::DrawStats st;
        bool done = false;
        for (int k = 0; k < 3200 && !done; ++k) {
            streamer.update();
            scene.update(rr, streamer);
            if (fin) fog_r.update(scene.world(), *fin, scene.space());
            deco.apply(scene.world());
            CHECK(render_frame(g, rr, o, vm, scene.instances(), nullptr, st, render::DrawMode::Lit, ro, scene.sky(), clear));
            const render::WorldStats& ws = scene.world().stats();
            done = scene.ready() && k > 2 && st.pending_pipeline == 0 && st.translation_pending == 0 && st.post_pending == 0 &&
                   ws.terrain_pending == 0 && ws.water_pending == 0 && ws.decorations.pending == 0 && ws.decorations.fow_draws > 0;
            if (!done && scene.ready()) {
                rr.wait_translations();
                rr.pipelines().wait_idle();
            }
        }
        CHECK(done);
        CHECK(fog_r.active() && fog_r.stats().uploads == 1 && fog_r.stats().skipped_same > 0);
        const render::DecorationStats dst = scene.world().stats().decorations;
        std::printf("gpu real: fog %ux%u (%u clear / %u fogged cells), %u FOW patches, %u decals, %u dynamic tracks drawn\n", fog_r.grid().width(),
                    fog_r.grid().height(), fog_r.stats().clear_cells, fog_r.stats().fogged_cells, dst.fow_draws, dst.decals, dst.dynamic_tracks);
        CHECK(dst.decals >= 1 && dst.dynamic_tracks >= 1);
        const std::vector<uint8_t> fog_on = o.pixels();
        write_png(out_dir + "/r5_endor_fog.png", o.w, o.h, fog_on);
        // Fog off (FOW-active / render option off): same frame without the multiply.
        render::FogFrameInput off = *fin;
        off.render_option = false;
        fog_r.update(scene.world(), off, scene.space());
        CHECK(!fog_r.active() && !scene.world().fow_enabled());
        CHECK(render_frame(g, rr, o, vm, scene.instances(), nullptr, st, render::DrawMode::Lit, ro, scene.sky(), clear));
        const std::vector<uint8_t> fog_off = o.pixels();
        write_png(out_dir + "/r5_endor_nofog.png", o.w, o.h, fog_off);
        // Centre (revealed) unchanged within the blur, top band (far, fogged) darkened towards 130/255.
        const uint32_t cx0 = o.w / 2 - 30, cy0 = o.h / 2 - 30;
        const double c_on = region_luma(fog_on, o.w, cx0, cy0, cx0 + 60, cy0 + 60), c_off = region_luma(fog_off, o.w, cx0, cy0, cx0 + 60, cy0 + 60);
        const double t_on = region_luma(fog_on, o.w, 0, 0, o.w, 40), t_off = region_luma(fog_off, o.w, 0, 0, o.w, 40);
        std::printf("gpu real: centre luma fog %.1f / off %.1f; top band fog %.1f / off %.1f (ratio %.3f)\n", c_on, c_off, t_on, t_off,
                    t_off > 0 ? t_on / t_off : 0.0);
        CHECK(c_off > 5.0 && std::fabs(c_on / c_off - 1.0) < 0.08);
        CHECK(t_off > 5.0 && t_on / t_off < 0.75 && t_on / t_off > 0.35);
        // Decorations off vs on (fog off): the scorch decal and the track change pixels.
        scene.world().set_runtime_decals({});
        scene.world().set_dynamic_tracks({});
        CHECK(render_frame(g, rr, o, vm, scene.instances(), nullptr, st, render::DrawMode::Lit, ro, scene.sky(), clear));
        const std::vector<uint8_t> plain = o.pixels();
        size_t diff = 0;
        for (size_t i = 0; i < plain.size(); i += 4)
            diff += (std::abs(int(plain[i]) - int(fog_off[i])) + std::abs(int(plain[i + 1]) - int(fog_off[i + 1])) +
                     std::abs(int(plain[i + 2]) - int(fog_off[i + 2]))) > 12
                        ? 1u
                        : 0u;
        std::printf("gpu real: %zu pixels changed by the runtime decal + track\n", diff);
        CHECK(diff > 500);
        deco.apply(scene.world());
        CHECK(render_frame(g, rr, o, vm, scene.instances(), nullptr, st, render::DrawMode::Lit, ro, scene.sky(), clear));
        write_png(out_dir + "/r5_endor_decals.png", o.w, o.h, o.pixels());
        // Integration fixes: WorldRenderer::set_fog_of_war at any point of a frame. A grid set before the frame gets
        // its copy / acquire recorded by this frame's record_acquires; a second call after that (mid-frame)
        // supersedes the upload, whose image must survive until the frame completed (renderer deferred release).
        {
            g.ring.wait_idle();
            const uint32_t e0 = g.ctx.validation_errors(), w0 = g.ctx.validation_warnings();
            const terrain::FowGrid grid = fog_r.grid();
            uint32_t mid_ok = 0;
            for (int k = 0; k < 6; ++k) {
                if (k % 2 == 0) CHECK(scene.world().set_fog_of_war(grid));  // pending upload into this frame's acquires
                CHECK(render_frame(g, rr, o, vm, scene.instances(), nullptr, st, render::DrawMode::Lit, ro, scene.sky(), clear,
                                   [&] { mid_ok += scene.world().set_fog_of_war(grid) ? 1u : 0u; }));
            }
            for (int k = 0; k < 120 && scene.world().fow_upload_pending(); ++k)  // the last upload lands (transfer timing varies)
                CHECK(render_frame(g, rr, o, vm, scene.instances(), nullptr, st, render::DrawMode::Lit, ro, scene.sky(), clear));
            g.ring.wait_idle();
            rr.collect_released();
            const uint32_t de = g.ctx.validation_errors() - e0, dw = g.ctx.validation_warnings() - w0;
            std::printf("gpu real: set_fog_of_war mid-frame x%u: %u validation errors, %u warnings; fog %s, upload %s\n", mid_ok, de, dw,
                        scene.world().fow_enabled() ? "on" : "off", scene.world().fow_upload_pending() ? "pending" : "resident");
            CHECK(mid_ok == 6 && de == 0 && dw == 0 && scene.world().fow_enabled() && !scene.world().fow_upload_pending());
        }
        g.ring.wait_idle();
        scene.stop(rr);
        streamer.destroy_all();
    }
    g.ring.wait_idle();
    o.destroy(g);
    rr.shutdown();
}

// R3 / R5 stage 2 on real data: muzzle flash + last-seen ghost on _MP_LAND_ENDOR (RV_T2BTANK.ALO), the space
// reinforcement overlay on _MP_SPACE_ENDOR, T6 scene-model poses (EI_TROOPER idle).
static void test_gpu_real_stage2(Gpu& g, const std::string& out_dir) {
    const std::string root = env_or_empty("ALAMO_FOC_DATA");
    if (root.empty()) {
        std::printf("gpu real stage 2: ALAMO_FOC_DATA not set, skipped\n");
        return;
    }
    vfs::VfsConfig cfg;
    cfg.foc_root = root;
    std::string err;
    auto files = vfs::Vfs::create(cfg, &err);
    CHECK(files != nullptr);
    if (!files) return;
    render::EffectCache effects(files.get());
    assets::AssetCache cache(*files, effects.model_load_options());
    jobs::JobSystem js;
    render::Renderer rr;
    render::RendererDesc rd;
    rd.color_format = VK_FORMAT_R8G8B8A8_UNORM;
    rd.effects = &effects;
    rd.js = &js;
    CHECK(rr.init(g.ctx, g.alloc, g.up, rd));
    rr.set_frame_ring(&g.ring);
    // T6 scene models: a real idle clip poses and moves with the tick.
    {
        render::SceneModelAnimator anim(files.get());
        render::GpuModel m;
        m.uid = 777;
        m.asset = cache.model("EI_TROOPER.ALO");
        CHECK(m.asset != nullptr);
        if (m.asset) {
            const std::vector<render::Mat3x4> p0(anim.pose(m, 0).begin(), anim.pose(m, 0).end());
            const std::span<const render::Mat3x4> p1 = anim.pose(m, 20);
            CHECK(p0.size() == m.asset->skeleton.size() && p1.size() == p0.size() && anim.animated() == 1);
            CHECK(!p0.empty() && std::memcmp(p0.data(), p1.data(), p0.size() * sizeof(render::Mat3x4)) != 0);
            std::printf("gpu real stage 2: EI_TROOPER idle pose %zu bones, moves between ticks 0 and 20\n", p0.size());
        }
    }
    const std::string dir = (fs::temp_directory_path() / "alamo_render_r5_tests").string();
    const auto w = render::make_synthetic_sim_world(dir, &err);
    CHECK(w != nullptr);
    if (!w) return;
    render::SnapshotTypeTable table;
    table.build(w->types, objects::ModeKind::Land);
    const objects::TypeIndex marker = table.find("Sim_Marker");
    CHECK(marker != objects::kNone);
    if (marker == objects::kNone) return;
    table.set_ghosts(marker, true, false);
    render::GpuModel tank;
    const bool up = rr.upload_model(cache.model("RV_T2BTANK.ALO"),
                                    [&cache](std::string_view n, assets::TextureRole role) { return cache.texture(n, role); }, tank, &js, &err);
    CHECK(up);
    Offscreen o;
    CHECK(o.create(g, 960, 540));
    auto settle = [&](render::MapScene& scene, render::ModelStreamer& streamer, const render::ViewMatrices& vm, const render::RecordOptions& ro,
                      const float* clear, const std::function<std::vector<render::Instance>()>& insts, bool space) {
        render::DrawStats st;
        bool done = false;
        for (int k = 0; k < 3200 && !done; ++k) {
            streamer.update();
            scene.update(rr, streamer);
            const std::vector<render::Instance> in = insts();
            CHECK(render_frame(g, rr, o, vm, in, up ? &tank : nullptr, st, render::DrawMode::Lit, ro, scene.sky(), clear));
            const render::WorldStats& ws = scene.world().stats();
            done = scene.ready() && k > 2 && st.pending_pipeline == 0 && st.translation_pending == 0 && st.post_pending == 0 &&
                   ws.terrain_pending == 0 && ws.water_pending == 0 && ws.decorations.pending == 0 &&
                   (space ? ws.decorations.space_fow_draws > 0 : ws.decorations.fow_draws > 0) && (!up || tank.resident());
            if (!done && scene.ready()) {
                rr.wait_translations();
                rr.pipelines().wait_idle();
            }
        }
        CHECK(done);
        return o.pixels();
    };
    auto count_diff = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
        size_t d = 0;
        for (size_t i = 0; i + 3 < a.size() && i + 3 < b.size(); i += 4)
            d += (std::abs(int(a[i]) - int(b[i])) + std::abs(int(a[i + 1]) - int(b[i + 1])) + std::abs(int(a[i + 2]) - int(b[i + 2]))) > 12 ? 1u : 0u;
        return d;
    };
    // ---- land: muzzle + ghost
    if (up) {
        render::ModelStreamer streamer(rr, cache, js);
        render::MapScene scene;
        scene.set_objects_enabled(false);
        CHECK(scene.load(*files, "_MP_LAND_ENDOR", nullptr, serial_for, &err));
        CHECK(scene.start(rr, streamer, cache, {}, &err));
        render::OrbitCamera cam = scene.tactical_camera(960.0f / 540.0f);
        cam.distance = cam.radius = 260.0f;
        const render::ViewMatrices vm = render::make_view(cam, o.w, o.h, render::MapScene::kNear, render::MapScene::kFar);
        render::RecordOptions ro;
        scene.apply(ro);
        const render::Float3 cc = scene.clear_color();
        const float clear[4] = {cc.x, cc.y, cc.z, 1.0f};
        // Fog: only a disc around the camera target is visible (state 255), the rest fogged (state 0).
        const terrain::FowGridDesc d = terrain::make_fow_grid(scene.map());
        auto fog_all = std::make_shared<render::FogFrameInput>();
        fog_all->valid = true;
        fog_all->width = d.width;
        fog_all->height = d.height;
        fog_all->x0 = d.x0;
        fog_all->y0 = d.y0;
        fog_all->x1 = d.x1;
        fog_all->y1 = d.y1;
        fog_all->cell_w = d.cell_x;
        fog_all->cell_h = d.cell_y;
        fog_all->state.assign(fog_all->cells(), 255);
        fog_all->fade.assign(fog_all->cells(), 0);
        fog_all->frame = 100;
        fog_all->last_decay = 80;
        auto fog_disc = std::make_shared<render::FogFrameInput>(*fog_all);
        for (uint32_t cy = 0; cy < d.height; ++cy)
            for (uint32_t cx = 0; cx < d.width; ++cx) {
                const std::array<float, 2> c = d.cell_center(cx, cy);
                if (std::hypot(c[0] - cam.target.x, c[1] - cam.target.y) > 70.0f) fog_disc->state[cy * d.width + cx] = 0;
            }
        render::FogRenderer fog_r;
        FakeModels models;
        models.model = &tank;
        render::SimInstanceBuilder builder(table);
        builder.set_fog_smooth_time(0.0f);
        auto ground = [&](float x, float y) { return Vec3{x, y, scene.map().terrain.sample_height(x, y)}; };
        render::SnapshotObject u_near = sobj(1, 0), u_far = sobj(2, 0);
        u_near.type = u_far.type = marker;
        u_near.position = ground(cam.target.x - 25.0f, cam.target.y + 10.0f);
        u_near.facing = {0, 1, 0};
        u_far.position = ground(cam.target.x + 95.0f, cam.target.y + 40.0f);
        u_far.facing = {-1, 0, 0};
        render::InterpolatedFrame f;
        f.fog = fog_all;
        f.objects = {u_near, u_far};
        f.tick = 1;
        std::vector<render::Instance> main, bg;
        builder.build(f, models, main, bg);  // both seen: last state recorded
        // Now `far` is fogged (hidden) and moves on; `near` fires MuzzleA_00.
        f.fog = fog_disc;
        f.objects[1].flags = render::kSnapFogHidden;
        f.objects[1].position = ground(cam.target.x + 400.0f, cam.target.y + 400.0f);
        f.tick = 2;
        bool muzzle = true, ghosts = true;
        auto insts = [&] {
            f.muzzles.clear();
            if (muzzle) f.muzzles.push_back({1, "MuzzleA_00", -1});
            builder.set_ghosts_enabled(ghosts);
            ++f.tick;
            builder.build(f, models, main, bg);
            return main;
        };
        fog_r.update(scene.world(), *fog_disc, false);
        const std::vector<uint8_t> shot = settle(scene, streamer, vm, ro, clear, insts, false);
        render::SimInstanceStats ist;
        builder.build(f, models, main, bg, &ist);
        std::printf("gpu real stage 2: builder %u drawn, %u ghosts, %u muzzle sub-objects\n", ist.drawn, ist.ghosts, ist.muzzle_meshes);
        CHECK(ist.ghosts == 1 && ist.muzzle_meshes >= 1 && builder.has_ghost(2));
        write_png(out_dir + "/r5_endor_ghost_muzzle.png", o.w, o.h, shot);
        render::DrawStats st;
        CHECK(render_frame(g, rr, o, vm, insts(), &tank, st, render::DrawMode::Lit, ro, scene.sky(), clear));
        std::printf("gpu real stage 2: %u instances, %u culled, %u draws (%u lit), tank at (%.0f, %.0f, %.0f), %zu submeshes\n", st.instances,
                    st.culled, st.draws, st.lit_draws, u_near.position.x, u_near.position.y, u_near.position.z, tank.submeshes.size());
        std::printf("gpu real stage 2: tank bounds (%.1f %.1f %.1f)-(%.1f %.1f %.1f), camera target (%.0f, %.0f, %.0f) distance %.0f\n",
                    tank.bounds.min.x, tank.bounds.min.y, tank.bounds.min.z, tank.bounds.max.x, tank.bounds.max.y, tank.bounds.max.z,
                    cam.target.x, cam.target.y, cam.target.z, cam.distance);
        muzzle = false;
        CHECK(render_frame(g, rr, o, vm, insts(), &tank, st, render::DrawMode::Lit, ro, scene.sky(), clear));
        const size_t dm = count_diff(shot, o.pixels());
        muzzle = true;
        ghosts = false;
        CHECK(render_frame(g, rr, o, vm, insts(), &tank, st, render::DrawMode::Lit, ro, scene.sky(), clear));
        const size_t dg = count_diff(shot, o.pixels());
        std::printf("gpu real stage 2: %zu pixels from the muzzle flash, %zu from the ghost\n", dm, dg);
        CHECK(dm > 20 && dg > 200);
        g.ring.wait_idle();
        scene.stop(rr);
        streamer.destroy_all();
    }
    // ---- space: reinforcement overlay
    {
        render::ModelStreamer streamer(rr, cache, js);
        render::MapScene scene;
        scene.set_objects_enabled(false);
        CHECK(scene.load(*files, "_MP_SPACE_ENDOR", nullptr, serial_for, &err));
        CHECK(scene.start(rr, streamer, cache, {}, &err));
        const render::OrbitCamera cam = scene.tactical_camera(960.0f / 540.0f);
        const render::ViewMatrices vm = render::make_view(cam, o.w, o.h, render::MapScene::kNear, render::MapScene::kFar);
        render::RecordOptions ro;
        scene.apply(ro);
        const float clear[4] = {0, 0, 0, 1};
        const terrain::FowGridDesc d = terrain::make_fow_grid(scene.map());
        render::FogFrameInput fin;
        fin.valid = true;
        fin.width = d.width;
        fin.height = d.height;
        fin.x0 = d.x0;
        fin.y0 = d.y0;
        fin.x1 = d.x1;
        fin.y1 = d.y1;
        fin.cell_w = d.cell_x;
        fin.cell_h = d.cell_y;
        fin.state.assign(fin.cells(), 0);
        fin.fade.assign(fin.cells(), 0);
        for (uint32_t cy = 0; cy < d.height; ++cy)
            for (uint32_t cx = 0; cx < d.width / 2; ++cx) fin.state[cy * d.width + cx] = 255;  // west half visible
        render::FogRenderer fog_r;
        fog_r.update(scene.world(), fin, true);
        auto none = [] { return std::vector<render::Instance>(); };
        const std::vector<uint8_t> plain = settle(scene, streamer, vm, ro, clear, none, true);
        write_png(out_dir + "/r5_space_fow.png", o.w, o.h, plain);
        fog_r.set_space_reinforce(true);
        CHECK(fog_r.update(scene.world(), fin, true) && scene.world().space_fow_reinforce());
        const std::vector<uint8_t> reinf = settle(scene, streamer, vm, ro, clear, none, true);
        write_png(out_dir + "/r5_space_reinforce.png", o.w, o.h, reinf);
        const size_t dr = count_diff(plain, reinf);
        std::printf("gpu real stage 2: reinforcement overlay changes %zu pixels (%u space FOW draws)\n", dr,
                    scene.world().stats().decorations.space_fow_draws);
        CHECK(dr > 1000);
        g.ring.wait_idle();
        scene.stop(rr);
        streamer.destroy_all();
    }
    g.ring.wait_idle();
    if (up) rr.destroy_model(tank);
    o.destroy(g);
    rr.shutdown();
}

static void test_gpu(const std::string& src_dir, const std::string& out_dir) {
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
    cd.app_name = "alamo_render_r5_tests";
    cd.validation = strict ? rhi::Validation::Required : rhi::Validation::IfAvailable;
    if (!g.ctx.init(cd)) {
        CHECK(!strict);
        std::printf("SKIP gpu: no suitable Vulkan 1.3 device\n");
        return;
    }
    std::printf("gpu: %s, validation %s%s\n", g.ctx.caps().name.c_str(), g.ctx.validation_enabled() ? "on" : "off", strict ? " (required)" : "");
    rhi::FrameRingDesc fd;
    fd.frames_in_flight = 2;
    fd.uniform_bytes = 16ull << 20;
    fd.recording_threads = jobs::JobSystem::default_worker_count() + 1;
    CHECK(g.alloc.init(g.ctx) && g.up.init(g.ctx, g.alloc) && g.ring.init(g.ctx, g.alloc, fd));
    {
        render::Renderer r;
        render::RendererDesc rd;
        rd.color_format = VK_FORMAT_R8G8B8A8_UNORM;
        rd.depth_format = VK_FORMAT_D32_SFLOAT;
        const bool ok = r.init(g.ctx, g.alloc, g.up, rd);
        CHECK(ok);
        r.set_frame_ring(&g.ring);
        if (ok) test_gpu_alt(g, r);
        g.ring.wait_idle();
        r.shutdown();
    }
    test_gpu_real(g, out_dir);
    test_gpu_real_stage2(g, out_dir);
    g.ring.shutdown();
    g.up.shutdown();
    g.alloc.shutdown();
    const uint32_t ve = g.ctx.validation_errors(), vw = g.ctx.validation_warnings();
    std::printf("gpu: validation %u errors, %u warnings\n", ve, vw);
    CHECK(ve == 0 && vw == 0);
    g.ctx.shutdown();
}

// ------------------------------------------------------------------------------------------- R5 stealth + fog springs (r5 t03)
// ABILITY_VISUALS 1.1 / 1.2 / 1.3 / 1.4: the critically damped spring, the Stealth_Color multiply, the hardpoint mesh mask,
// the shadow bit and the 0.25 s HideWhenFogged spring.
static void test_stealth_spring() {
    for (const double T : {0.3, 0.5, 0.8}) {  // the float spring against an independent double-precision reference
        render::SmoothSpring s;
        for (int i = 1; i <= 90; ++i) {
            render::smooth_damp(s, 1.0f, static_cast<float>(T));
            double vel = 0.0;
            const double ref = ref_spring(0.0, 1.0, T, i, &vel);
            CHECK(std::fabs(s.value - ref) < 1e-4 && std::fabs(s.velocity - vel) < 1e-3);
        }
    }
    {  // ~90 % of the way after 2 T (T = 0.6 s: 36 ticks), monotone, no overshoot, converges
        render::SmoothSpring s;
        float prev = 0.0f, peak = 0.0f;
        bool mono = true;
        for (int i = 1; i <= 36; ++i) {
            render::smooth_damp(s, 1.0f, 0.6f);
            mono = mono && s.value >= prev;
            prev = s.value;
            peak = std::max(peak, s.value);
        }
        CHECK(mono && s.value > 0.88f && s.value < 0.95f && peak <= 1.0f);
        render::smooth_damp_steps(s, 1.0f, 0.6f, 300);
        CHECK(s.value > 0.9999f && s.value <= 1.0f);
        render::smooth_damp_steps(s, 0.0f, 0.6f, 0);  // zero steps: nothing
        CHECK(s.value > 0.9999f);
        render::smooth_damp_steps(s, 0.0f, 0.6f, 1000000);  // capped, converged
        CHECK(std::fabs(s.value) < 1e-4f);
    }
    {  // T <= 0: instant
        render::SmoothSpring s{0.4f, 0.7f};
        render::smooth_damp(s, 1.0f, 0.0f);
        CHECK(s.value == 1.0f && s.velocity == 0.0f);
    }
    const float red[4] = {1.0f, 0.25f, 0.25f, 0.47f};
    {  // StealthFx: tint = lerp(white, colour, v) above 0.01; white below
        render::StealthFx fx;
        fx.snap(false, false);
        CHECK(!fx.tinted() && fx.tint(red).x == 1.0f && fx.tint(red).w == 1.0f && !fx.shadow_dropped());
        fx.snap(true, false);
        const materials::float4 full = fx.tint(red);
        CHECK(fx.tinted() && full.x == 1.0f && full.y == 0.25f && full.z == 0.25f && std::fabs(full.w - 0.47f) < 1e-6f && fx.shadow_dropped());
        render::StealthFx mid;
        mid.snap(false, false);
        mid.step(true, false, 0.5f, 10);
        const float v = mid.value();
        const materials::float4 t = mid.tint(red);
        CHECK(v > 0.05f && v < 0.95f && mid.tinted());
        CHECK(std::fabs(t.y - (1.0f + (0.25f - 1.0f) * v)) < 1e-6f && std::fabs(t.w - (1.0f + (0.47f - 1.0f) * v)) < 1e-6f);
        // below 0.01 the multiply is not applied
        render::StealthFx lo;
        lo.snap(false, false);
        lo.step(true, false, 0.5f, 1);
        CHECK(lo.value() > 0.0f && lo.value() < 0.01f && !lo.tinted() && lo.tint(red).y == 1.0f);
    }
    {  // shadow bit: Stealth_Ability keeps it lost, Force_Cloak restores it once target 0 and v < 0.01
        render::StealthFx st, cl;
        st.step(true, false, 0.3f, 5);
        cl.step(true, true, 0.3f, 5);
        CHECK(st.shadow_dropped() && cl.shadow_dropped() && cl.cloak() && !st.cloak());
        st.step(false, false, 0.3f, 3);
        cl.step(false, true, 0.3f, 3);
        CHECK(st.tinted() && cl.tinted() && st.shadow_dropped() && cl.shadow_dropped());  // still fading
        st.step(false, false, 0.3f, 300);
        cl.step(false, true, 0.3f, 300);
        CHECK(!st.tinted() && !cl.tinted());
        CHECK(st.shadow_dropped() && !cl.shadow_dropped());
    }
}

// 1.3: the mesh / proxy named like a hardpoint's Model_To_Attach (upper-cased, cut at .ALO) is hidden.
static void test_stealth_hardpoint_mask() {
    assets::ModelAsset ma;
    for (const char* n : {"HULL", "Hp_Turret_A", "BASEMESH_X", "HP_OTHER"}) {
        assets::MeshInfo mi;
        mi.name = n;
        ma.meshes.push_back(mi);
    }
    for (const char* n : {"hp_gun_b", "NOT_A_HP"}) {
        assets::Proxy p;
        p.name = n;
        ma.proxies.push_back(p);
    }
    render::HardpointLayout lay;
    lay.meshes = 4;
    lay.proxies = 2;
    lay.hardpoints = 4;
    lay.attach_model = {"Art/Models/HP_TURRET_A.ALO", "HP_Gun_B.alo", "", "no_such_mesh.alo"};
    std::vector<uint8_t> mask(lay.sub_objects(), 0);
    mask[3] = 1;  // HP_OTHER already hidden for another reason: not counted
    const uint32_t n = render::hide_hardpoint_meshes(ma, lay, mask);
    CHECK(n == 2);
    CHECK(mask[0] == 0 && mask[1] == 1 && mask[2] == 0 && mask[3] == 1);  // meshes
    CHECK(mask[lay.proxy_index(0)] == 1 && mask[lay.proxy_index(1)] == 0);  // proxies
    CHECK(mask[lay.attachment_index(0)] == 0);                              // attachment bits are the builder's business
    CHECK(render::hide_hardpoint_meshes(ma, lay, mask) == 0);               // idempotent
}

// Builder: tint / shadow / spring timing / break override / fog spring.
static void test_stealth_builder() {
    const std::string dir = (fs::temp_directory_path() / "alamo_render_r5_tests").string();
    std::string err;
    const auto w = render::make_synthetic_sim_world(dir, &err);
    CHECK(w != nullptr);
    if (!w) return;
    render::SnapshotTypeTable table;
    table.build(w->types, objects::ModeKind::Space);
    const float red[4] = {1.0f, 0.25f, 0.25f, 0.5f};
    const float blue[4] = {0.5f, 0.5f, 1.0f, 0.25f};
    table.set_stealth(0, red, 0.5f);
    table.set_stealth(0, blue, 0.4f, true);
    render::GpuModel gm;
    auto ma = three_bone_model();
    for (const char* n : {"BASEMESH_HULL", "STEALTH_SHELL"}) {
        assets::MeshInfo mi;
        mi.name = n;
        mi.attach_bone = 1;
        ma->meshes.push_back(mi);
    }
    gm.asset = ma;
    gm.mesh_hidden = {0, 0, 0, 0, 1};
    gm.max_alt = 1;
    FakeModels models;
    models.model = &gm;
    std::vector<render::Instance> main, bg;
    render::SimInstanceStats st;
    {
        render::SimInstanceBuilder b(table);
        render::InterpolatedFrame f;
        f.objects = {sobj(1, 0)};
        f.tick = 1;
        b.build(f, models, main, bg, &st);
        CHECK(main.size() == 1 && main[0].light_scale.x == 1.0f && main[0].light_scale.w == 1.0f && !main[0].no_shadow);
        CHECK(st.stealth_tinted == 0 && st.stealth_no_shadow == 0 && b.stealth_value(1) == 0.0f);
        f.objects[0].flags = render::kSnapStealthMeshes;
        f.tick = 2;
        b.build(f, models, main, bg, &st);  // 1 tick: v < 0.01, the shadow caster is already gone, the mesh swap is instant
        CHECK(std::fabs(b.stealth_value(1) - ref_spring(0.0, 1.0, 0.5, 1)) < 1e-4);
        CHECK(main.size() == 1 && main[0].no_shadow && st.stealth_no_shadow == 1 && st.stealth_tinted == 0 && st.stealthed == 1);
        CHECK(main[0].mesh_hidden.size() >= 5 && main[0].mesh_hidden[3] == 1 && main[0].mesh_hidden[4] == 0);
        f.tick = 32;  // 30 more steps
        b.build(f, models, main, bg, &st);
        const double v = ref_spring(0.0, 1.0, 0.5, 31);
        CHECK(std::fabs(b.stealth_value(1) - v) < 1e-4 && v > 0.9);
        CHECK(main.size() == 1 && st.stealth_tinted == 1);
        if (main.size() == 1) {
            const render::Instance& in = main[0];
            CHECK(std::fabs(in.light_scale.x - 1.0) < 1e-6 && std::fabs(in.light_scale.y - (1.0 + (0.25 - 1.0) * v)) < 1e-4 &&
                  std::fabs(in.light_scale.w - (1.0 + (0.5 - 1.0) * v)) < 1e-4);
        }
        f.tick = 32;  // same tick: no step
        b.build(f, models, main, bg, &st);
        CHECK(std::fabs(b.stealth_value(1) - v) < 1e-4);
        // Off again: the mesh swap reverses at once, the tint fades with the same T, the Stealth_Ability object keeps its
        // shadow bit lost for good.
        f.objects[0].flags = 0;
        f.tick = 40;
        b.build(f, models, main, bg, &st);
        CHECK(b.stealth_value(1) < v - 0.1 && b.stealth_value(1) > 0.0f);  // fading out
        CHECK(st.stealthed == 0 && main.size() == 1 && main[0].no_shadow && st.stealth_tinted == 1);
        f.tick = 400;
        b.build(f, models, main, bg, &st);
        CHECK(b.stealth_value(1) < 0.01f && main.size() == 1 && main[0].light_scale.y == 1.0f && main[0].no_shadow &&
              b.stealth_shadow_dropped(1));
    }
    {  // Force_Cloak: the shadow comes back below 0.01; the colour / time are the cloak ones; the break override is 0.3 s
        render::SimInstanceBuilder b(table);
        render::InterpolatedFrame f;
        f.objects = {sobj(1, 0), sobj(2, 10), sobj(3, 20)};
        for (render::SnapshotObject& o : f.objects) {
            o.flags = render::kSnapStealthMeshes;  // all three appear stealthed: they start converged at 1
            o.ability_bits[0] = uint64_t{1} << render::kAbilityForceCloak;
        }
        f.objects[2].ability_bits[0] = 0;  // a plain Stealth_Ability object
        f.tick = 1;
        b.build(f, models, main, bg, &st);
        CHECK(main.size() == 3 && b.stealth_value(1) == 1.0f && st.stealth_tinted == 3);
        CHECK(main[0].light_scale.x == 0.5f && main[0].light_scale.z == 1.0f && main[0].light_scale.w == 0.25f);  // cloak colour
        CHECK(main[2].light_scale.x == 1.0f && main[2].light_scale.y == 0.25f && main[2].light_scale.w == 0.5f);   // stealth colour
        // Off: object 1 with the type's cloak T (0.4), object 2 with the 0.3 s break override, object 3 with the stealth T (0.5).
        for (render::SnapshotObject& o : f.objects) o.flags = 0;
        f.objects[1].stealth_time = render::kStealthBreakFade;
        f.tick = 7;  // 6 steps
        b.build(f, models, main, bg, &st);
        CHECK(std::fabs(b.stealth_value(1) - ref_spring(1.0, 0.0, 0.4, 6)) < 1e-4);
        CHECK(std::fabs(b.stealth_value(2) - ref_spring(1.0, 0.0, 0.3, 6)) < 1e-4);
        CHECK(std::fabs(b.stealth_value(3) - ref_spring(1.0, 0.0, 0.5, 6)) < 1e-4);
        CHECK(b.stealth_value(2) < b.stealth_value(1) && b.stealth_value(1) < b.stealth_value(3));
        f.tick = 307;
        b.build(f, models, main, bg, &st);
        CHECK(main.size() == 3);
        CHECK(!b.stealth_shadow_dropped(1) && !b.stealth_shadow_dropped(2) && b.stealth_shadow_dropped(3));
        CHECK(!main[0].no_shadow && !main[1].no_shadow && main[2].no_shadow && st.stealth_tinted == 0);
    }
    {  // A type with no stealth data: the swap and the shadow follow the flag, no tint, instant.
        render::SnapshotTypeTable bare;
        bare.build(w->types, objects::ModeKind::Space);
        render::SimInstanceBuilder b(bare);
        render::InterpolatedFrame f;
        f.objects = {sobj(1, 0)};
        f.tick = 1;
        b.build(f, models, main, bg, &st);
        f.objects[0].flags = render::kSnapStealthMeshes;
        f.tick = 2;
        b.build(f, models, main, bg, &st);
        CHECK(b.stealth_value(1) == 1.0f && main.size() == 1 && main[0].no_shadow && main[0].light_scale.y == 1.0f && st.stealth_tinted == 0);
    }
    {  // HideWhenFogged: 0.25 s spring (~40 % left after 8 ticks, ~9 % after 2 T = 15 ticks, hidden below 0.025 after ~21 ticks)
        render::SimInstanceBuilder b(table);
        render::InterpolatedFrame f;
        f.objects = {sobj(1, 0)};
        f.tick = 1;
        b.build(f, models, main, bg, &st);
        f.objects[0].flags = render::kSnapFogHidden;
        f.tick = 9;
        b.build(f, models, main, bg, &st);
        CHECK(std::fabs(b.fog_alpha(1) - ref_spring(1.0, 0.0, 0.25, 8)) < 1e-4 && b.fog_alpha(1) > 0.3f && b.fog_alpha(1) < 0.45f);
        f.tick = 16;
        b.build(f, models, main, bg, &st);
        CHECK(std::fabs(b.fog_alpha(1) - ref_spring(1.0, 0.0, 0.25, 15)) < 1e-4 && b.fog_alpha(1) > 0.07f && b.fog_alpha(1) < 0.12f);
        CHECK(main.size() == 1 && std::fabs(main[0].light_scale.w - b.fog_alpha(1)) < 1e-6f);
        f.tick = 1 + 18;
        b.build(f, models, main, bg, &st);
        CHECK(b.fog_alpha(1) > render::SimInstanceBuilder::kFogHideAlpha && main.size() == 1);
        f.tick = 1 + 24;
        b.build(f, models, main, bg, &st);
        CHECK(b.fog_alpha(1) < render::SimInstanceBuilder::kFogHideAlpha && main.empty() && st.fog_faded_out == 1);
        // and back in: monotone rise, no overshoot
        f.objects[0].flags = 0;
        float prev = b.fog_alpha(1);
        bool mono = true;
        for (uint64_t t = 26; t <= 70; ++t) {
            f.tick = t;
            b.build(f, models, main, bg, &st);
            mono = mono && b.fog_alpha(1) >= prev && b.fog_alpha(1) <= 1.0f;
            prev = b.fog_alpha(1);
        }
        CHECK(mono && prev > 0.99f);
    }
    {  // Last_State_Visible_Under_FOW types switch instantly (type +0x88); 0 s smooth time = instant for every type
        render::SnapshotTypeTable t2;
        t2.build(w->types, objects::ModeKind::Space);
        t2.set_ghosts(0, true, false);
        render::SimInstanceBuilder b(t2);
        render::InterpolatedFrame f;
        f.objects = {sobj(1, 0)};
        f.tick = 1;
        b.build(f, models, main, bg, &st);
        f.objects[0].flags = render::kSnapFogHidden;
        f.tick = 2;
        b.build(f, models, main, bg, &st);
        CHECK(b.fog_alpha(1) == 0.0f);
        render::SimInstanceBuilder b0(table);
        b0.set_fog_smooth_time(0.0f);
        f.objects[0].flags = 0;
        f.tick = 1;
        b0.build(f, models, main, bg, &st);
        f.objects[0].flags = render::kSnapFogHidden;
        f.tick = 2;
        b0.build(f, models, main, bg, &st);
        CHECK(b0.fog_alpha(1) == 0.0f);
    }
    std::printf("stealth / fog springs: OK\n");
}

// Type table: Stealth_Color / Stealth_Transition_Time are read from the nested <Abilities> entries (r5 t03).
static void test_stealth_type_table() {
    render::SnapshotTypeInfo none;  // defaults: no stealth data, white
    CHECK(!none.has_stealth_color && !none.has_cloak_color && none.stealth_time == 0.0f && none.cloak_time == 0.0f &&
          none.stealth_color[3] == 1.0f);
    const std::string root = env_or_empty("ALAMO_FOC_DATA");
    if (root.empty()) {
        std::printf("stealth type table (real data): ALAMO_FOC_DATA not set, skipped\n");
        return;
    }
    vfs::VfsConfig cfg;
    cfg.foc_root = root;
    std::string err;
    auto files = vfs::Vfs::create(cfg, &err);
    CHECK(files != nullptr);
    if (!files) return;
    objects::ObjectTypeDatabase db;
    CHECK(db.load(*files));
    objects::GameObjectTypes types;
    CHECK(types.build(db, files.get()));
    render::SnapshotTypeTable table;
    table.build(types, objects::ModeKind::Space);
    auto close_to = [](float a, float b) { return std::fabs(a - b) < 1e-5f; };
    const objects::TypeIndex noghri = table.find("Noghri_Assassin"), urai = table.find("Urai_Fen"), phantom = table.find("TIE_Phantom"),
                             luke = table.find("Luke_Skywalker_Jedi");
    CHECK(noghri != objects::kNone && urai != objects::kNone && phantom != objects::kNone && luke != objects::kNone);
    if (noghri == objects::kNone || urai == objects::kNone || phantom == objects::kNone || luke == objects::kNone) return;
    const render::SnapshotTypeInfo& n = table[noghri];  // 255,255,255,64 / 0.5 s
    CHECK(n.has_stealth_color && close_to(n.stealth_color[0], 1.0f) && close_to(n.stealth_color[3], 64.0f / 255.0f) && close_to(n.stealth_time, 0.5f));
    const render::SnapshotTypeInfo& u = table[urai];  // 255,64,64,120 / 0.6 s
    CHECK(u.has_stealth_color && close_to(u.stealth_color[1], 64.0f / 255.0f) && close_to(u.stealth_color[3], 120.0f / 255.0f) && close_to(u.stealth_time, 0.6f));
    CHECK(table[phantom].has_stealth_color && close_to(table[phantom].stealth_time, 0.8f) && close_to(table[phantom].stealth_color[3], 1.0f));
    CHECK(table[luke].has_cloak_color && close_to(table[luke].cloak_time, 0.4f) && !table[luke].has_stealth_color);
    // Playtest V3: map marker flags have no drawable model (so no snapshot object); a backdrop marker and real units keep theirs.
    for (const char* flag : {"Attacker Entry Position", "Defending Forces Position", "Garrison Position", "Space Station Position"}) {
        const objects::TypeIndex m = table.find(flag);
        CHECK(m != objects::kNone && table[m].model.empty());
    }
    const objects::TypeIndex ds = table.find("Death_Star_Position");
    CHECK(ds != objects::kNone && !table[ds].model.empty() && table[ds].in_background);
    CHECK(!table[phantom].model.empty());
    uint32_t with_color = 0;
    for (objects::TypeIndex t = 0; t < table.size(); ++t) with_color += table[t].has_stealth_color || table[t].has_cloak_color ? 1u : 0u;
    CHECK(with_color >= 8);
    std::printf("stealth type table: %u types with Stealth_Color / Force_Cloak_Color; Noghri a=%.3f T=%.2f, Urai_Fen g=%.3f a=%.3f T=%.2f\n",
                with_color, static_cast<double>(n.stealth_color[3]), static_cast<double>(n.stealth_time),
                static_cast<double>(u.stealth_color[1]), static_cast<double>(u.stealth_color[3]), static_cast<double>(u.stealth_time));
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    log::set_min_level(log::Level::Error);
    const std::string src_dir = ALAMO_RENDER_SOURCE_DIR;
    const std::string out_dir = ALAMO_R5_OUT_DIR;
    test_fog_rules();
#ifdef ALAMO_R5_HAVE_FOG
    test_fog_sim_grid();
#endif
    test_snapshot_plumbing();
    test_oi_snapshot();
    test_builder();
    test_oi_builder();
    test_stage2_builder();
    test_stealth_spring();
    test_stealth_hardpoint_mask();
    test_stealth_builder();
    test_stealth_type_table();
#ifdef ALAMO_R5_HAVE_ANIM
    test_anim_visibility();
#endif
    render::SurfaceFxLibrary lib;
    test_surface_data(lib);
    test_runtime_decals(lib);
    test_sim_decorations(lib);
    test_sim_decorations_stage2(lib);
    test_real_surface_data();
    test_gpu(src_dir, out_dir);
    std::printf(g_fail ? "alamo_render_r5_tests: %d failure(s)\n" : "alamo_render_r5_tests: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
