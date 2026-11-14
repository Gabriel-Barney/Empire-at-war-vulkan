// tactical_battle: part 3 (bo3 t03 split; shared code in tactical_battle_internal.hpp).
#include "tactical_battle_internal.hpp"

namespace alamo::sim {
// S8 t17 (FIGHTER_FLYING §4.10 A, TACTICAL_COMBAT §2.2): Get_Scan_Range 0x566f50 / the per-candidate acceptance of the
// combat scan for a fighter (a squadron member: its squadron's formation). The inputs are fighter_push's for the auto
// target c; c = none: no goal / attacker match and c at the slot goal (rule 7: "c = null -> 1"). Rr = the first block's
// range (+ Autonomous_Move_Extension_Vs_Attacker for an attacker). Read-only: combat calls it from its parallel gather
// phase (nothing else runs then); the dead pushed blocks fighter_push erases are skipped instead.
uint8_t TacticalBattle::fighter_scan_range(const objects::ObjectStore& store, objects::ObjectId member, objects::ObjectId cand,
                                           float range, float& rr) const {
    rr = 0.0f;
    const auto unit_c = [this](objects::ObjectId id) -> const Unit* {
        const auto it = std::lower_bound(units_.begin(), units_.end(), id, [](const Unit& x, objects::ObjectId v) { return x.id < v; });
        return it == units_.end() || it->id != id ? nullptr : &*it;
    };
    const auto squad_c = [this](objects::ObjectId id) -> const mv::FighterSquad* {
        const std::span<const mv::FighterSquad> s = movement_->fighter_squads();
        const auto it = std::lower_bound(s.begin(), s.end(), id, [](const mv::FighterSquad& q, objects::ObjectId v) { return q.id < v; });
        return it == s.end() || it->id != id ? nullptr : &*it;
    };
    const auto key_of = [&](objects::ObjectId id) {  // fighter_goal_key
        const Unit* x = id != objects::kNoObjectId ? unit_c(id) : nullptr;
        if (x && x->container < containers_.size() && squad_c(containers_[x->container].id)) return containers_[x->container].id;
        return id;
    };
    const auto goal_live = [&](objects::ObjectId k) {  // fighter_goal_live
        if (k == objects::kNoObjectId) return false;
        if (const mv::FighterSquad* s = squad_c(k)) return !s->members.empty();
        const Unit* x = unit_c(k);
        return x && x->alive && !x->retreated && store.alive(x->h) && combat_->has_unit(x->id);
    };
    const Unit* u = unit_c(member);
    if (!u) return 0;
    const FighterFormationRecord* r = fighter_formation(fighter_key(*u));
    if (!r) return 0;
    const mv::FighterSquad* sq = squad_c(r->key);
    const objects::ObjectId lid = sq && !sq->members.empty() ? sq->members.front() : u->id;
    const Unit* leader = unit_c(lid);
    if (!leader || !movement_->has_unit(lid)) return 0;
    std::span<const objects::ObjectId> members(&leader->id, 1);
    if (sq) members = sq->members;
    objects::ObjectId last_push = objects::kNoObjectId;  // s78f t02: the movement formation's top, when a live auto block
    if (const mv::FighterFormation* ff = movement_->fighter_formation(r->key); ff && !ff->blocks.empty() && ff->blocks.back().pushed &&
                                                                               goal_live(ff->blocks.back().object))
        last_push = ff->blocks.back().object;
    const objects::ObjectId order_goal = key_of(combat_->attack_goal(lid));
    const mv::UnitView lv = movement_->view(lid);
    const FighterOrderView ov = fighter_order_view(*r);  // s78f review: the movement stack's order block
    FighterChaseInputs ci;
    ci.formation = true;
    ci.flags = ov.first_flags;
    ci.moving = lv.has_goal && !lv.arrived;
    ci.last_guard = (ov.flags & mv::kMoveFlagGuardMove) != 0 && last_push == objects::kNoObjectId;
    ci.single_position = !ci.moving && last_push == objects::kNoObjectId && ov.goal_object == objects::kNoObjectId && !goal_live(order_goal);
    const Unit* guarded = (ov.flags & mv::kMoveFlagGuardMove) != 0 && ov.goal_object != objects::kNoObjectId ? unit_c(ov.goal_object) : nullptr;
    if (guarded && (!guarded->alive || guarded->retreated || !store.alive(guarded->h))) guarded = nullptr;
    if (guarded && movement_->has_unit(guarded->id)) {
        const mv::UnitView gv = movement_->view(guarded->id);
        ci.guarded_moving = gv.has_goal && gv.speed > 0.0f;
    }
    ci.unit = store.position(leader->h);
    ci.goal = ov.goal;
    if (const Unit* go = ov.goal_object != objects::kNoObjectId ? unit_c(ov.goal_object) : nullptr;
        go && go->alive && !go->retreated && store.alive(go->h))
        ci.goal = store.position(go->h);
    ci.attack_range = range;
    uint32_t ty = leader->type;
    if (sq && leader->container < containers_.size() && containers_[leader->container].squad_type < catalog_->types().size())
        ty = containers_[leader->container].squad_type;
    if (ty < fchase_have_.size() && fchase_have_[ty]) {
        ci.guard_range = fchase_cache_[ty][0];
        ci.idle_range = fchase_cache_[ty][1];
        ci.attack_move_range = fchase_cache_[ty][2];
        ci.extension = fchase_cache_[ty][3];
    } else if (ty < catalog_->types().size()) {
        const objects::GameObjectTypes& types = catalog_->types();
        const auto ti = static_cast<objects::TypeIndex>(ty);
        ci.guard_range = types.get_float(ti, "Guard_Chase_Range");
        ci.idle_range = types.get_float(ti, "Idle_Chase_Range");
        ci.attack_move_range = types.get_float(ti, "Attack_Move_Response_Range");
        ci.extension = types.get_float(ti, "Autonomous_Move_Extension_Vs_Attacker");
    }
    if (cand == objects::kNoObjectId) {
        ci.candidate = ci.goal;
    } else {
        const Unit* c = unit_c(cand);
        if (!c || !store.alive(c->h)) return 0;
        const objects::ObjectId tk = key_of(cand);
        ci.candidate_is_goal = tk == order_goal || tk == key_of(ov.goal_object) || tk == last_push;
        std::span<const objects::ObjectId> attackers(&c->id, 1);
        if (const mv::FighterSquad* cs = squad_c(tk)) attackers = cs->members;
        for (const objects::ObjectId a : attackers) {
            const objects::ObjectId at = combat_->has_unit(a) ? combat_->current_target(a) : objects::kNoObjectId;
            if (at == objects::kNoObjectId) continue;
            if ((guarded && at == guarded->id) || std::find(members.begin(), members.end(), at) != members.end()) ci.candidate_attacker = true;
        }
        ci.candidate = store.position(c->h);
    }
    const uint8_t res = fighter_chase_test(ci);
    if (res != 0) {
        rr = (ci.flags & mv::kMoveFlagGuardMove) != 0 ? ci.guard_range : (ci.flags & mv::kMoveFlagAttackMove) != 0 ? ci.attack_move_range : ci.idle_range;
        if (rr < 0.0f) rr = 0.0f;
        if (ci.candidate_attacker) rr += ci.extension;
    }
    return res;
}

void TacticalBattle::feed_fighter_target(const Unit& u, const objects::ObjectStore& store) {
    mv::ObjectId addr = u.id;
    if (u.container < containers_.size()) {
        const objects::ObjectId cid = containers_[u.container].id;
        if (const mv::FighterSquad* s = movement_->fighter_squad(cid)) {
            if (s->members.empty() || s->members.front() != u.id) return;
            addr = cid;
        }
    }
    const auto live = [&](objects::ObjectId id) -> const Unit* {
        const Unit* x = id != objects::kNoObjectId ? unit_by_id(id) : nullptr;
        return x && x->alive && !x->retreated && store.alive(x->h) && combat_->has_unit(x->id) ? x : nullptr;
    };
    const int32_t me = store.owner(u.h);
    FighterTargetInputs in;
    if (live(combat_->ordered_target(u.id))) in.ordered = combat_->ordered_target(u.id);
    in.pushed = fighter_push(u, addr == u.id ? fighter_key(u) : addr, store);
    if (const Unit* g = live(combat_->attack_goal(u.id))) {
        in.goal = g->id;
        in.goal_hostile = combat_->is_enemy(me, store.owner(g->h));
    }
    mv::FighterTarget ft;
    const objects::ObjectId chosen = fighter_formation_target(in);
    if (chosen != objects::kNoObjectId && movement_->fighter_squad(chosen)) {  // a pushed squadron (a team: dogfight)
        ft.id = chosen;
        ft.hidden = fog_object_hidden(me, ft.id, true);
    } else if (const Unit* t = live(chosen)) {
        const mv::FighterSquad* ts = t->container < containers_.size() ? movement_->fighter_squad(containers_[t->container].id) : nullptr;
        if (ts) {
            ft.id = ts->id;
        } else {
            ft.id = t->id;
            const tactical::Vec3 p = combat_->position(t->id);
            ft.pos = {p.x, p.y, p.z};
            const tc::CombatData& cd = combat_->data();
            const uint32_t di = combat_->desc_index(t->id);
            if (di < cd.units.size()) {
                const tc::UnitDesc& ud = cd.units[di];
                ft.radius = ud.radius;
                const int32_t hp = combat_->ordered_target(u.id) == t->id ? combat_->ordered_hardpoint(u.id) : -1;
                if (hp >= 0 && static_cast<size_t>(hp) < ud.hardpoints.size() && ud.hardpoints[static_cast<size_t>(hp)] < cd.hardpoints.size() &&
                    combat_->hardpoint_targetable(t->id, static_cast<uint32_t>(hp))) {
                    const tactical::Vec3 l = cd.hardpoints[ud.hardpoints[static_cast<size_t>(hp)]].attach_offset;
                    const tactical::Vec3 f = combat_->facing(t->id);
                    float fx = f.x, fy = f.y;
                    const float len2 = fx * fx + fy * fy;
                    if (len2 > 0.0f) {
                        const float inv = 1.0f / dmath::sqrt(len2);
                        fx = fx * inv;
                        fy = fy * inv;
                    } else {
                        fx = 1.0f;
                        fy = 0.0f;
                    }
                    ft.hard_point = hp;
                    ft.pos = {p.x + (l.x * fx - l.y * fy), p.y + (l.x * fy + l.y * fx), p.z + l.z};
                }
            }
        }
        ft.hidden = fog_object_hidden(me, ft.id, true);
    }
    movement_->set_fighter_target(addr, ft);
}

// enter_idle (§4.7 step 4, 0x5c9ca0 -> 0x406370(team, null)): the squadron's team target is dropped - every member's
// TARGETING Set_Target(null), the hard points' targets too - so the targeting service rescans on its next due frame; the
// formation and its blocks are kept (§4.7, §4.9 step 3). S7 t25 (§4.10 D): enter_idle comes from the movement tick's
// EnterIdle events, raised only at its four retail call sites (Directed_Combat NoTarget, Moving path complete); the t22
// state-transition detection (which also caught the Dead_Stop / hyperspace ends) is gone. A member's ordered target that
// stands for its order's object block (the Attack / Attack_Move object) is kept [port: the block is the ordered slot];
// Provoke / Retarget orders (team targets only in retail) are dropped.
void TacticalBattle::fighter_enter_idle() {
    if (!space_ || !movement_) return;
    std::vector<objects::ObjectId> keys;  // first-seen order (event order)
    for (const mv::FighterEvent& e : movement_->fighter_events()) {
        if (e.kind != mv::FighterEvent::EnterIdle) continue;
        const Unit* u = unit_by_id(e.who);
        if (!u) continue;
        const objects::ObjectId key = fighter_key(*u);
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
    }
    for (const objects::ObjectId key : keys) {
        std::span<const objects::ObjectId> members(&key, 1);
        if (const mv::FighterSquad* s = movement_->fighter_squad(key)) members = s->members;
        const FighterFormationRecord* r = fighter_formation(key);
        for (const objects::ObjectId m : members) {
            if (!combat_->has_unit(m) || combat_->current_target(m) == objects::kNoObjectId) continue;
            const objects::ObjectId o = combat_->ordered_target(m);
            if (o != objects::kNoObjectId && (o == combat_->attack_goal(m) || (r && o == r->goal_object))) continue;
            combat_->clear_targets(m);
        }
    }
}

objects::ObjectId TacticalBattle::fighter_combat_unit(objects::ObjectId id) const {
    for (const mv::FighterSquad& s : movement_->fighter_squads())
        if (s.id == id) return s.members.empty() ? objects::kNoObjectId : s.members.front();
    return id;
}

// The locomotor's orders (0x38d730) as combat attack orders, in event order: Provoke (§4.5 step 2.2) - squadron `who`
// attacks `target`, unless `who` has unit ability 0x29 (ION_CANNON_SHOT) or 0x22 (LUCKY_SHOT) active (0x39b950; the
// spec names them by number only); Retarget (§4.3) - container `who` attacks a dogfight-cell entry. Every live member
// of `who` (or `who` itself when it is a unit) is ordered at the target object (a squadron: its leader).
void TacticalBattle::forward_fighter_events() {
    for (const mv::FighterEvent& e : movement_->fighter_events()) {
        if (e.kind == mv::FighterEvent::EnterIdle) continue;  // fighter_enter_idle
        if (e.kind == mv::FighterEvent::Provoke && abilities_ && (abilities_->is_active(e.who, 0x29) || abilities_->is_active(e.who, 0x22)))
            continue;
        const objects::ObjectId tgt = fighter_combat_unit(e.target);
        if (tgt == objects::kNoObjectId || !combat_->has_unit(tgt) || !combat_->alive(tgt)) continue;
        auto order = [&](objects::ObjectId m) {
            if (m == tgt || !combat_->has_unit(m) || !combat_->alive(m)) return;
            if (!combat_->is_enemy(combat_->owner(m), combat_->owner(tgt))) return;
            combat_->order_attack(m, tgt);
        };
        bool squad = false;
        for (const mv::FighterSquad& s : movement_->fighter_squads())
            if (s.id == e.who) {
                squad = true;
                for (const mv::ObjectId m : s.members) order(m);
            }
        if (!squad) order(e.who);
    }
}

const TacticalBattle::PresenceTypeInfo& TacticalBattle::presence_info(uint32_t type) {
    // s78l t03: Presence_Induced_Animations entries ("<anim type>, <type>, <type>, ..."). [port] repeated tags reach the
    // port as one flat token list, so a token naming a ModelAnimType starts the next entry.
    if (presence_cache_.size() < catalog_->types().size()) presence_cache_.resize(catalog_->types().size());
    static const PresenceTypeInfo kNone{};
    if (type >= presence_cache_.size()) return kNone;
    PresenceTypeInfo& info = presence_cache_[type];
    if (info.parsed) return info;
    info.parsed = true;
#ifdef ALAMO_SIM_HAVE_ANIM
    const objects::GameObjectTypes& ty = catalog_->types();
    const auto t = static_cast<objects::TypeIndex>(type);
    info.range = ty.get_float(t, "Presence_Range");
    for (std::string_view tok : ty.get_list(t, "Presence_Induced_Animations")) {
        while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.front()))) tok.remove_prefix(1);
        while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.back()))) tok.remove_suffix(1);
        if (tok.empty()) continue;
        const int32_t a = anim::find_anim_type(tok);
        if (a >= 0) {
            info.anims.push_back({a, {}});
        } else if (!info.anims.empty()) {
            const objects::TypeIndex r = ty.resolve_name(tok);
            if (r != objects::kNone) info.anims.back().second.push_back(static_cast<uint32_t>(r));
        }
    }
