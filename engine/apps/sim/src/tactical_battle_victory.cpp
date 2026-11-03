// --- story t06: the tactical victory monitor (docs/re/BATTLE_TRANSITIONS.md §2.5 / §3.1a, SKIRMISH_SETUP.md §4,
// TSW_PLANET_DESTROY.md §2 / §6; tactical_battle.hpp "story t06"). Serial code, no synced draws.
#include <algorithm>
#include <utility>

#include "alamo/objects/behaviors.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/sim/tactical_battle.hpp"

namespace alamo::sim {

namespace {

constexpr int32_t kRecordCountdown = 210;  // 0xd2: 7 s at 30 frames (§2.5, SKIRMISH_SETUP §2 step 4)
constexpr int32_t kStoryShortCountdown = 30;  // VICTORY with Reward_Param2 != 0: 1 s
constexpr int32_t kTswStoryCountdown = 30;    // TSW_PLANET_DESTROY §6 item 5 (b)
constexpr uint8_t kSkirmishLandBase = 0x13;   // SKIRMISH_ALL_ENEMY_BASE_COMPONENTS_DESTROYED
constexpr uint8_t kSkirmishStarbase = 0x16;   // SKIRMISH_SPACE_ENEMY_STARBASE_DESTROYED

}  // namespace

void TacticalBattle::vm_setup(TacticalMode& mode) {
    vm_mode_ = &mode;
    vm_records_.clear();
    if (skirmish_map()) {
        // SKIRMISH_SETUP §2 step 4: one record per player with the option's win condition [port: the defaults, land 0x13 /
        // space 0x16 (MP_Default_Land / Space_Tactical_Win_Condition); only the two slot players].
        for (int32_t s = 0; s < 2; ++s)
            if (const int32_t p = skirmish_slot_player(s); p >= 0) vm_add_record(space_ ? kSkirmishStarbase : kSkirmishLandBase, p, kRecordCountdown, false);
    } else {
        // §2.5 (0x28fda0): 0xb and 0xc for every player that is not Is_Neutral [port: the attacker and the defender, the only
        // players they are ever tested for], then the base / station types for the attacker.
        int32_t ids[2] = {req_.info.defender, req_.info.attacker};
        if (ids[0] > ids[1]) std::swap(ids[0], ids[1]);
        for (int32_t i = 0; i < 2; ++i) {
            if (ids[i] < 0 || (i == 1 && ids[1] == ids[0]) || bt_owner_neutral(ids[i])) continue;
            vm_add_record(galactic::kVictoryAllDestroyed, ids[i], kRecordCountdown, false);
            vm_add_record(galactic::kVictoryAllRetreated, ids[i], kRecordCountdown, false);
        }
        if (req_.info.attacker >= 0 && !space_ && req_.land_base_destruction_forces_retreat)
            vm_add_record(galactic::kVictoryEnemyBaseDestroyed, req_.info.attacker, kRecordCountdown, false);
        if (req_.info.attacker >= 0 && space_ && req_.space_station_destruction_forces_retreat)
            vm_add_record(galactic::kVictoryStarbaseDestroyed, req_.info.attacker, kRecordCountdown, false);
    }
    vm_scan(mode, vm_tracked_);  // the objects at the start: nothing has left a player yet
    vm_tsw_seen_ = l1_sw_.victory_holds();
}

bool TacticalBattle::vm_base_object(const objects::ObjectStore& st, objects::ObjectHandle h) const {
    static const objects::BehaviorTypeId kLandBase = objects::behavior_type_id("DUMMY_LAND_BASE_LEVEL_COMPONENT");
    static const objects::BehaviorTypeId kStarBase = objects::behavior_type_id("DUMMY_STAR_BASE");
    const objects::BehaviorTypeId want = space_ ? kStarBase : kLandBase;
    return want != objects::kNoBehavior && st.has_behavior(h, want);
}

void TacticalBattle::vm_scan(const TacticalMode& mode, std::vector<VmTracked>& out) {
    // Every active (not dead, not a dying death clone) owned object of a victory-relevant type (0x372230, its own type),
    // retreated battle units excluded (they have left the battle; [port] their store objects stay until the end).
    out.clear();
    const objects::ObjectStore& st = mode.primary();
    for (const objects::ObjectHandle h : st.all_objects()) {
        if (!st.active(h) || st.owner(h) < 0 || !victory_relevant(static_cast<uint32_t>(st.type(h)))) continue;
        const objects::ObjectId id = st.id(h);
        if (const Unit* u = unit_by_id(id); u && u->retreated) continue;
        out.push_back({id, st.owner(h), skirmish_map() && vm_base_object(st, h)});
    }
    std::sort(out.begin(), out.end(), [](const VmTracked& a, const VmTracked& b) { return a.id < b.id; });
}

void TacticalBattle::vm_add_record(uint8_t type, int32_t player, int32_t countdown, bool silent) {
    for (VmRecord& r : vm_records_)
        if (r.type == type && r.player == player) {  // [M] an existing record is refreshed, not duplicated
            r.countdown = countdown;
            r.silent = silent;
            return;
        }
    vm_records_.push_back({type, player, countdown, silent});
}

bool TacticalBattle::vm_blocker(int32_t player, bool base_only) {
    // §3.1a item 1: an object blocks `player`'s victory when its type is victory-relevant in this mode, it is not dying /
    // removed and its owner is an enemy of `player`. Skirmish base types (base_only) count only the slot players' objects
    // (SKIRMISH_SETUP §4: Neutral / Hostile objects never block).
    if (!vm_mode_ || !combat_) return false;
    const objects::ObjectStore& st = vm_mode_->primary();
    const int32_t slots[2] = {skirmish_slot_player(0), skirmish_slot_player(1)};
    for (const objects::ObjectHandle h : st.all_objects()) {
        if (!st.active(h)) continue;
        const int32_t o = st.owner(h);
        if (o < 0 || o == player || !combat_->is_enemy(player, o) || !victory_relevant(static_cast<uint32_t>(st.type(h)))) continue;
        if (const Unit* u = unit_by_id(st.id(h)); u && u->retreated) continue;
        if (base_only && (!vm_base_object(st, h) || (o != slots[0] && o != slots[1]))) continue;
        return true;
    }
    return false;
}

bool TacticalBattle::vm_evaluate(uint8_t type, int32_t player) {
    const bool side = player == req_.info.attacker || player == req_.info.defender;
    switch (type) {
        case galactic::kVictoryAllDestroyed:
            return side && !vm_blocker(player, false);
        case galactic::kVictoryAllRetreated: {
            // §3.1a item 1: the retreat coordinator finished with a retreating player R != E that is an enemy of E.
            if (!side || !retreat_complete_ || retreat_side_ < 0) return false;
            const int32_t r = side_player(static_cast<uint8_t>(retreat_side_));
            return r >= 0 && r != player && combat_ && combat_->is_enemy(player, r);
        }
        case galactic::kVictoryEnemyBaseDestroyed: {
            // [port] the attacker wins once every defender ground structure died (the old base rule; 0xd not modelled).
            if (space_ || player != req_.info.attacker) return false;
            uint32_t base_parts = 0, base_alive = 0;
            for (const Participant& p : parts_)
                if (p.structure && p.side == 0) {
                    ++base_parts;
                    if (!p.deployed || p.alive > 0) ++base_alive;
                }
            return base_parts > 0 && base_alive == 0;
        }
        case galactic::kVictoryStarbaseDestroyed: {
            if (!space_ || player != req_.info.attacker) return false;
            for (const Participant& p : parts_)
                if (p.starbase && p.deployed && p.alive == 0) return true;
            return false;
        }
        case galactic::kVictoryStory:
            return true;  // §3.1a item 3: type 0x10 always succeeds when tested
        case kSkirmishLandBase:
        case kSkirmishStarbase:
            return !vm_blocker(player, true);
        default:
            return false;
    }
}

bool TacticalBattle::vm_test(uint8_t type, int32_t player) {
    // 0x344710 gates (§3.1a item 2): held -> queued (no duplicates), "no"; declared -> "yes" only for the declared pair; no
    // record -> "no". [port] the global byte 0x140b303c1 and the galactic-only gate are not modelled.
    if (vm_hold_ > 0) {
        const std::pair<uint8_t, int32_t> q{type, player};
        if (std::find(vm_queue_.begin(), vm_queue_.end(), q) == vm_queue_.end()) vm_queue_.push_back(q);
        return false;
    }
    if (vm_declared_) return vm_decl_.type == type && vm_decl_.player == player;
    const auto rec = std::find_if(vm_records_.begin(), vm_records_.end(),
                                  [type, player](const VmRecord& r) { return r.type == type && r.player == player; });
    if (rec == vm_records_.end() || !vm_evaluate(type, player)) return false;
    // Declare (0x343b90): the record is copied, the countdown starts. [port] the win / lose text and the story notification
    // at the declaration (0x32f3e0) are the host's (STORY_VICTORY is dispatched at the battle end).
    vm_declared_ = true;
    vm_decl_ = *rec;
    vm_countdown_ = std::max(rec->countdown, static_cast<int32_t>(cfg_.end_hold_frames));
    vm_declared_frame_ = frame_;
    l1_tsw_story_winner_ = -1;
    return true;
}

void TacticalBattle::vm_object_lost(const VmTracked& t) {
    // 0x2c0340(gom, object, old owner P): for every enemy E of P - campaign: the attacker / defender, test 0xb then 0xe (land)
    // / 0xf (space); skirmish map: the slot players, 0x13 / 0x16 when the object is a base component.
    if (!combat_) return;
    const int32_t p = t.owner;
    if (skirmish_map()) {
        if (!t.base) return;
        for (int32_t s = 0; s < 2; ++s)
            if (const int32_t e = skirmish_slot_player(s); e >= 0 && e != p && combat_->is_enemy(p, e))
                vm_test(space_ ? kSkirmishStarbase : kSkirmishLandBase, e);
        return;
    }
    int32_t ids[2] = {req_.info.defender, req_.info.attacker};
    if (ids[0] > ids[1]) std::swap(ids[0], ids[1]);
    for (int32_t i = 0; i < 2; ++i) {
        const int32_t e = ids[i];
        if (e < 0 || (i == 1 && e == ids[0]) || e == p || !combat_->is_enemy(p, e)) continue;
        if (vm_test(galactic::kVictoryAllDestroyed, e)) continue;
        vm_test(space_ ? galactic::kVictoryStarbaseDestroyed : galactic::kVictoryEnemyBaseDestroyed, e);
    }
}

void TacticalBattle::vm_retreat_complete(int32_t retreated) {
    // 0x340750: every hold level released without replay (the queue is kept), then 0xc for every enemy of R that is the
    // attacker or defender.
    vm_sync_tsw();
    vm_hold_ = 0;
    if (!combat_ || retreated < 0) return;
    int32_t ids[2] = {req_.info.defender, req_.info.attacker};
    if (ids[0] > ids[1]) std::swap(ids[0], ids[1]);
    for (int32_t i = 0; i < 2; ++i) {
        const int32_t e = ids[i];
        if (e < 0 || (i == 1 && e == ids[0]) || e == retreated || !combat_->is_enemy(retreated, e)) continue;
        vm_test(galactic::kVictoryAllRetreated, e);
    }
}

void TacticalBattle::vm_raise() { vm_hold_ = std::max(0, vm_hold_ + 1); }  // 0x341fe0

void TacticalBattle::vm_release(bool replay) {
    // 0x3440a0: one level; at 0 with replay the deferred tests run in queue order until one declares, then the queue is freed.
    --vm_hold_;
    if (vm_hold_ != 0 || !replay) return;
    const std::vector<std::pair<uint8_t, int32_t>> queue = std::move(vm_queue_);
    vm_queue_.clear();
    for (const auto& [type, player] : queue)
        if (vm_test(type, player)) break;
}

void TacticalBattle::vm_sync_tsw() {
    // TSW_PLANET_DESTROY §2: Fire raises the hold, the state-13 service releases one level with replay. The special-weapon
    // system counts the TSWs in states 5..13; each change of that count is one raise / release here.
    const uint32_t n = l1_sw_.victory_holds();
    for (; vm_tsw_seen_ < n; ++vm_tsw_seen_) vm_raise();
    for (; vm_tsw_seen_ > n; --vm_tsw_seen_) vm_release(true);
}

int32_t TacticalBattle::victory_hold() const {
    int32_t h = vm_hold_;
    const uint32_t n = l1_sw_.victory_holds();
    for (uint32_t s = vm_tsw_seen_; s < n; ++s) h = std::max(0, h + 1);
    for (uint32_t s = vm_tsw_seen_; s > n; --s) --h;
    return h;
}

void TacticalBattle::set_tsw_story_victory(int32_t player) {
    // TSW_PLANET_DESTROY §6 item 5 (b) / (c): {0x10, TSW owner, 30} on the battle's monitor, then 0x28f570 tests it (held
    // until the state-13 release, which replays it).
    if (player < 0 || finished_) return;
    l1_tsw_story_winner_ = player;
    vm_add_record(galactic::kVictoryStory, player, kTswStoryCountdown, false);
    vm_sync_tsw();
    vm_test(galactic::kVictoryStory, player);
}

void TacticalBattle::reward_enable_victory(bool has_param, int32_t value) {
    // Reward switch 0x456970 enum 0x1d (§3.1a item 3): atoi(P1) == 0 holds; != 0 releases one level (replay) only while held.
    if (!has_param || finished_) return;
    vm_sync_tsw();
    if (value == 0) vm_raise();
    else if (vm_hold_ > 0) vm_release(true);
}

bool TacticalBattle::reward_victory(int32_t player, bool short_silent) {
    // VICTORY reward 0x462b70: release every level without replay, add {0x10, W, 7 s (1 s silent)} and test it at once.
    if (player < 0 || finished_) return false;
    vm_sync_tsw();
    vm_hold_ = 0;
    vm_add_record(galactic::kVictoryStory, player, short_silent ? kStoryShortCountdown : kRecordCountdown, short_silent);
    return vm_test(galactic::kVictoryStory, player);
}

void TacticalBattle::check_victory(TacticalMode& mode) {
    if (finished_) return;
    vm_sync_tsw();
    // 0x344670: the countdown runs one frame per battle tick after the declaration's tick (stops at 0 [M]).
    if (vm_declared_ && frame_ > vm_declared_frame_ && vm_countdown_ > 0) --vm_countdown_;
    // Victory-relevant objects that left their player since the last scan (0x2c0340 trigger points: removal / death / owner
    // change / retreat). [port] this tick's combat deaths first in death order, then the rest by ascending id.
    std::vector<VmTracked> now;
    vm_scan(mode, now);
    std::vector<VmTracked> lost;
    for (const VmTracked& t : vm_tracked_) {
        const auto it = std::lower_bound(now.begin(), now.end(), t.id, [](const VmTracked& a, objects::ObjectId v) { return a.id < v; });
        if (it == now.end() || it->id != t.id || it->owner != t.owner) lost.push_back(t);
    }
    vm_tracked_ = std::move(now);
    std::vector<VmTracked> ordered;
    ordered.reserve(lost.size());
    for (const objects::ObjectId id : vm_deaths_) {
        const auto it = std::find_if(lost.begin(), lost.end(), [id](const VmTracked& t) { return t.id == id; });
        if (it == lost.end()) continue;
        ordered.push_back(*it);
        lost.erase(it);
    }
    ordered.insert(ordered.end(), lost.begin(), lost.end());
    vm_deaths_.clear();
    for (const VmTracked& t : ordered) vm_object_lost(t);
    // §3.1 / 0x342d80: the end once declared, the countdown out and the monitor not held.
    if (vm_declared_ && vm_countdown_ <= 0 && vm_hold_ <= 0) {
        const int32_t w = vm_decl_.player;
        switch (vm_decl_.type) {
            case galactic::kVictoryAllRetreated:
                finish(w, galactic::kVictoryAllRetreated, retreat_side_ >= 0 ? side_player(static_cast<uint8_t>(retreat_side_)) : galactic::kNoPlayer);
                break;
            case galactic::kVictoryStory:  // §3.1 step 3: R = the loser
                finish(w, galactic::kVictoryStory, w == req_.info.attacker ? req_.info.defender : req_.info.attacker);
                break;
            case kSkirmishLandBase: finish(w, galactic::kVictoryEnemyBaseDestroyed, galactic::kNoPlayer); break;  // reported as 0x0e / 0x0f
            case kSkirmishStarbase: finish(w, galactic::kVictoryStarbaseDestroyed, galactic::kNoPlayer); break;
            default: finish(w, vm_decl_.type, galactic::kNoPlayer); break;
        }
        return;
    }
    // [port] time limit: the defender holds, the attacker withdraws; not while a victory is declared or the monitor held.
    if (!vm_declared_ && vm_hold_ <= 0 && cfg_.max_frames > 0 && frame_ >= cfg_.max_frames)
        finish(req_.info.defender, galactic::kVictoryAllRetreated, req_.info.attacker);
}

}  // namespace alamo::sim
