// OI abilities (owner: tactical abilities agent): the battle-host side of docs/re/TACTICAL_GAPS.md Â§1-Â§2 / Â§7 for
// engine/tactical/abilities - the AbilityHost hooks (fog tests, map rectangle, locomotor state, ground height, the
// Battlefield_Modifier reveal list), the repair credit hooks, the ability actions after the tick (move / stop orders,
// owner changes), the objects abilities create (mines, summons, deploy troopers, barrage targets) and the death spawns
// (Destruction_Survivors, Tactical_Respawn). Objects are created after the combat tick in request / event order, each
// consuming an object id [port: the original creates them inside the object update].
#include <algorithm>
#include <cmath>
#include <vector>

#include "alamo/core/dmath.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/sim/tactical_battle.hpp"
#include "alamo/tactical/movement/special_locomotors.hpp"
#include "alamo/tactical/movement/unit_params.hpp"
#ifdef ALAMO_SIM_HAVE_ANIM
#include "alamo/anim/anim_types.hpp"
#include "alamo/sim/sim_anim.hpp"
#endif

namespace alamo::sim {

namespace tc = alamo::tactical::combat;
namespace ta = alamo::tactical::abilities;
namespace mv = alamo::tactical::movement;

namespace {
Vec3 oi_sv(tactical::Vec3 v) { return Vec3{v.x, v.y, v.z}; }
std::string_view types_trim(std::string_view s) {
    auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && ws(s.front())) s.remove_prefix(1);
    while (!s.empty() && ws(s.back())) s.remove_suffix(1);
    return s;
}
// q6 (OBJECT_SCRIPTS Â§10.4 step 2, 0x20ACD0): Spawn_Unit's facing, from the point toward the map origin: yaw =
// atan2(-y, -x) (0 at the origin in x / y), pitch = atan2(z, horizontal distance) when z != 0. [port] land objects drop
// the pitch (they stand on the terrain; the movement and combat facings of the port are planar there).
Vec3 facing_to_origin(Vec3 p, bool space) {
    const float yaw = (p.x == 0.0f && p.y == 0.0f) ? 0.0f : dmath::atan2(-p.y, -p.x);
    const float pitch = (space && p.z != 0.0f) ? dmath::atan2(p.z, dmath::sqrt(p.x * p.x + p.y * p.y)) : 0.0f;
    float sy = 0.0f, cy = 1.0f, sp = 0.0f, cp = 1.0f;
    dmath::sincos(yaw, sy, cy);
    dmath::sincos(pitch, sp, cp);
    return Vec3{cp * cy, cp * sy, sp};
}
}  // namespace

void TacticalBattle::oi_add_ability_types() {
    // The types abilities and deaths create (DEPLOY_TROOPERS Spawned_Object_Type, Mine_Type / Object_Type,
    // Destruction_Survivors) join the battle tables so the created objects are combat / ability units. Transitive
    // (a survivor type naming further types), at most three rebuilds.
    if (!tables_) return;
    const objects::GameObjectTypes& types = catalog_->types();
    for (int pass = 0; pass < 3; ++pass) {
        const size_t before = types_used_.size();
        auto use = [&](int32_t t) {
            if (t < 0 || static_cast<size_t>(t) >= types.size()) return;
            const auto ut = static_cast<uint32_t>(t);
            if (std::find(types_used_.begin(), types_used_.end(), ut) == types_used_.end()) types_used_.push_back(ut);
        };
        for (const ta::AbilityObjectDesc& o : tables_->abilities.objects)
            for (const ta::UnitAbilityRecord& r : o.unit_abilities)
                if (r.type == ta::kDeployTroopers) {  // Â§5.2: the spawn requests name the member types
                    use(r.spawned_type);
                    std::vector<int32_t> members;
                    if (oi_company_members(r.spawned_type, members))
                        for (const int32_t m : members) use(m);
                }
        bool thief = false;
        for (const ta::SpecialAbilityDesc& s : tables_->abilities.specials) {
            use(s.spawn_type);
            thief = thief || s.cls == ta::SpecialClass::VehicleThief;
        }
        if (thief) {  // Vehicle_Thief: the capture variants (Vehicle_Thief_Inside_Clone) of the battle types
            const std::vector<uint32_t> now = types_used_;
            for (const uint32_t t : now) {
                const std::string_view name = types_trim(types.get_string(static_cast<objects::TypeIndex>(t), "Vehicle_Thief_Inside_Clone"));
                if (name.empty()) continue;
                const objects::TypeIndex v = types.resolve_name(name);
                if (v != objects::kNone) use(static_cast<int32_t>(v));
            }
        }
        for (const tc::UnitDesc& u : tables_->combat.units)
            for (const auto& [t, w] : u.destruction_survivors) {
                (void)w;
                use(t);
            }
        if (types_used_.size() == before) break;
        tables_extend();
    }
}

