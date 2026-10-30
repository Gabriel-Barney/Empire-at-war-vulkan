// A1 - DAMAGE_TRACKING (behaviour 0x3e) store and FindDeadlyEnemy, docs/re/AI_TACTICAL.md 6.3.
//
// Each tracked object keeps attackerId -> accumulated damage. Every behaviour service the entries
// decay by (maxHealth + maxShield) x AI_SpaceThreatDecayStep and are removed at <= 0 (0x58bd80)
// [H; M field names]. Feeding: one record() per damage event the combat system applies (see
// record_combat_damage when engine/tactical/combat is configured).
//
// [port] ORDER: the retail maps are MSVC hash containers (order = hash of the attacker id). Here
// every map is a std::map ascending by id: victims ascending, attackers ascending. FindDeadlyEnemy
// sums the victims' maps by attacker id (victims in the given order, so each attacker's float sum
// has a defined order) and returns the attacker with the strictly greatest total; ties -> the
// lowest attacker id (deviation from retail only on exact float ties) [H rule, port order].
// The attacker is looked up by id; if it no longer exists the result is nil (no fallback) [H].
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <vector>

namespace alamo {
class StateHasher;
class ByteWriter;
class ByteReader;
}

#if defined(ALAMO_AI_TACTICAL_HAVE_COMBAT)
#include "alamo/tactical/combat/combat_system.hpp"
#endif

namespace alamo::ai::tactical {

inline constexpr int32_t kNoAttacker = -1;

class DamageTrackingStore {
public:
    // The object has DAMAGE_TRACKING (spawn); max values feed the decay.
    void enable(int32_t object, float max_health, float max_shield);
    void set_max(int32_t object, float max_health, float max_shield);
    void disable(int32_t object);  // object removed / dead
    bool tracks(int32_t object) const { return objects_.count(object) != 0; }

    // Adds `amount` to victim[attacker]. Ignored for an untracked victim. Attacker validity is the
    // caller's (e.g. no record for kNoObject sources).
    void record(int32_t victim, int32_t attacker, float amount);
    // One behaviour service of `victim`: every entry -= (maxHealth + maxShield) * decay_step,
    // entries <= 0 removed.
    void service(int32_t victim, float decay_step);
    // Every tracked object, ascending id (when the host services all at one point of the frame).
    void service_all(float decay_step);
    // Entries of one victim (ascending attacker id); nullptr if untracked.
    const std::map<int32_t, float>* entries(int32_t victim) const;
    size_t tracked_count() const { return objects_.size(); }

    void hash(StateHasher& h) const;  // victims ascending: id, max values, entries ascending
    // N1 save / load (contract: alamo/core/save_file.hpp). The store is the HOST's (TacticalAiHost::damage_tracking), so
    // the host saves it in its own chunk. Layout: u32 version (1), u32 count + victims ascending (i32 id, f32 max
    // health, f32 max shield, u32 count + entries ascending (i32 attacker, f32 amount)). read() replaces the store;
    // false and unchanged on bad data; reads its own fields only (the owner checks done()).
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    struct Tracked {
        float max_health = 0.0f, max_shield = 0.0f;
        std::map<int32_t, float> by_attacker;
    };
    std::map<int32_t, Tracked> objects_;
};

// Expands one object to the tracked objects whose maps count for it: the object itself if it is
// tracked, else (a TEAM) its members recursively, in the member order `members` returns (members
// of a non-team: none). Appends to `out`.
using TeamMembersFn = std::function<void(int32_t object, std::vector<int32_t>& members)>;
void collect_tracked(const DamageTrackingStore& store, int32_t object, const TeamMembersFn& members,
                     std::vector<int32_t>& out);

// FindDeadlyEnemy core: sum the victims' maps by attacker id, pick the strictly greatest total
// (ties -> lowest id). Returns kNoAttacker when nothing is tracked or when `exists` says the picked
// attacker no longer exists. Forms (6.3): object/unit target = collect_tracked(object); task force
// = collect_tracked of every unit (task-force order); (location, radius, player) = the GOM query's
// objects of that player with DAMAGE_TRACKING within radius (the host's query, id order).
int32_t find_deadly_enemy(const DamageTrackingStore& store, std::span<const int32_t> victims,
                          const std::function<bool(int32_t attacker)>& exists = {});

#if defined(ALAMO_AI_TACTICAL_HAVE_COMBAT)
// Combat adapter: records one applied CombatSystem::take_damage result. PROVISIONAL [L]: the
// tracked amount is the damage after multipliers (DamageResult::after_multipliers); skipped when
// not applied or when the source is kNoObject. The attacker is the request's source object.
void record_combat_damage(DamageTrackingStore& store, const ::alamo::tactical::combat::DamageRequest& req,
                          const ::alamo::tactical::combat::DamageResult& res);
#endif

}  // namespace alamo::ai::tactical
