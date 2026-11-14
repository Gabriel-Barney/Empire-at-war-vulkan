// tactical_battle: part 2 (bo3 t03 split; shared code in tactical_battle_internal.hpp).
#include "tactical_battle_internal.hpp"

namespace alamo::sim {
void TacticalBattle::set_fleet_cohesion(uint8_t player, bool tight) {
    if (player < fleet_cohesion_.size()) fleet_cohesion_[player] = tight ? 1u : 0u;
}

// [port] fleet cohesion: only the player's own UI moves (flag 0x04) follow the setting; AI / Lua / script moves send 0.
bool TacticalBattle::cohesion_for(uint8_t player, uint8_t move_flags) const {
    return (move_flags & mv::kMoveFlagPlayerOrder) != 0 && fleet_cohesion(player);
}

cmd::RejectReason TacticalBattle::order_move(TacticalMode& mode, uint8_t player, std::span<const objects::ObjectId> units, float x,
                                             float y, SimRng& rng, float facing_deg, bool attack_move, bool queued,
                                             uint8_t move_flags, bool switch_locomotor) {
    using R = cmd::RejectReason;
    if (finished_ || !movement_) return R::NotAllowed;
    const auto own = owned_units<Unit>(mode.primary(), player, units, [this](objects::ObjectId id) { return unit_by_id(id); }, true);
    if (own.empty()) return R::InvalidUnit;
    if (!std::isfinite(x) || !std::isfinite(y)) return R::InvalidPosition;
    std::vector<objects::ObjectId> ids;
    for (const Unit* u : own) ids.push_back(u->id);
    if (abilities_)  // OI abilities: order cancel 0x38f5d0 (BARRAGE / LUCKY / ION / HUNT off on a new order)
        for (const objects::ObjectId id : ids) abilities_->order_cancel(id, frame_);
    drop_locked(ids);  // OI follow-up t03: the group move leaves locked units out (after the cancel, §28.9)
    if (ids.empty()) return R::None;
    if (!queued) {
        clear_queues(ids);
        raise_order_replaced(ids);  // S7 t31: signal 0xb
        exec_move(mode, ids, x, y, rng, facing_deg, attack_move, move_flags, switch_locomotor, kNoObjectId, false,
                  cohesion_for(player, move_flags));
        return R::None;
    }
    // MOVEMENT §6.2: the swap comes first, then the waypoint queue (each swapped unit continues its re-issued order).
    if (switch_locomotor)
        for (const objects::ObjectId id : ids) movement_->use_secondary_locomotor(id, true);
    const tactical::Aabb& bb = mode.bounds();
    // s78f t02 (MOVEMENT §6.2, FIGHTER_FLYING §4.11 B 1): a space fighter whose squadron (or own) formation exists takes the
    // shift-queued move into its block stack (the waypoint append 0x420f70: inserted at the bottom with 0x04, run after
    // everything queued; a guard above it ends once its object stops) instead of the host's order queue [port: Gabriel's
    // queue keeps the other units].
    if (space_) {
        std::vector<objects::ObjectId> wp, rest;
        for (const objects::ObjectId id : ids) {
            const Unit* u = unit_by_id(id);
            const bool fighter = u && movement_->has_unit(id) && movement_->view(id).locomotor == mv::LocomotorKind::Fighter;
            if (fighter && movement_->fighter_formation(fighter_key(*u))) wp.push_back(id);
            else rest.push_back(id);
        }
        if (!wp.empty()) {
            mv::MoveOrder o;
            o.units = wp;
            o.flags = static_cast<uint8_t>(move_flags | mv::kMoveFlagQueue | (attack_move ? mv::kMoveFlagAttackMove : 0));
            o.destination = {std::clamp(x, bb.min.x, bb.max.x), std::clamp(y, bb.min.y, bb.max.y)};
            if (std::isfinite(facing_deg)) o.facing_deg = facing_deg;
            o.waypoint = true;
            movement_->move(o, rng, frame_);
            // s78f review: the block went into the formation's stack (it does not replace the current order); the host
            // record (fighter_order) is only created when missing, and then mirrors the stack (fighter_sync).
            std::vector<objects::ObjectId> fresh;
            for (const objects::ObjectId id : wp)
                if (const Unit* u = unit_by_id(id); u && !fighter_formation(fighter_key(*u))) fresh.push_back(id);
            if (!fresh.empty())
                fighter_order(fresh, o.flags, Vec3{o.destination.x, o.destination.y, 0.0f});
        }
        ids = std::move(rest);
        if (ids.empty()) return R::None;
    }
    QueuedOrder q;
    q.kind = QueuedOrder::Kind::Move;
    q.player = player;
    q.x = std::clamp(x, bb.min.x, bb.max.x);
    q.y = std::clamp(y, bb.min.y, bb.max.y);
    if (std::isfinite(facing_deg)) q.facing = facing_deg;
    q.attack_move = attack_move;
    q.flags = move_flags;
    const std::vector<objects::ObjectId> now = enqueue(ids, q);
    raise_order_replaced(now);  // S7 t31: the units that start it now
    if (!now.empty())
        exec_move(mode, now, q.x, q.y, rng, q.facing, attack_move, move_flags, false, kNoObjectId, false,
                  cohesion_for(player, move_flags));
    return R::None;
}

cmd::RejectReason TacticalBattle::order_facing(TacticalMode& mode, uint8_t player, std::span<const objects::ObjectId> units,
                                               float facing_deg, SimRng& rng) {
    using R = cmd::RejectReason;
    if (finished_ || !movement_) return R::NotAllowed;
    if (!std::isfinite(facing_deg)) return R::InvalidPosition;
    const objects::ObjectStore& st = mode.primary();
    const auto own = owned_units<Unit>(st, player, units, [this](objects::ObjectId id) { return unit_by_id(id); }, true);
    if (own.empty()) return R::InvalidUnit;
    for (const Unit* u : own) {
        if (abilities_) abilities_->order_cancel(u->id, frame_);
        if (movement_locked(u->id)) continue;  // OI follow-up t03: the refused move path (§28.9 [M])
        raise_order_replaced(u->id);  // S7 t31 [M: as Turn_To_Face 0x581cd0]
        team_attack_drop(std::span<const objects::ObjectId>(&u->id, 1));  // s78n t20 review: the attack block ends too
        const Vec3 p = st.position(u->h);
        mv::MoveOrder o;
        o.units = {u->id};
        o.destination = {p.x, p.y};
        o.facing_deg = facing_deg;
        movement_->move(o, rng, frame_);
        fighter_order(o.units, 0, Vec3{p.x, p.y, 0.0f});  // S7 t22
    }
    return R::None;
}

cmd::RejectReason TacticalBattle::order_attack(TacticalMode& mode, uint8_t player, std::span<const objects::ObjectId> units,
                                               objects::ObjectId target, SimRng& rng, int32_t hardpoint, bool queued) {
    using R = cmd::RejectReason;
    if (finished_ || !combat_) return R::NotAllowed;
    const objects::ObjectStore& st = mode.primary();
    const Unit* t = unit_by_id(target);
    // q6: a TEAM container is a valid target on its own (A1_READINGS §33.2.1 row 1; TeamAttack in tactical_battle.hpp)
    const Container* ct = t ? nullptr : team_target(target);
    if (ct ? !combat_->is_enemy(player, st.owner(ct->h)) : (!t || !t->alive || t->retreated || !combat_->is_enemy(player, st.owner(t->h))))
        return R::InvalidTarget;
    const int32_t hp = (t && hardpoint >= 0 && static_cast<uint32_t>(hardpoint) < combat_->hardpoint_count(t->id)) ? hardpoint : -1;
    std::vector<objects::ObjectId> ids;
    for (const Unit* u : owned_units<Unit>(st, player, units, [this](objects::ObjectId id) { return unit_by_id(id); }, false))
        if (u->id != target && combat_->has_unit(u->id)) ids.push_back(u->id);
    if (ids.empty()) return R::InvalidUnit;
    if (abilities_)  // OI abilities: order cancel 0x38f5d0
        for (const objects::ObjectId id : ids) abilities_->order_cancel(id, frame_);
    if (!queued) {
        clear_queues(ids);
        raise_order_replaced(ids);  // S7 t31: signal 0xb
        team_attack_drop(ids);  // q6
        exec_attack(mode, ids, target, hp, rng);
        return R::None;
    }
    QueuedOrder q;
    q.kind = QueuedOrder::Kind::Attack;
    q.player = player;
    q.target = target;
    q.hardpoint = hp;
    const std::vector<objects::ObjectId> now = enqueue(ids, q);
    raise_order_replaced(now);  // S7 t31
    team_attack_drop(now);  // q6
    if (!now.empty()) exec_attack(mode, now, target, hp, rng);
    return R::None;
}

cmd::RejectReason TacticalBattle::order_move_to_object(TacticalMode& mode, uint8_t player, std::span<const objects::ObjectId> units,
                                                       objects::ObjectId target, SimRng& rng, bool attack_move, bool queued) {
    using R = cmd::RejectReason;
    if (finished_ || !movement_) return R::NotAllowed;
    const objects::ObjectStore& st = mode.primary();
    const objects::ObjectHandle th = st.find(target);
    if (!st.alive(th)) return R::InvalidTarget;
    std::vector<objects::ObjectId> movers;
    movers.reserve(units.size());
    for (const objects::ObjectId id : units)
        if (id != target) movers.push_back(id);
    const Vec3 p = st.position(th);
    if (!queued) return order_move(mode, player, movers, p.x, p.y, rng, mv::kNoFacing, attack_move, false);
    const auto own = owned_units<Unit>(st, player, movers, [this](objects::ObjectId id) { return unit_by_id(id); }, true);
    if (own.empty()) return R::InvalidUnit;
    std::vector<objects::ObjectId> ids;
    for (const Unit* u : own) ids.push_back(u->id);
    QueuedOrder q;
    q.kind = QueuedOrder::Kind::MoveToObject;
    q.player = player;
    q.target = target;
    q.x = p.x;  // the target's position at issue (marker); the move goes to where it is at start
    q.y = p.y;
    q.attack_move = attack_move;
    const std::vector<objects::ObjectId> now = enqueue(ids, q);
    raise_order_replaced(now);  // S7 t31: the MoveToObject executor raises 0xb
    if (!now.empty()) exec_move(mode, now, p.x, p.y, rng, mv::kNoFacing, attack_move);
    return R::None;
}

cmd::RejectReason TacticalBattle::order_stop(TacticalMode& mode, uint8_t player, std::span<const objects::ObjectId> units) {
    using R = cmd::RejectReason;
    if (finished_ || !movement_) return R::NotAllowed;
    const auto own = owned_units<Unit>(mode.primary(), player, units, [this](objects::ObjectId id) { return unit_by_id(id); }, false);
    if (own.empty()) return R::InvalidUnit;
    std::vector<objects::ObjectId> movers, all;
    for (const Unit* u : own) {
        all.push_back(u->id);
        if (u->movable) movers.push_back(u->id);
        if (combat_->has_unit(u->id)) {
            combat_->clear_order(u->id);
            combat_->set_attack_goal(u->id, tc::kNoObject);  // refixf t06
        }
    }
    clear_queues(all);  // [port: Gabriel] Stop clears the queue
    team_attack_drop(all);  // q6
    fighter_order_clear(all);  // S7 t22: Stop leaves the formation [M]
    if (!movers.empty()) movement_->stop(movers);
    return R::None;
}

// ---- [port] fleet groups (fleet_groups.hpp; docs/DECISIONS.md "Fleet groups: exception to the current focus") ----

FleetUnitInfo TacticalBattle::fleet_unit_info(const objects::ObjectStore& store, objects::ObjectId id) const {
    FleetUnitInfo out;
    const auto it = std::lower_bound(units_.begin(), units_.end(), id, [](const Unit& u, objects::ObjectId v) { return u.id < v; });
    if (it == units_.end() || it->id != id || !it->alive || it->retreated || !it->movable || !movement_ || !movement_->has_unit(id))
        return out;
    const mv::UnitView v = movement_->view(id);
    out.valid = true;
    out.owner = store.owner(it->h);
    out.pos = v.pos;
    out.yaw_deg = v.yaw;
    out.max_speed = v.max_speed;
    out.has_goal = v.has_goal;
    return out;
}

cmd::RejectReason TacticalBattle::fleet_group_command(TacticalMode& mode, uint8_t player, std::span<const uint8_t> data,
                                                      std::span<const objects::ObjectId> selection) {
    using R = cmd::RejectReason;
    if (!fleet_on_ || finished_ || !movement_) return R::NotAllowed;
    const std::optional<cmd::FleetGroupCommand> c = cmd::parse_fleet_group(data);
    if (!c) return R::Malformed;
    std::vector<objects::ObjectId> ids;
    for (const Unit* u : owned_units<Unit>(mode.primary(), player, selection, [this](objects::ObjectId id) { return unit_by_id(id); }, true))
        ids.push_back(u->id);
    const objects::ObjectStore& st = mode.primary();
    return fleet_.apply(player, *c, ids, [this, &st](objects::ObjectId id) { return fleet_unit_info(st, id); });
}

void TacticalBattle::fleet_order_move(TacticalMode& mode, uint8_t player, std::span<const objects::ObjectId> units, float x, float y,
                                      SimRng& rng, float facing_deg, bool attack_move, bool queued, uint8_t move_flags,
                                      std::vector<objects::ObjectId>& rest, uint32_t& moved) {
    moved = 0;
    rest.assign(units.begin(), units.end());
    if (!fleet_on_ || finished_ || !movement_ || queued || !std::isfinite(x) || !std::isfinite(y)) return;
    const objects::ObjectStore& st = mode.primary();
    const FleetGroups::InfoFn info = [this, &st](objects::ObjectId id) { return fleet_unit_info(st, id); };
    std::vector<FleetGroups::LockedMove> moves;
    fleet_.plan_move(player, units, {x, y}, facing_deg, info, moves, rest);
    for (const FleetGroups::LockedMove& m : moves) {
        bool any = false;
        for (const FleetGroups::MemberGoal& g : m.goals) {
            const objects::ObjectId one[1] = {g.id};
            if (order_move(mode, player, one, g.goal.x, g.goal.y, rng, m.facing_deg, attack_move, false, move_flags, false) ==
                cmd::RejectReason::None)
                any = true;
        }
        if (!any) continue;
        fleet_.start_move(m, info);
        ++moved;
    }
}

void TacticalBattle::fleet_tick(const TacticalMode& mode) {
    if (!fleet_on_ || !fleet_.used() || !movement_) return;
    const objects::ObjectStore& st = mode.primary();
    const FleetGroups::InfoFn info = [this, &st](objects::ObjectId id) { return fleet_unit_info(st, id); };
    fleet_.prune(info);
    if (!fleet_.any_moving()) return;
    fleet_.regulate(info, [this](objects::ObjectId id, float s) { movement_->set_speed_factor(id, movement_->speed_factor(id) * s); });
}

// ---- P host (owner: game host agent): human reinforcements (Reinforce, COMMANDS.md id 12) ------------------------------

float TacticalBattle::population_used(int32_t player) const {
    float used = 0.0f;
    std::vector<uint8_t> counted(parts_.size(), 0);
    for (const Unit& u : units_)
        if (u.alive && !u.retreated && parts_[u.participant].owner == player && !counted[u.participant]) {
            counted[u.participant] = 1;
            used += parts_[u.participant].pop;
        }
    return used;
}

std::vector<TacticalBattle::IconLink> TacticalBattle::icon_links() const {
    std::vector<IconLink> out;
    std::vector<objects::ObjectId> by_part(parts_.size(), objects::kNoObjectId), by_cont(containers_.size(), objects::kNoObjectId);
    for (const Unit& u : units_) {  // ascending id
        if (!u.alive || u.retreated || !u.movable) continue;
        objects::ObjectId* lead = u.container < by_cont.size()     ? &by_cont[u.container]
                                  : u.participant < by_part.size() ? &by_part[u.participant]
                                                                   : nullptr;
        if (!lead) continue;
        if (*lead == objects::kNoObjectId) *lead = u.id;
        out.push_back({u.id, *lead});
    }
    return out;
}

float TacticalBattle::population_cap(int32_t player) const {
    if (!space_ || !galaxy_ || player < 0 || static_cast<size_t>(player) >= galaxy_->players().size()) return -1.0f;
    return rules_->faction(galaxy_->players()[static_cast<size_t>(player)].faction).space_unit_cap;
}

std::vector<TacticalBattle::Reserve> TacticalBattle::reserves(int32_t player) const {
    std::vector<Reserve> out;
    for (const Participant& p : parts_) {
        if (p.owner != player || !p.reserve || p.deployed) continue;
        Reserve r;
        r.galactic_id = p.galactic_id;
        r.type = p.type;
        r.population = p.pop;
        r.units = static_cast<uint32_t>(spawn_types(p).size());
        out.push_back(r);
    }
    return out;
}

cmd::RejectReason TacticalBattle::reinforce_block(int32_t player, objects::ObjectId galactic_id) const {
    using R = cmd::RejectReason;
    for (const Participant& p : parts_) {
        if (p.galactic_id != galactic_id || p.owner != player || !p.reserve || p.deployed) continue;
        if (finished_ || !movement_ || retreat_side_ == static_cast<int32_t>(p.side) || arrival_held()) return R::NotAllowed;
        if (space_) {
            const uint32_t f = galaxy_->players()[static_cast<size_t>(player)].faction;
            float used = 0.0f;
            std::vector<uint8_t> counted(parts_.size(), 0);
            for (const Unit& u : units_)
                if (u.alive && !u.retreated && parts_[u.participant].owner == player && !counted[u.participant]) {
                    counted[u.participant] = 1;
                    used += parts_[u.participant].pop;
                }
            if (used + p.pop > rules_->faction(f).space_unit_cap) return R::QueueFull;
        } else if (!cfg_.skirmish) {
            const objects::GameObjectTypes& types = catalog_->types();
            if (p.type < types.size() && types.get_bool(p.type, "Stay_In_Transport_During_Ground_Battle")) return R::NotAllowed;
            if (galaxy_->planet_restricts(req_.info.planet, p.type)) return R::NotAllowed;
        }
        return R::None;
    }
    return R::InvalidUnit;
}

bool TacticalBattle::reinforce_point_ok(const TacticalMode& mode, int32_t player, objects::ObjectId galactic_id, float x,
                                        float y) const {
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    if (space_) return fog_reinforce_point_ok(player, x, y);
    return land_drop_valid(mode, player, galactic_id, x, y, true, catalog_->use_reinforcement_points());
}

cmd::RejectReason TacticalBattle::order_reinforce(TacticalMode& mode, uint8_t player, objects::ObjectId galactic_id, float x, float y,
                                                  SimRng& rng) {
    using R = cmd::RejectReason;
    if (finished_ || !movement_) return R::NotAllowed;
    if (!std::isfinite(x) || !std::isfinite(y)) return R::InvalidPosition;
    for (uint32_t i = 0; i < parts_.size(); ++i) {
        Participant& p = parts_[i];
        if (p.galactic_id != galactic_id || p.owner != static_cast<int32_t>(player) || !p.reserve || p.deployed) continue;
        if (retreat_side_ == static_cast<int32_t>(p.side)) return R::NotAllowed;
        if (arrival_held()) return R::NotAllowed;  // story t12: the hyperspace hold (space only)
        if (space_) {  // the unit cap rule (BattleAi::execute_order Reinforce); land: no tactical cap in the S10 battle
            const uint32_t f = galaxy_->players()[static_cast<size_t>(player)].faction;
            float used = 0.0f;
            std::vector<uint8_t> counted(parts_.size(), 0);
            for (const Unit& u : units_)
                if (u.alive && !u.retreated && parts_[u.participant].owner == static_cast<int32_t>(player) && !counted[u.participant]) {
                    counted[u.participant] = 1;
                    used += parts_[u.participant].pop;
                }
            if (used + p.pop > rules_->faction(f).space_unit_cap) return R::QueueFull;
            // battle host (fog agent ask): space drop validity 0x4d6fd0 - fogged (force 1) / inside a non-allied prevention
            // radius -> refused (TACTICAL_GAPS §4.2).
            if (!fog_reinforce_point_ok(static_cast<int32_t>(player), x, y)) return R::InvalidPosition;
        }
        if (!space_) {
            // a1h t06 (TACTICAL_GAPS §4.2.2 item 5): Execute re-runs the drop validity (force 0, landing rule on, movers
            // ignored only for an AI issuer); a refused drop is refused whole. Then the company layout around (x, y),
            // turned by the side's yaw - the slots just tested.
            const bool human = player < galaxy_->players().size() && galaxy_->players()[player].human;
            if (!land_drop_valid(mode, static_cast<int32_t>(player), galactic_id, x, y, human, catalog_->use_reinforcement_points()))
                return R::InvalidPosition;
            // a1i t05 (A1_READINGS §35.4 item 3): campaign refusals after the validity (silent): a type that stays in its
            // transport during ground battles, or one the battle planet restricts (0x373490). [port] A transport whose
            // contents hold such a type cannot be in the port's pool (it holds companies); the "battle decided" gate is [M].
            if (!cfg_.skirmish) {
                const objects::GameObjectTypes& types = catalog_->types();
                if (p.type < types.size() && types.get_bool(p.type, "Stay_In_Transport_During_Ground_Battle")) return R::NotAllowed;
                if (galaxy_->planet_restricts(req_.info.planet, p.type)) return R::NotAllowed;
            }
            const float yaw = drop_yaw(p.side);
            const LandDropShape shape = land_drop_shape(p.type);
            LandDropLayout lay = land_drop_layout(shape, x, y, yaw);
            if (lay.ok && p.other_type < 0) {
                // a1i t04 (A1_READINGS §35.3): a company with a Company_Transport_Unit comes down in its landing object at
                // (x, y); the members leave it for the unload offsets (0x643130: the team grid from 3.0 x the team
                // bounds, turned by the landing's yaw) [port: placed there at once, snapped to a passable cell of their
                // movement class; no exit move]. Without one the members drop at the validity layout's slots [port].
                Landing landing = spawn_landing(mode, p, x, y);
                landing.participant = i;
                drop_pos_.clear();
                // q6 t19 (SKIRMISH_SETUP §3.1 item 3): in skirmish a pool type without a Company_Transport_Unit cannot be
                // dropped (0x2b5fe0 returns null: no landing object, no members).
                if (landing.id == objects::kNoObjectId && cfg_.skirmish) return R::NotAllowed;
                if (landing.id != objects::kNoObjectId) {
                    lay = land_drop_layout(shape, x, y, yaw, 3.0f);
                    const objects::GameObjectTypes& types = catalog_->types();
                    for (const LandDropSlot& s : lay.slots) {
                        const mv::UnitDesc md = mv::unit_desc_from_type(types, s.type);
                        drop_pos_.push_back(snap_passable(Vec3{s.x, s.y, 0.0f}, movement_->classes().mask(md.movement_class), false));
                    }
                    // Exit targets: the step's team offset / vehicle slot, on a cell passable for its first member.
                    for (size_t g = 0, m = 0; g < lay.group_x.size(); ++g) {
                        const uint32_t t = m < lay.slots.size() ? lay.slots[m].type : 0u;
                        const mv::UnitDesc md = mv::unit_desc_from_type(types, t);
                        landing.targets.push_back(
                            snap_passable(Vec3{lay.group_x[g], lay.group_y[g], 0.0f}, movement_->classes().mask(md.movement_class), false));
                        m += shape.create_team ? std::min<size_t>(std::max<uint32_t>(shape.squad, 1u), lay.slots.size()) : 1u;
                    }
                    // s78j t01 (TACTICAL_COMBAT §3.1.1 "Units leaving a land landing object") [H], the untimed unload
                    // placing everything at once: team step j faces along its offset O_j (the layout's, before the
                    // passable-cell snap) from the landing centre, step 0 the landing's facing; a vehicle placed at its
                    // slot ends on the landing's facing (0x3a8710).
                    const Vec3 psi = mode.primary().facing(landing.h);
                    drop_face_.assign(lay.slots.size(), psi);
                    if (shape.create_team) {
                        const size_t k = std::min<size_t>(std::max<uint32_t>(shape.squad, 1u), lay.slots.size());
                        for (size_t g = 1, m = k; g < lay.group_x.size() && m < lay.slots.size(); ++g, m += k)
                            for (size_t q = m; q < std::min(m + k, lay.slots.size()); ++q)
                                drop_face_[q] = offset_facing(lay.group_x[g] - x, lay.group_y[g] - y);
                    }
                    if (cfg_.landing_unload_timed) {
                        drop_face_.clear();  // landing_service picks the facings step by step
                        // §35.2: the company stays inside; landing_service runs the state machine. The reserve leaves the
                        // pool now (deployed), its members spawn step by step.
                        landing.timed = true;
                        drop_pos_.clear();
                        p.deployed = true;
                        landings_.push_back(std::move(landing));
                        ++stats_.reinforcements;
                        return R::None;
                    }
                } else {
                    for (const LandDropSlot& s : lay.slots) drop_pos_.push_back(Vec3{s.x, s.y, 0.0f});
                }
                const size_t first = units_.size();
                deploy_unload_ = landing.id != objects::kNoObjectId;
                deploy(mode, i, rng);
                deploy_unload_ = false;
                drop_pos_.clear();
                drop_face_.clear();
                if (landing.id != objects::kNoObjectId) {
                    // [port] Every unload step runs at the order (the tactical AI takes the units at once, §35.3): the
                    // signal 0x19 record is posted now; the landing object stays for the steps' timers, then leaves.
                    LandingUnload rec;
                    rec.landing = landing.id;
                    rec.galactic_id = p.galactic_id;
                    rec.owner = p.owner;
                    rec.carried_type = p.type;
                    for (size_t k = first; k < units_.size(); ++k) rec.units.push_back(units_[k].id);
                    landings_unloaded_.push_back(std::move(rec));
                    landing.state = 3;
                    landing.remove_frame = frame_ + (static_cast<uint32_t>(landing.targets.size()) + 1u) * landing_step_frames(p.owner);
                    landings_.push_back(std::move(landing));
                }
                ++stats_.reinforcements;
                return R::None;
            }
        }
        if (space_ && p.other_type < 0) {  // a1i t05: Find_Free_Placement per member (A1_READINGS §35.4 item 4)
            std::vector<Vec3> spots;
            if (space_member_spots(mode, p, x, y, spots)) {
                drop_pos_ = std::move(spots);
                deploy(mode, i, rng);
                drop_pos_.clear();
                ++stats_.reinforcements;
                return R::None;
            }
        }
        // [port] landing position: the battle's row-of-6 slots around the requested point (clamped to the mode bounds).
        const tactical::Aabb& b = mode.bounds();
        const Vec3 save_anchor = anchor_[p.side];
        const uint32_t save_placed = placed_[p.side];
        anchor_[p.side] = {std::clamp(x, b.min.x, b.max.x), std::clamp(y, b.min.y, b.max.y), 0.0f};
        placed_[p.side] = 0;
        deploy(mode, i, rng);
        anchor_[p.side] = save_anchor;
        placed_[p.side] = save_placed;
        ++stats_.reinforcements;
        return R::None;
    }
    return R::InvalidUnit;
}

namespace bo3split {

// 0x20acd0: yaw of the vector 0 -> (x, y) in degrees, [0, 360); 0 for the zero vector.
float drop_yaw_of(float x, float y) {
    if (x == 0.0f && y == 0.0f) return 0.0f;
    float d = dmath::atan2(y, x) * 57.295776f;
    if (d < 0.0f) d += 360.0f;
    return d;
}
}  // namespace bo3split

namespace bo3split {

Off2 drop_rotate(float deg, float x, float y) {
    float sn = 0.0f, cs = 1.0f;
    dmath::sincos(deg * (dmath::kPiF / 180.0f), sn, cs);
    return {x * cs - y * sn, x * sn + y * cs};
}
}  // namespace bo3split

namespace bo3split {

// 0x40f970(cell = (a, b), n): rows of perRow slots, centred; output in world units (x 20), bounds in cell units.
bool drop_grid(float a, float b, uint32_t n, std::vector<Off2>& out, float& bx, float& by) {
    out.clear();
    if (b == 0.0f || n == 0) return false;
    const float side = std::sqrt(std::max(1.0f, static_cast<float>(n) * b * a * 1.8f));
    const float fr = side / b;
    const uint32_t per_row = fr >= 1.0f ? static_cast<uint32_t>(fr) : 1u;
    const uint32_t rows = (n + per_row - 1u) / per_row;
    float x = (static_cast<float>(rows) * a - a) / 2.0f;
    uint32_t left = n;
    for (uint32_t r = 0; r < rows; ++r) {
        const uint32_t k = std::min(per_row, left);
        const float gap = (side - static_cast<float>(k) * b) / static_cast<float>(k + 1u);
        float y = gap - side / 2.0f;
        for (uint32_t j = 0; j < k; ++j) {
            y += b / 2.0f;
            out.push_back({x * 20.0f, y * 20.0f});
            y += b / 2.0f + gap;
        }
        left -= k;
        x -= a;
    }
    bx = static_cast<float>(rows) * a / 2.0f;
    by = side / 2.0f;
    return true;
}
}  // namespace bo3split

namespace bo3split {

// 0x5abc00((ex, ey), g, s): ceil(sqrt(s)) columns along y per row, rows across x (row 0 = front, most +x), centred.
// Bounds (cell units): the slots' joint box half extents x 0.05.
void drop_team(float ex, float ey, float g, uint32_t s, std::vector<Off2>& out, float& bx, float& by) {
    out.clear();
    uint32_t c = static_cast<uint32_t>(std::sqrt(static_cast<float>(s)));
    if (c * c != s) ++c;
    if (c == 0) c = 1;
    const uint32_t rows = (s + c - 1u) / c;
    const float px = 2.0f * ex + g, py = 2.0f * ey + g;
    float lo_x = 0.0f, hi_x = 0.0f, lo_y = 0.0f, hi_y = 0.0f;
    uint32_t left = s;
    for (uint32_t r = 0; r < rows; ++r) {
        const uint32_t k = std::min(c, left);
        const float x = (static_cast<float>(rows - 1u) / 2.0f - static_cast<float>(r)) * px;
        for (uint32_t j = 0; j < k; ++j) {
            const float y = k == 1u ? 0.0f : -static_cast<float>(k - 1u) * py / 2.0f + static_cast<float>(j) * py;
            out.push_back({x, y});
            hi_x = std::max(hi_x, x + ex);
            lo_x = std::min(lo_x, x - ex);
            hi_y = std::max(hi_y, y + ey);
            lo_y = std::min(lo_y, y - ey);
        }
        left -= k;
    }
    bx = (hi_x - lo_x) / 2.0f * 0.05f;
    by = (hi_y - lo_y) / 2.0f * 0.05f;
}
}  // namespace bo3split

namespace bo3split {

bool drop_token_is(std::string_view list, std::string_view token) {  // case-insensitive token in a , / space list
    size_t i = 0;
    auto sep = [](char ch) { return ch == ',' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; };
    while (i < list.size()) {
        while (i < list.size() && sep(list[i])) ++i;
        size_t j = i;
        while (j < list.size() && !sep(list[j])) ++j;
        if (ascii_iequals(list.substr(i, j - i), token)) return true;
        i = j;
    }
    return false;
}
}  // namespace bo3split

LandDropLayout land_drop_layout(const LandDropShape& shape, float px, float py, float yaw_deg, float team_factor) {
    LandDropLayout out;
    const size_t n = shape.members.size();
    if (n == 0 || shape.hx.size() != n || shape.hy.size() != n) return out;
    const float fx = shape.hx[0], fy = shape.hy[0], fs = shape.formation_spacing;
    std::vector<Off2> local;
    float bx = 0.0f, by = 0.0f;
    if (!shape.create_team) {  // vehicles: one grid over all members, cell = (2 hx / 20 + FS, 2 hy / 20 + FS) of F
        if (!drop_grid(2.0f * fx / 20.0f + fs, 2.0f * fy / 20.0f + fs, static_cast<uint32_t>(n), local, bx, by)) return out;
    } else {
        const uint32_t sq = std::min<uint32_t>(std::max<uint32_t>(shape.squad, 1u), static_cast<uint32_t>(n));
        const uint32_t teams = (static_cast<uint32_t>(n) + sq - 1u) / sq;
        std::vector<Off2> team, offs;
        float tbx = 0.0f, tby = 0.0f;
        if (shape.container_locomotor)
            drop_team(fx, fy, 20.0f * fs, sq, team, tbx, tby);
        else if (!drop_grid(2.0f * fx / 20.0f + fs, 2.0f * fy / 20.0f + fs, sq, team, tbx, tby))
            return out;
        if (!drop_grid(team_factor * tbx, team_factor * tby, teams, offs, bx, by)) return out;
        local.resize(n);
        for (size_t i = 0; i < n; ++i) {
            const size_t t = i / sq, j = i - t * sq;
            const Off2 o = offs[t];
            const Off2 m = drop_rotate(drop_yaw_of(o[0], o[1]), team[j][0], team[j][1]);  // each team faces its offset
            local[i] = {o[0] + m[0], o[1] + m[1]};
        }
        for (uint32_t t = 0; t < teams; ++t) {  // a1i t04: one exit target per team (0x643130)
            const Off2 r = drop_rotate(yaw_deg, offs[t][0], offs[t][1]);
            out.group_x.push_back(px + r[0]);
            out.group_y.push_back(py + r[1]);
        }
    }
    out.half_x = bx * 20.0f;
    out.half_y = by * 20.0f;
    out.slots.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const Off2 r = drop_rotate(yaw_deg, local[i][0], local[i][1]);
        LandDropSlot& sl = out.slots[i];
        sl.type = shape.members[i];
        sl.x = px + r[0];
        sl.y = py + r[1];
        sl.hx = shape.hx[i];
        sl.hy = shape.hy[i];
        sl.yaw_deg = drop_yaw_of(r[0], r[1]);
        if (!shape.create_team) {  // vehicles: one exit target per member
            out.group_x.push_back(sl.x);
            out.group_y.push_back(sl.y);
        }
    }
    out.ok = true;
    return out;
}

