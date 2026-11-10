// OI abilities - the unit-ability framework pieces of docs/re/TACTICAL_GAPS.md §1 / §2.1 / §2.2 / §7.3-7.4 on top of
// the S9 AbilitySystem: SAP-init draws, Active_By_Default, the autofire driver, order cancel, event-driven classes
// (Combat_Bonus / Enhance_Defense auras, Battlefield_Modifier, Base_Power, Weatherproof, Retreat_Prevention,
// Enable_Ability), the handler bodies (DEPLOY, JET_PACK, BARRAGE, SPREAD_OUT, REPLENISH_WINGMEN, instant effects) and
// the HUNT / SELF_DESTRUCT / CONFUSE behaviours, proximity mines and the combat effect hook.
//
// RNG: every draw goes through draw_*() on the bound synced RNG (the tick's, or bind_rng() outside a tick), in the
// order the spec gives; no draw happens in a parallel phase (all of this runs in the combat tick's serial phase D or
// in command / Lua calls).
#include <algorithm>
#include <array>

#include "alamo/core/dmath.hpp"
#include "alamo/tactical/abilities/ability_system.hpp"
#include "alamo/tactical/category_reduce.hpp"

namespace alamo::tactical::abilities {

using combat::CombatSystem;
using combat::ModList;

// ================================================================================================
// helpers
// ================================================================================================

float AbilitySystem::draw_unit() { return rng_ ? rng_->inline_unit_float() : 0.0f; }
float AbilitySystem::draw_float(float lo, float hi) { return rng_ ? rng_->range_float(lo, hi) : lo; }
int32_t AbilitySystem::draw_int(int32_t lo, int32_t hi) { return rng_ ? rng_->range_int(lo, hi) : lo; }

bool AbilitySystem::moving(const Obj& o) const {
    if (host_.moving) return host_.moving(o.id);
    if (o.combat_unit) return combat_->moved(o.id);  // [port] fallback: position changed since the last tick
    for (ObjectId m : o.members)
        if (alive_unit(m) && combat_->moved(m)) return true;
    return false;
}

void AbilitySystem::move_order(ObjectId id, Vec3 p, uint32_t frame, uint8_t type) {
    AbilityEvent e;
    e.kind = AbilityEventKind::MoveOrder;
    e.type = type;
    e.id = id;
    e.point = p;
    e.frame = frame;
    events_.push_back(e);
}

void AbilitySystem::stop_order(ObjectId id, uint32_t frame, int32_t hold) {
    event(AbilityEventKind::StopOrder, 0, id, kNoObject, frame);
    events_.back().value = hold;
}

void AbilitySystem::change_owner(Obj& o, int32_t owner, uint32_t frame) {
    // 0x38e5a0 (Hack / Tactical_Bribe / convert): the object and, for a team member, its container.
    auto one = [&](Obj& x) {
        if (x.dead || x.owner == owner) return;
        x.owner = owner;
        if (x.combat_unit) combat_->set_owner(x.id, owner);
        AbilityEvent e;
        e.kind = AbilityEventKind::OwnerChanged;
        e.id = x.id;
        e.value = owner;
        e.frame = frame;
        events_.push_back(e);
    };
    const ObjectId cid = o.container;
    one(o);
    if (cid != kNoObject)
        if (Obj* c = obj(cid)) one(*c);
}

uint32_t AbilitySystem::request_spawn(SpawnRequest r) {
    r.serial = spawn_serial_++;
    spawns_.push_back(r);
    return r.serial;
}

bool AbilitySystem::bind_spawn(uint32_t serial, ObjectId id) {
    for (Mine& m : mines_)
        if (m.serial == serial) {
            m.id = id;
            return true;
        }
    // REMAINING_GAPS §5.2: a DEPLOY_TROOPERS trooper joins its owner's trooper list (+0x10), spawn order.
    for (const SpawnRequest& r : spawns_)
        if (r.serial == serial && r.purpose == SpawnPurpose::Troopers) {
            Obj* o = obj(r.source);
            if (!o) return false;
            o->troopers.push_back(id);
            o->trooper_squads.push_back(r.squad);  // cu8f t06: its squad (CU8F_T04_T06_RE §3.2)
            return true;
        }
    // cu8f t06 (CU8F_T04_T06_RE §3.1): the LURE Particle_Effect object (LureDataPack +0x18). cu8g t05: false (not bound:
    // the host removes it silently) when the lure was deactivated since the request, or its owner is gone.
    for (const SpawnRequest& r : spawns_)
        if (r.serial == serial && r.purpose == SpawnPurpose::LureEffect) {
            Obj* o = obj(r.source);
            if (!o || r.cancelled || o->dead) return false;
            o->lure_effect = id;
            return true;
        }
    return false;
}

float AbilitySystem::max_health_and_shield(ObjectId id) const {
    // (max tactical health 0x3727a0 + max shield 0x3725f0)
    return combat_->max_health(id) + combat_->max_shield_of(id);
}

bool AbilitySystem::map_rect(Vec3& lo, Vec3& hi) const {
    if (host_.map_rect && host_.map_rect(lo, hi)) return hi.x > lo.x && hi.y > lo.y;
    // [port] fallback: the bounding box of the live combat units, grown by 1000.
    bool any = false;
    for (size_t i = 0; i < combat_->unit_count(); ++i) {
        const ObjectId u = combat_->unit_id_at(i);
        if (!combat_->alive(u)) continue;
        const Vec3 p = combat_->position(u);
        if (!any) {
            lo = hi = p;
            any = true;
        }
        lo.x = std::min(lo.x, p.x);
        lo.y = std::min(lo.y, p.y);
        hi.x = std::max(hi.x, p.x);
        hi.y = std::max(hi.y, p.y);
    }
    if (!any) return false;
    lo.x -= 1000.0f;
    lo.y -= 1000.0f;
    hi.x += 1000.0f;
    hi.y += 1000.0f;
    return true;
}

// ================================================================================================
// creation: SAP init (§4.3) and Active_By_Default (§1.2)
// ================================================================================================

void AbilitySystem::sap_init(Obj& o, uint32_t frame, SimRng* rng) {
    // 0x37c050 (Init_From_Type 0x3989a0) [H draws, M path]: one synced Get_Int(now, now + interval) per special ability
    // with a service interval > 0 that is allowed in the mode (the first due frame stagger), Abilities list order.
    for (SpecialState& sp : o.specials) {
        const SpecialAbilityDesc& s = data_->specials[sp.desc];
        sp.victims.reserve(8);
        if (interval_of(s) <= 0 || !allowed(s)) continue;
        const auto now = static_cast<int32_t>(frame);
        sp.due = rng ? static_cast<uint32_t>(rng->range_int(now, now + interval_of(s))) : frame;
    }
}

void AbilitySystem::active_by_default(Obj& o, uint32_t frame) {
    // 0x3a82a0: the type's FIRST TWO Unit_Abilities_Data entries; each with Active_By_Default ->
    // Set_Unit_Ability_Active(obj, t, on, pos 0, owner, no target).
    const AbilityObjectDesc& d = desc(o);
    for (size_t i = 0; i < d.unit_abilities.size() && i < 2; ++i) {
        const UnitAbilityRecord& r = d.unit_abilities[i];
        if (!r.active_by_default) continue;
        if (o.dead) return;
        if (!active_state(o, r.type)) set_active(o, r.type, true, frame, kNoObject);
    }
}

void AbilitySystem::apply_active_by_default(ObjectId id, uint32_t frame) {
    if (Obj* o = obj(id)) {
        active_by_default(*o, frame);
        if (Obj* p = obj(id)) sync_combat(*p);
    }
}

// ================================================================================================
// autofire (§1.3) and order cancel (§1.1)
// ================================================================================================

bool AbilitySystem::set_ability_autofire(ObjectId id, uint8_t type, bool on) {
    // 0x3a78b0: slot byte +9 and the +0x100 map entry; Supports_Autofire required unless the owner is AI.
    Obj* o = obj(id);
    if (!o || type < 1 || type > kMaxAbilityType || !has(*o, type)) return false;
    const UnitAbilityRecord* r = record_of(*o, type);
    if (!r) return false;
    if (!r->supports_autofire && !combat_->ai_player(o->owner)) return false;
    o->autofire[type] = on ? 1 : 0;
    const uint32_t crc = r->gui_name_crc;
    for (auto& [c, v] : o->autofire_map)
        if (c == crc) {
            v = on ? 1 : 0;
            return true;
        }
    o->autofire_map.emplace_back(crc, static_cast<uint8_t>(on ? 1 : 0));
    return true;
}

bool AbilitySystem::autofire_on(ObjectId id, uint8_t type) const {
    const Obj* o = obj(id);
    if (!o) return false;
    const UnitAbilityRecord* r = record_of(*o, type);
    const uint32_t crc = r ? r->gui_name_crc : 0u;
    for (const auto& [c, v] : o->autofire_map)
        if (c == crc) return v != 0;
    return combat_->ai_player(o->owner);  // AI-controlled and not human (0x2814d0 / player +0x62)
}

void AbilitySystem::autofire_driver(Obj& o, ObjectId target, uint32_t frame) {
    // Issue_Auto_Target (0x38d730) for a targeting-acquired target -> 0x37d280 (no explicit ability) [H].
    if (o.dead || o.sap_disabled || o.pending_type >= 0 || !cfg_.abilities_enabled || !alive_unit(target)) return;  // cu8f t01
    std::array<uint16_t, 32> cand{};
    std::array<uint8_t, 32> cand_type{};
    uint32_t n = 0;
    for (size_t i = 0; i < o.specials.size() && n < cand.size(); ++i) {
        const SpecialAbilityDesc& s = data_->specials[o.specials[i].desc];
        if (s.style != ActivationStyle::SpecialAttack && s.style != ActivationStyle::UserInput) continue;
        // the unit ability that forwards to it (its countdown and autofire state); none = no countdown gate [M]
        uint8_t t = 0;
        for (uint32_t k = 1; k <= kMaxAbilityType && t == 0; ++k)
            if (const UnitAbilityRecord* r = record_of(o, k); r && has(o, k) && r->gui_name_crc == s.name_crc) t = static_cast<uint8_t>(k);
        if (s.style == ActivationStyle::UserInput) {  // (1) User_Input needs autofire on (0x48e240 -> 0x48d5e0)
            if (t == 0) continue;
            if (!autofire_on(o.id, t)) continue;
        }
        if (t != 0 && desc(o).countdown && o.cd.blocks_activation(t)) continue;  // (2) countdown ready (0x42f820)
        if (!allowed(s) || !enabled(o, s) || !accepts(o, s, target, frame)) continue;  // (3)
        if (bm_blocked(o.owner, s.must_be_bought_on_black_market, s.name_crc)) continue;   // black-market gate 0x39b0b0
        cand[n] = static_cast<uint16_t>(i);
        cand_type[n] = t;
        ++n;
    }
    if (n == 0) {
        // Fallback 0x37c9a0: an immediate style-9 trigger (0x37b580(9, ...)); success aborts the attack order.
        for (SpecialState& sp : o.specials) {
            const SpecialAbilityDesc& s = data_->specials[sp.desc];
            if (s.style != ActivationStyle::SpecialAttack || !allowed(s) || !enabled(o, s)) continue;
            if (!accepts(o, s, target, frame) || bm_blocked(o.owner, s.must_be_bought_on_black_market, s.name_crc)) continue;
            const ObjectId oid = o.id;
            direct_ctx_ = 0;
            const bool ok = apply(o, sp, target, -1, frame);
            direct_ctx_ = 1;
            if (ok) {
                if (Obj* self = obj(oid); self && self->combat_unit) combat_->clear_order(oid);
                return;
            }
            if (!obj(oid)) return;
        }
        return;
    }
    // one candidate -> it; n >= 2 -> one synced Get_Int(0, n - 1).
    const uint32_t pick = n == 1 ? 0u : static_cast<uint32_t>(draw_int(0, static_cast<int32_t>(n) - 1));
    const SpecialAbilityDesc& s = data_->specials[o.specials[cand[pick]].desc];
    // The pick becomes the pending ability (targeting vfunc +0x130), fired through the pending mechanism (§2.9).
    o.pending_target = target;
    o.pending_hp = -1;
    o.pending_crc = s.name_crc;
    o.pending_type = cand_type[pick];
    o.pending_direct = 0;
}

void AbilitySystem::order_cancel(ObjectId id, uint32_t frame) {
    // 0x38f5d0 (every new move / attack / escort order): BARRAGE, LUCKY_SHOT, ION_CANNON_SHOT (on the team container)
    // and HUNT go off when active. (The garrison (0x51) service is the S4 host's.)
    Obj* o = obj(id);
    if (!o || o->dead) return;
    if (active_state(*o, kBarrage)) set_active(*o, kBarrage, false, frame, kNoObject);
    Obj* team = o->container != kNoObject ? obj(o->container) : o;
    if (!team) team = o;
    for (const uint32_t t : {uint32_t{kLuckyShot}, uint32_t{kIonCannonShot}}) {
        if (team->active[t]) {  // the special's next update finds it inactive and removes it (recharge rules §1.11)
            team->active[t] = 0;
            sync_combat(*team);
            for (ObjectId m : team->members)
                if (Obj* mo = obj(m)) {
                    mo->active[t] = 0;
                    sync_combat(*mo);
                }
            event(AbilityEventKind::Deactivated, static_cast<uint8_t>(t), team->id, kNoObject, frame);
        }
    }
    if (Obj* self = obj(id); self && active_state(*self, kHunt)) set_active(*self, kHunt, false, frame, kNoObject);
}

// ================================================================================================
// queries
// ================================================================================================

bool AbilitySystem::retreat_blocked(int32_t player) const {
    for (const SourceEntry& e : source_entries_)
        if (e.player == player && e.kind <= 1) return true;
    return false;
}

bool AbilitySystem::repair_hardpoint(ObjectId id, uint32_t hp, int32_t player) {
    return combat_->add_hardpoint_repairer(id, hp, player);
}

bool AbilitySystem::set_targeting_priorities(ObjectId id, std::string_view set_name) {
    Obj* o = obj(id);
    if (!o || o->dead) return false;
    int32_t set = -1;
    if (!set_name.empty()) {
        set = combat_->priority_set_index(set_name);
        if (set < 0) return false;
    }
    bool ok = false;
    if (o->combat_unit) ok = combat_->set_priority_override(id, set);
    for (ObjectId m : o->members)  // [port] a container's members (they score with their head's set)
        if (alive_unit(m)) ok = combat_->set_priority_override(m, set) || ok;
    return ok;
}

float AbilitySystem::player_reduce(int32_t player, uint8_t list) const {
    // max-then-sum over stacking categories in ascending order (0x33fb70), allocation-free; the sum starts at 0.0f.
    return reduce_by_category(
        [&](auto&& f) {
            for (const PlayerEntry& e : player_entries_)
                if (e.player == player && e.list == list) f(e.category, e.value);
        },
        [](float v, float best) { return v > best; }, false);
}

float AbilitySystem::player_list(int32_t player, PlayerList l) const { return player_reduce(player, static_cast<uint8_t>(l)); }

uint32_t AbilitySystem::player_gate(int32_t player, PlayerGate g, bool& all) const {
    // cu8f t06 (CU8F_T04_T06_RE §3.3, 0x2800d0 with BitwiseAnd<bool>): the AND of the list's values, insertion order.
    all = true;
    uint32_t n = 0;
    for (const PlayerEntry& e : player_entries_)
        if (e.player == player && e.list == static_cast<uint8_t>(g)) {
            ++n;
            all = all && e.value != 0.0f;
        }
    return n;
}

void AbilitySystem::set_player_gate_entry(int32_t player, PlayerGate g, uint32_t key, bool allowed) {
    // Lua Disable_Bombing_Run / Disable_Orbital_Bombardment (L1_S4_REMAINING §3.7: no expiry, keyed insert): one entry per
    // key, no object source; a later call replaces its value.
    const float v = allowed ? 1.0f : 0.0f;
    for (PlayerEntry& e : player_entries_)
        if (e.player == player && e.list == static_cast<uint8_t>(g) && e.source == kNoObject && e.crc == key) {
            e.value = v;
            return;
        }
    player_entries_.push_back(PlayerEntry{player, static_cast<uint8_t>(g), 0, v, kNoObject, key});
}

void AbilitySystem::gate_entries(const Obj& src, const SpecialAbilityDesc& s, bool push) {
    // cu8f t06 (CU8F_T04_T06_RE §3.3 [H]): apply 0x6f02c0 pushes {true, no expiry, source = this ability on this object}
    // per set Enable_* flag onto each affected player's list; remove 0x6f13b0 deletes the entries of that source. Affected
    // players (0x6f1130): the owner, or with Apply_To_All_Allies every player allied with it (index order).
    // s78n t07: "allied" is the relation-0 test (`friendly`, CU8F_T04_T06_RE §3.3): a neutral player gets nothing.
    if (!push) {
        player_entries_.erase(std::remove_if(player_entries_.begin(), player_entries_.end(),
                                             [&](const PlayerEntry& e) {
                                                 return e.list >= static_cast<uint8_t>(PlayerGate::BombingRuns) &&
                                                        e.source == src.id && e.crc == s.name_crc;
                                             }),
                              player_entries_.end());
        return;
    }
    if (!s.enable_bombing_runs && !s.enable_special_weapons && !s.enable_bombardment) return;
    const auto np = static_cast<int32_t>(combat_->player_count());
    for (int32_t p = 0; p < np; ++p) {
        if (p != src.owner && !(s.apply_to_all_allies && friendly(src.owner, p))) continue;
        auto put = [&](bool on, PlayerGate g) {
            if (on) player_entries_.push_back(PlayerEntry{p, static_cast<uint8_t>(g), 0, 1.0f, src.id, s.name_crc});
        };
        put(s.enable_bombing_runs, PlayerGate::BombingRuns);       // +0x191 -> list +0x1c8
        put(s.enable_special_weapons, PlayerGate::SpecialWeapons);  // +0x192 -> list +0x1e0
        put(s.enable_bombardment, PlayerGate::Bombardment);         // +0x193 -> list +0x250
    }
}

void AbilitySystem::modifier_entries(const Obj& src, uint32_t desc, const SpecialAbilityDesc& s, bool push) {
    // Battlefield_Modifier apply 0x6f02c0 (push) / remove 0x6f13b0 (delete by source); cu8g t05 (CU8F_T04_T06_RE §3.3 [H]):
    // Reverse_Application_Logic swaps the two (the caller passes push = !reverse on apply, reverse on remove), and the
    // affected players are always the owner, or every player allied with it (index order) with Apply_To_All_Allies
    // (0x6f1130; the earlier "reverse -> the enemies" reading [M] is gone; s78n t07: allied = relation 0, `friendly`,
    // so a neutral player is not affected). Per player, permanent append inserts keyed by
    // the source: the reveal list +0x1f8 (FOW §5.2) and the float lists, then the Enable_* gate lists (gate_entries).
    const auto np = static_cast<int32_t>(combat_->player_count());
    const uint64_t key = (static_cast<uint64_t>(desc) << 32) | static_cast<uint32_t>(src.id);
    if (!push) {
        if (host_.fow_reveal_remove && s.fow_reveal_multiplier != 1.0f)
            for (int32_t p = 0; p < np; ++p) host_.fow_reveal_remove(p, key);
        player_entries_.erase(std::remove_if(player_entries_.begin(), player_entries_.end(),
                                             [&](const PlayerEntry& e) {
                                                 return e.list < static_cast<uint8_t>(PlayerGate::BombingRuns) && e.source == src.id &&
                                                        e.crc == s.name_crc;
                                             }),
                              player_entries_.end());
        gate_entries(src, s, false);
        return;
    }
    for (int32_t p = 0; p < np; ++p) {
        if (p != src.owner && !(s.apply_to_all_allies && friendly(src.owner, p))) continue;
        if (s.fow_reveal_multiplier != 1.0f && host_.fow_reveal_add)  // list +0x1f8 <- m - 1 (FOW §5.2)
            host_.fow_reveal_add(p, s.stacking_category, key, s.fow_reveal_multiplier);
        auto put = [&](PlayerList l, float m) {
            if (m == 1.0f) return;
            player_entries_.push_back(PlayerEntry{p, static_cast<uint8_t>(l), s.stacking_category, m - 1.0f, src.id, s.name_crc});
        };
        // +0x230 holds round(F x m) - F frames (F = the reinforcement-time constant, unknown here): kept as m - 1, the
        // host scales its own timer [port].
        put(PlayerList::ReinforcementTime, s.reinforcement_time_multiplier);
        put(PlayerList::StealthDuration, s.stealth_duration_multiplier);
        put(PlayerList::CaptureTime, s.capture_time_multiplier);
        put(PlayerList::ReinforcementDeploy, s.reinforcement_deploy_multiplier);
    }
    gate_entries(src, s, true);
}

bool AbilitySystem::weather_affected(int32_t player) const {
    // player +0x410: empty or the AND of the values; every Weatherproof entry is false (§7.4).
    for (const SourceEntry& e : source_entries_)
        if (e.player == player && e.kind == 2) return false;
    return true;
}

bool AbilitySystem::hunting(ObjectId id) const {
    const Obj* o = obj(id);
    return o && o->behavior[kHunt] != 0;
}
bool AbilitySystem::deployed(ObjectId id) const {
    const Obj* o = obj(id);
    return o && active_state(*o, kDeploy);
}
bool AbilitySystem::spread_out(ObjectId id) const {
    const Obj* o = obj(id);
    return o && o->spread != 0;
}

// ================================================================================================
// event-driven classes (TACTICAL_COMBAT §4.3 / §5.4, TACTICAL_GAPS §2.3.11, §7.3, §7.4)
// ================================================================================================

bool AbilitySystem::aura_target(const Obj& src, uint32_t di, const Obj& t) const {
    // s78m t05: CombatBonusAbilityClass accept vf +0x28 (0x6f4bf0, TACTICAL_COMBAT §5.4.3 item 4), in order. s78n t04:
    // Enhance_Defense no longer comes here (its accept is the constant-false stub; §5.4.4 item 3c, enhance_defense_apply).
    const SpecialAbilityDesc& s = data_->specials[di];
    if (s.cls != SpecialClass::CombatBonus) return false;
    if (!t.combat_unit || t.dead || !alive_unit(t.id)) return false;
    // 1. the target filter 0x48e290 always runs (§5.4.1: types, then categories - an empty mask rejects, then excluded
    //    types); with no filter tags the aura affects no unit.
    if (!target_filter(s, t.id)) return false;
    // 2. owner: with Specific_Faction resolved (+0x1c0, s78n t04, §5.4.4 item 3a) the target owner's faction (player
    //    +0x68) must equal it and the ally test is skipped [port: an owner without a known faction fails]; else the target
    //    is allied with the source's owner (0x39a230: relation == 0, own player included; a neutral player is not
    //    allied). Apply_To_All_Allies is not read here.
    const int32_t sf = di < specific_faction_.size() ? specific_faction_[di] : -1;
    if (sf >= 0) {
        if (!host_.player_faction || host_.player_faction(t.owner) != sf) return false;
    } else if (!friendly(src.owner, t.owner)) {
        return false;
    }
    // 3. the target's type is neither the source type's Unique_Ground_Unit (+0x1b60) nor Unique_Space_Unit (+0x1b50).
    const AbilityObjectDesc& sd = data_->objects[src.desc];
    const int32_t ot = combat_->data().units[combat_->desc_index(t.id)].object_type;
    if (ot >= 0 && (ot == sd.unique_ground_type || ot == sd.unique_space_type)) return false;
    // 4. (the targeting data pack is created on demand in the port)
    return true;
}

void AbilitySystem::enhance_defense_apply(Obj& src, SpecialState& sp, uint32_t frame) {
    // s78n t04 (TACTICAL_COMBAT §5.4.4 item 3c, EnhanceDefenseAbilityClass apply 0x6fa920, reached only from SAP init at
    // the source's creation - apply_automatic): one permanent entry {Enhancement_Percentage, keyed by Stacking_Category,
    // source = S} appended to list 0 (defense) of the player's tactical ModifierSet, for p = the owner (a neutral owner ->
    // the battle's defender through AbilityHost::defense_player; -1 -> nothing), or with Apply_To_All_Allies for every
    // player whose relation to p is 0 (p included, player order). Never removed: not at the source's death or removal,
    // not by an abilities-disabled cancel (the apply registers no target), so `applied` stays 0 and `aura` marks the one
    // insert of this source (a second apply adds nothing [port]). The reader is every list-0 read of the player's objects
    // (Gather_Modifiers merges the player set; max-then-sum per category, the 0.75 cap of list 0), so the version bump of
    // the player's set invalidates their cached reductions (CombatSystem::ensure_cache). The validate 0x6fb080 sets a
    // negative Enhancement_Percentage to 0.
    if (sp.aura || src.dead) return;
    const SpecialAbilityDesc& s = data_->specials[sp.desc];
    const int32_t p = host_.defense_player ? host_.defense_player(src.owner) : src.owner;
    const auto np = static_cast<int32_t>(combat_->player_count());
    if (p < 0 || p >= np) return;
    sp.aura = 1;
    const float v = std::max(s.bonus[static_cast<size_t>(ModList::Defense)], 0.0f);
    const uint32_t crc = desc(src).name_crc;  // the CRC of S's type name (type +0x120)
    for (int32_t i = 0; i < np; ++i) {
        if (i != p && !(s.apply_to_all_allies && friendly(i, p))) continue;
        combat_->player_modifiers(i).insert(ModList::Defense, s.stacking_category, v, combat::kPermanent, crc, src.id);
    }
    event(AbilityEventKind::EffectOn, sp.trigger, src.id, kNoObject, frame);  // the presentation 0x48e4d0
}

void AbilitySystem::aura_give(const Obj& src, const SpecialAbilityDesc& s, ObjectId target) {
    // CombatBonusAbilityClass::vf30 (0x6f42d0): one APPEND insert per non-zero field into the target object's set,
    // stacking category = Stacking_Category, source = the ability's owner, permanent. Health: current += new max - old.
    combat::ModifierSet* m = combat_->unit_modifiers(target);
    if (!m) return;
    const float before = combat_->max_health(target);
    bool health = false;
    for (uint32_t l = 0; l < combat::kModListCount; ++l) {
        const float v = s.bonus[l];
        if (v == 0.0f) continue;
        m->insert(static_cast<ModList>(l), s.stacking_category, v, combat::kPermanent, s.name_crc, src.id);
        health = health || l == static_cast<uint32_t>(ModList::MaxHealth);
    }
    if (health) {
        combat_->modifier(target, ModList::MaxHealth);  // refresh the cached reduction
        const float after = combat_->max_health(target);
        combat_->set_health(target, combat_->health(target) + (after - before));
    }
}

void AbilitySystem::aura_on(Obj& src, SpecialState& sp, uint32_t frame) {
    const SpecialAbilityDesc& s = data_->specials[sp.desc];
    if (sp.aura || src.dead) return;
    sp.aura = 1;
    sp.applied = 1;
    const auto np = static_cast<int32_t>(combat_->player_count());
    switch (s.cls) {
        case SpecialClass::CombatBonus:
            // s78n t04 (§5.4.4 item 3b, Create_Object 0x29f810): the new source offers its aura to every existing object
            // but itself, unless its type has Apply_Special_Ability_To_Self_On_Creation (+0xe8).
            for (Obj& t : objs_)  // ascending id
                if ((t.id != src.id || desc(src).apply_self_on_creation) && aura_target(src, sp.desc, t))
                    aura_give(src, s, t.id);
            break;
        case SpecialClass::BattlefieldModifier:
            // 0x6f10b0 (apply): cu8g t05 (CU8F_T04_T06_RE §3.3 [H]): Reverse_Application_Logic swaps apply and remove, for
            // the float lists and the reveal list as for the Enable_* gate lists (cu8f t06).
            modifier_entries(src, sp.desc, s, !s.reverse_logic);
            break;
        case SpecialClass::BasePower:  // 0x6efaf0: every player allied to the source (owner included), keyed by it
            // s78n t07 (TACTICAL_GAPS §7.3): allied = 0x39a2c0, relation 0 (`friendly`); a neutral player gets no power.
            for (int32_t p = 0; p < np; ++p)
                if (friendly(src.owner, p)) combat_->add_base_power(p, src.id);
            break;
        case SpecialClass::Weatherproof:  // 0x711720: false, permanent; all allies with Apply_To_All_Allies [M faction flag]
            // s78n t07 (TACTICAL_GAPS §7.4): "allied to the owner" = relation 0 (`friendly`); [port] the faction +0x20a8
            // flag [M] is not read.
            for (int32_t p = 0; p < np; ++p)
                if (p == src.owner || (s.apply_to_all_allies && friendly(src.owner, p)))
                    source_entries_.push_back(SourceEntry{p, src.id, s.name_crc, 2});
            break;
        case SpecialClass::RetreatPrevention:  // the interdict list of the enemy players [M]
            for (int32_t p = 0; p < np; ++p)
                if (enemy(src.owner, p)) source_entries_.push_back(SourceEntry{p, src.id, s.name_crc, 1});
            break;
        case SpecialClass::EnableAbility:  // 0x6f8fb0 [M]: the named special for the owner / all allies
            // s78n t07 (TACTICAL_GAPS §2 table): [port] "allies" read as relation 0 (`friendly`) like the other per-player
            // writers; 0x6f8fb0's own test is not traced [M].
            for (int32_t p = 0; p < np; ++p)
                if (p == src.owner || (s.apply_to_all_allies && friendly(src.owner, p))) set_special_enabled(p, s.ability_name_crc, true);
            break;
        default: break;
    }
    event(AbilityEventKind::EffectOn, sp.trigger, src.id, kNoObject, frame);
}

void AbilitySystem::aura_off(Obj& src, SpecialState& sp, uint32_t frame) {
    if (!sp.aura) return;
    const SpecialAbilityDesc& s = data_->specials[sp.desc];
    sp.aura = 0;
    sp.applied = 0;
    const auto np = static_cast<int32_t>(combat_->player_count());
    switch (s.cls) {
        case SpecialClass::CombatBonus:  // s78n t04: Enhance_Defense's entries are never removed
            for (Obj& t : objs_) {
                if (!t.combat_unit || !combat_->has_unit(t.id)) continue;
                if (combat::ModifierSet* m = combat_->unit_modifiers(t.id)) {
                    if (m->remove_source(src.id, s.name_crc) == 0) continue;
                    combat_->modifier(t.id, ModList::MaxHealth);
                    const float mh = combat_->max_health(t.id);  // [M] health clamped to the new maximum
                    if (combat_->health(t.id) > mh) combat_->set_health(t.id, mh);
                }
            }
            break;
        case SpecialClass::BattlefieldModifier:
            // 0x6f17d0 (remove); with Reverse_Application_Logic it pushes (cu8g t05, §3.3).
            modifier_entries(src, sp.desc, s, s.reverse_logic);
            break;
        case SpecialClass::BasePower:  // 0x6eff70
            for (int32_t p = 0; p < np; ++p) combat_->remove_base_power(p, src.id);
            break;
        case SpecialClass::Weatherproof:
        case SpecialClass::RetreatPrevention:
            source_entries_.erase(std::remove_if(source_entries_.begin(), source_entries_.end(),
                                                 [&](const SourceEntry& e) { return e.source == src.id && e.crc == s.name_crc; }),
                                  source_entries_.end());
            break;
        case SpecialClass::EnableAbility:
            for (int32_t p = 0; p < np; ++p)
                if (p == src.owner || (s.apply_to_all_allies && friendly(src.owner, p))) set_special_enabled(p, s.ability_name_crc, false);
            break;
        default: break;
    }
    (void)frame;  // remove_special raises Ended
}

void AbilitySystem::auras_unit_added(Obj& n, uint32_t /*frame*/) {
    // Apply-on-unit-add (vf18): every live aura source (ascending id) gives its bonus to the new unit when it matches.
    if (!n.combat_unit) return;
    for (const Obj& src : objs_) {
        if (src.dead || src.id == n.id) continue;
        for (const SpecialState& sp : src.specials) {
            if (!sp.aura) continue;
            const SpecialAbilityDesc& s = data_->specials[sp.desc];
            if (s.cls == SpecialClass::CombatBonus && aura_target(src, sp.desc, n)) aura_give(src, s, n.id);
        }
    }
}

// ================================================================================================
// unit-ability handler bodies (§2.1)
// ================================================================================================

bool AbilitySystem::team_gates(Obj& o, uint32_t t, uint32_t frame) {
    // "Gates: Force block, garrison count, no other mode active, all team members pass Can_Activate, locomotor not
    // busy" - the Can_Activate gates (no recharge test) [port: "no other mode active" not modelled].
    return can(o, t, false, true, frame);
}

void AbilitySystem::h_deploy(Obj& o, bool on, uint32_t frame) {
    // 0x3a1ce0 [H flow, M names]: the unit stops; on -> deploy (DeployedBehavior, host), off -> undeploy. No countdown.
    const bool ok = team_gates(o, kDeploy, frame);
    if (on && !ok) return;
    stop_order(o.id, frame, kStopAdopt);  // cu8j t02: 0x3a1ce0 adopts (return 0x3a1f81, MOVEMENT §4.7.2)
    if (host_.deploy) {
        // S8 t11 (MOVEMENT §3.2, AI_TACTICAL §7.3.3 item 3): 0x391260 calls the Walk locomotor's Deploy(on). The locomotor
        // owns the state (0x18 Deploying, clip, 0x19 Deployed; 0x1A Undeploying, clip, 0); the host reports each change
        // with set_deploy_state (Is_Active, kUnitDeploying, kUnitDeployTransition) and raises signal 0x29 at the clip ends.
        if (!host_.deploy(o.id, on, frame)) return;
    } else {
        // [port] no locomotor host: the deploy is instant.
        if (o.deployed == (on ? 1 : 0)) return;
        o.deployed = on ? 1 : 0;
        // S9 t04: the death-clone deploy remap reads "deploying or deployed" (S9_DEATH_CLONE_SHIELDS.md §1.2) [port: the
        // DEPLOY state stands in for the walk locomotor's deploy-pending flag / Deployed state].
        combat_->set_flag(o.id, combat::kUnitDeploying, on);
    }
    AbilityEvent e;
    e.kind = AbilityEventKind::Deployed;
    e.type = kDeploy;
    e.id = o.id;
    e.value = on ? 1 : 0;
    e.frame = frame;
    events_.push_back(e);
}

void AbilitySystem::set_deploy_state(ObjectId id, uint8_t walk_state) {
    Obj* o = obj(id);
    if (!o) return;
    const bool deployed = walk_state == 0x19;
    o->deployed = deployed ? 1 : 0;
    if (!combat_->has_unit(id)) return;
    combat_->set_flag(id, combat::kUnitDeploying, deployed);
    combat_->set_flag(id, combat::kUnitDeployTransition, walk_state == 0x18 || walk_state == 0x1A);
    // s78n t11 (TACTICAL_COMBAT §5.4.6 item 4.3): 0x18 alone, for the _Deployed range extents (not while undeploying).
    combat_->set_flag(id, combat::kUnitDeployingIn, walk_state == 0x18);
}

void AbilitySystem::h_jet_pack(Obj& o, ObjectId target, uint32_t frame) {
    // 0x3a38d0 (REMAINING_GAPS §5.3 [H flow]): gates (Force / garrison / mode / team Can_Activate); 2-D distance to the
    // point <= the record's Effective_Radius (+0x20) unless it is negative; an ordinary MOVE of the unit or its whole
    // team to the point, executed through 0x2a7b80 (MoveOrder events with type 17: the JETPACK locomotor, MOVEMENT /
    // REMAINING_GAPS §4.3, does the flight). Countdown on the acting record (the member's in a team, record_of):
    // Expiration_Seconds (+0x28) = 0 -> recharge at once (0x42fb70), else the active phase (0x42fde0). Every shipped
    // user has no Expiration, so 17 is never "active" (the locomotor must not use Is_Active(17) as a flight flag). No RNG.
    // The locomotor swap (0x3a9910, MOVEMENT §3.1.1; type +0x74) is the host's, on the MoveOrder event
    // (MovementSystem::use_secondary_locomotor). [port] not modelled: the order record's non-default fields (4, 0x3f,
    // 0x1000001, -999 [L meaning]).
    if (!team_gates(o, kJetPack, frame)) return;
    const UnitAbilityRecord* r = record_of(o, kJetPack);
    if (!r || target == kNoObject || !combat_->has_unit(target)) return;
    const Vec3 p = combat_->position(target);
    if (r->effective_radius >= 0.0f && combat::distance_xy(position(o), p) > r->effective_radius) return;
    const Obj* team = o.is_container ? &o : (o.container != kNoObject ? obj(o.container) : nullptr);
    if (team) {
        for (ObjectId m : team->members)
            if (alive_unit(m)) move_order(m, p, frame, kJetPack);
    } else if (o.combat_unit) {
        move_order(o.id, p, frame, kJetPack);
    }
    if (r->expiration_seconds == 0.0f) {
        start_recharge(o, kJetPack);
    } else {
        start_active(o, kJetPack);
    }
}

void AbilitySystem::h_eject_thief(Obj& o, uint32_t frame) {
    // REMAINING_GAPS §5.1 [H flow, M meaning]: the off path of EJECT_VEHICLE_THIEF (46), shared by CAPTURE_VEHICLE (18) off
    // (inline in 0x38a350; runs before set_active writes the slot byte, so "active" is the state being left).
    // 1. 18 present and active (0x39b950(obj, 18, 1)): its recharge restarts.
    if (has(o, kCaptureVehicle) && active_state(o, kCaptureVehicle)) start_recharge(o, kCaptureVehicle);
    // 2. 46 present and active (team members included): the thief = the FIRST occupant of the vehicle's container list;
    //    none -> nothing happens.
    if (!has(o, kEjectVehicleThief) || !active_state(o, kEjectVehicleThief)) return;
    const ObjectId thief = host_.first_occupant ? host_.first_occupant(o.id) : kNoObject;
    if (thief == kNoObject) return;
    // 3. unless the vehicle is already dying (+0x3a0 bit 0x40): Take_Damage(kind 0xD, 1,000,000, source = thief)
    //    (0x3a9e30) - the eject KILLS the stolen vehicle, credited to the thief. The thief leaves through the vehicle's
    //    death / garrison release (host); the death path makes its usual draws, the eject itself none.
    const ObjectId vid = o.id;
    if (o.combat_unit && alive_unit(vid)) {
        combat::DamageRequest r;
        r.victim = vid;
        r.kind = combat::kKindVehicleThief;  // 0xD
        r.amount = 1000000.0f;
        r.source = thief;
        combat_->take_damage(r, frame);
    }
    // 4. the thief's 18 starts recharging (0x3a9840).
    if (Obj* t = obj(thief)) start_recharge(*t, kCaptureVehicle);
}

void AbilitySystem::h_barrage(Obj& o, bool on, ObjectId target, uint32_t frame) {
    // 0x3a1930 / BarrageAreaBehavior (§2.2) [H].
    if (on) {
        if (o.barrage) return;  // re-target exception: true while running
        if (target == kNoObject || !combat_->has_unit(target)) return;
        Vec3 p = combat_->position(target);
        if (host_.point_fogged && host_.point_fogged(o.owner, p, true)) return;  // refused when fogged (force 1)
        const UnitAbilityRecord* r = record_of(o, kBarrage);
        const auto z = static_cast<float>(r ? r->target_position_z_offset : 0);
        p = Vec3{p.x + z, p.y + z, p.z + z};  // Target_Position_Z_Offset on x, y AND z (quirk)
        int32_t enemy_player = -1;  // the shooter's first enemy player (0x294910)
        for (int32_t q = 0; q < static_cast<int32_t>(combat_->player_count()) && enemy_player < 0; ++q)
            if (enemy(o.owner, q)) enemy_player = q;
        SpawnRequest sr;
        sr.purpose = SpawnPurpose::BarrageTarget;
        sr.position = p;
        sr.owner = enemy_player;
        sr.source = o.id;
        sr.ability_type = kBarrage;
        request_spawn(sr);  // the host builds and executes the Attack command on it (one id)
        o.barrage = 1;
        return;
    }
    if (active_state(o, kBarrage) && !o.cd.recharging(kBarrage)) deactivate_early(o, kBarrage, frame);
    o.barrage = 0;  // stop (0x592420)
}

void AbilitySystem::h_spread_out(Obj& o, bool on, uint32_t frame) {
    // 0x3a3500: on -> start active, off -> early deactivate; every member +0x448 := on (default idle Idle Crouch,
    // §6.2); the formation re-layout is the host's [M spacing].
    if (!team_gates(o, kSpreadOut, frame) && on) return;
    if (on) {
        start_active(o, kSpreadOut);
    } else {
        deactivate_early(o, kSpreadOut, frame);
    }
    o.spread = on ? 1 : 0;
    for (ObjectId m : o.members)
        if (Obj* mo = obj(m)) mo->spread = on ? 1 : 0;
}

void AbilitySystem::h_replenish(Obj& o, uint32_t frame) {
    // 0x3a2970: missing squadron members (vs the type's list +0x978/+0x980) are re-created at the formation slots
    // (one id each) - the host knows the composition and creates them for the Wingman request [port]; recharge 44.
    (void)frame;
    Obj* team = o.is_container ? &o : (o.container != kNoObject ? obj(o.container) : nullptr);
    SpawnRequest r;
    r.purpose = SpawnPurpose::Wingman;
    r.owner = o.owner;
    r.source = o.id;
    r.target = team ? team->id : o.id;
    r.position = position(team ? *team : o);
    r.ability_type = kReplenishWingmen;
    request_spawn(r);
    start_recharge(o, kReplenishWingmen);
}

void AbilitySystem::h_instant(Obj& o, uint32_t t, ObjectId target, uint32_t frame) {
    // 0x3a3e20 (19, 23-26, 28, 35, 36, 42; SELF_DESTRUCT 39): Can_Activate(t, 1) (skipped for 39), Has_Ability; creates
    // Spawned_Object_Type at the point (the unit position without one), unit facing, owner (one id). A projectile gets
    // source := user and the target id; a BOMB is armed with Bomb_Countdown_Seconds. Recharge (not for 39). The
    // gameplay effect is the spawned object's detonation (§1.10).
    if (t != kSelfDestruct && !can(o, t, true, true, frame)) return;
    const UnitAbilityRecord* r = record_of(o, t);
    if (!r) return;
    const bool real_target = target != kNoObject && target != o.id && combat_->has_unit(target) &&
                             !(combat_->flags(target) & combat::kUnitSkipTargeting);
    const Vec3 at = (target != kNoObject && combat_->has_unit(target)) ? combat_->position(target) : position(o);
    if (t == kFowRevealPing || r->spawned_projectile < 0) {
        // fog follow-up path: the host spawns the record's type (FOW ping projectiles reveal through REVEAL) [M].
        if (!r->spawned_object_type.empty())
            event(AbilityEventKind::SpawnObject, static_cast<uint8_t>(t), o.id, target != kNoObject ? target : o.id, frame);
    } else {
        combat::ProjectileLaunch l;
        l.shooter = o.combat_unit ? o.id : kNoObject;
        if (!o.combat_unit)
            for (ObjectId m : o.members)
                if (alive_unit(m)) {
                    l.shooter = m;
                    break;
                }
        l.owner = o.owner;
        l.projectile = static_cast<uint32_t>(r->spawned_projectile);
        l.from = at;
        l.point = at;
        l.target = real_target ? target : kNoObject;
        l.fuse_seconds = r->bomb_countdown_seconds > 0.0f ? r->bomb_countdown_seconds : 0.0f;  // [port] detonates here
        if (l.target != kNoObject) l.fuse_seconds = -1.0f;  // a projectile with a target flies to it
        combat_->launch_projectile(l, frame);
        event(AbilityEventKind::EffectOn, static_cast<uint8_t>(t), o.id, target, frame);
    }
    if (t != kSelfDestruct) start_recharge(o, t);
}

void AbilitySystem::deploy_troopers(Obj& o, const UnitAbilityRecord& r, uint32_t frame) {
    // REMAINING_GAPS §5.2 [H] (state 4 of 0x591a90 -> 0x590730). No draws. cu8f t06: the spawn does not read
    // Max_Num_Spawned_Objects (Can_Activate does); the troopers of one spawn form one squad [port: the state-7 grouping by
    // owner player always gives one group here], numbered per unit.
    (void)frame;
    const int32_t squad = o.squads_formed;
    // Spawned type: its Company_Units, else Squadron_Units; neither (or no model) -> state 10 (failed, nothing spawned).
    member_scratch_.clear();
    if (r.spawned_type < 0) return;
    if (host_.company_members) {
        if (!host_.company_members(r.spawned_type, member_scratch_) || member_scratch_.empty()) return;
    } else {
        member_scratch_.push_back(r.spawned_type);  // [port] host without the query: the type is its own single member
    }
    // N = member count - troopers already deployed (behaviour data +0x40).
    const int32_t n = static_cast<int32_t>(member_scratch_.size()) - o.troopers_deployed;
    const Vec3 facing = o.combat_unit ? combat_->facing(o.id) : Vec3{1.0f, 0.0f, 0.0f};
    int32_t spawned = 0;
    // Owner_Attachment_Bone list by index i: a bone the render model lacks is skipped but still uses member index i; each
    // found bone spawns member type i (one id each) at the bone, owner's facing and player. Stops at N.
    for (size_t i = 0; i < r.owner_attachment_bones.size() && spawned < n; ++i) {
        if (i >= member_scratch_.size()) break;  // [M] more bones than members
        Vec3 p = position(o);
        if (host_.bone_position) {
            const int found = host_.bone_position(o.id, r.owner_attachment_bones[i], p);
            if (found < 0) return;  // no model: state 10
            if (found == 0) continue;
        }
        SpawnRequest sr;
        sr.purpose = SpawnPurpose::Troopers;
        sr.object_type = member_scratch_[i];
        sr.position = p;
        sr.facing = facing;
        sr.owner = o.owner;
        sr.source = o.id;
        sr.ability_type = kDeployTroopers;
        sr.bone = static_cast<int32_t>(i);
        sr.squad = squad;
        request_spawn(sr);
        o.troopers_deployed += 1;  // +0x40 += 1 (the id joins the trooper list +0x10 in bind_spawn)
        ++spawned;
    }
    if (spawned > 0) o.squads_formed += 1;
}

int32_t AbilitySystem::live_squads(const Obj& o) const {
    // cu8f t06 (CU8F_T04_T06_RE §3.2): deploy pack +0x58 = the squads list count. A squad's entry goes when the squad is
    // removed from the game (signal 0x28 [M]): [port] a squad is live while one of its bound troopers is still a combat
    // unit, or while one of its spawn requests is not bound yet. Squad numbers ascend in spawn order, so equal numbers
    // are adjacent.
    int32_t n = 0, last = -1, bound_max = -1;
    for (size_t i = 0; i < o.troopers.size() && i < o.trooper_squads.size(); ++i) {
        const int32_t q = o.trooper_squads[i];
        bound_max = std::max(bound_max, q);
        if (q == last || !combat_->has_unit(o.troopers[i])) continue;
        ++n;
        last = q;
    }
    for (const SpawnRequest& r : spawns_)
        if (r.purpose == SpawnPurpose::Troopers && r.source == o.id && r.squad > bound_max && r.squad != last) {
            ++n;
            last = r.squad;
        }
    return n;
}

int32_t AbilitySystem::trooper_squads(ObjectId id) const {
    const Obj* o = obj(id);
    return o ? live_squads(*o) : 0;
}

// ================================================================================================
// LURE (cu8f t06, CU8F_T04_T06_RE §3.1 [H]; LureBehaviorClass 0x23)
// ================================================================================================

bool AbilitySystem::lure_running(ObjectId id) const {
    const Obj* o = obj(id);
    return o && o->lure_on != 0;
}
int32_t AbilitySystem::lure_pulses(ObjectId id) const {
    const Obj* o = obj(id);
    return o ? o->lure_pulses : 0;
}
uint32_t AbilitySystem::lure_next(ObjectId id) const {
    const Obj* o = obj(id);
    return o ? o->lure_next : 0;
}
ObjectId AbilitySystem::lure_effect(ObjectId id) const {
    const Obj* o = obj(id);
    return o ? o->lure_effect : kNoObject;
}

void AbilitySystem::lure_activate(Obj& o, uint32_t frame) {
    // vf23 0x62b190: no LURE record -> log only. Resume; count := 0, next := NOW (the first pulse at the next service).
    const UnitAbilityRecord* r = record_of(o, kLure);
    if (!r) return;
    o.lure_on = 1;
    o.lure_pulses = 0;
    o.lure_next = frame;
    // Particle_Effect (0x62af80 -> 0x29f810, a normal creation: one object id) when the owner is in no team, or is the
    // first member of its team; the colour fade 0x62b6f0 is presentation.
    bool first = true;
    if (o.container != kNoObject)
        if (const Obj* c = obj(o.container)) first = !c->members.empty() && c->members.front() == o.id;
    if (!first || r->particle_effect.empty() || o.lure_effect != kNoObject) return;
    SpawnRequest sr;
    sr.purpose = SpawnPurpose::LureEffect;
    sr.position = position(o);
    sr.facing = o.combat_unit ? combat_->facing(o.id) : Vec3{1.0f, 0.0f, 0.0f};
    sr.owner = o.owner;
    sr.source = o.id;
    sr.ability_type = kLure;
    request_spawn(sr);
}

void AbilitySystem::lure_deactivate(Obj& o, uint32_t frame) {
    // vf24 0x62b100: kill the effect object, base deactivate (suspend).
    o.lure_on = 0;
    if (o.lure_effect != kNoObject) {
        event(AbilityEventKind::RemoveSilently, kLure, o.lure_effect, o.id, frame);
        o.lure_effect = kNoObject;
    }
    // cu8g t05: an effect requested this tick and not bound yet goes too (bind_spawn refuses it, the host removes it).
    for (SpawnRequest& r : spawns_)
        if (r.purpose == SpawnPurpose::LureEffect && r.source == o.id) r.cancelled = true;
}

void AbilitySystem::lure_service(Obj& o, uint32_t frame) {
    // vf6 0x62b4c0, every frame: nothing while suspended or without the record; wait for the next-pulse frame.
    if (!o.lure_on) return;
    const UnitAbilityRecord* r = record_of(o, kLure);
    if (!r || frame < o.lure_next) return;
    if (o.lure_pulses < r->max_pulses) {
        o.lure_pulses += 1;
        const auto step = static_cast<int32_t>(static_cast<float>(cfg_.fps) * r->pulse_frequency_secs);  // truncation
        o.lure_next = frame + static_cast<uint32_t>(std::max(0, step));
        lure_pulse(o, *r, frame);
    } else {
        // Suspend (vfunc +0xc0). The ability's countdown is untouched: the slot stays active until Expiration_Seconds [M].
        o.lure_on = 0;
    }
}

void AbilitySystem::lure_pulse(Obj& o, const UnitAbilityRecord& r, uint32_t frame) {
    // Centre = the owner's position, r = Effective_Radius. Per Target_Types entry (XML order), every object whose type
    // equals it exactly (no variant / base matching), in the object list's order ([port] ascending id over the combat
    // units and the team containers), within the 3-D radius (double compare); a TEAM member is replaced by its container;
    // enemies of the owner's player are lured.
    const Vec3 c = position(o);
    const double r2 = static_cast<double>(r.effective_radius) * static_cast<double>(r.effective_radius);
    const auto in_radius = [&](Vec3 p) {
        const double dx = static_cast<double>(p.x) - static_cast<double>(c.x);
        const double dy = static_cast<double>(p.y) - static_cast<double>(c.y);
        const double dz = static_cast<double>(p.z) - static_cast<double>(c.z);
        return dx * dx + dy * dy + dz * dz <= r2;
    };
    const ObjectId self = o.id;
    for (const int32_t tt : r.target_types) {
        if (tt < 0) continue;
        size_t ui = 0, ci = 0;
        const size_t nu = combat_->unit_count();
        for (;;) {
            // next object in ascending id: a combat unit or a team container
            while (ci < objs_.size() && !objs_[ci].is_container) ++ci;
            const bool have_u = ui < nu, have_c = ci < objs_.size();
            if (!have_u && !have_c) break;
            ObjectId id = kNoObject;
            int32_t type = -1;
            Vec3 p;
            bool live = false;
            if (have_u && (!have_c || combat_->unit_id_at(ui) < objs_[ci].id)) {
                id = combat_->unit_id_at(ui++);
                live = alive_unit(id);
                if (live) {
                    type = combat_->data().units[combat_->desc_index(id)].object_type;
                    p = combat_->position(id);
                }
            } else {
                const Obj& t = objs_[ci++];
                id = t.id;
                live = !t.dead;
                if (live) {
                    type = desc(t).object_type;
                    p = position(t);
                }
            }
            if (!live || type != tt || id == self || !in_radius(p)) continue;
            ObjectId v = id;
            if (const ObjectId cont = container_of(id); cont != kNoObject && obj(cont)) v = cont;  // parent with TEAM
            const Obj* vo = obj(v);
            const int32_t vowner = vo && vo->is_container ? vo->owner : combat_->owner(v);
            Obj* me = obj(self);
            if (!me || !enemy(me->owner, vowner)) continue;
            lure_victim(*me, v, frame);
        }
    }
}

void AbilitySystem::lure_victim(Obj& o, ObjectId v, uint32_t frame) {
    // 0x62b270(behaviour, V, owner): V not inside a TEAM parent, not on a flagged path ([port] the host's movement lock
    // query), and V's current attack target is not the owner. Then Order_Attack(V, owner, ordered 1) (0x38d730); when it
    // takes, V (a team: each live member that is not movement-locked) joins a new coordinator moving toward the owner.
    const Obj* vo = obj(v);
    std::array<ObjectId, 64> units{};
    size_t n = 0;
    if (vo && vo->is_container) {
        for (ObjectId m : vo->members)
            if (alive_unit(m) && n < units.size()) units[n++] = m;
    } else {
        if (vo && vo->container != kNoObject && obj(vo->container)) return;  // inside a team (its container is not lured)
        if (alive_unit(v)) units[n++] = v;
    }
    if (n == 0) return;
    const ObjectId lead = units[0];  // [port] a team's path / target = its first live member's
    if (host_.movement_locked && host_.movement_locked(lead)) return;
    if (combat_->current_target(lead) == o.id) return;
    bool ok = false;
    for (size_t i = 0; i < n; ++i) ok = combat_->order_attack_issue(units[i], o.id) || ok;
    if (!ok) return;
    const ObjectId goal = o.container != kNoObject && obj(o.container) ? o.container : o.id;  // the owner, or its parent
    const Vec3 at = position(o);
    for (size_t i = 0; i < n; ++i) {
        const ObjectId u = units[i];
        if (vo && vo->is_container) {
            const Obj* mo = obj(u);
            if (mo ? locked(*mo) : (host_.movement_locked && host_.movement_locked(u))) continue;
        }
        AbilityEvent e;
        e.kind = AbilityEventKind::Lured;
        e.type = kLure;
        e.id = u;
        e.target = goal;
        e.point = at;
        e.frame = frame;
        events_.push_back(e);
    }
}

void AbilitySystem::h_behavior(Obj& o, uint32_t t, bool on, uint32_t frame) {
    // Toggle_Behavior_Ability 0x3a4070 (§6.6): on -> Enable (starting the countdown when the counter reaches 0), off ->
    // Disable, then recharge. HUNT / LURE are M = 1: Is_Active is the slot byte the caller writes afterwards.
    const UnitAbilityRecord* r = record_of(o, t);
    // cu8l t02 (CU8K_T02_T06_RE §2 rows 0x3a4138 / 0x3a41e6, rule 5): every toggle but HUNT on and SELF_DESTRUCT stops and
    // holds the unit (0x3a9880: one draw when it is not a squad member) before the behaviour's activate / deactivate; turning
    // a running behaviour off (0x4c3700; not HUNT) holds it a second time. [M] after the port's already-on / already-off
    // returns; [port] DEPLOY_TROOPERS is never running here (its state machine finishes in the toggle).
    const bool hold = t != kSelfDestruct && !(t == kHunt && on);
    if (on) {
        if (o.behavior[t]) return;
        if (hold) stop_order(o.id, frame, kStopHold);
        o.behavior[t] = 1;
        if (r && r->expiration_seconds > 0.0f) start_active(o, t);
        if (t == kHunt) o.hunt_due = frame;  // a re-enabled HUNT is due at once [M: its attach due frame is past]
        if (t == kLure) lure_activate(o, frame);  // cu8f t06
        if (t == kSelfDestruct) {
            o.self_destruct = 1;
            if (r && r->stop_when_activated) {  // vf23 0x630950: stop + lock only with Stop_When_Activated (§28.9)
                stop_order(o.id, frame, kStopHold);  // cu8l t02: a stop-and-hold (0x630b78, CU8K_T02_T06_RE §2)
                lock_pair(o, kLockSelfDestruct, true, frame);
            }
        }
        // [port] DEPLOY_TROOPERS' stop + lock (0x58f6a0) and its release (0x58f600) fall in this same call: no net lock.
        if (t == kDeployTroopers && r) {
            // DEPLOY_TROOPERS state machine 0x591a90: [port] the owner's anim 0x5e and the release state 5 (LANDING 0x1e)
            // are not timed here - state 4 (spawn, 0x590730) runs at once, then the behaviour is done and recharges.
            // cu8l t02 (CU8K_T02_T06_RE §2 row 0x58f7b0): the activate 0x58f6a0 stops and holds the unit at the deploy's
            // start (fewer than the maximum out: Can_Activate passed): a second draw after the toggle's. Its lock then
            // finds the unit in the adopt's coordinator (no draw) and is released in this same call.
            stop_order(o.id, frame, kStopHold);
            deploy_troopers(o, *r, frame);
            o.behavior[t] = 0;
            start_recharge(o, t);
        }
    } else {
        if (!o.behavior[t]) return;
        if (hold) {
            stop_order(o.id, frame, kStopHold);
            if (t == kLure && o.lure_on) stop_order(o.id, frame, kStopHold);  // the running behaviour: 0x3a41e6
        }
        o.behavior[t] = 0;
        if (t == kSelfDestruct) {
            o.self_destruct = 0;
            lock_pair(o, kLockSelfDestruct, false, frame);  // vf24 0x630840
        }
        if (t == kLure) lure_deactivate(o, frame);  // cu8f t06
        start_recharge(o, t);
    }
}

// ================================================================================================
// behaviours (§2.2)
// ================================================================================================

void AbilitySystem::behaviors_service(Obj& o, uint32_t frame) {
    if (o.dead) return;
    // CONFUSE expiry 0x636960: clear the flag, unlock, suspend.
    if (o.confused && frame >= o.confuse_end) {
        o.confused = 0;
        lock_pair(o, kLockConfuse, false, frame);
    }
    // SELF_DESTRUCT (0x43, interval 1): once 39's countdown is no longer in a running active phase (0x42f7a0), suspend,
    // run 0x3a3e20(obj, 39) and Die(obj, kind 1, killer = self).
    if (o.self_destruct) {
        const bool running = o.cd.phase[kSelfDestruct] == 1 && o.cd.counter[kSelfDestruct] < o.cd.maximum[kSelfDestruct];
        if (!running) {
            o.self_destruct = 0;
            o.behavior[kSelfDestruct] = 0;
            const ObjectId id = o.id;
            h_instant(o, kSelfDestruct, kNoObject, frame);
            if (Obj* self = obj(id); self && self->combat_unit) combat_->kill_unit(id, id, combat::kKindNormal, frame);
            return;
        }
    }
    lure_service(o, frame);  // cu8f t06: LURE (0x23), interval 1
    if (o.dead) return;
    // HUNT (0x21): interval 30, or 1 while burning (0x621840).
    if (o.behavior[kHunt] && frame >= o.hunt_due) {
        const bool burning = o.combat_unit && combat_->burning(o.id);
        o.hunt_due = frame + (burning ? 1u : 30u);
        // team members other than the leader do nothing (the leader orders the team) [port: leader = first live member]
        if (o.container != kNoObject) {
            if (const Obj* c = obj(o.container)) {
                ObjectId leader = kNoObject;
                for (ObjectId m : c->members)
                    if (alive_unit(m)) {
                        leader = m;
                        break;
                    }
                if (leader != o.id) return;
            }
        }
        // decides only when idle: not moving, no attack target (0x397a40) [port: idle animation / formation tests]
        if (moving(o)) return;
        if (o.combat_unit && combat_->current_target(o.id) != kNoObject) return;
        hunt_decide(o, frame);
    }
}

void AbilitySystem::hunt_decide(Obj& o, uint32_t frame) {
    // Destination 0x627d40 (§2.2) [H flow, M bounds]. Type flag +0x15 is taken as clear for every type [port: unknown].
    Vec3 lo, hi;
    if (!map_rect(lo, hi)) return;  // zero rectangle = stay
    const Vec3 me = position(o);
    Vec3 dest = me;
    bool found = false;
    // 1. u > 0.5: 99 random points (2 Get_Float each), keep the fogged ones (FOW_Point_Test force 0), Get_Int pick.
    const float u = draw_float(0.0f, 1.0f);
    if (u > 0.5f) {
        std::array<Vec3, 99> pts{};
        int32_t n = 0;
        for (int i = 0; i < 99; ++i) {
            const float x = draw_float(lo.x, hi.x);
            const float y = draw_float(lo.y, hi.y);
            const Vec3 p{x, y, me.z};
            if (host_.point_fogged && host_.point_fogged(o.owner, p, false)) pts[static_cast<size_t>(n++)] = p;
        }
        if (n > 0) {
            dest = pts[static_cast<size_t>(draw_int(0, n - 1))];
            found = true;
        }
    }
    if (!found) {
        // 2. all objects (0x2a9ff0 mode 1): those hidden from the owner (FOW_Object_Hidden force 0); pick hidden, else any.
        scratch_.clear();
        for (size_t i = 0; i < combat_->unit_count(); ++i) {
            const ObjectId id = combat_->unit_id_at(i);
            if (id == o.id || !alive_unit(id)) continue;
            if (host_.object_hidden && host_.object_hidden(o.owner, id, false)) scratch_.push_back(id);
        }
        if (scratch_.empty())
            for (size_t i = 0; i < combat_->unit_count(); ++i) {
                const ObjectId id = combat_->unit_id_at(i);
                if (id != o.id && alive_unit(id) && !(combat_->flags(id) & combat::kUnitSkipTargeting)) scratch_.push_back(id);
            }
        if (!scratch_.empty()) {
            dest = combat_->position(scratch_[static_cast<size_t>(draw_int(0, static_cast<int32_t>(scratch_.size()) - 1))]);
            found = true;
        }
    }
    if (!found) {
        // 3. no objects: one random point; used when its distance is in [500, 1000], else moved along that direction by a
        //    further Get_Float (bounds lost: [500, 1000] [M]); clamped into the rectangle [port: not shrunk by the reveal range].
        const float x = draw_float(lo.x, hi.x), y = draw_float(lo.y, hi.y);
        const float dx = x - me.x, dy = y - me.y, d = dmath::sqrt(dx * dx + dy * dy);
        dest = Vec3{x, y, me.z};
        if (!(d >= 500.0f && d <= 1000.0f)) {
            const float k = draw_float(500.0f, 1000.0f);
            if (d > 0.0f) dest = Vec3{me.x + dx / d * k, me.y + dy / d * k, me.z};
        }
        dest.x = std::clamp(dest.x, lo.x, hi.x);
        dest.y = std::clamp(dest.y, lo.y, hi.y);
    }
    // Jitter (type +0x15 clear): 2 more Get_Float(30, 400) on x and y [M].
    dest.x = dest.x + draw_float(30.0f, 400.0f);
    dest.y = dest.y + draw_float(30.0f, 400.0f);
    // Order: a move for the unit or its whole team (0x628820 -> 0x2a7b80).
    if (o.container != kNoObject) {
        if (const Obj* c = obj(o.container)) {
            for (ObjectId m : c->members)
                if (alive_unit(m)) move_order(m, dest, frame);
            return;
        }
    }
    move_order(o.id, dest, frame);
}

void AbilitySystem::confuse(Obj& v, float secs, uint32_t frame) {
    // CONFUSE (0x53) [H]: skipped with obj +0x3a2 bit 0x80; next service = now + int(FPS x time); 2 synced Get_Float
    // pick a point in the map rectangle (centre +- half extents); a move there (the whole team for a member, the leader
    // acts); lock (0x3a8e50) and the flag.
    if (v.confused || v.dead) return;
    Vec3 lo, hi;
    if (!map_rect(lo, hi)) {
        lo = hi = position(v);
    }
    const float cx = 0.5f * (lo.x + hi.x), cy = 0.5f * (lo.y + hi.y), hx = 0.5f * (hi.x - lo.x), hy = 0.5f * (hi.y - lo.y);
    const float x = draw_float(cx - hx, cx + hx);
    const float y = draw_float(cy - hy, cy + hy);
    const Vec3 p{x, y, position(v).z};
    v.confused = 1;
    v.confuse_end = frame + static_cast<uint32_t>(static_cast<int32_t>(static_cast<float>(cfg_.fps) * secs));
    // The move first, then the lock of the unit and its TEAM container (A1_READINGS §28.9): the locked unit keeps this
    // move; new orders are refused until the expiry releases it.
    if (v.container != kNoObject) {
        if (Obj* c = obj(v.container)) {
            c->confused = 1;
            c->confuse_end = v.confuse_end;
            for (ObjectId m : c->members)
                if (alive_unit(m)) move_order(m, p, frame);
            lock_pair(v, kLockConfuse, true, frame);
            lock_pair(*c, kLockConfuse, true, frame);
            return;
        }
    }
    move_order(v.id, p, frame);
    lock_pair(v, kLockConfuse, true, frame);
}

// ================================================================================================
// proximity mines (§2.3.3)
// ================================================================================================

void AbilitySystem::mines_update(uint32_t frame) {
    // ProximityMineBehaviorClass 0x6373b0 (interval 1): after the arm frame, on frames = 0 (mod 5), the FIRST live mobile
    // non-projectile enemy (ascending id) within 3-D <= Trigger_Radius makes the mine die and detonate via its death
    // clone / death projectile [M].
    for (Mine& m : mines_) {
        if (m.dead) continue;
        if (m.id != kNoObject && combat_->has_unit(m.id) && !combat_->alive(m.id)) {
            m.dead = 1;  // destroyed otherwise
            continue;
        }
        if (frame < m.arm || frame % 5u != 0u) continue;
        if (m.id != kNoObject && combat_->has_unit(m.id)) m.pos = combat_->position(m.id);
        for (size_t i = 0; i < combat_->unit_count(); ++i) {
            const ObjectId u = combat_->unit_id_at(i);
            if (u == m.id || !alive_unit(u) || !enemy(m.owner, combat_->owner(u))) continue;
            const combat::UnitDesc& ud = combat_->data().units[combat_->desc_index(u)];
            if (!ud.has_locomotor || (combat_->flags(u) & combat::kUnitSkipTargeting)) continue;
            if (combat::distance3(combat_->position(u), m.pos) > m.trigger) continue;
            m.dead = 1;
            AbilityEvent e;
            e.kind = AbilityEventKind::MineTriggered;
            e.type = kProximityMines;
            e.id = m.id;
            e.target = u;
            e.point = m.pos;
            e.frame = frame;
            events_.push_back(e);
            if (m.id != kNoObject && combat_->has_unit(m.id)) combat_->kill_unit(m.id, m.id, combat::kKindNormal, frame);
            break;
        }
    }
    mines_.erase(std::remove_if(mines_.begin(), mines_.end(), [](const Mine& m) { return m.dead != 0; }), mines_.end());
}

// ================================================================================================
// combat effect hook
// ================================================================================================

void AbilitySystem::unit_effect(ObjectId id, combat::UnitEffect effect, int32_t player, Vec3 point, uint32_t frame) {
    Obj* o = obj(id);
    if (!o || o->dead) return;
    using E = combat::UnitEffect;
    switch (effect) {
        case E::Stunned: {
            // STUNNABLE apply 0x647290 (REMAINING_GAPS §5.4 [H]): when obj +0x3a0 bit 0x10 is clear, set it and suspend
            // behaviours 1, 9, 0xB, 0x35 (combat: targeting / weapons idle while stunned(); the host freezes the
            // locomotor); STUNNABLE resumed with its next service := now + frames (combat stun_end, a plain assignment);
            // stealth breaks (0.3 s); Stunned_Effect attached (host / render).
            if (!o->weapons_disabled) o->weapons_disabled = 1;
            if (o->stealthed) break_stealth(*o, frame);
            o->stunned = 1;
            AbilityEvent e;
            e.kind = AbilityEventKind::Stunned;
            e.id = id;
            e.value = 1;
            e.frame = frame;
            events_.push_back(e);
            // OI t02 [port] (ABILITIES_HARDPOINTS §4.12, no RE pass): Destroy_When_Stunned_Over_Water - a unit whose stun
            // starts over water (the host's water plane above the terrain under it) loses its hover and is destroyed at
            // once: Die with kind 1, no killer. A stunned unit cannot move, so the test at the stun start is the only one.
            if (o->combat_unit && desc(*o).destroy_when_stunned_over_water && host_.over_water &&
                host_.over_water(combat_->position(id)))
                combat_->kill_unit(id, kNoObject, combat::kKindNormal, frame);
            break;
        }
        case E::StunEnded: {
            // STUNNABLE service vf6 0x647200 (§5.4 [H]): the first service at or after the end frame - when bit 0x10 is
            // set, clear it and resume the four behaviours (table order), then STUNNABLE suspends. The stun effect is
            // not removed here [M: its own lifetime].
            if (o->weapons_disabled) o->weapons_disabled = 0;
            o->stunned = 0;
            AbilityEvent e;
            e.kind = AbilityEventKind::Stunned;
            e.id = id;
            e.value = 0;
            e.frame = frame;
            events_.push_back(e);
            break;
        }
        case E::Distracted: move_order(id, point, frame); break;  // [M] order toward the detonation (0x2a7b80)
        case E::Converted: {
            o->owner = player;  // combat already changed the unit's owner
            AbilityEvent e;
            e.kind = AbilityEventKind::OwnerChanged;
            e.id = id;
            e.value = player;
            e.frame = frame;
            events_.push_back(e);
            break;
        }
        default: break;
    }
}

}  // namespace alamo::tactical::abilities
