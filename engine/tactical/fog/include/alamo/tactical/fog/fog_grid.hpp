// Fog of war: one player's FOW grid (docs/re/FOG_OF_WAR.md §1, §2, §4; original FOWGrid, 0x58 bytes).
//
// Per cell: a STATE byte (0 = fogged for game logic; 1..15 fading; 16..238 seen recently, regrowing;
// 239..255 currently revealed), a u16 REVEAL COUNT (saturating at 0xFFFF, clamped at 0 on undo) and a
// FADE byte (frames into the 16-frame decay period at reveal; renderer only).
//
// Geometry (§1.2): W = ceil(extent / desired cell), H likewise, each clamped to [0, 512]; actual cell =
// extent / count. Cell of a point: clamp into [x0 + 0.001, x1 - 0.001] x [y0 + 0.001, y1 - 0.001];
// col = (int)((x - x0) / cellW), row = (int)((y1 - y) / cellH) (row 0 = max-y edge); index = row * W + col
// clamped to [0, N - 1].
//
// Reveal primitive (§2.2): filled midpoint circle of r = (int)(R / cellW + 0.5) cells (cellW on both axes)
// drawn into a span array of (clampedB - clampedA + 1) rows indexed by the UNCLAMPED row offset 0..2r;
// span i is drawn at absolute row clampedA + i. Top-edge quirk [H, kept]: when row - r < 0 the circle is
// shifted toward +row by (r - row) rows and its far part is cut. Near the bottom edge rows are cut.
//
// Determinism: no RNG, integer state; float math only in the cell / radius conversions (no FMA: the
// engine flags set /fp:precise, -ffp-contract=off). Grids are independent: per-player grids may be
// updated in parallel. Not part of the frame CRC; hash() is an opt-in test digest.
#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "alamo/core/bytes.hpp"

namespace alamo {
class StateHasher;
}

namespace alamo::tactical::fog {

inline constexpr int32_t kMaxCellsPerAxis = 512;
inline constexpr uint8_t kStateFogged = 0;
inline constexpr uint8_t kStateFirstReveal = 0xEF;  // 239: a never-seen / regrown cell when first counted
inline constexpr uint8_t kStateRevealed = 0xFF;
inline constexpr uint16_t kCountMax = 0xFFFF;
inline constexpr float kNoModelFootprint = 10.0f;  // footprint radius without a model; also the min sight radius

// Map box: x0 left, x1 right, y0 bottom, y1 top (world units).
struct FogBox {
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
};

using CellList = std::vector<uint32_t>;

// Reveal(grid, pos, radius, outList, fill, unmaskedOK, maskedOK, apply) argument flags (§2.1).
struct RevealFlags {
    bool fill = false;         // with apply: state := 0xFF over the circle, no counting, no list
    bool unmasked_ok = true;   // take cells whose dense mask byte is 0
    bool masked_ok = true;     // take cells whose dense mask byte is 1
    bool apply = true;         // count the cells (and list them); false: only list them
};

// Refog event: (player, FOW cell index) went from a nonzero state to 0 in a decay (§4.3).
using RefogFn = std::function<void(int32_t player, uint32_t cell)>;

// Regrow step of the decay (§4.2): (int)(238 / (fps * seconds / 16)). Shipped 30 fps, 6.0 s -> 21.
int32_t regrow_step(float fps, float regrow_seconds);
// The original's ceiling (0x22f9f0): (int)v + 1 when v > (float)(int)v.
int32_t fog_ceil(float v);

class FogGrid {
public:
    FogGrid() = default;

    // Geometry; all arrays 0. No arrays when a desired cell size is not > 0 or a count is 0.
    void init(const FogBox& box, float desired_cell_w, float desired_cell_h, int32_t player);
    bool valid() const { return n_ > 0; }
    int32_t player() const { return player_; }
    int32_t width() const { return w_; }
    int32_t height() const { return h_; }
    uint32_t cells() const { return n_; }
    float cell_w() const { return cw_; }
    float cell_h() const { return ch_; }
    const FogBox& box() const { return box_; }

    // §1.2 cell of a point (clamped point; col / row unclamped by the index clamp).
    void cell_of(float x, float y, int32_t& col, int32_t& row) const;
    uint32_t index_of(float x, float y) const;
    bool inside(float x, float y) const;                   // inside the box (inclusive)
    void cell_centre(uint32_t cell, float& x, float& y) const;
    int32_t radius_cells(float radius) const;              // (int)(R / cellW + 0.5), x86 truncation rules