bool drop_boxes_overlap(const DropBox& a, const DropBox& b) {
    const Off2 ax = drop_rotate(a.yaw_deg, 1.0f, 0.0f), ay = drop_rotate(a.yaw_deg, 0.0f, 1.0f);
    const Off2 bu = drop_rotate(b.yaw_deg, 1.0f, 0.0f), bv = drop_rotate(b.yaw_deg, 0.0f, 1.0f);
    const float dx = b.cx - a.cx, dy = b.cy - a.cy;
    const Off2 axes[4] = {ax, ay, bu, bv};
    for (const Off2& u : axes) {
        const float ra = a.hx * std::fabs(ax[0] * u[0] + ax[1] * u[1]) + a.hy * std::fabs(ay[0] * u[0] + ay[1] * u[1]);
        const float rb = b.hx * std::fabs(bu[0] * u[0] + bu[1] * u[1]) + b.hy * std::fabs(bv[0] * u[0] + bv[1] * u[1]);
        if (std::fabs(dx * u[0] + dy * u[1]) >= ra + rb) return false;  // separating axis
    }
    return true;
}

bool land_drop_radius_ok(float x, float y, std::span<const LandDropRadiusObject> objects, bool use_reinforcement_points) {
    bool enabled = !use_reinforcement_points;
    for (const LandDropRadiusObject& o : objects) {
        const float dx = x - o.x, dy = y - o.y, d2 = dx * dx + dy * dy;
        if (o.allied) {
            if (o.enabling > 0.0f && d2 <= o.enabling * o.enabling) enabled = true;
        } else if (o.prevention > 0.0f && d2 < o.prevention * o.prevention && (!o.powered || o.owner_base_power)) {
            return false;  // prevention: a POWERED object blocks only while its owner has base power
        }
    }
    return enabled;
}