bool TacticalBattle::oi_company_members(int32_t type, std::vector<int32_t>& members) const {
    // REMAINING_GAPS Â§5.2: Company_Units (+0x958), else Squadron_Units (+0x978), list order; unknown names are skipped.
    members.clear();
    const objects::GameObjectTypes& types = catalog_->types();
    if (type < 0 || static_cast<size_t>(type) >= types.size()) return false;
    const auto t = static_cast<objects::TypeIndex>(type);
    std::span<const std::string_view> list = types.get_list(t, "Company_Units");
    if (list.empty()) list = types.get_list(t, "Squadron_Units");
    if (list.empty()) return false;
    for (const std::string_view n : list) {
        const objects::TypeIndex m = types.resolve_name(types_trim(n));
        if (m != objects::kNone) members.push_back(static_cast<int32_t>(m));
    }
    return true;
}

void TacticalBattle::oi_install(TacticalMode& mode, SimRng& rng) {
    abilities_->bind_rng(&rng);  // command-phase applies (chance rolls, Confuse destinations, mine scatter)
    // Hardpoint repair (command 45) pays from the galactic account (unset: 0 credits, as tactical builds).
    combat_->set_credit_hooks([this](int32_t p) { return credits_get_ ? credits_get_(p) : 0.0f; },
                              [this](int32_t p, float d) {
                                  if (credits_add_) credits_add_(p, d);
                              });
    ta::AbilityHost h;
    h.point_fogged = [this](int32_t p, tactical::Vec3 v, bool force) { return fog_point_fogged(p, v.x, v.y, force); };
    h.object_hidden = [this](int32_t p, tc::ObjectId id, bool force) { return fog_object_hidden(p, id, force); };
    h.fow_reveal_add = [this](int32_t p, int32_t c, uint64_t s, float m) { fog_add_reveal_multiplier(p, c, s, m); };
    h.fow_reveal_remove = [this](int32_t p, uint64_t s) { fog_remove_reveal_modifier(p, s); };
    // FIGHTER_FLYING Â§6.5: the snowspeeder tow cable circles its target (a team container: every live member).
    h.cable_attack = [this](tc::ObjectId user, tc::ObjectId target, float r_min, float r_max) {
        if (!movement_) return;
        if (movement_->has_unit(user)) {
            movement_->start_circling(user, target, r_min, r_max);
            return;
        }
        for (const Unit& u : units_)
            if (u.alive && !u.retreated && u.container < containers_.size() && containers_[u.container].id == user &&
                movement_->has_unit(u.id))
                movement_->start_circling(u.id, target, r_min, r_max);
    };
    h.map_rect = [this](tactical::Vec3& lo, tactical::Vec3& hi) {
        float mn[2], mx[2];
        if (!map_.playable_box(mn, mx)) return false;
        lo = tactical::Vec3{mn[0], mn[1], 0.0f};
        hi = tactical::Vec3{mx[0], mx[1], 0.0f};
        return true;
    };
    h.moving = [this](tc::ObjectId id) {
        Unit* u = unit_by_id(id);
        if (!u || !u->movable || !movement_) return false;
        return movement_->view(id).speed > 0.0f;
    };
    h.ground_height = [this](float x, float y) { return space_ ? 0.0f : ground_height(x, y); };
    // REMAINING_GAPS Â§5.1: the first occupant of the vehicle (EJECT_VEHICLE_THIEF).
    h.first_occupant = [this](tc::ObjectId vehicle) -> tc::ObjectId {
        const std::vector<objects::ObjectId> in = garrisoned_units(vehicle);
        return in.empty() ? tc::kNoObject : in.front();
    };
    // Â§5.2: Company_Units, else Squadron_Units, as host type indexes.
    h.company_members = [this](int32_t type, std::vector<int32_t>& members) { return oi_company_members(type, members); };
    // Â§5.2: the owner's model bone (bind pose [M]); > 0 found, 0 missing bone, < 0 no model.
    h.bone_position = [this, st = &mode.primary()](tc::ObjectId owner, std::string_view bone, tactical::Vec3& pos) {
        Vec3 p{};
        const int r = model_bone_point(*st, st->find(owner), bone, p);
        if (r > 0) pos = tactical::Vec3{p.x, p.y, p.z};
        return r;
    };
    // OI t02 (ABILITIES_HARDPOINTS Â§4.12 [port]): the map's water plane lies above the terrain under the point.
    h.over_water = [this](tactical::Vec3 p) {
        if (space_ || !map_.terrain.present || map_.terrain.header.water_type == terrain::WaterType::None) return false;
        return map_.terrain.sample_height(p.x, p.y) < map_.terrain.header.water_height;
    };
#ifdef ALAMO_SIM_HAVE_ANIM
    // OI t03 (Â§4.13): clip frame count / fps from the sim model (an XML animation name resolved to its type id).
    h.anim_clip = [this, m = &mode](tc::ObjectId id, int32_t type, std::string_view name, uint32_t variant, uint32_t& frames,
                                    float& fps) {
        return anim_clip_info(*m, id, type >= 0 ? type : anim::find_anim_type(name), frames, fps, variant);
    };
    // OI follow-up t01 (Â§4.14): the ModelAnimType id of a name, and the type's Attack_Animation_Is_Overlay.
    h.anim_type = [](std::string_view name) { return anim::find_anim_type(name); };
    h.attack_overlay = [this, m = &mode](tc::ObjectId id) {
        if (!anim_) return false;
        const objects::ObjectStore& st = m->primary();
        const objects::ObjectHandle oh = st.find(id);
        return st.exists(oh) && anim_->params_for(st.type(oh), m->kind()).attack_overlay;
    };
#endif
    // OI follow-up t03 (A1_READINGS Â§28.9): the script's shares of the movement lock (Lock_Current_Orders, a Divert).
    // a1i t04: + the unload exit path of a landing object's unloaded units (Â§35.2, the locomotor's forced-move term).
    h.movement_locked = [this](tc::ObjectId id) { return script_movement_locked(id) || landing_locked(id); };
    // S8 t06 (LOB_SOLVER §3.5): Remote_Bomb range tests against a garrisoned target's host. S8 t10 (§5.1): the host's gun
    // bones of its current ALT entry (Num_Garrison_Guns FP_0<i>_ALT<a>, CombatSystem::garrison_fire_bones; s78f t03: world
    // position and x-axis yaw of the current pose).
    h.garrison_holder = [this](tc::ObjectId id) -> tc::ObjectId { return garrison_holder_of(id); };
    h.garrison_fire_bones = [this](tc::ObjectId holder, tactical::Vec3& holder_pos, std::vector<tc::GarrisonFireBone>& bones) {
        return combat_ && combat_->garrison_fire_bones(holder, holder_pos, bones);
    };
    // S8 t11 (MOVEMENT Â§3.2): DEPLOY drives the Walk locomotor's deploy states; oi_deploy_sync reports the changes back.
    h.deploy = [this](tc::ObjectId id, bool on, uint32_t frame) {
        if (!movement_) return false;
        const mv::MovementSystem::DeployResult r = movement_->deploy(id, on, frame);
        return r != mv::MovementSystem::DeployResult::NoWalk && r != mv::MovementSystem::DeployResult::Ignored;
    };
    // S8 t10 (LOB_SOLVER Â§3.5 "Facing at apply"): the thrown abilities snap the movement yaw (the combat facing follows).
    h.snap_yaw = [this](tc::ObjectId id, float yaw_deg) {
        if (movement_) movement_->snap_yaw(id, yaw_deg);
    };
    // s78l t05 (TACTICAL_COMBAT §5.4.1): Tactical_Bribe pays from the player's credits (the battle's credit hooks: the
    // galactic account in campaign battles; unset hooks = 0 credits, so a bribe with a cost fails) and reads the battle
    // planet's corruption (GalacticSim::corrupted of the request's planet; no planet = uncorrupted).
    h.pay_credits = [this](int32_t player, float cost) {
        const float have = credits_get_ ? credits_get_(player) : 0.0f;
        if (have < cost) return false;
        if (cost > 0.0f && credits_add_) credits_add_(player, -cost);
        return true;
    };
    h.battle_planet_corrupted = [this]() {
        const auto p = req_.info.planet;
        return galaxy_ && p != kInvalidPlanet && static_cast<size_t>(p) < galaxy_->planets().size() && galaxy_->corrupted(p);
    };
    hg_ability_host(h);  // hangars: DEPLOY_SQUAD gate / handler (HangarSystem::deploy_ready / deploy_squad)
    abilities_->set_host(std::move(h));
}

