// OI abilities (owner: tactical abilities agent): the battle-host side of docs/re/TACTICAL_GAPS.md §1-§2 / §7 for
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
                if (r.type == ta::kDeployTroopers) {  // §5.2: the spawn requests name the member types
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
    // REMAINING_GAPS §5.2: Company_Units (+0x958), else Squadron_Units (+0x978), list order; unknown names are skipped.
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
#ifdef ALAMO_SIM_HAVE_FOG
    h.point_fogged = [this](int32_t p, tactical::Vec3 v, bool force) { return fog_point_fogged(p, v.x, v.y, force); };
    h.object_hidden = [this](int32_t p, tc::ObjectId id, bool force) { return fog_object_hidden(p, id, force); };
    h.fow_reveal_add = [this](int32_t p, int32_t c, uint64_t s, float m) { fog_add_reveal_multiplier(p, c, s, m); };
    h.fow_reveal_remove = [this](int32_t p, uint64_t s) { fog_remove_reveal_modifier(p, s); };
#endif
    // FIGHTER_FLYING §6.5: the snowspeeder tow cable circles its target (a team container: every live member).
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
    // REMAINING_GAPS §5.1: the first occupant of the vehicle (EJECT_VEHICLE_THIEF).
    h.first_occupant = [this](tc::ObjectId vehicle) -> tc::ObjectId {
        const std::vector<objects::ObjectId> in = garrisoned_units(vehicle);
        return in.empty() ? tc::kNoObject : in.front();
    };
    // §5.2: Company_Units, else Squadron_Units, as host type indexes.
    h.company_members = [this](int32_t type, std::vector<int32_t>& members) { return oi_company_members(type, members); };
    // §5.2: the owner's model bone (bind pose [M]); > 0 found, 0 missing bone, < 0 no model.
    h.bone_position = [this, st = &mode.primary()](tc::ObjectId owner, std::string_view bone, tactical::Vec3& pos) {
        Vec3 p{};
        const int r = model_bone_point(*st, st->find(owner), bone, p);
        if (r > 0) pos = tactical::Vec3{p.x, p.y, p.z};
        return r;
    };
    // OI t02 (ABILITIES_HARDPOINTS §4.12 [port]): the map's water plane lies above the terrain under the point.
    h.over_water = [this](tactical::Vec3 p) {
        if (space_ || !map_.terrain.present || map_.terrain.header.water_type == terrain::WaterType::None) return false;
        return map_.terrain.sample_height(p.x, p.y) < map_.terrain.header.water_height;
    };
#ifdef ALAMO_SIM_HAVE_ANIM
    // OI t03 (§4.13): clip frame count / fps from the sim model (an XML animation name resolved to its type id).
    h.anim_clip = [this, m = &mode](tc::ObjectId id, int32_t type, std::string_view name, uint32_t variant, uint32_t& frames,
                                    float& fps) {
        return anim_clip_info(*m, id, type >= 0 ? type : anim::find_anim_type(name), frames, fps, variant);
    };
    // OI follow-up t01 (§4.14): the ModelAnimType id of a name, and the type's Attack_Animation_Is_Overlay.
    h.anim_type = [](std::string_view name) { return anim::find_anim_type(name); };
    h.attack_overlay = [this, m = &mode](tc::ObjectId id) {
        if (!anim_) return false;
        const objects::ObjectStore& st = m->primary();
        const objects::ObjectHandle oh = st.find(id);
        return st.exists(oh) && anim_->params_for(st.type(oh), m->kind()).attack_overlay;
    };
#endif
    // OI follow-up t03 (A1_READINGS §28.9): the script's shares of the movement lock (Lock_Current_Orders, a Divert).
    // a1i t04: + the unload exit path of a landing object's unloaded units (§35.2, the locomotor's forced-move term).
    h.movement_locked = [this](tc::ObjectId id) { return script_movement_locked(id) || landing_locked(id); };
    // S8 t06 (LOB_SOLVER §3.5): Remote_Bomb range tests against a garrisoned target's host. S8 t10 (§5.1): the host's gun
    // bones of its current ALT entry (Num_Garrison_Guns FP_0<i>_ALT<a>, CombatSystem::garrison_fire_bones; s78f t03: world
    // position and x-axis yaw of the current pose).
    h.garrison_holder = [this](tc::ObjectId id) -> tc::ObjectId { return garrison_holder_of(id); };
    h.garrison_fire_bones = [this](tc::ObjectId holder, tactical::Vec3& holder_pos, std::vector<tc::GarrisonFireBone>& bones) {
        return combat_ && combat_->garrison_fire_bones(holder, holder_pos, bones);
    };
    // S8 t11 (MOVEMENT §3.2): DEPLOY drives the Walk locomotor's deploy states; oi_deploy_sync reports the changes back.
    h.deploy = [this](tc::ObjectId id, bool on, uint32_t frame) {
        if (!movement_) return false;
        const mv::MovementSystem::DeployResult r = movement_->deploy(id, on, frame);
        return r != mv::MovementSystem::DeployResult::NoWalk && r != mv::MovementSystem::DeployResult::Ignored;
    };
    // S8 t10 (LOB_SOLVER §3.5 "Facing at apply"): the thrown abilities snap the movement yaw (the combat facing follows).
    h.snap_yaw = [this](tc::ObjectId id, float yaw_deg) {
        if (movement_) movement_->snap_yaw(id, yaw_deg);
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

void TacticalBattle::oi_events(TacticalMode& mode) {
    if (!abilities_ || !combat_) return;
    objects::ObjectStore& store = mode.primary();
    // a. ability actions (copies: the handlers below may add objects, which raise new events)
    const std::vector<ta::AbilityEvent> events(abilities_->events().begin(), abilities_->events().end());
    std::vector<objects::ObjectId> remove_silently;  // STATION_HANGARS_SOUND §1.1: after the station upgrade (b)
    for (const ta::AbilityEvent& e : events) {
        switch (e.kind) {
            case ta::AbilityEventKind::RemoveSilently: remove_silently.push_back(e.id); break;
            case ta::AbilityEventKind::MoveOrder: {  // 0x2a7b80 moves (HUNT, JET_PACK, CONFUSE, ...): not an order cancel
                Unit* u = unit_by_id(e.id);
                if (u && u->alive && !u->retreated && u->movable && bt_rng_) {
                    // S7 t06: JET_PACK (0x3a38d0) swaps a Uses_Multiple_Locomotors unit to its secondary locomotor
                    // (0x3a9910, MOVEMENT §3.1.1) before the move; the JETPACK locomotor flies it and swaps back on landing.
                    // Its weapons are suspended meanwhile (kUnitWeaponsSuspended, set before each combat tick).
                    if (e.type == ta::kJetPack && movement_) movement_->use_secondary_locomotor(e.id, true);
                    const objects::ObjectId ids[] = {e.id};
                    exec_move(mode, ids, e.point.x, e.point.y, *bt_rng_, mv::kNoFacing, false);
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
                // S8 t10 (LOB_SOLVER §3.5 "MARKER removal"): the toss kills its ground-point MARKER (Dummy_Barrage_Target)
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
                // §5.2: member type q.object_type at the bone, the owner's facing; anim 0x5f (none when the model lacks
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
                // Vehicle_Thief (TACTICAL_GAPS §2.3.10): the victim type's capture variant (+0xda8, the resolved
                // Vehicle_Thief_Inside_Clone of +0xda0) at the victim with the victim's owner (one id); the thief
                // boards it; EJECT_VEHICLE_THIEF (46) on for the variant (REMAINING_GAPS §5.1). No variant: nothing.
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
                // BARRAGE (§2.2): Dummy_Barrage_Target owned by the shooter's first enemy, then an Attack command on it.
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
            case ta::SpawnPurpose::StationUpgrade: hg_station_upgrade(mode, q); break;  // STATION_HANGARS_SOUND §1.2
            default: break;  // InstantEffect: [port] the variant / composition is not resolved
        }
    }
    for (const objects::ObjectId id : remove_silently) {  // the upgrade object: out of its station, abilities, store
        hg_remove_silently(id);
        bt_kill_silently(mode, store.find(id));
    }
    // c. Die step 7 (§1.9): Tactical_Respawn (delayed) and Destruction_Survivors, event order. Owner rule 0x3751a0
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
        // AI_TACTICAL §7.3.3 item 3: Walk vf6 0x61e930 raises 0x29 at the end of the Deploy / Undeploy clip, Deploy(true)
        // 0x61d300 at once on a blocked reverse. This is the deploy's "unit ability finished".
        if (e.finished) ability_finished_.push_back(e.id);
    }
}

void TacticalBattle::oi_hash(StateHasher& h) const {
    for (const Unit& u : units_) {  // §5.2 trooper attachments (ascending id; nothing hashed without troopers)
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