void drop_half_extents(const objects::GameObjectTypes& types, const BattleCatalog* catalog, uint32_t type, float& hx, float& hy) {
    hx = hy = 0.0f;
    if (type >= types.size()) return;
    float sc = types.get_float(type, "Scale_Factor");
    if (!(sc > 0.0f)) sc = 1.0f;
    const float cx = types.get_float(type, "Custom_Hard_XExtent"), cy = types.get_float(type, "Custom_Hard_YExtent");
    if (cx > 0.0f && cy > 0.0f) {
        hx = cx * sc;
        hy = cy * sc;
        return;
    }
    // s78n t10 (TACTICAL_COMBAT §5.4.6 item 1 and port spec item 2, 0x371be0 with the land model; replaces a1h t06's
    // |max| of the bind-pose box with a fallback to the other modes' models): no land model (0x3729f0 reads the land slot
    // only) -> (0, 0), even with one custom set; else per axis the hard extent when > 0, else the |collision-bound half|
    // of the land model. [port] a named land model that does not load (no art data, or no catalog): per axis the hard
    // extent, else 0.
    if (ascii_trim(types.get_string(type, "Land_Model_Name")).empty()) return;
    const tactical::movement::AvoidModel* am = catalog ? catalog->avoid_model(type, false) : nullptr;
    hx = (cx > 0.0f ? cx : am ? std::fabs(am->bound_h.x) : 0.0f) * sc;
    hy = (cy > 0.0f ? cy : am ? std::fabs(am->bound_h.y) : 0.0f) * sc;
}

void drop_deployed_extents(const objects::GameObjectTypes& types, const BattleCatalog* catalog, uint32_t type, float& hx, float& hy) {
    drop_half_extents(types, catalog, type, hx, hy);
    if (type >= types.size()) return;
    const float dx = types.get_float(type, "Custom_Hard_XExtent_Deployed");
    if (!(dx > 0.0f)) return;  // 0x371d20: the deployed pair applies only when the deployed x extent is set
    float sc = types.get_float(type, "Scale_Factor");
    if (!(sc > 0.0f)) sc = 1.0f;
    hx = dx * sc;
    hy = types.get_float(type, "Custom_Hard_YExtent_Deployed") * sc;  // also when <= 0
}

LandDropShape land_drop_shape(const objects::GameObjectTypes& types, const BattleCatalog* catalog, const galactic::ConflictRules& rules,
                              uint32_t company_type) {
    LandDropShape s;
    const galactic::ConflictTypeInfo& ti = rules.type(company_type);
    if (ti.members.empty()) {
        s.members.push_back(company_type);
    } else {
        for (const auto& [mt, n] : ti.members)
            for (int32_t i = 0; i < n; ++i) s.members.push_back(mt);
    }
    if (s.members.empty()) return s;
    if (const uint32_t f = s.members.front(); f < types.size()) s.create_team = types.get_bool(f, "Create_Team");  // original member
    // a1h t06 (TACTICAL_GAPS §4.2.2 item 2): a member without a land model that names a Unique_Ground_Unit (+0x1b60; FoC:
    // General_Veers -> Veers_AT_AT_Walker) is laid out, passability-tested and box-tested as that type.
    auto trimmed = [](std::string_view v) {
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '\r' || v.front() == '\n')) v.remove_prefix(1);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r' || v.back() == '\n')) v.remove_suffix(1);
        return v;
    };
    for (uint32_t& t : s.members) {
        if (t >= types.size() || !trimmed(types.get_string(t, "Land_Model_Name")).empty()) continue;
        const std::string_view name = trimmed(types.get_string(t, "Unique_Ground_Unit"));
        const objects::TypeIndex u = name.empty() ? objects::kNone : types.resolve_name(name);
        if (u != objects::kNone && u < types.size()) t = u;
    }
    for (const uint32_t t : s.members) {
        float hx = 0.0f, hy = 0.0f;
        drop_half_extents(types, catalog, t, hx, hy);
        s.hx.push_back(hx);
        s.hy.push_back(hy);
    }
    if (const uint32_t f = s.members.front(); f < types.size()) s.formation_spacing = types.get_float(f, "FormationSpacing");
    if (company_type < types.size()) {
        const int32_t k = types.get_int(company_type, "Max_Squad_Size");  // +0x1e44, default 10 (bt_team)
        s.squad = k > 0 ? static_cast<uint32_t>(k) : 10u;
        std::string_view name = types.get_string(company_type, "Create_Team_Type");
        while (!name.empty() && (name.front() == ' ' || name.front() == '\t' || name.front() == '\r' || name.front() == '\n')) name.remove_prefix(1);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t' || name.back() == '\r' || name.back() == '\n')) name.remove_suffix(1);
        const objects::TypeIndex tt = name.empty() ? objects::kNone : types.resolve_name(name);
        // LandBehavior (+0x718) lists LAND_TEAM_CONTAINER_LOCOMOTOR (its behaviour id is shared with the other
        // locomotors, so the list text is read).
        if (tt != objects::kNone && tt < types.size())
            s.container_locomotor = drop_token_is(types.get_string(tt, "LandBehavior"), "LAND_TEAM_CONTAINER_LOCOMOTOR");
    }
    return s;
}

LandDropShape TacticalBattle::land_drop_shape(uint32_t company_type) const {
    return sim::land_drop_shape(catalog_->types(), catalog_, *rules_, company_type);
}

void TacticalBattle::drop_half_extents(uint32_t type, float& hx, float& hy) const {
    sim::drop_half_extents(catalog_->types(), catalog_, type, hx, hy);
}

bool TacticalBattle::mv_squad_add(TacticalMode& mode, size_t ci, uint32_t object_type) {
    // s78n t03 (TACTICAL_COMBAT §5.4.4 item 2, MOVEMENT §3.7, FIGHTER_FLYING §7): the LandTeamContainer locomotor. The
    // container (created at its members' mean with the first member's facing) becomes a mover of its own: free-position
    // search at add_unit, its own path and walk at the team speed (apply_speed_factors), its members steering to slots.
    if (space_ || !movement_ || ci >= containers_.size()) return false;
    const objects::GameObjectTypes& types = catalog_->types();
    if (object_type >= types.size()) return false;
    const Container& ct = containers_[ci];
    std::vector<mv::ObjectId> ids;
    std::vector<const Unit*> mem;
    for (const Unit& u : units_) {  // ascending id = the chunk's list order (team index order)
        if (u.container != ci) continue;
        if (!u.alive || u.retreated || !u.movable || !movement_->has_unit(u.id) || u.type >= types.size()) return false;
        if (movement_->view(u.id).locomotor != mv::LocomotorKind::Default || types.get_bool(u.type, "Uses_Multiple_Locomotors"))
            return false;
        ids.push_back(u.id);
        mem.push_back(&u);
    }
    if (ids.empty()) return false;
    // Slots (pack +0x114 / +0x118): the drop layout 0x5abc00 with the first member's half-extents and g = 20 x its
    // FormationSpacing (TACTICAL_GAPS §4.2.1), x along the container's facing, y across. s78n t14 (FIGHTER_FLYING §7.2): it
    // only seeds set_land_team - the container's first tick lays the slots out by its ContainerArrangement (Rows 0x5ac970 /
    // Dartboard 0x5ac2a0, MovementSystem::lay_out_land_team) with its own facing.
    const uint32_t f = mem.front()->type;
    float fx = 0.0f, fy = 0.0f;
    type_half_extents(f, fx, fy);
    std::vector<Off2> offs;
    float bx = 0.0f, by = 0.0f;
    drop_team(fx, fy, 20.0f * types.get_float(f, "FormationSpacing"), static_cast<uint32_t>(ids.size()), offs, bx, by);
    if (offs.size() != ids.size()) return false;
    std::vector<mv::V2> slots(ids.size());
    std::vector<ta::SquadSlot> box(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        slots[i] = {offs[i][0], offs[i][1]};
        box[i].along = offs[i][0];
        box[i].side = offs[i][1];
        unit_half_extents(*mem[i], box[i].hx, box[i].hy);
    }
    float jx = 0.0f, jy = 0.0f;
    ta::squad_joint_half_extents(box, types.get_float(object_type, "MaxJiggleDistance"), jx, jy);
    const objects::ObjectStore& st = mode.primary();
    const Vec3 p = st.position(ct.h);
    mv::UnitDesc d = mv::unit_desc_from_type(types, object_type);
    const mv::UnitDesc fm = mv::unit_desc_from_type(types, f);
    d.id = ct.id;
    d.pos = {p.x, p.y};
    d.z = p.z;
    d.yaw_deg = yaw_of(st.facing(ct.h));
    d.owner = ct.owner;
    d.team_container = true;
    d.team_member = false;
    d.locomotor = mv::LocomotorKind::Default;
    d.uses_multiple_locomotors = d.has_secondary_locomotor = false;
    // s78n t14 part B (FIGHTER_FLYING §7.5 item 1): the container keeps its type's UnitCollisionClass, Is_Squashable and
    // MovementPredictionInterval - it is a land collision checker / partner, and a crusher pushes it aside (crush-flee).
    d.crusher = d.supercrusher = false;
    d.turret = false;
    d.movement_class = fm.movement_class;  // [port] the container type's +0x1688 [M]: the first member's class
    // s78n t20 (MOVEMENT §5.4.4 item 3): the container TYPE's extents 0x371be0 - FoC's team container types set no hard
    // extents and [port] their models are not checked: (0, 0)
    d.type_hx = d.type_hy = 0.0f;
    // The formation layout reads the container's hard radius 0x397780: sqrt(hx^2 + hy^2) of the joint box.
    const float r = dmath::sqrt(jx * jx + jy * jy);
    if (r > 0.0f) d.radius = r;
    if (types.get_string(object_type, "Max_Speed").empty()) {  // [port] untagged: the members' minimum (the team cap rules)
        float mn = 3.0e38f;
        for (const Unit* u : mem) mn = std::min(mn, mv::unit_desc_from_type(types, u->type).max_speed);
        d.max_speed = mn;
    }
    if (!movement_->add_unit(d)) return false;
    if (!movement_->set_land_team(ct.id, ids, slots)) {
        (void)movement_->remove_unit(ct.id);
        return false;
    }
    for (const Unit* u : mem) mv_team_shape(*u);  // s78n t14: the layout's per-member half-extents
    const mv::UnitView v = movement_->view(ct.id);
    if (v.pos.x != p.x || v.pos.y != p.y)  // the free-position search moved it off a blocked centroid
        mode.primary().set_position(ct.h, Vec3{v.pos.x, v.pos.y, map_.terrain.sample_height(v.pos.x, v.pos.y)});
    return true;
}

void TacticalBattle::mv_team_shape(const Unit& u) {
    // s78n t14 (FIGHTER_FLYING §7.2): Rows reads each member's half-extents 0x396b70 / 0x396a30 (unit_half_extents),
    // Dartboard member 0's type half-extents 0x371be0 (type_half_extents).
    if (space_ || !movement_ || !movement_->has_unit(u.id)) return;
    float hx = 0.0f, hy = 0.0f, thx = 0.0f, thy = 0.0f;
    unit_half_extents(u, hx, hy);
    type_half_extents(u.type, thx, thy);
    (void)movement_->set_team_member_shape(u.id, hx, hy, thx, thy);
}

float TacticalBattle::drop_yaw(uint8_t side) const {
    if (side >= 2) return 0.0f;  // third players: no record written
    const Vec3 f = drop_facing_[side];
    return f.x == 0.0f && f.y == 0.0f ? 0.0f : yaw_of(f);
}

Vec3 TacticalBattle::player_drop_facing(int32_t player) const {
    for (uint8_t side = 0; side < 2; ++side) {
        if (side_player(side) != player) continue;
        const Vec3 f = drop_facing_[side];
        return f.x == 0.0f && f.y == 0.0f ? facing_of_yaw(0.0f) : f;
    }
    return facing_of_yaw(0.0f);  // a third player: no record written (yaw 0)
}

