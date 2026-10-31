#include "alamo/tactical/garrison/garrison.hpp"

#include <algorithm>

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

Vec3 GarrisonSystem::approach_point(ObjectId holder, Vec3 pos, Vec3 facing) const {
    const HolderDesc* d = holder_desc(holder);
    if (!d) return pos;
    float dx = 0.0f, dy = 0.0f;
    dir2(facing, dx, dy);
    const float r = d->enter_dist > 0.0f ? d->enter_dist : d->half_diagonal;
    return {pos.x + dx * r, pos.y + dy * r, pos.z};
}

Vec3 GarrisonSystem::exit_point(ObjectId holder, Vec3 pos, Vec3 facing) const {
    const HolderDesc* d = holder_desc(holder);
    if (!d) return pos;
    float dx = 0.0f, dy = 0.0f;
    dir2(facing, dx, dy);
    const float r = d->exit_dist > 0.0f ? d->exit_dist : 2.0f * d->half_diagonal;
    return {pos.x + dx * r, pos.y + dy * r, pos.z};
}

void GarrisonSystem::remove(ObjectId id, Vec3 pos, Vec3 facing, uint32_t frame) {
    if (const int32_t hi = holder_index(id); hi >= 0) {
        Holder& h = holders_[static_cast<size_t>(hi)];
        if (h.d.kind == HolderKind::Property) {  // no host behaviour: the occupants die with it
            const std::vector<ObjectId> inside = h.inside;
            for (const ObjectId uid : inside)
                if (const int32_t ui = unit_index(uid); ui >= 0) leave(units_[static_cast<size_t>(ui)], h, GarrisonEventKind::Killed, pos, frame);
        } else {
            eject_all(id, pos, facing, frame);
        }
        for (Unit& u : units_)
            if (u.host == id && u.state != State::Inside) {
                u.state = State::None;
                u.host = kNoObject;
                push(GarrisonEventKind::OrderDropped, u, id, pos, frame);
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
            if (!h.inside.empty()) {
                const std::vector<ObjectId> inside = h.inside;
                const Vec3 at = exit_point(id, pos, facing);
                for (const ObjectId uid : inside) {
                    const int32_t ui = unit_index(uid);
                    if (ui < 0) continue;
                    Unit& u = units_[static_cast<size_t>(ui)];
                    if (allied(u.owner, owner)) continue;
                    leave(u, holders_[static_cast<size_t>(hi)], GarrisonEventKind::Ejected, at, frame);
                }
            }
        }
    }
    if (const int32_t ui = unit_index(id); ui >= 0) units_[static_cast<size_t>(ui)].owner = owner;
}

bool GarrisonSystem::masks_match(ObjectId unit, ObjectId holder) const {
    const OccupantDesc* u = unit_desc(unit);
    const HolderDesc* h = holder_desc(holder);
    return u && h && mask_ok(u->category, h->accepts);
}

int32_t GarrisonSystem::reserved(ObjectId holder) const {
    int32_t n = 0;
    for (const Unit& u : units_) n += (u.host == holder && u.state == State::Moving) ? 1 : 0;
    return n;
}

bool GarrisonSystem::can_garrison(ObjectId unit, ObjectId holder) const {
    const int32_t ui = unit_index(unit), hi = holder_index(holder);
    if (ui < 0 || hi < 0 || unit == holder) return false;
    const Unit& u = units_[static_cast<size_t>(ui)];
    const Holder& h = holders_[static_cast<size_t>(hi)];
    if (u.state == State::Inside || u.state == State::Entering) return false;
    if (!host_open(h, u.owner) || !mask_ok(u.d.category, h.d.accepts)) return false;
    return free_slots(holder) - reserved(holder) - value_of(u) > 0;  // the stricter Lua rule (§2.5)
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
    int32_t n = 0;
    for (const ObjectId id : occupants(holder))
        if (const int32_t ui = unit_index(id); ui >= 0) n += value_of(units_[static_cast<size_t>(ui)]);
    return n;
}

int32_t GarrisonSystem::free_slots(ObjectId holder) const {
    const int32_t f = capacity(holder) - used_slots(holder);
    return f > 0 ? f : 0;
}

ObjectId GarrisonSystem::pending_holder(ObjectId unit) const {
    const int32_t ui = unit_index(unit);
    if (ui < 0) return kNoObject;
    const Unit& u = units_[static_cast<size_t>(ui)];
    return (u.state == State::Moving || u.state == State::Entering) ? u.host : kNoObject;
}

