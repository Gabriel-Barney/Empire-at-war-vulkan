// N1 stage 2: save / load of the fog of war (contract: alamo/core/save_file.hpp). Layouts: fog_grid.hpp (FogGrid)
// and fog_of_war.hpp (FogOfWar). The fog is saved in full rather than rebuilt: fade timing, regrowing states, last
// decay frames and the ally-undo / binarise quirks are not recoverable from the sources.
// Every read parses into temporaries and commits only when everything parsed and validated (false = unchanged).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include "alamo/tactical/fog/fog_of_war.hpp"

namespace alamo::tactical::fog {

namespace {

constexpr uint32_t kFogGridSaveVersion = 1;
constexpr uint32_t kFogSaveVersion = 2;  // 2: SetupPhaseCountdownSeconds and the setup manager (FOWF t05)

// PackBits: control c < 128 -> c + 1 literal bytes; c >= 128 -> the next byte repeated c - 125 (3..130) times.
// The encoder is deterministic (runs of >= 3 become repeat packets), so write -> read -> write is byte-identical.
void pack(ByteWriter& w, std::span<const uint8_t> v) {
    const size_t n = v.size();
    size_t i = 0;
    while (i < n) {
        size_t run = 1;
        while (i + run < n && run < 130 && v[i + run] == v[i]) ++run;
        if (run >= 3) {
            w.u8(static_cast<uint8_t>(run + 125)).u8(v[i]);
            i += run;
            continue;
        }
        size_t j = i;
        while (j < n && j - i < 128) {
            if (j + 2 < n && v[j] == v[j + 1] && v[j] == v[j + 2]) break;
            ++j;
        }
        w.u8(static_cast<uint8_t>(j - i - 1)).raw(v.subspan(i, j - i));
        i = j;
    }
}

// Decodes exactly out.size() bytes; false on truncation or a packet running past the end.
bool unpack(ByteReader& r, std::span<uint8_t> out) {
    size_t o = 0;
    while (o < out.size()) {
        const uint8_t c = r.u8();
        if (!r.ok()) return false;
        if (c < 128) {
            const size_t len = static_cast<size_t>(c) + 1;
            if (len > out.size() - o) return false;
            const std::span<const uint8_t> d = r.raw(len);
            if (!r.ok()) return false;
            std::memcpy(out.data() + o, d.data(), len);
            o += len;
        } else {
            const size_t len = static_cast<size_t>(c) - 125;
            const uint8_t v = r.u8();
            if (!r.ok() || len > out.size() - o) return false;
            std::fill(out.begin() + static_cast<std::ptrdiff_t>(o), out.begin() + static_cast<std::ptrdiff_t>(o + len), v);
            o += len;
        }
    }
    return true;
}

}  // namespace

// ================================================================ FogGrid
void FogGrid::write(ByteWriter& w) const {
    w.u32(kFogGridSaveVersion);
    w.i32(player_).f32(box_.x0).f32(box_.y0).f32(box_.x1).f32(box_.y1);
    w.i32(w_).i32(h_).u32(n_).f32(cw_).f32(ch_).u32(last_decay_).i32(step_).boolean(reveal_all_);
    w.raw(std::span<const uint8_t>(table_, 256));
    if (n_ == 0) return;
    pack(w, state_);
    pack(w, fade_);
    std::vector<uint8_t> plane(n_);
    for (uint32_t i = 0; i < n_; ++i) plane[i] = static_cast<uint8_t>(count_[i] & 0xFFu);
    pack(w, plane);
    for (uint32_t i = 0; i < n_; ++i) plane[i] = static_cast<uint8_t>(count_[i] >> 8);
    pack(w, plane);
}

bool FogGrid::read(ByteReader& r) {
    if (r.u32() != kFogGridSaveVersion || !r.ok()) return false;
    FogGrid g;
    g.player_ = r.i32();
    g.box_.x0 = r.f32();
    g.box_.y0 = r.f32();
    g.box_.x1 = r.f32();
    g.box_.y1 = r.f32();
    g.w_ = r.i32();
    g.h_ = r.i32();
    g.n_ = r.u32();
    g.cw_ = r.f32();
    g.ch_ = r.f32();
    g.last_decay_ = r.u32();
    g.step_ = r.i32();
    g.reveal_all_ = r.boolean();
    const std::span<const uint8_t> table = r.raw(256);
    if (!r.ok()) return false;
    std::memcpy(g.table_, table.data(), 256);
    // init() geometry: W, H in [1, 512] with N = W x H, or no arrays at all (W = H = N = 0).
    const bool empty = g.w_ == 0 && g.h_ == 0 && g.n_ == 0;
    const bool sized = g.w_ > 0 && g.h_ > 0 && g.w_ <= kMaxCellsPerAxis && g.h_ <= kMaxCellsPerAxis &&
                       g.n_ == static_cast<uint32_t>(g.w_) * static_cast<uint32_t>(g.h_);
    if (!empty && !sized) return false;
    // A sized grid has a proper box and cell size (cell_of clamps into [x0 + 0.001, x1 - 0.001] x [y0 + 0.001,
    // y1 - 0.001]: the bounds must be ordered).
    const auto finite = [](float v) { return std::isfinite(v); };
    if (sized && !(finite(g.box_.x0) && finite(g.box_.x1) && finite(g.box_.y0) && finite(g.box_.y1) &&
                   g.box_.x0 + 0.001f <= g.box_.x1 - 0.001f && g.box_.y0 + 0.001f <= g.box_.y1 - 0.001f &&
                   finite(g.cw_) && finite(g.ch_) && g.cw_ > 0.0f && g.ch_ > 0.0f))
        return false;
    if (sized) {
        g.state_.assign(g.n_, 0);
        g.fade_.assign(g.n_, 0);
        g.count_.assign(g.n_, 0);
        std::vector<uint8_t> lo(g.n_), hi(g.n_);
        if (!unpack(r, g.state_) || !unpack(r, g.fade_) || !unpack(r, lo) || !unpack(r, hi)) return false;
        for (uint32_t i = 0; i < g.n_; ++i) g.count_[i] = static_cast<uint16_t>(lo[i] | (static_cast<uint32_t>(hi[i]) << 8));
    }
    *this = std::move(g);  // mask_ = null: FogOfWar::read re-links
    return true;
}

// ================================================================ FogOfWar
void FogOfWar::write(ByteWriter& w) const {
    w.u32(kFogSaveVersion);
    w.boolean(started_).u8(static_cast<uint8_t>(mode_));
    w.f32(k_.desired_land_cell_size).f32(k_.desired_space_cell_size).f32(k_.land_regrow_time).f32(k_.space_regrow_time);
    w.f32(k_.fps).boolean(k_.ai_uses_fog_land).boolean(k_.ai_uses_fog_space).f32(k_.tactical_overrun_multiple);
    w.f32(k_.min_overrun_seconds).boolean(k_.setup_phase_enabled).f32(k_.setup_countdown_seconds);
    w.u32(static_cast<uint32_t>(players_.size()));
    for (const FogPlayer& p : players_) w.i32(p.id).boolean(p.grid).boolean(p.human);
    w.u32(static_cast<uint32_t>(grids_.size()));
    for (const FogGrid& g : grids_) {
        ByteWriter s;
        g.write(s);
        w.u32(static_cast<uint32_t>(s.size())).raw(s.bytes());
    }
    w.i32(rel_n_).raw(relation_);
    w.u32(static_cast<uint32_t>(player_list_.size()));
    for (const std::vector<RevealEntry>& list : player_list_) {
        w.u32(static_cast<uint32_t>(list.size()));
        for (const RevealEntry& e : list) w.i32(e.category).u64(e.source).f32(e.value);
    }
    w.u32(static_cast<uint32_t>(player_bonus_.size()));
    for (const float b : player_bonus_) w.f32(b);
    w.u32(static_cast<uint32_t>(mask_.size()));
    pack(w, mask_);
    w.i32(scale_player_).f32(scale_);
    w.u32(static_cast<uint32_t>(sources_.size()));
    for (const Source& s : sources_) {
        w.i32(s.id).i32(s.owner).f32(s.x0).f32(s.y0).boolean(s.dirty).boolean(s.revealed);
        w.u32(static_cast<uint32_t>(s.cells.size()));
        for (const uint32_t c : s.cells) w.u32(c);
    }
    w.f32(threshold_).u32(frame_).i32(step_);
    w.boolean(fow_active_).boolean(render_option_).boolean(setup_).boolean(overrun4_).boolean(overrun8_);
    w.u32(static_cast<uint32_t>(setup_ready_.size())).raw(setup_ready_);
    w.u32(setup_count_start_).i32(setup_count_frames_).boolean(setup_armed_).boolean(setup_all_ready_);
}

bool FogOfWar::read(ByteReader& r) {
    if (r.u32() != kFogSaveVersion || !r.ok()) return false;
    const auto count = [&r](size_t min_bytes, uint32_t& n) {
        n = r.u32();
        return r.ok() && n <= r.remaining() / min_bytes;
    };
    const bool started = r.boolean();
    const uint8_t mode = r.u8();
    if (!r.ok() || (mode != static_cast<uint8_t>(FogMode::Land) && mode != static_cast<uint8_t>(FogMode::Space))) return false;
    FogConstants k;
    k.desired_land_cell_size = r.f32();
    k.desired_space_cell_size = r.f32();
    k.land_regrow_time = r.f32();
    k.space_regrow_time = r.f32();
    k.fps = r.f32();
    k.ai_uses_fog_land = r.boolean();
    k.ai_uses_fog_space = r.boolean();
    k.tactical_overrun_multiple = r.f32();
    k.min_overrun_seconds = r.f32();
    k.setup_phase_enabled = r.boolean();
    k.setup_countdown_seconds = r.f32();
    uint32_t n = 0;
    if (!count(6, n)) return false;
    std::vector<FogPlayer> players(n);
    for (FogPlayer& p : players) {
        p.id = r.i32();
        p.grid = r.boolean();
        p.human = r.boolean();
    }
    if (!count(4, n)) return false;
    std::vector<FogGrid> grids(n);
    for (FogGrid& g : grids) {
        const uint32_t len = r.u32();
        const std::span<const uint8_t> d = r.raw(len);
        if (!r.ok()) return false;
        ByteReader s(d);
        if (!g.read(s) || !s.ok() || !s.done()) return false;
    }
    const int32_t rel_n = r.i32();
    if (!r.ok() || rel_n < 0 || static_cast<uint64_t>(rel_n) * static_cast<uint64_t>(rel_n) > r.remaining()) return false;
    const std::span<const uint8_t> rel = r.raw(static_cast<size_t>(rel_n) * static_cast<size_t>(rel_n));
    std::vector<uint8_t> relation(rel.begin(), rel.end());
    if (!count(4, n)) return false;
    std::vector<std::vector<RevealEntry>> lists(n);
    for (std::vector<RevealEntry>& list : lists) {
        uint32_t m = 0;
        if (!count(16, m)) return false;
        list.resize(m);
        for (RevealEntry& e : list) {
            e.category = r.i32();
            e.source = r.u64();
            e.value = r.f32();
        }
    }
    if (!count(4, n)) return false;
    std::vector<float> bonus(n);
    for (float& b : bonus) b = r.f32();
    const uint32_t mask_n = r.u32();
    if (!r.ok() || mask_n > 512u * 512u) return false;
    std::vector<uint8_t> mask(mask_n);
    if (!unpack(r, mask)) return false;
    const int32_t scale_player = r.i32();
    const float scale = r.f32();
    if (!count(22, n)) return false;
    std::vector<Source> sources(n);
    for (size_t i = 0; i < sources.size(); ++i) {
        Source& s = sources[i];
        s.id = r.i32();
        s.owner = r.i32();
        s.x0 = r.f32();
        s.y0 = r.f32();
        s.dirty = r.boolean();
        s.revealed = r.boolean();
        uint32_t m = 0;
        if (!count(4, m) || (i > 0 && s.id <= sources[i - 1].id)) return false;
        s.cells.resize(m);
        for (uint32_t& c : s.cells) c = r.u32();
    }
    const float threshold = r.f32();
    const uint32_t frame = r.u32();
    const int32_t step = r.i32();
    const bool fow_active = r.boolean(), render_option = r.boolean(), setup = r.boolean();
    const bool overrun4 = r.boolean(), overrun8 = r.boolean();
    if (!count(1, n)) return false;
    const std::span<const uint8_t> ready_bytes = r.raw(n);
    std::vector<uint8_t> ready(ready_bytes.begin(), ready_bytes.end());
    const uint32_t count_start = r.u32();
    const int32_t count_frames = r.i32();
    const bool armed = r.boolean(), all_ready = r.boolean();
    if (!r.ok()) return false;
    // Consistency the code relies on: one reduced sum per player list (reduce_player_list writes it), the dense mask
    // sized like every valid grid (it is indexed by their cells).
    if (bonus.size() != lists.size()) return false;
    if (!mask.empty())
        for (const FogGrid& g : grids)
            if (g.valid() && g.cells() != mask.size()) return false;
    // Commit (the refog hook is kept).
    started_ = started;
    mode_ = static_cast<FogMode>(mode);
    k_ = k;
    players_ = std::move(players);
    grids_ = std::move(grids);
    rel_n_ = rel_n;
    relation_ = std::move(relation);
    player_list_ = std::move(lists);
    player_bonus_ = std::move(bonus);
    mask_ = std::move(mask);
    scale_player_ = scale_player;
    scale_ = scale;
    sources_ = std::move(sources);
    threshold_ = threshold;
    frame_ = frame;
    step_ = step;
    fow_active_ = fow_active;
    render_option_ = render_option;
    setup_ = setup;
    overrun4_ = overrun4;
    overrun8_ = overrun8;
    setup_ready_ = std::move(ready);
    setup_count_start_ = count_start;
    setup_count_frames_ = count_frames;
    setup_armed_ = armed;
    setup_all_ready_ = all_ready;
    // The dense-mask link (set_dense_obstacles: every valid grid points at the shared mask).
    for (FogGrid& g : grids_)
        if (g.valid()) g.set_dense_mask(mask_.empty() ? nullptr : mask_.data());
    return true;
}

}  // namespace alamo::tactical::fog
