// R3 presence-induced idle + EXPLOSION flinch in the battle host (docs/re/ANIM_PARTICLES.md §A.11 / §A.12; the systems
// are engine/tactical/presence). The battle supplies the PresenceWorld (read-only view of its units), registers the
// IMPOSING_PRESENCE units at attach, runs the presence service in the object-service phase, feeds the presence-induced
// idle (77 Attention / 78 Celebrate) to SimAnimation's default-idle hook and turns EXPLOSION objects into flinches.
// Without ALAMO_SIM_HAVE_PRESENCE (no engine/tactical/presence or no engine/anim) every entry point is a no-op: the
// presence maps feed nothing but the animation.
//
// Determinism: the only draw is the IMPOSING_PRESENCE first-due draw at attach (synced RNG, attach order); explosion
// flinches go through SimAnimation::hit (the flinch draws) in victim-id order. Hash / save absent until a presence
// unit was registered, so battles without IMPOSING_PRESENCE keep their bytes.
#include "alamo/sim/tactical_battle.hpp"

#include "alamo/core/hash.hpp"
#include "alamo/objects/game_object_types.hpp"
#ifdef ALAMO_SIM_HAVE_PRESENCE
#include <algorithm>

#include "alamo/anim/anim_types.hpp"
#include "alamo/sim/sim_anim.hpp"
#include "alamo/tactical/presence/explosion_flinch.hpp"
#include "alamo/tactical/presence/presence_data.hpp"
#endif

