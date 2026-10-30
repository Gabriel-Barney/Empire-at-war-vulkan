#include "alamo/ai/tactical/threat_grid.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "alamo/core/sim_crc.hpp"

namespace alamo::ai::tactical {

namespace {

// Truncation toward 0 of a float that may be far out of int range (clamped).
int32_t trunc_i(float v) {
    if (!(v > -2.0e9f)) return -2000000000;
    if (v >= 2.0e9f) return 2000000000;
    return static_cast<int32_t>(v);
}

bool is_grid_hard_point(const PerceptionHardPoint& hp) {
    return hp.type >= kFirstGridHardPointType && hp.type <= kLastGridHardPointType && hp.grid_flag;
}

float hard_point_power(const PerceptionWorld& w, const PerceptionObject& o, int32_t index) {
    if (index < 0) return o.combat_power;
    if (!w.hard_points) {
        if (static_cast<size_t>(index) < o.hard_points.size()) return o.hard_points[static_cast<size_t>(index)].power;
        return 0.0f;
    }
    thread_local std::vector<PerceptionHardPoint> tmp;
    w.object_hard_points(o, tmp);
    if (static_cast<size_t>(index) < tmp.size()) return tmp[static_cast<size_t>(index)].power;
    return 0.0f;
}

}  // namespace

ThreatGridConfig threat_grid_config(TacticalMode mode, const TacticalAiConstants& c) {
    ThreatGridConfig g;
    g.fog_cells_per_threat_cell = c.fog_cells_per_threat_cell;
    const bool space = mode == TacticalMode::Space;
    g.desired_fow_cell_size = space ? c.desired_space_fow_cell_size : c.desired_land_fow_cell_size;
    g.threat_range_cap = space ? c.space_threat_range_cap : c.land_threat_range_cap;
    g.threat_look_ahead_time = space ? c.space_threat_look_ahead_time : c.land_threat_look_ahead_time;
    g.ai_uses_fog_of_war = space ? c.ai_uses_fow_space : c.ai_uses_fow_land;
    return g;
}

void TacticalPerceptionGrid::init(TacticalMode mode, const Rect2& map, uint32_t players, const ThreatGridConfig& cfg) {
    *this = TacticalPerceptionGrid();
    mode_ = mode;
    cfg_ = cfg;
    players_ = players;
    left_ = map.min_x;
    right_ = map.max_x;
    top_ = map.max_y;
    bottom_ = map.min_y;
    const float ex = right_ - left_, ey = top_ - bottom_;
    auto fow_cells = [&](float extent) {
        if (!(cfg.desired_fow_cell_size > 0.0f) || !(extent > 0.0f)) return 0;
        const float n = std::floor(extent / cfg.desired_fow_cell_size + 0.5f);  // [M] rounding of 0x22f9f0
        if (!(n > 0.0f)) return 0;
        return n >= static_cast<float>(kMaxFowCellsPerAxis) ? kMaxFowCellsPerAxis : static_cast<int32_t>(n);
    };
    fow_x_ = fow_cells(ex);
    fow_y_ = fow_cells(ey);
    const int32_t k = std::max(1, cfg.fog_cells_per_threat_cell);
    nx_ = (fow_x_ + k - 1) / k;
    ny_ = (fow_y_ + k - 1) / k;
    if (nx_ <= 0 || ny_ <= 0) {
        nx_ = ny_ = 0;
        return;
    }
    cell_w_ = ex / static_cast<float>(nx_);
    cell_h_ = ey / static_cast<float>(ny_);
    half_diag_ = std::sqrt(cell_w_ * cell_w_ + cell_h_ * cell_h_) / 2.0f;
    const uint32_t cells = cell_count();
    live_.assign(cells, {});
    threat_.assign(players, std::vector<float>(cells, 0.0f));
    snapshot_.assign(players, std::vector<float>(cells, 0.0f));
    last_seen_.assign(players, std::vector<int32_t>(cells, 0));
    memory_.assign(players, std::vector<std::vector<uint32_t>>(cells));
    player_objects_.assign(players, {});
}

int32_t TacticalPerceptionGrid::cell_clamped(float x, float y) const {
    if (!valid()) return -1;
    const int32_t col = std::clamp(trunc_i((x - left_) / cell_w_), 0, nx_ - 1);
    const int32_t row = std::clamp(trunc_i((top_ - y) / cell_h_), 0, ny_ - 1);
    return row * nx_ + col;
}

int32_t TacticalPerceptionGrid::cell_strict(float x, float y) const {
    if (!valid()) return -1;
    if (!(x >= left_ && x <= right_ && y >= bottom_ && y <= top_)) return -1;
    return cell_clamped(x, y);
}

Vec3f TacticalPerceptionGrid::cell_center(int32_t col, int32_t row) const {
    return {left_ + (static_cast<float>(col) + 0.5f) * cell_w_, top_ - (static_cast<float>(row) + 0.5f) * cell_h_, 0.0f};
}

void TacticalPerceptionGrid::footprint(float gx, float gy, float r, int32_t& c0, int32_t& c1, int32_t& r0, int32_t& r1) const {
    c0 = std::max(0, trunc_i((gx - r) / cell_w_));
    c1 = std::min(nx_ - 1, trunc_i((gx + r) / cell_w_));
    r0 = std::max(0, trunc_i((gy - r) / cell_h_));
    r1 = std::min(ny_ - 1, trunc_i((gy + r) / cell_h_));
}

void TacticalPerceptionGrid::rect_cells(const Rect2& r, std::vector<uint32_t>& out) const {
    out.clear();
    if (!valid()) return;
    const int32_t c0 = std::max(0, trunc_i((r.min_x - left_) / cell_w_));
    const int32_t c1 = std::min(nx_ - 1, trunc_i((r.max_x - left_) / cell_w_));
    const int32_t r0 = std::max(0, trunc_i((top_ - r.max_y) / cell_h_));
    const int32_t r1 = std::min(ny_ - 1, trunc_i((top_ - r.min_y) / cell_h_));
    for (int32_t row = r0; row <= r1; ++row)
        for (int32_t col = c0; col <= c1; ++col) out.push_back(static_cast<uint32_t>(row * nx_ + col));
}

void TacticalPerceptionGrid::zone_cells(const HintZone& z, std::vector<uint32_t>& out) const {
    out.clear();
    if (!valid()) return;
    for (const uint32_t packed : z.cells) {
        const auto x = static_cast<int32_t>(packed & 0xFFFFu), y = static_cast<int32_t>(packed >> 16);
        if (x < nx_ && y < ny_) out.push_back(static_cast<uint32_t>(y * nx_ + x));
    }
}

uint32_t TacticalPerceptionGrid::alloc_entry() {
    if (!free_entries_.empty()) {
        const uint32_t i = free_entries_.back();
        free_entries_.pop_back();
        entries_[i] = Entry();
        entries_[i].in_use = true;
        return i;
    }
    entries_.emplace_back();
    entries_.back().in_use = true;
    return static_cast<uint32_t>(entries_.size() - 1);
}

void TacticalPerceptionGrid::refresh_entry(const PerceptionWorld&, const PerceptionObject& o, Entry& e,
                                           std::span<const PerceptionHardPoint> hps) const {
    float r = 0.0f;
    Vec3f p = o.position;
    bool destroyed = false;
    if (e.hard_point < 0) {
        r = o.max_attack_range;
    } else if (static_cast<size_t>(e.hard_point) < hps.size()) {
        const PerceptionHardPoint& hp = hps[static_cast<size_t>(e.hard_point)];
        if (hp.destroyable && hp.destroyed) {
            destroyed = true;
        } else {
            r = hp.weapon_range;
            if (hp.turret) p = hp.world_position;
        }
    } else {
        destroyed = true;  // hard point vanished [port]
    }
    if (destroyed) {
        r = -1.0f;
    } else {
        if (o.mobile) r = r + o.speed * cfg_.threat_look_ahead_time;
        r = std::max(r, half_diag_);
        r = std::min(r, cfg_.threat_range_cap);
    }
    e.radius = r;
    e.gx = p.x - left_;
    e.gy = top_ - p.y;
    e.area = 4.0f * r * r;
    e.center_cell = cell_clamped(p.x, p.y);
}

float TacticalPerceptionGrid::entry_power(const PerceptionObject& o, const Entry& e, std::span<const PerceptionHardPoint> hps) const {
    if (e.hard_point < 0) return o.combat_power;
    if (static_cast<size_t>(e.hard_point) < hps.size()) return hps[static_cast<size_t>(e.hard_point)].power;
    return 0.0f;
}

float TacticalPerceptionGrid::entry_power_now(const PerceptionWorld& w, const PerceptionObject& o, const Entry& e) const {
    return hard_point_power(w, o, e.hard_point);
}

void TacticalPerceptionGrid::add_entry(const PerceptionWorld& w, const PerceptionObject& o, uint32_t ei,
                                       std::span<const PerceptionHardPoint> hps) {
    Entry& e = entries_[ei];
    e.projected = false;
    if (e.radius < 0.0f) return;  // destroyed hard point: not projected
    footprint(e.gx, e.gy, e.radius, e.c0, e.c1, e.r0, e.r1);
    const float power = entry_power(o, e, hps);
    e.v = power * cell_w_ * cell_h_ / e.area;
    const auto git = objects_.find(e.object);
    e.affects_snapshot = git != objects_.end() && !git->second.team_or_hide;
    const int32_t owner = git != objects_.end() ? git->second.owner : kNoPlayerIndex;
    e.hostile.assign(players_, 0);
    for (uint32_t p = 0; p < players_; ++p)
        e.hostile[p] = (owner != kNoPlayerIndex && w.rel(owner, static_cast<int32_t>(p)) == 1) ? 1 : 0;
    for (int32_t row = e.r0; row <= e.r1; ++row) {
        for (int32_t col = e.c0; col <= e.c1; ++col) {
            const auto cell = static_cast<uint32_t>(row * nx_ + col);
            for (uint32_t p = 0; p < players_; ++p) {
                if (!e.hostile[p]) continue;
                threat_[p][cell] += e.v;
                if (e.affects_snapshot) snapshot_[p][cell] -= e.v;
            }
            live_[cell].push_back(ei);
        }
    }
    e.projected = true;
}

void TacticalPerceptionGrid::erase_value(std::vector<uint32_t>& v, uint32_t x) {
    v.erase(std::remove(v.begin(), v.end(), x), v.end());
}

void TacticalPerceptionGrid::remove_entry(uint32_t ei) {
    Entry& e = entries_[ei];
    if (!e.projected) return;
    for (int32_t row = e.r0; row <= e.r1; ++row) {
        for (int32_t col = e.c0; col <= e.c1; ++col) {
            const auto cell = static_cast<uint32_t>(row * nx_ + col);
            for (uint32_t p = 0; p < players_ && p < e.hostile.size(); ++p) {
                if (!e.hostile[p]) continue;
                threat_[p][cell] = std::max(0.0f, threat_[p][cell] - e.v);
                if (e.affects_snapshot) snapshot_[p][cell] = std::max(0.0f, snapshot_[p][cell] - e.v);
            }
            erase_value(live_[cell], ei);
        }
    }
    e.projected = false;
}

bool TacticalPerceptionGrid::register_object(const PerceptionWorld& w, int32_t id, uint32_t frame) {
    if (!valid() || objects_.count(id)) return false;
    const PerceptionObject* o = w.object(id);
    if (!o || !o->has_unit_ai) return false;
    std::vector<PerceptionHardPoint> hps;
    w.object_hard_points(*o, hps);
    GridObject g;
    g.owner = w.is_player(o->owner) ? o->owner : kNoPlayerIndex;
    std::memcpy(g.transform, o->transform, sizeof g.transform);
    g.next_check = frame + cfg_.reprojection_frames;
    g.fogged.assign(players_, 0);
    g.team_or_hide = o->team_or_hide_when_fogged;
    std::vector<int32_t> hp_index;
    for (size_t i = 0; i < hps.size(); ++i)
        if (is_grid_hard_point(hps[i])) hp_index.push_back(static_cast<int32_t>(i));
    if (hp_index.empty()) hp_index.push_back(-1);
    if (const PerceptionType* t = w.type_of(*o)) category_union_ |= t->category;
    auto& rec = objects_.emplace(id, std::move(g)).first->second;
    for (const int32_t hi : hp_index) {
        const uint32_t ei = alloc_entry();
        rec.entries.push_back(ei);
        Entry& e = entries_[ei];
        e.object = id;
        e.hard_point = hi;
        e.snap.assign(players_, EntrySnap());
        refresh_entry(w, *o, e, hps);
        add_entry(w, *o, ei, hps);
    }
    if (rec.owner != kNoPlayerIndex) player_objects_[static_cast<size_t>(rec.owner)].push_back(id);
    registration_order_.push_back(id);
    return true;
}

bool TacticalPerceptionGrid::fog_test(const PerceptionWorld& w, int32_t player, int32_t id) const {
    if (!w.is_player(player)) return false;
    if (!w.players[static_cast<size_t>(player)].is_human && !cfg_.ai_uses_fog_of_war) return false;
    return w.object_fogged ? w.object_fogged(player, id) : false;
}

void TacticalPerceptionGrid::remove_from_memory(int32_t player, uint32_t ei, int32_t c0, int32_t c1, int32_t r0, int32_t r1) {
    auto& mem = memory_[static_cast<size_t>(player)];
    for (int32_t row = r0; row <= r1; ++row)
        for (int32_t col = c0; col <= c1; ++col) erase_value(mem[static_cast<size_t>(row * nx_ + col)], ei);
}

bool TacticalPerceptionGrid::service_object(const PerceptionWorld& w, int32_t id, uint32_t frame) {
    const auto it = objects_.find(id);
    if (it == objects_.end()) return false;
    const PerceptionObject* o = w.object(id);
    if (!o) return false;
    GridObject& g = it->second;
    // fog bytes (every service)
    for (uint32_t p = 0; p < players_; ++p) {
        const uint8_t now = (w.fow_active && g.team_or_hide && fog_test(w, static_cast<int32_t>(p), id)) ? 1 : 0;
        if (now == g.fogged[p]) continue;
        for (const uint32_t ei : g.entries) {
            Entry& e = entries_[ei];
            EntrySnap& s = e.snap[p];
            if (now) {
                s = {e.radius, e.gx, e.gy, e.area, e.center_cell, true};
            } else if (s.valid) {
                int32_t c0, c1, r0, r1;
                footprint(s.gx, s.gy, s.radius, c0, c1, r0, r1);
                remove_from_memory(static_cast<int32_t>(p), ei, c0, c1, r0, r1);
            }
        }
        g.fogged[p] = now;
    }
    // re-projection gate
    if (frame < g.next_check) return false;
    g.next_check += cfg_.reprojection_frames;
    if (std::memcmp(g.transform, o->transform, sizeof g.transform) == 0) return false;
    std::memcpy(g.transform, o->transform, sizeof g.transform);
    std::vector<PerceptionHardPoint> hps;
    w.object_hard_points(*o, hps);
    for (const uint32_t ei : g.entries) {
        remove_entry(ei);
        refresh_entry(w, *o, entries_[ei], hps);
        add_entry(w, *o, ei, hps);
    }
    return true;
}

void TacticalPerceptionGrid::service_all(const PerceptionWorld& w, uint32_t frame) {
    const std::vector<int32_t> order = registration_order_;
    for (const int32_t id : order) service_object(w, id, frame);
}

bool TacticalPerceptionGrid::on_object_destroyed(int32_t id) {
    const auto it = objects_.find(id);
    if (it == objects_.end()) return false;
    GridObject& g = it->second;
    for (const uint32_t ei : g.entries) {
        remove_entry(ei);
        for (auto& per_player : memory_)
            for (auto& cell : per_player) erase_value(cell, ei);
        entries_[ei] = Entry();
        free_entries_.push_back(ei);
    }
    if (g.owner != kNoPlayerIndex) {
        auto& v = player_objects_[static_cast<size_t>(g.owner)];
        v.erase(std::remove(v.begin(), v.end(), id), v.end());
    }
    registration_order_.erase(std::remove(registration_order_.begin(), registration_order_.end(), id), registration_order_.end());
    objects_.erase(it);
    return true;
}

bool TacticalPerceptionGrid::on_owner_changed(const PerceptionWorld& w, int32_t id) {
    const auto it = objects_.find(id);
    const PerceptionObject* o = w.object(id);
    if (it == objects_.end() || !o) return false;
    GridObject& g = it->second;
    const int32_t owner = w.is_player(o->owner) ? o->owner : kNoPlayerIndex;
    if (owner == g.owner) return false;
    for (const uint32_t ei : g.entries) remove_entry(ei);
    if (g.owner != kNoPlayerIndex) {
        auto& v = player_objects_[static_cast<size_t>(g.owner)];
        v.erase(std::remove(v.begin(), v.end(), id), v.end());
    }
    g.owner = owner;
    if (owner != kNoPlayerIndex) player_objects_[static_cast<size_t>(owner)].push_back(id);
    std::vector<PerceptionHardPoint> hps;
    w.object_hard_points(*o, hps);
    for (const uint32_t ei : g.entries) add_entry(w, *o, ei, hps);
    return true;
}

void TacticalPerceptionGrid::refog(int32_t player, uint32_t cell, uint32_t frame) {
    if (player < 0 || static_cast<uint32_t>(player) >= players_ || cell >= cell_count()) return;
    const auto p = static_cast<size_t>(player);
    last_seen_[p][cell] = static_cast<int32_t>(frame);
    memory_[p][cell] = live_[cell];
    snapshot_[p][cell] = threat_[p][cell];
}

void TacticalPerceptionGrid::refog_at(int32_t player, float x, float y, uint32_t frame) {
    const int32_t c = cell_strict(x, y);
    if (c >= 0) refog(player, static_cast<uint32_t>(c), frame);
}

bool TacticalPerceptionGrid::fogged_for(int32_t id, int32_t player) const {
    const auto it = objects_.find(id);
    if (it == objects_.end() || player < 0 || static_cast<size_t>(player) >= it->second.fogged.size()) return false;
    return it->second.fogged[static_cast<size_t>(player)] != 0;
}

float TacticalPerceptionGrid::threat(int32_t player, uint32_t cell) const {
    if (player < 0 || static_cast<uint32_t>(player) >= players_ || cell >= cell_count()) return 0.0f;
    return threat_[static_cast<size_t>(player)][cell];
}

float TacticalPerceptionGrid::snapshot(int32_t player, uint32_t cell) const {
    if (player < 0 || static_cast<uint32_t>(player) >= players_ || cell >= cell_count()) return 0.0f;
    return snapshot_[static_cast<size_t>(player)][cell];
}

int32_t TacticalPerceptionGrid::last_seen(int32_t player, uint32_t cell) const {
    if (player < 0 || static_cast<uint32_t>(player) >= players_ || cell >= cell_count()) return 0;
    return last_seen_[static_cast<size_t>(player)][cell];
}

std::span<const int32_t> TacticalPerceptionGrid::player_objects(int32_t player) const {
    if (player < 0 || static_cast<size_t>(player) >= player_objects_.size()) return {};
    return player_objects_[static_cast<size_t>(player)];
}

std::vector<std::pair<int32_t, int32_t>> TacticalPerceptionGrid::live_entries(uint32_t cell) const {
    std::vector<std::pair<int32_t, int32_t>> out;
    if (cell >= cell_count()) return out;
    for (const uint32_t ei : live_[cell]) out.emplace_back(entries_[ei].object, entries_[ei].hard_point);
    return out;
}

std::vector<std::pair<int32_t, int32_t>> TacticalPerceptionGrid::memory_entries(int32_t player, uint32_t cell) const {
    std::vector<std::pair<int32_t, int32_t>> out;
    if (player < 0 || static_cast<uint32_t>(player) >= players_ || cell >= cell_count()) return out;
    for (const uint32_t ei : memory_[static_cast<size_t>(player)][cell]) out.emplace_back(entries_[ei].object, entries_[ei].hard_point);
    return out;
}

uint32_t TacticalPerceptionGrid::entry_count() const {
    uint32_t n = 0;
    for (const Entry& e : entries_) n += e.in_use ? 1u : 0u;
    return n;
}

bool TacticalPerceptionGrid::owner_passes(const PerceptionWorld& w, const PerceptionObject& o, const GridQuery& q) const {
    if (o.id == q.exclude) return false;
    const PerceptionType* t = w.type_of(o);
    if (!t || (t->category & q.category) == 0) return false;
    if (!w.is_player(o.owner)) return false;
    const PerceptionPlayerInfo& owner = w.players[static_cast<size_t>(o.owner)];
    if (owner.neutral_faction) return false;
    if (!q.faction.empty() && !iequal(owner.faction, q.faction)) return false;
    const bool allied = w.rel(o.owner, q.self) == 0;
    return q.ally ? allied : !allied;
}

float TacticalPerceptionGrid::cell_force(const PerceptionWorld& w, uint32_t cell, const GridQuery& q) const {
    if (!valid() || cell >= cell_count() || (q.category & category_union_) == 0) return 0.0f;
    float sum = 0.0f;
    auto term = [&](const Entry& e) {
        const PerceptionObject* o = w.object(e.object);
        if (!o || !owner_passes(w, *o, q)) return;
        const float power = entry_power_now(w, *o, e);
        sum += power / e.area * cell_w_ * cell_h_ * (1.0f - (1.0f - o->health) * q.attenuator);
    };
    for (const uint32_t ei : live_[cell]) {
        const Entry& e = entries_[ei];
        if (fogged_for(e.object, q.self)) continue;
        term(e);
    }
    if (q.self >= 0 && static_cast<uint32_t>(q.self) < players_) {
        for (const uint32_t ei : memory_[static_cast<size_t>(q.self)][cell]) {
            const Entry& e = entries_[ei];
            const bool fogged = fogged_for(e.object, q.self);
            if (q.memory_counted_when_not_fogged ? fogged : !fogged) continue;  // [M] quirk (open item 3a)
            term(e);
        }
    }
    return sum;
}

float TacticalPerceptionGrid::cells_force(const PerceptionWorld& w, std::span<const uint32_t> cells, const GridQuery& q) const {
    if ((q.category & category_union_) == 0) return 0.0f;
    float sum = 0.0f;
    for (const uint32_t c : cells) sum += cell_force(w, c, q);
    return sum;
}

float TacticalPerceptionGrid::cell_concentration(const PerceptionWorld& w, uint32_t cell, const GridQuery& q) const {
    if (!valid() || cell >= cell_count() || (q.category & category_union_) == 0) return 0.0f;
    float sum = 0.0f;
    for (const uint32_t ei : live_[cell]) {
        const Entry& e = entries_[ei];
        if (e.center_cell != static_cast<int32_t>(cell) || fogged_for(e.object, q.self)) continue;
        const PerceptionObject* o = w.object(e.object);
        if (o && owner_passes(w, *o, q)) sum += entry_power_now(w, *o, e);
    }
    if (q.self >= 0 && static_cast<uint32_t>(q.self) < players_) {
        for (const uint32_t ei : memory_[static_cast<size_t>(q.self)][cell]) {
            const Entry& e = entries_[ei];
            if (e.center_cell != static_cast<int32_t>(cell) || !fogged_for(e.object, q.self)) continue;
            const PerceptionObject* o = w.object(e.object);
            if (o && owner_passes(w, *o, q)) sum += entry_power_now(w, *o, e);
        }
    }
    return sum;
}

float TacticalPerceptionGrid::cells_concentration(const PerceptionWorld& w, std::span<const uint32_t> cells, const GridQuery& q) const {
    float sum = 0.0f;
    for (const uint32_t c : cells) sum += cell_concentration(w, c, q);
    return sum;
}

uint64_t TacticalPerceptionGrid::cells_bitfield(const PerceptionWorld& w, std::span<const uint32_t> cells, const GridQuery& q,
                                                std::span<const BitfieldMatcher> matchers) const {
    uint64_t bits = 0;
    if (!valid()) return 0;
    auto test = [&](const Entry& e) {
        const PerceptionObject* o = w.object(e.object);
        if (!o || !owner_passes(w, *o, q)) return;
        const PerceptionType* t = w.type_of(*o);
        for (size_t i = 0; i < matchers.size() && i < 64; ++i) {
            const BitfieldMatcher& m = matchers[i];
            const bool hit = m.type_name.empty() ? (t && (t->category & m.category) != 0) : (t && iequal(t->name, m.type_name));
            if (hit) bits |= uint64_t{1} << i;
        }
    };
    for (const uint32_t cell : cells) {
        if (cell >= cell_count()) continue;
        for (const uint32_t ei : live_[cell])
            if (!fogged_for(entries_[ei].object, q.self)) test(entries_[ei]);
        if (q.self >= 0 && static_cast<uint32_t>(q.self) < players_)
            for (const uint32_t ei : memory_[static_cast<size_t>(q.self)][cell])
                if (fogged_for(entries_[ei].object, q.self)) test(entries_[ei]);
    }
    return bits;
}

bool TacticalPerceptionGrid::cell_visible(const PerceptionWorld& w, int32_t player, uint32_t cell) const {
    if (w.is_player(player) && !w.players[static_cast<size_t>(player)].is_human && !cfg_.ai_uses_fog_of_war) return true;
    if (cell >= cell_count()) return false;
    for (const uint32_t ei : live_[cell])
        if (!fogged_for(entries_[ei].object, player)) return true;
    return false;
}

float TacticalPerceptionGrid::point_danger(const PerceptionWorld& w, int32_t player, float x, float y) const {
    if (player < 0 || static_cast<uint32_t>(player) >= players_) return 0.0f;
    const int32_t c = cell_strict(x, y);
    if (c < 0) return 0.0f;
    const auto cell = static_cast<uint32_t>(c);
    const float layer = cell_visible(w, player, cell) ? threat_[static_cast<size_t>(player)][cell] : snapshot_[static_cast<size_t>(player)][cell];
    const float s = mode_ == TacticalMode::Space ? kSpacePointDangerArea : kLandPointDangerArea;
    return layer * s / (cell_w_ * cell_h_);
}

float TacticalPerceptionGrid::cells_danger(const PerceptionWorld& w, int32_t player, std::span<const uint32_t> cells) const {
    if (player < 0 || static_cast<uint32_t>(player) >= players_) return 0.0f;
    float sum = 0.0f;
    for (const uint32_t cell : cells) {
        if (cell >= cell_count()) continue;
        sum += cell_visible(w, player, cell) ? threat_[static_cast<size_t>(player)][cell] : snapshot_[static_cast<size_t>(player)][cell];
    }
    return sum;
}

void TacticalPerceptionGrid::hash(StateHasher& h) const {
    h.u8(static_cast<uint8_t>(mode_));
    h.i32(fow_x_);
    h.i32(fow_y_);
    h.i32(nx_);
    h.i32(ny_);
    h.f32(left_);
    h.f32(top_);
    h.f32(cell_w_);
    h.f32(cell_h_);
    h.f32(half_diag_);
    h.u64(category_union_);
    h.u32(players_);
    for (uint32_t c = 0; c < cell_count(); ++c) {
        h.u32(static_cast<uint32_t>(live_[c].size()));
        for (const uint32_t ei : live_[c]) {
            h.i32(entries_[ei].object);
            h.i32(entries_[ei].hard_point);
        }
    }
    for (uint32_t p = 0; p < players_; ++p) {
        for (uint32_t c = 0; c < cell_count(); ++c) {
            h.f32(threat_[p][c]);
            h.f32(snapshot_[p][c]);
            h.i32(last_seen_[p][c]);
            h.u32(static_cast<uint32_t>(memory_[p][c].size()));
            for (const uint32_t ei : memory_[p][c]) {
                h.i32(entries_[ei].object);
                h.i32(entries_[ei].hard_point);
            }
        }
        h.u32(static_cast<uint32_t>(player_objects_[p].size()));
        for (const int32_t id : player_objects_[p]) h.i32(id);
    }
    h.u32(static_cast<uint32_t>(objects_.size()));
    for (const auto& [id, g] : objects_) {
        h.i32(id);
        h.i32(g.owner);
        h.u32(g.next_check);
        for (const uint8_t f : g.fogged) h.u8(f);
        for (const uint32_t ei : g.entries) {
            const Entry& e = entries_[ei];
            h.i32(e.hard_point);
            h.f32(e.radius);
            h.f32(e.gx);
            h.f32(e.gy);
            h.f32(e.area);
            h.i32(e.center_cell);
            h.boolean(e.projected);
            h.f32(e.v);
        }
    }
    h.u32(static_cast<uint32_t>(registration_order_.size()));
    for (const int32_t id : registration_order_) h.i32(id);
}

}  // namespace alamo::ai::tactical