// a1i t04 (A1_READINGS §35.1 / §35.3): the landing object of a land reserve drop. L = the company's
// Company_Transport_Unit[0], then its Landing_Transport_Variant when set; none -> no landing object (kNoObjectId). Created
// at (x, y), z = terrain + L's Hover_Offset [the +0xd8 vehicle-height term is [M]], facing = the side's entry-facing
// record, parent id = the reserve's galactic id; TRANSPORT_LANDING attached, carried type = the company type. Removal
// frame: the unload steps (one per team of min(Max_Squad_Size, n) Create_Team members or per other member) plus the finish
// step, each 2 s x (1 + s), s = the owner's Reinforcement_Deploy_Time_Multiplier list (0x4ad3a0: (int)(30 x seconds));
// [port] the Land / Takeoff clips take no time and there is no clearance wait (§35.2 state 4).
TacticalBattle::Landing TacticalBattle::spawn_landing(TacticalMode& mode, const Participant& p, float x, float y) {
    Landing out;
    const objects::GameObjectTypes& types = catalog_->types();
    if (p.type >= types.size()) return out;
    const std::vector<objects::TypeIndex> tr = types.get_type_refs(p.type, "Company_Transport_Unit");
    objects::TypeIndex l = tr.empty() ? objects::kNone : tr.front();  // the first entry (0x400f30)
    if (l == objects::kNone || l >= types.size()) return out;
    std::string_view vname = types.get_string(l, "Landing_Transport_Variant");  // one name (+0x1e68)
    while (!vname.empty() && std::isspace(static_cast<unsigned char>(vname.front())) != 0) vname.remove_prefix(1);
    while (!vname.empty() && std::isspace(static_cast<unsigned char>(vname.back())) != 0) vname.remove_suffix(1);
    if (const objects::TypeIndex v = vname.empty() ? objects::kNone : types.resolve_name(vname); v != objects::kNone && v < types.size())
        l = v;
    objects::SpawnParams sp;
    sp.type = l;
    sp.owner = p.owner;
    sp.position = {x, y, map_.terrain.sample_height(x, y) + types.get_float(l, "Hover_Offset")};
    const Vec3 f = p.side < 2 ? drop_facing_[p.side] : Vec3{};
    sp.facing = f.x == 0.0f && f.y == 0.0f ? facing_of_yaw(0.0f) : f;
    sp.parent_mode_id = p.built ? objects::kNoObjectId : p.galactic_id;  // q6 t19: tactical production pool entries
    const objects::ObjectHandle h = mode.spawn(sp, 1.0f);
    objects::ObjectStore& st = mode.primary();
    st.add_behavior(h, objects::beh::TransportLanding);
    st.set_carried_type(h, p.type);
    out.id = st.id(h);
    out.h = h;
    return out;
}

uint32_t TacticalBattle::landing_step_frames(int32_t player) const {
    const float s = abilities_ ? abilities_->player_list(player, tactical::abilities::PlayerList::ReinforcementDeploy) : 0.0f;
    const float secs = 2.0f * (1.0f + s);
    return secs > 0.0f ? static_cast<uint32_t>(30.0f * secs) : 0u;  // 0x4ad3a0: (int)(FPS x seconds)
}

bool TacticalBattle::landing_hidden(objects::ObjectId landing) const {
    for (const Landing& l : landings_)
        if (l.id == landing) return l.state == 4;
    return false;
}

uint32_t TacticalBattle::landings_in_flight(int32_t player) const {
    uint32_t n = 0;
    for (const Landing& l : landings_)
        if (l.timed && l.state != 3 && parts_[l.participant].owner == player) ++n;
    return n;
}

bool TacticalBattle::landing_locked(objects::ObjectId id) const {
    for (const Landing& l : landings_)
        if (l.timed && l.state == 2 && std::find(l.units.begin(), l.units.end(), id) != l.units.end()) return true;
    return false;
}

// a1i t04 (A1_READINGS §35.2, timed mode): state 4 waits (hidden) until no other landing in state 1 / 2 overlaps its
// footprint (drop_half_extents box along the yaw [port: the footprint AABB with z +-100 of 0x38de80 as an oriented box; only
// this battle's landings are seen]); state 1 [port: the Land clip takes no time] turns into state 2 with the step timer;
// each expiry spawns one team chunk (min(k, members left)) or one vehicle at the landing position (deploy member range)
// and sends it on a group move to its exit target [port: no forced-move flag in the movement system], locked
// (landing_locked); after the last member the next expiry finishes: unlock, the signal 0x19 record (landings_unloaded),
// state 3 and the take-off [port: no Takeoff clip: removed at once]. Untimed landings are only removed at their frame.
void TacticalBattle::landing_service(TacticalMode& mode, SimRng& rng) {
    objects::ObjectStore& store = mode.primary();
    const objects::GameObjectTypes& types = catalog_->types();
    auto footprint = [&](const Landing& l) {
        DropBox b;
        const Vec3 q = store.position(l.h), f = store.facing(l.h);
        b.cx = q.x;
        b.cy = q.y;
        b.yaw_deg = f.x == 0.0f && f.y == 0.0f ? 0.0f : yaw_of(f);
        sim::drop_half_extents(types, catalog_, store.type(l.h), b.hx, b.hy);
        return b;
    };
    auto unloaded = [&](const Landing& l) {  // 0x6438a0: the signal 0x19 record (ReinforcementsUnloadedDataClass)
        const Participant& P = parts_[l.participant];
        LandingUnload rec;
        rec.landing = l.id;
        rec.galactic_id = P.galactic_id;
        rec.owner = P.owner;
        rec.carried_type = P.type;
        rec.units = l.units;
        landings_unloaded_.push_back(std::move(rec));
    };
    for (size_t li = 0; li < landings_.size(); ++li) {
        Landing& l = landings_[li];
        if (!store.alive(l.h)) {
            // a1j t01 (A1_READINGS §36): a timed landing destroyed before its finish. Its behaviour's detach (vf15 0x6442d0,
            // run when the object is deleted) finishes the unload when a unit is out (+0xb0 != 0): unlock, signal 0x19 with
            // the units out so far. With none out the original sends nothing and the Reinforce status waits forever
            // [port: the record is sent with no units, so the status's in-flight count still reaches 0]. The members not
            // yet out are lost.
            if (l.timed && l.state != 3) unloaded(l);
            l.units.clear();
            l.state = 3;
            l.remove_frame = 0;
            continue;
        }
        if (!l.timed) continue;
        const Participant& P = parts_[l.participant];
        if (l.state == 4) {
            bool blocked = false;
            const DropBox mine = footprint(l);
            for (size_t o = 0; o < landings_.size() && !blocked; ++o)
                if (o != li && (landings_[o].state == 1 || landings_[o].state == 2) && store.alive(landings_[o].h))
                    blocked = drop_boxes_overlap(mine, footprint(landings_[o]));
            if (blocked) continue;
            l.state = 1;  // 0x642c70: shown, landing dust [port: no particle]
        }
        if (l.state == 1) {  // [port] the Land clip passes 98 % at once
            l.state = 2;
            l.timer_start = frame_;
            l.timer_len = landing_step_frames(P.owner);
        }
        if (l.state != 2 || frame_ < l.timer_start + l.timer_len) continue;
        const std::vector<uint32_t> members = spawn_types(P);
        const auto n = static_cast<uint32_t>(members.size());
        if (l.unloaded >= n) {  // 0x6438a0: finish
            unloaded(l);
            l.units.clear();  // unlocked
            l.state = 3;
            l.remove_frame = frame_;
            continue;
        }
        // 0x644330: one team chunk or one vehicle at the landing position.
        const int32_t mss = types.get_int(P.type, "Max_Squad_Size");
        const uint32_t k = std::min(mss > 0 ? static_cast<uint32_t>(mss) : 10u, std::max(n, 1u));
        const uint32_t i = l.unloaded;
        const uint32_t cnt = members[i] < types.size() && types.get_bool(members[i], "Create_Team") ? std::min(k, n - i) : 1u;
        const Vec3 at = store.position(l.h);
        drop_pos_.assign(n, Vec3{at.x, at.y, 0.0f});
        // s78j t01 (TACTICAL_COMBAT §3.1.1 "Units leaving a land landing object") [H]: step j's units are created at the
        // landing centre facing along the step's offset O_j (0x20acd0((0, 0, 0), O_j)); a team's step 0 takes the
        // landing's own facing instead (vehicles have no step-0 override). O_j = the unload layout's step offset before the
        // passable-cell snap, rebuilt here from the landing position and the side's drop yaw (as order_reinforce built it).
        const Vec3 psi = store.facing(l.h);
        const bool team = cnt > 1 || (members[i] < types.size() && types.get_bool(members[i], "Create_Team"));
        {
            const LandDropLayout lay = land_drop_layout(land_drop_shape(P.type), at.x, at.y, drop_yaw(P.side), 3.0f);
            const uint32_t j = l.next_offset;
            Vec3 f = psi;
            if (lay.ok && j < lay.group_x.size() && !(team && j == 0)) f = offset_facing(lay.group_x[j] - at.x, lay.group_y[j] - at.y);
            drop_face_.assign(n, f);
        }
        const size_t first = units_.size();
        const uint32_t pi = l.participant;  // deploy may grow parts_-held state; keep the index
        deploy(mode, pi, rng, i, cnt);
        drop_pos_.clear();
        drop_face_.clear();
        Landing& lr = landings_[li];
        mv::MoveOrder o;
        for (size_t u = first; u < units_.size(); ++u) {
            lr.units.push_back(units_[u].id);
            if (units_[u].movable) o.units.push_back(units_[u].id);
        }
        if (!o.units.empty() && lr.next_offset < lr.targets.size()) {
            o.destination = {lr.targets[lr.next_offset].x, lr.targets[lr.next_offset].y};
            // s78k t01 (TACTICAL_COMBAT §3.1.1 "Units leaving a land landing object", MOVEMENT §6.1 / §3.5.1) [H]: the exit
            // move has flags 0x20 (a straight 2-point path without passability checks); a vehicle's block carries the
            // landing's yaw as its final facing (+0x5c = 1), so it turns to psi on arrival when more than 10 degrees off;
            // a team's block has none (+0x5c = 0) and keeps its travel heading.
            o.flags = mv::kMoveFlagSkipValidation;
            if (!team) o.final_facing_deg = yaw_of(psi);
            movement_->move(o, rng, frame_);
            ++stats_.orders;
        }
        lr.unloaded += cnt;
        ++lr.next_offset;
        lr.timer_start = frame_;
        lr.timer_len = landing_step_frames(P.owner);
    }
    // Take-off: removed at the frame (untimed: after the steps' timers; timed: at the finish).
    size_t w = 0;
    for (size_t i = 0; i < landings_.size(); ++i) {
        if (landings_[i].state == 3 && frame_ >= landings_[i].remove_frame) {
            if (store.alive(landings_[i].h)) mode.destroy(landings_[i].h);
            continue;
        }
        if (w != i) landings_[w] = std::move(landings_[i]);
        ++w;
    }
    landings_.resize(w);
}

bool find_free_placement(float px, float py, const FreePlacementBox& local, std::span<const FreePlacementBox> blockers, float radius,
                         float& out_x, float& out_y, float start_deg, const std::function<bool(float x, float y)>& accept) {
    const float step = 1.2f * std::max(std::fabs(local.max_x - local.min_x), std::fabs(local.max_y - local.min_y));
    for (float r = 0.0f; r < radius; r += step) {
        for (int32_t k = 0; k <= 16; ++k) {
            float sn = 0.0f, cs = 1.0f;
            dmath::sincos((start_deg + static_cast<float>(k) * 22.5f) * (dmath::kPiF / 180.0f), sn, cs);
            const float qx = px + r * cs, qy = py + r * sn;
            const FreePlacementBox q{qx + local.min_x, qy + local.min_y, qx + local.max_x, qy + local.max_y};
            bool blocked = accept && !accept(qx, qy);
            for (const FreePlacementBox& o : blockers)
                if (q.min_x < o.max_x && o.min_x < q.max_x && q.min_y < o.max_y && o.min_y < q.max_y) {
                    blocked = true;
                    break;
                }
            if (!blocked) {
                out_x = qx;
                out_y = qy;
                return true;
            }
            if (r == 0.0f) break;  // the centre is one test
        }
        if (!(step > 0.0f)) break;
    }
    return false;
}

bool TacticalBattle::space_member_spots(TacticalMode& mode, const Participant& p, float x, float y, std::vector<Vec3>& out) const {
    out.clear();
    const objects::GameObjectTypes& types = catalog_->types();
    const std::vector<uint32_t> members = spawn_types(p);
    const Vec3 at{x, y, 0.0f};
    if (members.size() <= 1) {  // a single unit: at P
        out.assign(members.size(), at);
        return true;
    }
    auto has_model = [&](uint32_t t) {
        std::string_view v = types.get_string(t, "Space_Model_Name");
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '\r' || v.front() == '\n')) v.remove_prefix(1);
        return !v.empty();
    };
    constexpr uint64_t kLayer20 = 0x20;  // Space_Layer 0x20: placed at P, never a blocker
    // Member boxes first: a member whose model is named but not loaded sends the whole drop to the rows of 6 [port].
    std::vector<FreePlacementBox> local(members.size());
    std::vector<uint8_t> at_p(members.size(), 0);
    for (size_t i = 0; i < members.size(); ++i) {
        const uint32_t t = members[i];
        if (t >= types.size() || !has_model(t) || types.get_mask(t, "Space_Layer") == kLayer20) {
            at_p[i] = 1;
            continue;
        }
        const BattleCatalog::ModelInfo* m = catalog_->model_info(t, true);
        if (!m || !m->has_bounds) return false;
        local[i] = {m->min[0], m->min[1], m->max[0], m->max[1]};
    }
    // Blockers: the object tree (all owners): live, not dying, not Is_Decoration, no TRANSPORT_LANDING / LAND_OBSTACLE /
    // PROJECTILE, Space_Layer != 0x20, with a model; their tree box in x / y [M: the type byte +0x3e test is not applied].
    const objects::ObjectStore& st = mode.primary();
    std::vector<FreePlacementBox> blockers;
    for (const objects::ObjectHandle h : st.all_objects()) {
        if (!st.alive(h) || st.dying(h)) continue;
        const objects::TypeIndex t = st.type(h);
        if (t >= types.size() || types.get_bool(t, "Is_Decoration") || !has_model(t)) continue;
        if (st.has_behavior(h, objects::beh::TransportLanding) || st.has_behavior(h, objects::beh::LandObstacle) ||
            st.has_behavior(h, objects::beh::Projectile))
            continue;
        if (types.get_mask(t, "Space_Layer") == kLayer20) continue;
        const tactical::Aabb b = mode.bounds_of(h);
        blockers.push_back({b.min.x, b.min.y, b.max.x, b.max.y});
    }
    // Members one at a time: each placed member blocks the later ones.
    for (size_t i = 0; i < members.size(); ++i) {
        if (at_p[i]) {
            out.push_back(at);
            continue;
        }
        float qx = 0.0f, qy = 0.0f;
        if (!find_free_placement(x, y, local[i], blockers, 2500.0f, qx, qy)) {
            qx = x;  // none: (0, 0, 0), replaced by P (z 0)
            qy = y;
        }
        out.push_back(Vec3{qx, qy, 0.0f});
        blockers.push_back({qx + local[i].min_x, qy + local[i].min_y, qx + local[i].max_x, qy + local[i].max_y});
    }
    return true;
}

