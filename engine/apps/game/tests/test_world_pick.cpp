// alamo_game_world_pick_tests (refixf t08, data-free): the tactical world pick 0x36600 (HUD_GAPS §2.2) through
// game::tactical_pick_tests on a SpaceMode with hand-built model collision data (TacticalMode::set_model_source):
// a hit on a collision mesh, a miss inside the model's bound box but outside its meshes (the box-only pick hits there),
// the Mouse_Collide_Override_Sphere_Radius sphere when the precise test misses, the nearest hit over the candidates
// (not the nearest box), equal t -> lower id, the tree box for a type without model data, and mouse_sphere_radii.
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/game/world_pick.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_types.hpp"
#include "alamo/sim/game_modes.hpp"
#include "alamo/tactical/movement/space_avoid.hpp"

using namespace alamo;
namespace mv = alamo::tactical::movement;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

namespace {

class MemoryProvider final : public FileProvider {
public:
    void add(const std::string& path, const std::string& text) {
        files_[normalize_path(path)] = std::vector<uint8_t>(text.begin(), text.end());
    }
    bool read(std::string_view path, std::vector<uint8_t>& out) const override {
        auto it = files_.find(normalize_path(path));
        if (it == files_.end()) return false;
        out = it->second;
        return true;
    }
    bool exists(std::string_view path) const override { return files_.count(normalize_path(path)) != 0; }
    std::string origin(std::string_view path) const override { return exists(path) ? "mem:" + std::string(path) : ""; }

private:
    std::map<std::string, std::vector<uint8_t>> files_;
};

const char* kTypes = R"(<?xml version="1.0"?>
<Pick>
 <SpaceUnit Name="Pick_Station"><Behavior>SELECTABLE</Behavior><Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living></SpaceUnit>
 <SpaceUnit Name="Pick_Fighter"><Behavior>SELECTABLE</Behavior><Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living>
  <Mouse_Collide_Override_Sphere_Radius>60.0</Mouse_Collide_Override_Sphere_Radius></SpaceUnit>
 <SpaceUnit Name="Pick_Box"><Behavior>SELECTABLE</Behavior><Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living><Custom_Hard_XExtent>30</Custom_Hard_XExtent></SpaceUnit>
</Pick>
)";

struct World {
    std::unique_ptr<MemoryProvider> files;
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    objects::TypeIndex station = objects::kNone, fighter = objects::kNone, box = objects::kNone;
    mv::AvoidModel station_model, fighter_model;
    std::vector<float> radii;
};

void add_quad(mv::AvoidModel::Mesh& m, mv::F3 a, mv::F3 b, mv::F3 c, mv::F3 d) {
    m.tris.insert(m.tris.end(), {a, b, c, a, c, d});
}

// A plate in the model x-z plane (y = 0): two strips, z in [150, 200] and [-200, -150], x in [-200, 200]; the
// collision bound is [-200, 200] x [-10, 10] x [-200, 200]. So the middle of the bound has no mesh.
mv::AvoidModel make_station_model() {
    mv::AvoidModel m;
    m.bound_c = {0.0f, 0.0f, 0.0f};
    m.bound_h = {200.0f, 10.0f, 200.0f};
    mv::AvoidModel::Mesh top;
    top.name = "TOP";
    top.lo = {-200.0f, -0.5f, 150.0f};
    top.hi = {200.0f, 0.5f, 200.0f};
    add_quad(top, {-200.0f, 0.0f, 150.0f}, {200.0f, 0.0f, 150.0f}, {200.0f, 0.0f, 200.0f}, {-200.0f, 0.0f, 200.0f});
    mv::AvoidModel::Mesh bottom;
    bottom.name = "BOTTOM";
    bottom.lo = {-200.0f, -0.5f, -200.0f};
    bottom.hi = {200.0f, 0.5f, -150.0f};
    add_quad(bottom, {-200.0f, 0.0f, -200.0f}, {200.0f, 0.0f, -200.0f}, {200.0f, 0.0f, -150.0f}, {-200.0f, 0.0f, -150.0f});
    m.meshes.push_back(std::move(top));
    m.meshes.push_back(std::move(bottom));
    return m;
}