    // Shared dense mask (W x H bytes owned by the FogOfWar), null = none.
    void set_dense_mask(const uint8_t* mask) { mask_ = mask; }
    const uint8_t* dense_mask() const { return mask_; }

    // ---- reveal primitives (§2) -------------------------------------------------------------------------------
    // `frame` = the mode frame F (fade byte of a newly counted cell).
    void reveal(float x, float y, float radius, CellList* out, const RevealFlags& f, uint32_t frame);
    // 0x4c0ec0: ordinary cells within rNormal, dense cells within rDense (one list, passes never overlap).
    void reveal_dense(float x, float y, float r_normal, float r_dense, CellList& out, uint32_t frame);
    // SP5: the cells reveal_dense would count, in the same order, without counting (const: safe in a parallel loop).
    // reveal_dense == dense_cells then count_cells of the list.
    void dense_cells(float x, float y, float r_normal, float r_dense, CellList& out) const;
    void collect_circle(float x, float y, float radius, CellList& out, bool unmasked_ok = true, bool masked_ok = true);
    void flash(float x, float y, float radius);            // state := 0xFF over the circle, counts untouched
    void count_cell(uint32_t cell, uint32_t frame);        // §2.3
    void count_cells(std::span<const uint32_t> cells, uint32_t frame);  // Lua reload 0x6a5120
    void undo(std::span<const uint32_t> cells);            // count -= 1 when > 0, no state change
    void reveal_all(uint32_t frame);                       // count += 1 everywhere, flag set
    void undo_reveal_all();                                // count -= 1 everywhere (clamped), flag cleared
    void reset_counts(std::span<const uint32_t> cells);    // 0x4c0d90
    void reset(bool keep_state);                           // counts 0; unless keep_state: state, fade 0
    // Setup phase (§3.4): this grid = the +8 player's, `other` = +4's (null: binarise alone).
    void binarise(FogGrid* other);

    // ---- decay (§4.2) -----------------------------------------------------------------------------------------
    // Refog callbacks in cell-index order. A null refog is allowed.
    void decay(uint32_t frame, int32_t step, const RefogFn& refog);

    // ---- state ------------------------------------------------------------------------------------------------
    std::span<const uint8_t> state() const { return state_; }
    std::span<const uint16_t> count() const { return count_; }
    std::span<const uint8_t> fade() const { return fade_; }
    uint8_t state_at(uint32_t cell) const { return state_[cell]; }
    uint16_t count_at(uint32_t cell) const { return count_[cell]; }
    uint8_t fade_at(uint32_t cell) const { return fade_[cell]; }
    // Game-logic test (0x4c0e00): fogged iff the point is outside the box or its cell's state is 0.
    bool fogged_at(float x, float y) const;
    uint32_t last_decay() const { return last_decay_; }    // 0 = never
    int32_t regrow() const { return step_; }               // -1 until the first decay
    bool reveal_all_active() const { return reveal_all_; }
    void hash(StateHasher& h) const;

    // N1 stage 2 save / load (contract: alamo/core/save_file.hpp; src/fog_save.cpp). Everything but the dense-mask
    // link: u32 version, i32 player, box (4 x f32), i32 W, i32 H, u32 N, f32 cell W / H, u32 last decay, i32 regrow
    // step, bool Reveal_All, the 256-byte decay table, then (N > 0) the state, fade, count-low-byte and
    // count-high-byte planes, each PackBits-packed (control c < 128: c + 1 literal bytes; c >= 128: the next byte
    // repeated c - 125 times). False (unchanged) on bad geometry or truncated / overlong packets. A successful read
    // clears the dense-mask link: FogOfWar::read re-links its mask.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    template <class CellFn>
    void circle(float x, float y, float radius, CellFn&& fn) const;
    bool mask_ok(uint32_t cell, bool unmasked_ok, bool masked_ok) const;

    std::vector<uint8_t> state_, fade_;
    std::vector<uint16_t> count_;
    const uint8_t* mask_ = nullptr;
    uint32_t n_ = 0;
    int32_t w_ = 0, h_ = 0;
    float cw_ = 0.0f, ch_ = 0.0f;
    FogBox box_;
    uint32_t last_decay_ = 0;
    int32_t player_ = -1;
    int32_t step_ = -1;
    bool reveal_all_ = false;
    uint8_t table_[256] = {};
};

}  // namespace alamo::tactical::fog