// ---- land drop validity (header; TACTICAL_GAPS §4.2.1 tests 1-4, §4.2.2 items 2-4) ----
bool TacticalBattle::land_drop_valid(const TacticalMode& mode, int32_t player, objects::ObjectId key, float x, float y, bool human,
                                     bool use_reinforcement_points) const {
    if (space_ || !movement_) return true;  // land only
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    const Participant* part = nullptr;
    for (const Participant& q : parts_)
        if (q.galactic_id == key && q.owner == player && q.reserve && !q.deployed) {
            part = &q;
            break;
        }
    if (!part) return false;
    const float yaw = drop_yaw(part->side);
    const LandDropLayout lay = land_drop_layout(land_drop_shape(part->type), x, y, yaw);
    if (!lay.ok) return false;
    const objects::GameObjectTypes& types = catalog_->types();
    const objects::ObjectStore& st = mode.primary();
    const mv::PassabilityGrid& grid = movement_->grid();
    float mn[2], mx[2];
    const bool box = map_.playable_box(mn, mx);
    // 1-2. passable for the member's movement class (inside the playable box [port]); not fogged for the player.
    for (const LandDropSlot& s : lay.slots) {
        if (box && (s.x < mn[0] || s.x > mx[0] || s.y < mn[1] || s.y > mx[1])) return false;
        if (s.type >= types.size()) return false;
        const mv::UnitDesc md = mv::unit_desc_from_type(types, s.type);
        if (!grid.passable_world({s.x, s.y}, movement_->classes().mask(md.movement_class))) return false;
        if (fog_point_fogged(player, s.x, s.y, true)) return false;
    }
    // 3. footprints: the formation box (broad phase), then each slot's own box against each blocking object's box.
    static const objects::BehaviorTypeId kMine = objects::behavior_type_id("PROXIMITY_MINE");
    static const objects::BehaviorTypeId kLanding = objects::behavior_type_id("TRANSPORT_LANDING");
    const DropBox formation{x, y, lay.half_x, lay.half_y, yaw};
    std::vector<LandDropRadiusObject> radius;
    for (const objects::ObjectHandle h : st.all_objects()) {  // GOM order
        if (!st.alive(h)) continue;
        const objects::TypeIndex t = st.type(h);
        if (t >= types.size()) continue;
        const int32_t o = st.owner(h);
        const Vec3 q = st.position(h);
        // 4. radius inputs (GOM list +0x2f0): dying / removing (+0x3a0 & 0x42) skipped.
        if (st.active(h)) {
            LandDropRadiusObject ro;
            ro.x = q.x;
            ro.y = q.y;
            ro.allied = o >= 0 && (o == player || !combat_->is_enemy(player, o));  // 0x2823c0 / 0x39a230
            ro.enabling = types.get_float(t, "Reinforcement_Enabling_Radius");
            ro.prevention = types.get_float(t, "Reinforcement_Prevention_Radius");
            ro.powered = st.has_behavior(h, objects::beh::Powered);
            ro.owner_base_power = o >= 0 && combat_->has_base_power(o);
            if (ro.enabling > 0.0f || ro.prevention > 0.0f) radius.push_back(ro);
        }
        // a1h t06 (§4.2.2 item 4): the object tree GOM +0xd8 - Collidable_By_Projectile_Living (a death clone: _Dead),
        // any owner; never proximity mines; an AI issuer ignores movers; a human issuer ignores landing transports (item 3).
        if (!types.get_bool(t, st.dying(h) ? "Collidable_By_Projectile_Dead" : "Collidable_By_Projectile_Living")) continue;
        if (kMine != objects::kNoBehavior && st.has_behavior(h, kMine)) continue;
        const bool obstacle = st.has_behavior(h, objects::beh::LandObstacle);
        if (!human && st.has_behavior(h, objects::beh::Locomotor) && !obstacle) continue;
        // a1i t05 (A1_READINGS §35.4 item 2): a persistent cinematic landing (pack +0xc9) still blocks a human drop.
        if (human && kLanding != objects::kNoBehavior && st.has_behavior(h, kLanding) && !st.landing_persist(h)) continue;
        // Footprint 0x38de80: centre + R(yaw)(Custom_Hard_X/YExtent_Offset) x Scale_Factor, half extents 0x371be0, along yaw.
        const Vec3 f = st.facing(h);
        DropBox xb;
        xb.yaw_deg = f.x == 0.0f && f.y == 0.0f ? 0.0f : yaw_of(f);
        float sc = types.get_float(t, "Scale_Factor");
        if (!(sc > 0.0f)) sc = 1.0f;
        const float ox = types.get_float(t, "Custom_Hard_XExtent_Offset") * sc, oy = types.get_float(t, "Custom_Hard_YExtent_Offset") * sc;
        float sn = 0.0f, cs = 1.0f;
        dmath::sincos(xb.yaw_deg * (dmath::kPiF / 180.0f), sn, cs);
        xb.cx = q.x + ox * cs - oy * sn;
        xb.cy = q.y + ox * sn + oy * cs;
        // a1i t05 (§35.4 item 1, 0x396850 rule 2): a Deploys type with a locomotor while it deploys / is deployed uses
        // its deployed extents. s78n t10 (TACTICAL_COMBAT §5.4.6 item 4.3): Walk state 0x18 Deploying or 0x19 Deployed of
        // a movement-backed unit (as unit_half_extents), else [port] DEPLOY active. [M, not ported] a land squad
        // container's joint box as a blocker (whether 0x3b6170's tree query returns containers was not traced).
        bool dep = false;
        if (types.get_bool(t, "Deploys") && st.has_behavior(h, objects::beh::Locomotor)) {
            if (movement_ && movement_->has_unit(st.id(h))) {
                const mv::LocoState ds = movement_->deploy_state(st.id(h));
                dep = ds == mv::LocoState::WalkDeploying || ds == mv::LocoState::WalkDeployed;
            } else {
                dep = abilities_ && abilities_->deployed(st.id(h));
            }
        }
        if (dep)
            sim::drop_deployed_extents(types, catalog_, t, xb.hx, xb.hy);
        else
            sim::drop_half_extents(types, catalog_, t, xb.hx, xb.hy);
        if (!drop_boxes_overlap(formation, xb)) continue;
        for (const LandDropSlot& s : lay.slots)
            if (drop_boxes_overlap(DropBox{s.x, s.y, s.hx, s.hy, s.yaw_deg}, xb)) return false;  // footprint taken
    }
    // 4. reinforcement radii (check-radius: AI and human drops are not forced).
    for (const LandDropSlot& s : lay.slots)
        if (!land_drop_radius_ok(s.x, s.y, radius, use_reinforcement_points)) return false;
    return true;
}

// ---- end a1g t04 follow-up ----


// ---- [port: Gabriel] queueable unit actions: order queue ----------------------------------------------------------

std::span<const TacticalBattle::QueuedOrder> TacticalBattle::queued_orders(objects::ObjectId unit) const {
    const auto it = std::lower_bound(queues_.begin(), queues_.end(), unit, [](const OrderQueue& q, objects::ObjectId v) { return q.id < v; });
    if (it == queues_.end() || it->id != unit) return {};
    return it->orders;
}

void TacticalBattle::clear_queues(std::span<const objects::ObjectId> ids) {
    if (queues_.empty()) return;  // AI-only battles: never queued, nothing to do
    for (const objects::ObjectId id : ids) {
        const auto it = std::lower_bound(queues_.begin(), queues_.end(), id, [](const OrderQueue& q, objects::ObjectId v) { return q.id < v; });
        if (it != queues_.end() && it->id == id) queues_.erase(it);
    }
}

// Live state "busy": with a current queue order of a known kind, its completion rule; else any of moving, an
// ordered attack on a live target (whose ordered hard point still stands), a pending ability.
bool TacticalBattle::order_busy(objects::ObjectId id, const OrderQueue* q) const {
    const bool moving = movement_->has_unit(id) && movement_->view(id).has_goal;
    bool attacking = false;
    if (combat_->has_unit(id)) {
        const tc::ObjectId t = combat_->ordered_target(id);
        if (t != tc::kNoObject && combat_->has_unit(t) && combat_->alive(t)) {
            const int32_t hp = combat_->ordered_hardpoint(id);
            attacking = hp < 0 || static_cast<uint32_t>(hp) >= combat_->hardpoint_count(t) ||
                        !combat_->hardpoint_destroyed(t, static_cast<uint32_t>(hp));
        }
    }
    const bool pending = abilities_->has_object(id) && abilities_->pending_type(id) >= 0;
    if (q && q->has_current) {
        switch (q->current) {
            case QueuedOrder::Kind::Move:
            case QueuedOrder::Kind::MoveToObject: return moving;
            case QueuedOrder::Kind::Attack: return attacking;
            case QueuedOrder::Kind::Ability: return pending;
        }
    }
    return moving || attacking || pending;
}

// Appends `o` (a new group serial) to the queues of the busy / already-queued ids; returns the idle ids with
// an empty queue, which the caller runs at once (their queue then tracks that order as the current one only
// when more entries follow later: an empty queue is not kept).
std::vector<objects::ObjectId> TacticalBattle::enqueue(std::span<const objects::ObjectId> ids, const QueuedOrder& o) {
    queue_used_ = true;
    QueuedOrder e = o;
    e.group = ++queue_group_;
    std::vector<objects::ObjectId> now;
    for (const objects::ObjectId id : ids) {
        auto it = std::lower_bound(queues_.begin(), queues_.end(), id, [](const OrderQueue& q, objects::ObjectId v) { return q.id < v; });
        const bool has = it != queues_.end() && it->id == id;
        if (!has && !order_busy(id, nullptr)) {
            now.push_back(id);
            continue;
        }
        if (!has) {
            OrderQueue q;
            q.id = id;
            it = queues_.insert(it, std::move(q));
        }
        it->orders.push_back(e);
    }
    return now;
}

bool TacticalBattle::queue_object_live(const TacticalMode& mode, objects::ObjectId id) const {
    const auto it = std::lower_bound(units_.begin(), units_.end(), id, [](const Unit& u, objects::ObjectId v) { return u.id < v; });
    if (it != units_.end() && it->id == id) return it->alive && !it->retreated;
    const objects::ObjectStore& st = mode.primary();  // ability team containers
    return abilities_->has_object(id) && st.alive(st.find(id));
}

void TacticalBattle::advance_queues(TacticalMode& mode, SimRng& rng) {
    if (queues_.empty()) return;
    const objects::ObjectStore& st = mode.primary();
    std::erase_if(queues_, [&](const OrderQueue& q) { return !queue_object_live(mode, q.id); });
    struct Start {
        QueuedOrder o;
        std::vector<objects::ObjectId> ids;  // ascending
    };
    // An entry that can no longer run (owner changed, target gone / no longer an enemy, hard point destroyed) is
    // dropped at pop time and the next one is tried; the current order stays what it was.
    auto runnable = [&](objects::ObjectId id, const QueuedOrder& o) {
        if (st.owner(st.find(id)) != static_cast<int32_t>(o.player)) return false;
        switch (o.kind) {
            case QueuedOrder::Kind::Move: return true;
            case QueuedOrder::Kind::MoveToObject: return st.alive(st.find(o.target));
            case QueuedOrder::Kind::Attack: {
                const Unit* t = unit_by_id(o.target);
                if (!t)  // q6: a TEAM container target
                    if (const Container* ct = team_target(o.target)) return combat_->is_enemy(o.player, st.owner(ct->h));
                if (!t || !t->alive || t->retreated || !combat_->is_enemy(o.player, st.owner(t->h))) return false;
                return o.hardpoint < 0 || !combat_->hardpoint_destroyed(t->id, static_cast<uint32_t>(o.hardpoint));
            }
            case QueuedOrder::Kind::Ability: return true;  // handle_ability re-validates
        }
        return false;
    };
    for (int pass = 0; pass < 8 && !queues_.empty(); ++pass) {
        std::vector<Start> starts;  // first-appearance order over ascending ids
        for (OrderQueue& q : queues_) {
            // s78n t06 review (A1_READINGS §28.9, HERO_CLASH §3.2): a movement-locked unit (hero clash, script lock, ability
            // immobilize, landing exit) takes no queued order. [port] Its queue is dropped, not kept for after the lock:
            // retail's lock callers leave every coordinator (Stop, the clash Begin 0x6abde0) or replace the move (Confuse),
            // which discards the formation's queued moves, and a locked unit's coordinator Move_To is dropped.
            if (movement_locked(q.id)) {
                q.orders.clear();
                continue;
            }
            if (order_busy(q.id, &q)) continue;
            while (!q.orders.empty() && !runnable(q.id, q.orders.front())) q.orders.erase(q.orders.begin());
            if (q.orders.empty()) continue;
            QueuedOrder o = std::move(q.orders.front());
            q.orders.erase(q.orders.begin());
            q.has_current = true;
            q.current = o.kind;
            Start* s = nullptr;
            for (Start& c : starts)
                if (c.o.group == o.group && c.o.player == o.player) s = &c;
            if (!s) {
                starts.push_back({std::move(o), {}});
                s = &starts.back();
            }
            s->ids.push_back(q.id);
        }
        std::erase_if(queues_, [](const OrderQueue& q) { return q.orders.empty(); });
        if (starts.empty()) break;
        for (Start& s : starts) {
            const QueuedOrder& o = s.o;
            switch (o.kind) {
                case QueuedOrder::Kind::Move:  // [port] fleet cohesion: the player's setting when the move starts
                    exec_move(mode, s.ids, o.x, o.y, rng, o.facing, o.attack_move, o.flags, false, kNoObjectId, false,
                              cohesion_for(o.player, o.flags));
                    break;
                case QueuedOrder::Kind::MoveToObject: {
                    const objects::ObjectHandle th = st.find(o.target);
                    if (!st.alive(th)) break;  // target gone: dropped, the next entry runs next pass
                    const Vec3 p = st.position(th);
                    exec_move(mode, s.ids, p.x, p.y, rng, mv::kNoFacing, o.attack_move);
                    break;
                }
                case QueuedOrder::Kind::Attack: {
                    const Unit* t = unit_by_id(o.target);
                    if (const Container* ct = t ? nullptr : team_target(o.target)) {  // q6: a TEAM container target
                        if (combat_->is_enemy(o.player, st.owner(ct->h))) exec_attack(mode, s.ids, o.target, -1, rng);
                        break;
                    }
                    if (!t || !t->alive || t->retreated || !combat_->is_enemy(o.player, st.owner(t->h))) break;
                    if (o.hardpoint >= 0 && combat_->hardpoint_destroyed(t->id, static_cast<uint32_t>(o.hardpoint))) break;
                    exec_attack(mode, s.ids, t->id, o.hardpoint, rng);
                    break;
                }
                case QueuedOrder::Kind::Ability: {
                    cmd::EventData ev = o.ability;
                    std::vector<uint32_t> w;
                    for (const objects::ObjectId id : s.ids) w.push_back(static_cast<uint32_t>(id));
                    const cmd::EventType t = ev.type();
                    ev.set_ids((t == cmd::EventType::TacticalSpecialAbility || t == cmd::EventType::TacticalSpecialAbilityWithDummyTarget)
                                   ? "users"
                                   : "units",
                               std::move(w));
                    handle_ability(mode, o.player, ev, rng);
                    break;
                }
            }
        }
    }
}

void TacticalBattle::hash_queues(StateHasher& h) const {
    h.u32(queue_group_);
    h.u32(static_cast<uint32_t>(queues_.size()));
    for (const OrderQueue& q : queues_) {
        h.i32(q.id);
        h.boolean(q.has_current);
        h.u8(static_cast<uint8_t>(q.current));
        h.u32(static_cast<uint32_t>(q.orders.size()));
        for (const QueuedOrder& o : q.orders) {
            h.u8(static_cast<uint8_t>(o.kind));
            h.u8(o.player);
            h.u32(o.group);
            h.i32(o.target);
            h.i32(o.hardpoint);
            h.f32(o.x);
            h.f32(o.y);
            h.f32(o.facing);
            h.boolean(o.attack_move);
            if (o.flags != 0) h.u8(o.flags);  // P1 move flags (absent = 0: older streams unchanged)
            if (o.kind == QueuedOrder::Kind::Ability) {
                std::vector<uint8_t> bytes;
                cmd::encode_payload(o.ability, bytes);
                h.u8(o.ability.type_id());
                h.u32(static_cast<uint32_t>(bytes.size()));
                for (const uint8_t b : bytes) h.u8(b);
            }
        }
    }
}

// ---- end human order API ---------------------------------------------------------------------------------------------