objects::ObjectId TacticalBattle::oi_spawn_unit(TacticalMode& mode, int32_t type, Vec3 pos, Vec3 facing, int32_t owner) {
    const objects::GameObjectTypes& types = catalog_->types();
    if (type < 0 || static_cast<size_t>(type) >= types.size() || !bt_rng_) return objects::kNoObjectId;
    objects::SpawnParams sp;
    sp.type = static_cast<objects::TypeIndex>(type);
    sp.owner = owner;
    sp.position = pos;
    sp.facing = facing;
    const objects::ObjectHandle h = mode.spawn(sp, 1.0f);
    if (!h) return objects::kNoObjectId;
    const objects::ObjectId id = mode.primary().id(h);
    const auto ut = static_cast<uint32_t>(type);
    if (std::find(types_used_.begin(), types_used_.end(), ut) == types_used_.end()) return id;  // a prop (not in the tables)
    bt_adopt_built(mode, h, owner, 1.0f, bt_rng_);  // combat / ability unit (hardpoint + SAP-init draws)
    Unit* u = unit_by_id(id);
    if (u && movement_ && mode.movable(h) && types.has_behavior(sp.type, objects::BehaviorSet::Union, objects::beh::Locomotor)) {
        mv::UnitDesc md = mv::unit_desc_from_type(types, ut);
        md.locomotor = mv::locomotor_kind_from_type(types, ut, space_ ? mv::MoveMode::Space : mv::MoveMode::Land);
        md.id = id;
        md.pos = {pos.x, pos.y};
        md.z = pos.z;
        md.yaw_deg = dmath::atan2(facing.y, facing.x) * (180.0f / dmath::kPiF);
        md.owner = owner;
        movement_->add_unit(md);
        u->movable = true;
    }
    return id;
}