#endif
    return info;
}

void TacticalBattle::imposing_presence(objects::ObjectStore& store, const std::vector<uint8_t>& gated) {
#ifdef ALAMO_SIM_HAVE_ANIM
    // s78l t03 (TACTICAL_COMBAT slot 47 "s78l t03" (c), TACTICAL_GAPS 6.2): IMPOSING_PRESENCE 0x624ab0 on its interval-30
    // schedule (the store's: first due frame drawn at the attach, TACTICAL_GAPS 6.4). Each due presence object P (alive,
    // the behaviour enabled; death clones keep it) refreshes, for every battle unit R with IDLE inside the box of half size
    // P's Presence_Range around P, not dying, whose type lists P's type in Presence_Induced_Animations, R's record of P to
    // int(FPS x Unit_Presence_Relaxation_Time). Skipped for an R with a target (TARGETING, a team member: its container's;
    // suspended or not) or that the moving gate stops. No draws; the order of the writes does not change the result.
    const GameMode* gm = hooks_ ? hooks_->anim_mode() : nullptr;
    if (!anim_ || !gm || &gm->primary() != &store) return;
    const float fr = static_cast<float>(anim::kSimTicksPerSecond) * cfg_.presence_relaxation_seconds;
    const int32_t frames = fr >= 1.0f ? (fr < 1.0e9f ? static_cast<int32_t>(fr) : 1000000000) : 0;
    if (frames < 1) return;
    anim::AnimBehaviors& beh = anim_->behaviors(*gm);
    const uint32_t mf = store.mode_frame();
    const objects::BehaviorTypeId kImposingPresence = 0x40;
    for (const objects::ObjectHandle p : store.all_objects()) {
        if (!store.alive(p)) continue;
        const uint32_t bi = store.behavior_index(p, kImposingPresence);
        if (bi == objects::kNone || !store.behavior_enabled(p, bi) || store.behavior_next_service(p, bi) > mf) continue;
        store.set_behavior_next_service(p, bi, mf + std::max<uint32_t>(1u, store.behavior_interval(p, bi)));
        const uint32_t pt = store.type(p);
        const float range = presence_info(pt).range;
        const Vec3 pp = store.position(p);
        const objects::ObjectId pid = store.id(p);
        for (size_t i = 0; i < units_.size(); ++i) {
            const Unit& u = units_[i];
            if (!u.alive || u.retreated || !store.active(u.h) || !store.has_behavior(u.h, objects::beh::Idle)) continue;
            if (i < gated.size() && gated[i]) continue;
            if (combat_->has_unit(u.id) && combat_->current_target(u.id) != tc::kNoObject) continue;
            const Vec3 q = store.position(u.h);
            if (std::fabs(q.x - pp.x) > range || std::fabs(q.y - pp.y) > range || std::fabs(q.z - pp.z) > range) continue;
            bool listed = false;
            for (const auto& e : presence_info(store.type(u.h)).anims)
                listed = listed || std::find(e.second.begin(), e.second.end(), pt) != e.second.end();
            if (listed) beh.presence_refresh(u.id, pid, frames);
        }
    }
#else
    (void)store, (void)gated;
#endif
}

int32_t TacticalBattle::presence_pick(const objects::ObjectStore& st, int32_t unit, const anim::IdleRecord* records, size_t count) {
#ifdef ALAMO_SIM_HAVE_ANIM
    // The default idle type's presence step (0x395920 step 2): among the records whose object still exists the largest
    // value wins; on <= an allied record (0x2823e0) replaces a non-allied best (ascending presence id); the idle = the
    // first Presence_Induced_Animations entry of the unit's type listing the winner's type, else none (the plain idle).
    const objects::ObjectHandle uh = st.find(unit);
    if (!st.exists(uh)) return anim::kNoAnimType;
    const int32_t uo = st.owner(uh);
    bool have = false, best_allied = false;
    int32_t best_frames = 0;
    objects::TypeIndex best_type = objects::kNone;
    for (const anim::IdleRecord& r : std::span<const anim::IdleRecord>(records, count)) {
        const objects::ObjectHandle ph = st.find(r.presence);
        if (!st.exists(ph)) continue;
        const int32_t po = st.owner(ph);
        // 0x2823e0: relation == 0 (self or Ally); a neutral (not an enemy) presence owner is not allied.
        const bool allied = po == uo || (galaxy_ && galaxy_->allied(po, uo));
        if (!have || r.frames > best_frames || (allied && !best_allied)) {
            have = true;
            best_frames = r.frames;
            best_allied = allied;
            best_type = st.type(ph);
        }
    }
    if (!have) return anim::kNoAnimType;
    for (const auto& e : presence_info(st.type(uh)).anims)
        if (std::find(e.second.begin(), e.second.end(), static_cast<uint32_t>(best_type)) != e.second.end()) return e.first;
    return anim::kNoAnimType;
#else
    (void)st, (void)unit, (void)records, (void)count;
    return -1;
#endif
}

void TacticalBattle::play_turn_clip_events(SimRng& rng, uint64_t gframe) {
    std::vector<mv::MovementSystem::TurnClipEvent> ev = movement_->take_turn_clip_events();
#ifdef ALAMO_SIM_HAVE_ANIM
    // s78l t02 (MOVEMENT §3.5.1 "Arrival turn, details"), in the movement's order (ascending id): a clip event plays
    // `anim` with index 0xFFFF (synced variant draw only when the clip has >= 2 variants), loop 0, at ev.speed, blend
    // ev.blend when >= 0 (the locomotor caps it with its [port] constant for GC 0x140b16d28), else the clip's global ([port] the move blend tunable: the
    // globals 0x140b398fc / 0x140b39900 are unread); an idle event plays the default idle type (slot 47: index 0,
    // Loop_Idle_Anim_00, [port] the idle blend for 0x140b39908, speed 1) unless the unit burns.
    anim::AnimWorld* w = hooks_ ? hooks_->world() : nullptr;
    const GameMode* gm = hooks_ ? hooks_->anim_mode() : nullptr;
    if (!anim_ || !w || !gm) return;

    const objects::ObjectStore& st = gm->primary();
    const anim::PresenceIdleFn presence = [this, &st](anim::ObjectId unit, std::span<const anim::IdleRecord> recs) {
        return presence_pick(st, unit, recs.data(), recs.size());
    };
    for (const mv::MovementSystem::TurnClipEvent& e : ev) {
        if (!w->contains(e.id)) continue;
        if (e.idle) {
            if (combat_->has_unit(e.id) && combat_->burning(e.id)) continue;
            if (anim_->play_default_idle(*gm, e.id, gframe, &presence)) ++stats_.anim_plays;
            continue;
        }
        if (e.anim < 0) continue;
        anim::PlayRequest r;
        r.type = e.anim;
        r.index = anim::kRandomVariant;
        r.loop = false;
        r.speed = e.speed;
        r.blend_seconds = e.blend >= 0.0f ? e.blend : w->tunables().move_blend_seconds;
        const anim::AnimModel* m = w->model(e.id);
        if (m && m->variant_count(e.anim) < 2) r.index = 0;  // one variant: no draw (0x1ffb40 returns at once)
        if (w->play(e.id, r, &rng, gframe) == anim::PlayResult::Played) ++stats_.anim_plays;
    }
#else
    (void)rng, (void)gframe;
#endif
}

bool TacticalBattle::anim_engaged_clip(objects::ObjectId id) const {
    // cu8g t01 (CU8F_T01_T02_T07_RE.md §2.3): HOLSTER_WEAPON's "engaged" clip test - the current animation type is ATTACK
    // (5) or SPINMOVE (0x68). Without the animation service: false.
#ifdef ALAMO_SIM_HAVE_ANIM
    const anim::AnimWorld* w = hooks_ ? hooks_->world() : nullptr;
    if (!w || !w->contains(id)) return false;
    const int32_t t = w->current_type(id);
    return t == static_cast<int32_t>(anim::kAnimAttack) || t == static_cast<int32_t>(anim::kAnimSpinMove);
#else
    (void)id;
    return false;
#endif
}

