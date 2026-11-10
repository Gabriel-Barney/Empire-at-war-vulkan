// s78n t06 - the battle side of the land hero clashes (docs/re/HERO_CLASH.md §2-§6): the HeroClashHost of
// tactical::combat::HeroClashManager over the battle's object store, MovementSystem and CombatSystem. Serial code only;
// the synced draws are the manager's (one float per fought exchange) and the Move_To of the clash start (MovementSystem).
#include <algorithm>
#include <cmath>

#include "alamo/core/dmath.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/sim/tactical_battle.hpp"
#ifdef ALAMO_SIM_HAVE_ANIM
#include "alamo/anim/anim_types.hpp"
#include "alamo/sim/sim_anim.hpp"
#endif

namespace alamo::sim {

namespace tc = alamo::tactical::combat;
namespace mv = alamo::tactical::movement;

namespace {

Vec3 clash_facing(float yaw_deg) {
    float s = 0.0f, c = 1.0f;
    dmath::sincos(yaw_deg * (dmath::kPiF / 180.0f), s, c);
    return {c, s, 0.0f};
}

}  // namespace

class BattleClashHost final : public tc::HeroClashHost {
public:
    BattleClashHost(TacticalBattle& b, TacticalMode& mode, SimRng& rng) : b_(b), mode_(mode), st_(mode.primary()), rng_(rng) {}

