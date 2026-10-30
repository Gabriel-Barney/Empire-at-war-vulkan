// alamo_render_sim_tests - R1 sim -> render snapshot (no framework; non-zero exit on failure).
//   * math: lerp / facing nlerp / tick alpha / object_transform (= terrain::placement_transform for yaw)
//   * SnapshotBuffer double buffering, interpolate() original vs smooth
//   * S7 t05 attitude: object_transform pitch + roll (FIGHTER_FLYING §1.1), apply_snapshot_attitude, angle interpolation
//   * read-only: the synthetic map for 600 ticks with and without a snapshot + interpolation + instance build
//     after every tick -> identical per-tick CRC logs; the store digest is unchanged by a capture
//   * thread determinism: snapshot digests per tick identical for 0/1/4/N workers and serial building
//   * instance conversion: transform, team colour (B.3), LOD_Bias, fade, background split, sticky hardpoints
//   * GPU (skipped without a Vulkan 1.3 device): the synthetic sim map drawn offscreen (debug boxes, unlit)
//     with 0 validation messages, PNG logs/render_sim_synthetic.png; the CRC log of that run = the headless one
//   * real data (ALAMO_FOC_DATA): FoC _SPACE_PLANET_CORELLIA_PIRACY and _LAND_PLANET_BYSS_KIDNAPPING, 600 ticks
//     with / without snapshots (GameTypeResolver type table) -> identical CRC logs
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/core/log.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/jobs/crc_recorder.hpp"
#include "alamo/jobs/job_system.hpp"
#include "alamo/render/png.hpp"
#include "alamo/render/sim_session.hpp"
#include "alamo/render/sim_snapshot.hpp"
#include "alamo/render/sim_view.hpp"
#include "alamo/rhi/rhi.hpp"
#include "alamo/terrain/placement.hpp"
#include "alamo/vfs/vfs.hpp"

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

static bool near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

// ------------------------------------------------------------------------------------------- math
static void test_math() {
    CHECK(render::interp_lerp(2.0f, 6.0f, 0.25f) == 3.0f);
    const Vec3 a{1, 0, 0}, b{0, 1, 0};
    const Vec3 h = render::interp_facing(a, b, 0.5f);
    CHECK(near(h.x, 0.70710678f) && near(h.y, 0.70710678f) && h.z == 0.0f);
    const Vec3 opp = render::interp_facing(a, Vec3{-1, 0, 0}, 0.5f);  // degenerate -> b
    CHECK(opp.x == -1.0f && opp.y == 0.0f);
    CHECK(render::tick_alpha(1000, 1000, 33) == 0.0f);
    CHECK(near(render::tick_alpha(1011, 1000, 33), 11.0f / 33.0f));
    CHECK(render::tick_alpha(1040, 1000, 33) == 1.0f);
    CHECK(near(render::tick_alpha(5, 0xFFFFFFF0u, 33), 21.0f / 33.0f));  // wrap
    CHECK(render::tick_alpha(7, 3, 0) == 1.0f);
    // object_transform of the facing (sin yaw, -cos yaw, 0) == placement_transform of yaw (models face -Y).
    for (float yaw : {0.0f, 37.0f, 90.0f, 181.5f, 300.0f}) {
        const float r = yaw * 0.017453292519943295f;
        const float pos[3] = {10, -20, 5}, rot[3] = {0, 0, yaw};
        const assets::Mat3x4 p = terrain::placement_transform(pos, rot, 2.5f);
        const assets::Mat3x4 o = render::object_transform({10, -20, 5}, {std::sin(r), -std::cos(r), 0}, 2.5f);
        bool same = true;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 4; ++j) same = same && near(p.m[i][j], o.m[i][j], 1e-4f);
        CHECK(same);
    }
    const assets::Mat3x4 z = render::object_transform({0, 0, 0}, {0, 0, 0}, 1.0f);  // zero facing -> identity
    CHECK(z.m[0][0] == 1.0f && z.m[1][1] == 1.0f && z.m[2][2] == 1.0f);
    const assets::Mat3x4 up = render::object_transform({0, 0, 0}, {0, 0, 2}, 1.0f);  // straight up: orthonormal
    CHECK(near(up.m[2][1], -1.0f) && near(up.m[0][0] * up.m[0][0] + up.m[1][0] * up.m[1][0] + up.m[2][0] * up.m[2][0], 1.0f));
}

// ----------------------------------------------------------------------------- buffer + interpolate
static render::SnapshotObject obj(int32_t id, float x, float fade = 1.0f) {
    render::SnapshotObject o;
    o.id = id;
    o.type = 0;
    o.position = {x, 2 * x, 0};
    o.facing = {1, 0, 0};
    o.health = x;
    o.fade = fade;
    return o;
}