void TacticalBattle::script_add_types(std::span<const uint32_t> add) {
    if (!tables_) return;
    const size_t before = types_used_.size();
    const size_t n = catalog_->types().size();
    for (const uint32_t t : add)
        if (t < n && std::find(types_used_.begin(), types_used_.end(), t) == types_used_.end()) types_used_.push_back(t);
    if (types_used_.size() == before) return;
    tables_extend();
    oi_add_ability_types();  // the new types' DEPLOY_TROOPERS / special spawn / Destruction_Survivors types
}

objects::ObjectHandle TacticalBattle::script_spawn(TacticalMode& mode, const objects::SpawnParams& p) {
    objects::ObjectHandle h{};
    if (p.container || p.attach_parent) {
        h = mode.spawn(p);
    } else {
        const uint32_t t = static_cast<uint32_t>(p.type);
        script_add_types(std::span<const uint32_t>(&t, 1));  // q6 t12: a real battle unit, not a prop
        const objects::ObjectId id = oi_spawn_unit(mode, static_cast<int32_t>(p.type), p.position, p.facing, p.owner);
        if (id != objects::kNoObjectId) h = mode.primary().find(id);
    }
    if (h) adopt_object(mode, h);
    return h;
}

objects::ObjectHandle TacticalBattle::script_spawn_company(TacticalMode& mode, const objects::SpawnParams& p) {
    const objects::GameObjectTypes& types = catalog_->types();
    const auto ct = static_cast<uint32_t>(p.type);
    if (finished_ || !bt_rng_ || !combat_ || ct >= types.size() || ct >= rules_->types.size() || rules_->type(ct).members.empty())
        return {};
    // The participant: a built company of p.owner (no galactic object), side by owner as bt_adopt_built.
    Participant P;
    P.galactic_id = objects::kNoObjectId;
    P.owner = p.owner;
    P.type = ct;
    P.side = p.owner == req_.info.defender ? 0 : 1;
    P.built = true;
    P.pop = 0.0f;
    const auto& gt = galaxy_->rules().types;
    P.power = ct < gt.size() ? gt[ct].ai_combat_power : 0.0f;
    // Tables: the members, the company type and its TEAM object / behaviour types (as the constructor does for companies).
    std::vector<uint32_t> add = spawn_types(P);
    add.push_back(ct);
    uint32_t ot = 0xFFFFFFFFu, bt = 0xFFFFFFFFu, squad = 0;
    if (!add.empty() && bt_team(ct, add.front(), ot, bt, squad)) {
        add.push_back(ot);
        add.push_back(bt);
    }
    script_add_types(add);
    // Â§10.4 steps 2 / 5: every member faces the map origin; its spot is the free placement ring search around the point
    // (members in Company_Units order, each later one avoiding the earlier ones). deploy() takes the spots in member
    // order (Participant::spots: position, facing).
    const Vec3 face = facing_to_origin(p.position, space_);
    const std::vector<uint32_t> member_types = spawn_types(P);
    std::vector<Vec3> spots;
    script_member_spots(mode, member_types, p.position, dmath::atan2(face.y, face.x) * (180.0f / dmath::kPiF), spots);
    for (const Vec3& s : spots) P.spots.emplace_back(s, face);
    const auto pi = static_cast<uint32_t>(parts_.size());
    parts_.push_back(std::move(P));
    const size_t first_unit = units_.size(), first_container = containers_.size();
    deploy(mode, pi, *bt_rng_);
    // Â§10.4 step 7: a TEAM container stands at its chunk's centroid, on land snapped passable [port: for the first
    // member's movement class]; the members keep their spots.
    if (!space_ && movement_ && units_.size() > first_unit) {
        objects::ObjectStore& st = mode.primary();
        const mv::UnitDesc md = mv::unit_desc_from_type(types, units_[first_unit].type);
        for (size_t ci = first_container; ci < containers_.size(); ++ci)
            if (st.alive(containers_[ci].h))
                st.set_position(containers_[ci].h, snap_passable(st.position(containers_[ci].h),
                                                                 movement_->classes().mask(md.movement_class), false));
    }
    bt_used_ = true;
    objects::ObjectHandle out{};
    if (containers_.size() > first_container) out = containers_[first_container].h;
    else if (units_.size() > first_unit) out = units_[first_unit].h;
    return out;  // deploy() registered the members with the L1 systems (l1_register)
}

