#include "alamo/tactical/fog/fog_grid.hpp"

#include <algorithm>

#include "alamo/core/sim_crc.hpp"

namespace alamo::tactical::fog {

namespace {

constexpr float kInset = 0.001f;  // point clamp inset (§1.2)

// x86 cvttss2si: truncation, out of range / NaN -> INT32_MIN.
int32_t trunc_i32(float v) {
    if (!(v > -2147483648.0f && v < 2147483648.0f)) return INT32_MIN;
    return static_cast<int32_t>(v);
}

}  // namespace

int32_t fog_ceil(float v) {
    const int32_t i = trunc_i32(v);
    return v > static_cast<float>(i) ? i + 1 : i;
}

int32_t regrow_step(float fps, float regrow_seconds) {
    const float periods = fps * regrow_seconds / 16.0f;
    return trunc_i32(238.0f / periods);
}

void FogGrid::init(const FogBox& box, float desired_cell_w, float desired_cell_h, int32_t player) {
    box_ = box;
    player_ = player;
    state_.clear();
    fade_.clear();
    count_.clear();
    n_ = 0;
    w_ = h_ = 0;
    cw_ = ch_ = 0.0f;
    last_decay_ = 0;
    step_ = -1;
    reveal_all_ = false;
    if (!(desired_cell_w > 0.0f) || !(desired_cell_h > 0.0f)) return;
    w_ = std::clamp(fog_ceil((box.x1 - box.x0) / desired_cell_w), 0, kMaxCellsPerAxis);
    h_ = std::clamp(fog_ceil((box.y1 - box.y0) / desired_cell_h), 0, kMaxCellsPerAxis);
    if (w_ <= 0 || h_ <= 0) {
        w_ = h_ = 0;
        return;
    }
    cw_ = (box.x1 - box.x0) / static_cast<float>(w_);
    ch_ = (box.y1 - box.y0) / static_cast<float>(h_);
    n_ = static_cast<uint32_t>(w_) * static_cast<uint32_t>(h_);
    state_.assign(n_, 0);
    fade_.assign(n_, 0);
    count_.assign(n_, 0);
}

void FogGrid::cell_of(float x, float y, int32_t& col, int32_t& row) const {
    const float cx = std::clamp(x, box_.x0 + kInset, box_.x1 - kInset);
    const float cy = std::clamp(y, box_.y0 + kInset, box_.y1 - kInset);
    col = trunc_i32((cx - box_.x0) / cw_);
    row = trunc_i32((box_.y1 - cy) / ch_);
}

uint32_t FogGrid::index_of(float x, float y) const {
    if (n_ == 0) return 0;
    int32_t col = 0, row = 0;
    cell_of(x, y, col, row);
    const int64_t i = static_cast<int64_t>(row) * w_ + col;
    return static_cast<uint32_t>(std::clamp<int64_t>(i, 0, static_cast<int64_t>(n_) - 1));
}

bool FogGrid::inside(float x, float y) const { return x >= box_.x0 && x <= box_.x1 && y >= box_.y0 && y <= box_.y1; }

void FogGrid::cell_centre(uint32_t cell, float& x, float& y) const {
    const uint32_t uw = static_cast<uint32_t>(w_ > 0 ? w_ : 1);
    const uint32_t col = cell % uw, row = cell / uw;
    x = box_.x0 + (static_cast<float>(col) + 0.5f) * cw_;
    y = box_.y1 - (static_cast<float>(row) + 0.5f) * ch_;
}

int32_t FogGrid::radius_cells(float radius) const { return trunc_i32(radius / cw_ + 0.5f); }

bool FogGrid::mask_ok(uint32_t cell, bool unmasked_ok, bool masked_ok) const {
    const bool dense = mask_ && mask_[cell] != 0;
    return dense ? masked_ok : unmasked_ok;
}

template <class CellFn>
void FogGrid::circle(float x, float y, float radius, CellFn&& fn) const {
    if (n_ == 0) return;
    int32_t col = 0, row = 0;
    cell_of(x, y, col, row);
    const int64_t r = radius_cells(radius);
    const int64_t ca = std::clamp<int64_t>(row - r, 0, h_ - 1);
    const int64_t cb = std::clamp<int64_t>(row + r, 0, h_ - 1);
    const int64_t n = cb - ca + 1;
    if (n <= 0) return;
    int32_t mn[kMaxCellsPerAxis], mx[kMaxCellsPerAxis];
    for (int64_t i = 0; i < n; ++i) {
        mn[i] = w_ - 1;
        mx[i] = 0;
    }
    // Span index = the unclamped row offset k (0..2r); writes outside the array are dropped.
    auto widen = [&](int64_t k, int64_t c) {
        if (k < 0 || k >= n) return;
        const auto cc = static_cast<int32_t>(std::clamp<int64_t>(c, -1, w_));  // monotone pre-clamp, same result
        mn[k] = std::min(mn[k], cc);
        mx[k] = std::max(mx[k], cc);
    };
    auto plot8 = [&](int64_t px, int64_t py) {
        widen(r + py, col + px);
        widen(r + py, col - px);
        widen(r + px, col + py);
        widen(r + px, col - py);
        widen(r - px, col + py);
        widen(r - px, col - py);
        widen(r - py, col + px);
        widen(r - py, col - px);
    };
    if (r <= 0) {
        plot8(0, 0);
    } else {
        int64_t px = 0, py = r, d = 1 - r, de = 3, dse = 5 - 2 * r;
        plot8(0, r);
        while (px < py) {
            const int64_t old = d;
            if (old >= 0) --py;
            ++px;
            plot8(px, py);
            d += old >= 0 ? dse : de;
            dse += old < 0 ? 2 : 4;
            de += 2;
        }
    }
    for (int64_t i = 0; i < n; ++i) {
        const int64_t abs_row = ca + i;
        const int64_t a = std::max<int64_t>(mn[i], 0), b = std::min<int64_t>(mx[i], w_ - 1);
        for (int64_t c = a; c <= b; ++c) fn(static_cast<uint32_t>(abs_row * w_ + c));
    }
}

void FogGrid::reveal(float x, float y, float radius, CellList* out, const RevealFlags& f, uint32_t frame) {
    circle(x, y, radius, [&](uint32_t i) {
        if (!mask_ok(i, f.unmasked_ok, f.masked_ok)) return;
        if (f.apply && f.fill) {
            state_[i] = kStateRevealed;
            return;
        }
        if (f.apply) count_cell(i, frame);
        if (out) out->push_back(i);
    });
}

void FogGrid::reveal_dense(float x, float y, float r_normal, float r_dense, CellList& out, uint32_t frame) {
    if (r_normal > r_dense) {
        reveal(x, y, r_normal, &out, RevealFlags{false, true, false, true}, frame);
        reveal(x, y, r_dense, &out, RevealFlags{false, false, true, true}, frame);
    } else {
        reveal(x, y, r_dense, &out, RevealFlags{false, true, true, true}, frame);
    }
}

void FogGrid::collect_circle(float x, float y, float radius, CellList& out, bool unmasked_ok, bool masked_ok) {
    reveal(x, y, radius, &out, RevealFlags{false, unmasked_ok, masked_ok, false}, 0);
}

void FogGrid::flash(float x, float y, float radius) { reveal(x, y, radius, nullptr, RevealFlags{true, true, true, true}, 0); }

void FogGrid::count_cell(uint32_t i, uint32_t frame) {
    if (i >= n_) return;
    if (count_[i] == 0) {
        const uint8_t s = state_[i];
        if (s < 16) {
            state_[i] = s == 0 ? kStateFirstReveal : static_cast<uint8_t>(255 - s);
            int64_t f = 0;
            if (last_decay_ != 0) f = std::clamp<int64_t>(static_cast<int64_t>(frame) - static_cast<int64_t>(last_decay_) - 1, 0, 15);
            fade_[i] = static_cast<uint8_t>(f);
        } else if (s < 239) {
            state_[i] = kStateRevealed;
            fade_[i] = 0;
        }
    }
    if (count_[i] != kCountMax) ++count_[i];
}

void FogGrid::count_cells(std::span<const uint32_t> cells, uint32_t frame) {
    for (uint32_t c : cells) count_cell(c, frame);
}

void FogGrid::undo(std::span<const uint32_t> cells) {
    for (uint32_t c : cells)
        if (c < n_ && count_[c] > 0) --count_[c];
}

void FogGrid::reveal_all(uint32_t frame) {
    for (uint32_t i = 0; i < n_; ++i) count_cell(i, frame);
    reveal_all_ = true;
}

void FogGrid::undo_reveal_all() {
    for (uint32_t i = 0; i < n_; ++i)
        if (count_[i] > 0) --count_[i];
    reveal_all_ = false;
}

void FogGrid::reset_counts(std::span<const uint32_t> cells) {
    for (uint32_t c : cells)
        if (c < n_) count_[c] = 0;
}

void FogGrid::reset(bool keep_state) {
    std::fill(count_.begin(), count_.end(), uint16_t{0});
    if (!keep_state) {
        std::fill(state_.begin(), state_.end(), uint8_t{0});
        std::fill(fade_.begin(), fade_.end(), uint8_t{0});
    }
}

void FogGrid::binarise(FogGrid* other) {
    if (other && other->n_ != n_) other = nullptr;
    // 0x4c1830 [H]: every fade byte of both grids := 0; uncounted cells of the +4 grid elsewhere are fogged too.
    for (uint32_t i = 0; i < n_; ++i) {
        const bool seen = count_[i] > 0;
        state_[i] = seen ? kStateRevealed : kStateFogged;
        fade_[i] = 0;
        if (!other) continue;
        other->fade_[i] = 0;
        if (seen) {
            other->state_[i] = kStateFogged;
            other->count_[i] = 0;
        } else if (other->count_[i] > 0) {
            other->state_[i] = kStateRevealed;
            other->count_[i] = 1;
        } else {
            other->state_[i] = kStateFogged;
        }
    }
}

void FogGrid::decay(uint32_t frame, int32_t step, const RefogFn& refog) {
    if (step != step_) {
        step_ = step;
        table_[0] = 0;
        for (int32_t i = 1; i <= 238; ++i) table_[i] = static_cast<uint8_t>(std::max(0, i - step));
        for (int32_t i = 239; i <= 255; ++i) table_[i] = 238;
    }
    for (uint32_t i = 0; i < n_; ++i) {
        if (count_[i] == 0) {
            const uint8_t s = state_[i];
            const uint8_t ns = table_[s];
            state_[i] = ns;
            if (s != 0 && ns == 0 && refog) refog(player_, i);
        } else {
            state_[i] = static_cast<uint8_t>(255 - fade_[i]);
            fade_[i] = 0;
        }
    }
    last_decay_ = frame;
}

bool FogGrid::fogged_at(float x, float y) const {
    if (n_ == 0) return false;
    if (!inside(x, y)) return true;
    return state_[index_of(x, y)] == 0;
}

void FogGrid::hash(StateHasher& h) const {
    h.i32(player_);
    h.i32(w_);
    h.i32(h_);
    h.u32(n_);
    if (n_ > 0) {
        h.bytes(state_.data(), state_.size());
        h.bytes(fade_.data(), fade_.size());
        for (uint16_t c : count_) h.u16(c);
    }
    h.u32(last_decay_);
    h.i32(step_);
    h.boolean(reveal_all_);
}

}  // namespace alamo::tactical::fog