static void test_buffer_interp() {
    render::SnapshotBuffer buf;
    CHECK(!buf.latest().curr && !buf.latest().prev);
    auto s1 = std::make_shared<render::SimSnapshot>();
    s1->tick = 1;
    s1->objects = {obj(1, 0), obj(2, 10), obj(4, 100)};
    auto s2 = std::make_shared<render::SimSnapshot>();
    s2->tick = 2;
    s2->objects = {obj(1, 4), obj(3, 50), obj(4, 104, 0.0f)};
    s2->objects[0].facing = {0, 1, 0};
    buf.publish(s1);
    CHECK(buf.latest().curr == s1 && !buf.latest().prev);
    buf.publish(s2);
    const render::SnapshotBuffer::Pair p = buf.latest();
    CHECK(p.prev == s1 && p.curr == s2 && buf.published() == 2);
    CHECK(s2->find(3) == &s2->objects[1] && s2->find(2) == nullptr);

    render::InterpolatedFrame f;
    render::interpolate(p.prev.get(), *p.curr, 0.25f, render::InterpMode::Original, f);
    CHECK(f.objects.size() == 3 && f.objects[0].position.x == 4.0f && f.objects[2].fade == 0.0f && f.tick == 2);
    render::interpolate(p.prev.get(), *p.curr, 0.25f, render::InterpMode::Smooth, f);
    CHECK(f.objects.size() == 3);                                               // id 2 (gone) dropped
    CHECK(f.objects[0].position.x == 1.0f && f.objects[0].position.y == 2.0f);  // 0 -> 4 at 0.25
    CHECK(f.objects[0].facing.x > f.objects[0].facing.y && near(std::hypot(f.objects[0].facing.x, f.objects[0].facing.y), 1.0f));
    CHECK(f.objects[1].id == 3 && f.objects[1].position.x == 50.0f);            // new this tick: current state
    CHECK(f.objects[2].position.x == 101.0f && f.objects[2].fade == 0.75f);     // dying: fades out over the tick
    render::interpolate(p.prev.get(), *p.curr, 7.0f, render::InterpMode::Smooth, f);  // clamped to 1 = current
    CHECK(f.alpha == 1.0f && f.objects[0].position.x == 4.0f);
    render::interpolate(p.prev.get(), *p.curr, std::nanf(""), render::InterpMode::Smooth, f);
    CHECK(f.alpha == 1.0f);
    render::interpolate(nullptr, *p.curr, 0.0f, render::InterpMode::Smooth, f);  // no previous: current
    CHECK(f.objects[0].position.x == 4.0f);
    buf.clear();
    CHECK(!buf.latest().curr);
}

// S7 t05: movement attitude (fighter / flyer roll and pitch) - transform, snapshot fill, digest, interpolation.
static Vec3 column(const assets::Mat3x4& m, int c) { return {m.m[0][c], m.m[1][c], m.m[2][c]}; }
static bool near3v(const Vec3& a, const Vec3& b, float eps = 1e-5f) {
    return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}

static void test_attitude() {
    // pitch 0 = exactly the roll-only transform; the SnapshotObject overload = the explicit one.
    const assets::Mat3x4 r0 = render::object_transform({1, 2, 3}, {0.6f, 0.8f, 0}, 2.0f, 0.4f);
    const assets::Mat3x4 r1 = render::object_transform({1, 2, 3}, {0.6f, 0.8f, 0}, 2.0f, 0.4f, 0.0f);
    CHECK(std::memcmp(&r0, &r1, sizeof r0) == 0);
    render::SnapshotObject so;
    so.position = {1, 2, 3};
    so.facing = {0.6f, 0.8f, 0};
    so.roll = 0.4f;
    so.pitch = -0.3f;
    const assets::Mat3x4 a0 = render::object_transform(so, 2.0f), a1 = render::object_transform({1, 2, 3}, {0.6f, 0.8f, 0}, 2.0f, 0.4f, -0.3f);
    CHECK(std::memcmp(&a0, &a1, sizeof a0) == 0);
    // FIGHTER_FLYING §1.1: orientation (R, P, Y) on the level facing (cos Y, sin Y, 0) = yaw, then pitch about the left
    // axis (positive = nose down), then roll about the forward axis (right-handed): nose = (cY cP, sY cP, -sP), left =
    // Rz(Y) (sR sP, cR, sR cP), up = Rz(Y) (cR sP, -sR, cR cP). Model columns: X = left, -Y = nose, Z = up.
    for (const float yd : {0.0f, 30.0f, 135.0f, -100.0f})
        for (const float pd : {0.0f, 20.0f, -35.0f, 89.0f, 100.0f})
            for (const float rd : {0.0f, -45.0f, 70.0f, 190.0f}) {
                const float Y = yd * 0.017453292f, P = pd * 0.017453292f, R = rd * 0.017453292f;
                const float cy = std::cos(Y), sy = std::sin(Y), cp = std::cos(P), sp = std::sin(P), cr = std::cos(R), sr = std::sin(R);
                const assets::Mat3x4 m = render::object_transform({5, 6, 7}, {cy, sy, 0}, 1.0f, R, P);
                auto yaw = [&](Vec3 v) { return Vec3{v.x * cy - v.y * sy, v.x * sy + v.y * cy, v.z}; };
                const Vec3 nose{cy * cp, sy * cp, -sp};
                const Vec3 y = column(m, 1);
                CHECK(near3v(Vec3{-y.x, -y.y, -y.z}, nose, 2e-5f));
                CHECK(near3v(column(m, 0), yaw({sr * sp, cr, sr * cp}), 2e-5f));
                CHECK(near3v(column(m, 2), yaw({cr * sp, -sr, cr * cp}), 2e-5f));
                CHECK(m.m[0][3] == 5.0f && m.m[1][3] == 6.0f && m.m[2][3] == 7.0f);
            }
    // Sign conventions: a left turn banks negative (§3.2) -> the left wing (model +X) goes down; positive pitch noses down.
    const assets::Mat3x4 bank = render::object_transform({0, 0, 0}, {1, 0, 0}, 1.0f, -0.5f, 0.0f);
    CHECK(column(bank, 0).z < -0.4f && near3v(column(bank, 1), {-1, 0, 0}));
    const assets::Mat3x4 dive = render::object_transform({0, 0, 0}, {1, 0, 0}, 1.0f, 0.0f, 0.5f);
    CHECK(column(dive, 1).z > 0.4f && near3v(column(dive, 0), {0, 1, 0}));  // nose (-Y) below the horizon, wings level

    // apply_snapshot_attitude: live movers get the source's values (non-finite -> 0); corpses keep their spin roll;
    // objects the source does not know keep theirs. The digest covers pitch.
    render::SimSnapshot s;
    s.objects = {obj(1, 0), obj(2, 1), obj(3, 2), obj(4, 3)};
    s.objects[1].flags = render::kSnapCorpse;
    s.objects[1].roll = 0.7f;
    s.objects[2].roll = 0.25f;
    const uint64_t d0 = s.digest();
    render::apply_snapshot_attitude(s, [](objects::ObjectId id, float& r, float& p) {
        if (id == 3) return false;
        r = id == 4 ? std::nanf("") : -0.3f;
        p = id == 4 ? 0.1f : 0.2f;
        return true;
    });
    CHECK(s.objects[0].roll == -0.3f && s.objects[0].pitch == 0.2f);
    CHECK(s.objects[1].roll == 0.7f && s.objects[1].pitch == 0.0f);   // corpse untouched
    CHECK(s.objects[2].roll == 0.25f && s.objects[2].pitch == 0.0f);  // unknown to the source
    CHECK(s.objects[3].roll == 0.0f && s.objects[3].pitch == 0.1f);
    const uint64_t d1 = s.digest();
    CHECK(d1 != d0);
    s.objects[0].pitch = 0.21f;
    CHECK(s.digest() != d1);
    render::apply_snapshot_attitude(s, nullptr);  // no source: unchanged
    CHECK(s.objects[0].pitch == 0.21f);

    // interp_angle: the shorter way across +-pi; an in-range step is exactly interp_lerp.
    CHECK(render::interp_angle(0.25f, 0.75f, 0.3f) == render::interp_lerp(0.25f, 0.75f, 0.3f));
    CHECK(render::interp_angle(-1.0f, 2.0f, 0.5f) == render::interp_lerp(-1.0f, 2.0f, 0.5f));
    CHECK(near(render::interp_angle(3.0f, -3.0f, 0.5f), 3.14159265f, 1e-4f));
    CHECK(near(render::interp_angle(-3.0f, 3.0f, 0.25f), -3.0f - 0.25f * (6.2831853f - 6.0f), 1e-4f));
    CHECK(std::isnan(render::interp_angle(1.0f, std::nanf(""), 0.5f)));  // non-finite step -> b

    // Smooth interpolation: roll / pitch blended the shorter way; a fold (facing reversed while pitched) is not blended.
    render::SimSnapshot p0, p1;
    p0.objects = {obj(1, 0), obj(2, 0)};
    p1.objects = {obj(1, 0), obj(2, 0)};
    p0.objects[0].roll = 3.0f;
    p1.objects[0].roll = -3.0f;
    p0.objects[0].pitch = 0.1f;
    p1.objects[0].pitch = 0.3f;
    p0.objects[1].pitch = 1.55f;  // nose near straight down ...
    p0.objects[1].roll = 0.2f;
    p1.objects[1].facing = {-1, 0, 0};  // ... then folded: yaw + 180, roll + 180, pitch back below 90
    p1.objects[1].pitch = 1.5f;
    p1.objects[1].roll = 0.2f - 3.14159265f;
    render::InterpolatedFrame f;
    render::interpolate(&p0, p1, 0.5f, render::InterpMode::Smooth, f);
    CHECK(near(std::fabs(f.objects[0].roll), 3.14159265f, 1e-4f) && near(f.objects[0].pitch, 0.2f));
    CHECK(f.objects[1].facing.x == -1.0f && f.objects[1].pitch == 1.5f && f.objects[1].roll == p1.objects[1].roll);
    render::interpolate(&p0, p1, 0.5f, render::InterpMode::Original, f);
    CHECK(f.objects[0].roll == -3.0f && f.objects[0].pitch == 0.3f);
}