bool TacticalBattle::script_damage(TacticalMode& mode, objects::ObjectId id, float amount, bool cannot_die) {
    Unit* u = unit_by_id(id);
    if (finished_ || !combat_ || !bt_rng_ || !u || !u->alive || u->retreated || !combat_->has_unit(id) || !combat_->alive(id))
        return false;
    const bool had_cannot_die = (combat_->flags(id) & tc::kUnitCannotDie) != 0;
    if (cannot_die && !had_cannot_die) combat_->set_flag(id, tc::kUnitCannotDie, true);
    tc::DamageRequest r;
    r.victim = id;
    r.amount = amount;
    r.kind = tc::kKindDebug;  // Lua Take_Damage: kind 0 (OBJECT_SCRIPTS Â§6.1 step 4)
    const size_t deaths_before = combat_->deaths().size();
    combat_->take_damage(r, frame_);
    if (cannot_die && !had_cannot_die && combat_->has_unit(id)) combat_->set_flag(id, tc::kUnitCannotDie, false);
    // The deaths this hit caused (the victim, team members it was shared with), in Die order.
    const std::vector<tc::DeathEvent> deaths(combat_->deaths().begin() + static_cast<std::ptrdiff_t>(deaths_before), combat_->deaths().end());
    for (const tc::DeathEvent& d : deaths) process_death(mode, d, *bt_rng_);
    return true;
}

bool TacticalBattle::script_invulnerable(objects::ObjectId id, bool on) {
    if (finished_ || !combat_) return false;
    auto apply = [this, on](objects::ObjectId u) {
        if (!combat_->has_unit(u)) return;
        combat_->set_flag(u, tc::kUnitInvulnerable, on);
        if (!on) combat_->end_invulnerable(u);
    };
    // A TEAM container (Â§10.2): each member, in member (unit list) order; the container itself has no combat entry.
    for (size_t ci = 0; ci < containers_.size(); ++ci) {
        if (containers_[ci].id != id || !containers_[ci].team) continue;
        for (const Unit& u : units_)
            if (u.container == ci && u.alive && !u.retreated) apply(u.id);
        return true;
    }
    if (!combat_->has_unit(id)) return false;
    apply(id);
    return true;
}

void TacticalBattle::script_member_spots(const TacticalMode& mode, std::span<const uint32_t> members, Vec3 at, float yaw_deg,
                                         std::vector<Vec3>& out) const {
    out.clear();
    const objects::GameObjectTypes& types = catalog_->types();
    auto has_model = [&](uint32_t t) {
        return !types_trim(types.get_string(t, space_ ? "Space_Model_Name" : "Land_Model_Name")).empty();
    };
    constexpr uint64_t kLayer20 = 0x20;  // Space_Layer 0x20: never a blocker (space_member_spots)
    // Blockers: as space_member_spots (the object tree, all owners: live, not dying, not Is_Decoration, a model for the
    // mode, no TRANSPORT_LANDING / LAND_OBSTACLE / PROJECTILE; space: Space_Layer != 0x20), their box in x / y.
    const objects::ObjectStore& st = mode.primary();
    std::vector<FreePlacementBox> blockers;
    for (const objects::ObjectHandle h : st.all_objects()) {
        if (!st.alive(h) || st.dying(h)) continue;
        const objects::TypeIndex t = st.type(h);
        if (t >= types.size() || types.get_bool(t, "Is_Decoration") || !has_model(t)) continue;
        if (st.has_behavior(h, objects::beh::TransportLanding) || st.has_behavior(h, objects::beh::LandObstacle) ||
            st.has_behavior(h, objects::beh::Projectile))
            continue;
        if (space_ && types.get_mask(t, "Space_Layer") == kLayer20) continue;
        const tactical::Aabb b = mode.bounds_of(h);
        blockers.push_back({b.min.x, b.min.y, b.max.x, b.max.y});
    }
    // Land: the candidate's cell is pure Clear (effective bits & Clear == bits; out of the grid: never).
    std::function<bool(float, float)> accept;
    if (!space_ && movement_) {
        const mv::PassabilityGrid* g = &movement_->grid();
        accept = [g](float x, float y) { return g->passable_world({x, y}, mv::kPassClear); };
    }
    for (const uint32_t t : members) {
        // The member's model box (model space, no Scale_Factor). [port] without model data (no art loaded) the type's
        // hard extents (drop_half_extents), so the members still spread instead of all standing on the point.
        FreePlacementBox local;
        const BattleCatalog::ModelInfo* m = t < types.size() ? catalog_->model_info(t, space_, true) : nullptr;
        if (m && m->has_bounds) {
            local = {m->min[0], m->min[1], m->max[0], m->max[1]};
        } else {
            float hx = 0.0f, hy = 0.0f;
            drop_half_extents(t, hx, hy);
            local = {-hx, -hy, hx, hy};
        }
        float qx = 0.0f, qy = 0.0f;
        if (!find_free_placement(at.x, at.y, local, blockers, 2500.0f, qx, qy, yaw_deg - 45.0f, accept)) {
            qx = at.x;  // none: (0, 0, 0), replaced by the given point
            qy = at.y;
        }
        const float z = (!space_ && map_.terrain.present) ? map_.terrain.sample_height(qx, qy) : at.z;
        out.push_back(Vec3{qx, qy, z});
        blockers.push_back({qx + local.min_x, qy + local.min_y, qx + local.max_x, qy + local.max_y});
    }
}