void TacticalBattle::drive_animation(objects::ObjectStore& store, SimRng& rng, uint64_t gframe) {
#ifdef ALAMO_SIM_HAVE_ANIM
    // R3 anim: WalkLocomotor (0x61f840, A.7 walk / move / turn rows) and the idle behaviour (0x62e570) for battle
    // units, through SimAnimation::service_locomotor (rules in anim_behaviors.hpp); Berserker state first.
    anim::AnimWorld* w = hooks_ ? hooks_->world() : nullptr;
    const GameMode* gm = hooks_ ? hooks_->anim_mode() : nullptr;
    if (!anim_ || !w || !gm) return;
    const objects::ObjectStore& st = gm->primary();
    const bool land = gm->kind() == objects::ModeKind::Land;
    // s78l t03 (a) (TACTICAL_COMBAT slot 47 "s78l t03"): the IDLE moving gate = the locomotor's vf +0x110. Walk / Bike /
    // Fighter / SimpleSpace: a path on the stack, whatever the speed ([port] has_goal, a pushed walk, or the Walk arrival
    // turn - UnitView::arrival_turn - which retail runs with the path still stacked: MOVEMENT 3.5.1); JetPack: state != Walk Stopped; Flying / LandBomber: [port] the goal (formation tests not
    // modelled); a land team member on the Default locomotor (LandTeamInfantry): its container's, [port] the container
    // follows its first live member, so that member's. No locomotor (not movable): never gated.
    const auto own_moving = [&](const Unit&, const mv::UnitView& v) {
        if (v.locomotor == mv::LocomotorKind::Jetpack) return v.loco_state != mv::LocoState::WalkStopped;
        return v.has_goal || v.pushed || v.arrival_turn;  // s78l t02: the arrival turn keeps the path stacked
    };
    const auto team_of = [&](const Unit& u) -> uint32_t {
        return land && u.container < containers_.size() && containers_[u.container].team && !containers_[u.container].removed
                   ? u.container
                   : 0xFFFFFFFFu;
    };
    // s78n t26.1 review (MOVEMENT §3.2.1 item 3): a walk that ended deployed this tick (0x16 -> 0x19) sets the Deploy clip on
    // its last frame (blend 0.2, rate 1.0; [port] variant 0, no draw), whatever clip plays (Deployed_Walk, a turn, an idle)
    for (const objects::ObjectId id : movement_->deploy_pose_events())
        if (const Unit* du = unit_by_id(id); du && du->alive && !du->retreated && w->contains(id) && anim_->deploy_at_end(*gm, id, gframe))
            ++stats_.anim_plays;
    std::vector<int8_t> head_moving(containers_.size(), -1);  // per team container: its first live member's gate
    idle_gate_.assign(units_.size(), 0);
    for (size_t i = 0; i < units_.size(); ++i) {
        const Unit& u = units_[i];
        if (!u.alive || u.retreated || !u.movable || !movement_->has_unit(u.id)) continue;
        const mv::UnitView v = movement_->view(u.id);
        const bool own = own_moving(u, v);
        const uint32_t c = team_of(u);
        if (c != 0xFFFFFFFFu && head_moving[c] < 0) head_moving[c] = own ? 1 : 0;
        const bool moving = (c != 0xFFFFFFFFu && v.locomotor == mv::LocomotorKind::Default) ? head_moving[c] > 0 : own;
        // Allow_Idle_When_Moving (+0x85) exempts the unit from the gate (the IDLE service and the presence writer alike).
        idle_gate_[i] = static_cast<uint8_t>(moving && !anim_->params_for(store.type(u.h), gm->kind()).allow_idle_when_moving);
    }
    imposing_presence(store, idle_gate_);
    // The default idle type's presence step (0x395920 step 2): among the records whose object still exists the largest
    // value wins; on <= an allied record (0x2823e0) replaces a non-allied best (ascending presence id); the idle = the
    // first Presence_Induced_Animations entry of the unit's type listing the winner's type, else none (the plain idle).
    const anim::PresenceIdleFn presence = [this, &st](anim::ObjectId unit, std::span<const anim::IdleRecord> recs) {
        return presence_pick(st, unit, recs.data(), recs.size());
    };
    for (size_t i = 0; i < units_.size(); ++i) {
        const Unit& u = units_[i];
        if (!u.alive || u.retreated || !w->contains(u.id)) continue;
        anim_->set_berserker(*gm, u.id, abilities_->is_active(u.id, ta::kBerserker));
        // s78k t02 (TACTICAL_COMBAT slot 47 "s78k t02", IdleBehavior 0x62e570) [H]: the IDLE service reads the TARGETING
        // behaviour's target (a land team member: its head's) whether or not it is suspended (Prevent_All_Fire), and
        // draws Get_Int(0, FPS) every service while one is held. current_target keeps the target while suspended.
        const anim::IdleTarget target = !combat_->has_unit(u.id)                         ? anim::IdleTarget::None
                                        : combat_->current_target(u.id) != tc::kNoObject ? anim::IdleTarget::Target
                                                                                         : anim::IdleTarget::NoTarget;
        const bool gated = idle_gate_[i] != 0;
        anim::LocomotorInput in;
        in.movable = u.movable;
        in.on_path = gated;
        in.target = target;
        if (u.movable) {
            const mv::UnitView v = movement_->view(u.id);
            // s78n t26.1 review: only 0x19 with the walking-deployed flag leaves the deploy states to the locomotor clips
            // (clip 115); 0x18 with the flag set (an order shorter than M while deploying) keeps its Deploy clip.
            if (v.deploy_state != mv::LocoState::None && !(v.deploy_state == mv::LocoState::WalkDeployed && v.walk_deployed)) {
                // S8 t11 follow-up (MOVEMENT §3.2, 0x61e930): the Walk deploy states own the animation - the Deploy /
                // Undeploy clip (synced random variant at Deployment_Anim_Rate) starts when the locomotor starts timing
                // it; Deployed keeps the finished clip. [port] a reversed clip is drawn from its first frame.
                // s78l t03: the IDLE deploy gate (deploying / undeploying / deployed) returns with nothing changed.
                // s78n t26.1: the deployed pose after a deployed walk is set above (deploy_pose_events).
                if (v.deploy_state != mv::LocoState::WalkDeployed && v.deploy_clip >= 0 && w->current_type(u.id) != v.deploy_clip &&
                    anim_->deploy(*gm, u.id, v.deploy_clip == static_cast<int32_t>(anim::kAnimUndeploy), rng, gframe))
                    ++stats_.anim_plays;
                continue;
            }
            // s78n t26.1 (MOVEMENT §3.2.1 item 3, state 0x13 0x61f840): walking deployed plays clip 115 (Deployed_Walk) at
            // Deployed_Walk_Anim_Speed x the speed over the UNDEPLOYED effective max (the move step's f, §3.5.2 C.1)
            in.deployed = v.walk_deployed;
            if (v.locomotor != mv::LocomotorKind::Default) {  // REMAINING_GAPS §4.3-4.7: the locomotor picks the clip
                if (v.anim != mv::kAnimNone && w->current_type(u.id) != v.anim) {
                    anim::PlayRequest req;
                    req.type = v.anim;  // variant 0, no draw; one-shot [port: the locomotor re-requests looping states]
                    if (w->play(u.id, req, nullptr, gframe) == anim::PlayResult::Played) ++stats_.anim_plays;
                }
                // s78l t03 (a): then the IDLE service under the same moving gate as every other locomotor.
                if (anim_->service_idle(*gm, u.id, gated, target, rng, gframe, &presence)) ++stats_.anim_plays;
                continue;
            }
            in.speed = v.speed;
            in.max_speed = v.max_speed;
            in.yaw_deg = v.yaw;
            // s78l t02: the arrival turn plays its own clips (play_turn_clip_events), also on the frame it ends. The IDLE gate
            // does not count that frame as moving: the path pops inside the locomotor (behaviour 1) before the IDLE service
            // (behaviour 7) of the same frame (MOVEMENT 3.5.1 step 6).
            in.host_turn = v.arrival_turn || v.arrival_turn_ended;
        }
        stats_.anim_plays += anim_->service_locomotor(*gm, u.id, in, cfg_.walk_speed_ratio, cfg_.min_move_anim_ratio, rng, gframe, &presence);
    }
#else
    (void)store, (void)rng, (void)gframe;
#endif
}

void TacticalBattle::spawn_particle(TacticalMode& mode, int32_t type, Vec3 pos, int32_t owner) {
    // R4 B.1 layer 1: a PARTICLE GameObject (consumes an id), removed at frame + Particle_Lifetime_Frames.
    if (type < 0 || static_cast<size_t>(type) >= catalog_->types().size()) return;
    objects::SpawnParams sp;
    sp.type = static_cast<objects::TypeIndex>(type);
    sp.owner = owner;
    sp.position = pos;
    sp.facing = {1.0f, 0.0f, 0.0f};
    const objects::ObjectHandle h = mode.primary().create(sp);  // not in the spatial trees (never a target)
    int32_t life = catalog_->types().get_int(sp.type, "Particle_Lifetime_Frames");
    if (life <= 0) life = static_cast<int32_t>(cfg_.particle_default_lifetime);
    particles_.push_back({mode.primary().id(h), h, frame_ + static_cast<uint32_t>(life)});
    ++stats_.particles;
}

objects::ObjectHandle TacticalBattle::spawn_death_clone(TacticalMode& mode, int32_t type, Vec3 pos, Vec3 facing, int32_t owner) {
    if (type < 0 || static_cast<size_t>(type) >= catalog_->types().size()) return {};
    objects::SpawnParams sp;
    sp.type = static_cast<objects::TypeIndex>(type);
    sp.owner = owner;
    sp.position = pos;
    sp.facing = facing;
    return mode.spawn_death_clone(sp);
}

bool TacticalBattle::death_clone_corpse_path(uint32_t dying_type, int32_t clone_type, int32_t key) const {
    // S9_DEATH_CLONE_SHIELDS.md §1.6 [L]: the clone is made by the mode's other object manager (mode +0x20, the
    // secondary / local store of OBJECT_RUNTIME §1 [M]) when the dying type is in category Infantry, the clone type has
    // none of HIDE_WHEN_FOGGED (0x19), IMPOSING_PRESENCE (0x40), LAND_OBSTACLE (0x14), PROXIMITY_MINE (0x55) and the
    // key is not 4 (Force lightning). [port] The 0x5051d0 test is not read (taken as passing). A secondary-store object
    // takes no sim id, so the port makes no object at all: the clone lives only as the dying unit's corpse row
    // (SimAnimation::corpse_clone). Vanilla: the typed infantry clones (Infantry_*_Death_Clone_*: melt, disruptor) take
    // it; the own-type infantry clones keep the primary path (their types have HIDE_WHEN_FOGGED).
    const objects::GameObjectTypes& types = catalog_->types();
    if (clone_type < 0 || static_cast<size_t>(clone_type) >= types.size() || dying_type >= types.size() || key == 4) return false;
    if (!infantry_category_) {
        const auto m = types.enums().get(objects::EnumId::GameObjectCategory).lookup("Infantry");
        infantry_category_ = m ? static_cast<uint64_t>(*m) : ~uint64_t{0};
    }
    if (*infantry_category_ == ~uint64_t{0} || (types.get_mask(dying_type, "CategoryMask") & *infantry_category_) == 0) return false;
    const auto ct = static_cast<objects::TypeIndex>(clone_type);
    for (const objects::BehaviorTypeId b : {objects::BehaviorTypeId{0x19}, objects::BehaviorTypeId{0x40}, objects::BehaviorTypeId{0x14},
                                            objects::BehaviorTypeId{0x55}})
        if (types.has_behavior(ct, objects::BehaviorSet::Union, b)) return false;
    return true;
}

objects::ObjectHandle TacticalBattle::spawn_prop(TacticalMode& mode, int32_t type, Vec3 pos, Vec3 facing, int32_t owner) {
    if (type < 0 || static_cast<size_t>(type) >= catalog_->types().size()) return {};
    objects::SpawnParams sp;
    sp.type = static_cast<objects::TypeIndex>(type);
    sp.owner = owner;
    sp.position = pos;
    sp.facing = facing;
    return mode.spawn(sp, 1.0f);
}