// Int-B consumers: sim-attached effects (damaged smoke) and a per-(object, index) destroyed-hardpoint source.
static void test_effects_hardpoints() {
    render::SimSnapshot s;
    s.objects = {obj(1, 0), obj(3, 1), obj(5, 2)};
    s.objects[1].hardpoint_count = 3;
    s.objects[1].hardpoint_first = 0;
    s.objects[2].hardpoint_count = 2;
    s.objects[2].hardpoint_first = 3;
    s.hardpoint_destroyed.assign(5, 0);
    const uint64_t d0 = s.digest();
    const std::vector<render::SnapshotEffect> fx = {
        {5, "P_SMOKE_A", 75, 10}, {3, "P_SMOKE_B", 75, 11}, {4, "P_GONE", 75, 12}, {5, "P_SMOKE_A", 50, 13}, {1, "", 75, 14}};
    render::set_snapshot_effects(s, fx);
    // id 4 not in the snapshot and the empty asset dropped; ascending id, report order kept within an id.
    CHECK(s.effects.size() == 3 && s.effects[0].id == 3 && s.effects[1].id == 5 && s.effects[1].percent == 75 &&
          s.effects[2].id == 5 && s.effects[2].percent == 50);
    CHECK(s.digest() != d0);
    // Source: object 3 has 3 hardpoints (index 1 destroyed), object 5 reports 4 (a different list: left alone).
    render::apply_hardpoint_source(
        s, [](objects::ObjectId id) { return id == 3 ? 3u : id == 5 ? 4u : 0u; },
        [](objects::ObjectId id, uint32_t k) { return id == 5 || k == 1; });
    CHECK((s.hardpoint_destroyed == std::vector<uint8_t>{0, 1, 0, 0, 0}));
    render::InterpolatedFrame f;
    render::interpolate(nullptr, s, 1.0f, render::InterpMode::Smooth, f);
    CHECK(f.effects.size() == 3 && f.effects[0].asset == "P_SMOKE_B" && f.hardpoint_destroyed == s.hardpoint_destroyed);
}

// ------------------------------------------------------------------------------------ sim runs
struct FakeModels final : render::SimModelSource {
    const render::GpuModel* model = nullptr;
    const render::SnapshotTypeTable* table = nullptr;
    const render::GpuModel* type_model(objects::TypeIndex t) override {
        return table && table->valid(t) && !(*table)[t].model.empty() ? model : nullptr;
    }
    const render::GpuModel* named_model(const std::string&) override { return nullptr; }
};

static bool near3(const assets::Float3& a, float x, float y, float z, float eps = 1e-3f) {
    return std::fabs(a.x - x) < eps && std::fabs(a.y - y) < eps && std::fabs(a.z - z) < eps;
}

// Projectiles (CombatSystem flat table -> SimSnapshot::projectiles -> instances) and ALO bone billboards.
struct FakeProjectileView {
    uint32_t serial = 0, desc = 0;
    int32_t owner = -1;
    int32_t shooter = -1;
    struct V {
        float x, y, z;
    } pos{0, 0, 0}, dir{1, 0, 0};
    float speed = 0.0f;
    bool alive = true;
};