// A 10-unit cube (half 5) with its 6 faces as triangles.
mv::AvoidModel make_cube_model() {
    mv::AvoidModel m;
    m.bound_c = {0.0f, 0.0f, 0.0f};
    m.bound_h = {5.0f, 5.0f, 5.0f};
    mv::AvoidModel::Mesh c;
    c.name = "HULL";
    c.lo = {-5.0f, -5.0f, -5.0f};
    c.hi = {5.0f, 5.0f, 5.0f};
    const float s = 5.0f;
    add_quad(c, {-s, -s, -s}, {s, -s, -s}, {s, -s, s}, {-s, -s, s});
    add_quad(c, {-s, s, -s}, {s, s, -s}, {s, s, s}, {-s, s, s});
    add_quad(c, {-s, -s, -s}, {-s, s, -s}, {-s, s, s}, {-s, -s, s});
    add_quad(c, {s, -s, -s}, {s, s, -s}, {s, s, s}, {s, -s, s});
    add_quad(c, {-s, -s, -s}, {s, -s, -s}, {s, s, -s}, {-s, s, -s});
    add_quad(c, {-s, -s, s}, {s, -s, s}, {s, s, s}, {-s, s, s});
    m.meshes.push_back(std::move(c));
    return m;
}

std::unique_ptr<World> make_world() {
    auto w = std::make_unique<World>();
    w->files = std::make_unique<MemoryProvider>();
    w->files->add("Data\\XML\\GameObjectFiles.xml", "<GameObjectFiles><File>PickTypes.xml</File></GameObjectFiles>");
    w->files->add("Data\\XML\\PickTypes.xml", kTypes);
    w->files->add("Data\\XML\\FactionFiles.xml", "<Faction_Files></Faction_Files>");
    CHECK(w->raw.load(*w->files));
    CHECK(w->types.build(w->raw, w->files.get()));
    w->station = w->types.resolve_name("Pick_Station");
    w->fighter = w->types.resolve_name("Pick_Fighter");
    w->box = w->types.resolve_name("Pick_Box");
    CHECK(w->station != objects::kNone && w->fighter != objects::kNone && w->box != objects::kNone);
    w->station_model = make_station_model();
    w->fighter_model = make_cube_model();
    w->radii = game::mouse_sphere_radii(w->types);
    return w;
}

// A space mode whose model source knows the station and fighter models (Pick_Box: no model data).
struct Battle {
    objects::IdCounter ids;
    std::unique_ptr<sim::SpaceMode> mode;
};
std::unique_ptr<Battle> make_battle(const World& w) {
    auto b = std::make_unique<Battle>();
    b->mode = std::make_unique<sim::SpaceMode>(w.types, b->ids, 2u);
    const World* wp = &w;
    b->mode->set_model_source([wp](objects::TypeIndex t) {
        sim::TacticalMode::ModelRef r;
        if (t == wp->station) r.model = &wp->station_model;
        if (t == wp->fighter) r.model = &wp->fighter_model;
        r.known = r.model != nullptr;
        return r;
    });
    return b;
}
uint32_t spawn(Battle& b, objects::TypeIndex type, alamo::Vec3 pos) {
    objects::SpawnParams p;
    p.type = type;
    p.owner = 1;
    p.position = pos;
    p.facing = {0.0f, 1.0f, 0.0f};  // model y -> world -y: the station plate lies in the world x-z plane
    const objects::ObjectHandle h = b.mode->spawn(p);
    return static_cast<uint32_t>(b.mode->primary().id(h));
}

// The camera on -Y looking at the origin; the test rays run along +Y (parallel to the view axis).
ui::View make_test_view() {
    ui::CameraParams cp;
    cp.eye = {0.0f, -2000.0f, 0.0f};
    cp.target = {0.0f, 0.0f, 0.0f};
    cp.zfar = 10000.0f;
    return ui::make_view(cp, 800, 600);
}
tactical::Ray ray_y(float x, float z) { return {{x, -2000.0f, z}, {0.0f, 4000.0f, 0.0f}, 1.0f}; }  // y -2000 -> 2000

bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

void test_mesh_hit_and_box_miss() {
    const auto w = make_world();
    const auto b = make_battle(*w);
    const uint32_t s = spawn(*b, w->station, {0.0f, 0.0f, 0.0f});
    const ui::View v = make_test_view();
    const ui::WorldPickTests tests = game::tactical_pick_tests(*b->mode, nullptr, w->radii);
    tactical::QueryContext qc;
    const tactical::SpatialTree& all = b->mode->trees().all();
    // The tree box is the model collision bound (refixf t07): 400 x 20 x 400.
    const tactical::Aabb* box = all.bounds(s);
    CHECK(box && near(box->max.x - box->min.x, 400.0f, 0.01f) && near(box->max.z - box->min.z, 400.0f, 0.01f));
    // On a strip: the precise test hits the plate at y = 0 (t = 0.5).
    auto hit = ui::pick_world(all, v, ray_y(0.0f, 175.0f), tactical::kSkipDead, qc, &tests);
    CHECK(hit && hit->id == s && near(hit->t, 0.5f, 1e-4f));
    hit = ui::pick_world(all, v, ray_y(-150.0f, -180.0f), tactical::kSkipDead, qc, &tests);
    CHECK(hit && hit->id == s);
    // Through the middle of the bound: inside the box, no mesh there. The box-only pick takes the station; the world pick
    // picks nothing (the right click on open space next to a station moves instead of attacking).
    CHECK(ui::pick(all, ray_y(0.0f, 0.0f), tactical::kSkipDead, qc).has_value());
    CHECK(!ui::pick_world(all, v, ray_y(0.0f, 0.0f), tactical::kSkipDead, qc, &tests).has_value());
    CHECK(!ui::pick_world(all, v, ray_y(120.0f, -100.0f), tactical::kSkipDead, qc, &tests).has_value());
    // Without tests (or without a precise test) pick_world is the box pick.
    CHECK(ui::pick_world(all, v, ray_y(0.0f, 0.0f), tactical::kSkipDead, qc, nullptr).has_value());
    // Outside the bound: nothing.
    CHECK(!ui::pick_world(all, v, ray_y(0.0f, 260.0f), tactical::kSkipDead, qc, &tests).has_value());
}

void test_sphere_override() {
    const auto w = make_world();
    CHECK(w->radii.size() == w->types.size() && near(w->radii[w->fighter], 60.0f, 0.0f) && w->radii[w->station] == 0.0f);
    const auto b = make_battle(*w);
    const uint32_t f = spawn(*b, w->fighter, {400.0f, 0.0f, 0.0f});
    const uint32_t s = spawn(*b, w->station, {-400.0f, 0.0f, 0.0f});
    const ui::View v = make_test_view();
    const ui::WorldPickTests tests = game::tactical_pick_tests(*b->mode, nullptr, w->radii);
    tactical::QueryContext qc;
    const tactical::SpatialTree& all = b->mode->trees().all();
    // On the hull: the precise hit (the cube face at y = -5).
    auto hit = ui::pick_world(all, v, ray_y(402.0f, 0.0f), tactical::kSkipDead, qc, &tests);
    CHECK(hit && hit->id == f && near(hit->t, (2000.0f - 5.0f) / 4000.0f, 1e-4f));
    // 40 units off the hull and off its tree box: the precise test misses, the 60-unit sphere at the position hits
    // (entry y = -sqrt(60^2 - 40^2)).
    CHECK(!ui::pick(all, ray_y(440.0f, 0.0f), tactical::kSkipDead, qc).has_value());
    hit = ui::pick_world(all, v, ray_y(440.0f, 0.0f), tactical::kSkipDead, qc, &tests);
    CHECK(hit && hit->id == f && near(hit->t, (2000.0f - std::sqrt(2000.0f)) / 4000.0f, 1e-3f));
    hit = ui::pick_world(all, v, ray_y(400.0f, 55.0f), tactical::kSkipDead, qc, &tests);
    CHECK(hit && hit->id == f);
    CHECK(!ui::pick_world(all, v, ray_y(470.0f, 0.0f), tactical::kSkipDead, qc, &tests).has_value());  // beyond 60
    // The station has no override sphere: its mesh gap stays a miss.
    CHECK(!ui::pick_world(all, v, ray_y(-400.0f, 0.0f), tactical::kSkipDead, qc, &tests).has_value());
    CHECK(s != f);
}

