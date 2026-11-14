// tactical_battle: part 1 (bo3 t03 split; shared code in tactical_battle_internal.hpp).
#include "tactical_battle_internal.hpp"

namespace alamo::sim {

TacticalBattle::~TacticalBattle() = default;

namespace bo3split {

Vec3 facing_of_yaw(float yaw_deg) {
    float s = 0.0f, c = 1.0f;
    dmath::sincos(yaw_deg * (dmath::kPiF / 180.0f), s, c);
    return {c, s, 0.0f};
}
}  // namespace bo3split

namespace bo3split {

float yaw_of(Vec3 f) { return dmath::atan2(f.y, f.x) * (180.0f / dmath::kPiF); }
}  // namespace bo3split

namespace bo3split {

// s78j t01 (TACTICAL_COMBAT §3.1.1 "Units leaving a land landing object"): 0x20acd0((0, 0, 0), O) - the level facing
// along the unload offset O (yaw atan2(O.y, O.x); yaw 0 when O's xy are zero).
Vec3 offset_facing(float ox, float oy) { return facing_of_yaw(ox == 0.0f && oy == 0.0f ? 0.0f : yaw_of(Vec3{ox, oy, 0.0f})); }
}  // namespace bo3split

namespace bo3split {
Vec3 sv(tactical::Vec3 v) { return Vec3{v.x, v.y, v.z}; }
}  // namespace bo3split

// ---- construction ---------------------------------------------------------------------------------------------

TacticalBattle::TacticalBattle(const BattleCatalog& catalog, const galactic::GalacticSim& galaxy,
                               const galactic::ConflictRules& rules, galactic::BattleRequest request, BattleConfig cfg,
                               const ParallelFor& pf)
    : catalog_(&catalog), galaxy_(&galaxy), rules_(&rules), req_(std::move(request)), cfg_(cfg) {
    space_ = req_.info.kind == galactic::ConflictKind::Space;
    for (const galactic::BattleForces& f : req_.forces) {
        const uint8_t side = f.player == req_.info.defender ? 0 : 1;
        for (objects::ObjectHandle h : f.start) add_participant(h, f.player, side, false, false, false);
        for (objects::ObjectHandle h : f.reserve) add_participant(h, f.player, side, true, false, false);
    }
    const objects::ObjectStore& gs = galaxy.store();
    if (gs.alive(req_.starbase)) {
        const int32_t o = gs.owner(req_.starbase);
        add_participant(req_.starbase, o, o == req_.info.defender ? 0 : 1, false, true, false);
    }
    for (objects::ObjectHandle s : req_.structures)
        if (gs.alive(s)) add_participant(s, gs.owner(s), gs.owner(s) == req_.info.defender ? 0 : 1, false, false, true);
    bt_force_split();  // battle host: land lead / raid split, space super-weapon start rule (TACTICAL_GAPS §4.2 / §4.6)
    // Types spawned in this battle, first-use order -> the battle tables (team containers after their members,
    // the dummy target type last).
    auto use = [this](uint32_t t) {
        if (std::find(types_used_.begin(), types_used_.end(), t) == types_used_.end()) types_used_.push_back(t);
    };
    for (const Participant& p : parts_) {
        const std::vector<uint32_t> st = spawn_types(p);
        for (uint32_t t : st) use(t);
        if (!rules_->type(p.type).members.empty()) use(p.type);
        // §3.4: the company's TEAM object type and its TEAM behaviour type (the company type when Create_Team_Type is empty).
        uint32_t ot = 0xFFFFFFFFu, bt = 0xFFFFFFFFu, squad = 0;
        if (!rules_->type(p.type).members.empty() && !st.empty() && bt_team(p.type, st.front(), ot, bt, squad)) {
            use(ot);
            use(bt);
        }
        // §3.2 steps 2-3: Tactical_Additional_Structure_Type temporaries, Place_Other_Type_At_Every_Specific_Marker_Position.
        if (p.structure) {
            const objects::GameObjectTypes& types = catalog.types();
            for (const char* tag : {"Tactical_Additional_Structure_Type", "Place_Other_Type_At_Every_Specific_Marker_Position"}) {
                const objects::TypeIndex x = types.resolve_name(types.get_string(p.type, tag));
                if (x != objects::kNone && x < types.size()) use(static_cast<uint32_t>(x));
            }
        }
    }
    const objects::TypeIndex dummy = catalog.types().resolve_name("Dummy_Barrage_Target");
    if (dummy != objects::kNone) {
        dummy_type_ = static_cast<int32_t>(dummy);
        use(static_cast<uint32_t>(dummy));
    }
    l1_add_types(types_used_);  // L1 systems: the mode's special-weapon types (projectiles in the tables)
    tables_ = std::make_unique<BattleTables>();  // battle host: model bones
    tables_build_ = catalog.begin_battle(*tables_, space_);
    tables_build_->pf = pf;
    tables_extend();
}

void TacticalBattle::tables_extend() {
    // OPT3 t09: setup appends types several times; each step builds only the new ones (BattleCatalog::extend_battle) instead
    // of rebuilding every type from the start. Same tables and descriptor indices either way. cu9 t12: the constructor
    // always makes the build and keeps it for the battle's life, so there is no full-rebuild fallback.
    catalog_->extend_battle(*tables_build_, types_used_, descs_);
}

// N1 stage 2 (tactical_battle_save.cpp): after a load, the combat hooks get the tick context of the saving run's last
// battle tick (command-phase hooks before the next battle tick use it).
void TacticalBattle::resume_tick_context() {
    if (frame_ > 0 && hooks_ && bt_rng_) hooks_->set_tick(bt_rng_, oi_global_frame_);
}

void TacticalBattle::add_participant(objects::ObjectHandle g, int32_t owner, uint8_t side, bool reserve, bool starbase,
                                     bool structure) {
    const objects::ObjectStore& gs = galaxy_->store();
    if (!gs.alive(g)) return;
    Participant p;
    p.galactic = g;
    p.galactic_id = gs.id(g);
    p.owner = owner;
    p.type = static_cast<uint32_t>(gs.type(g));
    p.side = side;
    p.reserve = reserve;
    p.starbase = starbase;
    p.structure = structure;
    const galactic::ConflictTypeInfo& ti = rules_->type(p.type);
    p.pop = static_cast<float>(ti.space_population_override >= 0 ? ti.space_population_override : ti.population);
    const auto& types = galaxy_->rules().types;
    p.power = p.type < types.size() ? types[p.type].ai_combat_power : 0.0f;
    for (const auto& [mt, n] : ti.members)
        if (mt < types.size()) p.power += static_cast<float>(n) * types[mt].ai_combat_power;
    parts_.push_back(p);
}

std::vector<uint32_t> TacticalBattle::spawn_types(const Participant& p) const {
    std::vector<uint32_t> out;
    const galactic::ConflictTypeInfo& ti = rules_->type(p.type);
    if (ti.members.empty()) {
        out.push_back(p.type);
    } else {
        for (const auto& [mt, n] : ti.members)
            for (int32_t i = 0; i < n; ++i) out.push_back(mt);
    }
    return out;
}

uint32_t TacticalBattle::desc_of(uint32_t type) const {
    for (size_t i = 0; i < types_used_.size(); ++i)
        if (types_used_[i] == type) return descs_[i];
    return 0;
}

const BattleTables::UnitVisuals* TacticalBattle::visuals_of_type(uint32_t type) const {
    for (size_t i = 0; i < types_used_.size(); ++i)
        if (types_used_[i] == type) return descs_[i] < tables_->units.size() ? &tables_->units[descs_[i]] : nullptr;
    return nullptr;
}

TacticalBattle::Unit* TacticalBattle::unit_by_id(objects::ObjectId id) {
    const auto it = std::lower_bound(units_.begin(), units_.end(), id, [](const Unit& u, objects::ObjectId v) { return u.id < v; });
    return (it == units_.end() || it->id != id) ? nullptr : &*it;
}

// cu8j t02 (MOVEMENT §4.7.1 locomotor table): the active locomotor's vf +0x220 (admitted) and vf +0x238 (kind) by its
// behaviour token, the mode's behaviour list first (as locomotor_kind_from_type). Walk / Bike / LandTeamInfantry /
// LandTeamContainer: kind 1; SimpleSpace / Fighter / Flying / LandBomber: kind 2; JetPack: kind 1 [port: on the ground,
// as spawned]; Team: kind 2 when a member admits (-1 here); Starship / Fleet / Simple and no locomotor: not admitted.
std::string_view TacticalBattle::mv_locomotor_token(uint32_t type) const {
    const objects::GameObjectTypes& types = catalog_->types();
    if (type >= types.size()) return {};
    for (const std::string_view tag : {space_ ? std::string_view("SpaceBehavior") : std::string_view("LandBehavior"), std::string_view("Behavior")}) {
        for (const std::string_view raw : types.get_list(type, tag)) {
            const std::string_view t = ascii_trim(raw);
            constexpr std::string_view kSuffix = "LOCOMOTOR";  // any *_LOCOMOTOR token is the locomotor slot
            if (t.size() >= kSuffix.size() && ascii_iequals(t.substr(t.size() - kSuffix.size()), kSuffix)) return t;
        }
    }
    return {};
}

int8_t TacticalBattle::mv_adopt_kind(uint32_t type) {
    const objects::GameObjectTypes& types = catalog_->types();
    if (type >= types.size()) return 0;
    if (adopt_kind_.size() < types.size()) adopt_kind_.resize(types.size(), int8_t{0});
    if (adopt_kind_[type] != 0) return static_cast<int8_t>(adopt_kind_[type] - 1);
    int8_t k = 0;
    const std::string_view t = mv_locomotor_token(type);
    if (ascii_iequals(t, "WALK_LOCOMOTOR") || ascii_iequals(t, "BIKE_LOCOMOTOR") || ascii_iequals(t, "JETPACK_LOCOMOTOR") ||
        ascii_iequals(t, "LAND_TEAM_INFANTRY_LOCOMOTOR") || ascii_iequals(t, "LAND_TEAM_CONTAINER_LOCOMOTOR"))
        k = 1;
    else if (ascii_iequals(t, "SIMPLE_SPACE_LOCOMOTOR") || ascii_iequals(t, "FIGHTER_LOCOMOTOR") ||
             ascii_iequals(t, "FLYING_LOCOMOTOR") || ascii_iequals(t, "LANDBOMBER_LOCOMOTOR"))
        k = 2;
    else if (ascii_iequals(t, "TEAM_LOCOMOTOR"))
        k = -1;
    adopt_kind_[type] = static_cast<int8_t>(k + 1);
    return k;
}

// cu8l t01 (CU8K_T02_T06_RE §1.2 / §1.5 / §4 rule 1): the locomotors whose behaviour init (vf9, run by the behaviour
// attach 0x38cb30 at creation) registers the object itself - a new coordinator holding a Move_To its own position:
// LandTeamContainer (0x5abac0) on land, SimpleSpace (0x625090) in space.
bool TacticalBattle::mv_self_registers(uint32_t type) const {
    const std::string_view t = mv_locomotor_token(type);
    return ascii_iequals(t, space_ ? std::string_view("SIMPLE_SPACE_LOCOMOTOR") : std::string_view("LAND_TEAM_CONTAINER_LOCOMOTOR"));
}

bool TacticalBattle::mv_adopt(const TacticalMode& mode, objects::ObjectHandle h, SimRng& rng, bool leave) {
    if (!movement_) return false;
    const objects::ObjectStore& st = mode.primary();
    if (!st.alive(h)) return false;
    const objects::ObjectId id = st.id(h);
    if (const Unit* u = unit_by_id(id); u && u->container != kNoContainer) return false;  // a TEAM member: never
    int8_t kind = mv_adopt_kind(static_cast<uint32_t>(st.type(h)));
    if (kind < 0) {  // TEAM_LOCOMOTOR (0x6305a0): vf +0x220 = any member's, vf +0x238 = 2
        kind = 0;
        for (size_t ci = 0; ci < containers_.size() && kind == 0; ++ci) {
            if (containers_[ci].id != id) continue;
            for (const Unit& m : units_)
                if (m.container == ci && m.alive && !m.retreated && mv_adopt_kind(m.type) > 0) {
                    kind = 2;
                    break;
                }
        }
    }
    if (kind != (space_ ? 2 : 1)) return false;
    if (movement_->has_unit(id)) return leave ? movement_->stop_and_hold(id, rng) : movement_->adopt(id, rng);
    // [port] an object outside the movement system (a TEAM container, an inert map object) keeps no coordinator record:
    // each adopt draws.
    (void)mv::MovementSystem::formation_ctor_draw(rng);
    return true;
}

// cu8l t02 (CU8K_T02_T06_RE §3 "movement lock", rule 6; A1_READINGS §28.8): the first lock 0x3a8e50 (counter 0 -> 1). An
// object in a formation is split off (0x4f84e0: one draw; the adopt then finds it in a coordinator: no second draw [M]);
// one in a coordinator without a formation draws nothing; one without a coordinator is adopted (0x39bcb0 predicate: one
// draw when admitted). [port] A TEAM member draws nothing (its container holds the formation); a TEAM container is read
// from its members (the movement system's units): any of them waiting for a composition or held by an adopt -> no draw,
// any in a formation -> the split's draw, else the container's adopt.
bool TacticalBattle::mv_first_lock(const TacticalMode& mode, objects::ObjectId id, SimRng& rng) {
    using Hold = mv::MovementSystem::CoordinatorHold;
    if (!movement_ || finished_) return false;
    if (const Unit* u = unit_by_id(id)) {
        if (!u->alive || u->retreated || u->container != kNoContainer) return false;
        if (u->movable && movement_->split_from_formation(id, rng)) return true;
        if (u->movable && movement_->coordinator_hold(id) != Hold::None) return false;
        return mv_adopt(mode, u->h, rng);
    }
    for (size_t ci = 0; ci < containers_.size(); ++ci) {
        if (containers_[ci].id != id || containers_[ci].removed) continue;
        if (movement_->has_unit(id)) {  // s78n t03: a land squad container is a mover with its own coordinator
            if (movement_->split_from_formation(id, rng)) return true;
            if (movement_->coordinator_hold(id) != Hold::None) return false;
            return mv_adopt(mode, containers_[ci].h, rng);
        }
        Hold hold = Hold::None;
        for (const Unit& m : units_)
            if (m.container == ci && m.alive && !m.retreated && m.movable) {
                const Hold mh = movement_->coordinator_hold(m.id);
                if (mh == Hold::NoFormation) hold = Hold::NoFormation;
                else if (mh == Hold::Formation && hold == Hold::None) hold = Hold::Formation;
            }
        if (hold == Hold::NoFormation) return false;
        if (hold == Hold::Formation) {
            (void)mv::MovementSystem::formation_ctor_draw(rng);  // the container's split
            return true;
        }
        return mv_adopt(mode, containers_[ci].h, rng);
    }
    return false;
}

void TacticalBattle::script_first_lock(TacticalMode& mode, objects::ObjectId id) {
    if (bt_rng_) (void)mv_first_lock(mode, id, *bt_rng_);
}

bool TacticalBattle::select_all_entry(objects::ObjectId id, int32_t owner, std::vector<objects::ObjectId>& members) const {
    if (owner >= 0 && owner == retreating_player()) return false;  // the retreat coordinator's list [port: the whole side]
    if (garrison_holder_of(id) != objects::kNoObjectId) return false;  // garrisoned: parent = the host
    const auto it = std::lower_bound(units_.begin(), units_.end(), id, [](const Unit& u, objects::ObjectId v) { return u.id < v; });
    if (it != units_.end() && it->id == id && it->container != kNoContainer) return false;  // a TEAM member
    for (size_t ci = 0; ci < containers_.size(); ++ci) {
        if (containers_[ci].id != id || !containers_[ci].team) continue;
        for (const Unit& u : units_)
            if (u.container == ci && u.alive && !u.retreated) members.push_back(u.id);
        break;
    }
    return true;
}

// FIGHTER_FLYING §6.3.1 type data: Uses_Multiple_Locomotors (+0x74), Stationary_Space_Layer (+0x16e0), the query
// half-extents 0x396850 and the land model's (type +0x2a0, no fallback to other modes' models) box z for the hit value.
// s78n t10 (TACTICAL_COMBAT §5.4.6 port spec item 1): the half-extents are 0x371be0 - both Custom_Hard extents > 0 ->
// x Scale_Factor; else no land model -> (0, 0); else per axis the custom value or the |collision-bound half x / y|
// (AvoidModel::bound_h, not |max| of the all-mesh bind-pose box), x Scale_Factor - and the hit value reads the collision
// bound's centre z and half z, so clearance_top = (c.z + h.z) x Scale_Factor + O.z. No team or deployed branch: flyers
// and Corvette-layer objects are neither. [port] a named land model that does not load (no art data): per axis the
// custom value, else 0, and no hit value. Pure function of the type and the model files: cached per type, not saved.
bool TacticalBattle::clearance_info(objects::ObjectId id, mv::ClearanceInfo& out) {
    const Unit* u = unit_by_id(id);
    const objects::GameObjectTypes& types = catalog_->types();
    if (!u || u->type >= types.size()) return false;
    const uint32_t t = u->type;
    if (clr_known_.size() < types.size()) {
        clr_known_.resize(types.size(), 0u);
        clr_types_.resize(types.size());
    }
    if (!clr_known_[t]) {
        mv::ClearanceInfo ci;
        ci.uses_multiple_locomotors = types.get_bool(t, "Uses_Multiple_Locomotors");
        ci.stationary_space_layer = static_cast<uint32_t>(types.get_int(t, "Stationary_Space_Layer"));
        ci.scale_factor = types.columns().scale_factor[t];
        const mv::AvoidModel* am = catalog_->avoid_model(t, false);
        const bool named = !ascii_trim(types.get_string(t, "Land_Model_Name")).empty();
        ci.has_box = am != nullptr;
        if (am) {
            ci.box_lo_z = am->bound_c.z;
            ci.box_hi_z = am->bound_h.z;
        }
        ci.half = mv::clearance_half_extents(types.get_float(t, "Custom_Hard_XExtent"), types.get_float(t, "Custom_Hard_YExtent"),
                                             ci.scale_factor, am != nullptr || named, am ? am->bound_h.x : 0.0f,
                                             am ? am->bound_h.y : 0.0f);
        clr_types_[t] = ci;
        clr_known_[t] = 1u;
    }
    out = clr_types_[t];
    return true;
}

// SPACE_AVOIDANCE §2 / §3 / §7: the avoidance tree members of a space battle. Membership (§2.1, decided by the type:
// not Is_Asteroid_Field / Is_Ion_Storm / Is_Nebula, Space_Layer with a bit other than Land) is a pure function of the
// type, so evaluating it over the live objects equals the retail insert-at-spawn / remove-at-destroy set [port: the
// Uses_Multiple_Locomotors branch of 0x397900 is land-only]. Positions / facings are the committed store values; the
// model is the type's Space_Model_Name collision data (BattleCatalog::avoid_model). [port] the model-level collide
// switch (off while contained) is not modelled: space members are never garrisoned. S7 t08 (SPACE_AVOIDANCE §5.5): no
// other game-object path switches it off (capture points and orbital props keep colliding), so nothing else to filter.
void TacticalBattle::avoid_snapshot(const objects::ObjectStore& store) {
    avoid_members_.clear();
    const objects::GameObjectTypes& types = catalog_->types();
    if (avoid_type_.size() < types.size()) avoid_type_.resize(types.size(), int8_t{-1});
    for (const ObjectHandle h : store.objects_newest_first()) {
        if (!store.alive(h)) continue;
        const uint32_t t = store.type(h);
        if (t >= types.size()) continue;
        if (avoid_type_[t] < 0) {
            const bool member = !types.get_bool(t, "Is_Asteroid_Field") && !types.get_bool(t, "Is_Ion_Storm") &&
                                !types.get_bool(t, "Is_Nebula") && (types.get_mask(t, "Space_Layer") & ~uint64_t{0x10}) != 0;
            const bool shielded = types.has_behavior(t, objects::BehaviorSet::Union, objects::beh::Shielded);
            avoid_type_[t] = static_cast<int8_t>(member ? (shielded ? 2 : 1) : 0);
        }
        if (avoid_type_[t] == 0) continue;
        mv::AvoidObstacle o;
        o.id = store.id(h);
        const Vec3 p = store.position(h), f = store.facing(h);
        o.pos = {p.x, p.y, p.z};
        o.facing = {f.x, f.y};
        o.scale = types.columns().scale_factor[t];
        o.model = catalog_->avoid_model(t, true);
        o.skip_shield = avoid_type_[t] == 2;
        mv::avoid_place(o);
        avoid_members_.push_back(o);
    }
    std::sort(avoid_members_.begin(), avoid_members_.end(),
              [](const mv::AvoidObstacle& a, const mv::AvoidObstacle& b) { return a.id < b.id; });
}

// ---- setup ---------------------------------------------------------------------------------------------------------

void TacticalBattle::setup(TacticalMode& mode, terrain::MapData map, SimRng& rng, uint32_t players, const ParallelFor& pf) {
    map_ = std::move(map);
#ifdef ALAMO_SIM_HAVE_ASSETS
    // refixf t07 (SPACE_AVOIDANCE §3.1 / §3.2 / §7.2): the tree boxes and the precise projectile test read the type
    // model's collision data (Space_Model_Name / Land_Model_Name, as the fighter avoidance does). A model that does not
    // load gives no data and its type keeps the extent box and box hits [port]. [port] So does a type without a model
    // name (retail: no model, box [P, P + 0.1], never hit): the synthetic test data has no model names, and among the FoC
    // projectile-collidable types only Dummy_Barrage_Target and abstract base types have none.
    if (catalog_->files()) {
        const BattleCatalog* cat = catalog_;
        const bool space = map_.is_space();
        mode.set_model_source([cat, space](objects::TypeIndex t) {
            TacticalMode::ModelRef r;
            r.model = cat->avoid_model(t, space);
            r.known = r.model != nullptr;
            return r;
        });
    }
#endif
    // --- OI objects: computed behaviour intervals (SHIELDED / POWERED) from GameConstants, before any object exists ---
    {
        const tc::CombatConfig& gc = catalog_->combat_config();
        objects::BehaviorIntervalConfig ic;
        ic.fps = gc.fps;
        ic.shield_recharge_interval_secs = gc.shield_recharge_interval_secs;
        ic.energy_recharge_interval_secs = gc.energy_recharge_interval_secs;
        mode.primary().set_interval_config(ic);
    }
    // --- end OI objects ---
    tables_build_->pf = pf;  // OPT3 t09
    bt_rng_ = &rng;            // battle host: pad objects join the battle with their add_unit draws
    bt_players_ = players;
    bt_map_prepass();          // REMAINING_GAPS §3.1: base-level filter + branched-map discard; map types -> tables
    bt_add_pad_types(map_);    // battle host: the map's pad UC / constructed types join the tables (before combat exists)
    oi_add_ability_types();    // OI abilities: ability-spawned / survivor types join the tables (before combat exists)
    hg_add_types();            // hangars: launched squadrons / ships and the station chain join the tables
#ifdef ALAMO_SIM_HAVE_ASSETS
    // OPT3 t05 / t09: the model files of the map's placements (after the prepass: a land skirmish's team markers are its pads
    // now) and the participants load in parallel here instead of one type at a time inside the spawns and stamps below
    // (the first battle tick's largest cost); results unchanged.
    if (catalog_->files()) {
        const TypeCrcIndex index(catalog_->types());
        std::vector<uint32_t> types;
        types.reserve(map_.objects.size() + parts_.size());
        for (const terrain::ObjectPlacement& pl : map_.objects)
            if (const objects::TypeIndex t = index.find(pl.type_crc); t != objects::kNone) types.push_back(static_cast<uint32_t>(t));
        for (const Participant& p : parts_) types.push_back(p.type);
        catalog_->preload_models(types, map_.is_space(), pf);
    }
#endif
    const objects::GameObjectTypes& types = catalog_->types();
    float mn[2] = {-5000.0f, -5000.0f}, mx[2] = {5000.0f, 5000.0f};
    if (!map_.playable_box(mn, mx)) {
        mn[0] = mn[1] = -5000.0f;
        mx[0] = mx[1] = 5000.0f;
    }
    float zmin = -1.0e6f, zmax = 1.0e6f;
    if (map_.is_space()) {
        zmin = map_.space.playable_min[2];
        zmax = map_.space.playable_max[2];
    }
    mode.set_bounds({{mn[0], mn[1], zmin}, {mx[0], mx[1], zmax}});
    // [port] fallback anchors / facings (maps without Defending Forces / Attacker Entry markers): +-45% of the box on X,
    // facing each other. bt_place_forces replaces them from the markers (§3.2 steps 6-7).
    {
        const float cx = 0.5f * (mn[0] + mx[0]), cy = 0.5f * (mn[1] + mx[1]), hx = 0.5f * (mx[0] - mn[0]);
        anchor_[0] = {cx + 0.45f * hx, cy, 0.0f};
        anchor_[1] = {cx - 0.45f * hx, cy, 0.0f};
        facing_[0] = {-1.0f, 0.0f, 0.0f};
        facing_[1] = {1.0f, 0.0f, 0.0f};
        drop_facing_[0] = drop_facing_[1] = Vec3{};  // a1h t06: mode +0x288 starts zeroed (no marker -> yaw 0)
    }
    // Movement world.
    mv::MovementConfig mc;
    mc.mode = space_ ? mv::MoveMode::Space : mv::MoveMode::Land;
    movement_ = std::make_unique<mv::MovementSystem>(mc);
    // cu8j t01 (MOVEMENT §4.6.2): the movement coordinator update runs at the end of the tick (TacticalBattle::tick), after
    // the object services, as retail's common tick step 10; the movement tick does not run it.
    movement_->set_deferred_composition(true);
    // OI tactical: crush hostility = the combat relation (player +0x370 table, MOVEMENT.md 5.5).
    movement_->set_hostility([this](int32_t a, int32_t b) { return combat_ && combat_->is_enemy(a, b); });
    // s78n t14 part B (FIGHTER_FLYING §7.1, 0x5c8890): Scatters_From_Crushers of the owner's faction ([port] false for an
    // owner without a galactic player).
    movement_->set_scatters_fn([this](int32_t owner) {
        if (!galaxy_ || !rules_ || owner < 0 || static_cast<size_t>(owner) >= galaxy_->players().size()) return false;
        return rules_->faction(galaxy_->players()[static_cast<size_t>(owner)].faction).scatters_from_crushers;
    });
    // cu8l t03 (CU8K_T03_T08_T09_RE §1.10 rule 1): the movement lock of the start fleet's hyperspace arrival (arrivals_).
    movement_->set_locked_fn([this](mv::ObjectId key) { return arrival_locked(key); });
    // s78n t10 (TACTICAL_COMBAT §5.4.6 port spec item 4): the space composition's candidate boxes are the world AABB
    // 0x3982b0 - the Space_Model_Name collision bound through the unit's yaw and Scale_Factor (as avoid_snapshot builds
    // the avoidance members); a type without a space model name has no model (the 0.1 box at the position). [port] no
    // model files in this run, or a named model that does not load: no data (MovementSystem keeps its stand-in).
    if (space_)
        movement_->set_model_bound_fn([this](mv::ObjectId id, const mv::AvoidModel*& model, float& scale) {
            const Unit* u = unit_by_id(id);
            const objects::GameObjectTypes& types = catalog_->types();
            if (!u || u->type >= types.size() || !catalog_->files()) return false;
            scale = types.columns().scale_factor[u->type];
            model = nullptr;
            if (ascii_trim(types.get_string(u->type, "Space_Model_Name")).empty()) return true;
            model = catalog_->avoid_model(u->type, true);
            return model != nullptr;
        });
    // S8 t24 (MOVEMENT §3.2 "Turret recentre around deploy"): a Deploys unit's TURRET aims at its live current combat
    // target (0x632050 [inferred: whenever the unit has one]); the raw position, no z adjust [inferred].
    movement_->set_turret_aim_fn([this](objects::ObjectId id, float& x, float& y, float& z) {
        if (!combat_ || !combat_->has_unit(id)) return false;
        const tc::ObjectId t = combat_->current_target(id);
        if (t == tc::kNoObject || !combat_->has_unit(t) || !combat_->alive(t)) return false;
        const auto p = combat_->position(t);
        x = p.x;
        y = p.y;
        z = p.z;
        return true;
    });
#ifdef ALAMO_SIM_HAVE_ANIM
    // REMAINING_GAPS §4.3-4.7: special-locomotor anim lengths = the model's clip (variant 0) in sim frames (30 / s).
    movement_->set_anim_length_fn([this](objects::ObjectId id, int32_t type) -> uint32_t {
        const anim::AnimWorld* w = hooks_ ? hooks_->world() : nullptr;
        const anim::AnimModel* m = w ? w->model(id) : nullptr;
        const anim::ClipRef* c = m ? m->clip(type, 0) : nullptr;
        if (!c || !c->clip || c->clip->fps <= 0.0f) return 0u;
        return static_cast<uint32_t>(std::ceil(static_cast<double>(c->clip->frame_count) * 30.0 / static_cast<double>(c->clip->fps)));
    });
    // S8 t11 (MOVEMENT §3.2): the Walk deploy clips (anim 0x19 / 0x1A, variant 0 [port]: retail draws a synced variant).
    movement_->set_deploy_clip_fn([this](objects::ObjectId id, int32_t type, uint32_t& frames, float& fps) {
        const anim::AnimWorld* w = hooks_ ? hooks_->world() : nullptr;
        // S8 t11 follow-up: the drawn variant once it plays (SimAnimation::deploy, drive_animation), else variant 0.
        const anim::SlotView sv = w ? w->slot(id) : anim::SlotView{};
        const anim::AnimModel* m = w ? w->model(id) : nullptr;
        const anim::ClipRef* c = sv.type == type && sv.clip ? sv.clip : (m ? m->clip(type, 0) : nullptr);
        if (!c || !c->clip || c->clip->fps <= 0.0f) return false;
        frames = c->clip->frame_count;
        fps = c->clip->fps;
        return true;
    });
#endif
    if (space_) {
        // S7 t07 (MOVEMENT §2.2): SPACE_OBSTACLE types without an XML radius are sized by their space model's collision
        // bound (the capture / construction pods), as in space_obstacles_sync.
        mv::SpaceObstacleHooks so_hooks;
        const BattleCatalog* so_cat = catalog_;
        so_hooks.model_half_extents = [so_cat](uint32_t t, float& hx, float& hy) { return so_cat->space_model_half_extents(t, hx, hy); };
        mv::space_from_map(movement_->space(), map_, &types, &so_hooks);
        // FIGHTER_FLYING §5: the fighter cell grids build from the playable box at the first tick with a fighter; a
        // map without one gets the fallback box above.
        if (!movement_->space().has_box()) movement_->space().set_box({mn[0], mn[1]}, {mx[0], mx[1]});
        // SPACE_AVOIDANCE: the fighter avoidance hook over this tick's tree members (avoid_snapshot).
        movement_->set_fighter_avoid_fn([this](mv::ObjectId, mv::F3 c, mv::F3 h, mv::F3 a, mv::F3 b, mv::F3& hit) {
            return mv::space_avoid(avoid_members_, c, h, a, b, hit);
        });
        fighter_formation_hooks();  // s78f t02
    } else {
        mv::PassabilityGrid grid = mv::grid_from_map(map_);
        // battle host: model bounds for the LAND_OBSTACLE footprints without Obstacle_Width / _Height and the
        // Fine_Tune_Occupied_Passability ray (MOVEMENT 1.3; S8 t09: the same hooks as land_obstacle_attach).
        const mv::MapStampHooks stamp_hooks = land_stamp_hooks(catalog_);
        mv::stamp_map_obstacles(grid, map_, types, &stamp_hooks, pf);  // OPT3 t11: footprints sampled in parallel
        movement_->set_grid(std::move(grid), pf);
        movement_->air().set_box({mn[0], mn[1]}, {mx[0], mx[1]});  // REMAINING_GAPS §4.3-4.6: air units plan in the playable box
        const terrain::MapData* m = &map_;
        movement_->set_height_fn([m](float x, float y) { return m->terrain.sample_height(x, y); });
        // FIGHTER_FLYING §6: flying hooks. object_position = a circling target that is not a mover (structures: the
        // combat position). clearance (0x63b640, §6.3.1) = the movers' Corvette-layer query with the type data of
        // clearance_info (MovementSystem::clearance_query).
        mv::FlyingHooks fh;
        fh.clearance = [this](mv::ObjectId id, mv::FlyVec3 from, mv::FlyVec3 to, float m) {
            return movement_->clearance_query(id, from, to, m,
                                              [this](mv::ObjectId o, mv::ClearanceInfo& ci) { return clearance_info(o, ci); });
        };
        fh.object_position = [this](mv::ObjectId id, mv::FlyVec3& pos) {
            if (!combat_ || !combat_->has_unit(id) || !combat_->alive(id)) return false;
            const tactical::Vec3 p = combat_->position(id);
            pos = mv::FlyVec3{p.x, p.y, p.z};
            return true;
        };
        movement_->set_flying_hooks(std::move(fh));
    }
    // Combat wrapped by the ability system (S9: it installs itself as the combat hooks; the forwarder adds the
    // animation hookups and replaces it as the installed hooks).
    tc::CombatConfig cc = catalog_->combat_config();  // OI tactical: GameConstants-driven defaults
    cc.mode = space_ ? tc::GameMode::Space : tc::GameMode::Land;
    cc.object_max_health_multiplier = space_ ? catalog_->space_health_multiplier() : catalog_->land_health_multiplier();
    cc.hardpoint_health_multiplier = space_ ? 1.5f : 1.0f;
    std::vector<tc::PlayerInfo> infos(players);
    for (uint32_t p = 0; p < players && p < galaxy_->players().size(); ++p) {
        infos[p].ai = !galaxy_->players()[p].human;
        // cu8 t03: the AI difficulty adjustment (TACTICAL_COMBAT §3.6 step 12 / §4.3) and the faction's retreat damage
        // factor for this mode (§3.6 step 9).
        if (const galactic::DifficultyRules* d = galaxy_->ai_difficulty(static_cast<int32_t>(p))) {
            infos[p].ai_damage_multiplier = d->damage_multiplier;
            infos[p].faction_health = d->health_multiplier;
            infos[p].faction_shield = d->shield_multiplier;
        }
        const galactic::ConflictFactionInfo& fi = rules_->faction(galaxy_->players()[p].faction);
        infos[p].retreat_damage_mod = space_ ? fi.space_retreat_damage_mod : fi.land_retreat_damage_mod;
        // TACTICAL_COMBAT §2.2: the target scan skips players whose faction is Is_Neutral (player +0x68 -> faction +0x108).
        infos[p].active = !bt_owner_neutral(static_cast<int32_t>(p));
    }
    combat_ = std::make_unique<tc::CombatSystem>(tables_->combat, cc, std::move(infos));
    // S7 t30 (TACTICAL_COMBAT §2.10) [H]: the relation table (0 friendly, 1 enemy, 2 neutral) from the skirmish / campaign
    // rules (bt_relation). Every combat path reads "enemy" = 1 (auto-target, formation target, the human attack, projectile
    // hits, area damage, stun, turret drop), so a relation-2 player (Is_Neutral faction) is attacked by nobody and attacks
    // nobody.
    for (uint32_t p = 0; p < players; ++p)
        for (uint32_t q = 0; q < players; ++q)
            if (q != p)
                combat_->set_enemy(static_cast<int32_t>(p), static_cast<int32_t>(q),
                                   bt_relation(static_cast<int32_t>(p), static_cast<int32_t>(q)) == 1);
    combat_->reserve(256, 2048, 32, 256);
    attach_rocket_terrain();  // refixf t01
    combat_->set_trees(&mode.trees());  // S8 t06: between-tick detonations (also right after a load)
    if (!space_) {
        // S8 t01 (TACTICAL_GAPS 3.1 slot 43): LAND_OBSTACLE targets' aim walk over the cells their stamp occupies (map
        // placements' stamps move to the spawned objects in bt_map_load; S8 t02: objects spawned later are stamped by
        // land_obstacles_sync). The cell centre is always written (ObstacleCellFn contract). A stamp with no cells (water
        // rule / Fine_Tune / off the grid) answers Free: retail gates the walk on the LAND_OBSTACLE behaviour (obj +0x346),
        // not on the cell count, so the walk starts on an unoccupied cell, snaps to it and uses E = sqrt(200). NoFootprint
        // (half-extent rule) only for an object never stamped [port: a type without Obstacle_Width / _Height and without
        // model extents].
        const mv::MovementSystem* ms = movement_.get();
        combat_->set_obstacle_cell_query([ms](tc::ObjectId id, tactical::Vec3 p, tactical::Vec3& c) {
            const mv::PassabilityGrid& g = ms->grid();
            const int32_t cx = mv::world_to_cell(p.x), cy = mv::world_to_cell(p.y);
            const mv::V2 cc = mv::cell_center(cx, cy);
            c = tactical::Vec3{cc.x, cc.y, p.z};
            if (!g.stamped(id)) return tc::ObstacleCell::NoFootprint;
            return g.occupies(id, cx, cy) ? tc::ObstacleCell::Occupied : tc::ObstacleCell::Free;
        });
        // S8 t10 (LOB_SOLVER §5.1): a garrisoned shooter's host for the garrison gun bones (obj +0x2b0).
        combat_->set_garrison_host_query([this](tc::ObjectId id) -> tc::ObjectId { return garrison_holder_of(id); });
    }
    abilities_ = std::make_unique<ta::AbilitySystem>(*combat_, tables_->abilities, catalog_->ability_config(space_, cfg_.skirmish));
    abilities_->reserve(256, 512);
    // --- P4 tactical: campaign Black Market gate, before any unit is added (Active_By_Default / automatic specials) ---
    for (uint32_t p = 0; p < players && p < galaxy_->players().size(); ++p) {
        std::vector<uint32_t> blocked = black_market_blocked_abilities(tables_->abilities, *galaxy_, static_cast<int32_t>(p));
        if (!blocked.empty()) abilities_->set_black_market_blocked(static_cast<int32_t>(p), std::move(blocked));
    }
    // --- end P4 tactical ---
    hooks_ =std::make_unique<BattleHooks>(*this, *abilities_);
    combat_->set_hooks(hooks_.get());
    if (mode.has_model_source()) {
        // refixf t07 (TACTICAL_COMBAT §3.4, HOTSPOTS 0x2abcf0): per Query_Ray entry, the precise model test (SPACE_AVOIDANCE
        // §5, the object's committed pose: position, planar facing, Scale_Factor), else, with Collision_Box_Modifier > 1,
        // the start, end and midpoint of the segment against a sphere of radius max_extent x modifier. [port] max_extent =
        // the largest half extent of the object's §3.1 world box and the sphere is centred on the position (neither is
        // pinned in docs/re); the hit fraction is the first point inside (start 0, end 1, midpoint 0.5). Read-only: the
        // sweeps run on job workers (resolve_models() runs serially before them).
        const TacticalMode* tm = &mode;
        combat_->set_precise_hit([tm](const tc::PreciseQuery& q, tc::PreciseResult& r) {
            const objects::ObjectStore& st = tm->primary();
            const objects::ObjectHandle h = st.find(q.victim);
            if (!st.exists(h)) return tc::PreciseOutcome::NotModelled;
            mv::AvoidObstacle o;
            if (!tm->obstacle_of(h, o, false)) return tc::PreciseOutcome::NotModelled;
            o.collide = q.collide;
            const mv::F3 a{q.a.x, q.a.y, q.a.z}, b{q.b.x, q.b.y, q.b.z};
            mv::AvoidSegmentHit hit;
            if (mv::avoid_precise_segment(o, a, b, q.mask, q.shield_up ? 2u : 0u, hit)) {
                r.t = hit.t;
                r.mesh = hit.mesh >= 0 ? &o.model->meshes[static_cast<size_t>(hit.mesh)].name : nullptr;
                return tc::PreciseOutcome::Hit;
            }
            const float k = tm->collision_box_modifier(st.type(h));
            if (k > 1.0f) {
                const float rad = std::max(std::max(o.box_h.x, o.box_h.y), o.box_h.z) * k;
                const mv::F3 pts[3] = {a, b, {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f}};
                const float ts[3] = {0.0f, 1.0f, 0.5f};
                for (int32_t i = 0; i < 3; ++i) {
                    const float dx = pts[i].x - o.pos.x, dy = pts[i].y - o.pos.y, dz = pts[i].z - o.pos.z;
                    if (dmath::sqrt((dx * dx + dy * dy) + dz * dz) <= rad) {
                        r.t = ts[i];
                        r.mesh = nullptr;
                        return tc::PreciseOutcome::Hit;
                    }
                }
            }
            return tc::PreciseOutcome::Miss;
        });
    }
    if (space_) {  // S8 t17 (FIGHTER_FLYING §4.10 A): the scan cube R + Rr of fighters with a formation (kUnitScanQuery)
        const TacticalMode* tm = &mode;
        combat_->set_scan_range_query([this, tm](tc::ObjectId u, tc::ObjectId c, float range, float& rr) {
            return fighter_scan_range(tm->primary(), u, c, range, rr);
        });
    }
    if (!space_)  // s78n t20 (FIGHTER_FLYING §4.11 E, 0x40edd0): can_hit's land line-of-fire term 0x3b5ad0 (L1_S4_REMAINING §1.2)
        combat_->set_shield_sides_query([this](tactical::Vec3 a, tactical::Vec3 b, bool shield_damage) {
            return opposite_sides_of_shield(sv(a), sv(b), -1, true, shield_damage);
        });
    if (movement_) {
        // s78n t26 review round 2 (MOVEMENT §4.3 correction [H]): the formation layout's rank key, Get attack range
        // 0x3749d0 (the value land_attack_order reads); a squad container through its first live member. bkm1 t05
        // (b0037): space formations rank by it too
        movement_->set_attack_range_query([this](mv::ObjectId id) {
            const objects::ObjectId unit = attack_lead(id);
            return unit != objects::kNoObjectId && combat_->has_unit(unit) ? combat_->attack_range(unit) : 0.0f;
        });
    }
    if (!space_ && movement_) {
        // s78n t25.3 (MOVEMENT §4.3.2 items 6 / 7, 0x40edd0 / 0x410e90; TACTICAL_COMBAT §2.8 B 4): the movement's fire
        // test - can the mover hit the target from p: slots 43 / 46 and the line of fire from p at the ground height
        // (CombatSystem::can_hit_from; a squad container through its first live member). Yes or NoTarget (nothing
        // tested) = it can fire. s78n t25.4: los_only (a guard block's trim, item 7: the movement tests the guard range)
        // answers the line of fire alone (CombatSystem::line_of_fire_from).
        movement_->set_fire_test([this](mv::ObjectId mover, mv::V2 p, mv::ObjectId target, bool los_only, mv::V2 g, float gz) {
            const objects::ObjectId unit = attack_lead(mover);
            if (unit == objects::kNoObjectId || !combat_->has_unit(unit)) return true;
            tactical::Vec3 from = combat_->can_hit_origin(unit);
            from.x = p.x;
            from.y = p.y;
            // bkt1 t06 (b0079, MOVEMENT §4.3.2 item 7 / 0x40f7c0): from p at the terrain height, or the water height for a
            // Water-capable class (the mover's movement mask has the Water bit) where the water lies above; to G at its
            // height (the movement's goal point); CombatSystem raises both by their Ranged_Target_Z_Adjust
            from.z = map_.terrain.sample_height(p.x, p.y);
            if ((movement_->mask_of(mover) & mv::kPassWater) != 0u && map_.terrain.header.water_type != terrain::WaterType::None)
                from.z = std::max(from.z, map_.terrain.header.water_height);
            const tactical::Vec3 to{g.x, g.y, gz};
            const tc::CanHit c = los_only ? combat_->line_of_fire_from(unit, from, target, &to) : combat_->can_hit_from(unit, from, target, &to);
            return c == tc::CanHit::Yes || c == tc::CanHit::NoTarget;
        });
        // s78n t26 review round 2 (MOVEMENT §4.3 correction [H]): the formation layout's rank key, Get attack range
        // 0x3749d0 (the value land_attack_order reads); a squad container through its first live member
        movement_->set_attack_range_query([this](mv::ObjectId id) {
            const objects::ObjectId unit = attack_lead(id);
            return unit != objects::kNoObjectId && combat_->has_unit(unit) ? combat_->attack_range(unit) : 0.0f;
        });
        // bkm1 t04 (MOVEMENT §4.2 step 2 item 7 test 9): Targeting_Min_Attack_Distance for the recruitment's engagement test
        movement_->set_min_attack_distance_query([this](mv::ObjectId id) {
            const objects::ObjectId unit = attack_lead(id);
            return unit != objects::kNoObjectId && combat_->has_unit(unit) ? combat_->min_attack_distance(unit) : 0.0f;
        });
    }
    combat_->bind_rng(&rng);  // Take_Damage listeners called outside the tick (host damage) draw here
    hg_setup(mode);           // hangars: SPAWN_SQUADRON descriptors and the HangarSystem (before any unit exists)
    oi_install(mode, rng);    // OI abilities: AbilityHost hooks, the abilities' RNG, repair credit hooks
#ifdef ALAMO_SIM_HAVE_ANIM
    if (anim_) hooks_->set_anim(anim_, &anim_->world(mode), mode.kind());
    if (anim_) hooks_->set_anim_mode(&mode);  // R3 anim: behaviour hand-offs
#endif
    retreat_allowed_frame_ = static_cast<uint32_t>((space_ ? rules_->space_retreat_allowed : rules_->land_retreat_allowed) * 30.0f);
    fog_begin(mode, players);  // fog of war: grids (owner: fog agent)
    bt_map_load(mode, rng);      // REMAINING_GAPS §3.1: map objects in map order (Map_Load_Spawn_Table draws)
    // cu8g t02 (CU8G_T02_ENV_PICK_RE §1 / §3 step 7): the environment pick, the map loader's last synced draw - after the
    // chunk-0x102 objects and their spawn-table draws, before every post-load step (skirmish base / start forces, the
    // campaign starbase / structures). Not saved: a loaded battle's read_part(State) takes the map's start index instead.
    env_index_ = pick_environment(map_.environments, rng);
    // cu8i t02 (CU8G_T02_ENV_PICK_RE §7 / §8 item 7): Load_Map's tail, after the pick: the deferred GOM post-load pass
    // (0x2b4440 -> 0x3952e0 -> 0x39bcb0) puts each movable object without a movement coordinator into a new one, which
    // creates a formation: one FormationClass ctor draw int(1, FPS) per object, in object order, whatever the owner: the
    // port's inert Is_Neutral map units count too (_MP_LAND_ENDOR: its one Land_Speeder, retail 1 draw). cu8j t02
    // (MOVEMENT §4.7.3): land and space alike, through the full adopt predicate (mv_adopt: the mode's locomotor kind).
    {
        const objects::ObjectStore& st = mode.primary();
        for (const objects::ObjectHandle h : st.all_objects()) (void)mv_adopt(mode, h, rng);
    }
    bt_pad_restore(mode);        // §6.5: persistent pad children
    bt_skirmish_base(mode);      // t50: the land skirmish's pre-built base
    l1_setup(mode);            // L1 systems: special weapons / hyperspace / engines (before the deploys)
    bt_place_forces(mode, rng, pf);  // §3.2: starbase, structures, defender / attacker start forces
    bt_landing_points(mode);     // §3.3: the attacker's map-owned reinforcement points
    mode.finish_load(pf);
    space_obstacles_sync(mode);  // refixf t02: the starbase / structures as space static entries (MOVEMENT §2.2)
    land_obstacles_sync(mode, pf);   // S8 t02: LAND_OBSTACLE objects spawned during the setup (MOVEMENT §1.3)
    bt_shield_stamps(mode);  // battle host: base-shield secondary occupants (land)
    fog_after_deploy(mode);  // fog of war: dense mask, battle scale (owner: fog agent)
    vm_setup(mode);          // story t06: victory monitor records (§2.5) and the start's victory-relevant objects
    // OPT3 t09: the setup's tables are complete. q6 t12: the build is kept (serial preload from now on) so a type first
    // spawned by a game-object script can join the live tables (script_add_types); nothing else extends them after setup.
    tables_build_->pf = serial_for;
}

// S8 t17 (FIGHTER_FLYING §4.10 B): each combat unit's squadron for the shared team target. Derived state (not saved):
// refreshed at the tick start and after a load, so a command applied before the first tick after a load sees the same
// groups as an uninterrupted run.
void TacticalBattle::share_groups_sync() {
    if (!space_ || !movement_ || !combat_) return;
    for (const Unit& u : units_)
        if (combat_->has_unit(u.id))
            combat_->set_share_group(u.id, u.container < containers_.size() && movement_->fighter_squad(containers_[u.container].id)
                                               ? containers_[u.container].id
                                               : tc::kNoObject);
}

void TacticalBattle::attach_rocket_terrain() {
    // refixf t01 (S9_DEATH_CLONE_SHIELDS.md §2.4.5 mode 1, §2.4.6): land spline rockets follow the heightmap and stay
    // inside its extent box (0x1320d0: centre (W c / 2, H c / 2), half sizes W c / 2, H c / 2). [M] W c, H c are taken as
    // the port grid's covered extent ((width - 1) x kVertexSpacing, origin 0: the frame of Terrain::sample_height).
    // Space battles and maps without a heightmap: flat ground at z 0, no box. Not saved: re-attached after a load.
    if (!combat_) return;
    tc::RocketTerrain rt;
    const terrain::Terrain& tr = map_.terrain;
    if (!space_ && tr.present && tr.width >= 2 && tr.height >= 2) {
        const terrain::Terrain* tp = &tr;
        rt.height = [tp](float x, float y) { return tp->sample_height(x, y); };
        const float w = static_cast<float>(tr.width - 1) * terrain::kVertexSpacing;
        const float h = static_cast<float>(tr.height - 1) * terrain::kVertexSpacing;
        rt.has_box = true;
        rt.cx = 0.5f * w;
        rt.cy = 0.5f * h;
        rt.hx = 0.5f * w;
        rt.hy = 0.5f * h;
        // S8 t06 (LOB_SOLVER.md §6.6): the grid for the projectile ground test (cell triangles + water plane), in the
        // same frame (origin 0, kVertexSpacing cells, int16 heights x kHeightScale).
        rt.grid.cell = terrain::kVertexSpacing;
        rt.grid.width = tr.width;
        rt.grid.height = tr.height;
        rt.grid.heights = tr.heights;
        rt.grid.height_scale = terrain::kHeightScale;
        rt.grid.water_height = tr.header.water_height;
        rt.grid.water_type = static_cast<uint32_t>(tr.header.water_type);
    }
    combat_->set_rocket_terrain(std::move(rt));
}

Vec3 TacticalBattle::slot_position(uint8_t side, uint32_t mask) {
    const uint32_t k = placed_[side]++;
    const float spacing = space_ ? cfg_.space_spacing : cfg_.land_spacing;
    const float row = static_cast<float>(k / 6u), col = static_cast<float>(k % 6u) - 2.5f;
    const Vec3 f = facing_[side];
    const Vec3 right{-f.y, f.x, 0.0f};
    const Vec3 p{anchor_[side].x - f.x * row * spacing + right.x * col * spacing,
                 anchor_[side].y - f.y * row * spacing + right.y * col * spacing, 0.0f};
    return snap_passable(p, mask, true);
}

Vec3 TacticalBattle::snap_passable(Vec3 p, uint32_t mask, bool always) const {
    float mn[2], mx[2];
    if (map_.playable_box(mn, mx)) {
        p.x = std::clamp(p.x, mn[0], mx[0]);
        p.y = std::clamp(p.y, mn[1], mx[1]);
    }
    if (!space_ && movement_) {
        const mv::PassabilityGrid& g = movement_->grid();
        if (!always && g.passable_world({p.x, p.y}, mask)) {
            p.z = map_.terrain.sample_height(p.x, p.y);
            return p;
        }
        const int32_t cx = mv::world_to_cell(p.x), cy = mv::world_to_cell(p.y);
        bool found = false;
        for (int32_t r = 0; r <= 60 && !found; ++r)
            for (int32_t dy = -r; dy <= r && !found; ++dy)
                for (int32_t dx = -r; dx <= r && !found; ++dx) {
                    if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
                    if (!g.passable_cell(cx + dx, cy + dy, mask)) continue;
                    const mv::V2 c = mv::cell_center(cx + dx, cy + dy);
                    p.x = c.x;
                    p.y = c.y;
                    found = true;
                }
        p.z = map_.terrain.sample_height(p.x, p.y);
    }
    return p;
}

void TacticalBattle::deploy(TacticalMode& mode, uint32_t pi, SimRng& rng, uint32_t member_first, uint32_t member_count) {
    Participant& P = parts_[pi];
    const bool range = member_first != 0 || member_count != 0xFFFFFFFFu;  // a1i t04: one landing unload step
    if (P.deployed && !range) return;
    P.deployed = true;
    const objects::GameObjectTypes& types = catalog_->types();
    const uint8_t side = P.side;
    const bool team = !rules_->type(P.type).members.empty();
    if (P.unplaced) return;  // §3.2 step 4: no free marker -> not spawned (the galactic structure is untouched)
    // 1. member objects (each consumes an id), movement.
    const size_t first = units_.size();
    std::vector<uint32_t> members_types = spawn_types(P);
    if (P.other_type >= 0) members_types.assign(P.spots.size(), static_cast<uint32_t>(P.other_type));  // §3.2 step 3
    size_t spot = 0;
    const size_t mk_end = std::min(members_types.size(), static_cast<size_t>(member_first) + member_count);
    for (size_t mk = member_first; mk < mk_end; ++mk) {
        const uint32_t t = members_types[mk];
        mv::UnitDesc md = mv::unit_desc_from_type(types, t);
        md.locomotor = mv::locomotor_kind_from_type(types, t, space_ ? mv::MoveMode::Space : mv::MoveMode::Land);  // REMAINING_GAPS §4.3-4.7
        Vec3 pos;
        Vec3 face = facing_[side];
        // s78i t02 (TACTICAL_COMBAT §3.1.1 "other spawns"): a reserve drop (0x2b5fe0 land / 0x2bf2d0 space) gives every
        // object it creates the owner's entry-facing record (mode +0x288; yaw 0 when unset). A landing object's company
        // takes the per-member unload facing of drop_face_ (s78j t01).
        if (P.reserve && !range && !deploy_unload_) {
            const Vec3 df = side < 2 ? drop_facing_[side] : Vec3{};  // as spawn_landing: the participant's side record
            face = df.x == 0.0f && df.y == 0.0f ? facing_of_yaw(0.0f) : df;
        }
        if (mk < drop_face_.size()) face = drop_face_[mk];
        const bool lead_sw = space_ && bt_have_ds_ && !P.reserve && bt_lead_ == objects::kNoObjectId && bt_is_super_weapon(t);
        if (spot < P.spots.size()) {  // §3.2: the marker chosen for the starbase / structure / step-3 type
            pos = P.spots[spot].first;
            face = P.spots[spot].second;
            ++spot;
        } else if (P.starbase) {
            pos = {anchor_[side].x - facing_[side].x * 600.0f, anchor_[side].y - facing_[side].y * 600.0f, 0.0f};
        } else if (lead_sw) {
            pos = bt_ds_pos_;  // battle host: the spawned super-weapon at DEATH_STAR_POSITION (TACTICAL_GAPS §4.8) [M]
        } else if (!drop_pos_.empty()) {  // a1g t04 follow-up: order_reinforce's land drop layout (member order)
            // a1h t06: the slots passed land_drop_valid, so they are used as they are (no clamp, no nearest-cell move).
            const size_t k = mk;
            if (k < drop_pos_.size()) {
                pos = drop_pos_[k];
                if (!space_) pos.z = map_.terrain.sample_height(pos.x, pos.y);
            } else {
                pos = slot_position(side, movement_->classes().mask(md.movement_class));
            }
        } else {
            pos = slot_position(side, space_ ? 0u : movement_->classes().mask(md.movement_class));
        }
        objects::SpawnParams sp;
        sp.type = t;
        sp.owner = P.owner;
        sp.position = pos;
        sp.facing = face;
        sp.parent_mode_id = P.built ? objects::kNoObjectId : P.galactic_id;  // q6 t19: a pool entry from tactical production
        const objects::ObjectHandle h = mode.spawn(sp, 1.0f);
        if (lead_sw) {  // battle host: lead object +0x28 = the spawned super-weapon's id
            bt_lead_ = mode.primary().id(h);
            bt_lead_participant_ = pi;
            bt_used_ = true;
        }
        Unit u;
        u.id = mode.primary().id(h);
        u.h = h;
        u.participant = pi;
        u.type = t;
        u.side = side;
        u.movable = mode.movable(h) && !P.starbase && !P.structure;
        const auto& ti = galaxy_->rules().types;
        u.power = t < ti.size() ? ti[t].ai_combat_power : 0.0f;
        if (u.movable) {
            md.id = u.id;
            md.pos = {pos.x, pos.y};
            md.z = pos.z;
            md.yaw_deg = yaw_of(face);
            md.owner = P.owner;  // OI tactical: crush hostility
            mv_type_extents(md, t);  // s78n t20
            movement_->add_unit(md);
        }
        if (side < 2 && u.movable && !P.reserve && !P.built && !stats_.start_placed[side]) {  // s78n t20 (stats only)
            stats_.start_placed[side] = true;
            stats_.first_start[side] = pos;
        }
        units_.push_back(u);
        ++P.spawned;
        ++P.alive;
        ++stats_.spawned;
        if (P.reserve) story_reinforced_.push_back(u.id);  // L1/S4 wave 2b story hook (STORY_REINFORCE)
    }
    // 2. REMAINING_GAPS §3.4 team helper 0x2a5a60 (start placement 0x2a00b0): it runs when the first spawned member's type
    //    has Create_Team, or always in space. Members in list order in chunks of min(k, remaining), k = the company's
    //    Max_Squad_Size (default 10); per chunk one object of the Create_Team_Type (fallback "Team"; neither resolves ->
    //    nothing), owner / facing of the chunk's first member, position = the chunk's float centroid [M: the land
    //    passability snap of the centroid is not applied], TEAM behaviour type = the team type, or the company type when
    //    Create_Team_Type is empty. Land companies without Create_Team keep separate members (no container). Containers
    //    are created after every member (ids ascending: members, then the teams in chunk order); then the members join
    //    combat / abilities in id order, then the containers (S10-int: not in the trees, not combat units; the ability
    //    system services them). [M] Space formation offsets of the members are not applied (members keep their slots).
    uint32_t team_obj = 0xFFFFFFFFu, team_beh = 0xFFFFFFFFu, squad = 0;
    const bool run_team = team && units_.size() > first && bt_team(P.type, units_[first].type, team_obj, team_beh, squad);
    std::vector<std::vector<tc::ObjectId>> groups;
    const size_t c0 = containers_.size();
    if (run_team) {
        const size_t n = units_.size() - first, g = static_cast<size_t>(squad);
        for (size_t s = 0; s < n; s += g) {
            const size_t end = std::min(n, s + g);
            Vec3 c{0.0f, 0.0f, 0.0f};
            for (size_t i = first + s; i < first + end; ++i) {
                const Vec3 p = mode.primary().position(units_[i].h);
                c.x = c.x + p.x;
                c.y = c.y + p.y;
                c.z = c.z + p.z;
            }
            const float inv = static_cast<float>(end - s);
            objects::SpawnParams sp;
            sp.type = static_cast<objects::TypeIndex>(team_obj);
            sp.owner = mode.primary().owner(units_[first + s].h);
            sp.position = Vec3{c.x / inv, c.y / inv, c.z / inv};
            sp.facing = mode.primary().facing(units_[first + s].h);
            const objects::ObjectHandle ch = mode.primary().create(sp);
            Container ct;
            ct.id = mode.primary().id(ch);
            ct.h = ch;
            ct.team = true;
            ct.type = team_beh;
            ct.owner = sp.owner;
            ct.squad_type = P.type;  // S7 fighters: Squadron_Formation_Error_Tolerance
            ct.alive = static_cast<uint32_t>(end - s);
            groups.emplace_back();
            for (size_t i = first + s; i < first + end; ++i) {
                units_[i].container = static_cast<uint32_t>(containers_.size());
                groups.back().push_back(units_[i].id);
            }
            containers_.push_back(ct);
            ++stats_.containers;
            bt_used_ = true;
            // cu8l t01 (CU8K_T02_T06_RE §1.2, §4 rules 1 / 4): a land team container's LandTeamContainer locomotor
            // registers it at creation (0x5abac0: its own coordinator, Move_To its own position, no formation): no draw
            // now; the coordinator composes at the next step 10 (one int(1, FPS), coordinator creation order) unless an
            // order takes the members first. s78n t03 (TACTICAL_COMBAT §5.4.4 item 2): the container is a movement unit
            // walking its own path, its members steering to their slots, so its own coordinator holds it. [port] A squad
            // with a member that cannot steer (mv_squad_add false) keeps the members standing in for the container.
            if (!space_ && movement_ && mv_self_registers(team_obj)) {
                if (mv_squad_add(mode, containers_.size() - 1, team_obj)) {
                    const mv::ObjectId self[] = {ct.id};
                    (void)movement_->hold_from_birth(self);
                } else {
                    (void)movement_->hold_from_birth(groups.back());
                }
            }
        }
    }
    // 3. combat / ability units (hardpoint draws, HardPoints order), then the containers.
    for (size_t i = first; i < units_.size(); ++i) {
        Unit& u = units_[i];
        tc::UnitSpawn cs;
        cs.id = u.id;
        cs.desc = desc_of(u.type);
        cs.owner = P.owner;
        cs.position = tv(mode.primary().position(u.h));
        cs.facing = tv(mode.primary().facing(u.h));
        // s78f t07 (TACTICAL_COMBAT §3.1.1 "Team targeting copies"): a land team member joins combat with its team (the
        // container's object id); the chunk's first member (list order, the lowest id, as slot 31's 0x4ec820) is the
        // team_leader standing in for the container, which is no combat unit here. This runs the TeamTargeting copy,
        // team scoring and the Take_Damage redirect / share and convert team rules. [port] Space squadrons keep their
        // FIGHTER_FLYING §4.10 B shared target and are not registered (no space team targeting service).
        if (!space_ && u.container != kNoContainer) {
            cs.team = static_cast<uint32_t>(containers_[u.container].id);
            cs.team_leader = i == first || units_[i - 1].container != u.container;
        }
        // s78g t06 (TACTICAL_COMBAT §3.6 step 2, no mode gate): a space squadron member still counts in its squadron for
        // the Take_Damage redirect / share rules (vanilla Luke_XWing in Red Squadron, TIE_Prototype in Vader's squadron).
        if (space_ && u.container != kNoContainer) cs.damage_team = static_cast<uint32_t>(containers_[u.container].id);
        abilities_->add_unit(cs, cs.desc, frame_, rng, u.container != kNoContainer ? containers_[u.container].id : tc::kNoObject);
        hg_adopt(u, rng);  // hangars: engine mode enter, SPAWN_SQUADRON add_carrier (attach order)
        u.range = combat_->attack_range(u.id);
        s4_register(mode, u);  // S4: garrison holder / occupant
        l1_register(mode, u.h, static_cast<int32_t>(u.type));  // L1 systems: special-weapon / TSW pack
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
        if (mode.primary().has_behavior(u.h, kDamageTrackingBehavior)) {
            damage_tracking_.enable(u.id, combat_->max_health(u.id), combat_->max_shield_of(u.id));
            u.tracked = true;
            u.dt_next = frame_;  // a new behaviour instance is due at once
        }
#endif
    }
    // cu8j t02 (MOVEMENT §4.7.3): the team helper 0x2a5a60 adopts each chunk's container (0x39bcb0, one FormationClass
    // ctor draw when it qualifies) right after creating it, before the next chunk: the adopt follows each container's
    // creation draws. The skirmish company placer runs the helper only for a Create_Team first member (the port's space
    // containers of other companies are not adopted).
    // cu8l t01 (CU8K_T02_T06_RE §4 rule 4): it draws only in space (TeamLocomotor squadrons); a land container already
    // sits in the coordinator its locomotor made (above), so the predicate's check 5 fails: no draw.
    // cu8l t02 (CU8K_T02_T06_RE §3 row 0x6a1f90, rule 8): Lua Spawn_Unit's company path runs the same placer 0x29e960 and
    // helper, so a script-spawned space container adopts at once too (with the placer's Create_Team condition [M]).
    const bool helper = (cfg_.skirmish || deploy_script_) ? units_.size() > first && units_[first].type < types.size() &&
                                                                types.get_bool(units_[first].type, "Create_Team")
                                                          : true;
    for (size_t k = 0; k < groups.size(); ++k) {  // OI abilities: + the container's SAP-init draws
        abilities_->add_container(containers_[c0 + k].id, desc_of(team_beh), P.owner, groups[k], frame_, &rng);
        if (space_ && helper) (void)mv_adopt(mode, containers_[c0 + k].h, rng);
    }
    // cu8l t01 (CU8K_T02_T06_RE §1.5, §4 rules 1 / 3) [port interim rule, untraced: the space skirmish trace is still
    // missing]: a ship whose locomotor is SimpleSpace registers itself at creation like a land container (0x625090): no
    // draw now, one space composition (0x5d58f0) at the next step 10, in unit order. [port] Not for a SimpleSpace team
    // member [L: whether its own registration survives joining the team is not read]. cu8l t03: the campaign start forces
    // too - the fleet placement then teleports (the pending composition empties: no draw) and re-adopts every placed
    // object, so only an unplaced ship composes; and the campaign spawn's adopt below finds it in that coordinator (no
    // draw: retail traced no 0x2a1bcd adopt for the Kashyyyk ships).
    if (space_ && movement_)
        for (size_t i = first; i < units_.size(); ++i)
            if (units_[i].container == kNoContainer && units_[i].movable && mv_self_registers(units_[i].type)) {
                const objects::ObjectId one[] = {units_[i].id};
                (void)movement_->hold_from_birth(one);
            }
    // cu8j t02: the campaign spawn 0x2a00b0 (start forces of a galactic battle) then adopts each member not in a team, one
    // draw each (land vehicles, heroes, ...). Skirmish start forces and reinforcements do not.
    if (!cfg_.skirmish && !P.reserve && !deploy_script_ && !range)
        for (size_t i = first; i < units_.size(); ++i)
            if (units_[i].container == kNoContainer) (void)mv_adopt(mode, units_[i].h, rng);
}

// refixf t02 (MOVEMENT §2.2 [H]): SpaceObstacleBehaviorClass inserts its object into the space occupancy as a static entry on
// attach and removes it on detach. Map placements enter through mv::space_from_map (ids mv::map_stamp_id(placement)); S7 t13:
// bt_map_load moves each entry to the object spawned for it (SpaceOccupancy::rekey_static; a Map_Load_Spawn_Table pick is
// re-sized by the type spawned, a placeholder that spawns nothing loses its entry), so a map-adopted unit's entry leaves with
// it. Battle units whose type has SPACE_OBSTACLE (the starbase, space structures, upgraded / built stations) enter here under
// their object id, so the destination search (§2.4) and the planner keep ships off them. Before, the starbase was no obstacle:
// an Attack_Move / Move_To on it (one order per task-force unit) sent each ship to its exact centre, the ships stacked on one
// point and Project_By_Unit_Range gave nil (foc-station seeds 1 and 4). [port] attach = the first sync (end of setup, end of
// each tick) with the unit live, detach = the first sync after it died or retreated; footprint as space_from_map (radius) at
// the attach position + Space_Obstacle_Offset rotated by the facing [M: rotation sense]. S7 t07: a type without an XML radius
// gets its space model's collision-bound extent (MOVEMENT §2.2); the model's collide switch is not consulted (SPACE_AVOIDANCE
// §5.5).
void TacticalBattle::space_obstacles_sync(const TacticalMode& mode) {
    if (!space_ || !movement_) return;
    mv::SpaceOccupancy& occ = movement_->space();
    const objects::ObjectStore& st = mode.primary();
    // S8 t09: an entry re-keyed by bt_map_load to an object that is not a battle unit (an asteroid / prop with
    // SPACE_OBSTACLE) leaves with that object, as land_obstacles_sync does for stamps (ascending id).
    for (const mv::ObjectId id : occ.static_ids())
        if (id > 0 && !st.alive(st.find(id))) occ.remove_static(id);
    for (const Unit& u : units_) {
        if (!u.alive || u.retreated || !st.alive(u.h)) {
            occ.remove_static(u.id);
            continue;
        }
        if (!occ.has_static(u.id)) space_obstacle_attach(mode, u.h);
    }
}

bool TacticalBattle::space_obstacle_attach(const TacticalMode& mode, objects::ObjectHandle h) {
    const objects::ObjectStore& st = mode.primary();
    if (!space_ || !movement_ || !st.alive(h)) return false;
    const objects::GameObjectTypes& types = catalog_->types();
    mv::SpaceObstacleHooks so_hooks;
    const BattleCatalog* so_cat = catalog_;
    so_hooks.model_half_extents = [so_cat](uint32_t t, float& hx, float& hy) { return so_cat->space_model_half_extents(t, hx, hy); };
    const auto t = static_cast<uint32_t>(st.type(h));
    float r = 0.0f;
    uint32_t bits = 0;
    if (!mv::space_obstacle_footprint(types, t, r, bits, &so_hooks)) return false;
    const Vec3 p = st.position(h), f = st.facing(h);
    const float len = dmath::sqrt(f.x * f.x + f.y * f.y);
    const mv::V2 fu{len > 0.0f ? f.x / len : 1.0f, len > 0.0f ? f.y / len : 0.0f};
    movement_->space().add_static(st.id(h), mv::space_obstacle_center(types, t, {p.x, p.y}, fu), r, bits);
    return true;
}

// S8 t02 (MOVEMENT §1.3 [H]): LandObstacleBehaviorClass stamps its object's footprint into the passability grid on attach and
// unstamps it on detach. Map placements are stamped at setup (mv::stamp_map_obstacles) and their stamps move to the objects
// spawned for them in bt_map_load; everything spawned later (the starbase / structures / persisted pad children at setup,
// built pads, reinforcement structures, script spawns, death clones whose type has LAND_OBSTACLE) is stamped here under its
// object id, and every stamped object that left the store (died, sold, removed) is unstamped. [port] attach / detach = this
// sync: end of setup (also after the structures deploy, before the start forces: their spawn ring search sees the
// structures), end of each tick (the next tick's pathing and targeting see the change). Gone first (ascending id), then new
// (the store's LAND_OBSTACLE attach order); footprint as stamp_map_obstacles at the object's position, yaw (facing = Rz(yaw)
// (0, -1), as map placements) and height, with the same hooks (land_stamp_hooks: Obstacle_Proxy_Type and the
// Fine_Tune_Occupied_Passability ray apply to every stamp, S8 t09). A death clone is in the LAND_OBSTACLE table iff it kept
// the behaviour through TacticalMode::spawn_death_clone's strip (Death_Clone_Is_Obstacle, MOVEMENT §1.3 [H]). Placement
// stamps never stay under a placement id here: bt_map_load hands each to its object or unstamps it (skipped company,
// no-spawn placeholder).
void TacticalBattle::land_obstacles_sync(const TacticalMode& mode, const ParallelFor& pf) {
    if (space_ || !movement_) return;
    const objects::ObjectStore& st = mode.primary();
    mv::PassabilityGrid& grid = movement_->grid();
    for (const mv::ObjectId id : grid.stamped_ids())
        if (id > 0 && !st.alive(st.find(id))) grid.unstamp(id);
    for (const objects::ObjectHandle h : st.behavior_table(objects::beh::LandObstacle).insertion)
        if (st.alive(h) && !grid.stamped(st.id(h))) land_obstacle_attach(mode, h, pf);
}

bool TacticalBattle::land_obstacle_attach(const TacticalMode& mode, objects::ObjectHandle h, const ParallelFor& pf) {
    const objects::ObjectStore& st = mode.primary();
    if (space_ || !movement_ || !st.alive(h)) return false;
    const objects::GameObjectTypes& types = catalog_->types();
    const auto t = static_cast<uint32_t>(st.type(h));
    if (t >= types.size()) return false;
    if (land_obstacle_none_.size() != types.size()) land_obstacle_none_.assign(types.size(), 0);
    if (land_obstacle_none_[t]) return false;
    const mv::MapStampHooks hooks = land_stamp_hooks(catalog_);
    const Vec3 p = st.position(h), f = st.facing(h);
    // yaw of the facing Rz(yaw) (0, -1) = (sin yaw, -cos yaw): the map placements' convention (bt_map_load yaw_facing)
    const float yaw = (f.x == 0.0f && f.y == 0.0f) ? 0.0f : dmath::atan2(f.x, -f.y) * (180.0f / dmath::kPiF);
    mv::ObstacleFootprint fp;
    if (!mv::land_obstacle_footprint(types, t, {p.x, p.y}, yaw, p.z, &hooks, fp)) {
        land_obstacle_none_[t] = 1;  // const type data (sizes, proxy, ray model): never retried
        return false;
    }
    // S8 t13 (MOVEMENT §1.3 "proxy bits"): the bits come from the object's own live behaviours (0x397350), never the proxy.
    fp.bits = mv::land_obstacle_bits(static_cast<uint32_t>(types.get_mask(t, "OverridePassability")), true,
                                     st.has_behavior(h, kWallBehavior));
    // OPT3 t12: the Fine_Tune rays (a whole-mesh test per 10-unit sample; a skirmish base structure has ~400 samples on
    // ~14k triangles) run through pf. The hook is const catalog data (as stamp_map_obstacles).
    movement_->grid().stamp(st.id(h), fp, pf);
    return true;
}

// ---- tick ----------------------------------------------------------------------------------------------------------

float TacticalBattle::side_power(uint8_t side) const {
    float p = 0.0f;
    for (const Unit& u : units_)
        if (u.side == side && u.alive && !u.retreated && !parts_[u.participant].third) p += u.power;
    for (const Participant& q : parts_)
        if (q.side == side && !q.deployed && !q.third) p += q.power;
    return p;
}

void TacticalBattle::reinforce(TacticalMode& mode, SimRng& rng, bool force_empty_sides) {
    if (arrival_held()) return;  // story t12: reinforcements wait in hyperspace
    for (uint8_t side = 0; side < 2; ++side) {
        bool empty = true;
        float live_pop = 0.0f;
        std::vector<uint8_t> counted(parts_.size(), 0);
        for (const Unit& u : units_)
            if (u.side == side && u.alive && !u.retreated && !parts_[u.participant].third) {
                empty = false;
                if (!counted[u.participant]) {
                    counted[u.participant] = 1;
                    live_pop += parts_[u.participant].pop;
                }
            }
        const bool due = cfg_.reinforce_interval > 0 && frame_ % cfg_.reinforce_interval == 0;
        if (!due && !(force_empty_sides && empty)) continue;
        const int32_t player = side_player(side);
        const uint32_t f = player >= 0 && static_cast<size_t>(player) < galaxy_->players().size()
                               ? galaxy_->players()[static_cast<size_t>(player)].faction
                               : galactic::kNoIndex;
        const float cap = rules_->faction(f).space_unit_cap;
        bool first = true;
        for (uint32_t i = 0; i < parts_.size(); ++i) {
            Participant& p = parts_[i];
            if (p.side != side || !p.reserve || p.deployed) continue;
            if (!empty && commanded_by_player(p.owner)) continue;  // P host: a human player's reserves deploy by Reinforce commands
            const bool fits = !space_ || live_pop + p.pop <= cap;
            if (!fits && !(empty && first)) continue;  // [port] an empty side always gets its next pool unit
            deploy(mode, i, rng);
            live_pop += p.pop;
            ++stats_.reinforcements;
            first = false;
        }
    }
}

void TacticalBattle::issue_orders(TacticalMode& mode, SimRng& rng) {
    const objects::ObjectStore& store = mode.primary();
    for (const Unit& u : units_) {
        if (!u.alive || u.retreated || !u.movable) continue;
        if (commanded_by_player(store.owner(u.h))) continue;  // integration fixes: human units take commands only
        if (!space_ && !standin_owner(store.owner(u.h))) continue;  // bkt1 t07 (b0084): land third parties idle
        const Vec3 up = store.position(u.h);
        const Unit* best = nullptr;
        float best_d2 = 0.0f;
        for (const Unit& e : units_) {
            if (!e.alive || e.retreated || e.side == u.side) continue;
            // s78n t25.3 [port]: on land the pick is an enemy of the unit's owner (the auto-attack push 0x41fd30 names what
            // the targeting scan picked, never a neutral object)
            if (!space_ && combat_ && !combat_->is_enemy(store.owner(u.h), store.owner(e.h))) continue;
            const Vec3 ep = store.position(e.h);
            const float dx = ep.x - up.x, dy = ep.y - up.y;
            const float d2 = dx * dx + dy * dy;
            if (!best || d2 < best_d2) {
                best = &e;
                best_d2 = d2;
            }
        }
        if (!best) continue;
        const float range = std::max(u.range, 100.0f);
        const float dist = dmath::sqrt(best_d2);
        // in range: hold, combat fires. s78n t20 stage A [port] (FIGHTER_FLYING §4.11 A, arrived re-check only): an arrived
        // unit that cannot hit its nearest enemy from where it stands approaches again (no land stop here).
        if (dist <= 0.8f * range && !standin_recheck(u, best->id)) continue;
        if (movement_locked(u.id)) continue;  // OI follow-up t03: a locked unit keeps its order (§28.9)
        if (!(dist > 0.0f)) continue;  // s78n t20 review: on the enemy's position there is no approach line
        const Vec3 ep = store.position(best->h);
        if (!space_) {  // s78n t25.3 [port] (MOVEMENT §4.3.2 item 2): the auto-attack block push 0x41fd30, one unit
            const objects::ObjectId one[] = {u.id};
            (void)land_attack_order(mode, one, best->id, ep, 0, false, rng);
            ++stats_.orders;
            continue;
        }
        // s78n t20 interim (space): the approach aims with the unit's own range (no 100 floor)
        const float k = (dist - 0.7f * (u.range > 0.0f ? u.range : range)) / dist;
        mv::MoveOrder o;
        o.units = {u.id};
        o.destination = {up.x + (ep.x - up.x) * k, up.y + (ep.y - up.y) * k};
        const tactical::Aabb& b = mode.bounds();
        o.destination.x = std::clamp(o.destination.x, b.min.x, b.max.x);
        o.destination.y = std::clamp(o.destination.y, b.min.y, b.max.y);
        o.skip_destination_check = true;  // s78n t20 fix (MOVEMENT 4.3.1 item 2): an attack approach, as attack_approach
        movement_->move(o, rng, frame_);
        fighter_order(o.units, 0, Vec3{o.destination.x, o.destination.y, 0.0f});  // S7 t22
        ++stats_.orders;
    }
}

// ---- OI follow-up t03: the movement lock (A1_READINGS §28.9) -------------------------------------------------------

void TacticalBattle::set_script_movement_lock(const void* key, std::function<bool(objects::ObjectId)> fn) {
    std::erase_if(script_locks_, [key](const auto& e) { return e.first == key; });
    if (fn) script_locks_.emplace_back(key, std::move(fn));
}

bool TacticalBattle::script_movement_locked(objects::ObjectId id) const {
    for (const auto& [key, f] : script_locks_)
        if (f(id)) return true;
    return false;
}

bool TacticalBattle::movement_locked(objects::ObjectId id) const {
    if (abilities_ && abilities_->immobilize_count(id) > 0) return true;
    // s78n t06 (HERO_CLASH §3.2): a hero in a clash has its movement orders locked (0x3a8e50(h, 1) with +0x3b4).
    if (hero_clash_locked(id)) return true;
    if (!landings_.empty() && landing_locked(id)) return true;  // a1i t04: the unload exit move (§35.2)
    return script_movement_locked(id);
}

bool TacticalBattle::hero_clash_locked(objects::ObjectId id) const {
    // s78n t06 review: the clash's share of the lock - +0x3b4 set and not released by a Lua TEAM Unlock (A1_READINGS §28.9
    // item 4, kUnitHeroClashUnlocked).
    if (!combat_ || !combat_->has_unit(id)) return false;
    const uint32_t f = combat_->flags(id);
    return (f & tc::kUnitHeroClash) && !(f & tc::kUnitHeroClashUnlocked);
}

bool TacticalBattle::release_hero_clash_lock(objects::ObjectId id) {
    if (!hero_clash_locked(id)) return false;
    combat_->set_flag(id, tc::kUnitHeroClashUnlocked, true);
    return true;
}

void TacticalBattle::drop_locked(std::vector<objects::ObjectId>& ids) const {
    ids.erase(std::remove_if(ids.begin(), ids.end(), [this](objects::ObjectId id) { return movement_locked(id); }), ids.end());
}

// ---- integration fixes: human order API (rules of BattleAi::execute_order) -----------------------------------------

bool TacticalBattle::standin_owner(int32_t player) const {
    if (req_.info.attacker < 0 && req_.info.defender < 0) return player >= 0;  // a battle built without sides: everyone
    return player >= 0 && (player == req_.info.attacker || player == req_.info.defender);
}

bool TacticalBattle::commanded_by_player(int32_t player) const {
    return cfg_.skip_human_units && player >= 0 && static_cast<size_t>(player) < galaxy_->players().size() &&
           galaxy_->players()[static_cast<size_t>(player)].human;
}

int32_t TacticalBattle::first_hardpoint_of_type(objects::ObjectId target, int32_t type) const {
    if (!combat_ || type < 0 || !combat_->has_unit(target)) return -1;
    for (uint32_t k = 0; k < combat_->hardpoint_count(target); ++k)
        if (static_cast<int32_t>(combat_->hardpoint_kind(target, k)) == type && combat_->hardpoint_targetable(target, k))
            return static_cast<int32_t>(k);
    return -1;
}

void TacticalBattle::exec_move(TacticalMode& mode, std::span<const objects::ObjectId> ids, float x, float y, SimRng& rng,
                               float facing_deg, bool attack_move, uint8_t flags, bool switch_locomotor,
                               objects::ObjectId goal_object, bool no_destination_check, bool cohesion_tight) {
    // s78n t26.1 (MOVEMENT §3.2.1 item 2, the group move 0x2a7b80): a deployed manual deployer is skipped entirely - its
    // orders (garrison enter, attack, team target) are kept and it is not moved
    std::vector<objects::ObjectId> admitted;
    if (!space_ && movement_) {
        for (const objects::ObjectId id : ids)
            if (!movement_->move_skips(id)) admitted.push_back(id);
        if (admitted.size() != ids.size()) ids = admitted;
        if (ids.empty()) return;
    }
    s4_cancel(ids);  // S4: a move replaces a pending garrison enter
    team_attack_drop(ids);  // q6: an ordered move clears the team target (FIGHTER_FLYING §4.10 B writer 3)
    const tactical::Aabb& bb = mode.bounds();
    mv::MoveOrder o;
    o.units.assign(ids.begin(), ids.end());
    // P1_DOUBLE_CLICK_MOVE.md §3.2: the event flags, unchanged; s78f t10: plus the attack-move bit, as the queued path and
    // the host's formation record (fighter_order below) carry it, so the fighter leash reads the same block.
    o.flags = static_cast<uint8_t>(flags | (attack_move ? mv::kMoveFlagAttackMove : 0));
    o.switch_locomotor = switch_locomotor;  // S7 t10: MoveToPosition's 1-bit (MOVEMENT §6.2)
    o.destination = {std::clamp(x, bb.min.x, bb.max.x), std::clamp(y, bb.min.y, bb.max.y)};
    if (std::isfinite(facing_deg)) o.facing_deg = facing_deg;
    o.goal_object = goal_object == objects::kNoObjectId ? -1 : goal_object;  // S7 t37 (movement: -1 = none)
    // s78n t20 (MOVEMENT §4.3.1 item 2): an object goal goes to the attack-arc slot test, not to the destination check
    o.skip_destination_check = no_destination_check || goal_object != objects::kNoObjectId;
    o.cohesion_tight = cohesion_tight && space_;  // [port] fleet cohesion (space only)
    movement_->move(o, rng, frame_);
    for (const objects::ObjectId id : o.units) {
        if (!attack_move) combat_->clear_order(id);
        // S8 t17 (FIGHTER_FLYING §4.10 B writer 3): an ordered move (0x39f800) clears the squadron's team target - every
        // member's Set_Target(null) and its hard points'.
        if (const Unit* mu = attack_move || !space_ ? nullptr : unit_by_id(id);
            mu && mu->container < containers_.size() && movement_->fighter_squad(containers_[mu->container].id))
            combat_->clear_targets(id);
        combat_->set_attack_goal(id, tc::kNoObject);  // refixf t06: a position block has no attack goal (§2.8)
    }
    // S7 t22: the squadrons' formation (block flags: 0x01 attack-move, 0x02 guard-move; P1_DOUBLE_CLICK §3.3)
    fighter_order(o.units, static_cast<uint8_t>((attack_move ? mv::kMoveFlagAttackMove : 0) | flags), Vec3{o.destination.x, o.destination.y, 0.0f});
}

// Attack rule of BattleAi::execute_order: order_attack, then (space) a move into 70 % of max(range, 100) beyond 80 %, or
// (land, s78n t25.3) one attack-block order per resolved target (land_attack_order).
uint32_t TacticalBattle::exec_attack(TacticalMode& mode, std::span<const objects::ObjectId> ids, objects::ObjectId target, int32_t hp,
                                     SimRng& rng) {
    const objects::ObjectStore& st = mode.primary();
    const Unit* t = unit_by_id(target);
    const Container* ct = t ? nullptr : team_target(target);  // q6: a TEAM container (TeamAttack, tactical_battle.hpp)
    if (!t && !ct) return 0;
    std::vector<TeamPick> picks;
    uint32_t n = 0;
    // s78n t25.3 (MOVEMENT §4.3.2 item 2): on land the attackers of each resolved target (a TEAM pick per attacker; groups
    // in order of first appearance) form one movement order with the attack block
    std::vector<std::pair<objects::ObjectId, std::vector<objects::ObjectId>>> groups;
    for (const objects::ObjectId id : ids) {
        const Unit* u = unit_by_id(id);
        if (!u || u->id == target || !combat_->has_unit(u->id)) continue;
        const Unit* tu = t;
        if (ct) {
            const objects::ObjectId m = team_pick(mode, *ct, st.position(u->h), combat_->share_group(u->id), picks);
            tu = m != objects::kNoObjectId ? unit_by_id(m) : nullptr;
            if (!tu) continue;
        }
        const Vec3 tp = st.position(tu->h);
        combat_->order_attack(u->id, tu->id, ct ? -1 : hp);
        if (ct) team_attack_set(u->id, ct->id, tu->id, false);
        const objects::ObjectId block = ct && movement_->fighter_squad(ct->id) ? ct->id : tu->id;
        if (!movement_locked(u->id))  // S7 t22: an object block (flags 0); a locked unit keeps its formation (as Move_To)
            fighter_order(std::span<const objects::ObjectId>(&u->id, 1), 0, tp, block);
        ++n;
        if (space_) {
            attack_approach(mode, *u, tp, rng, block, tu->id);
            continue;
        }
        auto g = std::find_if(groups.begin(), groups.end(), [&](const auto& x) { return x.first == tu->id; });
        if (g == groups.end()) g = groups.insert(groups.end(), {tu->id, {}});
        g->second.push_back(u->id);
        attack_block_set(u->id, ct ? ct->id : tu->id, 0, tp);  // s78n t20 stage A
    }
    for (const auto& [tid, us] : groups)
        (void)land_attack_order(mode, us, tid, st.position(unit_by_id(tid)->h), 0, false, rng);
    return n;
}

void TacticalBattle::attack_approach(TacticalMode& mode, const Unit& u, Vec3 tp, SimRng& rng, objects::ObjectId goal,
                                     objects::ObjectId target, bool replan) {
    if (!u.movable || movement_locked(u.id)) return;  // a locked unit fires from where it stands (§28.9 [M])
    if (!space_ && target != objects::kNoObjectId && combat_) {  // s78n t25.3 (MOVEMENT §4.3.2): the attack block
        const objects::ObjectId one[] = {u.id};
        (void)land_attack_order(mode, one, target, tp, 0, replan, rng);
        return;
    }
    const tactical::Aabb& bb = mode.bounds();
    const Vec3 up = mode.primary().position(u.h);
    const float dx = tp.x - up.x, dy = tp.y - up.y, d = dmath::sqrt(dx * dx + dy * dy);
    const float range = std::max(u.range, 100.0f);
    // s78f review (FIGHTER_FLYING §4.11 B 1 / B 3 "new group move"): Move_To(object) gives a fighter a fresh formation
    // whose only block is the object block (an earlier stack, e.g. a carrier guard, is gone): the move's block names the
    // target (`goal`); in range, nobody moves and the formation is still replaced (fighter_new_order).
    const bool fighter = goal != objects::kNoObjectId && space_ && movement_->has_unit(u.id) &&
                         movement_->view(u.id).locomotor == mv::LocomotorKind::Fighter;
    if (d > 0.8f * range) {
        const float k = (d - 0.7f * range) / d;
        mv::MoveOrder o;
        o.units = {u.id};
        o.destination = {std::clamp(up.x + dx * k, bb.min.x, bb.max.x), std::clamp(up.y + dy * k, bb.min.y, bb.max.y)};
        if (fighter) o.goal_object = goal;
        // s78n t20 (MOVEMENT §4.3.1 item 2): the approach stands for an object-goal block - no destination check (space;
        // s78n t25.3: land attack approaches are the attack block above)
        o.skip_destination_check = true;
        movement_->move(o, rng, frame_);
    } else if (fighter) {
        mv::MoveOrder o;
        o.units = {u.id};
        o.destination = {std::clamp(tp.x, bb.min.x, bb.max.x), std::clamp(tp.y, bb.min.y, bb.max.y)};
        o.goal_object = goal;
        movement_->fighter_new_order(o, frame_);
    }
}

// ---- q6: attack on a TEAM container (tactical_battle.hpp TeamAttack) -------------------------------------------------

const TacticalBattle::Container* TacticalBattle::team_target(objects::ObjectId id) const {
    if (id == objects::kNoObjectId) return nullptr;
    for (const Container& c : containers_)
        if (c.id == id) return c.team && !c.removed && c.alive > 0 ? &c : nullptr;
    return nullptr;
}

objects::ObjectId TacticalBattle::team_pick(const TacticalMode& mode, const Container& c, Vec3 from, objects::ObjectId group,
                                            std::vector<TeamPick>& picks) const {
    if (group != objects::kNoObjectId)
        for (const TeamPick& p : picks)
            if (p.group == group && p.container == c.id) return p.member;
    const objects::ObjectStore& st = mode.primary();
    const auto ci = static_cast<uint32_t>(&c - containers_.data());
    objects::ObjectId best = objects::kNoObjectId;
    float bd = 0.0f;
    for (const Unit& m : units_) {  // ascending id: the first strict minimum keeps the lower id on a tie [port]
        if (m.container != ci || !m.alive || m.retreated || !combat_->has_unit(m.id) || !combat_->alive(m.id)) continue;
        if (combat_->flags(m.id) & (tc::kUnitGarrisoned | tc::kUnitSkipTargeting)) continue;  // 0x5680b0 test 2 / untargetable
        const Vec3 p = st.position(m.h);
        const float dx = p.x - from.x, dy = p.y - from.y, d = dx * dx + dy * dy;
        if (best == objects::kNoObjectId || d < bd) {
            best = m.id;
            bd = d;
        }
    }
    if (group != objects::kNoObjectId && best != objects::kNoObjectId) picks.push_back({group, c.id, best});
    return best;
}

void TacticalBattle::team_attack_set(objects::ObjectId attacker, objects::ObjectId container, objects::ObjectId member, bool goal) {
    auto it = std::lower_bound(team_attacks_.begin(), team_attacks_.end(), attacker,
                               [](const TeamAttack& r, objects::ObjectId v) { return r.attacker < v; });
    if (it == team_attacks_.end() || it->attacker != attacker) it = team_attacks_.insert(it, TeamAttack{});
    *it = TeamAttack{attacker, container, member, goal};
}

void TacticalBattle::team_attack_drop(std::span<const objects::ObjectId> ids) {
    attack_block_drop(ids);  // s78n t20 stage A: the same writers end the land attack block
    if (team_attacks_.empty()) return;
    for (const objects::ObjectId id : ids) {
        const auto it = std::lower_bound(team_attacks_.begin(), team_attacks_.end(), id,
                                         [](const TeamAttack& r, objects::ObjectId v) { return r.attacker < v; });
        if (it != team_attacks_.end() && it->attacker == id) team_attacks_.erase(it);
    }
}

// Serial, after this tick's combat events (deaths, container removals) and before the order queues advance. Per record in
// attacker order: the order ends when the container is gone or no longer hostile, or when combat no longer holds a member of
// it (the order was dropped: fog, stealth, an owner change; or replaced). Otherwise the attacker's combat target becomes the
// current pick; a pick after the old member died also re-runs the order's approach move (and the squadron goal when the
// goal was the member) [port: the original's attacker follows the container object block; the port moves once per pick].
void TacticalBattle::team_attack_service(TacticalMode& mode, SimRng& rng) {
    if (team_attacks_.empty() || !combat_) return;
    const objects::ObjectStore& st = mode.primary();
    std::vector<TeamPick> picks;
    const auto live_unit = [this](const Unit* x) { return x && x->alive && !x->retreated && combat_->has_unit(x->id) && combat_->alive(x->id); };
    const auto keep = [&](TeamAttack& r) {
        const Unit* u = unit_by_id(r.attacker);
        if (!live_unit(u)) return false;
        const Container* ct = team_target(r.container);
        if (!ct) {
            // The container left with its last member: the order ends with it. A land team member runs no scan of its own
            // (s78f t03, TeamTargeting is the container's), so nothing else drops the dead member it still names.
            if (combat_->ordered_target(u->id) == r.member && !live_unit(unit_by_id(r.member))) {
                combat_->clear_order(u->id);
                if (r.goal) combat_->set_attack_goal(u->id, tc::kNoObject);
            }
            return false;
        }
        if (!combat_->is_enemy(st.owner(u->h), st.owner(ct->h))) return false;
        const auto ci = static_cast<uint32_t>(ct - containers_.data());
        const Unit* mu = unit_by_id(r.member);
        const bool member_live = live_unit(mu);
        const objects::ObjectId cur = combat_->ordered_target(u->id);
        bool lost = !member_live;
        if (cur != r.member) {
            const Unit* cu = cur != tc::kNoObject ? unit_by_id(cur) : nullptr;
            if (cu && cu->container == ci) {
                r.member = cur;  // a squadmate's order (combat share group) named another member
                lost = !live_unit(cu);
            } else if (cur != tc::kNoObject || member_live) {
                return false;  // dropped or replaced
            }
        }
        const objects::ObjectId m = team_pick(mode, *ct, st.position(u->h), combat_->share_group(u->id), picks);
        if (m == objects::kNoObjectId) {  // no member can be attacked (all garrisoned): the order ends, the target goes
            combat_->clear_order(u->id);
            if (r.goal) combat_->set_attack_goal(u->id, tc::kNoObject);
            return false;
        }
        if (m == cur) return true;
        combat_->order_attack(u->id, m, -1);
        if (r.goal) combat_->set_attack_goal(u->id, m, -1);
        r.member = m;
        if (lost) {
            const Vec3 tp = st.position(unit_by_id(m)->h);
            if (!movement_locked(u->id) && !movement_->fighter_squad(ct->id))
                fighter_order(std::span<const objects::ObjectId>(&u->id, 1), 0, tp, m);
            attack_approach(mode, *u, tp, rng, objects::kNoObjectId, m, true);  // s78n t25.3: land - a re-plan (slot kept)
        }
        return true;
    };
    size_t w = 0;
    for (size_t i = 0; i < team_attacks_.size(); ++i)
        if (keep(team_attacks_[i])) team_attacks_[w++] = team_attacks_[i];
    team_attacks_.resize(w);
}

// ---- s78n t20 stage A: land attack blocks (tactical_battle.hpp AttackBlock) -------------------------------------------

bool TacticalBattle::standin_recheck(const Unit& u, objects::ObjectId enemy) const {
    if (space_ || !combat_ || !movement_ || !combat_->has_unit(u.id)) return false;
    const objects::ObjectId mover = attack_mover(u.id);
    if (!movement_->has_unit(mover)) return false;
    const mv::UnitView v = movement_->view(mover);
    if (v.has_goal || v.pushed) return false;
    const tc::CanHit c = attack_can_hit(u.id, enemy);
    return c != tc::CanHit::Yes && c != tc::CanHit::NoTarget;
}

objects::ObjectId TacticalBattle::attack_mover(objects::ObjectId unit) const {
    objects::ObjectId c = objects::kNoObjectId;
    mv::V2 slot;
    return movement_ && movement_->team_slot(unit, c, slot) && c >= 0 ? c : unit;
}

tc::CanHit TacticalBattle::attack_can_hit(objects::ObjectId unit, objects::ObjectId target) const {
    const objects::ObjectId mover = attack_mover(unit);
    if (mover == unit || !movement_ || !movement_->has_unit(mover)) return combat_->can_hit(unit, target);
    tactical::Vec3 from = combat_->can_hit_origin(unit);  // its z (the lob solver's start height)
    const mv::V2 p = movement_->view(mover).pos;
    from.x = p.x;
    from.y = p.y;
    return combat_->can_hit_from(unit, from, target);
}

void TacticalBattle::attack_block_set(objects::ObjectId unit, objects::ObjectId target, uint8_t flags, Vec3 goal) {
    if (space_ || !movement_ || target == objects::kNoObjectId) return;
    const Unit* u = unit_by_id(unit);
    if (!u || !u->movable) return;
    const objects::ObjectId mover = attack_mover(unit);
    if (!movement_->has_unit(mover)) return;
    auto it = std::lower_bound(attack_blocks_.begin(), attack_blocks_.end(), mover,
                               [](const AttackBlock& r, objects::ObjectId v) { return r.mover < v; });
    if (it == attack_blocks_.end() || it->mover != mover) it = attack_blocks_.insert(it, AttackBlock{});
    // [port] the first check 30 frames after the order (the formation timer F +0x1d8 is set at its first state-1 step)
    *it = AttackBlock{mover, target, flags, frame_ + 30u, goal.x, goal.y};
}

void TacticalBattle::attack_block_drop(std::span<const objects::ObjectId> ids) {
    if (attack_blocks_.empty()) return;
    for (const objects::ObjectId id : ids) {
        const objects::ObjectId mover = attack_mover(id);
        const auto it = std::lower_bound(attack_blocks_.begin(), attack_blocks_.end(), mover,
                                         [](const AttackBlock& r, objects::ObjectId v) { return r.mover < v; });
        if (it != attack_blocks_.end() && it->mover == mover) attack_blocks_.erase(it);
    }
}

objects::ObjectId TacticalBattle::attack_lead(objects::ObjectId mover) {
    if (!movement_ || !movement_->is_land_team_container(mover)) return mover;
    for (const auto& [mid, slot] : movement_->land_team_slots(mover)) {
        (void)slot;
        const Unit* m = unit_by_id(mid);
        if (m && m->alive && !m->retreated && combat_->has_unit(mid)) return mid;
    }
    return objects::kNoObjectId;
}

bool TacticalBattle::pending_ability_ranges(objects::ObjectId unit, float& min_range, float& max_range, objects::ObjectId& on) const {
    // the unit's pending targeted ability, else its squad container's (AbilitySystem keeps a squad's on the container)
    on = objects::kNoObjectId;
    if (!abilities_) return false;
    for (const objects::ObjectId id : {unit, attack_mover(unit)})
        if (abilities_->pending_ranges(id, min_range, max_range)) {
            on = abilities_->pending_target(id);
            return true;
        }
    return false;
}

Vec3 TacticalBattle::attack_stage_goal(const TacticalMode& mode, std::span<const objects::ObjectId> ids, objects::ObjectId target,
                                       Vec3 tp, size_t movers) {
    // bkt1 t01 (b0068; 0x40e260, BKT1_ATTACK_RE §1.2 [H], TACTICAL_COMBAT §2.8; the AI's Attack_Move staging and stage
    // A's per-plan staging merged). M = the first unit with the largest minimum range: its type's
    // Targeting_Min_Attack_Distance, or its pending targeted ability's Activation_Min_Range when > 0. Only when M is inside
    // that min range of T is G moved:
    //   - crush case: a one-member formation whose M has SQUASH and T Is_Squashable (or M Is_Supercrusher and T
    //     Is_Squashable_By_Supercrusher) -> G = T's position (no hostility test);
    //   - else d = min + 0.2 x (Get_Base_Attack_Range(M) - min) and G = the free-position search 0x440f90 (mode 3) around
    //     M: rings centred on M from R0 = max(0, min - |M - G|) (3-D), step 1.0 growing by 0.8 up to min(1.5 r, 10)
    //     (r = M's hard footprint radius; maxStep 5 when r = 0), 20 rings, n = trunc(2 pi R / r) + 1 samples (at least
    //     4 after ring 0), counter-clockwise from the T -> M direction; the first sample >= d from G (3-D, the sample at
    //     M's z), inside the map extents and in M's zone for M's own movement class wins. None: G stays at T's point.
    // (the gate: slot 46 with the ability's Activation_Min_Range when M's pending ability is on T, else the type's)
    objects::ObjectId m = objects::kNoObjectId;
    float mn = 0.0f, gate = 0.0f;
    for (const objects::ObjectId id : ids) {
        if (!combat_->has_unit(id)) continue;
        const float tmin = combat_->min_attack_distance(id);
        float v = tmin, g = tmin;
        float amin = 0.0f, amax = 0.0f;
        objects::ObjectId on = objects::kNoObjectId;
        if (pending_ability_ranges(id, amin, amax, on) && amin > 0.0f) {
            v = std::max(v, amin);
            if (on == target) g = amin;
        }
        if (v > mn) {
            mn = v;
            gate = g;
            m = id;
        }
    }
    if (m == objects::kNoObjectId || !combat_->inside_min_range_of(m, target, gate)) return tp;
    const Unit* mu = unit_by_id(m);
    if (!mu) return tp;
    const objects::GameObjectTypes& types = catalog_->types();
    const Unit* tu = unit_by_id(target);
    if (movers == 1 && attack_mover(m) == m && tu && mu->type < types.size() && tu->type < types.size() &&
        types.has_behavior(mu->type, objects::BehaviorSet::Union, 0x26)) {  // SQUASH
        const bool squash = types.get_bool(tu->type, "Is_Squashable") ||
                            (types.get_bool(mu->type, "Is_Supercrusher") && types.get_bool(tu->type, "Is_Squashable_By_Supercrusher"));
        if (squash) return tp;
    }
    const float d = mn + 0.2f * (combat_->base_attack_range_of(m) - mn);
    const Vec3 mp = mode.primary().position(mu->h);
    const objects::ObjectId mover = attack_mover(m);
    const uint32_t mask = movement_ && movement_->has_unit(mover) ? movement_->mask_of(mover) : 0u;
    float hx = 0.0f, hy = 0.0f;
    unit_half_extents(*mu, hx, hy);
    const float r = dmath::sqrt(hx * hx + hy * hy);
    const float max_step = std::min(r > 0.0f ? 1.5f * r : 5.0f, 10.0f);
    const tactical::Aabb box = map_extents(mode);
    const float yaw = dmath::atan2(mp.y - tp.y, mp.x - tp.x);
    const auto dist3 = [](Vec3 a, Vec3 b) {
        const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return dmath::sqrt((dx * dx + dy * dy) + dz * dz);
    };
    float R = std::max(0.0f, mn - dist3(mp, tp));
    float step = 1.0f;
    for (int ring = 0; ring < 20; ++ring) {
        int32_t n = r > 0.0f ? static_cast<int32_t>(std::min(R * 2.0f * dmath::kPiF / r, 4096.0f)) + 1 : 1;
        if (ring > 0) n = std::max<int32_t>(4, n);
        for (int32_t k = 0; k < n; ++k) {
            const float th = 2.0f * dmath::kPiF * static_cast<float>(k) / static_cast<float>(n) + yaw;
            const Vec3 q{mp.x + R * dmath::cos(th), mp.y + R * dmath::sin(th), mp.z};
            if (d > dist3(q, tp)) continue;
            if (q.x < box.min.x || q.x > box.max.x || q.y < box.min.y || q.y > box.max.y) continue;
            if (!movement_ || !mv::zone_connected(movement_->regions(), mask, {mp.x, mp.y}, {q.x, q.y})) continue;
            return q;  // land_attack_order reads its x / y; the fire trim's end height is C's (bkt1 t06)
        }
        R += step;
        step = std::min(step + 0.8f, max_step);
    }
    return tp;
}

TacticalBattle::AttackPlan TacticalBattle::land_attack_order(TacticalMode& mode, std::span<const objects::ObjectId> ids,
                                                            objects::ObjectId target, Vec3 tp, uint8_t flags, bool replan,
                                                            SimRng& rng, bool guard, bool repath) {
    if (space_ || !movement_ || !combat_) return AttackPlan::None;
    const objects::ObjectStore& st = mode.primary();
    const Unit* tu = unit_by_id(target);
    mv::MoveOrder o;
    std::vector<objects::ObjectId> movers;
    for (const objects::ObjectId id : ids) {
        const Unit* u = unit_by_id(id);
        if (!u || !u->alive || u->retreated || !u->movable || movement_locked(id)) continue;
        const objects::ObjectId mover = attack_mover(id);
        if (!movement_->has_unit(mover)) continue;
        if (std::find(o.units.begin(), o.units.end(), id) == o.units.end()) o.units.push_back(id);
        if (std::find(movers.begin(), movers.end(), mover) == movers.end()) movers.push_back(mover);
    }
    if (o.units.empty()) return AttackPlan::None;
    // The target's footprint half extents (0x396850): its movement unit's, else the type's hard extents.
    float thx = 0.0f, thy = 0.0f;
    if (movement_->has_unit(target)) {
        const mv::V2 h = movement_->collision_half(target);
        thx = h.x;
        thy = h.y;
    } else if (tu) {
        unit_half_extents(*tu, thx, thy);
    }
    // s78n t25.4 [port]: a guard block is not staged (G = C; 0x40e260's min-range rule is for an attack on T)
    const Vec3 g = guard ? tp : attack_stage_goal(mode, o.units, target, tp, movers.size());
    const tactical::Aabb& bb = mode.bounds();
    o.destination = {std::clamp(g.x, bb.min.x, bb.max.x), std::clamp(g.y, bb.min.y, bb.max.y)};
    o.flags = static_cast<uint8_t>(flags & ~kAttackBlockNoLock);  // bkt1 t05: the host-only bit stays here
    o.goal_object = -1;  // land: the block's object is attack.target
    o.attack.target = target;
    o.attack.c = {tp.x, tp.y};
    o.attack.cz = tp.z;
    o.attack.g = o.destination;
    o.attack.t_hx = thx;
    o.attack.t_hy = thy;
    o.attack.replan = replan;
    o.attack.guard = guard;  // s78n t25.4
    // 0x410e90's gate: the target point not fogged for the first unit's owner, and not a stealthed target the first unit
    // cannot target (Target_Stealth_Units)
    const Unit* first = unit_by_id(o.units.front());
    bool trim = !fog_point_fogged(st.owner(first->h), tp.x, tp.y, false);
    if (trim && abilities_ && abilities_->is_stealthed(target, frame_)) {
        const objects::GameObjectTypes& types = catalog_->types();
        trim = first->type < types.size() && types.get_bool(first->type, "Target_Stealth_Units");
    }
    o.attack.trim = trim;
    // s78n t26 review round 3 (FIGHTER_FLYING §4.11 E [H], 0x40edd0): c is false for a target hidden for the first unit's
    // owner or stealthed against its type - no item-6 stop; G 5: a re-path keeps the standing in-range members
    bool hidden = false, stealthed = false;
    attack_target_unseen(st, *first, target, hidden, stealthed);
    o.attack.unseen = hidden || stealthed;
    o.attack.repath = repath && replan;
    for (const objects::ObjectId mover : movers) {
        const objects::ObjectId lead = attack_lead(mover);
        mv::LandAttackUnit e;
        e.id = mover;
        if (lead != objects::kNoObjectId && combat_->has_unit(lead)) {
            e.range = combat_->attack_range(lead);  // 0x3749d0 (Get_Base_Attack_Range)
            // 0x4f6e40 (§4.3.2 item 1; bkt1 t01, BKT1_ATTACK_RE §1.1 [H]): a pending targeted ability with an
            // Activation_Max_Range > 0 gives min(range, that range) - no extent, no guard cap; else + min(hx, hy) of the
            // target's extents ([port] land blocks name no hard point: the hard-point radius term is not reached)
            float amin = 0.0f, amax = 0.0f;
            objects::ObjectId on = objects::kNoObjectId;
            if (pending_ability_ranges(lead, amin, amax, on) && amax > 0.0f) {
                e.engage = std::min(e.range, amax);
            } else {
                e.engage = e.range + std::min(thx, thy);
                // s78n t25.4 (item 1): a guard block's engagement range is capped at Land_Guard_Range (GC +0x11ec4)
                if (guard) e.engage = std::min(e.engage, movement_->constants().land_guard_range);
            }
            const tc::CanHit c = attack_can_hit(lead, target);
            e.in_range = c == tc::CanHit::Yes || c == tc::CanHit::LineBlocked;  // slots 44 / 45: range only
            e.melee = combat_->melee_attacker(lead);
            // s78o (b0071): TARGETING - a weapon range, or a grenade arc (can_hit answers more than NoTarget)
            e.targeting = e.range > 0.0f || c != tc::CanHit::NoTarget;
        } else {
            e.targeting = false;  // not a combat unit: no TARGETING
        }
        o.attack.units.push_back(e);
    }
    const uint32_t stops = movement_->stats().attack_stops;
    movement_->move(o, rng, frame_);
    for (const objects::ObjectId mover : movers)
        if (movement_->view(mover).has_goal) return AttackPlan::Moved;
    return movement_->stats().attack_stops != stops ? AttackPlan::Stopped : AttackPlan::None;
}

void TacticalBattle::attack_target_unseen(const objects::ObjectStore& st, const Unit& unit, objects::ObjectId target,
                                          bool& hidden, bool& stealthed) {
    // the block's object: a squad member's container (FIGHTER_FLYING §4.11 D 1 / D 7: blocks name the team container; its
    // fog test uses the team box, FOG_OF_WAR §5.2), else the unit
    objects::ObjectId obj = target;
    const Unit* tu = unit_by_id(target);
    if (tu && tu->container < containers_.size() && containers_[tu->container].id != objects::kNoObjectId)
        obj = containers_[tu->container].id;
    hidden = fog_object_hidden(st.owner(unit.h), obj, true);
    stealthed = false;
    if (abilities_ && (abilities_->is_stealthed(target, frame_) || (obj != target && abilities_->is_stealthed(obj, frame_)))) {
        const objects::GameObjectTypes& types = catalog_->types();
        stealthed = !(unit.type < types.size() && types.get_bool(unit.type, "Target_Stealth_Units"));
    }
}

void TacticalBattle::attack_block_service(TacticalMode& mode, SimRng& rng) {
    if (attack_blocks_.empty() || space_ || !combat_ || !movement_) return;
    const objects::ObjectStore& st = mode.primary();
    const auto live = [&](const Unit* x) { return x && x->alive && !x->retreated && st.alive(x->h) && combat_->has_unit(x->id); };
    struct Checked {  // s78n t25.3: this pass's checked blocks (ascending mover)
        objects::ObjectId mover, lead, tid;
        bool due, plan, halted;
        bool locked = false;                   // bkt1 t03: the block's lead is movement-locked (not a formation lead)
        bool repath = false, dropped = false;  // s78n t26 review round 3: a moving formation's re-path (G 5); C 3's drop
        bool at_p = false;                     // C 3: the plan goes toward p (an unseen target after arrival)
        Vec3 p{};
    };
    std::vector<Checked> checked;
    const auto keep = [&](AttackBlock& r) {
        if (!movement_->has_unit(r.mover)) return false;
        const Container* ct = team_target(r.target);
        if (!ct && !live(unit_by_id(r.target))) return false;  // the target died (signal 0x10)
        // The unit the target is attacked through: the target itself, or (a TEAM container) the member the lead's order names.
        const auto names = [&](const Unit* x) -> objects::ObjectId {
            if (!live(x)) return objects::kNoObjectId;
            for (const tc::ObjectId o : {combat_->ordered_target(x->id), combat_->attack_goal(x->id)}) {
                if (o == tc::kNoObject) continue;
                if (!ct) {
                    if (o == r.target) return o;
                    continue;
                }
                const Unit* ou = unit_by_id(o);
                if (live(ou) && ou->container < containers_.size() && &containers_[ou->container] == ct) return o;
            }
            return objects::kNoObjectId;
        };
        const Unit* lead = nullptr;
        objects::ObjectId tid = objects::kNoObjectId;
        if (movement_->is_land_team_container(r.mover)) {
            for (const auto& [mid, slot] : movement_->land_team_slots(r.mover)) {
                (void)slot;
                const Unit* m = unit_by_id(mid);
                tid = names(m);
                if (tid != objects::kNoObjectId) {
                    lead = m;
                    break;
                }
            }
        } else {
            lead = unit_by_id(r.mover);
            tid = names(lead);
        }
        // bkt1 t05 (b0075, BKT1_ATTACK_RE §3 [H]): a unit Attack_Move on a non-hostile object - no unit's order names it
        // (nothing locks it), the block stands for the movement's object block alone: lead = the mover (a squad through
        // its first live member), C = the object
        if ((r.flags & kAttackBlockNoLock) != 0 && !ct) {
            const objects::ObjectId l = attack_lead(r.mover);
            lead = l == objects::kNoObjectId ? nullptr : unit_by_id(l);
            tid = r.target;
        }
        if (!lead || tid == objects::kNoObjectId) return false;  // the order no longer names the target
        // s78n t26 review round 2: the checks are the attack formation's (below); here the timer gate only. bkt1 t03: a
        // locked lead no longer suspends its formation here - the formation's lead is picked below among unlocked members.
        checked.push_back({r.mover, lead->id, tid, frame_ >= r.next, false, false, movement_locked(lead->id), false, false, false, Vec3{}});
        return true;
    };
    size_t w = 0;
    for (size_t i = 0; i < attack_blocks_.size(); ++i)
        if (keep(attack_blocks_[i])) attack_blocks_[w++] = attack_blocks_[i];
    attack_blocks_.resize(w);
    const auto bi = [&](objects::ObjectId mover) {
        return std::lower_bound(attack_blocks_.begin(), attack_blocks_.end(), mover,
                                [](const AttackBlock& b, objects::ObjectId v) { return b.mover < v; });
    };
    // s78n t26 review round 2 (FIGHTER_FLYING §4.11 A / E [H]): the checks are per attack formation F - the blocks on the
    // same target whose movers share UnitView::atk_form (0: the block alone), in ascending mover order (lead: bkt1 t03
    // below). F is checked when its lead's block is due (0x410a60 / 0x4192b0: frame + int(FPS +
    // 0.5)); every member's timer then resets. c = 0x40edd0(F): EVERY member can hit from where it stands (each block's
    // lead: a squad through its first member that names the target, D 5); moving = some member has a goal; stopping =
    // the lead's Walk state 0x14 (End Move) or 0x15 (Pre Stopped: [port] never observed, the port's walk passes it within
    // the frame), or the block has flag 0x01 and the lead has no path (0x39ade0: no record on its path stack).
    //   - arrived (no member has a goal), no member on a collision-owned top record, and not c -> plan again (0x410a60);
    //   - moving, c, not stopping and D <= R -> the land stop: every member with a goal halts at once;
    //   - moving and (stopping and not c, or D > R) -> plan again.
    // bkt1 t03 (b0073, FIGHTER_FLYING §4.11 G 3 [H]): F's lead = the member nearest the 3-D centroid L of F's movers
    // (summed in member order; strict <, first wins) among the movers that stand off their goal point (the unit their
    // lead's order names, tid: D 5's tested member of a TEAM container target), [port] skipping movement-locked ones (hero clash, immobilize, landing, Lua lock:
    // they keep their order, A1_READINGS §28.9); fallback the first unlocked member. Every member locked: F is not
    // checked, the timers wait. F is checked when its lead's block is due.
    std::vector<uint8_t> grouped(checked.size(), 0);
    const auto pos3 = [&](objects::ObjectId id) {
        const mv::V2 p = movement_->view(id).pos;
        const objects::ObjectHandle h = st.find(id);
        return Vec3{p.x, p.y, st.alive(h) ? st.position(h).z : 0.0f};
    };
    for (size_t i0 = 0; i0 < checked.size(); ++i0) {
        if (grouped[i0]) continue;
        grouped[i0] = 1;
        const uint32_t form = movement_->view(checked[i0].mover).atk_form;
        std::vector<size_t> g{i0};
        if (form != 0)
            for (size_t j = i0 + 1; j < checked.size(); ++j)
                if (!grouped[j] && bi(checked[j].mover)->target == bi(checked[i0].mover)->target &&
                    movement_->view(checked[j].mover).atk_form == form) {
                    grouped[j] = 1;
                    g.push_back(j);
                }
        size_t i = checked.size();
        {
            Vec3 L{};
            for (const size_t j : g) {
                const Vec3 q = pos3(checked[j].mover);
                L.x += q.x;
                L.y += q.y;
                L.z += q.z;
            }
            const float inv = 1.0f / static_cast<float>(g.size());
            L = Vec3{L.x * inv, L.y * inv, L.z * inv};
            float best = 0.0f;
            size_t first_free = checked.size();
            for (const size_t j : g) {
                if (checked[j].locked) continue;
                if (first_free == checked.size()) first_free = j;
                const Vec3 q = pos3(checked[j].mover);
                const Vec3 tc = st.position(unit_by_id(checked[j].tid)->h);
                if (q.x == tc.x && q.y == tc.y && q.z == tc.z) continue;  // its goal point is where it stands
                const float dx = q.x - L.x, dy = q.y - L.y, dz = q.z - L.z;
                const float dd = (dx * dx + dy * dy) + dz * dz;
                if (i == checked.size() || dd < best) {
                    best = dd;
                    i = j;
                }
            }
            if (i == checked.size()) i = first_free;
        }
        if (i == checked.size()) {  // every member locked: the formation waits
            for (const size_t j : g)
                if (checked[j].due) bi(checked[j].mover)->next = frame_ + 30u;
            continue;
        }
        const auto ri = bi(checked[i].mover);
        if (!checked[i].due) continue;
        for (const size_t j : g) bi(checked[j].mover)->next = frame_ + 30u;
        bool can = true, moving = false, pushed = false;
        for (const size_t j : g) {
            const tc::CanHit c = attack_can_hit(checked[j].lead, checked[j].tid);
            can = can && (c == tc::CanHit::Yes || c == tc::CanHit::NoTarget);  // NoTarget: nothing tested (0x40edd0 true)
            const mv::UnitView vj = movement_->view(checked[j].mover);
            moving = moving || vj.has_goal;
            pushed = pushed || vj.pushed;
        }
        const Unit* lead = unit_by_id(checked[i].lead);
        const Vec3 tp = st.position(unit_by_id(checked[i].tid)->h);
        const float gx = tp.x - ri->gx, gy = tp.y - ri->gy, R = 0.5f * lead->range;
        const bool far = gx * gx + gy * gy > R * R;
        // s78n t26 review round 3 (FIGHTER_FLYING §4.11 E [H]): c (0x40edd0) is false when the object is hidden for
        // members[0]'s owner or stealthed and members[0]'s type lacks Target_Stealth_Units ([port] the target unit the
        // lead's order names, tid: a TEAM container through that member)
        bool hidden = false, stealthed = false;
        attack_target_unseen(st, *lead, checked[i].tid, hidden, stealthed);
        if (hidden || stealthed) can = false;
        if (!moving) {
            if (pushed) continue;
            if (hidden || stealthed) {
                // §4.11 C 3 [H] (states 3 / 4, n = 1, the object hidden or stealthed for the lead): P = the object's goal
                // point 0x583200 - hidden: its position ([M] the block's stored point is the 1e18 default); stealthed: the
                // lead's own position (the member nearest the centroid L, G 3; [port] stealth is tested first). d = 2-D
                // |L - P|: d >= 40 -> plan again toward P; d < 40 -> the block becomes a position block at L (Set_Move kind 1,
                // clear) and every member's attack order is cleared (0x38d730(member, null) [M]). [port] checked on the
                // block's 30-frame timer.
                mv::V2 L{0.0f, 0.0f};
                for (const size_t j : g) L = L + movement_->view(checked[j].mover).pos;
                L = L * (1.0f / static_cast<float>(g.size()));
                Vec3 P = tp;
                if (stealthed) {
                    float best = 0.0f;
                    bool have = false;
                    for (const size_t j : g) {
                        const mv::V2 q = movement_->view(checked[j].mover).pos;
                        const float dd = (q.x - L.x) * (q.x - L.x) + (q.y - L.y) * (q.y - L.y);
                        if (!have || dd < best) {
                            best = dd;
                            have = true;
                            P = {q.x, q.y, tp.z};
                        }
                    }
                }
                const float dx = L.x - P.x, dy = L.y - P.y;
                if (dx * dx + dy * dy >= 40.0f * 40.0f) {
                    checked[i].plan = true;
                    checked[i].at_p = true;
                    checked[i].p = P;
                } else {
                    for (const size_t j : g) checked[j].dropped = true;
                }
                continue;
            }
            checked[i].plan = !can;  // arrived and some member cannot hit (FIGHTER_FLYING §4.11 A)
            continue;
        }
        // §4.11 C 1 / C 4 [H]: a hidden object - 0x4165f0 is false (no re-path) and c is false (no land stop); step 5 pops
        // nothing (n = 1): the formation walks on along its current path
        if (hidden) continue;
        const mv::UnitView lv = movement_->view(checked[i].mover);
        const bool stopping =
            lv.walk_state == mv::LocoState::WalkEndMove || ((ri->flags & mv::kMoveFlagAttackMove) != 0 && !lv.has_goal);
        if (can && !stopping && !far) {  // §4.11 E: the land stop of F
            std::vector<objects::ObjectId> ids;
            for (const size_t j : g)
                if (movement_->view(checked[j].mover).has_goal) ids.push_back(checked[j].mover);
            movement_->halt(ids, frame_);  // s78n t20 review: no speed zeroing, the walk brakes through End Move
            ++stats_.attack_land_stops;
            for (const size_t j : g) checked[j].halted = true;
        } else if ((stopping && !can) || far) {
            checked[i].plan = true;
            checked[i].repath = true;  // 0x4192b0 step 4's re-path: G 5
        }
    }
    // s78n t25.3 (MOVEMENT §4.3.2 item 8, §4.6.2): the re-plans, in ascending mover order. A re-plan is the formation's:
    // it takes the lead of every block on the same target whose mover is in the same attack formation (atk_form != 0)
    // and was not land-stopped this pass; their timers reset together. No new formation (no ctor draw), no slot test.
    std::vector<uint8_t> done(checked.size(), 0);
    for (size_t i = 0; i < checked.size(); ++i) {
        if (!checked[i].plan || done[i]) continue;
        const auto ri = bi(checked[i].mover);
        const objects::ObjectId target = ri->target;
        const uint8_t flags = ri->flags;
        const uint32_t form = movement_->view(checked[i].mover).atk_form;
        const Vec3 tp = checked[i].at_p ? checked[i].p : st.position(unit_by_id(checked[i].tid)->h);
        std::vector<objects::ObjectId> leads;
        for (size_t j = 0; j < checked.size(); ++j) {
            if (done[j] || checked[j].halted || checked[j].dropped) continue;
            if (j != i && (form == 0 || bi(checked[j].mover)->target != target || movement_->view(checked[j].mover).atk_form != form))
                continue;
            done[j] = 1;
            AttackBlock& rj = *bi(checked[j].mover);
            rj.next = frame_ + 30u;
            rj.gx = tp.x;
            rj.gy = tp.y;
            leads.push_back(checked[j].lead);
        }
        if (land_attack_order(mode, leads, checked[i].tid, tp, flags, true, rng, false, checked[i].repath) == AttackPlan::Moved)
            ++stats_.attack_reissues;
    }
    // s78n t26 review round 3 (FIGHTER_FLYING §4.11 C 3 [H]): the dropped blocks - each unit of the mover (a squad
    // container's members) loses its attack order and attack goal (a position block has none, TACTICAL_COMBAT §2.8), the
    // movement its attack block; the record goes. [port] no move toward L (the formation stands within 40 of it).
    std::vector<objects::ObjectId> gone;
    for (const Checked& c : checked)
        if (c.dropped) gone.push_back(c.mover);
    if (gone.empty()) return;
    for (const objects::ObjectId mover : gone) {
        std::vector<objects::ObjectId> us;
        if (movement_->is_land_team_container(mover)) {
            for (const auto& [mid, slot] : movement_->land_team_slots(mover)) {
                (void)slot;
                us.push_back(mid);
            }
        } else {
            us.push_back(mover);
        }
        for (const objects::ObjectId u : us) {
            if (!combat_->has_unit(u)) continue;
            combat_->clear_order(u);
            combat_->set_attack_goal(u, tc::kNoObject);
        }
        const auto it = bi(mover);
        if (it != attack_blocks_.end() && it->mover == mover) attack_blocks_.erase(it);
        ++stats_.attack_unseen_drops;
    }
    movement_->clear_attack_block(gone);
}
}  // namespace alamo::sim
