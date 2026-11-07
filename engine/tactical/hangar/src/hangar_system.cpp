#include "alamo/tactical/hangar/hangar_system.hpp"

#include <algorithm>

#include "alamo/core/ascii.hpp"
#include "alamo/core/dmath.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/tactical/abilities/ability_system.hpp"
#include "alamo/tactical/abilities/ability_types.hpp"

namespace alamo::tactical::hangar {

namespace {

bool parse_int(std::string_view s, int32_t& out) {
    s = ascii_trim(s);
    if (s.empty()) return false;
    bool neg = false;
    size_t i = 0;
    if (s[0] == '-' || s[0] == '+') {
        neg = s[0] == '-';
        i = 1;
    }
    if (i >= s.size()) return false;
    int64_t v = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
        if (v > 0x7fffffff) return false;
    }
    out = static_cast<int32_t>(neg ? -v : v);
    return true;
}

// XY-normalised facing (the convention of CombatSystem::local_to_world); (1, 0) when degenerate.
void unit_xy(Vec3 f, float& fx, float& fy) {
    fx = f.x;
    fy = f.y;
    const float len2 = fx * fx + fy * fy;
    if (len2 > 0.0f) {
        const float inv = 1.0f / dmath::sqrt(len2);
        fx = fx * inv;
        fy = fy * inv;
    } else {
        fx = 1.0f;
        fy = 0.0f;
    }
}

// Clamped to frame 0 (a negative delay would otherwise wrap).
uint32_t to_frame(int64_t f) { return f < 0 ? 0u : static_cast<uint32_t>(f); }

}  // namespace

bool parse_type_count(std::string_view text, std::string& type, int32_t& count) {
    text = ascii_trim(text);
    size_t cut = text.find(',');
    if (cut == std::string_view::npos) cut = text.find_last_of(" \t");
    if (cut == std::string_view::npos) return false;
    const std::string_view t = ascii_trim(text.substr(0, cut));
    if (t.empty() || !parse_int(text.substr(cut + 1), count)) return false;
    type.assign(t);
    return true;
}

SpawnList parse_spawn_list(std::span<const std::string_view> tokens, const std::function<int32_t(std::string_view)>& resolve) {
    SpawnList out;
    for (size_t i = 0; i < tokens.size(); ++i) {
        std::string t;
        int32_t n = 0;
        if (parse_type_count(tokens[i], t, n)) {
        } else if (i + 1 < tokens.size() && parse_int(tokens[i + 1], n)) {
            t.assign(ascii_trim(tokens[i]));
            ++i;
        } else {
            continue;
        }
        SpawnEntry e;
        e.type = resolve ? resolve(t) : -1;
        e.count = n;
        out.push_back(e);
    }
    return out;
}

int32_t tech_slot(const std::array<SpawnList, kTechSlots>& lists, int32_t tech) {
    if (tech < 0 || tech >= static_cast<int32_t>(kTechSlots)) return -1;  // tech > 5 gives no entries
    for (int32_t n = tech; n >= 0; --n)
        if (!lists[static_cast<size_t>(n)].empty()) return n;
    return -1;
}

HangarSystem::HangarSystem(combat::CombatSystem& combat, std::vector<HangarDesc> descs, HangarConfig config)
    : combat_(&combat), descs_(std::move(descs)), cfg_(config) {
    if (cfg_.fps == 0) cfg_.fps = combat::kDefaultFps;
    if (cfg_.service_interval == 0) cfg_.service_interval = 1;
}

int32_t HangarSystem::find(ObjectId id) const {
    const auto it = std::lower_bound(carriers_.begin(), carriers_.end(), id, [](const Carrier& c, ObjectId v) { return c.id < v; });
    return (it != carriers_.end() && it->id == id) ? static_cast<int32_t>(it - carriers_.begin()) : -1;
}
HangarSystem::Carrier* HangarSystem::carrier(ObjectId id) {
    const int32_t i = find(id);
    return i >= 0 ? &carriers_[static_cast<size_t>(i)] : nullptr;
}
const HangarSystem::Carrier* HangarSystem::carrier(ObjectId id) const {
    const int32_t i = find(id);
    return i >= 0 ? &carriers_[static_cast<size_t>(i)] : nullptr;
}