static void test_projectiles(const render::SyntheticSimWorld& w) {
    render::SnapshotTypeTable table;
    table.build(w.types, objects::ModeKind::Space);
    const objects::TypeIndex frig = w.types.resolve_name("Sim_Frigate");
    CHECK(frig != objects::kNone);
    // set_snapshot_projectiles: dead ones dropped, ascending serial, desc -> type mapping (out of range -> kNone).
    render::SimSnapshot s0, s1;
    std::vector<FakeProjectileView> v = {{9, 0, 1, 5, {10, 0, 0}, {1, 0, 0}, 10.0f, true},
                                         {4, 1, 0, 6, {0, 0, 0}, {0, 1, 0}, 5.0f, true},
                                         {6, 0, 0, 6, {0, 0, 0}, {0, 1, 0}, 5.0f, false}};
    const std::vector<objects::TypeIndex> types = {frig};
    const uint64_t d0 = s1.digest();
    render::set_snapshot_projectiles(s1, v, types);
    CHECK(s1.projectiles.size() == 2 && s1.projectiles[0].serial == 4 && s1.projectiles[1].serial == 9);
    CHECK(s1.projectiles[0].type == objects::kNone && s1.projectiles[1].type == frig && s1.projectiles[1].shooter == 5);
    CHECK(s1.digest() != d0);
    v[0].pos = {0, 0, 0};
    render::set_snapshot_projectiles(s0, std::span<const FakeProjectileView>(v.data(), 1), types);
    // Smooth interpolation by serial; serial 4 is new (current position); Original = current positions.
    render::InterpolatedFrame f;
    render::interpolate(&s0, s1, 0.25f, render::InterpMode::Smooth, f);
    CHECK(f.projectiles.size() == 2 && std::fabs(f.projectiles[1].position.x - 2.5f) < 1e-5f && f.projectiles[0].position.x == 0.0f);
    render::interpolate(&s0, s1, 0.25f, render::InterpMode::Original, f);
    CHECK(f.projectiles.size() == 2 && f.projectiles[1].position.x == 10.0f);
    // Not custom-rendered: object transform (the model's nose -Y along the direction, Scale_Factor) with the type's model.
    render::GpuModel box;
    box.asset = render::debug_box_model("BOX", 1.0f);
    box.uid = 777;
    table.set_model(frig, "BOX");
    render::SnapshotProjectile p;
    p.type = frig;
    p.position = {100, 0, 0};
    p.direction = {0, 1, 0};
    CHECK(near3(render::projectile_transform(p, &table[frig], &box).transform_dir({0, -1, 0}), 0, table[frig].scale, 0));
    render::Laser laser;
    CHECK(!render::projectile_laser(p, table[frig], laser));
    // Builder: model projectiles after the objects; fogged ones are hidden.
    FakeModels models;
    models.model = &box;
    models.table = &table;
    render::SimInstanceBuilder b(table);
    std::vector<render::Instance> main, bg;
    render::SimInstanceStats st;
    render::interpolate(nullptr, s1, 1.0f, render::InterpMode::Original, f);
    b.build(f, models, main, bg, &st);
    CHECK(st.projectiles == 2 && st.projectiles_drawn == 1 && main.size() == 1 && b.lasers().empty());  // serial 4 has no type
    // PROJECTILE_NEBULA_DRAW.md 1: Projectile_Custom_Render 1 / 2 never use the ALO (the model is dropped) and become a
    // procedural laser centred on the position along the flight direction - drawn even though no model is resident.
    table.set_projectile_render(frig, 2, 5.0f, 10.0f, 2, 0);
    table.set_projectile_color(frig, 139, 230, 104, 255);
    CHECK(table[frig].model.empty());
    CHECK(render::projectile_laser(p, table[frig], laser) && laser.kind == render::LaserKind::Kite && laser.width == 5.0f &&
          std::fabs(laser.length - 10.0f * table[frig].scale) < 1e-5f && laser.slot_col == 2 && laser.color[1] == 230);
    CHECK(near3(laser.position, 100, 0, 0) && near3(laser.direction, 0, 1, 0));
    models.model = nullptr;  // no ALO for the type
    b.build(f, models, main, bg, &st);
    CHECK(st.projectiles_drawn == 1 && st.projectiles_lasers == 1 && main.empty() && b.lasers().size() == 1);
    table.set_projectile_render(frig, 1, 1.2f, 12.0f, 0, 0);
    b.build(f, models, main, bg, &st);
    CHECK(b.lasers().size() == 1 && b.lasers()[0].kind == render::LaserKind::Beam);
    models.model = &box;
    table.set_projectile_render(frig, 0, 0.0f, 0.0f);
    table.set_model(frig, "BOX");
    auto fog = std::make_shared<render::FogFrameInput>();
    fog->valid = true;
    fog->width = fog->height = 2;
    fog->x0 = fog->y0 = -50;
    fog->x1 = fog->y1 = 50;
    fog->cell_w = fog->cell_h = 50;
    fog->state.assign(4, 0);  // all fogged
    fog->fade.assign(4, 0);
    f.fog = fog;
    b.build(f, models, main, bg, &st);
    CHECK(st.projectiles_fogged == 2 && st.projectiles_drawn == 0 && main.empty());
}