void test_nearest_hit_order() {
    const auto w = make_world();
    const ui::View v = make_test_view();
    tactical::QueryContext qc;
    {
        // The farther station first (lower id): the nearer hit wins whatever the id order.
        const auto b = make_battle(*w);
        const uint32_t far_id = spawn(*b, w->station, {0.0f, 500.0f, 0.0f});
        const uint32_t near_id = spawn(*b, w->station, {0.0f, -500.0f, 0.0f});
        CHECK(far_id < near_id);
        const ui::WorldPickTests tests = game::tactical_pick_tests(*b->mode, nullptr, w->radii);
        const tactical::SpatialTree& all = b->mode->trees().all();
        auto hit = ui::pick_world(all, v, ray_y(0.0f, 175.0f), tactical::kSkipDead, qc, &tests);
        CHECK(hit && hit->id == near_id && near(hit->t, 0.375f, 1e-4f));
        // ignore: the next hit.
        hit = ui::pick_world(all, v, ray_y(0.0f, 175.0f), tactical::kSkipDead, qc, &tests, near_id);
        CHECK(hit && hit->id == far_id && near(hit->t, 0.625f, 1e-4f));
    }
    {
        // The ray crosses the nearer station's bound in its mesh gap and the farther one's strip: the farther station
        // (the box-only pick returns the nearer box).
        const auto b = make_battle(*w);
        const uint32_t near_id = spawn(*b, w->station, {0.0f, -500.0f, 0.0f});
        const uint32_t far_id = spawn(*b, w->station, {0.0f, 500.0f, -100.0f});
        const ui::WorldPickTests tests = game::tactical_pick_tests(*b->mode, nullptr, w->radii);
        const tactical::SpatialTree& all = b->mode->trees().all();
        const auto boxed = ui::pick(all, ray_y(0.0f, 75.0f), tactical::kSkipDead, qc);
        CHECK(boxed && boxed->id == near_id);
        const auto hit = ui::pick_world(all, v, ray_y(0.0f, 75.0f), tactical::kSkipDead, qc, &tests);
        CHECK(hit && hit->id == far_id && near(hit->t, 0.625f, 1e-4f));
    }
    {
        // Equal t: the lower id. A type without model data: its tree box stands in.
        const auto b = make_battle(*w);
        const uint32_t a = spawn(*b, w->station, {0.0f, 0.0f, 0.0f});
        const uint32_t c = spawn(*b, w->station, {0.0f, 0.0f, 0.0f});
        const uint32_t x = spawn(*b, w->box, {-600.0f, 0.0f, 0.0f});
        const ui::WorldPickTests tests = game::tactical_pick_tests(*b->mode, nullptr, w->radii);
        const tactical::SpatialTree& all = b->mode->trees().all();
        auto hit = ui::pick_world(all, v, ray_y(0.0f, 175.0f), tactical::kSkipDead, qc, &tests);
        CHECK(a < c && hit && hit->id == a);
        hit = ui::pick_world(all, v, ray_y(-620.0f, 20.0f), tactical::kSkipDead, qc, &tests);  // inside the 30 extent box
        CHECK(hit && hit->id == x && near(hit->t, (2000.0f - 30.0f) / 4000.0f, 1e-4f));
        CHECK(!ui::pick_world(all, v, ray_y(-640.0f, 0.0f), tactical::kSkipDead, qc, &tests).has_value());
    }
}

}  // namespace

int main() {
    test_mesh_hit_and_box_miss();
    test_sphere_override();
    test_nearest_hit_order();
    std::printf(g_fail == 0 ? "game world pick tests passed\n" : "%d game world pick check(s) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
