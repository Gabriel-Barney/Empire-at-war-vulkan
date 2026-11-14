#include "alamo/tactical/garrison/garrison.hpp"
#include "alamo/core/sim_crc.hpp"

#include <algorithm>
#include <bit>

#include "alamo/core/dmath.hpp"

namespace alamo::tactical::garrison {

namespace {
template <class T>
int32_t find_sorted(const std::vector<T>& v, ObjectId id) {
    const auto it = std::lower_bound(v.begin(), v.end(), id, [](const T& e, ObjectId x) { return e.id < x; });
    return (it == v.end() || it->id != id) ? -1 : static_cast<int32_t>(it - v.begin());
}

// 2D unit direction of a facing vector; +X when degenerate.
void dir2(Vec3 f, float& dx, float& dy) {
    const float l2 = f.x * f.x + f.y * f.y;
    if (!(l2 > 0.0f)) {
        dx = 1.0f;
        dy = 0.0f;
        return;
    }
    const float inv = 1.0f / dmath::sqrt(l2);
    dx = f.x * inv;
    dy = f.y * inv;
}

float dist3(Vec3 a, Vec3 b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return dmath::sqrt(dx * dx + dy * dy + dz * dz);
}

bool same_bits(Vec3 a, Vec3 b) {
    return std::bit_cast<uint32_t>(a.x) == std::bit_cast<uint32_t>(b.x) && std::bit_cast<uint32_t>(a.y) == std::bit_cast<uint32_t>(b.y) &&
           std::bit_cast<uint32_t>(a.z) == std::bit_cast<uint32_t>(b.z);
}
}  // namespace

int32_t GarrisonSystem::holder_index(ObjectId id) const { return find_sorted(holders_, id); }
int32_t GarrisonSystem::unit_index(ObjectId id) const { return find_sorted(units_, id); }

bool GarrisonSystem::add_holder(ObjectId id, int32_t owner, const HolderDesc& d) {
    const auto it = std::lower_bound(holders_.begin(), holders_.end(), id, [](const Holder& e, ObjectId x) { return e.id < x; });
    if (it != holders_.end() && it->id == id) return false;
    Holder h;
    h.id = id;
    h.owner = owner;
    h.d = d;
    holders_.insert(it, std::move(h));
    return true;
}

bool GarrisonSystem::add_unit(ObjectId id, int32_t owner, const OccupantDesc& d) {
    const auto it = std::lower_bound(units_.begin(), units_.end(), id, [](const Unit& e, ObjectId x) { return e.id < x; });
    if (it != units_.end() && it->id == id) return false;
    Unit u;
    u.id = id;
    u.owner = owner;
    u.d = d;
    units_.insert(it, u);
    return true;
}

const HolderDesc* GarrisonSystem::holder_desc(ObjectId id) const {
    const int32_t i = holder_index(id);
    return i < 0 ? nullptr : &holders_[static_cast<size_t>(i)].d;
}
const OccupantDesc* GarrisonSystem::unit_desc(ObjectId id) const {
    const int32_t i = unit_index(id);
    return i < 0 ? nullptr : &units_[static_cast<size_t>(i)].d;
}

void GarrisonSystem::push(GarrisonEventKind k, const Unit& u, ObjectId holder, Vec3 p, uint32_t frame) {
    GarrisonEvent e;
    e.kind = k;
    e.unit = u.id;
    e.holder = holder;
    e.owner = u.owner;
    e.position = p;
    e.frame = frame;
    events_.push_back(e);
}

void GarrisonSystem::leave(Unit& u, Holder& h, GarrisonEventKind kind, Vec3 at, uint32_t frame) {
    const auto it = std::find(h.inside.begin(), h.inside.end(), u.id);
    if (it != h.inside.end()) h.inside.erase(it);
    u.state = State::None;
    u.host = kNoObject;
    push(kind, u, h.id, at, frame);
}

Vec3 GarrisonSystem::approach_point(ObjectId holder, Vec3 pos, Vec3 facing, int32_t bone) const {
    const HolderDesc* d = holder_desc(holder);
    if (!d || bone < 0 || static_cast<size_t>(bone) >= d->bones.size()) return pos;
    float fx = 0.0f, fy = 0.0f;
    dir2(facing, fx, fy);
    const Vec3 b = d->bones[static_cast<size_t>(bone)];
    // host-local x forward / y left -> world: forward (fx, fy), left (-fy, fx)
    Vec3 a{pos.x + b.x * fx - b.y * fy, pos.y + b.x * fy + b.y * fx, pos.z + b.z};
    if (d->enter_dist > 0.0f) {  // pushed outward along the 2-D host -> bone direction, the bone's z kept
        const float ox = a.x - pos.x, oy = a.y - pos.y, l2 = ox * ox + oy * oy;
        if (l2 > 0.0f) {
            const float k = d->enter_dist / dmath::sqrt(l2);
            a.x += ox * k;
            a.y += oy * k;
        }
    }
    return a;
}

Vec3 GarrisonSystem::approach_destination(ObjectId holder, Vec3 pos, Vec3 facing, Vec3 from, float extent_len,
                                          const ReachableFn& reachable, int32_t& bone) const {
    bone = -1;
    const HolderDesc* d = holder_desc(holder);
    if (!d) return pos;
    Vec3 best = pos;
    float best_d = 0.0f;
    for (size_t i = 0; i < d->bones.size(); ++i) {
        const Vec3 a = approach_point(holder, pos, facing, static_cast<int32_t>(i));
        const float dd = dist3(a, from);
        if (bone >= 0 && !(dd < best_d)) continue;
        if (reachable && !reachable(a)) continue;
        best = a;
        best_d = dd;
        bone = static_cast<int32_t>(i);
    }
    if (bone >= 0) return best;
    float dx = 0.0f, dy = 0.0f;
    dir2(facing, dx, dy);
    const float l = d->enter_dist > 0.0f ? d->enter_dist : 0.5f * extent_len;
    return {pos.x + dx * l, pos.y + dy * l, pos.z};
}

Vec3 GarrisonSystem::exit_point(ObjectId holder, Vec3 pos, Vec3 facing, int32_t bone) const {
    // s78n t20 batch 2 (FIGHTER_FLYING §7.4 "Exit", 0x444830): E = the bone's world point with yaw = host -> bone, else
    // the host position and yaw; X = E + dist (cos, sin)(yaw), dist = Garrison_Exit_Dist, else |model half-extent|.
    const HolderDesc* d = holder_desc(holder);
    if (!d) return pos;
    float dx = 0.0f, dy = 0.0f;
    dir2(facing, dx, dy);
    Vec3 e = pos;
    if (bone >= 0 && static_cast<size_t>(bone) < d->bones.size()) {
        const Vec3 b = d->bones[static_cast<size_t>(bone)];
        e = {pos.x + b.x * dx - b.y * dy, pos.y + b.x * dy + b.y * dx, pos.z + b.z};
        const float ox = e.x - pos.x, oy = e.y - pos.y, l2 = ox * ox + oy * oy;
        if (l2 > 0.0f) {
            const float k = 1.0f / dmath::sqrt(l2);
            dx = ox * k;
            dy = oy * k;
        }
    }
    const float r = d->exit_dist > 0.0f ? d->exit_dist : d->extent_len;
    return {e.x + dx * r, e.y + dy * r, e.z};
}

void GarrisonSystem::remove(ObjectId id, Vec3 pos, Vec3 facing, uint32_t frame, bool kill) {
    if (const int32_t hi = holder_index(id); hi >= 0) {
        Holder& h = holders_[static_cast<size_t>(hi)];
        if (kill || h.d.kind == HolderKind::Property) {  // no host behaviour (or a hover host in the air): they die with it
            const std::vector<ObjectId> inside = h.inside;
            for (const ObjectId uid : inside)
                if (const int32_t ui = unit_index(uid); ui >= 0) leave(units_[static_cast<size_t>(ui)], h, GarrisonEventKind::Killed, pos, frame);
        } else if (thief_holder(h)) {  // s78n t12 review: the thief leaves through the vehicle's death, alive, where it stood
            const std::vector<ObjectId> inside = h.inside;
            for (const ObjectId uid : inside)
                if (const int32_t ui = unit_index(uid); ui >= 0) leave(units_[static_cast<size_t>(ui)], h, GarrisonEventKind::Ejected, pos, frame);
        } else {
            // s78n t26.1 (FIGHTER_FLYING §7.4, 0x37a060): a destroyed host puts every occupant at its position
            (void)facing;
            const std::vector<ObjectId> inside = h.inside;  // entry order
            for (const ObjectId uid : inside)
                if (const int32_t ui = unit_index(uid); ui >= 0) leave(units_[static_cast<size_t>(ui)], h, GarrisonEventKind::Ejected, pos, frame);
        }
        for (Unit& u : units_)
            if (u.host == id && u.state != State::Inside) {
                const State was = u.state;
                u.state = State::None;
                u.host = kNoObject;
                u.awaiting = false;
                push(GarrisonEventKind::OrderDropped, u, id, pos, frame);
                events_.back().was = static_cast<uint8_t>(was);
            }
        holders_.erase(holders_.begin() + holder_index(id));
    }
    if (const int32_t ui = unit_index(id); ui >= 0) {
        Unit& u = units_[static_cast<size_t>(ui)];
        if (u.state == State::Inside)
            if (const int32_t hi = holder_index(u.host); hi >= 0) {
                std::vector<ObjectId>& in = holders_[static_cast<size_t>(hi)].inside;
                const auto it = std::find(in.begin(), in.end(), id);
                if (it != in.end()) in.erase(it);
            }
        units_.erase(units_.begin() + ui);
    }
}

void GarrisonSystem::set_owner(ObjectId id, int32_t owner, Vec3 pos, Vec3 facing, uint32_t frame) {
    if (const int32_t hi = holder_index(id); hi >= 0) {
        Holder& h = holders_[static_cast<size_t>(hi)];
        if (h.owner != owner) {
            h.owner = owner;
            // Occupants allied with the new owner stay (a capture by the garrison itself, rule (b) of capture.hpp);
            // the rest are ejected in entry order at the exit point (see the header).
            if (!h.inside.empty() && !thief_holder(h)) {  // s78n t12 review: a stolen vehicle keeps its thief
                const std::vector<ObjectId> inside = h.inside;
                for (const ObjectId uid : inside) {
                    const int32_t ui = unit_index(uid);
                    if (ui < 0) continue;
                    Unit& u = units_[static_cast<size_t>(ui)];
                    if (allied(u.owner, owner)) continue;
                    leave(u, holders_[static_cast<size_t>(hi)], GarrisonEventKind::Ejected, exit_point(id, pos, facing, u.bone), frame);
                }
            }
        }
    }
    if (const int32_t ui = unit_index(id); ui >= 0) units_[static_cast<size_t>(ui)].owner = owner;
}

int32_t GarrisonSystem::slot_value(const Unit& u) const {
    GarrisonGroup g;
    return group_of(u, g) ? (g.value > 0 ? g.value : 0) : value_of(u);
}

uint64_t GarrisonSystem::category_of(const Unit& u) const {
    GarrisonGroup g;
    return group_of(u, g) ? g.category : u.d.category;
}

bool GarrisonSystem::masks_match(ObjectId unit, ObjectId holder) const {
    const int32_t ui = unit_index(unit);
    const HolderDesc* h = holder_desc(holder);
    return ui >= 0 && h && mask_ok(category_of(units_[static_cast<size_t>(ui)]), h->accepts);
}

int32_t GarrisonSystem::reserved(ObjectId holder) const {
    int32_t n = 0;
    std::vector<ObjectId> groups;  // s78n t18: a squad walking in reserves once
    for (const Unit& u : units_) {
        if (u.host != holder || (u.state != State::Moving && u.state != State::Closing)) continue;  // states 1 - 2
        GarrisonGroup g;
        if (group_of(u, g)) {
            if (std::find(groups.begin(), groups.end(), g.id) != groups.end()) continue;
            groups.push_back(g.id);
        }
        ++n;
    }
    return n;
}

bool GarrisonSystem::can_garrison(ObjectId unit, ObjectId holder) const {
    const int32_t ui = unit_index(unit), hi = holder_index(holder);
    if (ui < 0 || hi < 0 || unit == holder) return false;
    const Unit& u = units_[static_cast<size_t>(ui)];
    const Holder& h = holders_[static_cast<size_t>(hi)];
    if (u.state == State::Inside || u.state == State::Entering) return false;
    if (thief_holder(h) || u.d.value == kBoardOnlyValue) return false;  // s78n t12 review: not a garrison / no GARRISON_UNIT
    if (!host_open(h, u.owner) || !mask_ok(category_of(u), h.d.accepts)) return false;
    return free_slots(holder) - reserved(holder) - slot_value(u) > 0;  // the stricter Lua rule (§2.5)
}

bool GarrisonSystem::can_garrison_fire(ObjectId id) const {
    if (const int32_t hi = holder_index(id); hi >= 0) return holders_[static_cast<size_t>(hi)].d.can_fire;
    const ObjectId in = holder_of(id);
    if (in == kNoObject) return false;
    const int32_t hi = holder_index(in);
    return hi >= 0 && holders_[static_cast<size_t>(hi)].d.can_fire;
}

ObjectId GarrisonSystem::holder_of(ObjectId unit) const {
    const int32_t ui = unit_index(unit);
    if (ui < 0) return kNoObject;
    const Unit& u = units_[static_cast<size_t>(ui)];
    return u.state == State::Inside ? u.host : kNoObject;
}

std::span<const ObjectId> GarrisonSystem::occupants(ObjectId holder) const {
    const int32_t hi = holder_index(holder);
    if (hi < 0) return {};
    return holders_[static_cast<size_t>(hi)].inside;
}

int32_t GarrisonSystem::used_slots(ObjectId holder) const {
    // 0x3954b0: the sum over the contents-list entries of the entry type's Garrison_Value. s78n t18: a squad is one
    // entry, its container (FIGHTER_FLYING §7.4): its members count once, with the group's value.
    int32_t n = 0;
    std::vector<ObjectId> groups;
    for (const ObjectId id : occupants(holder)) {
        const int32_t ui = unit_index(id);
        if (ui < 0) continue;
        const Unit& u = units_[static_cast<size_t>(ui)];
        GarrisonGroup g;
        if (group_of(u, g)) {
            if (std::find(groups.begin(), groups.end(), g.id) != groups.end()) continue;
            groups.push_back(g.id);
            n += g.value > 0 ? g.value : 0;
        } else {
            n += value_of(u);
        }
    }
    return n;
}

int32_t GarrisonSystem::free_slots(ObjectId holder) const {
    if (const int32_t hi = holder_index(holder); hi >= 0 && thief_holder(holders_[static_cast<size_t>(hi)])) return 0;
    const int32_t f = capacity(holder) - used_slots(holder);
    return f > 0 ? f : 0;
}

ObjectId GarrisonSystem::pending_holder(ObjectId unit) const {
    const int32_t ui = unit_index(unit);
    if (ui < 0) return kNoObject;
    const Unit& u = units_[static_cast<size_t>(ui)];
    return pending(u.state) ? u.host : kNoObject;
}

uint8_t GarrisonSystem::state_of(ObjectId unit) const {
    const int32_t ui = unit_index(unit);
    return ui < 0 ? uint8_t{0} : static_cast<uint8_t>(units_[static_cast<size_t>(ui)].state);
}

int32_t GarrisonSystem::bone_of(ObjectId unit) const {
    const int32_t ui = unit_index(unit);
    return ui < 0 ? -1 : units_[static_cast<size_t>(ui)].bone;
}

uint32_t GarrisonSystem::retries_of(ObjectId unit) const {
    const int32_t ui = unit_index(unit);
    return ui < 0 ? 0u : units_[static_cast<size_t>(ui)].retries;
}

bool GarrisonSystem::entering(ObjectId unit) const {
    const int32_t ui = unit_index(unit);
    return ui >= 0 && units_[static_cast<size_t>(ui)].state == State::Entering;
}

bool GarrisonSystem::enter_allowed(ObjectId unit, ObjectId holder) const {
    const int32_t ui = unit_index(unit), hi = holder_index(holder);
    if (ui < 0 || hi < 0 || unit == holder) return false;
    const Unit& u = units_[static_cast<size_t>(ui)];
    const Holder& h = holders_[static_cast<size_t>(hi)];
    if (u.state == State::Inside || u.state == State::Entering) return false;
    if (thief_holder(h) || u.d.value == kBoardOnlyValue) return false;  // s78n t12 review: entered only through board()
    return host_open(h, u.owner) && mask_ok(category_of(u), h.d.accepts);
}

bool GarrisonSystem::order_enter(ObjectId unit, ObjectId holder, int32_t bone) {
    if (!enter_allowed(unit, holder)) return false;
    Unit& u = units_[static_cast<size_t>(unit_index(unit))];
    u.state = State::Moving;
    u.host = holder;
    u.bone = bone;
    u.awaiting = true;  // the approach move is in flight: its end is the "move finished" signal
    return true;
}

bool GarrisonSystem::board(ObjectId unit, ObjectId holder, Vec3 at, uint32_t frame) {
    const int32_t ui = unit_index(unit), hi = holder_index(holder);
    if (ui < 0 || hi < 0 || unit == holder) return false;
    Unit& u = units_[static_cast<size_t>(ui)];
    if (u.state == State::Inside || u.state == State::Entering) return false;
    u.state = State::Inside;
    u.host = holder;
    holders_[static_cast<size_t>(hi)].inside.push_back(u.id);
    push(GarrisonEventKind::Entered, u, holder, at, frame);
    return true;
}

bool GarrisonSystem::cancel_enter(ObjectId unit) {
    const int32_t ui = unit_index(unit);
    if (ui < 0) return false;
    Unit& u = units_[static_cast<size_t>(ui)];
    if (u.state != State::Moving && u.state != State::Closing) return false;
    u.state = State::None;
    u.host = kNoObject;
    u.awaiting = false;
    return true;
}

bool GarrisonSystem::order_exit(ObjectId unit, Vec3 holder_pos, Vec3 holder_facing, uint32_t frame) {
    const int32_t ui = unit_index(unit);
    if (ui < 0) return false;
    Unit& u = units_[static_cast<size_t>(ui)];
    if (pending(u.state)) {  // cancel the approach
        const ObjectId host = u.host;
        const State was = u.state;
        u.state = State::None;
        u.host = kNoObject;
        u.awaiting = false;
        push(GarrisonEventKind::OrderDropped, u, host, holder_pos, frame);
        events_.back().was = static_cast<uint8_t>(was);
        return true;
    }
    const int32_t hi = u.state == State::Inside ? holder_index(u.host) : -1;
    if (hi < 0) return false;
    if (thief_holder(holders_[static_cast<size_t>(hi)])) return false;  // s78n t12 review: no leave order for the thief
    leave(u, holders_[static_cast<size_t>(hi)], GarrisonEventKind::Exited, exit_point(u.host, holder_pos, holder_facing, u.bone), frame);
    return true;
}

uint32_t GarrisonSystem::eject_all(ObjectId holder, Vec3 holder_pos, Vec3 holder_facing, uint32_t frame) {
    const int32_t hi = holder_index(holder);
    if (hi < 0) return 0;
    Holder& h = holders_[static_cast<size_t>(hi)];
    if (thief_holder(h)) return 0;  // s78n t12 review: Eject_Garrison does not reach a vehicle thief
    const std::vector<ObjectId> inside = h.inside;  // entry order
    uint32_t n = 0;
    for (const ObjectId id : inside) {
        const int32_t ui = unit_index(id);
        if (ui < 0) continue;
        Unit& u = units_[static_cast<size_t>(ui)];
        leave(u, h, GarrisonEventKind::Ejected, exit_point(holder, holder_pos, holder_facing, u.bone), frame);
        ++n;
    }
    return n;
}

// The cancel 0x443560: every member of the key leaves the approach (OrderDropped with the state it ended).
void GarrisonSystem::drop_key(const std::vector<size_t>& mem, ObjectId host, uint32_t frame, const PoseFn& pose) {
    for (const size_t i : mem) {
        Unit& m = units_[i];
        Vec3 mp, mf;
        (void)pose(m.id, mp, mf);
        const State was = m.state;
        m.state = State::None;
        m.host = kNoObject;
        m.awaiting = false;
        push(GarrisonEventKind::OrderDropped, m, host, mp, frame);
        events_.back().was = static_cast<uint8_t>(was);
    }
}

// 0x444440: entering (state 5, the fade), the move signal unsubscribed.
void GarrisonSystem::enter_key(const std::vector<size_t>& mem, ObjectId host, uint32_t frame, const PoseFn& pose, Vec3 at) {
    for (const size_t i : mem) {
        Unit& m = units_[i];
        Vec3 mp, mf;
        if (!pose(m.id, mp, mf)) mp = at;
        m.state = State::Entering;
        m.enter_at = frame + cfg_.enter_frames;
        m.awaiting = false;
        push(GarrisonEventKind::EnterStarted, m, host, mp, frame);
    }
}

// s78n t20 batch 2 (FIGHTER_FLYING §7.4 "Garrison approach in full" items 3 - 5): one approaching key - a squad's
// container (its pending members `mem`, ascending id) or a single unit - with the slots it takes.
void GarrisonSystem::service_key(const std::vector<size_t>& mem, ObjectId key, int32_t value, uint32_t frame, const PoseFn& pose,
                                 const ApproachFn& approach) {
    Unit& u0 = units_[mem.front()];
    const ObjectId host = u0.host;
    const int32_t hi = holder_index(host);
    Vec3 kp, kf, hp, hf;
    if (hi < 0 || !pose(key, kp, kf) || !pose(host, hp, hf)) return drop_key(mem, host, frame, pose);
    Holder& h = holders_[static_cast<size_t>(hi)];
    // capacity on every service and signal (free < the key's value -> cancel); s78n t12 review: a thief's vehicle
    if (thief_holder(h) || !host_open(h, u0.owner) || free_slots(host) < value) return drop_key(mem, host, frame, pose);
    if (u0.state == State::Entering) {
        if (frame < u0.enter_at) return;
        for (const size_t i : mem) {  // the whole key is inside in this pass
            Unit& m = units_[i];
            m.state = State::Inside;
            h.inside.push_back(m.id);
            push(GarrisonEventKind::Entered, m, host, hp, frame);
        }
        return;
    }
    ApproachInfo ai;
    if (approach && !approach(key, host, ai)) ai = ApproachInfo{};
    const Vec3 a = approach_point(host, hp, hf, u0.bone);
    const float d3 = dist3(a, kp) - ai.radius;
    // [M] the handler of the finished move runs before the frame's range test
    if (u0.awaiting && !ai.moving) {  // signal 10: the approach (or corrective) move finished - handler 0x443d10
        for (const size_t i : mem) units_[i].awaiting = false;
        if (u0.state == State::Closing) return enter_key(mem, host, frame, pose, kp);
        // state 1: the retry memo - an unchanged A (bit for bit) counts, past max_retries the approach is cancelled
        const bool same = same_bits(a, u0.memo);
        const uint32_t n = same ? u0.retries + 1 : 0;
        for (const size_t i : mem) {
            units_[i].memo = a;
            units_[i].retries = n;
        }
        if (same && n > cfg_.max_retries) return drop_key(mem, host, frame, pose);
        if (d3 <= cfg_.arrived_reach) {
            // state 2 and 0x443700: the host stops; 2-D excess e = |A - P| - r within Garrison_Radius -> enter, else a
            // flag-0x20 move of the whole excess toward A (the service enters on the way, its end enters)
            for (const size_t i : mem) units_[i].state = State::Closing;
            push(GarrisonEventKind::HostStop, u0, host, a, frame);
            const float dx = a.x - kp.x, dy = a.y - kp.y, d = dmath::sqrt(dx * dx + dy * dy);
            const float e = d - ai.radius;
            if (e <= h.d.radius || !(d > 0.0f)) return enter_key(mem, host, frame, pose, kp);
            const float k = e / d;
            push(GarrisonEventKind::CorrectiveMove, u0, host, Vec3{kp.x + dx * k, kp.y + dy * k, kp.z}, frame);
            for (const size_t i : mem) units_[i].awaiting = true;
            return;
        }
        if (ai.host_path) return drop_key(mem, host, frame, pose);  // "the host is moving" is read only here
        push(GarrisonEventKind::Reissue, u0, host, kp, frame);      // a new GarrisonEvent (flags 4): the host re-orders
        return;
    }
    // the service 0x443ff0, every frame in states 1 - 2: 3-D |A - P| - r <= Garrison_Radius -> entering
    if (d3 <= h.d.radius) enter_key(mem, host, frame, pose, kp);
}

void GarrisonSystem::service(uint32_t frame, const PoseFn& pose, const ApproachFn& approach) {
    std::vector<ObjectId> groups;  // s78n t18: the groups decided in this pass
    std::vector<size_t> mem;
    for (size_t idx = 0; idx < units_.size(); ++idx) {
        const Unit& u = units_[idx];
        if (!pending(u.state)) continue;
        mem.clear();
        ObjectId key = u.id;
        int32_t value = value_of(u);
        if (GarrisonGroup g; group_of(u, g)) {
            if (std::find(groups.begin(), groups.end(), g.id) != groups.end()) continue;
            groups.push_back(g.id);
            key = g.id;
            value = g.value > 0 ? g.value : 0;
            for (size_t i = idx; i < units_.size(); ++i) {
                const Unit& m = units_[i];
                GarrisonGroup mg;
                if (pending(m.state) && m.host == u.host && group_of(m, mg) && mg.id == g.id) mem.push_back(i);
            }
        } else {
            mem.push_back(idx);
        }
        service_key(mem, key, value, frame, pose, approach);
    }
}

std::vector<std::pair<ObjectId, ObjectId>> GarrisonSystem::occupancy() const {
    std::vector<std::pair<ObjectId, ObjectId>> out;
    for (const Holder& h : holders_)
        for (const ObjectId u : h.inside) out.emplace_back(h.id, u);
    return out;
}

void GarrisonSystem::hash(StateHasher& h) const {
    h.u32(static_cast<uint32_t>(holders_.size()));
    for (const Holder& x : holders_) {
        h.i32(x.id);
        h.i32(x.owner);
        if (thief_holder(x)) h.u8(static_cast<uint8_t>(x.d.kind));  // s78n t12 review: made mid-battle, not from the type
        h.u32(static_cast<uint32_t>(x.inside.size()));
        for (const ObjectId id : x.inside) h.i32(id);
    }
    h.u32(static_cast<uint32_t>(units_.size()));
    for (const Unit& u : units_) {
        h.i32(u.id);
        h.i32(u.owner);
        h.u8(static_cast<uint8_t>(u.state));
        h.i32(u.host);
        h.u32(u.enter_at);
        h.i32(u.bone);  // s78n t20 batch 2
        h.boolean(u.awaiting);
        h.f32(u.memo.x);
        h.f32(u.memo.y);
        h.f32(u.memo.z);
        h.u32(u.retries);
    }
}

}  // namespace alamo::tactical::garrison
