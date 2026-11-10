// S4 garrisons + structure capture hookup of TacticalBattle (owner: S4 agent; engine/tactical/garrison; rules:
// docs/re/L1_S4_REMAINING.md §3.5, §4.1, §4.2). Kept in its own file so the other battle hooks (fog, A1, L1 systems) stay
// separate. Serial code; the only RNG use is the
// MovementSystem move of order_garrison (same rule as order_move). Host-side [port] choices:
//   * garrisoned units stay combat units: kUnitGarrisoned (never a candidate, TACTICAL_COMBAT 2.3 step 3), position =
//     the host's every tick, fire range x Garrisoned_Max_Attack_Distance_Multiplier (a FireRange modifier entry of their
//     own stacking category) in a GarrisonCanFire host, else FireRange -1000 (no shot can reach). The Num_Garrison_Guns
//     muzzle bones and their synced draw per shot (§3.5 garrison fire) are not modelled (the combat fire path is S8's);
//   * they leave the MovementSystem while inside and are re-added at the exit point (no fade / walk-out);
//   * relations: allied = same owner or not CombatSystem::is_enemy; the galaxy's neutral player's hosts are open (§2.5);
//   * map placements with CAPTURE_POINT or a garrison behaviour and no locomotor are spawned for their map owner (mini 02
//     = player id, REMAINING_GAPS §3.3; probe: the campaign map ids vs the player order); the SELECTABLE ones (abandoned
//     sensor / turbolaser, mining facility, outfitter, capturable bunker) are also battle combat units of that owner
//     (bt_map_load, third party: health, damage and death; a destroyed one leaves the capture system), the others stay
//     store objects; the capture countdown skips points of the neutral player / an Is_Neutral faction;
//   * §6.5 capture reset (s4_capture_reset): a pad's capture point goes back to the neutral player when its child goes;
//   * an owner change of a non-combat object (capture point / placed host) = ObjectStore::set_owner + TreeSet::set_owner;
//     battle units go through change_owner();
//   * a host without a garrison behaviour that dies kills its occupants (CombatSystem::kill_unit, no killer credit).
#include "alamo/sim/tactical_battle.hpp"

#include "alamo/core/dmath.hpp"
#include "alamo/objects/game_object_types.hpp"
#include <algorithm>

#include "alamo/tactical/garrison/garrison_types.hpp"
#include "alamo/tactical/movement/special_locomotors.hpp"
#include "alamo/tactical/movement/unit_params.hpp"