bool HangarSystem::allied(int32_t a, int32_t b) const {
    if (host_.allied) return host_.allied(a, b);
    return a == b || !combat_->is_enemy(a, b);
}

bool HangarSystem::has_deploy_squad(const Carrier& c) const {
    if (abilities_) return abilities_->has_ability(c.id, abilities::kDeploySquad);
    return descs_[c.desc].deploy_squad;
}

bool HangarSystem::defend_on(const Carrier& c) const {
    if (host_.defend_active) return host_.defend_active(c.id);
    return abilities_ && abilities_->is_active(c.id, abilities::kDefend);
}

// ---- objects ----

bool HangarSystem::add_carrier(ObjectId id, uint32_t hangar_desc, uint32_t frame, SimRng* rng) {
    if (hangar_desc >= descs_.size() || !combat_->has_unit(id) || find(id) >= 0) return false;
    Carrier c;
    c.id = id;
    c.desc = hangar_desc;
    c.owner = combat_->owner(id);
    c.spawn_player = c.owner;
    // First due = attach frame + synced Get_Int(0, interval - 1) (TACTICAL_GAPS §6.4).
    c.due = frame;
    if (rng && cfg_.service_interval >= 2)
        c.due = frame + static_cast<uint32_t>(rng->range_int(0, static_cast<int32_t>(cfg_.service_interval) - 1));
    c.next = frame;
    const auto it = std::lower_bound(carriers_.begin(), carriers_.end(), id, [](const Carrier& a, ObjectId v) { return a.id < v; });
    carriers_.insert(it, std::move(c));
    return true;
}

void HangarSystem::remove_carrier(ObjectId id) {
    const int32_t i = find(id);
    if (i >= 0) carriers_.erase(carriers_.begin() + i);
}

bool HangarSystem::bind_launch(uint32_t serial, ObjectId object) {
    for (Carrier& c : carriers_)
        for (Spawned& s : c.spawned)
            if (s.serial == serial && s.id == kNoObject) {
                s.id = object;
                return true;
            }
    return false;
}

void HangarSystem::squadron_removed(ObjectId object, uint32_t frame) {
    // 0x593760 (§2.7, space part).
    if (object == kNoObject) return;
    for (Carrier& c : carriers_) {
        const auto it = std::find_if(c.spawned.begin(), c.spawned.end(), [&](const Spawned& s) { return s.id == object; });
        if (it == c.spawned.end()) continue;
        const int32_t type = it->type;
        c.spawned.erase(it);
        // The first entry of the removed object's type; "full" over the entries up to and including it (all when none
        // matched), read before the decrement.
        int32_t matched = -1;
        for (size_t i = 0; i < c.entries.size(); ++i)
            if (c.entries[i].type == type) {
                matched = static_cast<int32_t>(i);
                break;
            }
        const size_t upto = matched >= 0 ? static_cast<size_t>(matched) + 1 : c.entries.size();
        bool full = true;
        for (size_t i = 0; i < upto; ++i)
            if (c.entries[i].live < c.entries[i].max) full = false;
        if (matched >= 0) c.entries[static_cast<size_t>(matched)].live -= 1;
        if (full && c.state == 0) {  // truncate first, then add
            const float d = descs_[c.desc].delay_seconds * static_cast<float>(cfg_.fps);
            c.next = to_frame(static_cast<int64_t>(static_cast<int32_t>(d)) + static_cast<int64_t>(frame));
        }
        events_.push_back(HangarEvent{HangarEventKind::SquadronLost, c.id, object, matched, frame});
        return;
    }
}

void HangarSystem::set_spawn_enabled(ObjectId id, bool on) {
    if (Carrier* c = carrier(id)) c->enabled = on ? 1 : 0;
}