    void candidates(int32_t type, std::vector<tc::ObjectId>& out) override {
        // Battle units only: team containers are not battle units ([port], HeroClashHost::candidates).
        // s78n t06 review [port]: a garrisoned unit (obj +0x3a7, hidden in its host) is no candidate. 0x56c540 has no
        // +0x3a7 test (HERO_CLASH §2.2), but a garrisoned hero takes no damage (Take_Damage returns at +0x3a7, §8.4.1
        // step 7), so such a clash could never end by an exchange, and its snap would move a unit inside its host.
        for (const TacticalBattle::Unit& u : b_.units_)
            if (u.alive && !u.retreated && st_.exists(u.h) && static_cast<int32_t>(st_.type(u.h)) == type && !garrisoned(u.id))
                out.push_back(u.id);
    }
    bool alive(tc::ObjectId id) override {
        // Not dying / a death clone / removed: a live, non-retreated battle unit whose combat unit is not dead (the combat
        // kill flag is set at once; the battle's record follows at the death flush).
        const TacticalBattle::Unit* u = b_.unit_by_id(id);
        if (!u || !u->alive || u->retreated || !st_.alive(u->h)) return false;
        return !b_.combat_->has_unit(id) || b_.combat_->alive(id);
    }
    int32_t type_of(tc::ObjectId id) override {
        const objects::ObjectHandle h = st_.find(id);
        return st_.exists(h) ? static_cast<int32_t>(st_.type(h)) : -1;
    }
    int32_t owner(tc::ObjectId id) override {
        const objects::ObjectHandle h = st_.find(id);
        return st_.exists(h) ? st_.owner(h) : -1;
    }
    bool hero_type(int32_t type) override {
        if (type < 0) return false;
        const objects::GameObjectTypes& t = b_.catalog_->types();
        const auto ti = static_cast<objects::TypeIndex>(type);
        return t.get_bool(ti, "Is_Named_Hero") || t.get_bool(ti, "Is_Generic_Hero");
    }
    bool enemies(int32_t a, int32_t c) override { return b_.combat_->is_enemy(a, c); }
    tactical::Vec3 position(tc::ObjectId id) override {
        const objects::ObjectHandle h = st_.find(id);
        if (!st_.exists(h)) return {};
        const Vec3 p = st_.position(h);
        return {p.x, p.y, p.z};
    }
    float yaw_deg(tc::ObjectId id) override {
        if (b_.movement_->has_unit(id)) return b_.movement_->view(id).yaw;
        const objects::ObjectHandle h = st_.find(id);
        if (!st_.exists(h)) return 0.0f;
        const Vec3 f = st_.facing(h);
        return dmath::atan2(f.y, f.x) * (180.0f / dmath::kPiF);
    }
    float health(tc::ObjectId id) override { return b_.combat_->has_unit(id) ? b_.combat_->health(id) : 0.0f; }
    bool jetpacking(tc::ObjectId id) override {
        if (!b_.movement_->has_unit(id)) return false;
        const mv::UnitView v = b_.movement_->view(id);
        return v.secondary_locomotor && v.locomotor == mv::LocomotorKind::Jetpack;
    }
    tc::ObjectId attack_target(tc::ObjectId id) override {
        // 0x397a40(obj, 0): the formation attack target (the port's Attack_Move object goal), else the targeting data's
        // target (a ground team member: its container's, CombatSystem::current_target).
        if (!b_.combat_->has_unit(id)) return tc::kNoObject;
        const tc::ObjectId g = b_.combat_->attack_goal(id);
        return g != tc::kNoObject ? g : b_.combat_->current_target(id);
    }
    bool walking(tc::ObjectId id) override {
        // Locomotor vf +0x1c0(0): the walk has a path on its stack. s78n t06 [port]: an order that is moving (not waiting
        // for its composition at the next coordinator update, MOVEMENT §4.7.4, nor for a later start frame) or the arrival
        // turn (its path is still on the stack, s78l t02). So the start frame's Move_To is not walking yet and the heroes
        // snap (slide) to their spots on the start frame (HERO_CLASH §3.3 [M]).
        if (!b_.movement_->has_unit(id)) return false;
        const mv::UnitView v = b_.movement_->view(id);
        if (v.arrival_turn) return true;
        return v.has_goal && v.start_frame != mv::kStartPending && v.start_frame <= b_.frame_;
    }
    bool in_clash(tc::ObjectId id) override {
        return b_.combat_->has_unit(id) && (b_.combat_->flags(id) & tc::kUnitHeroClash) != 0;
    }
    void set_in_clash(tc::ObjectId id, bool on) override {
        // +0x3b4 = the combat flag: TARGETING suspended (CombatSystem::targeting_idle / phase D), the movement orders locked
        // (TacticalBattle::movement_locked), the pairing rule (CombatSystem::clash_allows). A new flag or a release also
        // ends a Lua TEAM Unlock's release of the clash's lock share (kUnitHeroClashUnlocked, s78n t06 review).
        if (b_.combat_->has_unit(id)) {
            b_.combat_->set_flag(id, tc::kUnitHeroClash, on);
            b_.combat_->set_flag(id, tc::kUnitHeroClashUnlocked, false);
        }
        if (on) {
            // s78n t06 review: Begin 0x6abde0 leaves the hero's coordinators, which ends a garrison order in progress.
            // [port] Both its approach and its entry fade end (as the exit order cancels them, GarrisonSystem::order_exit),
            // so a duelling hero is never garrisoned; order_garrison also refuses a clashing unit.
            if (const tactical::garrison::ObjectId ph = b_.garrison_.pending_holder(id);
                ph != tactical::garrison::kNoObject && !b_.garrison_.garrisoned(id)) {
                const objects::ObjectHandle hh = st_.find(ph);
                const Vec3 hp = st_.exists(hh) ? st_.position(hh) : Vec3{};
                const Vec3 hf = st_.exists(hh) ? st_.facing(hh) : Vec3{1.0f, 0.0f, 0.0f};
                if (b_.garrison_.order_exit(id, hp, hf, b_.frame_)) b_.s4_apply(mode_);
            }
            // s78n t06 review [port]: a land-squad steering member (mod data) leaves its squad's steering for the clash, so
            // the clash's move / snap / hold act on the hero itself (not on its container, the squad's formation unit);
            // the release steers it back to its slot (MovementSystem::suspend_team_member / rejoin_land_team).
            if (b_.movement_->has_unit(id)) (void)b_.movement_->suspend_team_member(id);
        } else if (b_.movement_->team_member_parked(id) && b_.movement_->has_unit(id) && alive(id)) {
            (void)b_.movement_->rejoin_land_team(id);
        }
    }
    void move_to(tc::ObjectId id, tactical::Vec3 spot) override {
        // Begin 0x6abde0: leave the coordinators, a new coordinator with a Move_To to the spot (no draw here; its
        // formation is composed, with its ctor draw, at the next coordinator update).
        if (!b_.movement_->has_unit(id)) return;
        b_.movement_->stop_and_leave(id);
        mv::MoveOrder o;
        o.units = {id};
        o.destination = {spot.x, spot.y};
        b_.movement_->move(o, rng_, b_.frame_);
    }
    void place(tc::ObjectId id, tactical::Vec3 pos, float yaw) override {
        // 0x3a8f90 / 0x3a8710: no pathing, no collision; the battle's teleport (store, movement, combat, trees).
        b_.teleport(mode_, id, Vec3{pos.x, pos.y, pos.z}, clash_facing(yaw));
    }
    void face(tc::ObjectId id, float yaw) override {
        if (b_.movement_->has_unit(id)) b_.movement_->snap_yaw(id, yaw);
        const objects::ObjectHandle h = st_.find(id);
        const Vec3 f = clash_facing(yaw);
        if (st_.alive(h)) st_.set_facing(h, f);
        if (b_.combat_->has_unit(id)) b_.combat_->set_facing(id, tactical::Vec3{f.x, f.y, f.z});
    }
    void hold(tc::ObjectId id, bool on) override {
        // Fight: the locomotor (slot 1) suspended - the hero stops where it stands. s78n t06 [port]: IDLE (slot 7) is only
        // the idle animation (not modelled for the hold); the resume (release) has nothing to undo (the orders stay
        // locked until the flag clears).
        if (on && b_.movement_->has_unit(id)) {
            const tc::ObjectId ids[1] = {id};
            b_.movement_->stop(ids);
        }
    }
    bool clip(tc::ObjectId id, std::string_view anim, uint32_t variant, uint32_t& variants, uint32_t& frames, float& fps) override {
        variants = 0;
        frames = 0;
        fps = 0.0f;
#ifdef ALAMO_SIM_HAVE_ANIM
        if (!b_.anim_) return false;
        const objects::ObjectHandle h = st_.find(id);
        if (!st_.exists(h)) return false;
        const anim::AnimModel* m = b_.anim_->model_for(st_.type(h), mode_.kind());
        if (!m) return false;
        // The model is known: a missing clip is the retail case "nothing played" (variants 0), not the fallback.
        const int32_t t = anim::find_anim_type(anim);
        if (t < 0) return true;
        variants = m->variant_count(t);
        const anim::ClipRef* c = m->clip(t, variant);
        if (c && c->clip && c->clip->frame_count > 0) {
            frames = c->clip->frame_count;
            fps = c->clip->fps;
        }
        return true;
#else
        (void)id, (void)anim, (void)variant;
        return false;
#endif
    }
    bool speech_idle() override { return true; }  // [port] no speech manager in the sim
    void damage(tc::ObjectId victim, float amount, tc::ObjectId source) override {
        // Take_Damage kind 1, the winner as a non-projectile source, no hardpoint: queued for combat phase D of this tick
        // (BattleHooks::before_units, like crush and attrition) so a death is reported by this tick's combat.
        tc::DamageRequest r;
        r.victim = victim;
        r.amount = amount;
        r.kind = tc::kKindNormal;
        r.source = source;
        r.source_is_projectile = false;
        r.no_diminishing = true;  // HERO_CLASH §3.7: no diminishing firepower for this source
        b_.hero_clash_hits_.push_back(r);
    }

private:
    bool garrisoned(tc::ObjectId id) const {
        return b_.garrison_.garrisoned(id) || (b_.combat_->has_unit(id) && (b_.combat_->flags(id) & tc::kUnitGarrisoned));
    }