void TacticalBattle::consume_events(TacticalMode& mode, SimRng& rng) {
    objects::ObjectStore& store = mode.primary();
    // a. damage events -> DAMAGE_TRACKING (call order, before the deaths disable the victims).
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
    // A1_READINGS §7 [H; M flag names]: amount = the actual hull + shield loss (shield absorbed + hull damage; a hit
    // routed to a hard point changes neither), recorded only when > 0 and only for DAMAGE_TRACKING victims. A per-attacker
    // entry needs the source type's Is_Valid_Target (+0x6b) and Collidable_By_Projectile_Living (+0x41); its key is the
    // source's TEAM parent when it has one. §30.2 [H]: the hull part is the clamped loss (hullBefore - max(0, hullAfter),
    // 0 when the cannot-die restore ran), so the killing hit records at most the remaining hull. [port] The running total /
    // last-damage frame (+0x8 / +0x50) are not kept (nothing reads them yet).
    {
        const objects::GameObjectTypes& dtypes = catalog_->types();
        for (const tc::DamageEvent& e : combat_->damage_events()) {
            if (e.source == tc::kNoObject || !damage_tracking_.tracks(static_cast<int32_t>(e.victim))) continue;
            const float amount = e.absorbed + (e.hardpoint < 0 ? e.hull_lost : 0.0f);
            if (!(amount > 0.0f)) continue;
            const objects::ObjectHandle sh = store.find(static_cast<objects::ObjectId>(e.source));
            if (!store.alive(sh)) continue;
            const objects::TypeIndex st = store.type(sh);
            if (st >= dtypes.size() || !dtypes.get_bool(st, "Is_Valid_Target") || !dtypes.get_bool(st, "Collidable_By_Projectile_Living"))
                continue;
            auto key = static_cast<int32_t>(e.source);
            if (const Unit* su = unit_by_id(static_cast<objects::ObjectId>(e.source)); su && su->container != 0xFFFFFFFFu &&
                                                                                       containers_[su->container].team)
                key = static_cast<int32_t>(containers_[su->container].id);
            damage_tracking_.record(static_cast<int32_t>(e.victim), key, amount);
        }
    }
#endif
    // b. projectile detonations -> PARTICLE objects: detonation particle, then the synced hit pick.
    for (const tc::HitEvent& e : combat_->hit_events()) {
        if (e.projectile_desc >= tables_->projectiles.size()) continue;
        const BattleTables::ProjectileVisuals& pv = tables_->projectiles[e.projectile_desc];
        const Vec3 at = sv(e.position);
        if (e.victim == tc::kNoObject) {
            spawn_particle(mode, pv.lifetime, at, e.owner);
            continue;
        }
        const int32_t det = e.absorbed ? pv.absorbed : (e.armor_reduced && pv.armor_reduced >= 0 ? pv.armor_reduced : pv.detonation);
        spawn_particle(mode, det, at, e.owner);
#ifdef ALAMO_SIM_HAVE_ANIM
        hooks_->anim_hit(e);  // R3 anim: flinch
#endif
        const objects::ObjectHandle vh = store.find(e.victim);
        if (e.pick < 0 || !store.exists(vh)) continue;
        if (const BattleTables::UnitVisuals* vis = visuals_of_type(static_cast<uint32_t>(store.type(vh)))) {
            const std::vector<int32_t>& list = e.absorbed ? vis->shield_hit : vis->hit;
            if (static_cast<size_t>(e.pick) < list.size()) spawn_particle(mode, list[static_cast<size_t>(e.pick)], at, store.owner(vh));
        }
    }
    // c. hardpoint destructions: Death_Explosion_Particles, then Death_Breakoff_Prop (each an id).
    for (const tc::HardpointEvent& e : combat_->hardpoint_events()) {
        ++stats_.hardpoints_destroyed;
        const objects::ObjectHandle oh = store.find(e.owner);
        if (!store.exists(oh)) continue;
        Vec3 at = store.position(oh);
        // battle host: at the hard point's attachment bone (bind pose, BattleCatalog model data; unit centre without it):
        // unit-local (x forward, y left, z up) rotated by the unit's yaw.
        if (combat_->has_unit(e.owner)) {
            const tc::UnitDesc& ud = combat_->data().units[combat_->desc_index(e.owner)];
            if (e.index < ud.hardpoints.size() && ud.hardpoints[e.index] < combat_->data().hardpoints.size()) {
                const auto off = combat_->data().hardpoints[ud.hardpoints[e.index]].attach_offset;
                const Vec3 f = store.facing(oh);
                const float len = dmath::sqrt(f.x * f.x + f.y * f.y);
                const float fx = len > 0.0f ? f.x / len : 1.0f, fy = len > 0.0f ? f.y / len : 0.0f;
                at = Vec3{at.x + fx * off.x - fy * off.y, at.y + fy * off.x + fx * off.y, at.z + off.z};
            }
        }
        spawn_particle(mode, e.explosion_type, at, store.owner(oh));
        if (e.breakoff_type >= 0 && spawn_prop(mode, e.breakoff_type, at, store.facing(oh), store.owner(oh))) ++stats_.props;
    }
    // d. deaths: destroy, death clone object, first Death_Explosions entry [port: pick rule untraced].
    for (const tc::DeathEvent& d : combat_->deaths()) process_death(mode, d, rng);
    // e. damaged smoke (render attachment).
    for (const tc::SmokeEvent& e : combat_->smoke_events()) {
        const objects::ObjectHandle h = store.find(e.id);
        if (!store.exists(h)) continue;
        const BattleTables::UnitVisuals* vis = visuals_of_type(static_cast<uint32_t>(store.type(h)));
        effects_.push_back({e.id, vis ? vis->smoke : std::string(), e.percent, frame_});
        ++stats_.smoke;
    }
    // f. ability events.
    engines_restarted_.clear();
    for (const ta::AbilityEvent& e : abilities_->events()) {
        ++stats_.ability_events;
        if (e.kind == ta::AbilityEventKind::Activated) ++stats_.activations;
        if (e.kind == ta::AbilityEventKind::EnginesOffline)
            if (Unit* u = unit_by_id(e.id)) {
                engine_loops_.engines_offline(e.id, frame_);  // §3.3: both engine loops off
                if (e.value == 1) {  // destroyed: Locomotor_Set_Engines(destroyed = 1), permanent
                    u->engines_offline = true;
                    l1_engines_destroyed(e.id);
                } else {  // disabled by the station-upgrade copy: EngineControl::disable(id, -1) (HARDPOINT_DISABLED §9.1)
                    l1_engines_.disable(e.id, -1.0f, frame_);
                    l1_used_ = true;
                }
            }
        if (e.kind == ta::AbilityEventKind::EnginesOnline)
            if (Unit* u = unit_by_id(e.id)) {  // an ENGINE hardpoint repaired to max (0x393980 -> 0x554640)
                if (l1_engines_.repair_online(e.id, frame_)) l1_used_ = true;
                u->engines_offline = false;
                engines_restarted_.push_back(e.id);
                engine_loops_.engines_online(e.id, frame_);  // §3.3: idle loop on
            }
        if (e.kind == ta::AbilityEventKind::SpawnObject) spawn_ability_object(mode, e);  // fog follow-up: FOW_REVEAL_PING
        // cu8g t01 (CU8F_T01_T02_T07_RE.md §2.3): Saber_Throw's throw snaps the saber holstered (0x635450: suppress, fade 0).
        // cu8k t01 (§2.7 items 1-4): the apply records T (e.point); the throw comes kSaberThrowDelay frames later (anim 0x6c
        // past its half [M]) and spawns the Saber_Name projectile (saber_throw_service); hg_holster_service snaps the saber
        // holstered at the throw, holds the draw while it flies (to the ability's end, resolved by the machine) and snaps
        // drawn only on a real catch. value 0 = the activation failed (the forwarding path's Unhandled): nothing thrown.
        if (e.kind == ta::AbilityEventKind::Unhandled && e.type == ta::kSaberThrow && e.value > 0) {
            SaberThrowState st;
            st.thrower = e.id;
            st.target = e.target;
            st.aim = Vec3{e.point.x, e.point.y, e.point.z};
            st.throw_frame = frame_ + ta::kSaberThrowDelay;
            if (abilities_) abilities_->saber_params(e.id, st.projectile, st.catch_dist);
            sabers_.erase(std::remove_if(sabers_.begin(), sabers_.end(), [&](const SaberThrowState& x) { return x.thrower == e.id; }),
                          sabers_.end());  // a new throw replaces a pending one (as saber_schedule)
            sabers_.push_back(st);
            engine_loops_.saber_schedule(e.id, st.throw_frame, 0u, tactical::hangar::EngineLoops::kSaberOpenEnd);
        }
        // S7 t15 A1 (ABILITIES_HARDPOINTS §2.6): every ability end raises signal 0x29, including the Remote_Bomb toss, which
        // ends at its Toss_Anim end (LOB_SOLVER §3.5 step 4, S8 t06).
        if (e.kind == ta::AbilityEventKind::Ended) ability_finished_.push_back(e.id);
        // S7 t26 (AI_TACTICAL §7.3.3 item 3): DEPLOY raises 0x29 when the deploy (anim 25 -> state 0x19) or undeploy (anim
        // 26 -> state 0) clip ends, at once on a blocked reverse: oi_deploy_sync raises it from the movement deploy events
        // (S8 t11), not the AbilitySystem Deployed event, which fires at the request.
    }
    oi_events(mode);  // OI abilities: ability moves / stops / owner changes, ability spawns, death survivors
    oi_deploy_sync();  // S8 t11 / t12: this pass's deploy requests and jetpack swaps
    abilities_->clear_events();
}

void TacticalBattle::process_death(TacticalMode& mode, const tc::DeathEvent& d, SimRng& rng, bool silent) {
    objects::ObjectStore& store = mode.primary();
    {
        Unit* u = unit_by_id(d.id);
        if (!u || !u->alive) return;  // not a battle unit (dummy targets are invulnerable)
        u->alive = false;
        --parts_[u->participant].alive;
        hg_unit_removed(u->id);  // hangars: carrier / single launched ship gone; engine loops forgotten
        // battle host: object removal record (death)
        bt_removals_.push_back({store.owner(u->h), u->type, u->container != kNoContainer ? containers_[u->container].type : u->type, false});
        const Vec3 at = store.position(u->h);
        const Vec3 face = store.facing(u->h);
        const int32_t owner = store.owner(u->h);
        mode.destroy(u->h);
        vm_deaths_.push_back(u->id);  // story t06: the victory monitor sees this tick's deaths in this order
        s4_hover_death(u->id, at, face);  // s78n t20 batch 2: before the mover (and its hover state) goes
        if (u->movable) movement_->remove_unit(u->id);
        else {
            (void)movement_->remove_unit(u->id);  // s78n t23 step 0: a registered mine obstacle (no-op otherwise)
            movement_->object_removed(u->id);     // s78f review: blocks naming a station / structure (fighter formations)
        }
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
        if (u->tracked) damage_tracking_.disable(u->id);
#endif
        ++stats_.deaths;
        // Die step 8 clone (0x29f270, S9_DEATH_CLONE_SHIELDS.md §1.1 / §1.6 [H]): every tactical death with kind != 15
        // makes one - the picked Death_Clone type, else the dying type itself (S9 t04) - at the dying object's transform,
        // owned by its player, with +0x3a0 |= 0xc0 (refix t01: TacticalMode::spawn_death_clone; the store's active()
        // searches skip it), carrying the death sequence (anim: the clone's own, by the killing kind). refix t03: the
        // infantry corpse path takes no object (death_clone_corpse_path).
        objects::ObjectHandle ch{};
        bool corpse_path = false;
        if (d.death_clone >= 0) {
            corpse_path = death_clone_corpse_path(u->type, d.death_clone, d.clone_key);
            if (!corpse_path) ch = spawn_death_clone(mode, d.death_clone, at, face, owner);
            if (ch) ++stats_.death_clones;
        }
        // refixf t04: the clone's DeathBehavior (spin-away, death clip, Remove_Upon_Death, persistence, fade) is planned
        // here from data and the battle removes the clone object at the planned frame - identical with and without the
        // animation service, which only plays the planned clip (schedule_death_clone).
        const bool carried_by_clone = static_cast<bool>(ch);
        if (ch) schedule_death_clone(mode, ch, d.death_clone, d.kind, rng);
#ifdef ALAMO_SIM_HAVE_ANIM
        {  // R3 anim: death clip / corpse of the unit (unless a clone carries it), or the infantry corpse path
            bool carried = carried_by_clone || silent;  // s78n t12 review: a silent removal plays no death clip
            if (!ch && corpse_path) carried = hooks_->anim_corpse_clone(u->id, d.death_clone, d.kind);
            hooks_->anim_death(u->id, carried, d.kind);
        }
#else
        (void)carried_by_clone;
#endif
        if (const BattleTables::UnitVisuals* vis = visuals_of_type(u->type))
            if (!silent && !vis->death_explosions.empty()) spawn_particle(mode, vis->death_explosions.front(), at, owner);
        unit_gone(mode, *u);
    }
}

// --- fog follow-up: S9 SpawnObject (FOW_REVEAL_PING, FOG_OF_WAR §3.3): the record's Spawned_Object_Type at the
// target's position (the user's when the target is gone) for the user's owner. The shipped ping projectiles have
// Max_Speed 0 / no flight distance and detonate at once [M]: the object lives one tick (PARTICLE-style expiry) and
// reveals through its REVEAL behaviour meanwhile (fog_ping). It consumes an object id.
void TacticalBattle::spawn_ability_object(TacticalMode& mode, const ta::AbilityEvent& e) {
    const ta::UnitAbilityRecord* r = abilities_->record(e.id, e.type);
    if (!r || r->spawned_object_type.empty()) return;
    const objects::TypeIndex t = catalog_->types().raw().find(r->spawned_object_type);
    if (t == objects::kNone || t >= catalog_->types().size()) return;
    objects::ObjectStore& store = mode.primary();
    const objects::ObjectHandle user = store.find(e.id);
    objects::ObjectHandle at = store.find(e.target);
    if (!store.alive(at)) at = user;
    if (!store.alive(at)) return;
    const objects::ObjectHandle h =
        spawn_prop(mode, static_cast<int32_t>(t), store.position(at), store.facing(at), store.exists(user) ? store.owner(user) : -1);
    if (!h) return;
    particles_.push_back({store.id(h), h, frame_ + 1});
    fog_ping(store.id(h));
}

// --- end fog follow-up ---

// --- OI tactical: crush (MOVEMENT.md 5.5) - 1,000,000 damage of the crusher type's Squash_Damage_Type ---
void TacticalBattle::oi_apply_crush(uint32_t frame) {
    for (const mv::CrushEvent& e : oi_crush_) {
        const Unit* cr = unit_by_id(e.crusher);
        if (!cr || !combat_->alive(e.victim)) continue;
        tc::DamageRequest r;
        r.victim = e.victim;
        r.amount = mv::kCrushDamage;
        r.source = e.crusher;
        std::string_view name = catalog_->types().get_string(cr->type, "Squash_Damage_Type");
        while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.remove_prefix(1);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.remove_suffix(1);
        r.damage_type = name.empty() ? tc::kNoTypeId : tables_->combat.damage.damage_index(name);
        combat_->take_damage(r, frame);
    }
    oi_crush_.clear();
}

// --- end OI tactical ---

