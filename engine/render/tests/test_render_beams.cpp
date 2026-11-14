// R5 t04 tests: ability beams (docs/re/ABILITY_VISUALS.md 2, 4, 6 finding 1). CPU: GameConstants / style / lightning
// parsing, ribbon + dot + lightning geometry, per-beam animation (scroll, dot travel, one-shots, super-laser continuation),
// SnapshotBeam -> world endpoints (bones, hardpoint, Ranged_Target_Z_Adjust, zap nearest bone), snapshot digest /
// interpolation, apply_beams on a real AbilitySystem + CombatSystem (tractor, energy weapon, laser-defense zap). GPU (needs
// ALAMO_FOC_DATA, Vulkan 1.3): the beams drawn additive by Renderer::set_beams in the Transparent bucket, validation 0
// errors.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "alamo/assets/asset_cache.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/core/log.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/jobs/job_system.hpp"
#include "alamo/render/beams.hpp"
#include "alamo/render/map_scene.hpp"
#include "alamo/render/pipelines.hpp"
#include "alamo/render/png.hpp"
#include "alamo/render/renderer.hpp"
#include "alamo/render/sim_beams.hpp"
#include "alamo/render/sim_snapshot.hpp"
#include "alamo/rhi/rhi.hpp"
#include "alamo/tactical/abilities/ability_system.hpp"
#include "alamo/vfs/vfs.hpp"
#include "alamo/test/support.hpp"
#include "alamo/render/test/gpu.hpp"

using namespace alamo;

