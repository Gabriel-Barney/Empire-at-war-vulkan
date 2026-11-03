#include "alamo/tactical/movement/passability.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "alamo/core/hash.hpp"
#include "alamo/data/xml.hpp"

namespace alamo::tactical::movement {

namespace {

constexpr std::string_view kClassNames[kMovementClassCount] = {
    "Infantry", "InfantryNoShield", "Wheeled", "WheeledNoShield", "Walker", "WalkerNoShield",
    "Flying", "Tracked", "TrackedNoShield", "Hover", "HoverNoShield", "LargeWalker",
    "LargeWalkerNoShield", "Space", "Lancet", "Spotter", "LargeHover", "BombingRun",
};

struct BitName {
    std::string_view name;
    uint32_t bit;
};
constexpr BitName kBitNames[] = {
    {"Clear", kPassClear},           {"Steep", kPassSteep},   {"Obstacle", kPassObstacle},
    {"RebelWall", kPassRebelWall},   {"EmpireWall", kPassEmpireWall}, {"Water", kPassWater},
    {"Shield", kPassShield},         {"InfantryOnly", kPassInfantryOnly}, {"Impassable", kPassImpassable},
    {"None", 0},
};

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    return s;
}

}  // namespace

bool parse_passability_bits(std::string_view text, uint32_t& out) {
    uint32_t m = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t bar = text.find('|', pos);
        if (bar == std::string_view::npos) bar = text.size();
        const std::string_view tok = trim(text.substr(pos, bar - pos));
        if (!tok.empty()) {
            bool found = false;
            for (const BitName& b : kBitNames) {
                if (iequals(tok, b.name)) {
                    m |= b.bit;
                    found = true;
                    break;
                }
            }
            if (!found) return false;
        }
        pos = bar + 1;
    }
    out = m;
    return true;
}

MovementClassTable::MovementClassTable() {
    const uint32_t C = kPassClear, St = kPassSteep, O = kPassObstacle, R = kPassRebelWall, E = kPassEmpireWall,
                   W = kPassWater, S = kPassShield, I = kPassInfantryOnly, X = kPassImpassable;
    masks_ = {
        C | S | I,                      // Infantry
        C | I,                          // InfantryNoShield
        C | S,                          // Wheeled
        C,                              // WheeledNoShield
        C | S,                          // Walker
        C,                              // WalkerNoShield
        C | St | W | R | E | S | O | I, // Flying
        C | S,                          // Tracked
        C,                              // TrackedNoShield
        C | W | S,                      // Hover
        C | W,                          // HoverNoShield
        C | R | S,                      // LargeWalker
        C | R,                          // LargeWalkerNoShield
        C | St | O | W | R | E | S | I, // Space
        C | W | R | E | S | I | St,     // Lancet
        C | S | I | W,                  // Spotter
        C | E | S | W,                  // LargeHover
        C | St | O | W | R | E | S | I | X,  // BombingRun
    };
}

bool MovementClassTable::load_xml(std::string_view text) {
    const data::XmlDocument doc = data::XmlDocument::parse(text);
    if (!doc.ok()) return false;
    bool ok = true;
    for (data::XmlNode n : doc.root().children()) {
        for (uint32_t c = 0; c < kMovementClassCount; ++c) {
            if (!iequals(n.name(), kClassNames[c])) continue;
            uint32_t m = 0;
            if (parse_passability_bits(n.text_trimmed(), m)) masks_[c] = m;
            else ok = false;
        }
    }
    return ok;
}

uint32_t MovementClassTable::mask(int32_t movement_class) const {
    if (movement_class < 0 || static_cast<uint32_t>(movement_class) >= kMovementClassCount) return kPassClear;
    return masks_[static_cast<uint32_t>(movement_class)];
}

void MovementClassTable::set_mask(int32_t movement_class, uint32_t m) {
    if (movement_class < 0 || static_cast<uint32_t>(movement_class) >= kMovementClassCount) return;
    masks_[static_cast<uint32_t>(movement_class)] = m;
}