namespace alamo::sim {

#ifndef ALAMO_SIM_HAVE_PRESENCE

void TacticalBattle::presence_setup(TacticalMode&) {}
void TacticalBattle::presence_adopt(const Unit&, SimRng&) {}
void TacticalBattle::presence_adopt_object(objects::ObjectId, uint32_t, SimRng&) {}
void TacticalBattle::presence_removed(objects::ObjectId) {}
int32_t TacticalBattle::presence_idle_of(objects::ObjectId) const { return -1; }
void TacticalBattle::presence_tick(TacticalMode&) {}
void TacticalBattle::presence_explosion(TacticalMode&, int32_t, Vec3) {}
void TacticalBattle::presence_hash(StateHasher&) const {}
void TacticalBattle::presence_detach() {}

#else

namespace ps = alamo::tactical::presence;

namespace {
constexpr int8_t kKindPresence = 1;   // IMPOSING_PRESENCE (0x40)
constexpr int8_t kKindExplosion = 2;  // EXPLOSION (generic Behavior list; shares id 0x20 with INDIGENOUS_UNIT)
constexpr int8_t kKindIdle = 4;       // IDLE (0x07)
constexpr objects::BehaviorTypeId kImposingPresence = 0x40;
}  // namespace

void TacticalBattle::presence_setup(TacticalMode& mode) {
    presence_view_.b = this;
    presence_view_.mode = &mode;
    // The IMPOSING_PRESENCE behaviour is sim state (its attach draw, its maps) with or without an animation service;
    // only the idle hook and the EXPLOSION flinches need one.
    const objects::GameObjectTypes& types = catalog_->types();
    presence_.set_types(ps::build_presence_types(types, &anim::find_anim_type));
    ps::PresenceConfig cfg;
    cfg.fps = 30;  // the battle's frame rate (retreat / ability timers use 30 too)
    cfg.relaxation_seconds = catalog_->presence_relaxation_seconds();
    presence_.set_config(cfg);
    presence_kind_.assign(types.size(), 0);
    for (size_t i = 0; i < types.size(); ++i) {
        const auto t = static_cast<objects::TypeIndex>(i);
        int8_t k = 0;
        if (types.has_behavior(t, objects::BehaviorSet::Union, kImposingPresence)) k = static_cast<int8_t>(k | kKindPresence);
        if (types.behavior_count(t, "EXPLOSION") > 0) k = static_cast<int8_t>(k | kKindExplosion);
        if (types.has_behavior(t, objects::BehaviorSet::Union, objects::beh::Idle)) k = static_cast<int8_t>(k | kKindIdle);
        presence_kind_[i] = k;
    }
    // 0x395920 step 2 through SimAnimation's default-idle hook (71 Idle Crouch comes from LocomotorInput::crouched and
    // wins over it). presence_idle, not default_idle: an empty map must leave the animation's own default (0 Idle).
    if (!anim_) return;
    presence_link_ = std::make_shared<const TacticalBattle*>(this);
    anim_->set_presence_idle_hook([link = presence_link_](const GameMode& m, objects::ObjectId id) -> int32_t {
        const TacticalBattle* b = *link;
        if (!b || &m != static_cast<const GameMode*>(b->presence_view_.mode)) return anim::kNoAnimType;
        return b->presence_.presence_idle(id, b->presence_view_);
    });
}

void TacticalBattle::presence_adopt(const Unit& u, SimRng& rng) { presence_adopt_object(u.id, u.type, rng); }

void TacticalBattle::presence_adopt_object(objects::ObjectId id, uint32_t type, SimRng& rng) {
    // IMPOSING_PRESENCE attach (TACTICAL_GAPS §6.4): the first-due draw, synced Get_Int(0, 29), in attach order.
    if (type >= presence_kind_.size() || (presence_kind_[type] & kKindPresence) == 0) return;
    if (presence_.add_presence_unit(id, frame_, &rng)) presence_used_ = true;
}

void TacticalBattle::presence_removed(objects::ObjectId id) {
    if (presence_used_) presence_.remove_object(id);
}

int32_t TacticalBattle::presence_idle_of(objects::ObjectId id) const {
    return presence_used_ && presence_view_.mode ? presence_.presence_idle(id, presence_view_) : ps::kNoAnimType;
}

void TacticalBattle::presence_tick(TacticalMode& mode) {
    if (!presence_used_) return;
    presence_view_.mode = &mode;
    // Objects that left the battle drop their registration and map (remove_object is a no-op for unknown ids).
    for (const Unit& u : units_)
        if (!u.alive || u.retreated) presence_.remove_object(u.id);
    // Object-service phase: per object (descending id) the IDLE map upkeep, then the IMPOSING_PRESENCE service.
    // [port] The IDLE upkeep's side effects (ClearedTarget -> the IDLE service's combat timer, ClearedMoving -> drop
    // the flinch hold) are not forwarded: update() does not return them and anim has no setter for either.
    presence_.update(frame_, presence_view_);
}

void TacticalBattle::presence_explosion(TacticalMode& mode, int32_t type, Vec3 pos) {
    // §A.12: an EXPLOSION object's Initialise flinches every IDLE object in the cube of half-extent
    // Induced_Flinch_Range; no owner / dying / self filter here (the flinch gates are anim's).
    if (!anim_ || !bt_rng_ || type < 0 || static_cast<size_t>(type) >= presence_kind_.size() ||
        (presence_kind_[static_cast<size_t>(type)] & kKindExplosion) == 0)
        return;
    const anim::AnimWorld* w = anim_->find_world(mode);
    if (!w) return;
    presence_view_.mode = &mode;
    const ps::ExplosionFlinchParams p = ps::explosion_flinch_params(catalog_->types(), static_cast<ps::TypeId>(type));
    for (const ps::ExplosionFlinch& f : ps::explosion_flinches(pos.x, pos.y, pos.z, p.range, p.intensity, presence_view_)) {
        if (!w->contains(f.victim) || !combat_ || !combat_->has_unit(f.victim)) continue;
        const tactical::Vec3 fc = combat_->facing(f.victim);
        ++explosion_victims_;
        // [port] D is passed 2-D (the flinch picks its clip from yaw(D) - facing; dz does not enter it).
        if (anim_->hit(mode, f.victim, Vec3{fc.x, fc.y, fc.z}, Vec3{f.dx, f.dy, 0.0f}, *bt_rng_, oi_global_frame_, f.intensity))
            ++stats_.anim_plays;
    }
}

void TacticalBattle::presence_hash(StateHasher& h) const {
    if (presence_used_) presence_.hash(h);
}

void TacticalBattle::presence_detach() {
    if (presence_link_) *presence_link_ = nullptr;
}

// ---- PresenceWorld view ---------------------------------------------------------------------------------------------

bool TacticalBattle::PresenceView::unit(ps::ObjectId id, ps::UnitView& out) const {
    if (!b || !mode) return false;
    const auto it = std::lower_bound(b->units_.begin(), b->units_.end(), id,
                                     [](const Unit& u, objects::ObjectId v) { return u.id < v; });
    // [port] A unit the battle took out (dead or retreated) is gone; the original keeps dying objects live until their
    // removal, the port's dying unit hands its body to the death clone / corpse row.
    const objects::ObjectStore& st = mode->primary();
    if (it == b->units_.end() || it->id != id) {
        // Not a battle unit: a death clone object (R3 review 4: the FoC AT-AT clones are IMPOSING_PRESENCE units). It is
        // dying (+0x3a0 & 0x40), has no target and no locomotor; other store objects (particles, projectiles) are not
        // presence participants and stay invisible here.
        const objects::ObjectHandle h = st.find(id);
        if (!st.alive(h) || !st.dying(h)) return false;
        const Vec3 p = st.position(h);
        out = ps::UnitView{};
        out.type = static_cast<ps::TypeId>(st.type(h));
        out.owner = st.owner(h);
        out.x = p.x;
        out.y = p.y;
        out.z = p.z;
        out.dying = true;
        out.has_idle = out.type < b->presence_kind_.size() && (b->presence_kind_[out.type] & kKindIdle) != 0;
        return true;
    }
    if (!it->alive || it->retreated) return false;
    if (!st.exists(it->h)) return false;
    const Vec3 p = st.position(it->h);
    out = ps::UnitView{};
    out.type = it->type;
    out.owner = st.owner(it->h);
    out.x = p.x;
    out.y = p.y;
    out.z = p.z;
    out.dying = st.dying(it->h);
    out.has_idle = it->type < b->presence_kind_.size() && (b->presence_kind_[it->type] & kKindIdle) != 0;
    // TARGETING: the unit's current target (ordered, else auto). [port] The parent fallback for units without their own
    // TARGETING (0x62e450) is not modelled: every battle unit has its own combat record.
    out.has_target = b->combat_ && b->combat_->has_unit(id) && b->combat_->current_target(id) != tactical::combat::kNoObject;
    out.has_locomotor = it->movable;
    out.moving = it->movable && b->movement_ && b->movement_->has_unit(id) && b->movement_->view(id).speed > 0.0f;
    // [port] Deployed only: the abilities keep no deploying / undeploying state (the transition is the clip).
    out.deploy_busy = b->abilities_ && b->abilities_->deployed(id);
    return true;
}

void TacticalBattle::PresenceView::box_query(float cx, float cy, float cz, float h, std::vector<ps::ObjectId>& out) const {
    if (!b || !mode) return;
    const objects::ObjectStore& st = mode->primary();
    for (const Unit& u : b->units_) {
        if (!u.alive || u.retreated || !st.exists(u.h)) continue;
        const Vec3 p = st.position(u.h);
        if (p.x >= cx - h && p.x <= cx + h && p.y >= cy - h && p.y <= cy + h && p.z >= cz - h && p.z <= cz + h)
            out.push_back(u.id);
    }
}

bool TacticalBattle::PresenceView::allied(int32_t a, int32_t c) const {
    // Owner relation 0 (0x2823e0): same owner, or two players that are not enemies.
    if (a == c) return true;
    return b && b->combat_ && a >= 0 && c >= 0 && !b->combat_->is_enemy(a, c);
}

#endif

}  // namespace alamo::sim