void HangarSystem::deploy_squad(ObjectId id, uint32_t frame, float recharge_seconds) {
    Carrier* c = carrier(id);
    if (!c) return;
    c->state = 0;
    c->deploy_latch = 1;
    const float d = (recharge_seconds - 1.0f) * static_cast<float>(cfg_.fps);
    c->next = to_frame(static_cast<int64_t>(frame) + static_cast<int64_t>(static_cast<int32_t>(d)));
}

// ---- service ----

void HangarSystem::build(Carrier& c, uint32_t frame) {
    // 0x594b30 (§2.3): +0x60 := owner; one entry per starting type of the tech slot; next := now; bay list.
    const HangarDesc& d = descs_[c.desc];
    const int32_t tech = host_.tech_level ? host_.tech_level(c.owner) : 0;
    const int32_t ss = tech_slot(d.starting, tech);
    const int32_t rs = tech_slot(d.reserve, tech);
    c.spawn_player = c.owner;
    c.entries.clear();
    if (ss >= 0) {
        for (const SpawnEntry& e : d.starting[static_cast<size_t>(ss)]) {
            int32_t reserve = 0;  // no reserve entry of the type: start only
            if (rs >= 0)
                for (const SpawnEntry& r : d.reserve[static_cast<size_t>(rs)])
                    if (r.type == e.type) {
                        reserve = r.count;
                        break;
                    }
            Entry n;
            n.type = e.type;
            n.max = e.count;
            n.remaining = reserve >= 0 ? e.count + reserve : -1;
            c.entries.push_back(n);
        }
    }
    c.next = frame;
    c.bays.clear();
    const uint32_t n = combat_->hardpoint_count(c.id);
    for (uint32_t i = 0; i < n; ++i)
        if (combat_->hardpoint_kind(c.id, i) == combat::HardpointKind::FighterBay) c.bays.push_back(i);
}

bool HangarSystem::eligible(const Carrier& c, const Entry& e) const {
    (void)c;
    if (e.live >= e.max || e.remaining == 0) return false;
    return !(host_.planet_restricted && e.type >= 0 && host_.planet_restricted(e.type));
}

bool HangarSystem::can_spawn(Carrier& c, uint32_t frame, SimRng& rng) {
    // 0x593350 (§2.4), in order.
    if (host_.cinematic_camera && host_.cinematic_camera()) return false;              // 1
    if (host_.corruption_blocked && host_.corruption_blocked(c.owner)) return false;  // 2
    if (host_.in_hyperspace && host_.in_hyperspace(c.id)) return false;                // 3
    if (defend_on(c)) return false;                                                    // 4
    if (c.state != 0 || c.next > frame) return false;                                  // 5
    // 6 (space): erase destroyed bays (destroyable and health <= 0); disabled bays stay. Empty -> never again.
    const combat::CombatData& cd = combat_->data();
    const combat::UnitDesc& ud = cd.units[combat_->desc_index(c.id)];
    c.bays.erase(std::remove_if(c.bays.begin(), c.bays.end(),
                                [&](uint32_t i) {
                                    if (combat_->hardpoint_destroyed(c.id, i)) return true;
                                    const bool destroyable = i < ud.hardpoints.size() && cd.hardpoints[ud.hardpoints[i]].destroyable;
                                    return destroyable && !(combat_->hardpoint_health(c.id, i) > 0.0f);
                                }),
                 c.bays.end());
    if (c.bays.empty()) {
        if (!c.bays_gone) events_.push_back(HangarEvent{HangarEventKind::BaysGone, c.id, kNoObject, 0, frame});
        c.bays_gone = 1;
        return false;
    }
    // 7: entry draw (>= 2 entries), circular scan.
    if (c.enabled && !c.entries.empty()) {
        const size_t n = c.entries.size();
        const size_t r = n >= 2 ? static_cast<size_t>(rng.range_int(0, static_cast<int32_t>(n) - 1)) : 0;
        for (size_t k = 0; k < n; ++k)
            if (eligible(c, c.entries[(r + k) % n])) return true;
    }
    // 8: skirmish station: the first allied player (index order) with a pending space delivery.
    if (cfg_.skirmish && descs_[c.desc].station && host_.pending_delivery) {
        const auto players = static_cast<int32_t>(combat_->player_count());
        for (int32_t i = 0; i < players; ++i)
            if (allied(c.owner, i) && host_.pending_delivery(i, false) >= 0) {
                c.spawn_player = i;
                return true;
            }
    }
    return false;
}