std::vector<uint32_t> MovementClassTable::distinct_masks() const {
    std::vector<uint32_t> v(masks_.begin(), masks_.end());
    v.push_back(kPassClear);
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

ObstacleFootprint make_footprint(V2 position, float yaw_deg, float obstacle_width, float obstacle_height,
                                 float obstacle_x_offset, float obstacle_y_offset, float model_half_x,
                                 float model_half_y, float scale, uint32_t bits) {
    ObstacleFootprint f;
    f.position = position;
    f.yaw_deg = yaw_deg;
    const float hx = std::fabs(model_half_x) * scale, hy = std::fabs(model_half_y) * scale;
    f.width = obstacle_width > 0.0f ? obstacle_width : 2.0f * hx;
    f.height = obstacle_height > 0.0f ? obstacle_height : 2.0f * hy;
    // Offsets: the XML field when the width/height came from XML, else -half extent (§1.3).
    f.x_offset = obstacle_width > 0.0f ? obstacle_x_offset : -hx;
    f.y_offset = obstacle_height > 0.0f ? obstacle_y_offset : -hy;
    f.bits = bits != 0 ? bits : kPassObstacle;
    return f;
}

// ---------------------------------------------------------------- grid
void PassabilityGrid::init(uint32_t width, uint32_t height) {
    w_ = width;
    h_ = height;
    const size_t n = static_cast<size_t>(width) * height;
    base_.assign(n, kPassClear);
    eff_.assign(n, kPassClear);
    forced_.assign(n, 0);
    counts_.clear();
    stamps_.clear();
    secondary_.clear();
    ++version_;
}

bool PassabilityGrid::set_base(std::span<const uint32_t> row_major) {
    if (row_major.size() != base_.size()) return false;
    std::copy(row_major.begin(), row_major.end(), base_.begin());
    recompute_all();
    return true;
}

void PassabilityGrid::set_base(int32_t x, int32_t y, uint32_t v) {
    if (!in_range(x, y)) return;
    base_[idx(x, y)] = v;
    recompute(idx(x, y));
}

void PassabilityGrid::set_playable_box(V2 min_xy, V2 max_xy) {
    for (uint32_t y = 0; y < h_; ++y) {
        for (uint32_t x = 0; x < w_; ++x) {
            const V2 c = cell_center(static_cast<int32_t>(x), static_cast<int32_t>(y));
            const bool out = c.x < min_xy.x || c.x > max_xy.x || c.y < min_xy.y || c.y > max_xy.y;
            forced_[static_cast<size_t>(y) * w_ + x] = out ? 1 : 0;
        }
    }
    recompute_all();
}

void PassabilityGrid::clear_playable_box() {
    std::fill(forced_.begin(), forced_.end(), uint8_t{0});
    recompute_all();
}

void PassabilityGrid::recompute(size_t i) {
    uint32_t v = base_[i];
    if (forced_[i]) v |= kPassObstacle;
    if (!counts_.empty()) {
        const auto& c = counts_[i];
        for (uint32_t b = 0; b < 9; ++b)
            if (c[b] != 0) v |= (1u << b);
    }
    if ((v & ~kPassClear) != 0) v &= ~kPassClear;
    if (v != eff_[i]) {
        eff_[i] = v;
        ++version_;
    }
}

void PassabilityGrid::recompute_all() {
    for (size_t i = 0; i < eff_.size(); ++i) recompute(i);
    ++version_;
}

void PassabilityGrid::sample_cells(const ObstacleFootprint& fp, std::vector<uint32_t>& out) const {
    out.clear();
    // S8 t13 (MOVEMENT §1.3): a zero width or height (no XML size, no model) samples nothing.
    if (!(fp.width > 0.0f) || !(fp.height > 0.0f)) return;
    const V2 dir = dir_from_deg(fp.yaw_deg);
    const V2 left = perp_left(dir);
    // Samples every 10 world units on both axes, both edges included (§1.3).
    const uint32_t nx = static_cast<uint32_t>(std::ceil(std::max(0.0f, fp.width) / 10.0f));
    const uint32_t ny = static_cast<uint32_t>(std::ceil(std::max(0.0f, fp.height) / 10.0f));
    for (uint32_t j = 0; j <= ny; ++j) {
        const float ly = fp.y_offset + std::min(static_cast<float>(j) * 10.0f, fp.height);
        for (uint32_t i = 0; i <= nx; ++i) {
            const float lx = fp.x_offset + std::min(static_cast<float>(i) * 10.0f, fp.width);
            const V2 p = fp.position + dir * lx + left * ly;
            const int32_t cx = world_to_cell(p.x), cy = world_to_cell(p.y);
            if (!in_range(cx, cy)) continue;
            const size_t ci = idx(cx, cy);
            // Water rule: a submerged low object does not block.
            if (has_water_ && (base_[ci] & kPassWater) != 0 && !(water_height_ + 5.0f <= fp.terrain_z + fp.obstacle_height))
                continue;
            // OI tactical: Fine_Tune_Occupied_Passability - the sample's vertical ray must hit the collision mesh.
            if (fp.fine_tune && !fp.fine_tune(p)) continue;
            out.push_back(static_cast<uint32_t>(ci));
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

void PassabilityGrid::apply(const Stamp& s, int delta) {
    for (uint32_t ci : s.cells) {
        for (uint32_t b = 0; b < 9; ++b) {
            if (!(s.bits & (1u << b))) continue;
            if (delta > 0) ++counts_[ci][b];
            else if (counts_[ci][b] > 0) --counts_[ci][b];
        }
        recompute(ci);
    }
}

uint32_t PassabilityGrid::stamp(ObjectId id, const ObstacleFootprint& fp) {
    unstamp(id);
    Stamp s;
    s.bits = fp.bits & 0x1FFu;
    if (w_ != 0 && h_ != 0) {  // S8 t09: an empty grid still records the (cell-less) stamp, as the header promises
        if (counts_.empty()) counts_.assign(base_.size(), std::array<uint16_t, 9>{});
        sample_cells(fp, s.cells);
        apply(s, 1);
    }
    const uint32_t n = static_cast<uint32_t>(s.cells.size());
    stamps_.emplace(id, std::move(s));
    return n;
}

std::vector<uint32_t> PassabilityGrid::stamp_all(std::span<const std::pair<ObjectId, ObstacleFootprint>> list, const ParallelFor& pf) {
    std::vector<Stamp> sampled(list.size());
    const bool cells = w_ != 0 && h_ != 0;
    if (cells)
        pf(static_cast<uint32_t>(list.size()), 1, [&](uint32_t begin, uint32_t end) {
            for (uint32_t i = begin; i < end; ++i) sample_cells(list[i].second, sampled[i].cells);
        });
    std::vector<uint32_t> out(list.size());
    for (size_t i = 0; i < list.size(); ++i) {
        const auto& [id, fp] = list[i];
        unstamp(id);
        Stamp& s = sampled[i];
        s.bits = fp.bits & 0x1FFu;
        if (cells) {
            if (counts_.empty()) counts_.assign(base_.size(), std::array<uint16_t, 9>{});
            apply(s, 1);
        }
        out[i] = static_cast<uint32_t>(s.cells.size());
        stamps_.emplace(id, std::move(s));
    }
    return out;
}

bool PassabilityGrid::unstamp(ObjectId id) {
    auto it = stamps_.find(id);
    if (it == stamps_.end()) return false;
    apply(it->second, -1);
    stamps_.erase(it);
    return true;
}

std::vector<ObjectId> PassabilityGrid::stamped_ids() const {
    std::vector<ObjectId> out;
    out.reserve(stamps_.size());
    for (const auto& kv : stamps_) out.push_back(kv.first);
    return out;
}

bool PassabilityGrid::occupies(ObjectId id, int32_t x, int32_t y) const {
    if (!in_range(x, y)) return false;
    const auto it = stamps_.find(id);
    if (it == stamps_.end()) return false;
    return std::binary_search(it->second.cells.begin(), it->second.cells.end(), static_cast<uint32_t>(idx(x, y)));
}

std::span<const uint32_t> PassabilityGrid::stamp_cells(ObjectId id) const {
    const auto it = stamps_.find(id);
    if (it == stamps_.end()) return {};
    return it->second.cells;
}

bool PassabilityGrid::rekey(ObjectId from, ObjectId to) {
    if (from == to || stamps_.find(to) != stamps_.end()) return false;
    auto node = stamps_.extract(from);
    if (node.empty()) return false;
    node.key() = to;
    stamps_.insert(std::move(node));
    return true;
}

uint32_t PassabilityGrid::stamp_secondary(ObjectId id, const ObstacleFootprint& fp) {
    unstamp_secondary(id);
    Stamp s;
    s.bits = fp.bits & 0x1FFu;  // SecondaryOccupationPassability (no Obstacle default for secondary occupants)
    if (w_ != 0 && h_ != 0) {   // S8 t09: recorded without cells on an empty grid, as stamp()
        if (counts_.empty()) counts_.assign(base_.size(), std::array<uint16_t, 9>{});
        sample_cells(fp, s.cells);
        apply(s, 1);
    }
    const uint32_t n = static_cast<uint32_t>(s.cells.size());
    secondary_.emplace(id, std::move(s));
    return n;
}

uint32_t PassabilityGrid::stamp_secondary_circle(ObjectId id, V2 center, float radius, uint32_t bits) {
    unstamp_secondary(id);
    if (w_ == 0 || h_ == 0 || !(radius >= 0.0f)) return 0;
    if (counts_.empty()) counts_.assign(base_.size(), std::array<uint16_t, 9>{});
    Stamp s;
    s.bits = bits & 0x1FFu;
    const int32_t x0 = world_to_cell(center.x - radius), x1 = world_to_cell(center.x + radius);
    const int32_t y0 = world_to_cell(center.y - radius), y1 = world_to_cell(center.y + radius);
    for (int32_t y = y0; y <= y1; ++y)
        for (int32_t x = x0; x <= x1; ++x)
            if (in_range(x, y) && length_sq(cell_center(x, y) - center) <= radius * radius) s.cells.push_back(static_cast<uint32_t>(idx(x, y)));
    std::sort(s.cells.begin(), s.cells.end());
    apply(s, 1);
    const uint32_t n = static_cast<uint32_t>(s.cells.size());
    secondary_.emplace(id, std::move(s));
    return n;
}

bool PassabilityGrid::unstamp_secondary(ObjectId id) {
    auto it = secondary_.find(id);
    if (it == secondary_.end()) return false;
    apply(it->second, -1);
    secondary_.erase(it);
    return true;
}
void PassabilityGrid::hash(StateHasher& h) const {
    h.u32(w_);
    h.u32(h_);
    h.span(eff_);
    h.u32(static_cast<uint32_t>(stamps_.size()));
    for (const auto& [id, s] : stamps_) {
        h.i32(id);
        h.u32(s.bits);
        h.u32(static_cast<uint32_t>(s.cells.size()));
        h.span(s.cells);
    }
    if (!secondary_.empty()) {  // OI tactical
        h.u32(static_cast<uint32_t>(secondary_.size()));
        for (const auto& [id, s] : secondary_) {
            h.i32(id);
            h.u32(s.bits);
            h.u32(static_cast<uint32_t>(s.cells.size()));
            h.span(s.cells);
        }
    }
}

}  // namespace alamo::tactical::movement
