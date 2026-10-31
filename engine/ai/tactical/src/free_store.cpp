#include "alamo/ai/tactical/free_store.hpp"

#include "alamo/core/sim_crc.hpp"

namespace alamo::ai::tactical {

namespace {

// 0x4d2c90
bool base_admits(const FreeStoreObjectInfo& o) { return !o.dead_or_removing && o.in_world; }

}  // namespace

// 0x690a10 (A1_READINGS 6.2), in the original's order.
bool land_free_store_admits(const FreeStoreObjectInfo& o) {
    if (o.type_create_team || o.is_transport) return false;
    const bool qualifies = o.has_lobbing_superweapon ||
                           (!o.type_category_bit_5f && (o.is_starship_combatant || o.is_team) && o.has_locomotor &&
                            !o.type_is_dummy);
    if (!qualifies) return false;
    if (o.has_indigenous_or_explosion || (o.is_team && o.team_has_bit_20)) return false;
    if (o.type_bit_5b || !o.owned_by_player || o.type_stay_in_transport) return false;
    if (o.subobject_f8_nonempty || o.in_mode_manager_set) return false;
    return base_admits(o);
}

// 0x6904b0 (A1_READINGS 6.1), in the original's order.
bool space_free_store_admits(const FreeStoreObjectInfo& o) {
    if (o.type_squadron_member || o.type_company_member) return false;
    const bool qualifies =
        o.has_special_weapon || (!o.type_is_dummy && (o.is_starship_combatant || o.is_team) && o.has_locomotor);
    if (!qualifies) return false;
    if (o.type_dummy_orbital_structure || o.type_dummy_star_base || o.has_tactical_super_weapon) return false;
    if (!o.owned_by_player || o.subobject_f8_nonempty) return false;
    return base_admits(o);
}

bool TacticalFreeStore::admit(int32_t object_id, uint32_t type_id) {
    const auto [it, inserted] = entries_.try_emplace(object_id, Entry{type_id, false});
    if (!inserted) return false;
    ++per_type_[type_id];
    return true;
}

bool TacticalFreeStore::remove(int32_t object_id) {
    const auto it = entries_.find(object_id);
    if (it == entries_.end()) return false;
    const auto t = per_type_.find(it->second.type_id);
    if (t != per_type_.end() && --t->second <= 0) per_type_.erase(t);
    entries_.erase(it);
    return true;
}

int32_t TacticalFreeStore::type_count(uint32_t type_id) const {
    const auto it = per_type_.find(type_id);
    return it == per_type_.end() ? 0 : it->second;
}

bool TacticalFreeStore::set_order_lock(int32_t object_id, bool locked) {
    const auto it = entries_.find(object_id);
    if (it == entries_.end()) return false;
    it->second.locked = locked;
    return true;
}

bool TacticalFreeStore::order_locked(int32_t object_id) const {
    const auto it = entries_.find(object_id);
    return it != entries_.end() && it->second.locked;
}

void TacticalFreeStore::units(std::vector<int32_t>& out, bool unlocked_only) const {
    out.clear();
    for (const auto& [id, e] : entries_)
        if (!unlocked_only || !e.locked) out.push_back(id);
}

void TacticalFreeStore::hash(StateHasher& h) const {
    h.u32(static_cast<uint32_t>(entries_.size()));
    for (const auto& [id, e] : entries_) {
        h.i32(id);
        h.u32(e.type_id);
        h.boolean(e.locked);
    }
    h.u32(static_cast<uint32_t>(per_type_.size()));
    for (const auto& [t, n] : per_type_) {
        h.u32(t);
        h.i32(n);
    }
}

void FreeStoreSchedule::hash(StateHasher& h) const {
    h.boolean(service_rate.has_value());
    h.f64(service_rate.value_or(0.0));
    h.boolean(unit_service_rate.has_value());
    h.f64(unit_service_rate.value_or(0.0));
    h.f64(last_service);
    h.f64(last_unit_service);
}

FreeStoreSchedule busy_tactical_schedule() {
    FreeStoreSchedule s;
    s.service_rate = 20.0;
    s.unit_service_rate = 2.0;
    return s;
}

// 1.0f / 30.0f is the float 0.033333335f (0x140b0a344).
float free_store_mode_time(uint32_t mode_frame) { return static_cast<float>(mode_frame) * (1.0f / 30.0f); }

void free_store_service(FreeStoreSchedule& sched, const TacticalFreeStore& store, uint32_t mode_frame,
                        FreeStoreServiceStep& out) {
    out.now = free_store_mode_time(mode_frame);
    out.run_service = false;
    out.run_unit_service = false;
    out.units.clear();
    const double now = static_cast<double>(out.now);
    if (sched.service_rate && now - sched.last_service > *sched.service_rate) {
        out.run_service = true;
        sched.last_service = now;
    }
    if (sched.unit_service_rate && now - sched.last_unit_service > *sched.unit_service_rate) {
        out.run_unit_service = true;
        store.units(out.units, true);
        sched.last_unit_service = now;
    }
}

}  // namespace alamo::ai::tactical