    TacticalBattle& b_;
    TacticalMode& mode_;
    objects::ObjectStore& st_;
    SimRng& rng_;
};

void TacticalBattle::set_hero_clash_data(std::shared_ptr<const tc::HeroClashData> d) {
    hero_clash_data_ = std::move(d);
    std::vector<std::pair<int32_t, int32_t>> types;
    if (hero_clash_data_) {
        const objects::GameObjectTypes& t = catalog_->types();
        auto resolve = [&](const std::string& name) {
            if (name.empty()) return -1;
            const objects::TypeIndex ti = t.resolve_name(name);
            return ti == objects::kNone ? -1 : static_cast<int32_t>(ti);
        };
        for (const tc::HeroClashEntry& e : hero_clash_data_->entries) types.emplace_back(resolve(e.first_type), resolve(e.second_type));
    }
    hero_clash_.set_data(hero_clash_data_.get(), std::move(types));
}

void TacticalBattle::hero_clash_tick(TacticalMode& mode, SimRng& rng, uint64_t global_frame) {
    // HERO_CLASH §2.1: land only (the mode's manager is null in space); 0x56ce20(manager, global frame).
    if (space_ || !hero_clash_data_ || !combat_ || !movement_) return;
    BattleClashHost host(*this, mode, rng);
    hero_clash_.update(static_cast<uint32_t>(global_frame), host, rng);
    (void)hero_clash_.take_ended();  // [port] the end events are diagnostics (the win speech is audio only)
    if (hero_clash_.take_changed()) hero_clash_sync();
}

void TacticalBattle::hero_clash_sync() {
    if (combat_) combat_->set_hero_clashes(hero_clash_.pairs());
}

void TacticalBattle::hero_clash_apply_hits(uint32_t frame) {
    for (const tc::DamageRequest& r : hero_clash_hits_)
        if (combat_->alive(r.victim)) combat_->take_damage(r, frame);
    hero_clash_hits_.clear();
}

}  // namespace alamo::sim