// refixf t04: DeathBehavior of a death clone object (ANIM_PARTICLES §A.7 death / corpse rows, TACTICAL_GAPS §1.9 [H]):
// spin-away decision, death clip (Specific_Death_Anim_Type / _Index, by damage kind), removal at once (no clip, no spin,
// Remove_Upon_Death), else the corpse countdown (death clip length, Death_Persistence_Duration < 0 = forever, then
// Death_Fade_Time) - anim::plan_death, with the clip data of the clone's model read from the files (the clip length is
// sim-visible, §A.8). The draws (spin float, random variant) happen here, at the spawn, in the sim's draw order, with or
// without the animation service; the service plays the planned clip and draws nothing more for the clone. [port] The
// original runs this in the clone's DEATH behaviour (0x58cdb0 / 0x58d430) and removes the object there; the port keeps
// the removal frame with the battle so a headless run removes the same objects on the same frame as alamo_game.
void TacticalBattle::schedule_death_clone(TacticalMode& mode, objects::ObjectHandle clone, int32_t clone_type, uint8_t kind, SimRng& rng) {
    const auto t = static_cast<objects::TypeIndex>(clone_type);
#ifdef ALAMO_SIM_HAVE_ANIM
    SimAnimation* src = anim_ ? anim_ : clone_models_;
    if (!src && catalog_->files()) {
        if (!own_clone_models_) own_clone_models_ = std::make_unique<SimAnimation>(catalog_->types(), *catalog_->files());
        src = own_clone_models_.get();
    }
    const anim::DeathPlan plan = src ? src->plan_clone_death(t, mode.kind(), kind, rng)
                                     : anim::plan_death(nullptr, anim::death_params(anim::type_anim_params(catalog_->types(), t, mode.kind())),
                                                        kind, rng);
    const bool remove_now = plan.remove_now, forever = plan.forever;
    const uint64_t lifetime = plan.lifetime;
#else
    // [port] a build without engine/anim has no clip data and no spin-away: Remove_Upon_Death -> at once, else
    // Death_Persistence_Duration (< 0 forever, default) + Death_Fade_Time (default 0.25 s), int(30 x s) each.
    (void)rng, (void)kind;
    const objects::GameObjectTypes& types = catalog_->types();
    const auto seconds = [&types, t](const char* field, float def) {
        return types.get_string(t, field).empty() ? def : types.get_float(t, field);
    };
    const auto frames = [](float s) { return s > 0.0f ? static_cast<uint64_t>(s * 30.0f) : uint64_t{0}; };
    const bool remove_now = types.get_bool(t, "Remove_Upon_Death");
    const float persist = seconds("Death_Persistence_Duration", -1.0f);
    const bool forever = !remove_now && persist < 0.0f;
    const uint64_t lifetime = frames(persist) + frames(seconds("Death_Fade_Time", 0.25f));
#endif
    if (remove_now || (!forever && lifetime == 0)) {
        mode.destroy(clone);
        ++stats_.death_clones_removed;
        return;
    }
    uint32_t at = kForeverFrame;
    if (!forever) at = lifetime >= static_cast<uint64_t>(kForeverFrame - 1u - frame_) ? kForeverFrame - 1u : frame_ + static_cast<uint32_t>(lifetime);
    death_clones_.push_back({mode.primary().id(clone), clone, at});
    register_clone_obstacle(mode, clone, clone_type);  // s78n t20 batch 1
#ifdef ALAMO_SIM_HAVE_ANIM
    hooks_->anim_clone(clone, kind, &plan);
#endif
}

// s78n t20 batch 1 (MOVEMENT §5.4.5 item 2, §5.2 item 4): the clone is in the land layer as an object without a locomotor
// (obj +0x333 = -1): never Moving, never a checker, the obstacle b of the movers' collision records. Its footprint is
// its type's (Custom_Hard_X/YExtent x Scale_Factor, else the radius; UnitCollisionClass through Variant_Of_Existing_Type).
// [port] registered only on land and only for clones that stay (a Remove_Upon_Death clone is destroyed before). s78n t23
// step 0: also a Proximity_Mine created by an ability (oi_spawn_unit, the battle unit's death removes it); not yet a
// resting Proj_Remote_Bomb (a CombatSystem projectile, no battle object).
void TacticalBattle::register_clone_obstacle(TacticalMode& mode, objects::ObjectHandle clone, int32_t clone_type) {
    if (space_ || !movement_ || clone_type < 0) return;
    const objects::GameObjectTypes& types = catalog_->types();
    const auto t = static_cast<objects::TypeIndex>(clone_type);
    if (t >= types.size() || !types.get_bool(t, "IsDeathCloneObstacle")) return;
    mv::UnitDesc md = mv::unit_desc_from_type(types, t);
    if (md.collision_class < 0) return;
    const objects::ObjectStore& store = mode.primary();
    const Vec3 pos = store.position(clone), face = store.facing(clone);
    md.id = store.id(clone);
    md.pos = {pos.x, pos.y};
    md.z = pos.z;
    md.yaw_deg = yaw_of(face);
    md.owner = store.owner(clone);
    md.no_locomotor = true;
    md.death_clone_obstacle = true;
    // the locomotor-side data of the variant's base type does not apply (the clone's behaviours were stripped)
    md.locomotor = mv::LocomotorKind::Default;
    md.crusher = md.supercrusher = false;
    md.turret = false;
    md.hover_height = 0.0f;
    md.uses_multiple_locomotors = md.has_secondary_locomotor = false;
    mv_type_extents(md, t);
    movement_->add_unit(md);
}

void TacticalBattle::kill_pushed_clones(TacticalMode& mode) {
    if (space_ || !movement_ || movement_->push_kills().empty()) return;
    // 0x391310(b, 1): the clone dies by the normal death. [port] it is destroyed here (its own death plan ends at once;
    // a clone has no death clone of its own), and leaves the movement system and the battle's clone list.
    const std::vector<objects::ObjectId> ids(movement_->push_kills().begin(), movement_->push_kills().end());
    objects::ObjectStore& store = mode.primary();
    for (const objects::ObjectId id : ids) {
        movement_->remove_unit(id);
        const auto it = std::find_if(death_clones_.begin(), death_clones_.end(), [id](const TimedObject& o) { return o.id == id; });
        if (it == death_clones_.end()) continue;
        if (store.alive(it->h)) {
            mode.destroy(it->h);
            ++stats_.death_clones_removed;
        }
        death_clones_.erase(it);
    }
}

void TacticalBattle::remove_death_clones(TacticalMode& mode, bool all) {
    // Due clones go at their planned frame; at the battle's end (all) every clone left - forever ones included - goes with
    // it [port]: the tactical store is dropped when the mode pops (BT §3.1), and nothing of a clone (a removing object, no
    // battle unit) reaches the galactic write-back (units / participants only), the removal records or a later save.
    objects::ObjectStore& store = mode.primary();
    size_t w = 0;
    for (size_t i = 0; i < death_clones_.size(); ++i) {
        if (all || frame_ >= death_clones_[i].frame) {
            if (store.alive(death_clones_[i].h)) {
                mode.destroy(death_clones_[i].h);
                ++stats_.death_clones_removed;
            }
            if (movement_) movement_->remove_unit(death_clones_[i].id);  // s78n t20 batch 1 (a registered clone obstacle)
            continue;
        }
        death_clones_[w++] = death_clones_[i];
    }
    death_clones_.resize(w);
}

void TacticalBattle::expire_objects(TacticalMode& mode) {
    remove_death_clones(mode, false);  // refixf t04
    objects::ObjectStore& store = mode.primary();
    size_t w = 0;
    for (size_t i = 0; i < particles_.size(); ++i) {
        if (frame_ >= particles_[i].frame) {
            if (store.alive(particles_[i].h)) store.destroy(particles_[i].h);
            continue;
        }
        particles_[w++] = particles_[i];
    }
    particles_.resize(w);
    // Dummy targets: removed once no unit holds a pending ability on them (or after dummy_lifetime) [port].
    w = 0;
    for (size_t i = 0; i < dummies_.size(); ++i) {
        const TimedObject d = dummies_[i];
        bool used = frame_ <= d.frame;
        if (!used && frame_ < d.frame + cfg_.dummy_lifetime)
            for (const Unit& u : units_)
                used = used || (u.alive && !u.retreated && abilities_->pending_target(u.id) == d.id);
        if (used) {
            dummies_[w++] = d;
            continue;
        }
        abilities_->remove(d.id);
        if (store.alive(d.h)) store.destroy(d.h);
    }
    dummies_.resize(w);
}