bool TacticalBattle::retreat_prevented(int32_t player) const {
    // cu8 t01 (TACTICAL_GAPS §1.5, BATTLE_TRANSITIONS §3.4 request check): an interdict / Retreat_Prevention entry that
    // targets the player refuses the request (0x282750 space / 0x2825e0 land).
    if (abilities_ && abilities_->retreat_blocked(player)) return true;
    // cu8f t07 (CU8F_T01_T02_T07_RE.md §3.2): space only, the fallback query 0x2a9ff0 - behaviour 0x2C
    // TACTICAL_SUPER_WEAPON in the requesting player's own object list, no distance or hostility test: a player who owns
    // such an object in the battle (FoC: Death_Star, Death_Star_II) cannot retreat (reason 6). Land has no such query.
    // [port] live (not dead, not retreated) battle units, current owner.
    if (!space_ || !combat_) return false;
    for (const Unit& u : units_) {
        if (!u.alive || u.retreated || !bt_is_super_weapon(u.type)) continue;
        int32_t owner = -1;
        if (combat_->has_unit(u.id)) {
            owner = combat_->owner(u.id);
        } else if (l1_mode_ && l1_mode_->primary().alive(u.h)) {
            owner = l1_mode_->primary().owner(u.h);
        }
        if (owner == player) return true;
    }
    return false;
}

bool TacticalBattle::request_retreat(int32_t player) {
    const int32_t side = player == req_.info.defender ? 0 : player == req_.info.attacker ? 1 : -1;
    if (side < 0 || finished_ || vm_declared_ || frame_ < retreat_allowed_frame_ || retreat_side_ >= 0) return false;
    if (retreat_prevented(player)) return false;
    const uint32_t f = static_cast<size_t>(player) < galaxy_->players().size() ? galaxy_->players()[static_cast<size_t>(player)].faction
                                                                                 : galactic::kNoIndex;
    const float secs = space_ ? rules_->faction(f).space_retreat_countdown : rules_->faction(f).land_retreat_countdown;
    retreat_side_ = side;
    retreat_done_frame_ = frame_ + static_cast<uint32_t>(secs * 30.0f);
    for (const Unit& u : units_)
        if (u.side == side && u.alive) combat_->set_flag(u.id, tc::kUnitRetreating, true);
    return true;
}

int32_t TacticalBattle::retreat_block(int32_t player) const {
    const bool side = player == req_.info.defender || player == req_.info.attacker;
    if (!side || finished_ || vm_declared_) return 1;
    if (frame_ < retreat_allowed_frame_) return 2;
    if (retreat_side_ >= 0) return 5;
    if (retreat_prevented(player)) return 6;
    return 0;
}

float TacticalBattle::retreat_allowed_progress() const {
    if (retreat_allowed_frame_ == 0 || frame_ >= retreat_allowed_frame_) return 1.0f;
    return static_cast<float>(frame_) / static_cast<float>(retreat_allowed_frame_);
}

float TacticalBattle::retreat_countdown_progress() const {
    if (retreat_side_ < 0 || retreat_complete_) return -1.0f;
    const int32_t player = side_player(static_cast<uint8_t>(retreat_side_));
    const uint32_t f = player >= 0 && static_cast<size_t>(player) < galaxy_->players().size()
                           ? galaxy_->players()[static_cast<size_t>(player)].faction
                           : galactic::kNoIndex;
    const float total = (space_ ? rules_->faction(f).space_retreat_countdown : rules_->faction(f).land_retreat_countdown) * 30.0f;
    if (!(total > 0.0f) || frame_ >= retreat_done_frame_) return 1.0f;
    return std::clamp(1.0f - static_cast<float>(retreat_done_frame_ - frame_) / total, 0.0f, 1.0f);
}

void TacticalBattle::retreat_policy() {
    if (!(cfg_.retreat_ratio > 0.0f) || retreat_side_ >= 0 || frame_ < retreat_allowed_frame_) return;
    for (uint8_t side = 0; side < 2; ++side)
        if (side_power(side) < cfg_.retreat_ratio * side_power(static_cast<uint8_t>(1 - side))) {
            request_retreat(side_player(side));
            return;
        }
}

void TacticalBattle::unit_gone(TacticalMode& mode, Unit& u) {
    if (u.container == kNoContainer) return;
    Container& c = containers_[u.container];
    if (c.alive > 0) --c.alive;
    if (c.alive == 0 && !c.removed) {
        c.removed = true;
        if (movement_) (void)movement_->remove_unit(c.id);  // s78n t03: a squad container's own mover
        bt_removals_.push_back({c.owner, c.type, c.type, u.retreated});  // battle host: the container leaves with its last member
        hg_unit_removed(c.id);  // hangars: a launched squadron container is gone
        abilities_->remove(c.id);
        mode.primary().destroy(c.h);
    }
}

void TacticalBattle::complete_retreat(TacticalMode& mode, uint8_t side) {
    // Post-retreat (vf11): the retreating units leave the map and are preserved in the galaxy. Playtest QA D1: every other
    // object of the player dies (BT §3.4): in space that is what cannot jump out, the station and other units without a
    // locomotor. They go through this tick's combat phase D like the attrition victims, so they break up and count as losses.
    // [port] land keeps its old rule (structures stay); units with destroyed engines still count as retreatable.
    if (space_) {
        for (const Unit& u : units_) {
            if (u.side != side || !u.alive || u.retreated || u.movable) continue;
            if (std::find(bt_attrition_ids_.begin(), bt_attrition_ids_.end(), u.id) == bt_attrition_ids_.end()) {
                bt_attrition_ids_.push_back(u.id);
                bt_used_ = true;
            }
        }
    }
    for (Unit& u : units_) {
        if (u.side != side || !u.alive || u.retreated) continue;
        // battle host: retreat attrition victims stay to be killed at this tick's combat phase D (§4.9)
        if (std::find(bt_attrition_ids_.begin(), bt_attrition_ids_.end(), u.id) != bt_attrition_ids_.end()) continue;
        u.retreated = true;
        hg_unit_removed(u.id);     // hangars / engine loops: the object leaves the battle
        abilities_->remove(u.id);  // combat unit removed too
        if (u.movable) movement_->remove_unit(u.id);
        else {
            (void)movement_->remove_unit(u.id);  // s78n t23 step 0: a registered mine obstacle (no-op otherwise)
            movement_->object_removed(u.id);     // s78f review
        }
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
        if (u.tracked) damage_tracking_.disable(u.id);
#endif
        unit_gone(mode, u);
        // [port] with an end hold the map stays on screen: the ship is gone (no jump-out flight yet), not left frozen there.
        if (cfg_.end_hold_frames > 0) mode.destroy(u.h);
    }
    retreat_complete_ = true;
    vm_retreat_complete(side_player(side));  // story t06: 0x340750 (holds released, 0xc tested)
}

void TacticalBattle::service_damage_tracking() {
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
    // Behaviour service step of DAMAGE_TRACKING (object service order is irrelevant: per-object state only).
    for (Unit& u : units_) {
        if (!u.tracked || !u.alive || u.retreated || frame_ < u.dt_next) continue;
        damage_tracking_.service(u.id, catalog_->threat_decay_step());
        u.dt_next = frame_ + kDamageTrackingInterval;
    }
#endif
}

void TacticalBattle::finish(int32_t winner, uint8_t victory, int32_t retreated) {
    finished_ = true;
    result_.winner = winner;
    result_.victory = victory;
    result_.retreated = retreated;
    result_.destroyed.clear();
    result_.starbase_destroyed = false;
    for (const Participant& p : parts_) {
        if (p.built || !p.deployed || p.spawned == 0 || p.alive > 0) continue;  // battle host: pad objects have no galactic object
        if (p.starbase) result_.starbase_destroyed = true;
        else result_.destroyed.push_back(p.galactic);
    }
    bt_battle_end();  // battle host: lead object / super-weapon step 4 (TACTICAL_GAPS §4.8)
    // battle host: every unit / container still on the map (or retreated) leaves the mode alive, ascending id.
    for (const Unit& u : units_)
        if (u.alive) bt_removals_.push_back({parts_[u.participant].owner, u.type, u.container != kNoContainer ? containers_[u.container].type : u.type, true});
    for (const Container& c : containers_)
        if (!c.removed) bt_removals_.push_back({c.owner, c.type, c.type, true});
}

// --- P4 sim (owner: galactic agent): tactical_battle.hpp stop_for_auto_resolve ---
bool TacticalBattle::stop_for_auto_resolve(int32_t player) {
    // Gate (P4_SIM_SYSTEMS §9.3 step 1): the retreat coordinator's "can retreat" check for the player = 0, i.e. the
    // checks of request_retreat: a side, not finished, retreat allowed (countdown), no retreat running.
    const bool side = player == req_.info.defender || player == req_.info.attacker;
    if (!side || finished_ || vm_declared_ || frame_ < retreat_allowed_frame_ || retreat_side_ >= 0) return false;
    if (retreat_prevented(player)) return false;  // cu8 t01
    finish(kNoPlayer, galactic::kVictoryAllDestroyed, kNoPlayer);
    return true;
}

// --- end P4 sim ---