namespace alamo::sim {

namespace cmd = alamo::commands;


namespace tg = alamo::tactical::garrison;
namespace tc = alamo::tactical::combat;
namespace mv = alamo::tactical::movement;

namespace {
constexpr int32_t kS4RangeCategory = 0x53344752;  // own stacking category ("S4GR")
constexpr uint32_t kS4RangeCrc = 0x47415252u;     // source name CRC stand-in ("GARR")
constexpr int32_t kS4RangeSource = -0x5334;       // source id (no object)
constexpr float kNoGarrisonFire = -1000.0f;       // [port] FireRange r for a host without GarrisonCanFire
tactical::Vec3 to_tv(Vec3 v) { return tactical::Vec3{v.x, v.y, v.z}; }
}  // namespace

void TacticalBattle::s4_init() {
    // s78n t12 review: keyed on the systems, not on s4_used_ - a loading battle whose rebuilt setup made no garrison use
    // (the saved battle's first use came later: a reinforcement, a vehicle theft) creates them when the garrison part
    // shows the saved battle had them (read_part), and the state part read after it sets the flag again.
    if (captures_) return;
    s4_used_ = true;
    const int32_t neutral = galaxy_->neutral_player();
    auto allied = [this](int32_t a, int32_t b) { return a == b || (combat_ && !combat_->is_enemy(a, b)); };
    garrison_.set_allied(allied);
    garrison_.set_neutral_player(neutral);
    // s78n t18 (FIGHTER_FLYING §7.4): a land squad garrisons as one object, its team container
    garrison_.set_group_fn([this](tg::ObjectId id, tg::GarrisonGroup& g) { return s4_group(id, g); });
    const float gc_secs = catalog_->capture_countdown(space_);  // REMAINING_GAPS §3.3: GameConstants when present
    // skai: L1_S4_REMAINING §4.2 start gate: the countdown is 0 in skirmish (a skirmish map battle: skirmish_map).
    const float secs = skirmish_map() ? 0.0f : gc_secs >= 0.0f ? gc_secs : space_ ? cfg_.space_capture_countdown : cfg_.land_capture_countdown;
    captures_ = std::make_unique<tg::CapturePointSystem>(neutral, 4u, secs > 0.0f ? static_cast<uint32_t>(30.0f * secs + 0.5f) : 0u);
    captures_->set_allied(allied);
    captures_->set_neutral_owner([this](int32_t o) { return bt_owner_neutral(o); });  // REMAINING_GAPS §3.3
    // q6 t19 rule (a) (AI_GALACTIC_TOKENS §9.11): a pad holding a UC or a child is never captured (keeps its owner).
    captures_->set_pad_occupied([this](tg::ObjectId id) { return pad_contents(id) != objects::kNoObjectId; });
    captures_->set_occupant_owner([this](tg::ObjectId id) {  // rule (b): the first occupant's owner
        const auto in = garrison_.occupants(id);
        if (in.empty() || !s4_store_) return -1;
        const objects::ObjectHandle h = s4_store_->find(in.front());
        return s4_store_->exists(h) ? s4_store_->owner(h) : -1;
    });
}

objects::ObjectId TacticalBattle::s4_squad_of(objects::ObjectId unit) const {
    const auto it = std::lower_bound(units_.begin(), units_.end(), unit, [](const Unit& u, objects::ObjectId v) { return u.id < v; });
    if (space_ || !movement_ || it == units_.end() || it->id != unit || it->container >= containers_.size()) return objects::kNoObjectId;
    const Container& c = containers_[it->container];
    if (!c.team || c.removed || !movement_->is_land_team_container(c.id)) return objects::kNoObjectId;
    return c.id;
}

bool TacticalBattle::s4_group(objects::ObjectId unit, tg::GarrisonGroup& g) const {
    // FIGHTER_FLYING §7.4 items 1 - 3: capacity and the category mask read the CONTAINER's type - Garrison_Value (the
    // type constructor's default 1: no FoC team container sets it, so a squad takes one slot) and Garrison_Category.
    const objects::ObjectId k = s4_squad_of(unit);
    if (k == objects::kNoObjectId) return false;
    const auto it = std::lower_bound(units_.begin(), units_.end(), unit, [](const Unit& u, objects::ObjectId v) { return u.id < v; });
    const Container& c = containers_[it->container];
    const objects::GameObjectTypes& types = catalog_->types();
    g.id = k;
    g.value = 1;
    g.category = 0;
    if (c.type < types.size()) {
        const auto t = static_cast<objects::TypeIndex>(c.type);
        if (!types.get_string(t, "Garrison_Value").empty()) g.value = types.get_int(t, "Garrison_Value");
        g.category = types.get_mask(t, "Garrison_Category");
    }
    return true;
}

float TacticalBattle::s4_squad_radius(TacticalMode& mode, objects::ObjectId container) const {
    // 0x443ce0 on a TEAM object: the 3-D length of the half-extent vector of the box 0x3982b0 = the live union of the
    // members' model bounds (0x405320), world axis-aligned. It grows when the squad is strung out.
    const objects::ObjectStore& store = mode.primary();
    bool any = false;
    tactical::Aabb box{};
    for (const Unit& m : units_) {
        if (!m.alive || m.retreated || m.container >= containers_.size() || containers_[m.container].id != container) continue;
        if (!store.alive(m.h)) continue;
        const tactical::Aabb b = mode.bounds_of(m.h);
        if (!any) {
            box = b;
            any = true;
            continue;
        }
        box.min.x = std::min(box.min.x, b.min.x);
        box.min.y = std::min(box.min.y, b.min.y);
        box.min.z = std::min(box.min.z, b.min.z);
        box.max.x = std::max(box.max.x, b.max.x);
        box.max.y = std::max(box.max.y, b.max.y);
        box.max.z = std::max(box.max.z, b.max.z);
    }
    if (!any) return 0.0f;
    const float ex = 0.5f * (box.max.x - box.min.x), ey = 0.5f * (box.max.y - box.min.y), ez = 0.5f * (box.max.z - box.min.z);
    return dmath::sqrt(ex * ex + ey * ey + ez * ez);
}

void TacticalBattle::s4_owner_changed(TacticalMode& mode, objects::ObjectId id, int32_t, int32_t owner) {
    if (!s4_used_) return;
    const objects::ObjectStore& st = mode.primary();
    const objects::ObjectHandle h = st.find(id);
    const Vec3 p = st.exists(h) ? st.position(h) : Vec3{};
    const Vec3 f = st.exists(h) ? st.facing(h) : Vec3{1.0f, 0.0f, 0.0f};
    garrison_.set_owner(id, owner, p, f, frame_);  // ejects the occupants hostile to the new owner (applied at the next s4_tick)
}

bool TacticalBattle::s4_placement(TacticalMode& mode, objects::TypeIndex t, const objects::SpawnParams& sp, float health,
                                  objects::ObjectHandle* spawned) {
    if (spawned) *spawned = objects::ObjectHandle{};
    const objects::GameObjectTypes& types = catalog_->types();
    tg::CapturePointDesc cd;
    tg::HolderDesc hd;
    const bool capture = tg::capture_desc_from_type(types, t, cd);
    const bool holder = tg::holder_desc_from_type(types, t, hd);
    if (!capture && !holder) return false;
    s4_store_ = &mode.primary();
    s4_init();
    const objects::SpawnParams& p = sp;  // REMAINING_GAPS §3.3: the map owner (mini 02) is kept
    const objects::ObjectHandle h = mode.spawn(p, health);
    if (!h) return true;
    if (spawned) *spawned = h;
    const objects::ObjectId id = mode.primary().id(h);
    if (capture) captures_->add_point(id, p.owner, p.position, cd, frame_);
    if (holder) {
        const tactical::Aabb b = mode.extent_box(h);
        const float dx = b.max.x - b.min.x, dy = b.max.y - b.min.y;
        hd.half_diagonal = 0.5f * dmath::sqrt(dx * dx + dy * dy);
        garrison_.add_holder(id, p.owner, hd);
    }
    return true;
}

void TacticalBattle::s4_capture_reset(TacticalMode& mode, objects::ObjectId pad) {
    // REMAINING_GAPS §6.5 0x501050(capture behaviour, neutral player, -1): owner := neutral (store, trees, garrison state),
    // capture record controller := neutral. [port] skin / particle / reveal refreshes are presentation; no story event.
    if (!s4_used_ || !captures_ || !captures_->is_point(pad)) return;
    const int32_t neutral = galaxy_->neutral_player();
    if (neutral < 0) return;
    objects::ObjectStore& store = mode.primary();
    const objects::ObjectHandle h = store.find(pad);
    if (!store.alive(h)) return;
    if (const int32_t old = store.owner(h); old != neutral && !change_owner(mode, h, neutral)) {
        store.set_owner(h, neutral);
        mode.trees().set_owner(static_cast<tactical::EntityHandle>(pad), neutral);
        garrison_.set_owner(pad, neutral, store.position(h), store.facing(h), frame_);
        fog_owner_changed(mode, h, old);  // a1h t01: REVEAL vf18 of a non-unit point
    }
    captures_->reset(pad, neutral);
}

void TacticalBattle::s4_capture_grant(TacticalMode&, objects::ObjectId point, int32_t owner) {
    // S10 landing point [probe 2026-10-29]: the store owner was changed by the caller (script_change_owner); the capture
    // record follows (owner and controller := owner, progress cleared, as 0x501050).
    if (!s4_used_ || !captures_ || !captures_->is_point(point)) return;
    captures_->reset(point, owner);
}

bool TacticalBattle::s4_board(TacticalMode& mode, objects::ObjectId unit, objects::ObjectId holder) {
    // Vehicle_Thief (TACTICAL_GAPS §2.3.10): the thief is inside the capture variant at once; the Entered event is
    // applied now (movement removal, kUnitGarrisoned, garrison count) as for a walked-in unit.
    if (!s4_used_) return false;
    const objects::ObjectStore& store = mode.primary();
    const objects::ObjectHandle hh = store.find(holder);
    if (!store.alive(hh)) return false;
    if (!garrison_.board(unit, holder, store.position(hh), frame_)) return false;
    s4_apply(mode);
    return true;
}

void TacticalBattle::s4_register(TacticalMode& mode, const Unit& u) {
    const objects::GameObjectTypes& types = catalog_->types();
    const int32_t owner = mode.primary().owner(u.h);
    tg::HolderDesc hd;
    tg::OccupantDesc od;
    if (tg::holder_desc_from_type(types, u.type, hd)) {
        s4_store_ = &mode.primary();
        s4_init();
        const tactical::Aabb b = mode.extent_box(u.h);
        const float dx = b.max.x - b.min.x, dy = b.max.y - b.min.y;
        hd.half_diagonal = 0.5f * dmath::sqrt(dx * dx + dy * dy);
        garrison_.add_holder(u.id, owner, hd);
        abilities_->set_garrison_count(u.id, 0);
    }
    if (u.movable && tg::occupant_desc_from_type(types, u.type, od)) {
        s4_store_ = &mode.primary();
        s4_init();
        od.radius = mv::unit_desc_from_type(types, u.type).radius;
        garrison_.add_unit(u.id, owner, od);
    }
}

void TacticalBattle::s4_positions(TacticalMode& mode) {
    if (!s4_used_) return;
    objects::ObjectStore& store = mode.primary();
    for (const auto& [holder, unit] : garrison_.occupancy()) {
        const objects::ObjectHandle hh = store.find(holder), uh = store.find(unit);
        if (!store.alive(hh) || !store.alive(uh)) continue;
        const Vec3 p = store.position(hh);
        store.set_position(uh, p);
        mode.trees().update(static_cast<tactical::EntityHandle>(unit), mode.bounds_of(uh));
        if (combat_->has_unit(unit)) combat_->set_position(unit, to_tv(p));
    }
}

void TacticalBattle::s4_cancel(std::span<const objects::ObjectId> ids) {
    if (!s4_used_) return;
    for (const objects::ObjectId id : ids) {
        garrison_.cancel_enter(id);
        // s78n t18 (FIGHTER_FLYING §7.4): the squad approaches as one - a new order on one member ends the approach of all
        if (const objects::ObjectId k = s4_squad_of(id); k != objects::kNoObjectId)
            for (const Unit& m : units_)
                if (m.id != id && m.container < containers_.size() && containers_[m.container].id == k) garrison_.cancel_enter(m.id);
    }
}

void TacticalBattle::s4_set_owner(TacticalMode& mode, objects::ObjectId id, int32_t owner) {
    objects::ObjectStore& store = mode.primary();
    const objects::ObjectHandle h = store.find(id);
    if (!store.alive(h) || owner < 0) return;
    if (const int32_t old = store.owner(h); !change_owner(mode, h, owner)) {  // not a battle unit: store + trees (+ garrison state)
        store.set_owner(h, owner);
        mode.trees().set_owner(static_cast<tactical::EntityHandle>(id), owner);
        garrison_.set_owner(id, owner, store.position(h), store.facing(h), frame_);
        fog_owner_changed(mode, h, old);  // a1h t01: REVEAL vf18 of a non-unit point (FOG_OF_WAR §3.2)
    }
    if (captures_ && captures_->is_point(id) && captures_->owner(id) != owner) captures_->set_owner(id, owner);
}

void TacticalBattle::s4_leave(TacticalMode& mode, objects::ObjectId unit, Vec3 at, objects::ObjectId holder) {
    objects::ObjectStore& store = mode.primary();
    Unit* u = unit_by_id(unit);
    if (!u || !u->alive || u->retreated) return;
    if (!space_) at.z = map_.terrain.sample_height(at.x, at.y);
    store.set_position(u->h, at);
    mode.trees().update(static_cast<tactical::EntityHandle>(unit), mode.bounds_of(u->h));
    if (combat_->has_unit(unit)) {
        combat_->set_flag(unit, tc::kUnitGarrisoned, false);
        combat_->set_position(unit, to_tv(at));
        if (tc::ModifierSet* ms = combat_->unit_modifiers(unit)) ms->remove_source_in_list(tc::ModList::FireRange, kS4RangeSource, kS4RangeCrc);
    }
    if (u->movable && !movement_->has_unit(unit)) {
        mv::UnitDesc md = mv::unit_desc_from_type(catalog_->types(), u->type);
        md.locomotor = mv::locomotor_kind_from_type(catalog_->types(), u->type, space_ ? mv::MoveMode::Space : mv::MoveMode::Land);
        md.id = unit;
        md.pos = {at.x, at.y};
        md.z = at.z;
        md.yaw_deg = 0.0f;
        movement_->add_unit(md);
        s4_rejoin_squad(mode, *u, at);
        // s78n t18 (MOVEMENT §5.3 "Garrison host id"): the host id lasts until the unit's next order - standing beside its
        // former host it forms no collision object with it (a squad: its container, s4_apply)
        if (holder != objects::kNoObjectId) (void)movement_->set_garrison_exit(unit, holder);
    }
    // cu8j t02 (MOVEMENT §4.7.2): the garrison leave 0x37a060 adopts the unit (0x39bcb0, return 0x37a369): one ctor draw
    // when it qualifies (never a team member). [M: its place among the leave's other draws] cu8l t02 (CU8K_T02_T06_RE §3
    // rows 0x3abdf8 / 0x37a369, rule 7): the exit's reposition 0x3abb80 and the leave both adopt; [L] one draw is kept (the
    // second adopt would find the unit in the first one's coordinator when the leave runs after the reposition).
    if (bt_rng_) (void)mv_adopt(mode, u->h, *bt_rng_);
}

// s78n review (TACTICAL_COMBAT §5.4.4 item 2, L1_S4_REMAINING §3.5): a land squad member leaving a garrison steers to its
// old slot around its container again (MovementSystem::rejoin_land_team). s78n t14: where the squad leaves is placed by
// s4_squad_leave (the squad leaves as one); the member's layout shape is set again (a fresh mover).
void TacticalBattle::s4_rejoin_squad(TacticalMode&, const Unit& u, Vec3) {
    if (space_ || !movement_ || u.container >= containers_.size()) return;
    const Container& c = containers_[u.container];
    if (c.removed || !movement_->is_land_team_container(c.id) || !movement_->team_member_parked(u.id)) return;
    if (movement_->rejoin_land_team(u.id)) mv_team_shape(u);
}

// s78n t14 (FIGHTER_FLYING §7.4, GarrisonUnitBehavior entry 0x4446b0): retail garrisons the squad container K (K at the
// host position, its members at host + their old offsets, all hidden). [port] The port garrisons the members one by one
// (each parked with its offset from K: MovementSystem::park_team_member); when the last steering member of a squad is
// inside, its container joins the garrison: teleported (stopped) to the host position, where it stays.
void TacticalBattle::s4_squad_enter(TacticalMode& mode, const Unit& u, Vec3 host) {
    if (space_ || !movement_ || u.container >= containers_.size()) return;
    const Container& c = containers_[u.container];
    if (c.removed || !movement_->is_land_team_container(c.id) || !movement_->team_member_parked(u.id)) return;
    if (!movement_->land_team(c.id).empty()) return;  // members still outside: the container stays with them
    objects::ObjectStore& store = mode.primary();
    const objects::ObjectHandle ch = store.find(c.id);
    if (!store.alive(ch)) return;
    host.z = map_.terrain.sample_height(host.x, host.y);
    (void)teleport(mode, c.id, host, store.facing(ch));
}

// s78n t14 (FIGHTER_FLYING §7.4): where a leaving garrison occupant goes. Not a parked squad member, or one whose container
// is still outside with other members ([port] it rejoins from the unit's own exit point): the event position. Else the
// squad leaves as one (the first member out of an entirely garrisoned squad starts it, the rest follow in this batch):
//   * Exited (exit 0x444dd0, one call for the whole squad): the container is teleported to the exit point E - [port] the
//     host centre (the sim resolves no exit bones) - with its members at E + their offsets; the squad's other members in
//     the same host are ordered out with it (their events join this batch); after the batch the container is ordered to
//     the walk-out point X (the garrison's exit point: host + facing x Garrison_Exit_Dist) with flag 0x20 (s4_apply), so
//     its members get start delays and a re-lay at the move start;
//   * Ejected (host destroyed / release all 0x37a060): the container and every member at the host position, no walk-out.
Vec3 TacticalBattle::s4_squad_leave(TacticalMode& mode, const tg::GarrisonEvent& e, std::vector<SquadExit>& outs,
                                    std::vector<tg::GarrisonEvent>& events) {
    const Unit* u = unit_by_id(e.unit);
    if (space_ || !movement_ || !u || u->container >= containers_.size()) return e.position;
    const Container& c = containers_[u->container];
    if (c.removed || !movement_->is_land_team_container(c.id) || !movement_->team_member_parked(u->id)) return e.position;
    mv::V2 off{};
    (void)movement_->parked_offset(u->id, off);
    size_t k = 0;
    while (k < outs.size() && outs[k].container != c.id) ++k;
    if (k == outs.size()) {
        if (!movement_->land_team(c.id).empty()) return e.position;
        objects::ObjectStore& store = mode.primary();
        const objects::ObjectHandle hh = store.find(e.holder), ch = store.find(c.id);
        if (!store.alive(ch)) return e.position;
        const Vec3 hp = store.exists(hh) ? store.position(hh) : e.position;
        const Vec3 hf = store.exists(hh) ? store.facing(hh) : Vec3{1.0f, 0.0f, 0.0f};
        SquadExit so;
        so.container = c.id;
        so.holder = e.holder;
        so.at = Vec3{hp.x, hp.y, map_.terrain.sample_height(hp.x, hp.y)};
        so.to = e.position;
        so.walk = e.kind == tg::GarrisonEventKind::Exited;
        (void)teleport(mode, c.id, so.at, so.walk ? store.facing(ch) : hf);
        // s78n t18 (exit 0x444dd0 -> 0x37a8d0): the container is unhidden with its squad, at once (the per-tick feed of
        // apply_speed_factors comes later; a hidden container would refuse the walk-out order)
        (void)movement_->set_pair_gate(c.id, static_cast<mv::ObjectId>(e.holder), false);
        outs.push_back(so);
        if (so.walk) {  // the squad's other members inside the same host leave with it
            for (const Unit& m : units_)
                if (m.id != u->id && m.container == u->container && m.alive && !m.retreated &&
                    garrison_.holder_of(m.id) == e.holder)
                    (void)garrison_.order_exit(m.id, hp, hf, frame_);
            events.insert(events.end(), garrison_.events().begin(), garrison_.events().end());
            garrison_.clear_events();
        }
    }
    const SquadExit& so = outs[k];
    if (!so.walk) return so.at;
    return Vec3{so.at.x + off.x, so.at.y + off.y, so.at.z};
}

// cu8l t02 (CU8K_T02_T06_RE §2, rule 5): the garrison host's stop-and-hold (GarrisonVehicle / GarrisonHoverTransport vf28
// at a unit's reservation, vf29 when it ends [M], the approach 0x443700 in range): one draw through the adopt predicate
// for a movable host with a land locomotor; a structure (no locomotor) or a dead host: nothing.
void TacticalBattle::s4_hold(TacticalMode& mode, objects::ObjectId holder) {
    const Unit* h = unit_by_id(holder);
    if (!bt_rng_ || !movement_ || !h || !h->alive || h->retreated || !h->movable || !movement_->has_unit(holder)) return;
    (void)mv_adopt(mode, h->h, *bt_rng_, true);
}

void TacticalBattle::s4_apply(TacticalMode& mode) {
    objects::ObjectStore& store = mode.primary();
    std::vector<tg::GarrisonEvent> events(garrison_.events().begin(), garrison_.events().end());
    garrison_.clear_events();
    std::vector<SquadExit> outs;  // s78n t14: the squads leaving in this batch (s4_squad_leave)
    // s78n t18 (FIGHTER_FLYING §7.4): a squad enters as one object - its container is stopped once and the host's hold
    // is drawn once per squad and event kind in a batch, not once per member.
    std::vector<std::pair<objects::ObjectId, tg::GarrisonEventKind>> squad_done;
    const auto squad_first = [&](objects::ObjectId unit, tg::GarrisonEventKind kind, objects::ObjectId& k) {
        k = s4_squad_of(unit);
        if (k == objects::kNoObjectId) return true;  // not a squad member: every event counts
        const std::pair<objects::ObjectId, tg::GarrisonEventKind> key{k, kind};
        if (std::find(squad_done.begin(), squad_done.end(), key) != squad_done.end()) return false;
        squad_done.push_back(key);
        return true;
    };
    // s78n t14: index loop - a squad leave appends its members' exits (and [port] any other event pending then).
    for (size_t ei = 0; ei < events.size(); ++ei) {
        const tg::GarrisonEvent e = events[ei];
        switch (e.kind) {
            case tg::GarrisonEventKind::EnterStarted: {  // the host stops the unit at the approach point
                // s78n t18 (FIGHTER_FLYING §7.4, entry 0x4446b0): a squad enters as its container - the container is
                // stopped (vf +0x140: speed 0 at once), once; its members keep steering to their slots while they fade.
                objects::ObjectId k = objects::kNoObjectId;
                if (!squad_first(e.unit, e.kind, k)) break;
                const objects::ObjectId one[] = {k != objects::kNoObjectId ? k : e.unit};
                movement_->stop(one);
                // cu8l t02 (CU8K_T02_T06_RE §2 row 0x443780, rule 5): in range with room, the approach 0x443700 stops and
                // holds the HOST (a garrison vehicle / hover transport: one draw; a structure has no locomotor: none).
                s4_hold(mode, e.holder);
                break;
            }
            case tg::GarrisonEventKind::Entered: {
                Unit* u = unit_by_id(e.unit);
                if (!u) break;
                // cu8l t02 (CU8K_T02_T06_RE §2 row 0x44474f, §3 row 0x3abdf8, rule 5): the entry 0x4446b0 moves the unit
                // into the host (the eject / reposition 0x3abb80: leave, move, adopt) and then stops and holds it: two
                // draws for a unit that is not a squad member. [M] then the host's reservation ends (vf29: its hold).
                if (bt_rng_ && u->alive && u->movable && movement_->has_unit(e.unit)) {
                    (void)mv_adopt(mode, u->h, *bt_rng_, true);
                    (void)mv_adopt(mode, u->h, *bt_rng_, true);
                }
                if (objects::ObjectId k = objects::kNoObjectId; squad_first(e.unit, e.kind, k)) s4_hold(mode, e.holder);
                // s78n review: a land squad member keeps its team place (container, slot, team index) for the leave.
                if (!movement_->park_team_member(e.unit)) movement_->remove_unit(e.unit);
                s4_squad_enter(mode, *u, e.position);  // s78n t14: the last member in takes the container in
                store.set_position(u->h, e.position);
                mode.trees().update(static_cast<tactical::EntityHandle>(e.unit), mode.bounds_of(u->h));
                if (combat_->has_unit(e.unit)) {
                    combat_->set_flag(e.unit, tc::kUnitGarrisoned, true);
                    combat_->clear_order(e.unit);
                    combat_->set_position(e.unit, to_tv(e.position));
                    const float r = garrison_.can_garrison_fire(e.holder) ? (catalog_->garrisoned_range_multiplier() >= 0.0f ? catalog_->garrisoned_range_multiplier() : cfg_.garrisoned_range_multiplier) - 1.0f : kNoGarrisonFire;
                    if (tc::ModifierSet* ms = combat_->unit_modifiers(e.unit))
                        ms->set_by_source(tc::ModList::FireRange, kS4RangeCategory, r, tc::kPermanent, kS4RangeCrc, kS4RangeSource);
                }
                if (abilities_->has_object(e.holder))
                    abilities_->set_garrison_count(e.holder, static_cast<int32_t>(garrison_.occupants(e.holder).size()));
                story_garrisoned_.push_back({e.unit, e.holder});
                break;
            }
            case tg::GarrisonEventKind::Exited:
            case tg::GarrisonEventKind::Ejected:
                s4_leave(mode, e.unit, s4_squad_leave(mode, e, outs, events), e.holder);  // s78n t14: a squad leaves as one
                if (abilities_->has_object(e.holder))
                    abilities_->set_garrison_count(e.holder, static_cast<int32_t>(garrison_.occupants(e.holder).size()));
                break;
            case tg::GarrisonEventKind::Killed: {  // host without a garrison behaviour died
                Unit* u = unit_by_id(e.unit);
                if (!u || !u->alive) break;
                if (combat_->has_unit(e.unit)) combat_->kill_unit(e.unit, tc::kNoObject, tc::kKindNormal, frame_);
                u->alive = false;
                --parts_[u->participant].alive;
                mode.destroy(u->h);
                movement_->remove_unit(e.unit);
                ++stats_.deaths;
                unit_gone(mode, *u);
                break;
            }
            case tg::GarrisonEventKind::OrderDropped:  // cu8l t02 [M]: the host's reservation ends (vf29: its hold)
                if (objects::ObjectId k = objects::kNoObjectId; squad_first(e.unit, e.kind, k)) s4_hold(mode, e.holder);
                break;
        }
    }
    // s78n t14 (FIGHTER_FLYING §7.4): a squad that left (Exited) walks out: Move_To the walk-out point X with flag 0x20
    // through the coordinator (the container is always admitted, 0x425e30); [port] without the battle RNG the container
    // and its members are teleported there instead (0x3abb80(K, X): the offsets kept).
    for (const SquadExit& so : outs) {
        if (!so.walk || !store.alive(store.find(so.container))) continue;
        const objects::ObjectId one[] = {so.container};
        if (bt_rng_) {
            exec_move(mode, one, so.to.x, so.to.y, *bt_rng_, mv::kNoFacing, false, mv::kMoveFlagSkipValidation);
        } else {
            // s78n t18 (0x3abb80): the container's teleport takes its members along, each at its offset
            const Vec3 to{so.to.x, so.to.y, map_.terrain.sample_height(so.to.x, so.to.y)};
            (void)teleport(mode, so.container, to, store.facing(store.find(so.container)));
        }
        // s78n t18 (MOVEMENT §5.3 "Garrison host id"): the squad's host id lasts through the walk-out (and, standing, until
        // its next order): no collision object with the host it leaves from under
        (void)movement_->set_garrison_exit(so.container, so.holder);
    }
    if (captures_) {
        const std::vector<tg::CaptureEvent> caps(captures_->events().begin(), captures_->events().end());
        captures_->clear_events();
        for (const tg::CaptureEvent& c : caps) {
            vm_cp_capture(c.point, c.new_owner);  // ccpgt t02: 0x360d90 runs before the owner is written
            s4_set_owner(mode, c.point, c.new_owner);
            story_captured_.push_back({c.point, c.old_owner, c.new_owner});
        }
    }
    // s78n t18 review: who is inside / walking in changed - the movement pair gates and "hidden" follow at once (a
    // command right after this batch, or a save and load, meets the same gates)
    if (!events.empty()) feed_pair_gates();
}

void TacticalBattle::s4_tick(TacticalMode& mode, SimRng&) {
    if (!s4_used_) return;
    objects::ObjectStore& store = mode.primary();
    s4_store_ = &store;  // s78n t12 review: a loaded battle whose own setup registered nothing has none yet
    s4_apply(mode);  // owner changes since the last tick (ejections)
    // 1. dead / retreated battle units leave the garrison state (a host ejects / kills its occupants).
    for (const Unit& u : units_) {
        if (u.alive && !u.retreated) continue;
        if (captures_ && captures_->is_point(u.id)) captures_->remove(u.id);  // a destroyed capture structure (combat unit)
        if (!garrison_.is_holder(u.id) && !garrison_.is_unit(u.id)) continue;
        Vec3 at = store.exists(u.h) ? store.position(u.h) : Vec3{};
        const Vec3 f = store.exists(u.h) ? store.facing(u.h) : Vec3{1.0f, 0.0f, 0.0f};
        // s78n t12 review: a host killed between ticks (script_damage) is flushed from the store before this runs; its
        // occupants still sit where it stood (s4_positions), so the first live one gives the place they leave from.
        if (!store.exists(u.h))
            for (const objects::ObjectId in : garrison_.occupants(u.id))
                if (const objects::ObjectHandle ih = store.find(in); store.alive(ih)) {
                    at = store.position(ih);
                    break;
                }
        garrison_.remove(u.id, at, f, frame_);
    }
    s4_apply(mode);
    // 2. approaches / entering units.
    garrison_.service(
        frame_,
        [&store](tg::ObjectId id, Vec3& p, Vec3& f) {
            const objects::ObjectHandle h = store.find(id);
            if (!store.alive(h)) return false;
            p = store.position(h);
            f = store.facing(h);
            return true;
        },
        // s78n t18 (FIGHTER_FLYING §7.4 item 4): a squad's range test - the radius of its members' live union box, and
        // whether its container's approach move has ended
        [&](tg::ObjectId group, tg::GroupReach& gr) {
            gr.radius = s4_squad_radius(mode, group);
            gr.arrived = movement_ && movement_->has_unit(group) && !movement_->view(group).has_goal;
            return true;
        });
    s4_apply(mode);
    // 3. capture points: live, non-retreated, non-garrisoned battle units with an owner and Influences_Capture_Point.
    if (captures_) {
        std::vector<tg::Influencer> inf;
        const objects::GameObjectTypes& types = catalog_->types();
        for (const Unit& u : units_) {
            if (!u.alive || u.retreated || garrison_.garrisoned(u.id) || !store.alive(u.h)) continue;
            const int32_t owner = store.owner(u.h);
            if (owner < 0 || !tg::influences_capture(types, u.type)) continue;
            inf.push_back({u.id, owner, store.position(u.h)});
        }
        captures_->service(frame_, inf);
        s4_apply(mode);
    }
}

cmd::RejectReason TacticalBattle::s4_command(TacticalMode& mode, uint8_t player, const cmd::EventData& ev, SimRng& rng) {
    using T = cmd::EventType;
    auto id_of = [](uint32_t w) { return (w == cmd::kNoObject || w > 0x7FFFFFFFu) ? objects::kNoObjectId : static_cast<objects::ObjectId>(w); };
    if (ev.type() == T::Garrison) {
        const objects::ObjectId unit = id_of(ev.u("unit")), holder = id_of(ev.u("garrison"));
        const objects::ObjectId one[] = {unit};
        if (unit == objects::kNoObjectId) {  // §3.5: unit 0x3FFFFF = eject every occupant of the host
            const objects::ObjectHandle hh = mode.primary().find(holder);
            if (!mode.primary().alive(hh) || mode.primary().owner(hh) != static_cast<int32_t>(player)) return cmd::RejectReason::InvalidTarget;
            return eject_garrison(mode, holder) > 0 ? cmd::RejectReason::None : cmd::RejectReason::InvalidUnit;
        }
        if (holder == objects::kNoObjectId) return order_leave_garrison(mode, player, one);
        return order_garrison(mode, player, one, holder, rng);
    }
    // MoveToGarrison (55) [port field use]: a = the host, b / c = units (kNoObject = none).
    std::vector<objects::ObjectId> units;
    for (const char* f : {"b", "c"})
        if (const objects::ObjectId id = id_of(ev.u(f)); id != objects::kNoObjectId) units.push_back(id);
    return order_garrison(mode, player, units, id_of(ev.u("a")), rng);
}

void TacticalBattle::s4_hash(StateHasher& h) const {
    if (!s4_used_) return;
    h.u32(0x53344741u);
    garrison_.hash(h);
    h.boolean(captures_ != nullptr);
    if (captures_) captures_->hash(h);
}

bool TacticalBattle::can_garrison(objects::ObjectId unit, objects::ObjectId holder) const {
    const auto it = std::lower_bound(units_.begin(), units_.end(), unit, [](const Unit& u, objects::ObjectId v) { return u.id < v; });
    if (it == units_.end() || it->id != unit || !it->alive || it->retreated) return false;
    return garrison_.can_garrison(unit, holder);
}
bool TacticalBattle::can_garrison_fire(objects::ObjectId id) const { return garrison_.can_garrison_fire(id); }
objects::ObjectId TacticalBattle::garrison_holder_of(objects::ObjectId unit) const {
    const tg::ObjectId h = garrison_.holder_of(unit);
    return h == tg::kNoObject ? objects::kNoObjectId : h;
}
std::vector<objects::ObjectId> TacticalBattle::garrisoned_units(objects::ObjectId holder) const {
    const auto in = garrison_.occupants(holder);
    return {in.begin(), in.end()};
}

cmd::RejectReason TacticalBattle::order_garrison(TacticalMode& mode, uint8_t player, std::span<const objects::ObjectId> units,
                                                 objects::ObjectId holder, SimRng& rng) {
    using R = cmd::RejectReason;
    if (finished_ || !movement_) return R::NotAllowed;
    objects::ObjectStore& store = mode.primary();
    const objects::ObjectHandle hh = store.find(holder);
    if (!store.alive(hh) || !garrison_.is_holder(holder)) return R::InvalidTarget;
    // s78n t19: a vehicle a thief boarded is no garrison (HolderKind::VehicleThief: entered only through the boarding) and
    // a board-only unit (no GARRISON_UNIT: kBoardOnlyValue) takes no garrison order. Both are refused here, before the
    // queues are cleared and the units are sent walking (GarrisonSystem::order_enter would refuse them only after that).
    if (const tg::HolderDesc* thd = garrison_.holder_desc(holder); thd && thd->kind == tg::HolderKind::VehicleThief) return R::InvalidTarget;
    // s78n t18 (FIGHTER_FLYING §7.4): a squad garrisons as one object - an order on any member orders every live member
    std::vector<objects::ObjectId> all(units.begin(), units.end());
    for (const objects::ObjectId id : units)
        if (const objects::ObjectId k = s4_squad_of(id); k != objects::kNoObjectId)
            for (const Unit& m : units_)
                if (m.container < containers_.size() && containers_[m.container].id == k &&
                    std::find(all.begin(), all.end(), m.id) == all.end())
                    all.push_back(m.id);
    std::vector<objects::ObjectId> ids;
    bool refused_host = false;
    for (const objects::ObjectId id : all) {
        Unit* u = unit_by_id(id);
        if (!u || !u->alive || u->retreated || !u->movable || store.owner(u->h) != static_cast<int32_t>(player)) continue;
        if (std::find(ids.begin(), ids.end(), id) != ids.end() || garrison_.garrisoned(id) || garrison_.entering(id)) continue;
        if (const tg::OccupantDesc* od = garrison_.unit_desc(id); od && od->value == tg::kBoardOnlyValue) continue;  // s78n t19
        if (!garrison_.masks_match(id, holder)) continue;
        // s78n t06 review [port]: a hero in a clash (+0x3b4) is not sent into a garrison (its locked movement keeps it in
        // the duel; retail's group move leaves a locked unit out, so it never reaches the host).
        if (combat_->has_unit(id) && (combat_->flags(id) & tc::kUnitHeroClash)) continue;
        // s78n t18 review: a host that would refuse the unit's approach (hostile to it: GarrisonSystem::order_enter's
        // rule) is refused here too, before the queues are cleared and the units are sent walking
        if (!garrison_.enter_allowed(id, holder)) {
            refused_host = true;
            continue;
        }
        ids.push_back(id);
    }
    if (ids.empty()) return refused_host ? R::InvalidTarget : R::InvalidUnit;  // InvalidTarget: hostile host
    clear_queues(ids);
    raise_order_replaced(ids);  // S7 t31: garrison enter (A1_READINGS §33.3 [M: raised at the order])
    const Vec3 a = garrison_.approach_point(holder, store.position(hh), store.facing(hh));
    exec_move(mode, ids, a.x, a.y, rng, mv::kNoFacing, false);  // group move to the approach point (cancels older ones)
    uint32_t n = 0;
    std::vector<objects::ObjectId> squads;  // s78n t18: a squad reserves once (one hold per squad, not per member)
    for (const objects::ObjectId id : ids)
        if (garrison_.order_enter(id, holder)) {
            ++n;
            const objects::ObjectId k = s4_squad_of(id);
            if (k != objects::kNoObjectId) {
                if (std::find(squads.begin(), squads.end(), k) != squads.end()) continue;
                squads.push_back(k);
            }
            s4_hold(mode, holder);  // cu8l t02 (CU8K_T02_T06_RE §2 rows 0x635efb / 0x635f7b): the reservation's host hold
        }
    // s78n t18 (MOVEMENT §5.3 "Hover state", GARRISON_HOVER_TRANSPORT vf +0xe0 0x635ee0): a hover transport whose slot a
    // unit reserves lowers
    if (n > 0)
        if (const tg::HolderDesc* hd = garrison_.holder_desc(holder); hd && hd->kind == tg::HolderKind::Hover) (void)movement_->hover_lower(holder);
    return n > 0 ? R::None : R::InvalidTarget;  // hostile host
}

cmd::RejectReason TacticalBattle::order_leave_garrison(TacticalMode& mode, uint8_t player, std::span<const objects::ObjectId> units) {
    using R = cmd::RejectReason;
    if (finished_) return R::NotAllowed;
    objects::ObjectStore& store = mode.primary();
    uint32_t n = 0;
    for (const objects::ObjectId id : units) {
        const Unit* u = unit_by_id(id);
        if (!u || !u->alive || u->retreated || store.owner(u->h) != static_cast<int32_t>(player)) continue;
        tg::ObjectId holder = garrison_.holder_of(id);
        if (holder == tg::kNoObject) holder = garrison_.pending_holder(id);
        if (holder == tg::kNoObject) continue;
        const objects::ObjectHandle hh = store.find(holder);
        const Vec3 p = store.exists(hh) ? store.position(hh) : store.position(u->h);
        const Vec3 f = store.exists(hh) ? store.facing(hh) : Vec3{1.0f, 0.0f, 0.0f};
        if (garrison_.order_exit(id, p, f, frame_)) {
            ++n;
            raise_order_replaced(id);  // S7 t31: garrison exit (A1_READINGS §33.3)
            // s78n t18 (GARRISON_HOVER_TRANSPORT vf +0xe8 0x635fc0): a hover transport asked to eject lowers
            if (const tg::HolderDesc* hd = garrison_.holder_desc(holder); movement_ && hd && hd->kind == tg::HolderKind::Hover)
                (void)movement_->hover_lower(holder);
        }
    }
    s4_apply(mode);
    return n > 0 ? R::None : R::InvalidUnit;
}

uint32_t TacticalBattle::eject_garrison(TacticalMode& mode, objects::ObjectId holder) {
    if (finished_) return 0;
    objects::ObjectStore& store = mode.primary();
    const objects::ObjectHandle hh = store.find(holder);
    if (!store.exists(hh)) return 0;
    const uint32_t n = garrison_.eject_all(holder, store.position(hh), store.facing(hh), frame_);
    // s78n t18 (GARRISON_HOVER_TRANSPORT vf +0xe8): a hover transport asked to eject lowers
    if (const tactical::garrison::HolderDesc* hd = garrison_.holder_desc(holder); n > 0 && movement_ && hd && hd->kind == tactical::garrison::HolderKind::Hover)
        (void)movement_->hover_lower(holder);
    s4_apply(mode);
    return n;
}


}  // namespace alamo::sim