using ::alamo::test::g_fail;
#define CHECK_NEAR(a, b, eps)                                                                                          \
    do {                                                                                                               \
        const double va_ = static_cast<double>(a), vb_ = static_cast<double>(b);                                       \
        if (!(std::fabs(va_ - vb_) <= (eps))) {                                                                        \
            std::printf("FAIL %s:%d: %s ~ %s (%.6g vs %.6g)\n", __FILE__, __LINE__, #a, #b, va_, vb_);                \
            ++g_fail;                                                                                                  \
        }                                                                                                              \
    } while (0)

using ::alamo::test::env_or_empty;

using render::Float3;
static std::span<const uint8_t> bytes_of(const std::string& s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}
static float len3(const Float3& a, const Float3& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

// ------------------------------------------------------------------------------------------------ data
static void test_constants_and_styles() {
    render::BeamConstants gc;  // vanilla defaults
    CHECK(gc.tractor.width == 10.0f && gc.tractor.frames == 10 && gc.tractor.color[1] == 75 && gc.tractor.color[0] == 0);
    CHECK(gc.energy.width == 30.0f && gc.energy.color[0] == 255 && gc.energy.color[1] == 75 && gc.energy.texture == "tractor_beam00.tga");
    const std::string xml =
        "<GameConstants><Tractor_Beam_Width>12.5</Tractor_Beam_Width><Tractor_Beam_Frames>8</Tractor_Beam_Frames>"
        "<Tractor_Beam_Texture>tb.tga</Tractor_Beam_Texture><Tractor_Beam_Color>1, 2, 3</Tractor_Beam_Color>"
        "<Energy_Beam_Width>31</Energy_Beam_Width><Energy_Beam_Color>9, 8, 7, 6</Energy_Beam_Color></GameConstants>";
    render::BeamConstants g2;
    CHECK(render::parse_beam_constants(bytes_of(xml), g2));
    CHECK(g2.tractor.width == 12.5f && g2.tractor.frames == 8 && g2.tractor.texture == "tb.tga" && g2.tractor.color[2] == 3 &&
          g2.tractor.color[3] == 255);
    CHECK(g2.energy.width == 31.0f && g2.energy.color[0] == 9 && g2.energy.color[3] == 6 && g2.energy.frames == 10);
    const std::string bad = "<GameConstants><unclosed>";
    render::BeamConstants g3;
    CHECK(!render::parse_beam_constants(bytes_of(bad), g3) || g3.tractor.width == 10.0f);

    // GC-only styles (4.2 fallbacks, 2, 4.3, 4.4)
    const render::BeamStyle tr = render::default_beam_style(render::kBeamTractor, gc);
    CHECK(tr.mode == 0 && tr.dots == 4 && tr.dot_dir == -1 && tr.lightning == "Tractor_Beam" && tr.start_bone == "HP_trac_bone" &&
          tr.width == 10.0f && tr.color[1] == 75);
    const render::BeamStyle ew = render::default_beam_style(render::kBeamEnergyWeapon, gc);
    CHECK(ew.mode == 0 && ew.dots == 10 && ew.dot_dir == 1 && ew.lightning == "Piet_Energy_Weapon" && ew.width == 30.0f && ew.color[0] == 255);
    const render::BeamStyle sl = render::default_beam_style(render::kBeamSuperLaser, gc);
    CHECK(sl.mode == 1 && sl.dots == 0 && sl.lightning.empty() && sl.texture == "tractor_beam01.tga" && sl.frames == 5 &&
          sl.color[1] == 237);
    const render::BeamStyle zp = render::default_beam_style(render::kBeamLaserZap, gc);
    CHECK(zp.mode == 0 && zp.dots == 0 && zp.lifetime == 2 && zp.width == 4.0f && zp.color[2] == 235);

    // LightningEffectTypes
    const render::LightningLibrary lib = render::LightningLibrary::builtin();
    const render::LightningStyle* tb = lib.find("tractor_beam");
    CHECK(tb && tb->helix() && tb->radius == 5.0f && tb->rotations_per_second == 3.0f && tb->bolts == 3 && tb->update_all &&
          tb->color_start[0] == 255 && tb->color_start[1] == 255 && tb->color_start[2] == 0);
    const render::LightningStyle* pw = lib.find("Piet_Energy_Weapon");
    CHECK(pw && !pw->helix() && pw->bolts == 5 && pw->detail == 2.0f && pw->width_max == 3.0f && pw->width_min == 0.3f &&
          pw->color_start[1] == 75 && pw->color_end[1] == 255);
    render::LightningLibrary lib2;
    const std::string lx =
        "<LightningEffects><LightningEffect name=\"Leech_Shields\"><Width_Max>2</Width_Max><Width_Min>.5</Width_Min>"
        "<Displace>0.3, 0.3, 0.0</Displace><Detail>.5</Detail><Color_Start>34,34,235,255</Color_Start>"
        "<Color_End>0,0,0,0</Color_End><Number_Bolts>4</Number_Bolts><Update_All>true</Update_All></LightningEffect></LightningEffects>";
    CHECK(lib2.load(bytes_of(lx)) && lib2.size() == 1);
    const render::LightningStyle* ls = lib2.find("LEECH_SHIELDS");
    CHECK(ls && ls->bolts == 4 && ls->width_min == 0.5f && ls->displace[0] == 0.3f && ls->color_start[2] == 235 && ls->update_all);
}

// ------------------------------------------------------------------------------------------------ geometry
static render::BeamInstance make_beam(uint8_t kind, Float3 a, Float3 b, const render::BeamConstants& gc = {}) {
    render::BeamInstance bi;
    bi.key = kind;
    bi.kind = kind;
    bi.start = a;
    bi.end = b;
    bi.style = render::default_beam_style(kind, gc);
    return bi;
}

static void test_geometry() {
    CHECK_NEAR(render::van_der_corput(1), 0.5, 1e-7);
    CHECK_NEAR(render::van_der_corput(2), 0.25, 1e-7);
    CHECK_NEAR(render::van_der_corput(3), 0.75, 1e-7);
    CHECK_NEAR(render::van_der_corput(4), 0.125, 1e-7);
    CHECK_NEAR(render::van_der_corput(10), 0.3125, 1e-7);

    Float3 dir, side;
    float len = 0;
    CHECK(render::beam_frame({0, 0, 0}, {100, 0, 0}, dir, len, side) && len == 100.0f && dir.x == 1.0f);
    CHECK(side.z == 0.0f && std::fabs(side.x * dir.x + side.y * dir.y) < 1e-6f && std::fabs(side.y) == 1.0f);  // horizontal width axis
    CHECK(render::beam_frame({0, 0, 0}, {0, 0, 50}, dir, len, side) && side.x == 1.0f && side.y == 0.0f);  // vertical: fallback +X
    CHECK(!render::beam_frame({1, 2, 3}, {1, 2, 3}, dir, len, side));

    // mode 0: one quad, +-W/2 across, U 1 -> 0 across, V 1 (start) -> 0 (end)
    render::BeamInstance t = make_beam(render::kBeamTractor, {0, 0, 0}, {300, 0, 0});
    std::vector<render::PrimVertex> v;
    std::vector<uint32_t> idx;
    CHECK(render::beam_ribbon(t, 0.0f, v, idx) && v.size() == 4 && idx.size() == 6);
    CHECK_NEAR(len3({v[0].pos[0], v[0].pos[1], v[0].pos[2]}, {v[1].pos[0], v[1].pos[1], v[1].pos[2]}), 10.0, 1e-4);  // W across
    CHECK_NEAR(len3({v[0].pos[0], v[0].pos[1], v[0].pos[2]}, {v[2].pos[0], v[2].pos[1], v[2].pos[2]}), 300.0, 1e-3);  // L along
    CHECK(v[0].uv[0] == 1.0f && v[1].uv[0] == 0.0f && v[0].uv[1] == 1.0f && v[2].uv[1] == 0.0f);
    CHECK(v[0].color == (uint32_t{75} << 8 | 0xFF000000u));  // beam colour (0, 75, 0, 255) as D3DCOLOR
    // mode 1: segments of 200 W, U 0 -> 200 along; the partial last segment has U = remainder / W; the scroll subtracts
    render::BeamInstance s = make_beam(render::kBeamTractor, {0, 0, 0}, {5000, 0, 0});
    s.style.mode = 1;
    v.clear();
    idx.clear();
    CHECK(render::beam_ribbon(s, 0.0f, v, idx) && v.size() == 12 && idx.size() == 18);  // W 10: 2000 + 2000 + 1000
    CHECK(v[0].uv[0] == 0.0f && v[2].uv[0] == 200.0f && v[4].uv[0] == 0.0f && v[6].uv[0] == 200.0f && v[10].uv[0] == 100.0f);
    CHECK(v[0].uv[1] == 1.0f && v[1].uv[1] == 0.0f);  // V 1 -> 0 across
    CHECK_NEAR(v[2].pos[0], 2000.0, 1e-3);
    v.clear();
    idx.clear();
    CHECK(render::beam_ribbon(s, 0.25f, v, idx) && v[0].uv[0] == -0.25f && v[2].uv[0] == 199.75f);
    // dot: 3.5 x 3.5 in the ribbon plane, colour 0.7 x beam colour
    v.clear();
    idx.clear();
    CHECK(render::beam_dot(t, 0.5f, v, idx) && v.size() == 4);
    CHECK_NEAR(len3({v[0].pos[0], v[0].pos[1], v[0].pos[2]}, {v[1].pos[0], v[1].pos[1], v[1].pos[2]}), 3.5, 1e-4);
    CHECK_NEAR(v[0].pos[0] + v[3].pos[0], 300.0, 1e-3);  // centred on 0.5 along the beam
    CHECK(((v[0].color >> 8) & 0xFF) == 53 && ((v[0].color >> 24) & 0xFF) == 179);  // 75 x 0.7 = 52.5, 255 x 0.7 = 178.5

    // lightning: helix at radius 5 around the axis, endpoints on/near it, deterministic; arc: exact endpoints, 2^(detail+3)
    const render::LightningLibrary lib = render::LightningLibrary::builtin();
    const render::LightningStyle& helix = *lib.find("Tractor_Beam");
    const std::vector<Float3> hp = render::lightning_points(helix, {0, 0, 0}, {400, 0, 0}, 77u, 0.5f, 0.0f);
    CHECK(hp.size() >= 5);
    for (size_t i = 1; i + 1 < hp.size(); ++i) {
        const float r = std::sqrt(hp[i].y * hp[i].y + hp[i].z * hp[i].z);
        CHECK(r > 3.5f && r < 6.5f);  // radius 5 + a small shimmer
    }
    CHECK(render::lightning_points(helix, {0, 0, 0}, {400, 0, 0}, 77u, 0.5f, 0.0f).size() == hp.size() &&
          render::lightning_points(helix, {0, 0, 0}, {400, 0, 0}, 77u, 0.5f, 0.0f)[3].y == hp[3].y);
    CHECK(render::lightning_points(helix, {0, 0, 0}, {400, 0, 0}, 77u, 0.5f, 0.0f)[3].y !=
          render::lightning_points(helix, {0, 0, 0}, {400, 0, 0}, 77u, 0.6f, 0.0f)[3].y);  // rotates with time
    const std::vector<Float3> arc = render::lightning_points(*lib.find("Piet_Energy_Weapon"), {0, 0, 0}, {800, 0, 0}, 5u, 0.0f, 0.0f);
    CHECK(arc.size() == 33 && arc.front().x == 0.0f && arc.back().x == 800.0f);  // Detail 2 -> 5 levels -> 32 segments
    float maxoff = 0.0f;
    for (const Float3& p : arc) maxoff = std::max(maxoff, std::sqrt(p.y * p.y + p.z * p.z));
    CHECK(maxoff > 1.0f && maxoff < 200.0f);  // jagged, not a straight line
    const std::vector<Float3> arc2 = render::lightning_points(*lib.find("Piet_Energy_Weapon"), {0, 0, 0}, {800, 0, 0}, 6u, 0.0f, 0.0f);
    CHECK(arc2.size() == arc.size() && arc2[7].y != arc[7].y);  // a different seed, a different bolt
    std::vector<render::PrimVertex> sv;
    std::vector<uint32_t> si;
    const uint8_t col[4] = {255, 75, 0, 0};
    render::lightning_strip(arc, {400, -500, 300}, 1.5f, col, sv, si);
    CHECK(sv.size() == 66 && si.size() == 32 * 6 && sv[0].uv[1] == 0.0f && sv[64].uv[1] == 1.0f);
    CHECK_NEAR(len3({sv[10].pos[0], sv[10].pos[1], sv[10].pos[2]}, {sv[11].pos[0], sv[11].pos[1], sv[11].pos[2]}), 1.5, 1e-3);
}

// ------------------------------------------------------------------------------------------------ animation
static void test_animation() {
    const render::LightningLibrary lib = render::LightningLibrary::builtin();
    // tractor: dots start at 0.45 0.70 0.50 0.89 from the owner end and travel towards the owner, 1/50 per tick
    {
        render::BeamAnimator an;
        an.set_lightning(&lib);
        const render::BeamInstance t = make_beam(render::kBeamTractor, {0, 0, 0}, {500, 0, 0});
        an.step(100, std::span<const render::BeamInstance>(&t, 1));
        CHECK(an.draws().size() == 1 && an.draws()[0].dots.size() == 4);
        CHECK_NEAR(an.draws()[0].dots[0], 0.45, 1e-6);
        CHECK_NEAR(an.draws()[0].dots[3], 0.89, 1e-6);
        an.step(100, std::span<const render::BeamInstance>(&t, 1));  // same tick: nothing advances
        CHECK_NEAR(an.draws()[0].dots[0], 0.45, 1e-6);
        an.step(101, std::span<const render::BeamInstance>(&t, 1));
        CHECK_NEAR(an.draws()[0].dots[0], 0.43, 1e-5);
        CHECK_NEAR(an.draws()[0].dots[1], 0.68, 1e-5);
        an.step(104, std::span<const render::BeamInstance>(&t, 1));  // 3 ticks at once
        CHECK_NEAR(an.draws()[0].dots[0], 0.37, 1e-5);
        for (uint64_t k = 105; k <= 154; ++k) an.step(k, std::span<const render::BeamInstance>(&t, 1));  // 50 more: a full loop
        CHECK_NEAR(an.draws()[0].dots[0], 0.37, 1e-4);
        // wraps into [0, 1)
        render::BeamAnimator w;
        render::BeamInstance t2 = t;
        t2.style.dots = 1;
        w.step(0, std::span<const render::BeamInstance>(&t2, 1));
        for (uint64_t k = 1; k <= 23; ++k) w.step(k, std::span<const render::BeamInstance>(&t2, 1));
        CHECK(w.draws()[0].dots[0] >= 0.0f && w.draws()[0].dots[0] < 1.0f);  // 0.45 - 23/50 = -0.01 -> 0.99
        CHECK_NEAR(w.draws()[0].dots[0], 0.99, 1e-4);
    }
    // energy weapon: 10 dots from the van der Corput sequence, travelling owner -> target
    {
        render::BeamAnimator an;
        const render::BeamInstance e = make_beam(render::kBeamEnergyWeapon, {0, 0, 0}, {500, 0, 0});
        an.step(0, std::span<const render::BeamInstance>(&e, 1));
        CHECK(an.draws()[0].dots.size() == 10 && an.draws()[0].dots[0] == 0.5f && an.draws()[0].dots[1] == 0.25f);
        an.step(1, std::span<const render::BeamInstance>(&e, 1));
        CHECK_NEAR(an.draws()[0].dots[0], 0.52, 1e-5);
    }
    // super laser: the texture scrolls one repeat every `frames` ticks
    {
        render::BeamAnimator an;
        render::BeamInstance sl = make_beam(render::kBeamSuperLaser, {0, 0, 0}, {1000, 0, 0});
        an.step(0, std::span<const render::BeamInstance>(&sl, 1));
        CHECK(an.draws()[0].u_offset == 0.0f && an.draws()[0].dots.empty() && an.draws()[0].lightning == nullptr);
        an.step(1, std::span<const render::BeamInstance>(&sl, 1));
        CHECK_NEAR(an.draws()[0].u_offset, 0.2, 1e-6);
        an.step(5, std::span<const render::BeamInstance>(&sl, 1));  // 4 more ticks
        CHECK_NEAR(an.draws()[0].u_offset, 0.0, 1e-6);  // wrapped after 5 ticks in all
        // continuation (4.3): the beam disappears -> kept for `continuation` ticks, ending at start + 2 x (end - start)
        sl.style.continuation = 2;
        an.step(6, std::span<const render::BeamInstance>(&sl, 1));
        an.step(7, {});
        CHECK(an.draws().size() == 1 && an.draws()[0].beam.end.x == 2000.0f && an.draws()[0].beam.start.x == 0.0f);
        an.step(8, {});
        CHECK(an.draws().size() == 1);
        an.step(9, {});
        CHECK(an.draws().empty());
        // a plain beam that vanishes is gone at once
        render::BeamInstance tt = make_beam(render::kBeamTractor, {0, 0, 0}, {10, 0, 0});
        an.step(10, std::span<const render::BeamInstance>(&tt, 1));
        an.step(11, {});
        CHECK(an.draws().empty());
    }
    // zap: a one-shot lives `lifetime` (= Laser_Beam_Frames 2) ticks on its own, a repeat of the key does not extend it
    {
        render::BeamAnimator an;
        render::BeamInstance z = make_beam(render::kBeamLaserZap, {0, 0, 0}, {0, 100, 0});
        z.continuous = false;
        z.key = 777;
        an.step(10, std::span<const render::BeamInstance>(&z, 1));
        CHECK(an.draws().size() == 1);
        an.step(11, std::span<const render::BeamInstance>(&z, 1));
        CHECK(an.draws().size() == 1);
        an.step(12, {});
        CHECK(an.draws().empty());
    }
    // lightning: the Tractor_Beam effect has up to 3 bolts, Piet_Energy_Weapon up to 5; same seed -> same bolts
    {
        render::BeamAnimator a1(42u), a2(42u);
        a1.set_lightning(&lib);
        a2.set_lightning(&lib);
        const render::BeamInstance t = make_beam(render::kBeamTractor, {0, 0, 0}, {500, 0, 0});
        const render::BeamInstance e = make_beam(render::kBeamEnergyWeapon, {0, 100, 0}, {500, 100, 0});
        render::BeamInstance both[2] = {t, e};
        both[1].key = 99;
        size_t max_t = 0, max_e = 0;
        for (uint64_t k = 0; k < 40; ++k) {
            a1.step(k, both);
            a2.step(k, both);
            max_t = std::max(max_t, a1.draws()[0].bolts.size());
            max_e = std::max(max_e, a1.draws()[1].bolts.size());
            CHECK(a1.draws()[0].bolts.size() == a2.draws()[0].bolts.size() && a1.draws()[1].bolts.size() == a2.draws()[1].bolts.size());
            if (!a1.draws()[0].bolts.empty()) CHECK(a1.draws()[0].bolts[0].seed == a2.draws()[0].bolts[0].seed);
        }
        CHECK(max_t == 3 && max_e >= 1 && max_e <= 5);
        CHECK(a1.draws()[0].lightning && a1.draws()[0].lightning->helix() && a1.draws()[1].lightning && !a1.draws()[1].lightning->helix());
        // Update_All: the shape seed of a live tractor bolt changes every tick
        const uint32_t s0 = a1.draws()[0].bolts.empty() ? 0u : a1.draws()[0].bolts[0].seed;
        a1.step(40, both);
        a1.step(41, both);
        CHECK(!a1.draws()[0].bolts.empty() && a1.draws()[0].bolts[0].seed != s0);
        a1.set_lightning(nullptr);  // resets
        CHECK(a1.draws().empty() || a1.size() == 0);
    }
}

static void test_geometry_build() {
    const render::LightningLibrary lib = render::LightningLibrary::builtin();
    render::BeamAnimator an(1u);
    an.set_lightning(&lib);
    render::BeamInstance in[3] = {make_beam(render::kBeamTractor, {0, 0, 0}, {500, 0, 0}),
                                  make_beam(render::kBeamEnergyWeapon, {0, 200, 0}, {500, 200, 0}),
                                  make_beam(render::kBeamSuperLaser, {0, -200, 0}, {500, -200, 0})};
    in[1].key = 2;
    in[2].key = 3;
    for (uint64_t k = 0; k < 20; ++k) an.step(k, in);
    render::OrbitCamera cam;
    cam.distance = 900.0f;
    const render::ViewMatrices view = render::make_view(cam, 640, 360, 10.0f, 7000.0f);
    render::BeamGeometry geo;
    geo.build(an.draws(), view);
    CHECK(geo.ribbons == 3 && geo.skipped == 0 && geo.dots == 14 && geo.bolts >= 4);
    // batches per texture: tractor_beam00 (tractor + energy ribbons), tractor_beam01 (super laser), the dot texture, the bolts
    bool b00 = false, b01 = false, dot = false, lit = false;
    uint32_t total = 0;
    for (const render::BeamGeometry::Batch& b : geo.batches) {
        b00 = b00 || (b.texture == "tractor_beam00.tga" && b.index_count == 12);
        b01 = b01 || (b.texture == "tractor_beam01.tga" && b.index_count == 6 * 1);
        dot = dot || (b.texture == render::kBeamDotTexture && b.index_count == 14 * 6);
        lit = lit || (b.texture == render::kBeamLightningTexture && b.index_count > 0);
        total += b.index_count;
        CHECK(b.first_index + b.index_count <= geo.indices.size());
    }
    CHECK(b00 && dot && lit);
    CHECK(b01 || geo.batches.size() >= 3);  // 1000 W 35 mode 1 is one 7000-long segment
    CHECK(total == geo.indices.size());
    for (uint32_t i : geo.indices) CHECK(i < geo.vertices.size());
    // a degenerate beam is skipped
    render::BeamInstance d = make_beam(render::kBeamTractor, {1, 1, 1}, {1, 1, 1});
    render::BeamAnimator an2;
    an2.step(0, std::span<const render::BeamInstance>(&d, 1));
    render::BeamGeometry g2;
    g2.build(an2.draws(), view);
    CHECK(g2.ribbons == 0 && g2.skipped == 1 && g2.vertices.empty());
}

// ------------------------------------------------------------------------------------------------ snapshot -> world beams
static render::SnapshotObject obj(objects::ObjectId id, objects::TypeIndex type, float x, float y, float z, int32_t owner) {
    render::SnapshotObject o;
    o.id = id;
    o.type = type;
    o.owner = owner;
    o.position = {x, y, z};
    return o;
}

static void test_frame_resolution() {
    render::BeamStyleTable styles;
    styles.set_z_adjust(1, 12.0f);
    styles.set_hardpoint_bones(1, {"HP_Turbolaser_01", "HP_Shield_Gen"});
    render::InterpolatedFrame f;
    f.objects = {obj(10, 0, 0, 0, 0, 0), obj(20, 1, 600, 0, 0, 1), obj(30, 1, 600, 500, 0, 1)};
    // bone source: owner HP_trac_bone at (5, 0, 7), MuzzleA_00 at (9, 0, 0), muzzleB_00 / muzzleB_01; target hardpoint bone at (610, 3, 4)
    const render::BeamBoneFn bone = [](const render::SnapshotObject& o, std::string_view name, Vec3& out) {
        if (o.id == 10 && name == "HP_trac_bone") out = {5, 0, 7};
        else if (o.id == 10 && name == "MuzzleA_00") out = {9, 0, 0};
        else if (o.id == 10 && name == "muzzleB_00") out = {0, 50, 0};
        else if (o.id == 10 && name == "muzzleB_01") out = {0, -50, 0};
        else if (o.id == 20 && name == "HP_Shield_Gen") out = {610, 3, 4};
        else return false;
        return true;
    };
    render::SnapshotBeam tr;
    tr.kind = render::kBeamTractor;
    tr.owner = 10;
    tr.target = 20;
    tr.end_z_adjust = 12.0f;
    tr.style = render::default_beam_style(render::kBeamTractor, {});
    render::SnapshotBeam ew;
    ew.kind = render::kBeamEnergyWeapon;
    ew.owner = 10;
    ew.target = 20;
    ew.hardpoint = 1;
    ew.end_z_adjust = 12.0f;
    ew.style = render::default_beam_style(render::kBeamEnergyWeapon, {});
    render::SnapshotBeam sl;
    sl.kind = render::kBeamSuperLaser;
    sl.owner = 10;
    sl.target = 30;
    sl.style = render::default_beam_style(render::kBeamSuperLaser, {});
    render::SnapshotBeam zp;
    zp.kind = render::kBeamLaserZap;
    zp.owner = 10;
    zp.fixed_end = true;
    zp.end = {0, 80, 0};
    zp.frame = 1234;
    zp.style = render::default_beam_style(render::kBeamLaserZap, {});
    zp.style.bones = {"muzzleB_00", "muzzleB_01", "nope"};
    f.beams = {tr, ew, sl, zp};
    const std::vector<render::BeamInstance> out = render::beams_from_frame(f, &styles, bone);
    CHECK(out.size() == 4);
    if (out.size() == 4) {
        CHECK(out[0].kind == 30 && out[0].start.x == 5.0f && out[0].start.z == 7.0f && out[0].end.x == 600.0f && out[0].end.z == 12.0f);
        CHECK(out[0].continuous);
        CHECK(out[1].kind == 31 && out[1].end.x == 610.0f && out[1].end.y == 3.0f && out[1].end.z == 4.0f);  // the hardpoint's bone
        CHECK(out[2].kind == 55 && out[2].start.x == 9.0f && out[2].end.y == 500.0f);  // MuzzleA_00, target centre (type 1 has z adjust only via the record: 0)
        CHECK(out[3].kind == 56 && !out[3].continuous && out[3].start.y == 50.0f && out[3].end.y == 80.0f);  // nearest bone to the end point
        CHECK(out[0].key != out[1].key && out[3].key != beam_key(f.beams[0]));
    }
    // no bone source: owner position, target centre + z
    const std::vector<render::BeamInstance> nb = render::beams_from_frame(f, &styles, {});
    CHECK(nb.size() == 4 && nb[0].start.x == 0.0f && nb[1].end.x == 600.0f && nb[1].end.z == 12.0f);
    // hidden / missing endpoints are left out
    f.objects[1].flags = render::kSnapFogHidden;  // the tractor / energy target
    CHECK(render::beams_from_frame(f, &styles, bone).size() == 2);
    f.objects[0].flags = render::kSnapContained;  // the owner
    CHECK(render::beams_from_frame(f, &styles, bone).empty());
    f.objects.erase(f.objects.begin() + 2);
    f.objects[0].flags = 0;
    CHECK(render::beams_from_frame(f, &styles, bone).size() == 1);  // only the zap (fixed end) remains
}

static void test_snapshot_fields() {
    render::SimSnapshot a, b;
    a.objects = {obj(1, 0, 0, 0, 0, 0), obj(2, 0, 100, 0, 0, 1)};
    b = a;
    CHECK(a.digest() == b.digest());
    render::SnapshotBeam sb;
    sb.kind = render::kBeamTractor;
    sb.owner = 1;
    sb.target = 2;
    sb.style = render::default_beam_style(render::kBeamTractor, {});
    b.beams.push_back(sb);
    CHECK(a.digest() != b.digest());
    render::SimSnapshot c = b;
    CHECK(b.digest() == c.digest());
    c.beams[0].style.width = 11.0f;
    CHECK(b.digest() != c.digest());
    c = b;
    c.beams[0].hardpoint = 3;
    CHECK(b.digest() != c.digest());
    render::InterpolatedFrame f;
    render::interpolate(&a, b, 0.5f, render::InterpMode::Smooth, f);
    CHECK(f.beams.size() == 1 && f.beams[0].owner == 1 && f.beams[0].style.width == 10.0f);
    render::interpolate(nullptr, a, 1.0f, render::InterpMode::Original, f);
    CHECK(f.beams.empty());
}

// ------------------------------------------------------------------------------------------------ real abilities
namespace tc = alamo::tactical::combat;
namespace ta = alamo::tactical::abilities;

static uint32_t push_unit(tc::CombatData& cd, ta::AbilityData& ad, const tc::UnitDesc& u, ta::AbilityObjectDesc a) {
    cd.units.push_back(u);
    a.name = u.name;
    a.name_crc = name_crc(u.name);
    a.category_mask = u.category_mask;
    ta::link_combat_abilities(a, cd, cd.units.back());
    ad.objects.push_back(std::move(a));
    return static_cast<uint32_t>(cd.units.size() - 1);
}
static ta::UnitAbilityRecord gui_record(uint8_t type, float exp, float rech, const char* name) {
    ta::UnitAbilityRecord r;
    r.type = type;
    r.combat.ability_type = type;
    r.expiration_seconds = exp;
    r.recharge_seconds = rech;
    r.gui_activated_ability_name = name;
    r.gui_name_crc = name_crc(name);
    return r;
}
static uint32_t add_special(ta::AbilityData& ad, ta::SpecialAbilityDesc s) {
    s.name_crc = name_crc(s.name);
    ad.specials.push_back(std::move(s));
    return static_cast<uint32_t>(ad.specials.size() - 1);
}

struct Sandbox {
    tc::CombatData cd;
    ta::AbilityData ad;
    std::unique_ptr<tc::CombatSystem> sys;
    std::unique_ptr<ta::AbilitySystem> ab;
    tactical::TreeSet trees{2};
    SimRng rng{4242u};
    uint32_t frame = 1;
    uint32_t sd = 0, corvette = 0, frigate = 0, boat = 0;

    Sandbox() {
        tc::ProjectileDesc laser;
        laser.name = "Laser";
        laser.max_speed = 60.0f;
        laser.damage = 2.0f;
        laser.category = tc::ProjectileCategory::Laser;
        tc::ProjectileDesc missile = laser;
        missile.name = "Missile";
        missile.category = tc::ProjectileCategory::Missile;
        missile.max_speed = 5.0f;
        missile.max_flight_distance = 5000.0f;
        cd.projectiles = {laser, missile};
        tc::HardpointDesc tb;
        tb.name = "HP_Tractor";
        tb.kind = tc::HardpointKind::TractorBeam;
        tb.health = 100.0f;
        tb.destroyable = tb.targetable = true;
        tc::HardpointDesc gen = tb;
        gen.name = "HP_Gen";
        gen.kind = tc::HardpointKind::ShieldGenerator;
        gen.attach_offset = {40, 0, 0};
        cd.hardpoints = {tb, gen};
        ta::SpecialAbilityDesc s;
        s.style = ta::ActivationStyle::UserInput;
        s.applicable_categories = ~uint64_t{0};
        s.name = "Tractor";
        s.cls = ta::SpecialClass::TractorBeam;
        s.max_range = 1000.0f;
        s.target_speed_decrease = 0.75f;
        const uint32_t tractor_sp = add_special(ad, s);
        s = ta::SpecialAbilityDesc{};
        s.style = ta::ActivationStyle::UserInput;
        s.applicable_categories = ~uint64_t{0};
        s.name = "EnergyWeapon";
        s.cls = ta::SpecialClass::EnergyWeapon;
        s.max_range = 800.0f;
        s.damage_per_frame = 50.0f;
        const uint32_t ew_sp = add_special(ad, s);
        s = ta::SpecialAbilityDesc{};
        s.style = ta::ActivationStyle::UserInput;
        s.name = "LaserDef";
        s.cls = ta::SpecialClass::LaserDefense;
        s.defense_duration = 10.0f;
        s.protection_radius = 400.0f;
        s.zap_recharge = 0.25f;
        s.dist_travelled = 40.0f;
        s.targeted_categories = 1u << 1;
        const uint32_t ld_sp = add_special(ad, s);
        tc::UnitDesc u;
        u.name = "SD";
        u.tactical_health = 5000.0f;
        u.radius = 60.0f;
        u.category_mask = 8;
        u.hardpoints = {0};
        u.max_attack_distance = 1500.0f;
        ta::AbilityObjectDesc a;
        a.unit_abilities = {gui_record(ta::kTractorBeam, 0.0f, 5.0f, "Tractor"), gui_record(ta::kEnergyWeapon, 2.0f, 10.0f, "EnergyWeapon")};
        a.specials = {tractor_sp, ew_sp};
        sd = push_unit(cd, ad, u, a);
        tc::UnitDesc cv;
        cv.name = "Corvette";
        cv.tactical_health = 400.0f;
        cv.radius = 20.0f;
        cv.category_mask = 2;
        cv.hardpoints = {1};
        ta::AbilityObjectDesc ca;
        ca.unit_abilities = {gui_record(ta::kLaserDefense, 0.0f, 30.0f, "LaserDef")};
        ca.specials = {ld_sp};
        corvette = push_unit(cd, ad, cv, ca);
        tc::UnitDesc fr = cv;
        fr.name = "Frigate";
        fr.category_mask = 4;
        fr.hardpoints = {1, 1};
        frigate = push_unit(cd, ad, fr, ta::AbilityObjectDesc{});
        tc::UnitDesc mf;
        mf.name = "MissileBoat";
        mf.tactical_health = 50.0f;
        mf.radius = 5.0f;
        mf.category_mask = 1;
        mf.max_attack_distance = 3000.0f;
        mf.weapon_projectile = 1;
        mf.weapon_recharge = 1.0f;
        mf.weapon_pulse_count = 1;
        boat = push_unit(cd, ad, mf, ta::AbilityObjectDesc{});
        tc::CombatConfig cfg;
        cfg.inaccuracy_enabled = false;
        cfg.opportunity_fire = false;
        cfg.object_max_health_multiplier = 1.0f;
        cfg.hardpoint_health_multiplier = 1.0f;
        ta::AbilityConfig acfg;
        sys = std::make_unique<tc::CombatSystem>(cd, cfg, std::vector<tc::PlayerInfo>{tc::PlayerInfo{}, tc::PlayerInfo{}});
        ab = std::make_unique<ta::AbilitySystem>(*sys, ad, acfg);
        sys->bind_rng(&rng);
    }
    static tactical::Aabb box(tactical::Vec3 p, float r) { return {{p.x - r, p.y - r, p.z - r}, {p.x + r, p.y + r, p.z + r}}; }
    void add(tc::ObjectId id, uint32_t desc, int32_t owner, tactical::Vec3 pos) {
        tc::UnitSpawn s;
        s.id = id;
        s.desc = desc;
        s.owner = owner;
        s.position = pos;
        CHECK(ab->add_unit(s, desc, frame, rng));
        tactical::TreeMembership m;
        m.owner = owner;
        CHECK(trees.add(static_cast<tactical::EntityHandle>(id), box(pos, cd.units[desc].radius), 1u, m));
    }
    void tick() {
        ab->tick(frame, trees, rng);
        for (const tc::DeathEvent& d : sys->deaths()) trees.remove(static_cast<tactical::EntityHandle>(d.id));
        ++frame;
    }
    render::SimSnapshot snapshot(std::initializer_list<render::SnapshotObject> objs) const {
        render::SimSnapshot s;
        s.objects = objs;
        return s;
    }
};

static void test_apply_beams() {
    // tractor beam (30): applied special + its target
    {
        Sandbox w;
        w.add(1, w.sd, 0, {0, 0, 0});
        w.add(2, w.corvette, 1, {500, 0, 0});
        render::SimSnapshot snap = w.snapshot({obj(1, 0, 0, 0, 0, 0), obj(2, 1, 500, 0, 0, 1)});
        render::apply_beams(*w.ab, *w.sys, w.frame, snap, nullptr);
        CHECK(snap.beams.empty());  // nothing active
        const tc::ObjectId sd[] = {1};
        CHECK(w.ab->targeted_ability(sd, ta::kTractorBeam, 2, -1, w.frame) == 1);
        w.tick();
        w.tick();
        render::apply_beams(*w.ab, *w.sys, w.frame, snap, nullptr);
        CHECK(snap.beams.size() == 1);
        if (snap.beams.size() == 1) {
            const render::SnapshotBeam& b = snap.beams[0];
            CHECK(b.kind == 30 && b.owner == 1 && b.target == 2 && !b.fixed_end && b.hardpoint == -1 && b.frame == w.frame);
            CHECK(b.style.mode == 0 && b.style.dots == 4 && b.style.lightning == "Tractor_Beam" && b.style.start_bone == "HP_trac_bone");
        }
        // the style table's fields win over the GC fallbacks; Z adjust comes from the target's type
        render::BeamStyleTable styles;
        render::BeamStyle custom = render::default_beam_style(render::kBeamTractor, {});
        custom.width = 77.0f;
        styles.add(name_crc("Tractor"), custom);
        styles.set_z_adjust(1, 9.0f);
        render::apply_beams(*w.ab, *w.sys, w.frame, snap, &styles);
        CHECK(snap.beams.size() == 1 && snap.beams[0].style.width == 77.0f && snap.beams[0].end_z_adjust == 9.0f);
        // the target out of the snapshot (not drawn): no record
        render::SimSnapshot lone = w.snapshot({obj(1, 0, 0, 0, 0, 0)});
        render::apply_beams(*w.ab, *w.sys, w.frame, lone, nullptr);
        CHECK(lone.beams.empty());
        // released: the record disappears
        CHECK(w.ab->set_unit_ability_mode(sd, ta::kTractorBeam, false, w.frame) == 1);
        w.tick();
        w.tick();
        render::apply_beams(*w.ab, *w.sys, w.frame, snap, nullptr);
        CHECK(snap.beams.empty());
    }
    // energy weapon (31): the targeted hardpoint (the best one when nothing is ordered)
    {
        Sandbox w;
        w.add(1, w.sd, 0, {0, 0, 0});
        w.add(2, w.frigate, 1, {400, 0, 0});
        const tc::ObjectId sd[] = {1};
        CHECK(w.ab->targeted_ability(sd, ta::kEnergyWeapon, 2, -1, w.frame) == 1);
        w.tick();
        w.tick();
        render::SimSnapshot snap = w.snapshot({obj(1, 0, 0, 0, 0, 0), obj(2, 2, 400, 0, 0, 1)});
        snap.objects[1].hardpoint_count = 2;
        render::apply_beams(*w.ab, *w.sys, w.frame, snap, nullptr);
        CHECK(snap.beams.size() == 1);
        if (snap.beams.size() == 1) {
            const render::SnapshotBeam& b = snap.beams[0];
            CHECK(b.kind == 31 && b.owner == 1 && b.target == 2 && b.style.dots == 10 && b.style.dot_dir == 1 &&
                  b.style.lightning == "Piet_Energy_Weapon" && b.style.width == 30.0f);
            CHECK(b.hardpoint == w.sys->choose_target_hardpoint(1, 2) || b.hardpoint == w.sys->ordered_hardpoint(1));
            CHECK(b.hardpoint >= 0 && b.hardpoint < 2);
        }
    }
    // laser defense (56): (a) the event path (host reads AbilityEvent::Zap before clearing: end toward the shooter, [M]);
    // (b) the game path: the host's tick has cleared the event, the zap comes from two consecutive snapshots' projectiles
    // and ends exactly one flight step on from the vanished projectile's last position.
    {
        Sandbox w;
        w.add(1, w.corvette, 0, {0, -1000, 0});
        w.add(4, w.boat, 1, {0, -1500, 0});
        w.sys->order_attack(4, 1);
        const tc::ObjectId cv[] = {1};
        CHECK(w.ab->set_unit_ability_mode(cv, ta::kLaserDefense, true, w.frame) == 1);
        render::BeamStyleTable zstyles;  // Protection_Radius 400 comes from the type XML in the game
        render::BeamStyle zs = render::default_beam_style(render::kBeamLaserZap, {});
        zs.radius = 400.0f;
        zstyles.add(name_crc("LaserDef"), zs);
        render::SimSnapshot prev;
        bool have_prev = false;
        int events = 0, event_beams = 0, snapshot_beams = 0;
        for (int i = 0; i < 300 && snapshot_beams < 3; ++i) {
            w.tick();
            bool ev = false;
            for (const ta::AbilityEvent& e : w.ab->events())
                if (e.kind == ta::AbilityEventKind::Zap && e.id == 1) ev = true;
            events += ev ? 1 : 0;
            const tactical::Vec3 cp = w.sys->position(1);
            auto fresh = [&] {
                render::SimSnapshot snap = w.snapshot({obj(1, 1, cp.x, cp.y, cp.z, 0), obj(4, 3, 0, -1500, 0, 1)});
                for (size_t k = 0; k < w.sys->projectile_count(); ++k) {
                    const tc::ProjectileView v = w.sys->projectile(k);
                    if (!v.alive) continue;
                    render::SnapshotProjectile sp;
                    sp.serial = v.serial;
                    sp.desc = v.desc;
                    sp.owner = v.owner;
                    sp.shooter = v.shooter;
                    sp.position = {v.pos.x, v.pos.y, v.pos.z};
                    sp.direction = {v.dir.x, v.dir.y, v.dir.z};
                    sp.speed = v.speed;
                    snap.projectiles.push_back(sp);
                }
                return snap;
            };
            render::SimSnapshot a = fresh();
            render::apply_beams(*w.ab, *w.sys, w.frame, a, &zstyles);  // (a) event still pending
            for (const render::SnapshotBeam& b : a.beams)
                if (b.kind == 56) {
                    ++event_beams;
                    CHECK(ev && b.fixed_end && b.owner == 1 && b.style.mode == 0 && b.style.lifetime == 2);
                    const float dy = b.end.y - cp.y;
                    CHECK(dy < 0.0f && std::fabs(dy) <= 400.01f);  // 400 from the owner towards the shooter (the boat is at -y)
                }
            w.ab->clear_events();  // what TacticalBattle::tick does before the host captures
            render::SimSnapshot g = fresh();
            render::apply_beams(*w.ab, *w.sys, w.frame, g, &zstyles);
            CHECK(g.beams.empty());  // no event any more: nothing from the event path
            render::apply_beam_zaps(*w.ab, *w.sys, w.frame, g, have_prev ? &prev : nullptr, &zstyles);
            size_t zaps = 0;
            for (const render::SnapshotBeam& b : g.beams)
                if (b.kind == 56) {
                    ++zaps;
                    ++snapshot_beams;
                    CHECK(b.fixed_end && b.owner == 1 && b.frame == w.frame && b.style.lifetime == 2);
                    // exactly the vanished projectile's previous position + one step, inside Protection_Radius
                    bool match = false;
                    for (const render::SnapshotProjectile& p : prev.projectiles) {
                        const float ex = p.position.x + p.direction.x * p.speed, ey = p.position.y + p.direction.y * p.speed,
                                    ez = p.position.z + p.direction.z * p.speed;
                        match = match || (std::fabs(ex - b.end.x) < 1e-3f && std::fabs(ey - b.end.y) < 1e-3f && std::fabs(ez - b.end.z) < 1e-3f);
                    }
                    CHECK(match);
                    const float dx = b.end.x - cp.x, dy = b.end.y - cp.y, dz = b.end.z - cp.z;
                    CHECK(std::sqrt(dx * dx + dy * dy + dz * dz) <= 400.5f);
                }
            CHECK(zaps == (ev && have_prev ? 1u : 0u));  // a zap exactly on the ticks the sim zapped
            prev = g;
            have_prev = true;
        }
        CHECK(events >= 3 && event_beams >= 3 && snapshot_beams >= 3);
    }
}

// ------------------------------------------------------------------------------------------------ GPU
using Gpu = render::test::GpuCore;
using Offscreen = render::test::OffscreenWithDepth;  // alamo/render/offscreen.hpp

static bool render_frame(Gpu& g, render::Renderer& r, Offscreen& o, const render::ViewMatrices& vm, render::DrawStats& stats,
                         const float* clear) {
    rhi::Frame* f = g.ring.begin_frame();
    if (!f) return false;
    VkCommandBuffer cmd = f->cmd();
    const rhi::DeviceFns& fn = g.ctx.vkd();
    const rhi::AcquireResult acq = g.up.record_acquires(cmd);
    render::PassTarget pt;
    pt.color = o.color.handle;
    pt.color_view = o.color.view;
    pt.depth = o.depth.handle;
    pt.depth_view = o.depth.view;
    pt.extent = {o.w, o.h};
    std::memcpy(pt.clear, clear, sizeof pt.clear);
    pt.color_after = rhi::ImageState::TransferSrc;
    render::RecordOptions ro;
    ro.post = false;
    const bool drawn = r.render(*f, cmd, pt, vm, {}, render::DrawMode::Lit, ro, &stats, nullptr, {});
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

// The beams on black with the shipped textures: lit pixels around each beam, tractor green, energy weapon orange, super
// laser green; PNG logs\render_beams.png.
static void test_beams_gpu(Gpu& g, const std::string& src_dir) {
    const std::string root = env_or_empty("ALAMO_FOC_DATA");
    if (root.empty()) {
        std::printf("beams gpu: ALAMO_FOC_DATA not set, skipped\n");
        return;
    }
    vfs::VfsConfig cfg;
    cfg.foc_root = root;
    if (const std::string eaw = env_or_empty("ALAMO_EAW_DATA"); !eaw.empty()) cfg.eaw_root = eaw;
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
    rr.set_lighting(render::Lighting::space_default());
    const render::TextureSource tex = [&](std::string_view n, assets::TextureRole r) { return cache.texture(n, r); };
    render::BeamConstants gc;
    {
        std::vector<uint8_t> xml;
        if (files->read("Data\\XML\\GameConstants.xml", xml)) CHECK(render::parse_beam_constants(xml, gc));
    }
    CHECK(gc.tractor.width == 10.0f && gc.tractor.color[1] == 75 && gc.energy.width == 30.0f);
    render::LightningLibrary lightning = render::LightningLibrary::builtin();
    {
        std::vector<uint8_t> xml;
        if (files->read("Data\\XML\\LightningEffectTypes.xml", xml)) CHECK(lightning.load(xml));
    }
    CHECK(lightning.find("Tractor_Beam") && lightning.find("Piet_Energy_Weapon") && lightning.find("Leech_Shields"));
    CHECK(rr.init_beams(tex, &lightning) && rr.beams_ready());
    Offscreen o;
    CHECK(o.create(g, 960, 540));
    render::OrbitCamera cam;
    cam.target = {0, 0, 0};
    cam.yaw = 0.0f;
    cam.pitch = 1.5f;
    cam.distance = 600.0f;
    cam.fov_x = render::tactical_fov_x(55.0f, 960.0f / 540.0f);
    const render::ViewMatrices v = render::make_view(cam, o.w, o.h, 10.0f, 7000.0f);
    // tractor (y = +120), energy weapon (y = 0), super laser (y = -120), zap (y = -200), all 400 long along X
    render::BeamInstance beams[4] = {make_beam(render::kBeamTractor, {-200, 120, 0}, {200, 120, 0}, gc),
                                     make_beam(render::kBeamEnergyWeapon, {-200, 0, 0}, {200, 0, 0}, gc),
                                     make_beam(render::kBeamSuperLaser, {-200, -120, 0}, {200, -120, 0}, gc),
                                     make_beam(render::kBeamLaserZap, {-200, -200, 0}, {200, -200, 0}, gc)};
    for (int i = 0; i < 4; ++i) beams[i].key = static_cast<uint64_t>(i + 1);
    beams[3].continuous = false;
    const float clear[4] = {0, 0, 0, 1};
    render::DrawStats st;
    uint64_t tick = 0;
    bool done = false;
    for (int k = 0; k < 300 && !done; ++k) {
        rr.set_beams(beams, tick);  // the tractor / energy weapon animate over the loop; the zap lives 2 ticks
        CHECK(render_frame(g, rr, o, v, st, clear));
        done = k > 2 && st.beam_pending == 0 && st.beam_ribbons >= 3;
        rr.pipelines().wait_idle();
        if (k > 2) ++tick;
    }
    std::printf("beams gpu: %u ribbons, %u dots, %u bolts, %u pending; %zu vertices, %zu batches\n", st.beam_ribbons, st.beam_dots, st.beam_bolts,
                st.beam_pending, rr.last_beam_geometry().vertices.size(), rr.last_beam_geometry().batches.size());
    CHECK(st.beam_ribbons >= 3 && st.beam_pending == 0 && st.beam_dots >= 14);
    const std::vector<uint8_t> px = o.pixels();
    CHECK(render::write_png_rgba(src_dir + "/logs/render_beams.png", o.w, o.h, [&] {
        std::vector<uint8_t> p = px;
        for (size_t i = 3; i < p.size(); i += 4) p[i] = 255;
        return p;
    }()));
    auto region = [&](float wx, float wy, double (&sum)[3]) {
        const render::Float4 c = v.view_proj * render::Float4{wx, wy, 0.0f, 1.0f};
        const int cx = static_cast<int>((c.x / c.w + 1.0f) * 0.5f * static_cast<float>(o.w));
        const int cy = static_cast<int>((1.0f - c.y / c.w) * 0.5f * static_cast<float>(o.h));
        size_t lit = 0;
        sum[0] = sum[1] = sum[2] = 0;
        for (int y = cy - 14; y <= cy + 14; ++y)
            for (int x = cx - 40; x <= cx + 40; ++x) {
                if (x < 0 || y < 0 || x >= static_cast<int>(o.w) || y >= static_cast<int>(o.h)) continue;
                const uint8_t* q = px.data() + 4 * (static_cast<size_t>(y) * o.w + static_cast<size_t>(x));
                for (int ch = 0; ch < 3; ++ch) sum[ch] += q[ch];
                lit += (q[0] + q[1] + q[2]) > 30 ? 1u : 0u;
            }
        return lit;
    };
    double s[3];
    CHECK(region(0.0f, 120.0f, s) > 20 && s[1] > s[0]);        // tractor: dark green (0, 75, 0) dominates
    CHECK(region(0.0f, 0.0f, s) > 20 && s[0] > s[2]);          // energy weapon: orange (255, 75, 0)
    CHECK(region(0.0f, -120.0f, s) > 20 && s[1] > s[0]);       // super laser: green (0, 237, 119)
    // no beams: nothing drawn, the pass is skipped
    rr.set_beams({}, tick + 40);  // past the zap lifetime; the super laser enters its continuation
    rr.set_beams({}, tick + 80);  // and ends it
    render::DrawStats st2;
    CHECK(render_frame(g, rr, o, v, st2, clear));
    CHECK(st2.beam_ribbons == 0 && st2.beam_pending == 0);
    const std::vector<uint8_t> px2 = o.pixels();
    size_t lit2 = 0;
    for (size_t i = 0; i < px2.size(); i += 4) lit2 += (px2[i] + px2[i + 1] + px2[i + 2]) > 8 ? 1u : 0u;
    CHECK(lit2 == 0);
    o.destroy(g);
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    log::set_min_level(log::Level::Error);
    const std::string src_dir = ALAMO_RENDER_SOURCE_DIR;
    test_constants_and_styles();
    test_geometry();
    test_animation();
    test_geometry_build();
    test_frame_resolution();
    test_snapshot_fields();
    test_apply_beams();
    const bool strict = render::setup_validation_env(src_dir);
    {
        rhi::Loader probe;
        if (!probe.open()) {
            std::printf("SKIP gpu: no Vulkan loader\n");
            std::printf(g_fail ? "alamo_render_beams_tests: %d failure(s)\n" : "alamo_render_beams_tests: OK (cpu)\n", g_fail);
            return g_fail ? 1 : 0;
        }
    }
    Gpu g;
    rhi::ContextDesc cd;
    cd.app_name = "alamo_render_beams_tests";
    cd.validation = strict ? rhi::Validation::Required : rhi::Validation::IfAvailable;
    if (!g.ctx.init(cd)) {
        std::printf("SKIP gpu: no suitable Vulkan 1.3 device\n");
        std::printf(g_fail ? "alamo_render_beams_tests: %d failure(s)\n" : "alamo_render_beams_tests: OK (cpu)\n", g_fail);
        return g_fail || strict ? 1 : 0;
    }
    rhi::FrameRingDesc fd;
    fd.frames_in_flight = 2;
    fd.uniform_bytes = 16ull << 20;
    fd.recording_threads = jobs::JobSystem::default_worker_count() + 1;
    CHECK(g.alloc.init(g.ctx) && g.up.init(g.ctx, g.alloc) && g.ring.init(g.ctx, g.alloc, fd));
    test_beams_gpu(g, src_dir);
    g.ring.shutdown();
    g.up.shutdown();
    g.alloc.shutdown();
    const uint32_t ve = g.ctx.validation_errors(), vw = g.ctx.validation_warnings();
    std::printf("gpu: validation %u errors, %u warnings\n", ve, vw);
    CHECK(ve == 0 && vw == 0);
    g.ctx.shutdown();
    std::printf(g_fail ? "alamo_render_beams_tests: %d failure(s)\n" : "alamo_render_beams_tests: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