void HangarSystem::emit(Carrier& c, int32_t type, int32_t entry, bool pending, uint32_t frame, SimRng& rng) {
    // 0x594dc0 (space): bay draw (>= 2 bays), P0 = bay bone, P1 = P0 + X axis x Fighter_Bay_Flyout_Distance.
    LaunchRequest q;
    q.serial = serial_++;
    q.carrier = c.id;
    q.owner = c.spawn_player;
    q.type = type;
    q.entry = entry;
    q.from_pending = pending;
    const size_t nb = c.bays.size();
    const size_t bi = nb >= 2 ? static_cast<size_t>(rng.range_int(0, static_cast<int32_t>(nb) - 1)) : 0;
    const uint32_t bay = c.bays[bi];
    q.bay = static_cast<int32_t>(bay);
    const combat::CombatData& cd = combat_->data();
    const combat::UnitDesc& ud = cd.units[combat_->desc_index(c.id)];
    float flyout = 0.0f;
    Vec3 local;
    if (bay < ud.hardpoints.size()) {
        const combat::HardpointDesc& hd = cd.hardpoints[ud.hardpoints[bay]];
        flyout = hd.fighter_bay_flyout_distance;
        local = hd.attach_offset;
    }
    Vec3 p0, x;
    if (!(host_.bay_bone && host_.bay_bone(c.id, bay, p0, x))) {  // [port] attachment offset, carrier facing
        const Vec3 pos = combat_->position(c.id);
        float fx = 1.0f, fy = 0.0f;
        unit_xy(combat_->facing(c.id), fx, fy);
        p0 = Vec3{pos.x + (local.x * fx - local.y * fy), pos.y + (local.x * fy + local.y * fx), pos.z + local.z};
        x = Vec3{fx, fy, 0.0f};
    }
    q.origin = p0;
    q.flyout = Vec3{p0.x + x.x * flyout, p0.y + x.y * flyout, p0.z + x.z * flyout};
    const Vec3 d{q.flyout.x - p0.x, q.flyout.y - p0.y, q.flyout.z - p0.z};
    const float len2 = d.x * d.x + d.y * d.y + d.z * d.z;
    if (len2 > 0.0f) {
        const float inv = 1.0f / dmath::sqrt(len2);
        q.direction = Vec3{d.x * inv, d.y * inv, d.z * inv};
        q.facing = q.direction;
    } else {
        const float xl2 = x.x * x.x + x.y * x.y + x.z * x.z;
        if (xl2 > 0.0f) {
            const float inv = 1.0f / dmath::sqrt(xl2);
            q.facing = Vec3{x.x * inv, x.y * inv, x.z * inv};
        }
    }
    q.escort = ud.has_locomotor;  // behaviour slot +0x333 (locomotor present): guard the carrier
    requests_.push_back(q);
    Spawned s;
    s.serial = q.serial;
    s.type = type;
    s.entry = entry;
    c.spawned.push_back(s);
    events_.push_back(HangarEvent{HangarEventKind::Launched, c.id, kNoObject, entry, frame});
}