static void test_billboards() {
    // A disc facing -Y at the parent origin (planet Glow): sun_glow moves it along the sun by distance x parent scale
    // and turns its -Y face to the camera; the column lengths stay.
    assets::Mat3x4 parent;
    for (int i = 0; i < 3; ++i) parent.m[i][i] = 2.0f;
    parent.m[0][3] = 10.0f;
    assets::Mat3x4 world = parent;
    const assets::Float3 eye{10, -100, 1}, right{1, 0, 0}, up{0, 0, 1}, fwd{0, 1, 0}, sun{0, 0, 1};
    const assets::Mat3x4 g = render::billboard_world(world, 6, parent, 0.5f, eye, right, up, fwd, sun);
    CHECK(near3(g.translation(), 10, 0, 1));                 // parent origin + sun x 0.5 x 2
    CHECK(near3(g.transform_dir({0, -1, 0}), 0, -2, 0));     // -Y towards the camera, scale 2 kept
    // sun: at eye + sun x distance x scale.
    CHECK(near3(render::billboard_world(world, 7, parent, 3.0f, eye, right, up, fwd, sun).translation(), 10, -100, 7));
    // none / wind: unchanged; parallel: camera axes.
    CHECK(near3(render::billboard_world(world, 0, parent, 1.0f, eye, right, up, fwd, sun).translation(), 10, 0, 0));
    const assets::Mat3x4 par = render::billboard_world(world, 1, parent, 1.0f, eye, {0, 1, 0}, up, {-1, 0, 0}, sun);
    CHECK(near3(par.transform_dir({1, 0, 0}), 0, 2, 0) && near3(par.transform_dir({0, 0, 1}), 0, 0, 2));
}

struct RunSpec {
    uint64_t ticks = 600;
    int32_t workers = -1;
    uint32_t seed = 7;
    bool snapshots = false;       // capture + publish + interpolate + instances after every tick
    bool parallel = true;         // snapshot fan-out on the session's job system (else serial_for)
    bool check_store = false;     // the primary store's digest is unchanged by every capture
    std::vector<uint64_t>* digests = nullptr;  // per-tick snapshot digests
    uint32_t* max_objects = nullptr;
    const FileProvider* anim_files = nullptr;  // R3: sim-owned animation on the session (SimSessionSpec)
    bool anim_crc = false;
    bool* anim_seen = nullptr;  // set when the session exposed an AnimWorld after a tick
};

static CrcLog run(const objects::GameObjectTypes& types, const terrain::MapData& map, const sim::TypeCrcIndex& index,
                  const render::SnapshotTypeTable& table, const RunSpec& spec) {
    jobs::JobSystem js(jobs::JobSystemConfig{spec.workers});
    render::SimSessionSpec ss;
    ss.seed = spec.seed;
    ss.anim_files = spec.anim_files;
    ss.anim_crc = spec.anim_crc;
    render::SimSession s(types, js, map, index, ss);
    render::SnapshotBuffer buf;
    render::InterpolatedFrame frame;
    render::SimInstanceBuilder builder(table);
    render::GpuModel fake;
    FakeModels models;
    models.model = &fake;
    models.table = &table;
    std::vector<render::Instance> fg, bg;
    const ParallelFor pf = spec.parallel ? ParallelFor(jobs::make_parallel_for(js)) : ParallelFor(serial_for);
    for (uint64_t i = 0; i < spec.ticks && s.tick(); ++i) {
        if (!spec.snapshots) continue;
        uint64_t before = 0;
        if (spec.check_store) {
            StateHasher h;
            s.host().tactical()->primary().hash_state(h);
            before = h.digest();
        }
        auto snap = std::make_shared<render::SimSnapshot>();
        CHECK(render::capture_snapshot(s.host(), table, *snap, pf));
        if (spec.anim_seen && s.anim_world()) *spec.anim_seen = true;
        if (spec.check_store) {
            StateHasher h;
            s.host().tactical()->primary().hash_state(h);
            CHECK(h.digest() == before);
        }
        if (spec.digests) spec.digests->push_back(snap->digest());
        if (spec.max_objects) *spec.max_objects = std::max(*spec.max_objects, static_cast<uint32_t>(snap->objects.size()));
        buf.publish(std::move(snap));
        const render::SnapshotBuffer::Pair p = buf.latest();
        for (render::InterpMode m : {render::InterpMode::Original, render::InterpMode::Smooth}) {
            render::interpolate(p.prev.get(), *p.curr, 0.5f, m, frame);
            builder.build(frame, models, fg, bg);
        }
    }
    return s.host().crc_log();
}

static bool same_log(const CrcLog& a, const CrcLog& b, const char* what) {
    const CrcDivergence d = first_divergence(a, b);
    std::printf("  %s: %zu vs %zu records: %s\n", what, a.records.size(), b.records.size(), d.message.c_str());
    return !d.diverged && a.records.size() == b.records.size() && !a.records.empty();
}