// --- P4 tactical: tactical_battle.hpp black_market_blocked_abilities ---
std::vector<uint32_t> black_market_blocked_abilities(const ta::AbilityData& data, const galactic::GalacticSim& galaxy,
                                                     int32_t player) {
    // The galactic item lists hold special_name_crc (the name as written); the tactical side matches name_crc.
    std::vector<uint32_t> out;
    for (const ta::AbilityObjectDesc& o : data.objects)
        for (const ta::UnitAbilityRecord& r : o.unit_abilities)
            if (r.must_be_bought_on_black_market && !r.gui_activated_ability_name.empty() &&
                !galaxy.bm_allows_unit_ability(player, galactic::special_name_crc(r.gui_activated_ability_name), true))
                out.push_back(r.gui_name_crc);
    for (const ta::SpecialAbilityDesc& s : data.specials)
        if (s.must_be_bought_on_black_market && !s.name.empty() &&
            !galaxy.bm_allows_special_ability(player, galactic::special_name_crc(s.name), true))
            out.push_back(s.name_crc);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// --- end P4 tactical ---

std::vector<TacticalBattle::RemovalRecord> TacticalBattle::unit_losses() const {
    std::vector<RemovalRecord> out;
    for (const RemovalRecord& r : bt_removals_) {
        if (r.survived) continue;
        const bool team = std::any_of(containers_.begin(), containers_.end(), [&r](const Container& c) { return c.type == r.object_type; });
        if (!team) out.push_back(r);
    }
    return out;
}

bool TacticalBattle::victory_relevant(uint32_t type) {
    const objects::GameObjectTypes& types = catalog_->types();
    if (type >= types.size()) return false;
    if (vr_cache_.size() != types.size()) vr_cache_.assign(types.size(), 0);
    uint8_t& c = vr_cache_[type];
    if (c == 0) {
        const auto t = static_cast<objects::TypeIndex>(type);
        const bool r = types.get_bool(t, "Victory_Relevant") || types.get_bool(t, space_ ? "Space_Victory_Relevant" : "Land_Victory_Relevant");
        c = r ? 2 : 1;
    }
    return c == 2;
}

void TacticalBattle::apply_speed_factors() {
    // MOVEMENT 3.4 items 4, 5, 7 (product order: Mod_Multiplier, engines, list 5) [port: one factor].
    const float engines = catalog_->engines_disabled_speed_modifier();
    // s78k t03 (TACTICAL_COMBAT §4.4 "Who the squad leader is", MOVEMENT §3.4 / §3.7, FIGHTER_FLYING §4.11 item 4): a TEAM
    // container's locomotor runs at the container's effective max speed - the minimum member Max_Speed (0x4049e0) x the
    // container's factors, whose SPEED Mod_Multiplier is the squad value 0x398010 returns for the container
    // (team_mod_multiplier, keyed by the container id: land teams and space squadrons alike, no mode gate). [port] The
    // container has no mover, so the movement layer applies it (set_team): a land member walks at most at that speed, a
    // squadron's formation speed is that speed. Every member keeps its own product for item 4 (a member never takes the
    // squad value; this replaces s78j t03's stand-in that gave members the team value). The container's engines / list 5
    // are not modelled (the port's containers take neither), so its factor is the Mod_Multiplier alone. One value per
    // team per tick (sorted (team, value) pairs, probed only).
    std::vector<std::pair<uint32_t, float>> team_speed;
    // s78n t14 (FIGHTER_FLYING §7.3 item 6): the land squads with a current target (0x397a40(K, 0) [M]: the container's,
    // else a member's - members read their head's target), ascending container id.
    std::vector<mv::ObjectId> targeted;
    const auto team_mult = [&](uint32_t team) {
        auto it = std::lower_bound(team_speed.begin(), team_speed.end(), team,
                                   [](const std::pair<uint32_t, float>& e, uint32_t t) { return e.first < t; });
        if (it == team_speed.end() || it->first != team)
            it = team_speed.insert(it, {team, combat_->team_mod_multiplier(team, tc::ModKind::Speed)});
        return it->second;
    };
    for (const Unit& u : units_) {
        if (!u.alive || u.retreated || !u.movable) continue;
        float f = combat_->mod_multiplier(u.id, tc::ModKind::Speed);
        if (u.engines_offline || l1_engines_off(u.id)) f = f * engines;  // L1 systems: + timed engine disables
        f = f * (1.0f + combat_->modifier(u.id, tc::ModList::MaxSpeed));
        // OI follow-up t03 (A1_READINGS §28.9): the movement lock does not slow the unit (no speed-0 freeze); its users
        // stop it first and new move orders skip it (movement_locked).
        if (combat_->stunned(u.id, frame_)) f = 0.0f;  // OI abilities: land stun suspends the locomotor (0x647290)
        movement_->set_speed_factor(u.id, f);
        movement_->set_speed_override(u.id, abilities_->speed_override(u.id));  // OI host: Berserker_Speed (-1 = none)
        const bool in_team = u.container < containers_.size() && containers_[u.container].team && !containers_[u.container].removed;
        const auto team = in_team ? static_cast<uint32_t>(containers_[u.container].id) : 0u;
        // s78l t01 (MOVEMENT §3.4 "Team cap, exact"): the container type's raw Max_Speed is in the team minimum (-1: no
        // tag / not in a team). [port] the container's further chain factors have no host state here (fleeing is the movement
        // layer's, s78n t14 part B).
        float cmax = -1.0f;
        if (in_team && containers_[u.container].type < catalog_->types().size()) {
            const auto ct = static_cast<objects::TypeIndex>(containers_[u.container].type);
            if (!catalog_->types().get_string(ct, "Max_Speed").empty()) cmax = catalog_->types().get_float(ct, "Max_Speed");
        }
        movement_->set_team(u.id, in_team ? containers_[u.container].id : mv::ObjectId{-1}, in_team ? team_mult(team) : 1.0f, cmax);
        if (in_team && !space_ && combat_ && combat_->has_unit(u.id) && combat_->current_target(u.id) != tc::kNoObject)
            targeted.push_back(containers_[u.container].id);
    }
    std::sort(targeted.begin(), targeted.end());
    // s78n t03: a movement-backed land squad container walks at the team speed - the same value its members' set_team
    // carries (the members' minimum with its own Max_Speed, x its factor).
    for (const Container& c : containers_) {
        if (space_ || !c.team || c.removed || !movement_->has_unit(c.id)) continue;
        float cmax = -1.0f;
        if (c.type < catalog_->types().size()) {
            const auto ct = static_cast<objects::TypeIndex>(c.type);
            if (!catalog_->types().get_string(ct, "Max_Speed").empty()) cmax = catalog_->types().get_float(ct, "Max_Speed");
        }
        movement_->set_team(c.id, c.id, team_mult(static_cast<uint32_t>(c.id)), cmax);
        // s78n t14 (FIGHTER_FLYING §7.2 / §7.3): the squad's SPREAD_OUT state (a change re-lays its slots) and whether it has
        // a target (its members then do not recover to their slots).
        (void)movement_->set_team_spread_out(c.id, abilities_ && abilities_->spread_out(c.id));
        const bool tgt = (combat_ && combat_->has_unit(c.id) && combat_->current_target(c.id) != tc::kNoObject) ||
                         std::binary_search(targeted.begin(), targeted.end(), c.id);
        (void)movement_->set_team_target(c.id, tgt);
    }
    feed_pair_gates();
}

// s78n t18 (MOVEMENT §5.3 "Pair gates"): the garrison-host gate and "hidden" of the land contact tests
// (MovementSystem::set_pair_gate). A unit walking to / entering a host takes no collision object with that host; a
// squad garrisons as its container (FIGHTER_FLYING §7.4): its host is its first live member's, and while every live
// member is inside the container is hidden (no layer object, it does not walk, it takes no order). The host a unit
// has left is kept by set_garrison_exit (s4_leave / s4_apply) until its next order.
// s78n t18 review: the movement system does not save these, and its move() reads "hidden" in the command phase. They are
// a function of saved state only (units, containers, the garrison system), so they are set before the movement tick
// (apply_speed_factors), after every batch of garrison events (s4_apply), at the end of the battle tick (a death or a
// retreat changes who is "every live member") and after a load (read_part): an order given right after a load meets
// the same gates as it would have in the saving run.
void TacticalBattle::feed_pair_gates() {
    if (space_ || !movement_) return;
    namespace tg = alamo::tactical::garrison;
    const auto host_of = [](tg::ObjectId h) { return h == tg::kNoObject ? mv::ObjectId{-1} : static_cast<mv::ObjectId>(h); };
    std::vector<tg::ObjectId> link(containers_.size(), tg::kNoObject);
    std::vector<uint8_t> outside(containers_.size(), 0), inside(containers_.size(), 0);
    for (const Unit& u : units_) {
        if (!u.alive || u.retreated) continue;
        const tg::ObjectId in = garrison_.holder_of(u.id), pend = garrison_.pending_holder(u.id);
        if (u.movable && movement_->has_unit(u.id)) (void)movement_->set_pair_gate(u.id, host_of(pend), false);
        if (u.container >= containers_.size()) continue;
        const Container& c = containers_[u.container];
        if (!c.team || c.removed) continue;
        (in == tg::kNoObject ? outside : inside)[u.container] = 1;
        const tg::ObjectId h = in != tg::kNoObject ? in : pend;
        if (h != tg::kNoObject && link[u.container] == tg::kNoObject) link[u.container] = h;
    }
    for (size_t i = 0; i < containers_.size(); ++i) {
        const Container& c = containers_[i];
        if (!c.team || c.removed || !movement_->has_unit(c.id)) continue;
        (void)movement_->set_pair_gate(c.id, host_of(link[i]), inside[i] != 0 && outside[i] == 0);
    }
}

// s78n t20 (MOVEMENT §5.4.3 rule 4, §5.4.4 item 1, §5.4.5 items 1 / 3; MovementSystem::set_collision_info). The current
// target 0x397a40 of a single unit is its combat target (ordered, else acquired); of a squad container its own, else its
// first live member's that has one (team index = id order). A target that is a member of a movement-backed squad is
// named by its container (the layer object the collision records see), at the member's position. Attack range 0x3749d0 =
// Get_Max_Attack_Range; "short-range" 0x372210 = melee_attacker; the target's radius 0x397780 is added [port] for every
// unit the combat system knows (the weapon-list test of 0x685e10 is [M]). A container reads its targeting member, else
// its first live member, for the range and the melee test. Hero clash: the unit's own flag (+0x3b4).
void TacticalBattle::feed_collision_info(const objects::ObjectStore& store) {
    if (space_ || !movement_ || !combat_) return;
    using Info = mv::MovementSystem::CollisionInfo;
    const auto fill = [&](objects::ObjectId src, Info& ci) {
        if (!combat_->has_unit(src)) return;
        ci.attack_range = combat_->attack_range(src);
        ci.short_range = combat_->melee_attacker(src);
        const tc::ObjectId t = combat_->current_target(src);
        const Unit* tu = t != tc::kNoObject ? unit_by_id(t) : nullptr;
        if (!tu || !tu->alive || !store.alive(tu->h)) return;
        const Vec3 tp = store.position(tu->h);
        ci.target = tu->id;
        if (tu->container < containers_.size()) {
            const Container& tk = containers_[tu->container];
            if (tk.team && !tk.removed && movement_->is_land_team_container(tk.id)) ci.target = tk.id;
        }
        ci.target_pos = {tp.x, tp.y};
        ci.target_radius = movement_->collision_radius(ci.target);
    };
    for (const Unit& u : units_) {
        if (!u.alive || u.retreated || !u.movable || !movement_->has_unit(u.id)) continue;
        Info ci;
        fill(u.id, ci);
        ci.hero_clash = combat_->has_unit(u.id) && (combat_->flags(u.id) & tc::kUnitHeroClash) != 0;
        (void)movement_->set_collision_info(u.id, ci);
    }
    std::vector<objects::ObjectId> first(containers_.size(), objects::kNoObjectId), aiming(containers_.size(), objects::kNoObjectId);
    for (const Unit& u : units_) {
        if (!u.alive || u.retreated || u.container >= containers_.size()) continue;
        if (first[u.container] == objects::kNoObjectId) first[u.container] = u.id;
        if (aiming[u.container] == objects::kNoObjectId && combat_->has_unit(u.id) && combat_->current_target(u.id) != tc::kNoObject)
            aiming[u.container] = u.id;
    }
    for (size_t i = 0; i < containers_.size(); ++i) {
        const Container& c = containers_[i];
        if (!c.team || c.removed || !movement_->is_land_team_container(c.id)) continue;
        Info ci;
        if (combat_->has_unit(c.id) && combat_->current_target(c.id) != tc::kNoObject) fill(c.id, ci);
        else if (aiming[i] != objects::kNoObjectId) fill(aiming[i], ci);
        else if (first[i] != objects::kNoObjectId) fill(first[i], ci);
        ci.hero_clash = combat_->has_unit(c.id) && (combat_->flags(c.id) & tc::kUnitHeroClash) != 0;
        (void)movement_->set_collision_info(c.id, ci);
    }
}

// REMAINING_GAPS §4.4 / §4.7: flying units strafe and fighters run at their combat target (ordered, else auto).
void TacticalBattle::feed_attack_targets(const objects::ObjectStore& store) {
    for (const Unit& u : units_) {
        if (!u.alive || u.retreated || !u.movable || !movement_->has_unit(u.id)) continue;
        const mv::LocomotorKind k = movement_->view(u.id).locomotor;
        if (k == mv::LocomotorKind::Fighter && space_) {
            feed_fighter_target(u, store);  // FIGHTER_FLYING §4.3: the formation attack target (no point target)
            continue;
        }
        if (k != mv::LocomotorKind::Flying && k != mv::LocomotorKind::Fighter) continue;
        const Unit* t = unit_by_id(combat_->current_target(u.id));
        if (t && t->alive && store.alive(t->h)) {
            const Vec3 p = store.position(t->h);
            movement_->set_attack_target(u.id, true, {p.x, p.y}, p.z);
        } else {
            movement_->set_attack_target(u.id, false);
        }
    }
}

// Serial, before the movement tick: every fighter gets its XML values once (set_fighter_params), and every live TEAM
// container with fighter members is (re)registered as a squadron whenever its live member list (ascending id = team
// index order) differs from the MovementSystem's - so map-start companies, reinforcements, hangar launches and
// replenished wingmen all join before their first fighter frame. Removed containers drop their squadron.
void TacticalBattle::fighter_sync() {
    if (!space_ || !movement_) return;
    const objects::GameObjectTypes& types = catalog_->types();
    std::vector<std::vector<objects::ObjectId>> want(containers_.size());
    for (Unit& u : units_) {
        if (!u.alive || u.retreated || !u.movable || !movement_->has_unit(u.id)) continue;
        if (movement_->view(u.id).locomotor != mv::LocomotorKind::Fighter) continue;
        if (!u.fighter_seen) {
            u.fighter_seen = true;
            movement_->set_fighter_params(u.id, mv::fighter_params_from_type(types, u.type));
        }
        if (u.container < containers_.size()) want[u.container].push_back(u.id);
    }
    for (size_t c = 0; c < containers_.size(); ++c) {
        const Container& ct = containers_[c];
        const mv::FighterSquad* s = movement_->fighter_squad(ct.id);
        if (ct.removed || !ct.team || want[c].empty()) {
            if (s) movement_->remove_fighter_squad(ct.id);
            continue;
        }
        if (s && s->members == want[c]) continue;
        mv::FighterSquadDesc d;
        d.id = ct.id;
        d.members = want[c];
        for (size_t n = 0; n < d.members.size(); ++n) d.slots.push_back(fighter_slot(n));
        d.tolerance = ct.squad_type < types.size() ? mv::squadron_tolerance_from_type(types, ct.squad_type) : 25.0f;
        // S7 t37 / s78f t02 (A1_READINGS §33.4.1 step 3, FIGHTER_FLYING §4.11 D 2): T = the type of the first team member
        // (a fighter); Space_Layer 0 or Disregard_Space_Layer_For_Guard_Attack_Move -> its guard of a moving object
        // follows it. The chase ranges (0x4165f0's u = the container) are the squadron type's.
        if (const Unit* m0 = unit_by_id(d.members.front()); m0 && m0->type < types.size()) {
            const objects::TypeIndex mt = static_cast<objects::TypeIndex>(m0->type);
            d.guard_follow = types.get_mask(mt, "Space_Layer") == 0 || types.get_bool(mt, "Disregard_Space_Layer_For_Guard_Attack_Move");
        }
        if (ct.squad_type < types.size()) d.chase = mv::fighter_chase_ranges_from_type(types, ct.squad_type);
        movement_->set_fighter_squad(d);
    }
    // S7 t22: a formation goes with its squadron (or fighter).
    std::erase_if(fformations_, [this](const FighterFormationRecord& r) {
        if (movement_->fighter_squad(r.key)) return false;
        const Unit* x = unit_by_id(r.key);
        return !x || !x->alive || x->retreated || !movement_->has_unit(x->id);
    });
    // S7 t25 / t37 / s78f t02 (§4.10 E, the coordinator's target listener 0x4f77e0 on signals 0x10 / 0x1a / 0x2e): a
    // guarded object that is gone turns the guard block into a position block (kind 1, object null); the flags stay 0x02,
    // so the squadron keeps a position guard with Guard_Chase_Range. The movement layer's formation (mv::FighterFormation)
    // does the conversion at the object's removal (position = the last waypoint of the formation's path). s78f review: the
    // record follows the formation's CURRENT block that is not an auto block (fighter_order_view: the order's own block -
    // e.g. the guard while it is on top - keeps the order's values; a queued waypoint takes over once it is popped), not
    // blocks[0]. A dead object still in the movement layer drops the record's object.
    for (FighterFormationRecord& r : fformations_) {
        if (movement_->fighter_formation(r.key)) {  // the record = the view (unchanged while the order's block is current)
            const FighterOrderView v = fighter_order_view(r);
            r.flags = v.flags;
            r.goal = v.goal;
            r.goal_object = v.goal_object;
        }
        if (r.goal_object == objects::kNoObjectId || movement_->fighter_squad(r.goal_object)) continue;
        const Unit* g = unit_by_id(r.goal_object);
        if (!g || !g->alive || g->retreated) r.goal_object = objects::kNoObjectId;
    }
}

// The order's own block (the one Move_To built) carries no 0x08 and sits above every queued block (the waypoint append
// inserts at the bottom; a patrol block re-queued by a pop gets 0x08): while it is the current block the record's values
// stand for it [port: not every host order path hands the movement layer its block flags / object]; a queued block (0x08)
// is read from the stack. A dead object the movement layer already converted gives the conversion point.
FighterOrderView TacticalBattle::fighter_order_view(const FighterFormationRecord& r) const {
    FighterOrderView v;
    v.first_flags = v.flags = r.flags;
    v.goal = r.goal;
    v.goal_object = r.goal_object;
    const mv::FighterFormation* f = movement_ ? movement_->fighter_formation(r.key) : nullptr;
    if (!f || f->blocks.empty()) return v;
    constexpr uint8_t kMask = mv::kMoveFlagAttackMove | mv::kMoveFlagGuardMove;
    const auto queued = [](const mv::FighterBlock& b) { return (b.flags & mv::kMoveFlagQueue) != 0; };
    if (queued(f->blocks.front())) v.first_flags = static_cast<uint8_t>(f->blocks.front().flags & kMask);
    const mv::FighterBlock* c = &f->blocks.front();  // the bottom block is never an auto block (Push keeps it)
    for (size_t i = f->blocks.size(); i-- > 0;)
        if (!f->blocks[i].pushed) {
            c = &f->blocks[i];
            break;
        }
    if (queued(*c)) {
        v.flags = static_cast<uint8_t>(c->flags & kMask);
        v.goal = Vec3{c->point.x, c->point.y, 0.0f};  // an object block: the order-time point; readers follow the object
        v.goal_object = c->kind == mv::kFighterBlockObject ? c->object : objects::kNoObjectId;
    } else if (c->kind == mv::kFighterBlockPosition && r.goal_object != objects::kNoObjectId) {
        const auto it = std::lower_bound(units_.begin(), units_.end(), r.goal_object,
                                         [](const Unit& x, objects::ObjectId id) { return x.id < id; });
        const bool live = it != units_.end() && it->id == r.goal_object && it->alive && !it->retreated;
        const std::span<const mv::FighterSquad> sq = movement_->fighter_squads();
        const auto si = std::lower_bound(sq.begin(), sq.end(), r.goal_object, [](const mv::FighterSquad& q, objects::ObjectId id) { return q.id < id; });
        if (!live && !(si != sq.end() && si->id == r.goal_object)) {
            v.goal = Vec3{c->point.x, c->point.y, 0.0f};
            v.goal_object = objects::kNoObjectId;
        }
    }
    return v;
}

std::vector<objects::ObjectId> TacticalBattle::fighter_pushed(objects::ObjectId key) const {
    std::vector<objects::ObjectId> out;
    if (!movement_) return out;
    if (const mv::FighterFormation* f = movement_->fighter_formation(key))
        for (const mv::FighterBlock& b : f->blocks)
            if (b.pushed && b.kind == mv::kFighterBlockObject) out.push_back(b.object);
    return out;
}

// s78f t02 (FIGHTER_FLYING §4.11 C / D / E): the movement layer's fighter formations read the fog (FOW_Object_Hidden with
// force 1, 0x35f470), an object's current attack target (0x397a40; a squadron container: its first member's, the shared
// team target [port]), the member range test (TARGETING slots 43 / 46: not inside the min range, centre distance <= the
// max attack range [M: no target radius]) and the positions of objects that are not movers (stations, structures).
void TacticalBattle::fighter_formation_hooks() {
    mv::FighterFormationHooks h;
    h.hidden = [this](int32_t owner, mv::ObjectId id) { return fog_object_hidden(owner, id, true); };
    h.attack_target = [this](mv::ObjectId id) -> mv::ObjectId {
        const objects::ObjectId cu = fighter_combat_unit(id);
        if (cu == objects::kNoObjectId || !combat_ || !combat_->has_unit(cu)) return -1;
        const objects::ObjectId t = combat_->current_target(cu);
        return t == objects::kNoObjectId ? -1 : t;
    };
    h.in_range = [this](mv::ObjectId m, mv::ObjectId o) {
        const objects::ObjectId ou = fighter_combat_unit(o);
        if (!combat_ || !combat_->has_unit(m) || ou == objects::kNoObjectId || !combat_->has_unit(ou)) return false;
        if (combat_->inside_min_range(m, ou)) return false;
        const tactical::Vec3 a = combat_->position(m), b = combat_->position(ou);
        const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        const float r = combat_->attack_range(m);
        return (dx * dx + dy * dy) + dz * dz <= r * r;
    };
    h.position = [this](mv::ObjectId id, mv::F3& p) {
        // s78f review: a dead or retreated object (combat keeps dead units until its compaction) has no position, so a
        // block naming it is dropped (the death itself also reaches MovementSystem::object_removed).
        const Unit* x = unit_by_id(id);
        if (!combat_ || !combat_->has_unit(id) || !x || !x->alive || x->retreated) return false;
        const tactical::Vec3 v = combat_->position(id);
        p = {v.x, v.y, v.z};
        return true;
    };
    movement_->set_fighter_formation_hooks(std::move(h));
}

// FIGHTER_FLYING §2 step 6 / §4.3 step 2 / §4.9: the formation attack target Formation_Attack_Target(F, &hp, -1)
// (0x41a2e0, TACTICAL_COMBAT §2.8: newest valid block first; a block qualifies with an object hostile to the leader's
// owner, guard blocks never). The fighter states read only this goal, never the combat target (§4.9). A squadron's
// target comes from its leader. fighter_formation_target (tactical_battle.hpp) picks it:
//   1. the ordered combat target (Attack / Attack_Target, the set path of a task-force Attack_Move, path B's lock, the
//      locomotor's own Provoke / Retarget orders) [port: the ordered slot stands for the order's object block and the
//      locked team target; while it is set the team target is locked, so §4.9 step 2 pushes no auto block];
//   2. else the newest pushed auto attack block that is live and hostile (S7 t22: §4.9 step 2, fighter_push - the only
//      way an auto target moves a squadron; it is newer than the order's own block);
//   3. else the attack goal (the order's object block: Attack_Move(object)) when hostile to the leader's owner;
//   4. else none: 0x41a2e0 finds no object (a position, guard or no formation) and the squadron keeps Moving (§4.1) or
//      parks (§4.2); its weapons still fire at targets of opportunity (combat).
// S7 t12's "moving -> none" rule and t12's idle "combat auto target unless hidden" stand-in are gone: a plain Move_To
// does not peel off at enemies along the way because the chase test measures Idle_Chase_Range from its destination
// (fighter_chase_test rule 5), an Attack_Move(position) does (Attack_Move_Response_Range, rule 4).
// A target that is a member of a registered squadron is that squadron (a team: dogfight §4.5); otherwise the object with
// its combat radius (0x397780) and, for an ordered hard point, the hard point's position (0x396ce0: the combat attachment
// point, unit-local x forward / z up). hidden = FOW_Object_Hidden for the attacker's owner with force 1 (0x35f470): no
// filter here, so a fogged goal toggles Idle <-> Directed_Combat every frame as in retail (§4.9 step 3). No target clears
// it (the locomotor then retargets from its dogfight cell and reports Retarget).
objects::ObjectId fighter_formation_target(const FighterTargetInputs& in) {
    if (in.ordered != objects::kNoObjectId) return in.ordered;
    if (in.pushed != objects::kNoObjectId) return in.pushed;
    if (in.goal != objects::kNoObjectId && in.goal_hostile) return in.goal;
    return objects::kNoObjectId;
}

uint8_t fighter_chase_test(const FighterChaseInputs& in) {
    if (!in.formation || in.candidate_is_goal) return 0;  // rule 1
    float rr = (in.flags & mv::kMoveFlagGuardMove) != 0    ? in.guard_range
               : (in.flags & mv::kMoveFlagAttackMove) != 0 ? in.attack_move_range
                                                           : in.idle_range;  // rule 2
    if (rr < 0.0f) rr = 0.0f;
    uint8_t base = 0;
    if (in.candidate_attacker) {  // rule 3
        rr += in.extension;
        base = 1;
    } else if (in.guarded_moving) {
        return 0;
    }
    if (in.moving && (in.flags & mv::kMoveFlagAttackMove) != 0) return 2;  // rule 4 [M: within Rr of its own path point]
    if (!in.last_guard && !in.single_position) return 0;                    // rule 5
    const auto within = [](Vec3 a, Vec3 b, float r) {
        const float dx = a.x - b.x, dy = a.y - b.y;
        return dx * dx + dy * dy <= r * r;
    };
    if (in.moving && !within(in.unit, in.goal, rr)) return 0;  // rule 6 (state 3 skips it)
    if (within(in.candidate, in.goal, rr)) return 1;           // rule 7
    return within(in.candidate, in.goal, rr + in.attack_range) ? static_cast<uint8_t>(base + 1) : 0;
}

const FighterFormationRecord* TacticalBattle::fighter_formation(objects::ObjectId key) const {
    const auto it = std::lower_bound(fformations_.begin(), fformations_.end(), key,
                                     [](const FighterFormationRecord& r, objects::ObjectId k) { return r.key < k; });
    return it != fformations_.end() && it->key == key ? &*it : nullptr;
}

FighterFormationRecord* TacticalBattle::fighter_formation_mut(objects::ObjectId key) {
    return const_cast<FighterFormationRecord*>(std::as_const(*this).fighter_formation(key));
}

objects::ObjectId TacticalBattle::fighter_key(const Unit& u) const {
    if (u.container < containers_.size() && containers_[u.container].team) return containers_[u.container].id;
    return u.id;
}

objects::ObjectId TacticalBattle::fighter_goal_key(objects::ObjectId id) {
    const Unit* x = id != objects::kNoObjectId ? unit_by_id(id) : nullptr;
    if (x && x->container < containers_.size() && movement_->fighter_squad(containers_[x->container].id)) return containers_[x->container].id;
    return id;
}

bool TacticalBattle::fighter_goal_live(objects::ObjectId key, const objects::ObjectStore& store) {
    if (key == objects::kNoObjectId) return false;
    if (const mv::FighterSquad* s = movement_->fighter_squad(key)) return !s->members.empty();
    const Unit* x = unit_by_id(key);
    return x && x->alive && !x->retreated && store.alive(x->h) && combat_->has_unit(x->id);
}

// A move / attack order replaces the formation of every fighter squadron it names (one record per squadron).
void TacticalBattle::fighter_order(std::span<const objects::ObjectId> ids, uint8_t flags, Vec3 goal, objects::ObjectId goal_object) {
    if (!space_ || !movement_) return;
    for (const objects::ObjectId id : ids) {
        const Unit* u = unit_by_id(id);
        if (!u || !u->alive || u->retreated || !movement_->has_unit(id) || movement_->view(id).locomotor != mv::LocomotorKind::Fighter) continue;
        const objects::ObjectId key = fighter_key(*u);
        FighterFormationRecord* r = fighter_formation_mut(key);
        if (!r) {
            const auto it = std::lower_bound(fformations_.begin(), fformations_.end(), key,
                                             [](const FighterFormationRecord& a, objects::ObjectId k) { return a.key < k; });
            FighterFormationRecord nr;
            nr.key = key;
            r = &*fformations_.insert(it, std::move(nr));
        }
        r->flags = static_cast<uint8_t>(flags & (mv::kMoveFlagAttackMove | mv::kMoveFlagGuardMove));
        r->goal = goal;
        r->goal_object = goal_object;
    }
}

void TacticalBattle::fighter_order_clear(std::span<const objects::ObjectId> ids) {
    for (const objects::ObjectId id : ids)
        if (const Unit* u = unit_by_id(id)) {
            const objects::ObjectId key = fighter_key(*u);
            std::erase_if(fformations_, [key](const FighterFormationRecord& r) { return r.key == key; });
        }
}

// §4.9 step 2 (Service_Space -> Auto_Engage, LocomotorCommon 0x42d0f0 on the container's pack). t = the squadron's
// unlocked auto target: none while the leader holds a live ordered (locked team) target; else the first member's in
// team index order (the squadron shares the first member's pick, 0x406370 [port: combat keeps per-member auto targets]).
// t (as its squadron when it is a team member) is pushed as the newest auto block when it is not the formation's goal
// (newest valid block, -1) already and fighter_chase_test passes [port: rr = 1's detach into a formation of its own is
// moot, the record is per squadron; the mode's hold table 0x4944e0 is not modelled]. No formation: nothing is pushed.
// s78f t02 (FIGHTER_FLYING §4.11 B 3): the block goes into the movement layer's block stack (MovementSystem::fighter_push,
// 0x41fd30 with its pre-pop: it replaces an earlier auto block, never a guard); the movement layer pops it when the leash
// (0x4165f0) fails or its object is hidden, so the auto blocks are read back from there (fighter_pushed).
objects::ObjectId TacticalBattle::fighter_push(const Unit& leader, objects::ObjectId key, const objects::ObjectStore& store) {
    FighterFormationRecord* r = fighter_formation_mut(key);
    if (!r) return objects::kNoObjectId;
    const int32_t me = store.owner(leader.h);
    const auto hostile = [&](objects::ObjectId k) {
        const objects::ObjectId cu = fighter_combat_unit(k);
        return cu != objects::kNoObjectId && combat_->has_unit(cu) && combat_->is_enemy(me, combat_->owner(cu));
    };
    const auto newest = [&]() -> objects::ObjectId {
        const std::vector<objects::ObjectId> pushed = fighter_pushed(key);
        for (auto it = pushed.rbegin(); it != pushed.rend(); ++it)
            if (fighter_goal_live(*it, store) && hostile(*it)) return *it;
        return objects::kNoObjectId;
    };
    const auto top_pushed = [&]() -> objects::ObjectId {  // the formation's top block when it is a live auto block
        const mv::FighterFormation* f = movement_->fighter_formation(key);
        if (!f || f->blocks.empty() || !f->blocks.back().pushed) return objects::kNoObjectId;
        return fighter_goal_live(f->blocks.back().object, store) ? f->blocks.back().object : objects::kNoObjectId;
    };
    if (combat_->has_unit(leader.id) && fighter_goal_live(combat_->ordered_target(leader.id), store)) return newest();
    std::span<const objects::ObjectId> members(&leader.id, 1);
    const mv::FighterSquad* sq = movement_->fighter_squad(key);
    if (sq) members = sq->members;
    objects::ObjectId t = objects::kNoObjectId;
    bool any_target = false;
    for (const objects::ObjectId m : members) {
        if (!combat_->has_unit(m)) continue;
        any_target = any_target || combat_->current_target(m) != objects::kNoObjectId;
        if (combat_->ordered_target(m) != objects::kNoObjectId) continue;
        const objects::ObjectId a = combat_->auto_target(m);
        if (a != objects::kNoObjectId && fighter_goal_live(a, store)) {
            t = a;
            break;
        }
    }
    if (t == objects::kNoObjectId) {
        // S7 t25 (§4.9 step 2, LocomotorCommon 0x42d0f0): Auto_Engage(null) with a formation goal -> 0x4105a0 [M: ends the
        // pushed block] - the squadron's members hold no target at all (dropped by enter_idle, fog, death, an owner
        // change): the newest auto block goes (dead blocks were erased above), so a fogged or converted pushed goal does
        // not hold the squadron.
        if (!any_target && top_pushed() != objects::kNoObjectId) movement_->fighter_pop(key);
        return newest();
    }
    const objects::ObjectId tk = fighter_goal_key(t);
    if (!hostile(tk)) return newest();  // an owner change: the targeting service drops it at its next due frame
    objects::ObjectId g = newest();
    const objects::ObjectId order_goal = fighter_goal_key(combat_->attack_goal(leader.id));
    if (g == objects::kNoObjectId && fighter_goal_live(order_goal, store) && hostile(order_goal)) g = order_goal;
    if (tk == g) return g;
    // S7 t25 (§4.10 C): Auto_Engage reaches each member's fighter locomotor (TeamLocomotor 0x630140 -> vf +0x260
    // 0x5cb580) after the container's own push: t a squadron, the member's lock timer expired -> lock the first member of
    // t's squadron (index order) whose own timer expired; both timers 10 s, the victim's own lock dropped. No block.
    if (const mv::FighterSquad* ts = sq ? movement_->fighter_squad(tk) : nullptr) {
        const std::vector<objects::ObjectId> mine(sq->members.begin(), sq->members.end());
        const std::vector<objects::ObjectId> theirs(ts->members.begin(), ts->members.end());
        for (const objects::ObjectId m : mine) {
            mv::FighterState* fs = movement_->fighter_state(m);
            if (!fs || fs->lock_start + fs->lock_frames > frame_) continue;
            for (const objects::ObjectId x : theirs) {
                mv::FighterState* xs = movement_->fighter_state(x);
                if (!xs || xs->lock_start + xs->lock_frames > frame_) continue;
                fs->lock = x;
                fs->lock_start = frame_;
                fs->lock_frames = mv::kFighterLockFrames;
                xs->lock = -1;
                xs->lock_start = frame_;
                xs->lock_frames = mv::kFighterLockFrames;
                break;
            }
        }
    }
    const mv::UnitView lv = movement_->view(leader.id);
    const objects::ObjectId last_push = top_pushed();
    const bool live_push = last_push != objects::kNoObjectId;  // a live pushed auto block is the last block
    const FighterOrderView ov = fighter_order_view(*r);           // s78f review: the movement stack's order block
    FighterChaseInputs ci;
    ci.formation = true;
    ci.flags = ov.first_flags;  // Rr and the attack-move branch read the first block
    ci.moving = lv.has_goal && !lv.arrived;
    ci.last_guard = (ov.flags & mv::kMoveFlagGuardMove) != 0 && !live_push;
    ci.single_position = !ci.moving && !live_push && ov.goal_object == objects::kNoObjectId && !fighter_goal_live(order_goal, store);
    ci.candidate_is_goal = tk == order_goal || tk == fighter_goal_key(ov.goal_object) || (live_push && tk == last_push);
    // Rule 3: the guarded object (a guard block's object) and whether c attacks it or the squadron.
    const Unit* guarded = (ov.flags & mv::kMoveFlagGuardMove) != 0 && ov.goal_object != objects::kNoObjectId ? unit_by_id(ov.goal_object) : nullptr;
    if (guarded && (!guarded->alive || guarded->retreated || !store.alive(guarded->h))) guarded = nullptr;
    if (guarded && movement_->has_unit(guarded->id)) {
        const mv::UnitView gv = movement_->view(guarded->id);
        ci.guarded_moving = gv.has_goal && gv.speed > 0.0f;
    }
    {
        std::vector<objects::ObjectId> attackers;  // c, or every member of c's squadron
        if (const mv::FighterSquad* cs = movement_->fighter_squad(tk)) attackers.assign(cs->members.begin(), cs->members.end());
        else attackers.push_back(t);
        for (const objects::ObjectId a : attackers) {
            if (!combat_->has_unit(a)) continue;
            const objects::ObjectId at = combat_->current_target(a);
            if (at == objects::kNoObjectId) continue;
            if ((guarded && at == guarded->id) || std::find(members.begin(), members.end(), at) != members.end()) ci.candidate_attacker = true;
        }
    }
    ci.unit = store.position(leader.h);
    // Rr: the squadron type's (a fighter without one: its own type's); cached per type.
    const objects::GameObjectTypes& types = catalog_->types();
    uint32_t ty = leader.type;
    if (sq && leader.container < containers_.size() && containers_[leader.container].squad_type < types.size())
        ty = containers_[leader.container].squad_type;
    if (ty < types.size()) {
        if (fchase_have_.size() != types.size()) {
            fchase_have_.assign(types.size(), 0);
            fchase_cache_.assign(types.size(), {});
        }
        if (!fchase_have_[ty]) {
            const auto ti = static_cast<objects::TypeIndex>(ty);
            fchase_cache_[ty] = {types.get_float(ti, "Guard_Chase_Range"), types.get_float(ti, "Idle_Chase_Range"),
                                 types.get_float(ti, "Attack_Move_Response_Range"),
                                 types.get_float(ti, "Autonomous_Move_Extension_Vs_Attacker")};
            fchase_have_[ty] = 1;
        }
        ci.guard_range = fchase_cache_[ty][0];
        ci.idle_range = fchase_cache_[ty][1];
        ci.attack_move_range = fchase_cache_[ty][2];
        ci.extension = fchase_cache_[ty][3];
    }
    ci.attack_range = combat_->attack_range(leader.id);
    ci.goal = ov.goal;
    if (const Unit* go = ov.goal_object != objects::kNoObjectId ? unit_by_id(ov.goal_object) : nullptr;
        go && go->alive && !go->retreated && store.alive(go->h))
        ci.goal = store.position(go->h);  // an object block: the slot goal follows the object
    const Unit* tu = unit_by_id(t);
    ci.candidate = store.position(tu->h);
    if (fighter_chase_test(ci) != 0) movement_->fighter_push(key, tk);  // 0x41cf70: the auto attack block {kind 3, t, +0x5d = 1}
    return newest();
}
}  // namespace alamo::sim