void HangarSystem::launch(Carrier& c, uint32_t frame, SimRng& rng) {
    // §2.5 (space, state 0).
    const float df = static_cast<float>(frame) + static_cast<float>(cfg_.fps) * descs_[c.desc].delay_seconds;
    const uint32_t next = to_frame(static_cast<int64_t>(static_cast<int32_t>(df)));  // float sum, then truncate
    if (has_deploy_squad(c) && !combat_->ai_player(c.owner) && !c.deploy_latch) {
        c.state = 4;
        c.next = next;
        events_.push_back(HangarEvent{HangarEventKind::Manual, c.id, kNoObject, 0, frame});
        return;
    }
    c.deploy_latch = 0;
    bool spawned = false;
    if (c.enabled && !c.entries.empty()) {  // 0x594600: second entry draw, circular scan, first that spawns
        const size_t n = c.entries.size();
        const size_t r = n >= 2 ? static_cast<size_t>(rng.range_int(0, static_cast<int32_t>(n) - 1)) : 0;
        for (size_t k = 0; k < n && !spawned; ++k) {
            const size_t i = (r + k) % n;
            Entry& e = c.entries[i];
            if (!eligible(c, e) || e.type < 0) continue;  // [port] an unresolved type fails to spawn
            emit(c, e.type, static_cast<int32_t>(i), false, frame, rng);
            if (e.remaining > 0) {
                e.remaining -= 1;
                c.spawned.back().spent = 1;
            }
            e.live += 1;
            spawned = true;
            if (cfg_.skirmish && descs_[c.desc].community_property) {  // +0x60 round-robin among the owner's allies
                const auto players = static_cast<int32_t>(combat_->player_count());
                for (int32_t k2 = 1; k2 <= players; ++k2) {
                    const int32_t p = (c.spawn_player + k2) % players;
                    if (allied(c.owner, p)) {
                        c.spawn_player = p;
                        break;
                    }
                }
            }
        }
    }
    if (!spawned && cfg_.skirmish && host_.pending_delivery) {  // skirmish space: player +0x60's pending list
        const int32_t t = host_.pending_delivery(c.spawn_player, true);
        if (t >= 0) emit(c, t, -1, true, frame, rng);
    }
    c.next = next;
}