void TacticalBattle::tick(TacticalMode& mode, SimRng& rng, const ParallelFor& pf, double sim_time, uint64_t global_frame) {
    if (finished_) {  // finished outside a tick (auto-resolve stop): the clones go before the mode pops (refixf t04)
        if (!death_clones_.empty()) remove_death_clones(mode, true);
        return;
    }
    ++frame_;
    oi_global_frame_ = global_frame;
    stats_.frames = frame_;
    // s78n t12 review: deaths raised between ticks by command-phase ability effects - the vehicle thief's eject (unit
    // ability 46 / 18 off: Take_Damage kind 13 kills the stolen vehicle), a STIM_PACK that kills its user - are still in
    // the combat system's list, which the coming combat tick clears. Process them now, in list order, like the combat
    // tick's own deaths (process_death skips a unit that was processed already: last tick's deaths, script_damage's).
    // [port] before this the host never saw such a death (the object stayed, dead in combat only).
    if (combat_ && !combat_->deaths().empty()) {
        // s78n t19: the hooks get this tick's context first, so what a late death starts (its death clip, a corpse
        // clone) is stamped with the current frame, not with the tick before
        if (hooks_) hooks_->set_tick(&rng, global_frame);
        const std::vector<tc::DeathEvent> late(combat_->deaths().begin(), combat_->deaths().end());
        for (const tc::DeathEvent& d : late) process_death(mode, d, rng);
    }
    story_reinforced_.clear();  // L1/S4 wave 2b story hook: this tick's reserve deployments
    ability_finished_.clear();  // S7 t15 A1: this tick's signal 0x29
    landings_unloaded_.clear();  // a1i t04: signal 0x19 records of the last tick
    production_done_.clear();    // q6 t19: signal 0x22 records of the last tick
    story_garrisoned_.clear();  // S4: this tick's garrison entries / structure captures
    story_captured_.clear();
    share_groups_sync();  // S8 t17 (FIGHTER_FLYING §4.10 B): squadron of the shared team target, before this tick's orders
    objects::ObjectStore& store = mode.primary();
    l1_weather_tick(rng);  // L1 systems: land weather service (before the common tick)
    hero_clash_tick(mode, rng, global_frame);  // s78n t06: land tick step 4, after the weather (HERO_CLASH §2.1)
    fog_decay();  // fog of war: common-tick step 4, before the object services (owner: fog agent)
    hg_production(mode, rng);  // station contents / skirmish tactical production (before the object services)
    profile::lap("tac.setup");
    // 1. reinforcements, 2. stand-in orders, 3. retreat coordinator.
    reinforce(mode, rng, true);
    landing_service(mode, rng);  // a1i t04: landing objects (timed unload steps, take-off)
    if (cfg_.order_interval > 0 && (frame_ - 1) % cfg_.order_interval == 0) issue_orders(mode, rng);
    retreat_policy();
    if (retreat_side_ >= 0 && !retreat_complete_ && frame_ >= retreat_done_frame_) {
        bt_retreat_attrition(static_cast<uint8_t>(retreat_side_), rng);  // battle host: vf12 at the countdown end (§4.9)
        complete_retreat(mode, static_cast<uint8_t>(retreat_side_));
    }
    profile::lap("tac.orders");
    attack_block_service(mode, rng);  // s78n t20 stage A: serial, ascending mover, before the collision feed and the movement tick
    // 4. speed factors, movement, positions into the store and the trees, locomotor animation.
    apply_speed_factors();
    fleet_tick(mode);  // [port] fleet groups: slowest-member cap and straggler wait (no-op while no group exists)
    feed_collision_info(store);  // s78n t20: targets / ranges / hero clashes for the land collision decisions
    fighter_sync();  // S7 fighters: new squadrons (map start, reinforcements, hangar launches, wingmen) and params
    feed_attack_targets(store);
    if (space_) avoid_snapshot(store);  // SPACE_AVOIDANCE: committed member transforms for the fighter avoidance hook
    profile::lap("tac.pre_move");
    movement_->tick(frame_, rng, pf);
    movement_->commit(store);
    kill_pushed_clones(mode);  // s78n t20 batch 1: the death clones a collision push killed
    play_turn_clip_events(rng, global_frame);  // s78l t02: the arrival turn's clip plays (synced variant draws), ascending id
    s4_exit_arrivals();  // s78n t26.1: the garrison hosts' +0xf8 hook for the walk-out moves that finished
    oi_deploy_sync();  // S8 t11: deploy clip ends (Is_Active, deploy flags, signal 0x29) before the combat tick
    profile::lap("tac.movement");
    fighter_enter_idle();  // S7 t22 / t25: enter_idle drops the squadrons' team targets (§4.7 step 4, §4.10 D)
    forward_fighter_events();  // S7 fighters: Provoke / Retarget -> combat attack orders (before the combat tick)
    for (const objects::ObjectId id : movement_->despawned())  // §4.6: land bombers at their path end, no death effects
        if (const Unit* u = unit_by_id(id)) bt_kill_silently(mode, u->h);
    oi_crush_.assign(movement_->crush_events().begin(), movement_->crush_events().end());  // OI tactical
    l1_motion(mode, rng);  // L1 systems: hyperspace jumps, engine re-enables
    arrival_service(mode, rng);  // cu8l t03: the start fleet's hyperspace arrivals (timer draws, finishes)
    l1_force_disable(mode);  // cu8 t02: Disable_Force_Abilities_Range emitters (Force-disable stamps)
    hg_engine_service();   // engine loops: SimpleSpace locomotor services (moving first, then idle)
    bt_team_positions(mode);  // battle host: space squadrons / non-steering land squads at their members' box centre
    profile::lap("tac.motion_post");
    mode.resolve_models();  // refixf t07: after a load, before the boxes and the parallel sweeps read the model cache
    for (const Unit& u : units_)
        if (u.alive && !u.retreated && u.movable && store.alive(u.h))
            mode.trees().update(static_cast<tactical::EntityHandle>(u.id), mode.bounds_of(u.h));
    mode.trees().maintain(sim_time, pf);
    profile::lap("tac.trees");
    drive_animation(store, rng, global_frame);
    profile::lap("tac.anim");
    // 5. combat (abilities run inside its serial phases), events.
    for (const Unit& u : units_) {
        if (!u.alive || u.retreated || !u.movable) continue;
        combat_->set_position(u.id, tv(store.position(u.h)));
        const mv::UnitView mvv = movement_->view(u.id);
        combat_->set_facing(u.id, tv(facing_of_yaw(mvv.yaw)));
        // S7 t14 (MOVEMENT §3.1.1 step 6): the locomotor swap disables TARGETING / WEAPON / SPECIAL_WEAPON for the jetpack
        // flight and enables them on the swap back. The swap runs inside MovementSystem (orders, the JET_PACK ability, the
        // jetpack's state 0x16), so the flag mirrors the active locomotor here, before every combat tick.
        // S7 t15 A1: the swap back then raises "unit ability finished" (signal 0x29: task-force event 19, the blocking
        // Activate_Ability); the flag still set from the previous tick marks it.
        if (!mvv.secondary_locomotor && (combat_->flags(u.id) & tc::kUnitWeaponsSuspended) != 0) ability_finished_.push_back(u.id);
        combat_->set_flag(u.id, tc::kUnitWeaponsSuspended, mvv.secondary_locomotor);
        // S8 t11 (TACTICAL_COMBAT §3.1.1 "Gate details" step 6): fighter Idle (0x1C), and Walk Stopped (0x16) on a locomotor
        // whose vf +0x240 is true (Flying, JetPack, LandBomber; [port] Starship: the port's space locomotors keep no 0x16).
        // ROCKET_ATTACK in any Unit_Abilities_Data entry ("ROCKET_ATTACK detection").
        const bool air = mvv.locomotor == mv::LocomotorKind::Flying || mvv.locomotor == mv::LocomotorKind::Jetpack ||
                         mvv.locomotor == mv::LocomotorKind::LandBomber;
        combat_->set_flag(u.id, tc::kUnitFighterIdle, mvv.loco_state == mv::LocoState::FighterIdle);
        combat_->set_flag(u.id, tc::kUnitHoverRest, air && mvv.loco_state == mv::LocoState::WalkStopped);
        // s78n t20 batch 2 (MOVEMENT §5.3 "Hover in full"): a Hover_Height unit raised (state 1) - melee attackers refuse it
        combat_->set_flag(u.id, tc::kUnitHoverRaised, movement_->hover_state(u.id) == 1);
        combat_->set_flag(u.id, tc::kUnitRocketAttack, abilities_ && abilities_->is_active(u.id, 11));
        // s78f t03 (TACTICAL_COMBAT §3.1.1 "Targeting states"): the locomotor is moving (an order in progress or speed), and
        // the model has an Attack clip (0x397ae0(obj, 5)): a moving land unit without TURRET fires only without one.
        combat_->set_flag(u.id, tc::kUnitMoving, mvv.has_goal || mvv.speed > 0.0f);
        bool attack_clip = false;
#ifdef ALAMO_SIM_HAVE_ANIM
        if (const anim::AnimWorld* w = hooks_ ? hooks_->world() : nullptr)
            if (const anim::AnimModel* m = w->contains(u.id) ? w->model(u.id) : nullptr) attack_clip = m->variant_count(anim::kAnimAttack) > 0;
#endif
        combat_->set_flag(u.id, tc::kUnitAttackClip, attack_clip);
        // S8 t17 (FIGHTER_FLYING §4.10 A / B): a squadron member shares its squadron's team target; a fighter whose
        // formation record exists scans R + Rr (fighter_scan_range).
        if (space_) {
            const bool member = u.container < containers_.size() && movement_->fighter_squad(containers_[u.container].id);
            combat_->set_share_group(u.id, member ? containers_[u.container].id : tc::kNoObject);
            combat_->set_flag(u.id, tc::kUnitScanQuery,
                              mvv.locomotor == mv::LocomotorKind::Fighter && fighter_formation(fighter_key(u)) != nullptr);
        }
    }
    s4_positions(mode);  // S4: garrisoned units sit on their holders
    combat_team_positions(store);  // s78n t03: the land teams' TeamTargeting P_U (cleared after the combat tick)
    profile::lap("tac.combat_pre");
    hooks_->set_tick(&rng, global_frame);
    abilities_->tick(frame_, mode.trees(), rng, pf);
    combat_->clear_team_positions();  // s78n t03: readers between ticks use the head's own position (save / load equal)
    profile::lap("tac.combat");
    consume_events(mode, rng);
    oi_respawn_due(mode);  // OI abilities: Tactical_Respawn creations due this frame
    profile::lap("tac.events");
    hg_tick(mode, rng);    // hangars: SPAWN_SQUADRON services and the launches they request
    saber_throw_service(mode);  // cu8k t01: Saber_Throw throws / SWORD_CONTROL / SWORD_CATCH (after the saber's step)
    hg_holster_service();  // cu8g t01: HOLSTER_WEAPON (engine-loop suppression, draw / holster SFX requests)
    l1_weapons(mode, rng);  // L1 systems: special-weapon volleys, TSW state machines
    profile::lap("tac.services");
    expire_objects(mode);
    s4_tick(mode, rng);  // S4: garrison enters / exits / ejects, structure capture
    pads_tick(mode);     // Int-final: build pads (construction completion, children gone)
    sk_income(mode);     // skai: skirmish map battles: the Income_Stream credit trickle
    bt_shield_stamps(mode);  // battle host: base-shield secondary occupants follow the live shields (land)
    service_damage_tracking();
    profile::lap("tac.cleanup");
    fog_service(mode);  // fog of war: REVEAL sources, attacker flashes (owner: fog agent)
    fog_setup_tick();   // FOWF t05: setup phase end (all humans ready / countdown)
    profile::lap("tac.fog");
    team_attack_service(mode, rng);  // q6: attacks on TEAM containers (re-pick, end), before the queues read the orders
    advance_queues(mode, rng);  // [port: Gabriel] 5b. queueable unit actions: completed orders start the next entry
    late_swap_back_signals();   // S7 t27: a jetpack swap back after the combat_pre loop raises signal 0x29 in this tick
    stats_.shots = combat_->stats().shots;
    stats_.hits = combat_->stats().hits;
    // cu8j t01 (MOVEMENT §4.6.2): the movement coordinator update (retail common tick step 10, after the object services):
    // this tick's new coordinators compose their formations (the FormationClass ctor draws, the land stagger draws).
    // Orders given after it (the AI and story services of the host) compose at the next tick's.
    movement_->coordinator_update(frame_, global_frame, rng);
    // 6. victory.
    check_victory(mode);  // story t06: the victory monitor (losses since the last tick, countdown, end)
    if (finished_ && !death_clones_.empty()) remove_death_clones(mode, true);  // refixf t04: the battle's clones end with it
    space_obstacles_sync(mode);  // refixf t02: this tick's new / dead / retreated SPACE_OBSTACLE units
    land_obstacles_sync(mode, pf);   // S8 t02: this tick's new / gone LAND_OBSTACLE objects
    hg_publish_engine_requests();  // engine loops: this tick's requests for the audio host
    feed_pair_gates();  // s78n t18 review: the gates the command phase reads follow this tick's deaths / retreats
    profile::lap("tac.victory");
}

// S7 t27 (MOVEMENT §3.1.1 step 6): the swap back raises signal 0x29 at once. The combat_pre loop sees the swaps of the
// movement tick; a swap back later in the tick (after that loop: ability events, orders, queues) is reported here, in the
// same tick, and the flag cleared so the next combat_pre loop does not report it again.
void TacticalBattle::late_swap_back_signals() {
    if (!movement_ || !combat_) return;
    for (const Unit& u : units_) {
        if (!u.alive || u.retreated || !u.movable || !combat_->has_unit(u.id)) continue;
        if ((combat_->flags(u.id) & tc::kUnitWeaponsSuspended) == 0 || movement_->view(u.id).secondary_locomotor) continue;
        ability_finished_.push_back(u.id);
        combat_->set_flag(u.id, tc::kUnitWeaponsSuspended, false);
    }
}

// ---- S7 t31: signal 0xb (AI_TACTICAL §7.3.3 item 2, A1_READINGS §33.3) ----------------------------------------------

uint32_t TacticalBattle::add_order_replaced_listener(OrderReplacedListener fn) {
    const uint32_t token = next_listener_token_++;
    order_replaced_listeners_.emplace_back(token, std::move(fn));
    return token;
}

void TacticalBattle::remove_order_replaced_listener(uint32_t token) {
    order_replaced_listeners_.erase(std::remove_if(order_replaced_listeners_.begin(), order_replaced_listeners_.end(),
                                                   [token](const auto& l) { return l.first == token; }),
                                    order_replaced_listeners_.end());
}

void TacticalBattle::raise_order_replaced(std::span<const objects::ObjectId> ids) {
    if (order_replaced_listeners_.empty()) return;
    const std::vector<std::pair<uint32_t, OrderReplacedListener>> ls = order_replaced_listeners_;  // a listener may unsubscribe
    for (const objects::ObjectId id : ids)
        for (const auto& l : ls)
            if (l.second) l.second(id);
}

bool TacticalBattle::ability_raises_order_replaced(uint8_t ability) {
    // 0x570a50 / 0x220ed0: 2 3 5 6 13 15 16 17 18 20 21 22 27 31 32 34 37 38 39 41 45 46 50 51 52 53 55 58 59 64 66 67 69 71 73 74
    static constexpr uint8_t kIds[] = {2,  3,  5,  6,  13, 15, 16, 17, 18, 20, 21, 22, 27, 31, 32, 34, 37, 38,
                                       39, 41, 45, 46, 50, 51, 52, 53, 55, 58, 59, 64, 66, 67, 69, 71, 73, 74};
    return std::find(std::begin(kIds), std::end(kIds), ability) != std::end(kIds);
}

// ---- commands / owner ---------------------------------------------------------------------------------------------

std::vector<objects::ObjectId> TacticalBattle::owned_ids(const TacticalMode& mode, uint8_t player, const std::vector<uint32_t>& wire) const {
    std::vector<objects::ObjectId> out;
    const objects::ObjectStore& store = mode.primary();
    for (uint32_t w : wire) {
        if (w == cmd::kNoObject || w > 0x7FFFFFFFu) continue;
        const objects::ObjectHandle h = store.find(static_cast<objects::ObjectId>(w));
        if (!store.active(h) || store.owner(h) != static_cast<int32_t>(player)) continue;
        const objects::ObjectId id = store.id(h);
        if (!abilities_->has_object(id) || std::find(out.begin(), out.end(), id) != out.end()) continue;
        out.push_back(id);
    }
    return out;
}

