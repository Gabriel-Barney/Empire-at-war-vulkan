#include "alamo/render/sim_session.hpp"

#include <filesystem>
#include <fstream>

#include "alamo/core/hash.hpp"

namespace alamo::render {

SimSession::SimSession(const objects::GameObjectTypes& types, jobs::JobSystem& js, const terrain::MapData& map,
                       const sim::TypeCrcIndex& index, const SimSessionSpec& spec)
    : host_(types, js, [&] {
          sim::SimConfig cfg;
          cfg.seed = spec.seed;
          return cfg;
      }()),
      orders_gen_(spec.seed),
      live_(spec.live_orders) {
#ifdef ALAMO_SIM_HAVE_ANIM
    // R3: sim-owned animation, attached before the game starts (alamo_headless --battles MODE+anim order).
    if (spec.anim_files) {
        anim_ = std::make_unique<sim::SimAnimation>(types, *spec.anim_files);
        anim_->attach(host_, spec.anim_crc);
    }
#endif
    host_.start_game(map.is_space() ? sim::ModeKind::Space : sim::ModeKind::Land);
    spawn_ = sim::spawn_map(*host_.tactical(), map, index, host_.parallel_for());
    if (spec.ai)
        for (uint32_t p = 0; p < host_.players(); ++p) host_.set_controller(static_cast<uint8_t>(p), sim::AiStandIn{});
    between();  // UI input before the first tick (sim::run_map order)
}

SimSession::~SimSession() {
#ifdef ALAMO_SIM_HAVE_ANIM
    if (anim_) anim_->detach(host_);
#endif
}

bool SimSession::has_animation() const {
#ifdef ALAMO_SIM_HAVE_ANIM
    return anim_ != nullptr;
#else
    return false;
#endif
}

const anim::AnimWorld* SimSession::anim_world() const {
#ifdef ALAMO_SIM_HAVE_ANIM
    if (!anim_) return nullptr;
    const sim::TacticalMode* tm = host_.modes().top_tactical();
    return tm ? anim_->find_world(*tm) : nullptr;
#else
    return nullptr;
#endif
}

void SimSession::between() {
    if (live_) orders_ += orders_gen_.issue(host_);
}

bool SimSession::tick() {
    if (!host_.tick_once()) return false;
    ++ticks_;
    between();
    return true;
}

bool SimSession::loop_iteration(uint32_t now_ms) {
    if (!host_.loop_iteration(now_ms)) return false;
    ++ticks_;
    between();
    return true;
}

// ------------------------------------------------------------------------------------ synthetic world
namespace {

bool write_file(const std::filesystem::path& p, const std::string& s) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    f << s;
    return static_cast<bool>(f);
}

}  // namespace

std::unique_ptr<SyntheticSimWorld> make_synthetic_sim_world(const std::string& dir, std::string* error) {
    const std::filesystem::path root = path_from_utf8(dir);
    const std::string model = std::string("<Space_Model_Name>") + kSyntheticModel + "</Space_Model_Name>";
    const bool ok =
        write_file(root / "Data/XML/GameObjectFiles.xml", "<GameObjectFiles><File>RenderSimTest.xml</File></GameObjectFiles>\n") &&
        write_file(root / "Data/XML/RenderSimTest.xml",
                   "<RenderSimTest>\n"
                   " <SpaceUnit Name=\"Sim_Frigate\">" + model + "<Scale_Factor>60</Scale_Factor>\n"
                   "  <Tactical_Health>1200</Tactical_Health><Max_Speed>4.0</Max_Speed>\n"
                   "  <Targeting_Max_Attack_Distance>900</Targeting_Max_Attack_Distance>\n"
                   "  <Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living>\n"
                   "  <Behavior>SELECTABLE, TARGETING</Behavior><SpaceBehavior>FIGHTER_LOCOMOTOR</SpaceBehavior>\n"
                   " </SpaceUnit>\n"
                   " <SpaceUnit Name=\"Sim_Corvette\"><Variant_Of_Existing_Type>Sim_Frigate</Variant_Of_Existing_Type>\n"
                   "  <Scale_Factor>40</Scale_Factor><Max_Speed>6.5</Max_Speed><Tactical_Health>600</Tactical_Health></SpaceUnit>\n"
                   " <SpaceUnit Name=\"Sim_Station\">" + model + "<Scale_Factor>240</Scale_Factor><Tactical_Health>8000</Tactical_Health>\n"
                   "  <Custom_Hard_XExtent>300</Custom_Hard_XExtent><Behavior>SELECTABLE</Behavior>\n"
                   "  <Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living></SpaceUnit>\n"
                   " <SpaceUnit Name=\"Sim_Asteroid\">" + model + "<Tactical_Health>50</Tactical_Health><Scale_Factor>90</Scale_Factor>\n"
                   "  <Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living></SpaceUnit>\n"
                   " <SpaceUnit Name=\"Sim_Marker\"><Tactical_Health>1</Tactical_Health></SpaceUnit>\n"
                   "</RenderSimTest>\n");
    if (!ok) {
        if (error) *error = "cannot write the synthetic types under " + dir;
        return nullptr;
    }
    auto w = std::make_unique<SyntheticSimWorld>();
    w->files = std::make_unique<DirectoryProvider>(dir);
    if (!w->db.load(*w->files) || !w->types.build(w->db)) {
        if (error) *error = "synthetic types did not load";
        return nullptr;
    }
    w->index = std::make_unique<sim::TypeCrcIndex>(w->types);
    // Two fleets + a station each, neutral asteroids (owner 3), a model-less marker, an unknown type.
    terrain::MapData& m = w->map;
    uint32_t s = 12345u;
    auto rnd = [&s](float lo, float hi) {
        s = s * 1103515245u + 12345u;
        return lo + (hi - lo) * static_cast<float>((s >> 8) & 0xFFFFu) / 65535.0f;
    };
    auto add = [&m](const char* type, uint32_t owner, float x, float y, float yaw, float health) {
        terrain::ObjectPlacement p;
        p.type_crc = name_crc(type);
        p.owner = owner;
        p.position[0] = x;
        p.position[1] = y;
        p.rotation_deg[2] = yaw;
        p.health_fraction = health;
        m.objects.push_back(p);
    };
    for (uint32_t side = 0; side < 2; ++side) {
        const float cx = side == 0 ? -3000.0f : 3000.0f;
        add("Sim_Station", side, cx * 1.3f, 0.0f, side ? 180.0f : 0.0f, 1.0f);
        for (int i = 0; i < 8; ++i) add("Sim_Frigate", side, cx + rnd(-600, 600), rnd(-1500, 1500), rnd(0, 360), 1.0f);
        for (int i = 0; i < 4; ++i) add("Sim_Corvette", side, cx + rnd(-400, 400), rnd(-1500, 1500), rnd(0, 360), 0.75f);
    }
    for (int i = 0; i < 30; ++i) add("SIM_ASTEROID", 3, rnd(-4500, 4500), rnd(-4500, 4500), rnd(0, 360), 1.0f);
    add("Sim_Marker", 3, 0, 0, 0, 1.0f);
    add("No_Such_Type", 3, 0, 0, 0, 1.0f);
    return w;
}

}  // namespace alamo::render