static void test_synthetic(const render::SyntheticSimWorld& w) {
    render::SnapshotTypeTable table;
    table.build(w.types, objects::ModeKind::Space);
    // Type table from the typed view (variant Sim_Corvette inherits the model; the marker has none).
    const objects::TypeIndex frig = w.types.resolve_name("Sim_Frigate"), corv = w.types.resolve_name("Sim_Corvette"),
                             mark = w.types.resolve_name("Sim_Marker");
    CHECK(table[frig].model == render::kSyntheticModel && table[corv].model == render::kSyntheticModel && table[mark].model.empty());
    CHECK(table[frig].scale == 60.0f && table[corv].scale == 40.0f && table[frig].lod_bias == 4.0f && table[frig].max_health == 1200.0f);

    // Read-only: CRC logs with and without the render side after every tick.
    RunSpec off;
    const CrcLog a = run(w.types, w.map, *w.index, table, off);
    RunSpec on = off;
    on.snapshots = true;
    on.check_store = true;
    uint32_t max_objects = 0;
    on.max_objects = &max_objects;
    const CrcLog b = run(w.types, w.map, *w.index, table, on);
    std::printf("synthetic: 600 ticks, snapshots of up to %u drawable objects\n", max_objects);
    CHECK(same_log(a, b, "snapshots off vs on"));
    CHECK(a.records.size() == 601);
    CHECK(max_objects == 2 + 16 + 8 + 30);  // stations, frigates, corvettes, asteroids (marker / unknown not drawable)

    // R3: sim-owned animation on the session (both opt-in states of the "anim" CRC system): snapshots + reads of the
    // session's AnimWorld after every tick leave the CRC log unchanged; "anim" is registered only when opted in.
    for (bool crc : {false, true}) {
        RunSpec aoff;
        aoff.ticks = 300;
        aoff.anim_files = w.files.get();
        aoff.anim_crc = crc;
        const CrcLog c = run(w.types, w.map, *w.index, table, aoff);
        RunSpec aon = aoff;
        aon.snapshots = true;
        bool seen = false;
        aon.anim_seen = &seen;
        const CrcLog e = run(w.types, w.map, *w.index, table, aon);
        const bool has = std::find(e.systems.begin(), e.systems.end(), "anim") != e.systems.end();
        std::printf("synthetic + sim-owned animation (\"anim\" CRC %s): world %s\n", crc ? "on" : "off", seen ? "exposed" : "MISSING");
        CHECK(same_log(c, e, "snapshots off vs on"));
#ifdef ALAMO_SIM_HAVE_ANIM
        CHECK(has == crc && seen);
#else
        CHECK(!has);
#endif
    }

    // Thread determinism of snapshot building: per-tick digests for every worker count + serial.
    std::vector<uint64_t> ref;
    RunSpec d = on;
    d.ticks = 200;
    d.check_store = false;
    d.max_objects = nullptr;
    d.parallel = false;
    d.workers = 0;
    d.digests = &ref;
    run(w.types, w.map, *w.index, table, d);
    CHECK(ref.size() == 200);
    for (int32_t workers : jobs::determinism_worker_counts()) {
        std::vector<uint64_t> got;
        d.parallel = true;
        d.workers = workers;
        d.digests = &got;
        run(w.types, w.map, *w.index, table, d);
        CHECK(got == ref);
        std::printf("  snapshot digests workers %d: %s (last %016" PRIx64 ")\n", workers, got == ref ? "identical" : "DIFFER",
                    got.empty() ? 0 : got.back());
    }

    // Snapshot contents at tick 0 (after spawn).
    jobs::JobSystem js(jobs::JobSystemConfig{2});
    render::SimSession s(w.types, js, w.map, *w.index);
    render::SimSnapshot snap;
    CHECK(render::capture_snapshot(s.host(), table, snap, jobs::make_parallel_for(js)));
    CHECK(snap.objects.size() == 56 && snap.store_objects == 57 && snap.tick == 0);
    bool ascending = true;
    for (size_t i = 1; i < snap.objects.size(); ++i) ascending = ascending && snap.objects[i - 1].id < snap.objects[i].id;
    CHECK(ascending);
    const render::SnapshotObject& station0 = snap.objects[0];  // first placement: side 0 station
    CHECK(station0.id == 1 && station0.owner == 0 && station0.team_index == 0 && station0.flags == render::kSnapDrawable);
    std::printf("  tick 0: object 1 type %u owner %d at (%.1f, %.1f) health %.1f (%.3f), colour (%.3f, %.3f, %.3f)\n", station0.type,
                station0.owner, static_cast<double>(station0.position.x), static_cast<double>(station0.position.y),
                static_cast<double>(station0.health), static_cast<double>(station0.health_fraction),
                static_cast<double>(station0.team_color.x), static_cast<double>(station0.team_color.y),
                static_cast<double>(station0.team_color.z));
    CHECK(station0.team_color.x == table.palette()[0].x && near(station0.position.x, -3900.0f, 0.01f) && station0.health_fraction == 1.0f);
    const render::SnapshotObject* corvette = nullptr;
    for (const auto& o : snap.objects)
        if (o.type == corv && !corvette) corvette = &o;
    CHECK(corvette && near(corvette->health_fraction, 0.75f) && corvette->team_index <= 1);
    render::SimSnapshot all;
    render::SnapshotOptions everything;
    everything.drawable_only = false;
    render::capture_snapshot(s.host(), table, all, serial_for, everything);
    CHECK(all.objects.size() == 57);
}

// --------------------------------------------------------------------------------- instance builder
static void test_instances(const render::SyntheticSimWorld& w) {
    render::SnapshotTypeTable table;
    table.build(w.types, objects::ModeKind::Space);
    render::GpuModel box;
    box.asset = render::debug_box_model("BOX");
    box.uid = 12345;
    FakeModels models;
    models.model = &box;
    models.table = &table;
    const objects::TypeIndex frig = w.types.resolve_name("Sim_Frigate"), mark = w.types.resolve_name("Sim_Marker");
    render::InterpolatedFrame f;
    render::SnapshotObject a;
    a.id = 5;
    a.type = frig;
    a.position = {100, 200, 3};
    a.facing = {0, 1, 0};
    a.lod_bias = 2.5f;
    a.fade = 0.5f;
    a.team_color = {0.25f, 0.5f, 0.75f, 1};
    a.hardpoint_count = 2;
    a.hardpoint_first = 0;
    render::SnapshotObject bgo = a;
    bgo.id = 6;
    bgo.flags = render::kSnapBackground;
    bgo.fade = 1.0f;
    bgo.hardpoint_count = 0;
    render::SnapshotObject faded = a;
    faded.id = 7;
    faded.fade = 0.0f;
    faded.hardpoint_count = 0;
    render::SnapshotObject none = a;
    none.id = 8;
    none.type = mark;
    none.hardpoint_count = 0;
    f.objects = {a, bgo, faded, none};
    f.hardpoint_destroyed = {0, 1};
    render::SimInstanceBuilder builder(table);
    std::vector<render::Instance> fg, bg;
    render::SimInstanceStats st;
    builder.build(f, models, fg, bg, &st);
    CHECK(fg.size() == 1 && bg.size() == 1 && st.objects == 4 && st.drawn == 2 && st.background == 1 && st.faded_out == 1 &&
          st.no_model == 1 && st.hardpoints_destroyed == 1);
    if (fg.size() == 1) {
        const render::Instance& in = fg[0];
        const assets::Mat3x4 t = render::object_transform(a.position, a.facing, 60.0f);
        CHECK(std::memcmp(&in.transform, &t, sizeof t) == 0);
        CHECK(near(in.transform.m[0][0], -60.0f) && near(in.transform.m[1][1], -60.0f) && in.transform.m[0][3] == 100.0f);  // nose (-Y) along +Y
        CHECK(in.model == &box && in.light_scale.w == 0.5f && in.team_color_override && in.team_color.z == 0.75f && in.lod_bias == 2.5f);
        CHECK(in.mesh_hidden.size() == box.asset->meshes.size() + 2);  // fallback layout: meshes + hardpoint slots
    }
    const render::HardpointVisibility* hv = builder.hardpoint_state(5);
    CHECK(hv && hv->destroyed(1) && !hv->destroyed(0));
    // Sticky: the destroyed bit stays when a later frame no longer reports it; objects leaving the frame drop state.
    f.hardpoint_destroyed = {0, 0};
    f.objects = {a};
    builder.build(f, models, fg, bg, &st);
    hv = builder.hardpoint_state(5);
    CHECK(hv && hv->destroyed(1) && st.hardpoints_destroyed == 1);
    CHECK(builder.tracked_objects() == 1 && !builder.hardpoint_state(6));
    // Camera interface: parameters in, the same matrices as the orbit camera out.
    render::OrbitCamera cam;
    cam.target = {1, 2, 3};
    cam.yaw = 0.3f;
    cam.pitch = 0.9f;
    cam.distance = 500.0f;
    const render::CameraParams cp = render::camera_params(cam, 10.0f, 7000.0f);
    const render::ViewMatrices v0 = render::make_view(cam, 640, 480, 10.0f, 7000.0f), v1 = render::camera_view(cp, 640, 480);
    CHECK(std::memcmp(&v0.view_proj, &v1.view_proj, sizeof v0.view_proj) == 0 && v0.eye.x == v1.eye.x);
}