bool GarrisonSystem::entering(ObjectId unit) const {
    const int32_t ui = unit_index(unit);
    return ui >= 0 && units_[static_cast<size_t>(ui)].state == State::Entering;
}

bool GarrisonSystem::order_enter(ObjectId unit, ObjectId holder) {
    const int32_t ui = unit_index(unit), hi = holder_index(holder);
    if (ui < 0 || hi < 0 || unit == holder) return false;
    Unit& u = units_[static_cast<size_t>(ui)];
    const Holder& h = holders_[static_cast<size_t>(hi)];
    if (u.state == State::Inside || u.state == State::Entering) return false;
    if (!host_open(h, u.owner) || !mask_ok(u.d.category, h.d.accepts)) return false;
    u.state = State::Moving;
    u.host = holder;
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
    if (ui < 0 || units_[static_cast<size_t>(ui)].state != State::Moving) return false;
    units_[static_cast<size_t>(ui)].state = State::None;
    units_[static_cast<size_t>(ui)].host = kNoObject;
    return true;
}

bool GarrisonSystem::order_exit(ObjectId unit, Vec3 holder_pos, Vec3 holder_facing, uint32_t frame) {
    const int32_t ui = unit_index(unit);
    if (ui < 0) return false;
    Unit& u = units_[static_cast<size_t>(ui)];
    if (u.state == State::Moving || u.state == State::Entering) {  // cancel the approach
        const ObjectId host = u.host;
        u.state = State::None;
        u.host = kNoObject;
        push(GarrisonEventKind::OrderDropped, u, host, holder_pos, frame);
        return true;
    }
    const int32_t hi = u.state == State::Inside ? holder_index(u.host) : -1;
    if (hi < 0) return false;
    leave(u, holders_[static_cast<size_t>(hi)], GarrisonEventKind::Exited, exit_point(u.host, holder_pos, holder_facing), frame);
    return true;
}

uint32_t GarrisonSystem::eject_all(ObjectId holder, Vec3 holder_pos, Vec3 holder_facing, uint32_t frame) {
    const int32_t hi = holder_index(holder);
    if (hi < 0) return 0;
    Holder& h = holders_[static_cast<size_t>(hi)];
    const std::vector<ObjectId> inside = h.inside;  // entry order
    const Vec3 at = exit_point(holder, holder_pos, holder_facing);
    uint32_t n = 0;
    for (const ObjectId id : inside) {
        const int32_t ui = unit_index(id);
        if (ui < 0) continue;
        leave(units_[static_cast<size_t>(ui)], h, GarrisonEventKind::Ejected, at, frame);
        ++n;
    }
    return n;
}

void GarrisonSystem::service(uint32_t frame, const PoseFn& pose) {
    for (Unit& u : units_) {
        if (u.state != State::Moving && u.state != State::Entering) continue;
        const ObjectId host = u.host;
        const int32_t hi = holder_index(host);
        Vec3 up, uf, hp, hf;
        auto drop = [&] {
            u.state = State::None;
            u.host = kNoObject;
            push(GarrisonEventKind::OrderDropped, u, host, up, frame);
        };
        if (hi < 0 || !pose(u.id, up, uf) || !pose(host, hp, hf)) {
            drop();
            continue;
        }
        Holder& h = holders_[static_cast<size_t>(hi)];
        if (!host_open(h, u.owner) || free_slots(host) < value_of(u)) {
            drop();
            continue;
        }
        if (u.state == State::Moving) {
            const Vec3 a = approach_point(host, hp, hf);
            const float dx = up.x - a.x, dy = up.y - a.y;
            const float reach = (h.d.radius > cfg_.reach_slack ? h.d.radius : cfg_.reach_slack) + u.d.radius;
            if (dx * dx + dy * dy > reach * reach) continue;  // still walking
            u.state = State::Entering;
            u.enter_at = frame + cfg_.enter_frames;
            push(GarrisonEventKind::EnterStarted, u, host, up, frame);
            continue;
        }
        if (frame < u.enter_at) continue;
        u.state = State::Inside;
        h.inside.push_back(u.id);
        push(GarrisonEventKind::Entered, u, host, hp, frame);
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
    }
}

}  // namespace alamo::tactical::garrison