bool TacticalBattle::script_has_model(const TacticalMode& mode, objects::ObjectHandle h) const {
    const objects::ObjectStore& store = mode.primary();
    if (!store.exists(h)) return false;
    return !types_trim(catalog_->types().get_string(store.type(h), space_ ? "Space_Model_Name" : "Land_Model_Name")).empty();
}

Vec3 TacticalBattle::script_spawn_facing(Vec3 p) const { return facing_to_origin(p, space_); }

void TacticalBattle::oi_events(TacticalMode& mode) {
    if (!abilities_ || !combat_) return;
    objects::ObjectStore& store = mode.primary();
    // a. ability actions (copies: the handlers below may add objects, which raise new events)
    const std::vector<ta::AbilityEvent> events(abilities_->events().begin(), abilities_->events().end());
    std::vector<objects::ObjectId> remove_silently;  // STATION_HANGARS_SOUND Â§1.1: after the station upgrade (b)
    for (const ta::AbilityEvent& e : events) {
        switch (e.kind) {
            case ta::AbilityEventKind::RemoveSilently: remove_silently.push_back(e.id); break;
            case ta::AbilityEventKind::MoveOrder: {  // 0x2a7b80 moves (HUNT, JET_PACK, CONFUSE, ...): not an order cancel
                Unit* u = unit_by_id(e.id);
                if (u && u->alive && !u->retreated && u->movable && bt_rng_) {
                    // S7 t06: JET_PACK (0x3a38d0) swaps a Uses_Multiple_Locomotors unit to its secondary locomotor
                    // (0x3a9910, MOVEMENT Â§3.1.1) before the move; the JETPACK locomotor flies it and swaps back on landing.
                    // Its weapons are suspended meanwhile (kUnitWeaponsSuspended, set before each combat tick).
                    if (e.type == ta::kJetPack && movement_) movement_->use_secondary_locomotor(e.id, true);
                    const objects::ObjectId ids[] = {e.id};
                    exec_move(mode, ids, e.point.x, e.point.y, *bt_rng_, mv::kNoFacing, false);
                }
                break;
            }
            case ta::AbilityEventKind::Lured: {
                // cu8f t06 (CU8F_T04_T06_RE §3.1): the lured unit's coordinator move toward the lure owner - an attack-move
                // with the owner (or its team) as the goal object, so the ordered attack the ability issued stays.
                Unit* u = unit_by_id(e.id);
                if (u && u->alive && !u->retreated && u->movable && bt_rng_) {
                    const objects::ObjectId ids[] = {e.id};
                    exec_move(mode, ids, e.point.x, e.point.y, *bt_rng_, mv::kNoFacing, true, 0, false, e.target);
                }
                break;
            }
            case ta::AbilityEventKind::StopOrder: {
                Unit* u = unit_by_id(e.id);
                if (u && u->alive && u->movable && movement_) {
                    const std::vector<objects::ObjectId> ids{e.id};
                    movement_->stop(ids);
                }
                break;
            }
            case ta::AbilityEventKind::MarkerTossed: {
                // S8 t10 (LOB_SOLVER Â§3.5 "MARKER removal"): the toss kills its ground-point MARKER (Dummy_Barrage_Target)
                // with an ordinary Destroy_Object, spawned bomb or not. [port] removed here, after the ability pass.
                for (size_t i = 0; i < dummies_.size(); ++i) {
                    if (dummies_[i].id != e.target) continue;
                    abilities_->remove(dummies_[i].id);
                    if (store.alive(dummies_[i].h)) store.destroy(dummies_[i].h);
                    dummies_.erase(dummies_.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
                break;
            }
            case ta::AbilityEventKind::OwnerChanged: {  // Hack / Tactical_Bribe / convert: store, trees, movement, fog
                const objects::ObjectHandle h = store.find(e.id);
                if (store.alive(h) && store.owner(h) != e.value) change_owner(mode, h, e.value);
                // s78k t04 (TACTICAL_COMBAT §3.5 "Convert and its team rule" steps 4-5, 0x38e5a0): Change_Owner spreads
                // through the team, so the TEAM container follows its member - the store owner (not a combat unit, so
                // CombatSystem's convert spread raises no event for it) and Container::owner (loss records, replenish) -
                // and every other live member not changed yet ([port] container, then ascending id: the notification
                // order). Convert raises one event per member (each finds the team done); Hack / Bribe raise the object
                // and its container. A non-team object has no container: nothing more.
                if (store.alive(h) && store.owner(h) == e.value) team_change_owner(mode, e.id, e.value);
                break;
            }
            default: break;
        }
    }
    // b. objects the abilities asked for, request order (one id each)
    const std::vector<ta::SpawnRequest> spawns(abilities_->spawn_requests().begin(), abilities_->spawn_requests().end());
    for (const ta::SpawnRequest& q : spawns) {
        switch (q.purpose) {
            case ta::SpawnPurpose::Mine:
            case ta::SpawnPurpose::Summon: {
                const objects::ObjectId id = oi_spawn_unit(mode, q.object_type, oi_sv(q.position), oi_sv(q.facing), q.owner);
                if (q.purpose == ta::SpawnPurpose::Mine && id != objects::kNoObjectId) abilities_->bind_spawn(q.serial, id);
                break;
            }
            case ta::SpawnPurpose::Troopers: {
                // Â§5.2: member type q.object_type at the bone, the owner's facing; anim 0x5f (none when the model lacks
                // it), attached at bone q.bone of q.source (0x38d320), +0x3a2 |= 2, then the trooper list (bind_spawn).
                // [port] locomotor vf +0x200 is the movement registration in oi_spawn_unit.
                const objects::ObjectId id = oi_spawn_unit(mode, q.object_type, oi_sv(q.position), oi_sv(q.facing), q.owner);
                if (id == objects::kNoObjectId) break;
                play_clip(mode, id, 0x5f, 0.0f, 1.0f, oi_global_frame_);
                if (Unit* u = unit_by_id(id)) {
                    u->attached_to = q.source;
                    u->attach_bone = q.bone;
                    u->flags_3a2 = static_cast<uint8_t>(u->flags_3a2 | 2u);
                }
                abilities_->bind_spawn(q.serial, id);
                break;
            }
            case ta::SpawnPurpose::CaptureVariant: {
                // Vehicle_Thief (TACTICAL_GAPS Â§2.3.10): the victim type's capture variant (+0xda8, the resolved
                // Vehicle_Thief_Inside_Clone of +0xda0) at the victim with the victim's owner (one id); the thief
                // boards it; EJECT_VEHICLE_THIEF (46) on for the variant (REMAINING_GAPS Â§5.1). No variant: nothing.
                int32_t vt = q.object_type;
                const Unit* victim = unit_by_id(q.target);
                if (vt < 0 && victim) {
                    const std::string_view name =
                        types_trim(catalog_->types().get_string(static_cast<objects::TypeIndex>(victim->type), "Vehicle_Thief_Inside_Clone"));
                    if (!name.empty()) {
                        const objects::TypeIndex t = catalog_->types().resolve_name(name);
                        if (t != objects::kNone) vt = static_cast<int32_t>(t);
                    }
                }
                if (vt < 0) break;
                const objects::ObjectId id = oi_spawn_unit(mode, vt, oi_sv(q.position), oi_sv(q.facing), q.owner);
                if (id == objects::kNoObjectId) break;
                s4_board(mode, q.source, id);
                abilities_->set_unit_ability_active(id, ta::kEjectVehicleThief, true, frame_);
                break;
            }
            case ta::SpawnPurpose::BarrageTarget: {
                // BARRAGE (Â§2.2): Dummy_Barrage_Target owned by the shooter's first enemy, then an Attack command on it.
                if (!bt_rng_ || !combat_->has_unit(q.source)) break;
                const int32_t issuer = combat_->owner(q.source);
                const objects::ObjectId d = spawn_dummy_target(mode, issuer, oi_sv(q.position), *bt_rng_);
                if (d == objects::kNoObjectId) break;
                if (!dummies_.empty()) dummies_.back().frame = frame_ + 600;  // [port] the barrage keeps its target 20 s
                combat_->order_attack(q.source, d);
                break;
            }
            case ta::SpawnPurpose::Wingman: {
                // OI host: REPLENISH_WINGMEN (0x3a2970) - the members missing from the squadron's list (Company_Units, else
                // Squadron_Units of the team's participant type, list order; live members matched per type) are created
                // one each at the request point with the team's owner and join the team. [port] formation slots are not
                // modelled (every new member at the team position); a type outside the battle tables is skipped.
                uint32_t ci = 0xFFFFFFFFu;
                for (uint32_t c = 0; c < containers_.size(); ++c)
                    if (containers_[c].id == q.target && !containers_[c].removed) ci = c;
                if (ci == 0xFFFFFFFFu || !bt_rng_) break;
                int32_t squad_type = -1;
                std::vector<int32_t> live;
                for (const Unit& u : units_)
                    if (u.container == ci && u.alive && !u.retreated) {
                        if (squad_type < 0) squad_type = static_cast<int32_t>(parts_[u.participant].type);
                        live.push_back(static_cast<int32_t>(u.type));
                    }
                std::vector<int32_t> members;
                if (squad_type < 0 || !oi_company_members(squad_type, members)) break;
                for (const int32_t m : members) {
                    if (const auto it = std::find(live.begin(), live.end(), m); it != live.end()) {
                        live.erase(it);  // present
                        continue;
                    }
                    objects::SpawnParams sp;
                    sp.type = static_cast<objects::TypeIndex>(m);
                    sp.owner = containers_[ci].owner;
                    sp.position = oi_sv(q.position);
                    sp.facing = oi_sv(q.facing);
                    const objects::ObjectHandle h = mode.spawn(sp, 1.0f);
                    if (!h) continue;
                    if (bt_adopt_built(mode, h, sp.owner, 1.0f, bt_rng_, false, ci) == 0xFFFFFFFFu) store.destroy(h);
                }
                break;
            }
            case ta::SpawnPurpose::LureEffect: {
                // cu8f t06 (CU8F_T04_T06_RE §3.1): the LURE record's Particle_Effect at the owner (a prop, one object id);
                // the ability system raises RemoveSilently for it at the deactivation.
                const ta::UnitAbilityRecord* r = abilities_->record(q.source, ta::kLure);
                if (!r || r->particle_effect.empty()) break;
                const objects::TypeIndex t = catalog_->types().raw().find(r->particle_effect);
                if (t == objects::kNone || t >= catalog_->types().size()) break;
                const objects::ObjectHandle h = spawn_prop(mode, static_cast<int32_t>(t), oi_sv(q.position), oi_sv(q.facing), q.owner);
                // cu8g t05: not bound (the lure went off, or its owner is gone, before this pass): removed at once.
                if (h && !abilities_->bind_spawn(q.serial, store.id(h))) remove_silently.push_back(store.id(h));
                break;
            }
            case ta::SpawnPurpose::StationUpgrade: hg_station_upgrade(mode, q); break;  // STATION_HANGARS_SOUND Â§1.2
            default: break;  // InstantEffect: [port] the variant / composition is not resolved
        }
    }
    for (const objects::ObjectId id : remove_silently) {  // the upgrade object: out of its station, abilities, store
        hg_remove_silently(id);
        bt_kill_silently(mode, store.find(id));
    }
    // c. Die step 7 (Â§1.9): Tactical_Respawn (delayed) and Destruction_Survivors, event order. Owner rule 0x3751a0
    //    [port: the dying object's owner].
    for (const tc::SurvivorEvent& s : combat_->survivor_events()) {
        if (s.respawn) {
            oi_respawns_.push_back(OiRespawn{s.due_frame, s.type, s.owner, oi_sv(s.position), oi_sv(s.facing)});
        } else {
            oi_spawn_unit(mode, s.type, oi_sv(s.position), oi_sv(s.facing), s.owner);
        }
    }
}

void TacticalBattle::oi_respawn_due(TacticalMode& mode) {
    size_t w = 0;
    for (size_t i = 0; i < oi_respawns_.size(); ++i) {
        const OiRespawn r = oi_respawns_[i];
        if (r.due <= frame_) {
            oi_spawn_unit(mode, r.type, r.pos, r.facing, r.owner);
            continue;
        }
        oi_respawns_[w++] = r;
    }
    oi_respawns_.resize(w);
}

void TacticalBattle::oi_deploy_sync() {
    if (!movement_) return;
    for (const mv::MovementSystem::DeployEvent& e : movement_->take_deploy_events()) {
        if (abilities_) abilities_->set_deploy_state(e.id, static_cast<uint8_t>(e.to));
        // AI_TACTICAL Â§7.3.3 item 3: Walk vf6 0x61e930 raises 0x29 at the end of the Deploy / Undeploy clip, Deploy(true)
        // 0x61d300 at once on a blocked reverse. This is the deploy's "unit ability finished".
        if (e.finished) ability_finished_.push_back(e.id);
    }
}

void TacticalBattle::oi_hash(StateHasher& h) const {
    for (const Unit& u : units_) {  // Â§5.2 trooper attachments (ascending id; nothing hashed without troopers)
        if (u.attached_to == objects::kNoObjectId) continue;
        h.i32(u.id);
        h.i32(u.attached_to);
        h.i32(u.attach_bone);
        h.u32(u.flags_3a2);
    }
    h.u32(static_cast<uint32_t>(oi_respawns_.size()));
    for (const OiRespawn& r : oi_respawns_) {
        h.u32(r.due);
        h.i32(r.type);
        h.i32(r.owner);
        h.f32(r.pos.x);
        h.f32(r.pos.y);
        h.f32(r.pos.z);
    }
}

}  // namespace alamo::sim
