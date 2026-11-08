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
    if (s4_used_) return;
    s4_used_ = true;
    const int32_t neutral = galaxy_->neutral_player();
    auto allied = [this](int32_t a, int32_t b) { return a == b || (combat_ && !combat_->is_enemy(a, b)); };
    garrison_.set_allied(allied);
    garrison_.set_neutral_player(neutral);
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
    for (const objects::ObjectId id : ids) garrison_.cancel_enter(id);
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

void TacticalBattle::s4_leave(TacticalMode& mode, objects::ObjectId unit, Vec3 at) {
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
    }
    // cu8j t02 (MOVEMENT §4.7.2): the garrison leave 0x37a060 adopts the unit (0x39bcb0, return 0x37a369): one ctor draw
    // when it qualifies (never a team member). [M: its place among the leave's other draws]
    if (bt_rng_) (void)mv_adopt(mode, u->h, *bt_rng_);
}

void TacticalBattle::s4_apply(TacticalMode& mode) {
    objects::ObjectStore& store = mode.primary();
    const std::vector<tg::GarrisonEvent> events(garrison_.events().begin(), garrison_.events().end());
    garrison_.clear_events();
    for (const tg::GarrisonEvent& e : events) {
        switch (e.kind) {
            case tg::GarrisonEventKind::EnterStarted: {  // the host stops the unit at the approach point
                const objects::ObjectId one[] = {e.unit};
                movement_->stop(one);
                break;
            }
            case tg::GarrisonEventKind::Entered: {
                Unit* u = unit_by_id(e.unit);
                if (!u) break;
                movement_->remove_unit(e.unit);
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
                s4_leave(mode, e.unit, e.position);
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
            case tg::GarrisonEventKind::OrderDropped: break;
        }
    }
    if (captures_) {
        const std::vector<tg::CaptureEvent> caps(captures_->events().begin(), captures_->events().end());
        captures_->clear_events();
        for (const tg::CaptureEvent& c : caps) {
            s4_set_owner(mode, c.point, c.new_owner);
            story_captured_.push_back({c.point, c.old_owner, c.new_owner});
        }
    }
}

void TacticalBattle::s4_tick(TacticalMode& mode, SimRng&) {
    if (!s4_used_) return;
    objects::ObjectStore& store = mode.primary();
    s4_apply(mode);  // owner changes since the last tick (ejections)
    // 1. dead / retreated battle units leave the garrison state (a host ejects / kills its occupants).
    for (const Unit& u : units_) {
        if (u.alive && !u.retreated) continue;
        if (captures_ && captures_->is_point(u.id)) captures_->remove(u.id);  // a destroyed capture structure (combat unit)
        if (!garrison_.is_holder(u.id) && !garrison_.is_unit(u.id)) continue;
        const Vec3 at = store.exists(u.h) ? store.position(u.h) : Vec3{};
        const Vec3 f = store.exists(u.h) ? store.facing(u.h) : Vec3{1.0f, 0.0f, 0.0f};
        garrison_.remove(u.id, at, f, frame_);
    }
    s4_apply(mode);
    // 2. approaches / entering units.
    garrison_.service(frame_, [&store](tg::ObjectId id, Vec3& p, Vec3& f) {
        const objects::ObjectHandle h = store.find(id);
        if (!store.alive(h)) return false;
        p = store.position(h);
        f = store.facing(h);
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
    std::vector<objects::ObjectId> ids;
    for (const objects::ObjectId id : units) {
        Unit* u = unit_by_id(id);
        if (!u || !u->alive || u->retreated || !u->movable || store.owner(u->h) != static_cast<int32_t>(player)) continue;
        if (std::find(ids.begin(), ids.end(), id) != ids.end() || garrison_.garrisoned(id) || garrison_.entering(id)) continue;
        if (!garrison_.masks_match(id, holder)) continue;
        ids.push_back(id);
    }
    if (ids.empty()) return R::InvalidUnit;
    clear_queues(ids);
    raise_order_replaced(ids);  // S7 t31: garrison enter (A1_READINGS §33.3 [M: raised at the order])
    const Vec3 a = garrison_.approach_point(holder, store.position(hh), store.facing(hh));
    exec_move(mode, ids, a.x, a.y, rng, mv::kNoFacing, false);  // group move to the approach point (cancels older ones)
    uint32_t n = 0;
    for (const objects::ObjectId id : ids) n += garrison_.order_enter(id, holder) ? 1u : 0u;
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
    s4_apply(mode);
    return n;
}


}  // namespace alamo::sim