// [port: Gabriel] queueable unit actions: a queued ability command appends one Ability entry per eligible user
// (idle users with an empty queue run it at once, as one command); a plain one clears its users' queues first.
cmd::RejectReason TacticalBattle::handle_command(TacticalMode& mode, uint8_t player, const cmd::EventData& ev, SimRng& rng) {
    using R = cmd::RejectReason;
    using T = cmd::EventType;
    if (finished_ || !abilities_) return R::NotAllowed;
    const T t = ev.type();
    if (t == T::Garrison || t == T::MoveToGarrison) return s4_command(mode, player, ev, rng);  // S4 garrisons
    // battle host: TacticalBuildEvent (33) / TacticalSell (34) - L1_S4_REMAINING §3.6; AI players build without a charge in campaign
    // battles and pay like a human in land skirmish (skai t09, tactical_build).
    if (t == T::Production) return hg_production_command(mode, player, ev);  // skirmish station production (else Unhandled)
    if (t == T::SetupPhaseMove || t == T::SetupPhaseTriggerEnd) return fog_setup_command(mode, player, ev);  // FOWF t05
    if (t == T::TacticalBuild) {
        const uint32_t pw = ev.u("pad"), tw = ev.u("type_index");
        if (pw == cmd::kNoObject || pw > 0x7FFFFFFFu) return R::InvalidTarget;
        const bool ai = player >= galaxy_->players().size() || !galaxy_->players()[player].human;
        return tactical_build(mode, player, static_cast<objects::ObjectId>(pw), static_cast<objects::TypeIndex>(tw), rng, ai);
    }
    if (t == T::TacticalSell) {
        const uint32_t ow = ev.u("object");
        if (ow == cmd::kNoObject || ow > 0x7FFFFFFFu) return R::InvalidUnit;
        return tactical_sell(mode, player, static_cast<objects::ObjectId>(ow));
    }
    // --- OI abilities: RepairHardpointEvent (45, 0x4308b0): adds the event player as a repairer of the hardpoint; no
    // ownership or credit check at command time, no cancel (TACTICAL_GAPS §1.12) ---
    if (t == T::RepairHardpoint) {
        const uint32_t ow = ev.u("object");
        if (ow == cmd::kNoObject || ow > 0x7FFFFFFFu) return R::InvalidUnit;
        return abilities_->repair_hardpoint(static_cast<objects::ObjectId>(ow), ev.u("hardpoint"), static_cast<int32_t>(player))
                   ? R::None
                   : R::InvalidTarget;
    }
    // --- end OI abilities ---
    // S8 t26 (TACTICAL_COMBAT §3.1.1 "Fire-at-position order", command 25 execute 0x40a3b0): readiness >= 0.99, point = the
    // command position when "at position" is set or there is no target object, else the target's bare position; then
    // Fire_Lobbing_At_Position (sw_fire_lobbing_at), then the destroy-after sweep (S8 t30: every live object of the
    // weapon's owner with DUMMY_DESTROY_AFTER_SPECIAL_WEAPON_FIRED 0x67, a copy of the owner's list in its order, killed
    // silently) [M: after an accepted order only, as the Lua binding]. [port] Lobbing superweapons of the issuing player
    // only.
    if (t == T::SpecialWeaponFire) {
        const uint32_t ww = ev.u("weapon");
        if (ww == cmd::kNoObject || ww > 0x7FFFFFFFu) return R::InvalidUnit;
        const objects::ObjectStore& store = mode.primary();
        const objects::ObjectHandle wh = store.find(static_cast<objects::ObjectId>(ww));
        if (!store.alive(wh) || store.owner(wh) != static_cast<int32_t>(player)) return R::InvalidUnit;
        const objects::ObjectId wid = store.id(wh);
        static const objects::BehaviorTypeId kDestroyAfter = objects::behavior_type_id("DUMMY_DESTROY_AFTER_SPECIAL_WEAPON_FIRED");
        auto destroy_after = [&]() {
            if (kDestroyAfter == objects::kNoBehavior) return;
            const auto owned = store.owned_by(store.owner(wh));
            const std::vector<objects::ObjectHandle> list(owned.begin(), owned.end());
            for (const objects::ObjectHandle h : list)
                if (store.alive(h) && store.has_behavior(h, kDestroyAfter)) bt_kill_silently(mode, h);
        };
        // ui1 t04 [port] (retail's human UI path is not traced, backlog b0238): the space special-weapon button sends
        // command 25 with a target object for a SPECIAL_WEAPON object or a space station; it fires as Lua
        // Fire_Special_Weapon does (SCRIPT_GAPS §1.1 steps 4 / 5): readiness >= 1, the SpecialWeaponsAllowed gate, begin
        // targeting + fire at the object (0x491b40 / 0x491cd0), or the station's special-weapon hard point (can-fire test
        // 0x386220 + assign); then the destroy-after sweep (step 6).
        if (ev.u("at_position") == 0 && !(combat_ && combat_->fires_at_position(wid))) {
            const uint32_t tw = ev.u("target");
            const objects::ObjectHandle th = tw == cmd::kNoObject || tw > 0x7FFFFFFFu ? objects::ObjectHandle{} : store.find(static_cast<objects::ObjectId>(tw));
            if (!store.alive(th)) return R::InvalidTarget;
            const objects::ObjectId tid = store.id(th);
            if ((l1_sw_.kinds(wid) & ta::kSwSpecialWeapon) != 0) {
                if (l1_sw_.readiness(wid, frame_) < 1.0f) return R::AbilityNotReady;
                if (!special_weapons_allowed(static_cast<int32_t>(player))) return R::NotAllowed;
                if (!l1_sw_.targeting(wid) && !sw_begin_targeting(wid)) return R::NotAllowed;
                if (!sw_fire_at_object(mode, wid, tid)) {
                    if (l1_sw_.targeting(wid)) sw_begin_targeting(wid);  // a second begin clears the flag (0x491b40)
                    return R::InvalidTarget;
                }
                destroy_after();
                return R::None;
            }
            if (combat_ && combat_->special_weapon_readiness(wid) >= 0.0f) {
                if (combat_->special_weapon_readiness(wid) < 1.0f) return R::AbilityNotReady;
                if (!special_weapons_allowed(static_cast<int32_t>(player))) return R::NotAllowed;
                if (!combat_->special_weapon_fire(wid, tid)) return R::InvalidTarget;
                destroy_after();
                return R::None;
            }
            return R::Unhandled;
        }
        if (!combat_ || !combat_->fires_at_position(wid)) return R::Unhandled;
        if (l1_sw_.readiness(wid, frame_) < 0.99f) return R::AbilityNotReady;
        Vec3 at{ev.f("x"), ev.f("y"), ev.f("z")};
        if (ev.u("at_position") == 0) {
            const uint32_t tw = ev.u("target");
            const objects::ObjectHandle th = tw == cmd::kNoObject || tw > 0x7FFFFFFFu ? objects::ObjectHandle{} : store.find(static_cast<objects::ObjectId>(tw));
            if (store.alive(th)) at = store.position(th);
        }
        if (!sw_fire_lobbing_at(mode, wid, at)) return R::NotAllowed;
        destroy_after();
        return R::None;
    }
    // cu9g t02 (HUD_GAPS §9.7, command 28 execute 0x5246d0): land only (no bombing-run manager in space). The point is the
    // dequantized wire value (the original's q1..q3 round trip); a valid target starts the run (0x492d60). No RNG.
    // Outcomes: no record / space -> NotAllowed, invalid point -> InvalidPosition (both silent in the original),
    // valid point but the run is not available -> AbilityNotReady (the original logs it and plays the GUI error SFX for
    // the local issuer; the app does that from this reason).
    if (t == T::BombingRun) {
        if (space_ || !l1_mode_) return R::NotAllowed;
        const bool has_record = std::any_of(l1_runs_.begin(), l1_runs_.end(), [&](const L1BombingRun& r) { return r.player == static_cast<int32_t>(player) && r.enabled; });
        if (!has_record) return R::NotAllowed;
        const Vec3 at{ev.f("x"), ev.f("y"), ev.f("z")};
        if (!bombing_run_target_valid(static_cast<int32_t>(player), at)) return R::InvalidPosition;
        if (!bombing_run_available(static_cast<int32_t>(player))) return R::AbilityNotReady;
        return start_bombing_run(mode, static_cast<int32_t>(player), at) ? R::None : R::NotAllowed;
    }
    if (t != T::SetUnitAbilityMode && t != T::TacticalSpecialAbility && t != T::TacticalSpecialAbilityWithDummyTarget &&
        t != T::SetAbilityAutofire)
        return R::Unhandled;
    const char* list = (t == T::TacticalSpecialAbility || t == T::TacticalSpecialAbilityWithDummyTarget) ? "users" : "units";
    if (!ev.queued()) {
        if (!queues_.empty()) clear_queues(owned_ids(mode, player, ev.ids(list)));
        return handle_ability(mode, player, ev, rng);
    }
    const std::vector<objects::ObjectId> ids = owned_ids(mode, player, ev.ids(list));
    if (ids.empty()) return R::InvalidUnit;
    const objects::ObjectStore& store = mode.primary();
    QueuedOrder q;
    q.kind = QueuedOrder::Kind::Ability;
    q.player = player;
    if (t == T::TacticalSpecialAbility) {
        const uint32_t w = ev.u("target");
        const objects::ObjectHandle th = w <= 0x7FFFFFFFu ? store.find(static_cast<objects::ObjectId>(w)) : objects::ObjectHandle{};
        if (!store.alive(th)) return R::InvalidTarget;
        q.target = store.id(th);
        const Vec3 p = store.position(th);
        q.x = p.x;
        q.y = p.y;
    } else if (t == T::TacticalSpecialAbilityWithDummyTarget) {
        if (dummy_type_ < 0) return R::NotAllowed;
        q.x = ev.f("x");
        q.y = ev.f("y");
    }
    q.ability = ev;
    q.ability.set_queued(false);
    const std::vector<objects::ObjectId> now = enqueue(ids, q);
    if (now.empty()) return R::None;
    cmd::EventData run = q.ability;
    std::vector<uint32_t> wire;
    for (const objects::ObjectId id : now) wire.push_back(static_cast<uint32_t>(id));
    run.set_ids(list, std::move(wire));
    return handle_ability(mode, player, run, rng);
}

cmd::RejectReason TacticalBattle::handle_ability(TacticalMode& mode, uint8_t player, const cmd::EventData& ev, SimRng& rng) {
    using R = cmd::RejectReason;
    using T = cmd::EventType;
    if (finished_ || !abilities_) return R::NotAllowed;
    objects::ObjectStore& store = mode.primary();
    auto object_of = [&store](uint32_t w) -> tc::ObjectId {
        if (w == cmd::kNoObject || w > 0x7FFFFFFFu) return tc::kNoObject;
        const objects::ObjectHandle h = store.find(static_cast<objects::ObjectId>(w));
        return store.alive(h) ? store.id(h) : tc::kNoObject;
    };
    switch (ev.type()) {
        case T::SetUnitAbilityMode: {  // 43
            const auto ids = owned_ids(mode, player, ev.ids("units"));
            if (ids.empty()) return R::InvalidUnit;
            ++stats_.ability_commands;
            const uint32_t n = abilities_->set_unit_ability_mode(ids, static_cast<uint8_t>(ev.u("ability")), ev.u("on") != 0, frame_,
                                                                 object_of(ev.u("object")));
            return n > 0 ? R::None : R::AbilityNotReady;
        }
        case T::TacticalSpecialAbility: {  // 40
            const auto users = owned_ids(mode, player, ev.ids("users"));
            const tc::ObjectId target = object_of(ev.u("target"));
            if (users.empty()) return R::InvalidUnit;
            if (target == tc::kNoObject) return R::InvalidTarget;
            ++stats_.ability_commands;
            const uint32_t hp = ev.u("id2");  // [port] the hardpoint index field (COMMANDS 2.2: "id32")
            const uint32_t n = abilities_->targeted_ability(users, static_cast<uint8_t>(ev.u("ability")), target,
                                                            hp < 64 ? static_cast<int32_t>(hp) : -1, frame_);
            return n > 0 ? R::None : R::AbilityNotReady;
        }
        case T::TacticalSpecialAbilityWithDummyTarget: {  // 57: Dummy_Barrage_Target at the ground point
            const auto users = owned_ids(mode, player, ev.ids("users"));
            if (users.empty()) return R::InvalidUnit;
            if (dummy_type_ < 0) return R::NotAllowed;
            const Vec3 at{ev.f("x"), ev.f("y"), ev.f("z")};
            const objects::ObjectId dummy = spawn_dummy_target(mode, static_cast<int32_t>(player), at, rng);  // wave 2b: shared
            // S8 t10 (LOB_SOLVER §3.5 "MARKER removal"): a Remote_Bomb marker has no lifetime - only the toss kills it
            // (MarkerTossed); it stays when the toss never happens. [port] the other abilities' markers keep the expiry.
            // A refused command (no user took the ability) leaves the marker on the normal expiry.
            ++stats_.ability_commands;
            const uint32_t n = abilities_->targeted_ability(users, static_cast<uint8_t>(ev.u("ability")), dummy, -1, frame_);
            if (n > 0 && ev.u("ability") == static_cast<uint32_t>(ta::kPlaceRemoteBomb) && dummy != objects::kNoObjectId && !dummies_.empty() && dummies_.back().id == dummy)
                dummies_.back().frame = 0xFFFFFFFFu;
            return n > 0 ? R::None : R::AbilityNotReady;
        }
        case T::SetAbilityAutofire: {  // 58
            const auto ids = owned_ids(mode, player, ev.ids("units"));
            if (ids.empty()) return R::InvalidUnit;
            ++stats_.ability_commands;
            abilities_->set_autofire(ids, static_cast<uint8_t>(ev.u("ability")), ev.u("on") != 0);
            return R::None;
        }
        default: return R::Unhandled;
    }
}

// --- L1/S4 wave 2b story hook: the command-57 Dummy_Barrage_Target (moved out of handle_command, unchanged) ---
// [port] owned by the issuer's enemy side (attack orders need an enemy target), never auto-targeted, invulnerable;
// not in the spatial trees.
objects::ObjectId TacticalBattle::spawn_dummy_target(TacticalMode& mode, int32_t player, Vec3 at, SimRng& rng) {
    if (dummy_type_ < 0 || !abilities_) return objects::kNoObjectId;
    objects::ObjectStore& store = mode.primary();
    const int32_t owner = player == req_.info.attacker ? req_.info.defender : req_.info.attacker;
    objects::SpawnParams sp;
    sp.type = static_cast<objects::TypeIndex>(dummy_type_);
    sp.owner = owner;
    sp.position = at;
    const objects::ObjectHandle h = store.create(sp);
    tc::UnitSpawn cs;
    cs.id = store.id(h);
    cs.desc = desc_of(static_cast<uint32_t>(dummy_type_));
    cs.owner = owner;
    cs.position = tv(at);
    cs.flags = tc::kUnitInvulnerable | tc::kUnitSkipTargeting;
    abilities_->add_unit(cs, cs.desc, frame_, rng);
    dummies_.push_back({cs.id, h, frame_});
    ++stats_.dummies;
    return cs.id;
}

