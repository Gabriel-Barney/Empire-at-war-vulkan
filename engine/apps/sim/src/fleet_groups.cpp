// [port] Fleet groups, phase 1: sim/fleet_groups.hpp.
#include "alamo/sim/fleet_groups.hpp"

#include <algorithm>

#include "alamo/core/bytes.hpp"
#include "alamo/core/sim_crc.hpp"

namespace alamo::sim {

namespace mv = tactical::movement;
using R = commands::RejectReason;

namespace {

constexpr uint32_t kSaveVersion = 1;

std::vector<objects::ObjectId> sorted_unique(std::span<const objects::ObjectId> units) {
    std::vector<objects::ObjectId> v(units.begin(), units.end());
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

bool contains(const std::vector<objects::ObjectId>& sorted, objects::ObjectId id) {
    return std::binary_search(sorted.begin(), sorted.end(), id);
}

bool has_any(const FleetGroup& g, const std::vector<objects::ObjectId>& sorted) {
    for (const FleetMember& m : g.members)
        if (contains(sorted, m.id)) return true;
    return false;
}

bool explicit_facing(float facing_deg) { return facing_deg - facing_deg == 0.0f && facing_deg < 1.0e17f && facing_deg > -1.0e17f; }

}  // namespace

const FleetGroup* FleetGroups::group_of(objects::ObjectId id) const {
    for (const FleetGroup& g : groups_)
        for (const FleetMember& m : g.members)
            if (m.id == id) return &g;
    return nullptr;
}

const FleetGroup* FleetGroups::group(uint16_t id) const {
    for (const FleetGroup& g : groups_)
        if (g.id == id) return &g;
    return nullptr;
}

FleetGroup* FleetGroups::find(uint16_t id) {
    for (FleetGroup& g : groups_)
        if (g.id == id) return &g;
    return nullptr;
}

bool FleetGroups::any_moving() const {
    for (const FleetGroup& g : groups_)
        if (g.moving) return true;
    return false;
}

void FleetGroups::remove_units(std::span<const objects::ObjectId> units) {
    const std::vector<objects::ObjectId> s = sorted_unique(units);
    for (FleetGroup& g : groups_)
        g.members.erase(std::remove_if(g.members.begin(), g.members.end(), [&s](const FleetMember& m) { return contains(s, m.id); }),
                        g.members.end());
}

void FleetGroups::drop_empty() {
    groups_.erase(std::remove_if(groups_.begin(), groups_.end(), [](const FleetGroup& g) { return g.members.empty(); }), groups_.end());
}

// Slots from the members' current positions: the centre is their mean (summed in member order), the frame's forward the
// first valid member's yaw.
void FleetGroups::capture_slots(FleetGroup& g, const InfoFn& info) {
    mv::V2 sum{0.0f, 0.0f};
    uint32_t n = 0;
    bool have_facing = false;
    std::vector<FleetUnitInfo> infos;
    infos.reserve(g.members.size());
    for (const FleetMember& m : g.members) {
        infos.push_back(info(m.id));
        const FleetUnitInfo& u = infos.back();
        if (!u.valid) continue;
        sum = sum + u.pos;
        ++n;
        if (!have_facing) {
            g.facing_deg = u.yaw_deg;
            have_facing = true;
        }
    }
    if (n == 0) return;
    const mv::V2 centre = sum * (1.0f / static_cast<float>(n));
    const mv::V2 f = mv::dir_from_deg(g.facing_deg);
    const mv::V2 l = mv::perp_left(f);
    for (size_t i = 0; i < g.members.size(); ++i) {
        FleetMember& m = g.members[i];
        m.moving = false;
        if (!infos[i].valid) continue;
        const mv::V2 d = infos[i].pos - centre;
        m.slot = {mv::dot(d, f), mv::dot(d, l)};
    }
    g.moving = false;
}

R FleetGroups::apply(uint8_t player, const commands::FleetGroupCommand& c, std::span<const objects::ObjectId> units,
                     const InfoFn& info) {
    using Op = commands::FleetGroupOp;
    const std::vector<objects::ObjectId> sel = sorted_unique(units);
    switch (c.op) {
        case Op::Create: {
            if (sel.empty()) return R::InvalidUnit;
            // Groups the selection empties make room; count the player's groups that remain.
            size_t remaining = 0;
            for (const FleetGroup& g : groups_) {
                if (g.owner != player) continue;
                bool emptied = true;
                for (const FleetMember& m : g.members)
                    if (!contains(sel, m.id)) emptied = false;
                if (!emptied) ++remaining;
            }
            if (remaining >= kMaxGroupsPerPlayer || next_id_ == 0xFFFFu) return R::QueueFull;
            remove_units(sel);
            drop_empty();
            FleetGroup g;
            g.id = next_id_++;
            g.owner = player;
            for (const objects::ObjectId id : sel) {
                if (g.members.size() >= kMaxMembers) break;
                FleetMember m;
                m.id = id;
                g.members.push_back(m);
            }
            capture_slots(g, info);
            groups_.push_back(std::move(g));
            used_ = true;
            return R::None;
        }
        case Op::Dissolve: {
            const size_t before = groups_.size();
            groups_.erase(std::remove_if(groups_.begin(), groups_.end(),
                                         [&](const FleetGroup& g) { return g.owner == player && has_any(g, sel); }),
                          groups_.end());
            return groups_.size() == before ? R::InvalidUnit : R::None;
        }
        case Op::RemoveUnits: {
            bool any = false;
            for (const FleetGroup& g : groups_)
                if (g.owner == player && has_any(g, sel)) any = true;
            if (!any) return R::InvalidUnit;
            remove_units(sel);
            drop_empty();
            return R::None;
        }
        case Op::SetLock: {
            bool any = false;
            for (FleetGroup& g : groups_) {
                if (g.owner != player || !has_any(g, sel)) continue;
                any = true;
                if (c.locked && !g.locked) capture_slots(g, info);
                if (!c.locked) {
                    g.moving = false;
                    for (FleetMember& m : g.members) m.moving = false;
                }
                g.locked = c.locked;
            }
            return any ? R::None : R::InvalidUnit;
        }
        case Op::Rename: {
            FleetGroup* g = find(c.group);
            if (!g || g->owner != player) return R::InvalidTarget;
            g->name = c.name.substr(0, commands::kFleetGroupNameMax);
            return R::None;
        }
    }
    return R::Malformed;
}

void FleetGroups::plan_move(uint8_t player, std::span<const objects::ObjectId> units, mv::V2 dest, float facing_deg, const InfoFn& info,
                            std::vector<LockedMove>& moves, std::vector<objects::ObjectId>& rest) const {
    moves.clear();
    rest.clear();
    const std::vector<objects::ObjectId> sel = sorted_unique(units);
    std::vector<objects::ObjectId> taken;
    for (const FleetGroup& g : groups_) {
        if (g.owner != player || !g.locked) continue;
        mv::V2 pos_sum{0.0f, 0.0f}, slot_sum{0.0f, 0.0f};
        uint32_t n = 0;
        std::vector<const FleetMember*> movers;
        for (const FleetMember& m : g.members) {
            if (!contains(sel, m.id)) continue;
            const FleetUnitInfo u = info(m.id);
            if (!u.valid) continue;
            movers.push_back(&m);
            pos_sum = pos_sum + u.pos;
            slot_sum = slot_sum + m.slot;
            ++n;
        }
        if (n == 0) continue;
        const float inv = 1.0f / static_cast<float>(n);
        const mv::V2 centre = pos_sum * inv, mean_slot = slot_sum * inv;
        LockedMove lm;
        lm.group = g.id;
        if (explicit_facing(facing_deg)) lm.facing_deg = facing_deg;
        else if (mv::length_sq(dest - centre) > 1.0f) lm.facing_deg = mv::heading_deg(dest - centre);
        else lm.facing_deg = g.facing_deg;
        const mv::V2 f = mv::dir_from_deg(lm.facing_deg);
        const mv::V2 l = mv::perp_left(f);
        for (const FleetMember* m : movers) {
            const mv::V2 s = m->slot - mean_slot;
            lm.goals.push_back({m->id, dest + f * s.x + l * s.y});
            taken.push_back(m->id);
        }
        moves.push_back(std::move(lm));
    }
    std::sort(taken.begin(), taken.end());
    for (const objects::ObjectId id : units)
        if (!contains(taken, id)) rest.push_back(id);
}

void FleetGroups::start_move(const LockedMove& lm, const InfoFn& info) {
    FleetGroup* g = find(lm.group);
    if (!g) return;
    g->moving = true;
    g->facing_deg = lm.facing_deg;
    for (FleetMember& m : g->members) {
        m.moving = false;
        for (const MemberGoal& goal : lm.goals) {
            if (goal.id != m.id) continue;
            const FleetUnitInfo u = info(m.id);
            m.moving = true;
            m.goal = goal.goal;
            m.start_dist = u.valid ? mv::distance(u.pos, goal.goal) : 0.0f;
        }
    }
}

void FleetGroups::interrupt(std::span<const objects::ObjectId> units) {
    const std::vector<objects::ObjectId> sel = sorted_unique(units);
    for (FleetGroup& g : groups_) {
        if (!g.moving || !has_any(g, sel)) continue;
        g.moving = false;
        for (FleetMember& m : g.members) m.moving = false;
    }
}

void FleetGroups::prune(const InfoFn& info) {
    bool changed = false;
    for (FleetGroup& g : groups_) {
        const size_t before = g.members.size();
        const int32_t owner = g.owner;
        g.members.erase(std::remove_if(g.members.begin(), g.members.end(),
                                       [&info, owner](const FleetMember& m) {
                                           const FleetUnitInfo u = info(m.id);
                                           return !u.valid || u.owner != owner;
                                       }),
                        g.members.end());
        changed = changed || g.members.size() != before;
    }
    if (changed) drop_empty();
}

void FleetGroups::regulate(const InfoFn& info, const std::function<void(objects::ObjectId, float)>& scale) {
    for (FleetGroup& g : groups_) {
        if (!g.moving) continue;
        std::vector<FleetUnitInfo> infos;
        infos.reserve(g.members.size());
        bool any_goal = false;
        float slowest = 3.0e38f, p_min = 3.0e38f;
        for (const FleetMember& m : g.members) {
            infos.push_back(m.moving ? info(m.id) : FleetUnitInfo{});
            const FleetUnitInfo& u = infos.back();
            if (!m.moving || !u.valid || !u.has_goal) continue;
            any_goal = true;
            if (u.max_speed > 0.0f && u.max_speed < slowest) slowest = u.max_speed;
            const float p = m.start_dist > 1.0e-3f ? 1.0f - mv::distance(u.pos, m.goal) / m.start_dist : 1.0f;
            if (p < p_min) p_min = p;
        }
        if (!any_goal) {
            g.moving = false;
            for (FleetMember& m : g.members) m.moving = false;
            continue;
        }
        for (size_t i = 0; i < g.members.size(); ++i) {
            const FleetMember& m = g.members[i];
            const FleetUnitInfo& u = infos[i];
            if (!m.moving || !u.valid || !u.has_goal || !(u.max_speed > 0.0f)) continue;
            float s = slowest < u.max_speed ? slowest / u.max_speed : 1.0f;
            const float p = m.start_dist > 1.0e-3f ? 1.0f - mv::distance(u.pos, m.goal) / m.start_dist : 1.0f;
            const float lead = (p - p_min) * m.start_dist;
            if (lead > straggler_distance && straggler_distance > 0.0f)
                s = s * std::max(min_leader_scale, 1.0f - (lead - straggler_distance) / straggler_distance);
            if (s < 1.0f) scale(m.id, s);
        }
    }
}

void FleetGroups::hash(StateHasher& h) const {
    h.u16(next_id_);
    h.u32(static_cast<uint32_t>(groups_.size()));
    for (const FleetGroup& g : groups_) {
        h.u16(g.id);
        h.u8(g.owner);
        h.boolean(g.locked);
        h.boolean(g.moving);
        h.f32(g.facing_deg);
        h.str(g.name);
        h.u32(static_cast<uint32_t>(g.members.size()));
        for (const FleetMember& m : g.members) {
            h.i32(m.id);
            h.f32(m.slot.x);
            h.f32(m.slot.y);
            h.boolean(m.moving);
            if (!m.moving) continue;
            h.f32(m.goal.x);
            h.f32(m.goal.y);
            h.f32(m.start_dist);
        }
    }
}

void FleetGroups::write(ByteWriter& w) const {
    w.u32(kSaveVersion);
    w.u16(next_id_);
    w.boolean(used_);
    w.u32(static_cast<uint32_t>(groups_.size()));
    for (const FleetGroup& g : groups_) {
        w.u16(g.id).u8(g.owner).boolean(g.locked).boolean(g.moving).f32(g.facing_deg).str(g.name);
        w.u32(static_cast<uint32_t>(g.members.size()));
        for (const FleetMember& m : g.members) {
            w.i32(m.id).f32(m.slot.x).f32(m.slot.y).f32(m.goal.x).f32(m.goal.y).f32(m.start_dist).boolean(m.moving);
        }
    }
}

bool FleetGroups::read(ByteReader& r) {
    if (r.u32() != kSaveVersion || !r.ok()) return false;
    const uint16_t next = r.u16();
    const bool used = r.boolean();
    uint32_t n = 0;
    if (!r.count(n, 12)) return false;
    std::vector<FleetGroup> groups;
    groups.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        FleetGroup g;
        g.id = r.u16();
        g.owner = r.u8();
        g.locked = r.boolean();
        g.moving = r.boolean();
        g.facing_deg = r.f32();
        g.name = r.str();
        uint32_t members = 0;
        if (!r.count(members, 25) || members == 0 || members > kMaxMembers || g.name.size() > commands::kFleetGroupNameMax ||
            g.id == 0 || g.id >= next)
            return false;
        for (uint32_t k = 0; k < members; ++k) {
            FleetMember m;
            m.id = r.i32();
            m.slot.x = r.f32();
            m.slot.y = r.f32();
            m.goal.x = r.f32();
            m.goal.y = r.f32();
            m.start_dist = r.f32();
            m.moving = r.boolean();
            if (!g.members.empty() && !(g.members.back().id < m.id)) return false;
            g.members.push_back(m);
        }
        groups.push_back(std::move(g));
    }
    if (!r.ok()) return false;
    groups_ = std::move(groups);
    next_id_ = next;
    used_ = used;
    return true;
}

}  // namespace alamo::sim