// ------------------------------------------------------------------------------------------- GPU
struct Gpu {
    rhi::Context ctx;
    rhi::Allocator alloc;
    rhi::Uploader up;
    rhi::FrameRing ring;
    render::Renderer renderer;
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
               g.alloc.create_buffer(static_cast<VkDeviceSize>(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                     rhi::MemoryUsage::Readback, readback);
    }
    void destroy(Gpu& g) {
        g.alloc.destroy(color);
        g.alloc.destroy(depth);
        g.alloc.destroy(readback);
    }
};

static bool render_frame(Gpu& g, Offscreen& o, const render::ViewMatrices& vm, std::span<const render::Instance> inst,
                         render::GpuModel& resident, render::DrawStats& stats) {
    rhi::Frame* f = g.ring.begin_frame();
    if (!f) return false;
    VkCommandBuffer cmd = f->cmd();
    const rhi::DeviceFns& fn = g.ctx.vkd();
    const rhi::AcquireResult acq = g.up.record_acquires(cmd);
    g.renderer.make_resident(resident);
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
    pt.color_after = rhi::ImageState::TransferSrc;
    render::RecordOptions ro;
    ro.post = false;
    ro.shadows = false;
    const bool drawn = g.renderer.render(*f, cmd, pt, vm, inst, render::DrawMode::Unlit, ro, &stats);
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

static void test_gpu(const render::SyntheticSimWorld& w, const std::string& src_dir) {
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
    cd.app_name = "alamo_render_sim_tests";
    cd.validation = strict ? rhi::Validation::Required : rhi::Validation::IfAvailable;
    if (!g.ctx.init(cd)) {
        CHECK(!strict);  // validation layer present but the device / layer failed
        std::printf("SKIP gpu: no suitable Vulkan 1.3 device\n");
        return;
    }
    std::printf("gpu: %s, validation %s%s\n", g.ctx.caps().name.c_str(), g.ctx.validation_enabled() ? "on" : "off",
                strict ? " (required)" : "");
    rhi::FrameRingDesc fd;
    fd.frames_in_flight = 2;
    fd.uniform_bytes = 16ull << 20;
    fd.recording_threads = jobs::JobSystem::default_worker_count() + 1;
    CHECK(g.alloc.init(g.ctx) && g.up.init(g.ctx, g.alloc) && g.ring.init(g.ctx, g.alloc, fd));
    render::RendererDesc rd;
    rd.color_format = VK_FORMAT_R8G8B8A8_UNORM;
    rd.depth_format = VK_FORMAT_D32_SFLOAT;
    const bool rinit = g.renderer.init(g.ctx, g.alloc, g.up, rd);
    CHECK(rinit);
    g.renderer.set_frame_ring(&g.ring);
    Offscreen o;
    CHECK(o.create(g, 512, 512));
    render::GpuModel box;
    std::string err;
    const bool up = rinit && g.renderer.upload_model(render::debug_box_model(render::kSyntheticModel),
                                                     [](std::string_view, assets::TextureRole) {
                                                         return std::shared_ptr<const assets::TextureAsset>();
                                                     },
                                                     box, nullptr, &err);
    CHECK(up);
    if (rinit && up) {
        render::SnapshotTypeTable table;
        table.build(w.types, objects::ModeKind::Space);
        FakeModels models;
        models.model = &box;
        models.table = &table;
        // Headless reference run, then the same sim with a snapshot + 2 rendered frames (smooth, alpha 0 / 0.5) per tick.
        RunSpec off;
        off.ticks = 90;
        const CrcLog ref = run(w.types, w.map, *w.index, table, off);
        jobs::JobSystem js(jobs::JobSystemConfig{-1});
        render::SimSessionSpec ss;
        ss.seed = off.seed;
        render::SimSession s(w.types, js, w.map, *w.index, ss);
        render::SnapshotBuffer buf;
        render::InterpolatedFrame frame;
        render::SimInstanceBuilder builder(table);
        std::vector<render::Instance> fg, bg;
        render::OrbitCamera cam;
        cam.target = {0, 0, 0};
        cam.yaw = -1.5707963f;
        cam.pitch = 1.0f;
        cam.distance = 9000.0f;
        cam.fov_x = render::tactical_fov_x(55.0f, 1.0f);
        const render::ViewMatrices vm = render::camera_view(render::camera_params(cam, 10.0f, 50000.0f), o.w, o.h);
        render::DrawStats st;
        uint32_t frames = 0;
        bool all_drawn = true;
        for (uint64_t t = 0; t < off.ticks && s.tick(); ++t) {
            auto snap = std::make_shared<render::SimSnapshot>();
            render::capture_snapshot(s.host(), table, *snap, jobs::make_parallel_for(js));
            buf.publish(std::move(snap));
            const render::SnapshotBuffer::Pair p = buf.latest();
            for (float alpha : {0.0f, 0.5f}) {
                render::interpolate(p.prev.get(), *p.curr, alpha, render::InterpMode::Smooth, frame);
                builder.build(frame, models, fg, bg);
                all_drawn = render_frame(g, o, vm, fg, box, st) && all_drawn;
                ++frames;
            }
        }
        CHECK(all_drawn && frames == 180);
        CHECK(same_log(ref, s.host().crc_log(), "rendering off vs on (90 ticks, 180 frames)"));
        // Last frame: the boxes are drawn (pixels differ from the clear colour), PNG for inspection.
        const auto* px = static_cast<const uint8_t*>(o.readback.mapped());
        uint32_t lit = 0;
        for (size_t i = 0; i < static_cast<size_t>(o.w) * o.h; ++i)
            if (px[i * 4 + 0] > 40 || px[i * 4 + 1] > 40) ++lit;
        std::printf("gpu: synthetic sim frame: %u instances, %u draws, %u object pixels\n", st.instances, st.draws, lit);
        CHECK(fg.size() == 56 && st.draws >= 1 && lit > 200);
        const std::vector<uint8_t> rgba(px, px + static_cast<size_t>(o.w) * o.h * 4);
        const std::string png = src_dir + "/logs/render_sim_synthetic.png";
        CHECK(render::write_png_rgba(png, o.w, o.h, rgba));
        std::printf("wrote %s\n", png.c_str());
    }
    g.ring.wait_idle();
    if (up) g.renderer.destroy_model(box);
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

// ------------------------------------------------------------------------------------- real data
static void test_real_data() {
    const std::string root = env_or_empty("ALAMO_FOC_DATA");
    if (root.empty()) {
        std::printf("real data: ALAMO_FOC_DATA not set, skipped\n");
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
    const sim::TypeCrcIndex index(types);
    data::GameDatabase gdb;
    CHECK(gdb.load(*files));
    render::GameTypeResolver resolver(gdb);
    CHECK(resolver.load_render_xml(*files));
    for (const char* name : {"_SPACE_PLANET_CORELLIA_PIRACY", "_LAND_PLANET_BYSS_KIDNAPPING"}) {
        terrain::MapData map;
        CHECK(terrain::load_map(*files, name, map, &err));
        render::SnapshotTypeTable table;
        table.build(types, map.is_space() ? objects::ModeKind::Space : objects::ModeKind::Land, &resolver);
        RunSpec off;
        off.workers = 4;
        const CrcLog a = run(types, map, index, table, off);
        RunSpec on = off;
        on.snapshots = true;
        on.check_store = true;
        uint32_t max_objects = 0;
        on.max_objects = &max_objects;
        const CrcLog b = run(types, map, index, table, on);
        std::printf("%s: %zu placements, snapshots of up to %u drawable objects\n", name, map.objects.size(), max_objects);
        CHECK(same_log(a, b, "snapshots off vs on (600 ticks)"));
        CHECK(max_objects > 10);
        // Particle gates + land damage states read from the XML (R4 asks).
        uint32_t culling = 0, below = 0, damage = 0;
        for (objects::TypeIndex t = 0; t < table.size(); ++t) {
            culling += table[t].allow_particle_model_culling ? 1u : 0u;
            below += table[t].no_particle_below_detail > 0.0f ? 1u : 0u;
            damage += table[t].damage_thresholds.empty() ? 0u : 1u;
        }
        std::printf("%s: type table: %u Allow_Particle_Model_Culling, %u No_Particle_Below_Detail_Level, %u with damage states\n",
                    name, culling, below, damage);
        // R1 (TACTICAL_GAPS §6.3): no mode test - the same types carry their Land_Damage_* states on space maps too.
        CHECK(damage > 0);
    }
}

// Damage _ALT index rule (SnapshotObject::alt).
static void test_damage_alt() {
    render::SnapshotTypeInfo t;
    CHECK(render::damage_alt_index(t, 0.5f) == 0);  // no damage states
    t.damage_thresholds = {1.0f, 0.66f, 0.33f, 0.0f};
    t.damage_alternates = {0, 1, 2, 3};
    CHECK(render::damage_alt_index(t, 1.0f) == 0);
    CHECK(render::damage_alt_index(t, 0.8f) == 0);
    CHECK(render::damage_alt_index(t, 0.66f) == 1);
    CHECK(render::damage_alt_index(t, 0.5f) == 1);
    CHECK(render::damage_alt_index(t, 0.2f) == 2);
    CHECK(render::damage_alt_index(t, 0.0f) == 3);
    CHECK(render::damage_alt_index(t, std::nanf("")) == 0);
    t.damage_alternates = {0, 1};  // shorter list wins
    CHECK(render::damage_alt_index(t, 0.0f) == 1);
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    log::set_min_level(log::Level::Error);
    const std::string src_dir = ALAMO_RENDER_SOURCE_DIR;
    test_math();
    test_buffer_interp();
    test_attitude();
    test_effects_hardpoints();
    test_damage_alt();
    test_billboards();
    std::string err;
    const fs::path dir = fs::temp_directory_path() / "alamo_render_sim_tests";
    std::error_code ec;
    remove_all_long(dir, ec);
    const auto world = render::make_synthetic_sim_world(path_to_utf8(dir), &err);
    CHECK(world != nullptr);
    if (world) {
        test_synthetic(*world);
        test_instances(*world);
        test_projectiles(*world);
        test_gpu(*world, src_dir);
    } else {
        std::printf("synthetic world: %s\n", err.c_str());
    }
    test_real_data();
    std::printf(g_fail ? "alamo_render_sim_tests: %d failure(s)\n" : "alamo_render_sim_tests: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