// --- end L1/S4 wave 2b ---

bool TacticalBattle::change_owner(TacticalMode& mode, objects::ObjectHandle h, int32_t owner) {
    objects::ObjectStore& store = mode.primary();
    if (!store.alive(h) || owner < 0) return false;
    const objects::ObjectId id = store.id(h);
    if (!abilities_ || !abilities_->has_object(id)) return false;
    const int32_t old_owner = store.owner(h);
    if (!store.set_owner(h, owner)) return false;
    mode.trees().set_owner(static_cast<tactical::EntityHandle>(id), owner);
    abilities_->set_owner(id, owner);
    l1_sw_.set_owner(id, owner);  // L1 systems
    if (movement_) movement_->set_owner(id, owner);  // OI tactical: crush hostility
    fog_owner_changed(mode, h, old_owner);  // fog of war: REVEAL vf18 (owner: fog agent)
    s4_owner_changed(mode, id, old_owner, owner);  // S4: garrison owners (STORY_CAPTURE_HERO has no dispatcher: L1_S4_REMAINING §4.3)
    if (Unit* u = unit_by_id(id)) {
        if (owner == req_.info.defender) u->side = 0;
        else if (owner == req_.info.attacker) u->side = 1;
    }
    return true;
}

bool TacticalBattle::script_change_owner(TacticalMode& mode, objects::ObjectHandle h, int32_t owner) {
    objects::ObjectStore& store = mode.primary();
    if (!store.alive(h) || owner < 0) return false;
    if (!owner_change_one(mode, h, owner)) return false;
    team_change_owner(mode, store.id(h), owner);
    return true;
}

bool TacticalBattle::owner_change_one(TacticalMode& mode, objects::ObjectHandle h, int32_t owner) {
    objects::ObjectStore& store = mode.primary();
    if (change_owner(mode, h, owner)) return true;
    const objects::ObjectId id = store.id(h);
    const int32_t old_owner = store.owner(h);
    if (!store.set_owner(h, owner)) return false;
    mode.trees().set_owner(static_cast<tactical::EntityHandle>(id), owner);
    fog_owner_changed(mode, h, old_owner);
    s4_owner_changed(mode, id, old_owner, owner);
    return true;
}

// s78j t04 (TACTICAL_COMBAT §3.5 "Convert and its team rule" step 4, 0x38e5a0): Change_Owner spreads through a team by
// itself - a member calls it on its TEAM container, a container on each member in list order, and an object that already
// has the new owner returns at once. So after `id` (a member or the container) changed, the container follows, then
// every other live member ([port] list order = ascending id, as convert_container's spread in CombatSystem). The
// container's record (Container::owner: loss records, replenish spawns) follows too. CombatSystem::set_owner does not
// spread, so the host does it here for the Lua / story / AI Change_Owner paths.
void TacticalBattle::team_change_owner(TacticalMode& mode, objects::ObjectId id, int32_t owner) {
    objects::ObjectStore& store = mode.primary();
    uint32_t ci = kNoContainer;
    if (const Unit* u = unit_by_id(id)) {
        ci = u->container;
    } else {
        for (size_t c = 0; c < containers_.size(); ++c)
            if (containers_[c].id == id && !containers_[c].removed) ci = static_cast<uint32_t>(c);
    }
    if (ci >= containers_.size()) return;
    Container& ct = containers_[ci];
    ct.owner = owner;
    if (ct.id != id && !ct.removed && store.alive(ct.h) && store.owner(ct.h) != owner) owner_change_one(mode, ct.h, owner);
    for (size_t i = 0; i < units_.size(); ++i) {  // owner_change_one does not add or remove units
        const Unit& m = units_[i];
        if (m.container != ci || m.id == id || !m.alive || m.retreated || !store.alive(m.h) || store.owner(m.h) == owner) continue;
        owner_change_one(mode, m.h, owner);
    }
}

int32_t TacticalBattle::container_owner(objects::ObjectId id) const {
    for (const Container& c : containers_)
        if (c.id == id) return c.owner;
    return -1;
}

bool TacticalBattle::test_attack_idle(objects::ObjectId id, bool arg, SimRng& rng) {
#ifdef ALAMO_SIM_HAVE_ANIM
    if (!hooks_ || !hooks_->world() || !hooks_->world()->contains(id)) return false;
    hooks_->attack_idle(id, arg, frame_, rng);
    return true;
#else
    (void)id, (void)arg, (void)rng;
    return false;
#endif
}

bool TacticalBattle::test_play_clip(objects::ObjectId id, int32_t clip_type) {
#ifdef ALAMO_SIM_HAVE_ANIM
    if (!hooks_ || !hooks_->world() || !hooks_->world()->contains(id)) return false;
    anim::PlayRequest r;
    r.type = clip_type;
    r.index = 0;
    r.loop = true;
    return hooks_->world()->play(id, r, nullptr, hooks_->global_frame()) == anim::PlayResult::Played;
#else
    (void)id, (void)clip_type;
    return false;
#endif
}

// ---- reporting / CRC ----------------------------------------------------------------------------------------------

bool TacticalBattle::unit_attitude(objects::ObjectId id, float& roll_rad, float& pitch_rad) const {
    float r = 0.0f, p = 0.0f;
    const bool ok = movement_ && movement_->attitude(id, r, p);
    constexpr float kDegToRad = 0.017453292f;  // FIGHTER_FLYING §1.1 k
    roll_rad = r * kDegToRad;
    pitch_rad = p * kDegToRad;
    return ok;
}

std::string TacticalBattle::summary() const {
    auto faction = [this](int32_t p) -> std::string {
        if (p < 0 || static_cast<size_t>(p) >= galaxy_->players().size()) return "-";
        const uint32_t f = galaxy_->players()[static_cast<size_t>(p)].faction;
        return f < galaxy_->rules().factions.size() ? galaxy_->rules().factions[f].name : "?";
    };
    return std::string(space_ ? "space" : "land") + " battle " + req_.map + ": " + std::to_string(stats_.frames) + " frames, " +
           std::to_string(stats_.spawned) + " spawned (" + std::to_string(stats_.reinforcements) + " reinforcements), " +
           std::to_string(stats_.deaths) + " deaths, " + std::to_string(combat_ ? combat_->stats().scans : 0) + " scans, " + std::to_string(stats_.shots) + " shots / " + std::to_string(stats_.hits) +
           " hits, " + std::to_string(stats_.hardpoints_destroyed) + " hardpoints destroyed, " + std::to_string(stats_.activations) +
           " ability activations, " + std::to_string(stats_.particles) + " particle objects, " + std::to_string(stats_.props + stats_.death_clones) +
           " props/clones, winner " + faction(result_.winner) + ", victory " + victory_name(result_.victory) + ", retreated " +
           faction(result_.retreated) + ", galactic losses " + std::to_string(result_.destroyed.size()) +
           (result_.starbase_destroyed ? " + starbase" : "") +
           (skirmish_land() ? ", skirmish base " + std::to_string(stats_.skirmish_prebuilt) + " pre-built of " + std::to_string(stats_.skirmish_pads) +
                                  " pads, " + std::to_string(stats_.pad_builds) + " structures started"
                            : std::string());
}

void TacticalBattle::hash(StateHasher& h) const {
    h.u32(frame_);
    h.boolean(finished_);
    h.i32(retreat_side_);
    h.u32(retreat_done_frame_);
    h.boolean(retreat_complete_);
    if (vm_cp_armed_) {  // ccpgt t02: the domination timer (absent while not armed)
        h.i32(vm_cp_timer_);
        h.i32(vm_cp_player_);
    }
    if (vm_declared_ || vm_hold_ != 0 || !vm_queue_.empty()) {  // story t06: the victory monitor (absent while idle)
        h.boolean(vm_declared_);
        h.u8(vm_decl_.type);
        h.i32(vm_decl_.player);
        h.i32(vm_countdown_);
        h.i32(vm_hold_);
        h.u32(static_cast<uint32_t>(vm_queue_.size()));
        for (const auto& [type, player] : vm_queue_) {
            h.u8(type);
            h.i32(player);
        }
    }
    for (size_t p = 0; p < fleet_cohesion_.size(); ++p)  // [port] fleet cohesion: only tight players (none: as before)
        if (fleet_cohesion_[p] != 0) {
            h.u8(0x43);
            h.u32(static_cast<uint32_t>(p));
        }
    if (!attack_blocks_.empty()) {  // s78n t20 stage A (absent while none: space and older land states hash as before)
        h.u32(static_cast<uint32_t>(attack_blocks_.size()));
        for (const AttackBlock& r : attack_blocks_) {
            h.i32(r.mover);
            h.i32(r.target);
            h.u8(r.flags);
            h.u32(r.next);
            h.f32(r.gx);
            h.f32(r.gy);
        }
    }
    h.u32(placed_[0]);
    h.u32(placed_[1]);
    h.u32(static_cast<uint32_t>(parts_.size()));
    for (const Participant& p : parts_) {
        h.i32(p.galactic_id);
        h.boolean(p.deployed);
        h.u32(p.spawned);
        h.u32(p.alive);
    }
    h.u32(static_cast<uint32_t>(units_.size()));
    for (const Unit& u : units_) {
        h.i32(u.id);
        h.boolean(u.alive);
        h.boolean(u.retreated);
    }
    h.i32(result_.winner);
    h.u8(result_.victory);
    h.i32(result_.retreated);
    h.boolean(result_.starbase_destroyed);
    h.u32(static_cast<uint32_t>(result_.destroyed.size()));
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
    damage_tracking_.hash(h);
#else
    h.u32(0);  // empty DAMAGE_TRACKING store (same layout without engine/ai)
#endif
    if (queue_used_) hash_queues(h);  // [port: Gabriel] order queues (absent until an order was queued)
    if (fleet_.used()) fleet_.hash(h);  // [port] fleet groups (absent until a group was made; never with the switch off)
    s4_hash(h);                       // S4: garrisons / capture points (absent while none exist)
    if (bt_used_) bt_hash(h);         // battle host: pad units, HIJACK reserves, lead object, attrition (absent until used)
    if (!fformations_.empty()) {      // S7 t22: fighter formations (absent while none)
        h.u32(static_cast<uint32_t>(fformations_.size()));
        for (const FighterFormationRecord& r : fformations_) {
            h.i32(r.key);
            h.u8(r.flags);
            h.f32(r.goal.x);
            h.f32(r.goal.y);
            h.f32(r.goal.z);
            h.i32(r.goal_object);  // s78f t02: the auto blocks are hashed with the movement formation
        }
    }
    if (!hero_clash_.empty()) hero_clash_.hash(h);  // s78n t06: running hero clashes (absent while none)
    if (!landings_.empty()) {         // a1i t04: landing objects waiting for take-off (absent while none)
        h.u32(static_cast<uint32_t>(landings_.size()));
        for (const Landing& l : landings_) {
            h.i32(l.id);
            h.u8(l.state);
            h.u32(l.unloaded);
            h.u32(l.timer_start + l.timer_len);
            h.u32(l.remove_frame);
        }
    }
}

void TacticalBattle::hash_abilities(StateHasher& h) const {
    if (abilities_) abilities_->hash(h);
    if (combat_) combat_->hash_extended(h);
    if (movement_) movement_->hash_speed(h);
    h.u32(static_cast<uint32_t>(units_.size()));
    for (const Unit& u : units_) {
        h.u8(u.side);
        h.boolean(u.engines_offline);
    }
    h.u32(static_cast<uint32_t>(containers_.size()));
    for (const Container& c : containers_) {
        h.i32(c.id);
        h.u32(c.alive);
    }
    h.u32(static_cast<uint32_t>(particles_.size()));
    for (const TimedObject& p : particles_) {
        h.i32(p.id);
        h.u32(p.frame);
    }
    h.u32(static_cast<uint32_t>(dummies_.size()));
    for (const TimedObject& d : dummies_) {
        h.i32(d.id);
        h.u32(d.frame);
    }
    if (!death_clones_.empty()) {  // refixf t04 (absent while none)
        h.u32(static_cast<uint32_t>(death_clones_.size()));
        for (const TimedObject& c : death_clones_) {
            h.i32(c.id);
            h.u32(c.frame);
        }
    }
    if (l1_used_) l1_hash(h);  // L1 systems (absent until a pack / jump / engine entry existed)
    if (pads_used_) pads_hash(h);  // Int-final: build pads (absent until a pad was used)
    if (!oi_respawns_.empty()) oi_hash(h);  // OI abilities: pending Tactical_Respawn creations (absent while none)
    if (hg_used_) hg_hash(h);  // hangars: carriers, launches, upgrade tech (absent until a carrier / upgrade existed)
}
}  // namespace alamo::sim