void HangarSystem::update(uint32_t frame, SimRng& rng) {
    // Dead carriers: tracking stops; spawned units stay.
    for (size_t i = 0; i < carriers_.size();) {
        if (!combat_->alive(carriers_[i].id)) {
            events_.push_back(HangarEvent{HangarEventKind::CarrierLost, carriers_[i].id, kNoObject, 0, frame});
            carriers_.erase(carriers_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    // Services, newest object first (descending id).
    for (size_t k = carriers_.size(); k-- > 0;) {
        Carrier& c = carriers_[k];
        if (c.due > frame) continue;
        c.due = frame + cfg_.service_interval;
        if (c.entries.empty()) build(c, frame);
        if (c.state == 0 && can_spawn(c, frame, rng)) launch(c, frame, rng);
    }
}

void HangarSystem::clear_requests() {
    for (Carrier& c : carriers_) {
        for (size_t i = 0; i < c.spawned.size();) {
            Spawned& s = c.spawned[i];
            if (s.id != kNoObject) {
                ++i;
                continue;
            }
            if (s.entry >= 0 && static_cast<size_t>(s.entry) < c.entries.size()) {  // the spawn failed: roll back
                Entry& e = c.entries[static_cast<size_t>(s.entry)];
                e.live -= 1;
                if (s.spent) e.remaining += 1;
            }
            c.spawned.erase(c.spawned.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    requests_.clear();
    events_.clear();
}

// ---- queries ----

bool HangarSystem::spawn_enabled(ObjectId id) const {
    const Carrier* c = carrier(id);
    return c && c->enabled;
}
bool HangarSystem::deploy_ready(ObjectId id) const {
    const Carrier* c = carrier(id);
    return c && c->state == 4;
}
uint8_t HangarSystem::state(ObjectId id) const {
    const Carrier* c = carrier(id);
    return c ? c->state : 0;
}
uint32_t HangarSystem::entry_count(ObjectId id) const {
    const Carrier* c = carrier(id);
    return c ? static_cast<uint32_t>(c->entries.size()) : 0u;
}
int32_t HangarSystem::live(ObjectId id, uint32_t entry) const {
    const Carrier* c = carrier(id);
    return (c && entry < c->entries.size()) ? c->entries[entry].live : 0;
}
int32_t HangarSystem::max_alive(ObjectId id, uint32_t entry) const {
    const Carrier* c = carrier(id);
    return (c && entry < c->entries.size()) ? c->entries[entry].max : 0;
}
int32_t HangarSystem::remaining(ObjectId id, uint32_t entry) const {
    const Carrier* c = carrier(id);
    return (c && entry < c->entries.size()) ? c->entries[entry].remaining : 0;
}
int32_t HangarSystem::entry_type(ObjectId id, uint32_t entry) const {
    const Carrier* c = carrier(id);
    return (c && entry < c->entries.size()) ? c->entries[entry].type : -1;
}
uint32_t HangarSystem::bay_count(ObjectId id) const {
    const Carrier* c = carrier(id);
    return c ? static_cast<uint32_t>(c->bays.size()) : 0u;
}
uint32_t HangarSystem::next_spawn_frame(ObjectId id) const {
    const Carrier* c = carrier(id);
    return c ? c->next : 0u;
}
uint32_t HangarSystem::next_service_frame(ObjectId id) const {
    const Carrier* c = carrier(id);
    return c ? c->due : 0u;
}
int32_t HangarSystem::spawn_player(ObjectId id) const {
    const Carrier* c = carrier(id);
    return c ? c->spawn_player : -1;
}
ObjectId HangarSystem::carrier_of(ObjectId object) const {
    if (object == kNoObject) return kNoObject;
    for (const Carrier& c : carriers_)
        for (const Spawned& s : c.spawned)
            if (s.id == object) return c.id;
    return kNoObject;
}

void HangarSystem::hash(StateHasher& h) const {
    h.u32(static_cast<uint32_t>(carriers_.size()));
    for (const Carrier& c : carriers_) {
        h.i32(c.id);
        h.u32(c.desc);
        h.i32(c.owner);
        h.i32(c.spawn_player);
        h.u32(c.due);
        h.u32(c.next);
        h.u8(c.state);
        h.u8(c.enabled);
        h.u8(c.deploy_latch);
        h.u32(static_cast<uint32_t>(c.bays.size()));
        for (const uint32_t b : c.bays) h.u32(b);
        h.u32(static_cast<uint32_t>(c.entries.size()));
        for (const Entry& e : c.entries) {
            h.i32(e.type);
            h.i32(e.live);
            h.i32(e.max);
            h.i32(e.remaining);
        }
        h.u32(static_cast<uint32_t>(c.spawned.size()));
        for (const Spawned& s : c.spawned) {
            h.i32(s.id);
            h.i32(s.type);
            h.i32(s.entry);
        }
    }
}

void HangarSystem::register_crc(CrcRegistry& registry, const std::string& prefix) const {
    registry.add(prefix + ".carriers", [this](StateHasher& h) { hash(h); });
}

bool complete_station_upgrade(combat::CombatSystem& combat, HangarSystem* hangars, ObjectId old_station,
                              ObjectId new_station, uint32_t /*frame*/) {
    if (!combat.has_unit(old_station) || !combat.has_unit(new_station) || old_station == new_station) return false;
    // Step 4 (0x70ec70): hardpoint state by index.
    if (!combat.copy_upgrade_hardpoints(old_station, new_station)) return false;
    if (hangars) {
        // Step 9: +0x3CB S -> S' (a non-spawner S counts as 1, the create default).
        if (hangars->has_carrier(new_station))
            hangars->set_spawn_enabled(new_station, !hangars->has_carrier(old_station) || hangars->spawn_enabled(old_station));
        // Step 10 (hangar part): S's spawned units stay alive, uncounted.
        hangars->remove_carrier(old_station);
    }
    return true;
}

}  // namespace alamo::tactical::hangar
