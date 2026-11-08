// Fog of war tests (docs/re/FOG_OF_WAR.md): hand-computed rules for the grid, the reveal circle (incl. the
// top-edge quirk), counting, decay cadence, dense mask, REVEAL sources, queries, Lua semantics, setup phase,
// overrun, and order independence.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/tactical/fog/fog_of_war.hpp"
#include "alamo/test/support.hpp"

using namespace alamo::tactical::fog;

namespace {

using ::alamo::test::g_fail;
using ::alamo::test::g_checks;

// 20 x 20 land grid (cell 15) over [0, 300]^2: centre of (col, row).
float cx_of(int32_t col) { return 15.0f * static_cast<float>(col) + 7.5f; }
float cy_of(int32_t row) { return 300.0f - (15.0f * static_cast<float>(row) + 7.5f); }

FogGrid land_grid() {
    FogGrid g;
    g.init({0.0f, 0.0f, 300.0f, 300.0f}, 15.0f, 15.0f, 0);
    return g;
}

// Row -> (min col, max col, count) of a cell list on a 20-wide grid.
struct RowSpan {
    int32_t lo = 999, hi = -1, n = 0;
};
std::map<int32_t, RowSpan> rows_of(const CellList& cells, int32_t w = 20) {
    std::map<int32_t, RowSpan> m;
    for (uint32_t c : cells) {
        RowSpan& r = m[static_cast<int32_t>(c) / w];
        const int32_t col = static_cast<int32_t>(c) % w;
        r.lo = std::min(r.lo, col);
        r.hi = std::max(r.hi, col);
        ++r.n;
    }
    return m;
}

FogSetup land_setup() {
    FogSetup s;
    s.mode = FogMode::Land;
    s.box = {0.0f, 0.0f, 300.0f, 300.0f};
    s.players = {{0, true, true}, {1, true, false}, {2, true, true}, {3, false, false}};
    return s;
}

FogObject unit(int32_t owner, float x, float y, float sight) {
    FogObject o;
    o.owner = owner;
    o.x = x;
    o.y = y;
    o.sight_range = sight;
    return o;
}

uint32_t count_sum(const FogGrid& g) {
    uint32_t s = 0;
    for (uint16_t c : g.count()) s += c;
    return s;
}

// ---- geometry ------------------------------------------------------------------------------------------------

void test_geometry() {
    CHECK(fog_ceil(20.0f) == 20 && fog_ceil(20.5f) == 21 && fog_ceil(0.0f) == 0);
    CHECK(regrow_step(30.0f, 6.0f) == 21);
    FogGrid g;
    g.init({0.0f, 0.0f, 301.0f, 150.0f}, 15.0f, 15.0f, 4);
    CHECK(g.width() == 21 && g.height() == 10 && g.cells() == 210u && g.player() == 4);
    CHECK(g.cell_w() == 301.0f / 21.0f && g.cell_h() == 15.0f);
    g.init({0.0f, 0.0f, 10000.0f, 30.0f}, 15.0f, 15.0f, 0);
    CHECK(g.width() == 512 && g.height() == 2);
    g.init({0.0f, 0.0f, 300.0f, 300.0f}, 0.0f, 15.0f, 0);
    CHECK(!g.valid());
    FogGrid l = land_grid();
    int32_t c = 0, r = 0;
    l.cell_of(0.0f, 300.0f, c, r);
    CHECK(c == 0 && r == 0);
    l.cell_of(299.9f, 0.5f, c, r);
    CHECK(c == 19 && r == 19);
    l.cell_of(-50.0f, 400.0f, c, r);  // clamped into the inset box
    CHECK(c == 0 && r == 0);
    CHECK(l.index_of(1000.0f, -1000.0f) == 399u);
    CHECK(l.index_of(cx_of(3), cy_of(5)) == 5u * 20u + 3u);
    float x = 0.0f, y = 0.0f;
    l.cell_centre(5u * 20u + 3u, x, y);
    CHECK(x == cx_of(3) && y == cy_of(5));
    CHECK(l.radius_cells(0.0f) == 0 && l.radius_cells(7.4f) == 0 && l.radius_cells(7.5f) == 1 && l.radius_cells(30.0f) == 2);
    CHECK(l.radius_cells(1.0e12f) == INT32_MIN);  // x86 truncation of an out-of-range value
}

// ---- circle --------------------------------------------------------------------------------------------------

void test_circle() {
    FogGrid g = land_grid();
    CellList cells;
    g.collect_circle(cx_of(10), cy_of(10), 0.0f, cells);
    CHECK(cells.size() == 1 && cells[0] == 210u);
    cells.clear();
    g.collect_circle(cx_of(10), cy_of(10), 15.0f, cells);  // r = 1: plus
    CHECK((cells == CellList{190u, 209u, 210u, 211u, 230u}));
    cells.clear();
    g.collect_circle(cx_of(10), cy_of(10), 30.0f, cells);  // r = 2: 3,5,5,5,3
    auto m = rows_of(cells);
    CHECK(cells.size() == 21 && m.size() == 5);
    CHECK(m[8].n == 3 && m[9].n == 5 && m[10].n == 5 && m[11].n == 5 && m[12].n == 3);
    CHECK(m[8].lo == 9 && m[8].hi == 11 && m[10].lo == 8 && m[10].hi == 12);
    CHECK(std::is_sorted(cells.begin(), cells.end()));
    // r = 3 midpoint: widths 3,5,7,7,7,5,3 (octant points (0,3),(1,3),(2,2)).
    cells.clear();
    g.collect_circle(cx_of(10), cy_of(10), 45.0f, cells);
    m = rows_of(cells);
    CHECK(m.size() == 7 && m[7].n == 3 && m[8].n == 5 && m[9].n == 7 && m[10].n == 7 && m[11].n == 7 && m[12].n == 5 && m[13].n == 3);
    // Top-edge quirk: centre row 0, r = 2 -> spans 0..2 (3,5,5) drawn at rows 0..2 (the circle's TOP part,
    // shifted down by 2 rows), not the clipped lower half (5,5,3).
    cells.clear();
    g.collect_circle(cx_of(10), cy_of(0), 30.0f, cells);
    m = rows_of(cells);
    CHECK(m.size() == 3 && m[0].n == 3 && m[1].n == 5 && m[2].n == 5 && m[0].lo == 9);
    // Centre row 1, r = 2: rows 0..3 with widths 3,5,5,5 (shifted by 1).
    cells.clear();
    g.collect_circle(cx_of(10), cy_of(1), 30.0f, cells);
    m = rows_of(cells);
    CHECK(m.size() == 4 && m[0].n == 3 && m[1].n == 5 && m[2].n == 5 && m[3].n == 5);
    // Bottom edge: simply cut (rows 17..19 = 3,5,5).
    cells.clear();
    g.collect_circle(cx_of(10), cy_of(19), 30.0f, cells);
    m = rows_of(cells);
    CHECK(m.size() == 3 && m[17].n == 3 && m[18].n == 5 && m[19].n == 5);
    // Left edge: columns clamped (2,3,3,3,2).
    cells.clear();
    g.collect_circle(cx_of(0), cy_of(10), 30.0f, cells);
    m = rows_of(cells);
    CHECK(m[8].n == 2 && m[9].n == 3 && m[10].n == 3 && m[11].n == 3 && m[12].n == 2 && m[10].lo == 0);
    // A huge radius in the top-left corner reveals the circle's narrow top sliver only (quirk).
    cells.clear();
    g.collect_circle(cx_of(0), cy_of(0), 15.0f * 100.0f, cells);
    m = rows_of(cells);
    CHECK(m.size() == 20 && m[0].lo == 0 && m[0].n < 20 && m[0].n < m[19].n && cells.size() < 400u);
}

// ---- counting ------------------------------------------------------------------------------------------------

void test_counts() {
    FogGrid g = land_grid();
    const uint32_t c = 210u;
    g.count_cell(c, 5);
    CHECK(g.state_at(c) == 0xEF && g.count_at(c) == 1 && g.fade_at(c) == 0);  // never decayed -> fade 0
    g.count_cell(c, 5);
    CHECK(g.state_at(c) == 0xEF && g.count_at(c) == 2);
    const uint32_t one[] = {c};
    g.undo(one);
    g.undo(one);
    g.undo(one);
    CHECK(g.count_at(c) == 0 && g.state_at(c) == 0xEF);  // undo never touches the state
    g.decay(32, 21, nullptr);
    CHECK(g.state_at(c) == 238 && g.last_decay() == 32 && g.regrow() == 21);
    g.count_cell(c, 33);  // 16..238 -> 255, fade 0
    CHECK(g.state_at(c) == 0xFF && g.fade_at(c) == 0);
    g.undo(one);
    for (int i = 0; i < 12; ++i) g.decay(48 + 16 * static_cast<uint32_t>(i), 21, nullptr);  // 238 -> ... -> 7
    CHECK(g.state_at(c) == 7);
    g.count_cell(c, 48 + 16 * 11 + 3);  // 1..15 -> 255 - s; fade = F - last - 1
    CHECK(g.state_at(c) == 248 && g.fade_at(c) == 2);
    // Fade clamps to [0, 15]; the next decay with count > 0 sets 255 - fade.
    const uint32_t d = 0u;
    g.count_cell(d, 300);
    CHECK(g.state_at(d) == 0xEF && g.fade_at(d) == 15);
    g.decay(304, 21, nullptr);
    CHECK(g.state_at(d) == 240 && g.fade_at(d) == 0);
    const uint32_t e = 1u;
    g.count_cell(e, 304);  // same frame as the last decay: clamp(-1) = 0
    CHECK(g.fade_at(e) == 0);
    // Saturation.
    for (int i = 0; i < 70000; ++i) g.count_cell(5u, 400);
    CHECK(g.count_at(5u) == 0xFFFF);
    // Binarise (0x4c1830, FOG_OF_WAR §3.4 [H]): every fade byte of both grids := 0; the other grid's uncounted cells
    // elsewhere fogged, its counted ones 0xFF / count 1.
    FogGrid bo = land_grid();
    bo.count_cell(7u, 300);  // fade 15 on the other grid's counted cell
    bo.count_cell(8u, 300);  // ... and on a cell this grid sees
    const uint32_t eight[] = {8u};
    bo.undo(eight);          // uncounted, state 0xEF, fade 15
    g.binarise(&bo);
    CHECK(g.fade_at(c) == 0 && g.state_at(c) == 0xFF && g.fade_at(d) == 0);
    CHECK(bo.state_at(7u) == 0xFF && bo.count_at(7u) == 1 && bo.fade_at(7u) == 0);
    CHECK(bo.state_at(8u) == 0 && bo.fade_at(8u) == 0);
    // Flash: state 255, counts untouched.
    FogGrid f = land_grid();
    f.flash(cx_of(10), cy_of(10), 15.0f);
    CHECK(f.state_at(210u) == 0xFF && f.state_at(190u) == 0xFF && f.count_at(210u) == 0 && f.state_at(170u) == 0);
    f.decay(16, 21, nullptr);
    CHECK(f.state_at(210u) == 238);
    // Reveal_All / undo.
    f.reveal_all(20);
    CHECK(f.reveal_all_active() && count_sum(f) == 400u && f.state_at(0) == 0xEF && f.state_at(210u) == 0xFF);
    f.undo_reveal_all();
    CHECK(!f.reveal_all_active() && count_sum(f) == 0u);
    // Reset.
    f.reveal_all(20);
    f.reset(true);
    CHECK(count_sum(f) == 0u && f.state_at(0) == 0xEF);
    f.reset(false);
    CHECK(f.state_at(0) == 0);
}

// ---- decay cadence -------------------------------------------------------------------------------------------

void test_decay_window() {
    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    for (uint32_t phase = 0; phase < 16; ++phase) {
        FogOfWar fog;
        fog.start(land_setup());
        std::map<uint32_t, int> refogs;
        std::vector<uint32_t> order;
        fog.set_refog([&](int32_t p, uint32_t cell) {
            if (p == 0) {
                ++refogs[cell];
                order.push_back(cell);
            }
        });
        const uint32_t drop = 40 + phase;
        uint32_t fogged_at = 0;
        const float px = cx_of(10), py = cy_of(10);
        for (uint32_t f = 1; f < 400 && !fogged_at; ++f) {
            fog.decay_step(f);  // step 4
            if (f == 2) fog.source_service(7, unit(0, px, py, 40.0f));
            if (f == drop) fog.source_remove(7);
            if (f > drop && fog.point_fogged(0, px, py, false)) fogged_at = f;
            if (f >= 2 && f < drop) CHECK(!fog.point_fogged(0, px, py, false));
        }
        const uint32_t elapsed = fogged_at - drop;
        lo = std::min(lo, elapsed);
        hi = std::max(hi, elapsed);
        CHECK(!refogs.empty());
        bool once = true;
        for (const auto& kv : refogs) once = once && kv.second == 1;
        CHECK(once);
        CHECK(std::is_sorted(order.begin(), order.end()));  // one decay, cell-index order
    }
    std::printf("decay window: %u..%u frames (%.2f..%.2f s)\n", lo, hi, static_cast<double>(lo) / 30.0, static_cast<double>(hi) / 30.0);
    CHECK(lo == 193 && hi == 208);  // 6.43..6.93 s
    // Stagger: player 1 decays on F = 1 (mod 16), player 2 on 2; player 3 has no grid.
    FogOfWar fog;
    fog.start(land_setup());
    for (uint32_t f = 1; f <= 20; ++f) fog.decay_step(f);
    CHECK(fog.grid(0)->last_decay() == 16 && fog.grid(1)->last_decay() == 17 && fog.grid(2)->last_decay() == 18);
    CHECK(fog.grid(3) == nullptr);
    // Setup phase: no decay.
    fog.set_setup_phase(true);
    fog.decay_step(32);
    CHECK(fog.grid(0)->last_decay() == 16);
}

// ---- dense mask ----------------------------------------------------------------------------------------------

void test_dense() {
    FogSetup s;
    s.mode = FogMode::Space;
    s.box = {0.0f, 0.0f, 2000.0f, 2000.0f};
    s.players = {{0, true, true}, {1, true, false}};
    FogOfWar fog;
    fog.start(s);
    CHECK(fog.grid(0)->width() == 20 && fog.move_threshold_sq() == 10000.0f);
    const FogOfWar::DenseCircle nebula[] = {{1050.0f, 950.0f, 500.0f}};
    fog.set_dense_obstacles(nebula);
    const FogGrid& g = *fog.grid(0);
    const uint32_t centre = g.index_of(1050.0f, 950.0f);  // col 10, row 10
    CHECK(centre == 210u && fog.dense_mask()[centre] == 1 && fog.dense_mask()[210u + 5u] == 1 && fog.dense_mask()[210u + 6u] == 0);
    FogObject o = unit(0, 1050.0f, 950.0f, 600.0f);  // r = 600 (6 cells), dense 300 (3 cells)
    fog.source_service(1, o);
    CHECK(g.count_at(210u + 3u) == 1);  // dense, within rDense
    CHECK(g.count_at(210u + 4u) == 0);  // dense, beyond rDense
    CHECK(g.count_at(210u + 6u) == 1);  // ordinary, within r
    CHECK(g.count_at(210u + 7u) == 0);
    // rNormal <= rDense: one pass with rDense over both kinds.
    o.dense_multiplier = 1.0f;
    CellList l;
    fog.reveal_object(1, o, l, true);
    CHECK(fog.grid(1)->count_at(210u + 6u) == 1 && fog.grid(1)->count_at(210u + 4u) == 1);
    // Space mode keeps the carried FOW-active flag; land forces it on.
    s.fow_active = false;
    FogOfWar sp;
    sp.start(s);
    CHECK(!sp.fow_active());
    FogSetup ls = land_setup();
    ls.fow_active = false;
    FogOfWar lf;
    lf.start(ls);
    CHECK(lf.fow_active());
}

// ---- sources -------------------------------------------------------------------------------------------------

void test_sources() {
    FogOfWar fog;
    fog.start(land_setup());
    fog.set_relation(0, 2, 0);
    fog.set_relation(2, 0, 0);
    CHECK(fog.allied(0, 0) && fog.allied(0, 2) && !fog.allied(0, 1) && fog.move_threshold_sq() == 225.0f);
    const FogObject a = unit(0, cx_of(10), cy_of(10), 30.0f);  // r = 30 -> 2 cells, 21 cells
    fog.source_service(5, a);
    CHECK(count_sum(*fog.grid(0)) == 21u && count_sum(*fog.grid(2)) == 21u && count_sum(*fog.grid(1)) == 0u);
    // Sight floors at 10 (r = (int)(10/15 + .5) = 1 -> 5 cells).
    fog.source_service(6, unit(0, cx_of(2), cy_of(2), 0.0f));
    CHECK(count_sum(*fog.grid(0)) == 26u);
    fog.source_remove(6);
    // Moving less than one average cell: no refresh.
    FogObject b = a;
    b.x += 10.0f;
    fog.source_service(5, b);
    CHECK(fog.grid(0)->count_at(210u - 2u) == 1 && fog.grid(0)->count_at(210u + 3u) == 0);
    b.x = a.x + 16.0f;  // >= one cell: refresh around the new position (col 11)
    fog.source_service(5, b);
    CHECK(fog.grid(0)->count_at(210u - 2u) == 0 && fog.grid(0)->count_at(210u + 3u) == 1 && count_sum(*fog.grid(0)) == 21u);
    CHECK(count_sum(*fog.grid(2)) == 21u);
    // Owner change: undo for the old owner's allies, reveal for the new owner's.
    b.owner = 1;
    fog.source_change_owner(5, 0, b);
    CHECK(count_sum(*fog.grid(0)) == 0u && count_sum(*fog.grid(2)) == 0u && count_sum(*fog.grid(1)) == 21u);
    // Garrisoned: undone.
    fog.source_service(5, b, true);
    CHECK(count_sum(*fog.grid(1)) == 0u);
    fog.source_service(5, b);  // exit (FOG_OF_WAR §3.2 [H]): not dirty, within one cell of the last reveal -> nothing
    CHECK(count_sum(*fog.grid(1)) == 0u);
    b.x += 16.0f;  // one cell from the last reveal: re-reveals
    fog.source_service(5, b);
    CHECK(count_sum(*fog.grid(1)) == 21u);
    // Removal.
    fog.source_remove(5);
    CHECK(count_sum(*fog.grid(1)) == 0u && fog.source_count() == 0);
    // Setup-only source outside the setup phase: nothing.
    fog.source_service(9, a, false, true);
    CHECK(count_sum(*fog.grid(0)) == 0u);
    // Fired hook: flash the shooter's footprint on the target owner's grid only (player 2 is allied to 0 but
    // gets nothing).
    FogObject att = unit(1, cx_of(4), cy_of(4), 500.0f);
    fog.source_fired(att, 0);
    CHECK(fog.grid(0)->state_at(4u * 20u + 4u) == 0xFF && fog.grid(0)->count_at(4u * 20u + 4u) == 0);
    CHECK(fog.grid(2)->state_at(4u * 20u + 4u) == 0);
    // Player reveal list (owner's +0x1f8) and object modifiers: 10 x (1 + 1) x (1 + 0.5) = 30 -> 2 cells.
    FogOfWar f2;
    f2.start(land_setup());
    f2.add_player_reveal_multiplier(0, 3, 77u, 2.0f);  // stored 1.0
    f2.add_player_reveal_multiplier(0, 3, 78u, 1.0f);  // skipped at 1.0
    CHECK(f2.player_reveal_bonus(0) == 1.0f && f2.player_reveal_bonus(1) == 0.0f);
    FogObject m = unit(0, cx_of(10), cy_of(10), 10.0f);
    m.object_modifier = 0.5f;
    CellList l;
    f2.reveal_object(0, m, l, false);
    CHECK(l.size() == 21u);
    // Reduction: categories ascending, MostExtreme (largest |v|, first on ties) within one, summed.
    FogOfWar f3;
    f3.start(land_setup());
    f3.add_player_reveal_modifier(1, 5, 1u, 0.25f);
    f3.add_player_reveal_modifier(1, 5, 2u, -0.5f);  // |-0.5| > 0.25: wins category 5
    f3.add_player_reveal_modifier(1, 2, 3u, 0.5f);
    f3.add_player_reveal_modifier(1, 2, 4u, -0.5f);  // tie: first kept
    CHECK(f3.player_reveal_bonus(1) == 0.0f);        // 0.5 + (-0.5)
    f3.remove_player_reveal_modifier(1, 2u);
    CHECK(f3.player_reveal_bonus(1) == 0.75f);       // 0.5 + 0.25
    f3.remove_player_reveal_modifier(1, 3u);
    CHECK(f3.player_reveal_bonus(1) == -0.25f);      // -0.5 + 0.25
    f3.remove_player_reveal_modifier(1, 1u);
    f3.remove_player_reveal_modifier(1, 4u);
    CHECK(f3.player_reveal_bonus(1) == 0.0f);
    // Battle scale for its player only.
    f2.set_battle_scale(0, 0.5f);
    l.clear();
    f2.reveal_object(0, m, l, false);  // 30 x 0.5 = 15 -> 1 cell
    CHECK(l.size() == 5u);
}

// ---- queries -------------------------------------------------------------------------------------------------

void test_queries() {
    FogOfWar fog;
    fog.start(land_setup());
    const float x = cx_of(10), y = cy_of(10);
    CHECK(fog.point_fogged(0, x, y, false));        // human
    CHECK(!fog.point_fogged(1, x, y, false));       // AI: never fogged unless forced
    CHECK(fog.point_fogged(1, x, y, true));
    CHECK(!fog.point_fogged(3, x, y, true));        // no grid
    fog.source_service(1, unit(0, x, y, 30.0f));
    CHECK(!fog.point_fogged(0, x, y, false));
    CHECK(fog.point_fogged(0, -10.0f, y, false));   // outside the box
    FogHideQuery q;
    q.owner = 1;
    q.x = cx_of(2);
    q.y = cy_of(2);
    CHECK(!fog.object_hidden(0, q, false));  // neither TEAM nor HIDE_WHEN_FOGGED: never hidden
    q.team_or_hide_when_fogged = true;
    CHECK(fog.object_hidden(0, q, false));
    CHECK(!fog.object_hidden(1, q, false));  // AI gate
    q.multisample = true;
    for (size_t i = 0; i < 8; ++i) {
        q.corners[2 * i] = cx_of(2);
        q.corners[2 * i + 1] = cy_of(2);
    }
    CHECK(fog.object_hidden(0, q, false));
    q.corners[6] = x;  // one corner in a visible cell
    q.corners[7] = y;
    CHECK(!fog.object_hidden(0, q, false));
    q.multisample = false;
    q.owner = 0;
    q.has_reveal = true;
    CHECK(!fog.object_hidden(0, q, false));  // allied with REVEAL
    q.owner = 1;
    fog.reveal_all(0);
    CHECK(!fog.object_hidden(0, q, false));  // Reveal_All grid
    fog.undo_reveal_all(0);
    CHECK(!fog.object_hidden(0, q, false));  // counts gone, but the cells stay visible until they regrow
    for (uint32_t f = 16; f <= 16u * 14u; f += 16) fog.decay_step(f);
    CHECK(fog.object_hidden(0, q, false));
    fog.disable_rendering(true);
    CHECK(!fog.fow_active() && !fog.render_option() && !fog.point_fogged(0, cx_of(2), cy_of(2), true) && !fog.object_hidden(0, q, true));
    fog.disable_rendering(false);
    fog.reward_enable_fow(true, 0);
    CHECK(!fog.fow_active() && !fog.render_option());
    fog.reward_enable_fow(true, 3);
    CHECK(fog.fow_active() && fog.render_option());
    fog.reward_enable_fow(false, 0);
    CHECK(!fog.fow_active() && fog.render_option());
    // AIUsesFogOfWarLand = True fogs the AI without force.
    FogSetup s = land_setup();
    s.constants.ai_uses_fog_land = true;
    FogOfWar ai;
    ai.start(s);
    CHECK(ai.point_fogged(1, x, y, false));
    // Render view.
    const FogRenderView v = fog.render_view(0);
    CHECK(v.valid && v.width == 20 && v.height == 20 && v.state.size() == 400u && v.fade.size() == 400u && v.regrow_step == 21 && v.last_decay == 224u);
    CHECK(!fog.render_view(3).valid);
}

// ---- Lua -----------------------------------------------------------------------------------------------------

void test_lua() {
    FogOfWar fog;
    fog.start(land_setup());
    FogCells c = fog.lua_reveal_position(0, cx_of(10), cy_of(10), 30.0f, 30.0f);
    CHECK(c.player == 0 && c.cells.size() == 21u && count_sum(*fog.grid(0)) == 21u);
    FogCells c2 = fog.lua_reveal_position(0, cx_of(10), cy_of(10), 30.0f, 30.0f);
    fog.undo_reveal(c);
    CHECK(c.cells.empty() && count_sum(*fog.grid(0)) == 21u);
    fog.undo_reveal(c);  // second call does nothing
    CHECK(count_sum(*fog.grid(0)) == 21u);
    fog.undo_reveal(c2);
    CHECK(count_sum(*fog.grid(0)) == 0u);
    // Reveal(player, object): footprint for an enemy object (box half extent 20 -> 1 cell), sight for an allied one.
    FogObject e = unit(1, cx_of(5), cy_of(5), 300.0f);
    e.has_model = true;
    e.box_hx = 20.0f;
    e.box_hy = 5.0f;
    FogCells fe = fog.lua_reveal_object(0, e);
    CHECK(fe.cells.size() == 5u);
    fog.undo_reveal(fe);
    FogCells fa = fog.lua_reveal_object(1, e);
    CHECK(fa.cells.size() > 100u);
    // Battle scale applies to position reveals of its player.
    fog.set_battle_scale(0, 0.5f);
    FogCells cs = fog.lua_reveal_position(0, cx_of(10), cy_of(10), 60.0f, 60.0f);
    CHECK(cs.cells.size() == 21u);
    fog.set_battle_scale(-1, 1.0f);
    // Load path: recount.
    const FogCells saved = cs;
    fog.undo_reveal(cs);
    fog.recount(saved);
    CHECK(count_sum(*fog.grid(0)) == 21u);
    // Temporary_Reveal: flash at the footprint centre; radius 0 -> footprint radius.
    FogOfWar t;
    t.start(land_setup());
    FogObject o = unit(1, cx_of(3), cy_of(3), 0.0f);
    o.has_model = true;
    o.box_cx = 15.0f;
    o.box_hx = 10.0f;
    t.temporary_reveal(0, o, 0.0f);
    CHECK(t.grid(0)->state_at(3u * 20u + 4u) == 0xFF && count_sum(*t.grid(0)) == 0u);
    t.temporary_reveal(0, o, 30.0f);
    CHECK(t.grid(0)->state_at(3u * 20u + 6u) == 0xFF);
    // Reveal_All (no undo object); saved player list re-applied at start.
    t.reveal_all(2);
    CHECK((t.reveal_all_players() == std::vector<int32_t>{2}));
    FogSetup s = land_setup();
    s.saved_reveal_all = {2};
    FogOfWar r;
    r.start(s);
    CHECK(r.grid(2)->reveal_all_active() && count_sum(*r.grid(2)) == 400u);
}

// ---- setup phase / overrun -----------------------------------------------------------------------------------

void test_setup_overrun() {
    FogSetup s = land_setup();
    FogOfWar fog;
    fog.start(s);
    const int32_t p4 = 0, p8 = 1;
    std::vector<FogSetupObject> objs(3);
    objs[0].obj = unit(p8, cx_of(15), cy_of(15), 30.0f);  // defender unit, sees around (15, 15)
    objs[0].has_locomotor = objs[0].has_reveal = true;
    objs[1].obj = unit(p4, cx_of(3), cy_of(3), 30.0f);    // attacker unit
    objs[1].has_locomotor = objs[1].has_reveal = true;
    objs[2].obj = unit(p8, cx_of(14), cy_of(15), 30.0f);  // defender structure, revealed for the attacker
    objs[2].reveal_for_attacker = true;
    objs[2].reveal_during_setup = true;
    fog.source_service(4, objs[1].obj);  // behaviour Initialise services once before setup start
    const FogGrid& g8 = *fog.grid(p8);
    const FogGrid& g4 = *fog.grid(p4);
    CHECK(g4.count_at(3u * 20u + 3u) == 1);
    fog.begin_setup(objs, p4, p8);
    CHECK(fog.setup_phase());
    CHECK(g8.state_at(15u * 20u + 15u) == 0xFF && g8.state_at(0u) == 0);
    CHECK(g4.state_at(15u * 20u + 15u) == 0 && g4.count_at(15u * 20u + 15u) == 0);  // cells +8 sees: forced fogged
    CHECK(g4.state_at(3u * 20u + 3u) == 0xFF && g4.count_at(3u * 20u + 3u) == 1);
    fog.decay_step(16);
    CHECK(g4.state_at(3u * 20u + 3u) == 0xFF);  // no decay while setup runs
    FogObject moved = objs[1].obj;
    moved.x += 60.0f;
    fog.source_service(4, moved);  // REVEAL disabled during setup: nothing
    CHECK(g4.count_at(3u * 20u + 7u) == 0);
    const uint32_t g8_before = count_sum(g8);
    fog.source_service(5, objs[0].obj);  // created during setup: record only
    CHECK(fog.has_source(5) && count_sum(g8) == g8_before);
    fog.end_setup(objs, p4, p8);
    CHECK(!fog.setup_phase() && count_sum(g4) == 0u && count_sum(g8) == 0u && g8.state_at(15u * 20u + 15u) == 0xEF);
    fog.source_service(4, objs[1].obj);  // TACTICAL_GAPS §5.4: not dirty, did not move -> no re-reveal (quirk)
    CHECK(g4.count_at(3u * 20u + 3u) == 0);
    fog.source_service(4, moved);  // moved >= one cell: re-reveals (its stale list undoes clamped at 0)
    CHECK(g4.count_at(3u * 20u + 7u) == 1 && g4.count_at(3u * 20u + 3u) == 0);
    fog.source_service(5, objs[0].obj);  // never revealed (dirty): reveals now
    CHECK(g8.count_at(15u * 20u + 15u) == 1);

    FogOfWar o;
    o.start(land_setup());
    CHECK(o.service_overrun(899, 1, true, 0, 100.0f, 1, 20.0f) == -1);  // before 30 s
    CHECK(o.service_overrun(905, 1, true, 0, 100.0f, 1, 20.0f) == -1);  // not a 15-frame boundary
    CHECK(o.service_overrun(900, 3, true, 0, 100.0f, 1, 20.0f) == -1);  // campaign / story game type
    CHECK(o.service_overrun(900, 2, true, 0, 100.0f, 1, 40.0f) == -1);  // 40 x 3 > 100
    CHECK(o.service_overrun(900, 2, true, 0, 100.0f, 1, 20.0f) == 0);
    CHECK(o.grid(0)->reveal_all_active() && o.overrun_done(false));
    CHECK(o.service_overrun(915, 2, true, 0, 100.0f, 1, 20.0f) == -1);  // once
    CHECK(o.service_overrun(930, 4, true, 0, 10.0f, 1, 50.0f) == 1);

    CHECK(weather_sight_factor(0.6f, true, true, 0.7f) == 0.7f && weather_sight_factor(0.5f, true, true, 0.7f) == 1.0f);
    CHECK(weather_sight_factor(0.9f, true, true, 0.00005f) == 1.0f && weather_sight_factor(0.9f, false, true, 0.7f) == 1.0f);
    const float mods[] = {-0.5f, -0.75f};
    const float one[] = {-0.25f};
    CHECK(battle_reveal_scale(mods) == 0.25f && battle_reveal_scale(one) == 0.75f && battle_reveal_scale({}) == 1.0f);
    // TACTICAL_GAPS §5.3: land scales the defender by the attacker's objects, space the attacker by the defender's.
    const BattleScaleSides ls = battle_scale_sides(FogMode::Land, 4, 8), ss = battle_scale_sides(FogMode::Space, 4, 8);
    CHECK(ls.scaled == 8 && ls.summed == 4 && ss.scaled == 4 && ss.summed == 8);
}

// ---- FOWF t03 / t05: setup reveal conditions, base power, setup manager (FOG_OF_WAR §3.4) ----------------------

void test_setup_manager() {
    const int32_t p4 = 0, p8 = 1;
    // Setup reveal outside the setup phase (no binarise): which objects reveal, and the prevention circle for +4.
    auto reveal_once = [&](const std::vector<FogSetupObject>& objs, bool hook, bool power) {
        auto f = std::make_unique<FogOfWar>();
        f->start(land_setup());
        if (hook) f->set_base_power([power](int32_t p) { return power && p == 1; });  // base power for p8 only
        f->setup_reveal(objs, p4, p8);
        return f;
    };
    // A forced object (Reveal_For_Defender) without locomotor or REVEAL reveals ([H] force || (REVEAL && locomotor)).
    std::vector<FogSetupObject> forced(1);
    forced[0].obj = unit(p4, cx_of(5), cy_of(5), 30.0f);
    forced[0].reveal_for_defender = true;
    CHECK(reveal_once(forced, false, false)->grid(p8)->count_at(5u * 20u + 5u) == 1);
    // REVEAL without a locomotor (and no force): nothing.
    std::vector<FogSetupObject> still(1);
    still[0].obj = unit(p8, cx_of(5), cy_of(5), 30.0f);
    still[0].has_reveal = true;
    CHECK(reveal_once(still, false, false)->grid(p8)->count_at(5u * 20u + 5u) == 0);
    // Prevention circle of a POWERED defender structure: drawn for +4 (counts zeroed, state 0xEF) only with base power.
    std::vector<FogSetupObject> shield(1);
    shield[0].obj = unit(p8, cx_of(10), cy_of(10), 10.0f);
    shield[0].reveal_during_setup = true;
    shield[0].has_powered = true;
    shield[0].reinforcement_prevention_radius = 60.0f;
    const uint32_t far_cell = 10u * 20u + 13u;  // 3 cells east: inside the 60-unit circle, outside the sight radius 10
    {
        const auto off = reveal_once(shield, false, false);
        CHECK(off->grid(p8)->count_at(10u * 20u + 10u) == 1 && off->grid(p4)->state_at(far_cell) == 0);
        const auto unpowered = reveal_once(shield, true, false);
        CHECK(unpowered->grid(p4)->state_at(far_cell) == 0);
        const auto powered = reveal_once(shield, true, true);
        CHECK(powered->grid(p4)->state_at(far_cell) == kStateFirstReveal && powered->grid(p4)->count_at(far_cell) == 0);
    }
    shield[0].has_powered = false;  // no POWERED behaviour: no base power needed
    CHECK(reveal_once(shield, false, false)->grid(p4)->state_at(far_cell) == kStateFirstReveal);
    shield[0].reveal_for_attacker = true;  // target = +4, not +8: no prevention circle (own reveal only)
    {
        const auto f = reveal_once(shield, false, false);
        CHECK(f->grid(p4)->state_at(far_cell) == 0 && f->grid(p4)->count_at(10u * 20u + 10u) == 1);
    }

    // Manager: players 0 and 2 are human (land_setup). Countdown 5 s = 150 frames from the first ready.
    FogOfWar fog;
    FogSetup s = land_setup();
    s.constants.setup_phase_enabled = true;
    fog.start(s);
    CHECK(!fog.setup_end_due(1000));  // not in the setup phase
    fog.begin_setup({}, p4, p8);
    CHECK(fog.setup_phase() && !fog.setup_end_due(1000) && !fog.setup_countdown_armed());
    fog.set_frame(100);
    fog.setup_player_ready(1);  // the AI player: arms the countdown, humans not ready
    CHECK(fog.setup_ready(1) && fog.setup_countdown_armed() && !fog.setup_all_ready());
    fog.set_frame(120);
    fog.setup_player_ready(0);  // one human: countdown not re-armed
    CHECK(!fog.setup_all_ready() && !fog.setup_end_due(249) && fog.setup_end_due(250));
    fog.setup_player_ready(2);  // every human ready: due at once
    CHECK(fog.setup_all_ready() && fog.setup_end_due(121));
    {  // the manager survives a save / load
        alamo::ByteWriter w;
        fog.write(w);
        FogOfWar l;
        alamo::ByteReader r(w.bytes());
        CHECK(l.read(r) && l.setup_phase() && l.setup_all_ready() && l.setup_ready(2) && l.setup_end_due(121) &&
              l.constants().setup_countdown_seconds == 5.0f);
    }
    fog.end_setup({}, p4, p8);
    CHECK(!fog.setup_phase() && !fog.setup_end_due(1000));
}

// ---- space reinforcement sampler (TACTICAL_GAPS §5.5) ----------------------------------------------------------

void test_reinforce_sampler() {
    FogOfWar fog;
    fog.start(land_setup());
    fog.set_relation(0, 2, 0);
    const PreventionCircle circles[] = {{1, cx_of(10), cy_of(10), 30.0f}, {2, cx_of(2), cy_of(2), 60.0f}};
    CHECK(fog.reinforce_sample(0, cx_of(10), cy_of(10), circles) == 0);           // enemy circle
    CHECK(fog.reinforce_sample(0, cx_of(10) + 30.0f, cy_of(10), circles) == 0xFF);  // strict <
    CHECK(fog.reinforce_sample(0, cx_of(10) + 29.0f, cy_of(10), circles) == 0);
    CHECK(fog.reinforce_sample(0, cx_of(2), cy_of(2), circles) == 0xFF);          // allied circle ignored
    CHECK(fog.reinforce_sample(1, cx_of(2), cy_of(2), circles) == 0);
    CHECK(fog.reinforce_sample(1, cx_of(10), cy_of(10), circles) == 0xFF);        // its own circle
    std::vector<uint8_t> ov;
    CHECK(fog.reinforce_overlay(0, circles, ov) && ov.size() == 400u);
    CHECK(ov[10u * 20u + 10u] == 0 && ov[10u * 20u + 11u] == 0 && ov[10u * 20u + 12u] == 0xFF && ov[2u * 20u + 2u] == 0xFF);
    CHECK(!fog.reinforce_overlay(3, circles, ov) && ov.empty());
    // Drop validity (fog part): fogged for the AI too (force 1), then the prevention circles.
    CHECK(!fog.reinforce_point_ok(1, cx_of(15), cy_of(15), circles));
    fog.source_service(1, unit(1, cx_of(15), cy_of(15), 30.0f));
    CHECK(fog.reinforce_point_ok(1, cx_of(15), cy_of(15), circles));
    fog.source_service(2, unit(0, cx_of(10), cy_of(10), 30.0f));
    CHECK(!fog.reinforce_point_ok(0, cx_of(10), cy_of(10), circles));
    fog.disable_rendering(true);  // FOW off: never fogged
    CHECK(fog.reinforce_point_ok(1, cx_of(5), cy_of(15), circles));
}

// ---- order independence --------------------------------------------------------------------------------------

uint64_t run_sources(bool shuffled) {
    FogOfWar fog;
    fog.start(land_setup());
    fog.set_relation(0, 2, 0);
    fog.set_relation(2, 0, 0);
    uint32_t seed = 12345;
    auto rnd = [&seed](uint32_t n) {
        seed = seed * 1103515245u + 12345u;
        return (seed >> 8) % n;
    };
    struct S {
        int32_t id, owner;
        float x, y, vx, vy, sight;
        uint32_t die;
    };
    std::vector<S> src;
    for (int32_t i = 0; i < 40; ++i)
        src.push_back({i + 1, static_cast<int32_t>(rnd(3)), static_cast<float>(rnd(300)), static_cast<float>(rnd(300)),
                       static_cast<float>(rnd(9)) - 4.0f, static_cast<float>(rnd(9)) - 4.0f, static_cast<float>(10 + rnd(80)),
                       60 + rnd(200)});
    uint32_t perm_seed = 777;
    for (uint32_t f = 1; f <= 300; ++f) {
        fog.decay_step(f);
        std::vector<size_t> order(src.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        if (shuffled)
            for (size_t i = order.size(); i > 1; --i) {
                perm_seed = perm_seed * 1664525u + 1013904223u;
                std::swap(order[i - 1], order[(perm_seed >> 8) % i]);
            }
        for (size_t k : order) {
            S& s = src[k];
            if (f == s.die) {
                fog.source_remove(s.id);
                continue;
            }
            if (f > s.die) continue;
            s.x = std::clamp(s.x + s.vx, 0.0f, 300.0f);
            s.y = std::clamp(s.y + s.vy, 0.0f, 300.0f);
            fog.source_service(s.id, unit(s.owner, s.x, s.y, s.sight));
        }
        if (f % 37 == 0) {
            FogCells c = fog.lua_reveal_position(1, 150.0f, 150.0f, 45.0f, 45.0f);
            fog.temporary_reveal(0, unit(1, 40.0f, 260.0f, 0.0f), 20.0f);
            if (f % 74 == 0) fog.undo_reveal(c);
        }
    }
    alamo::StateHasher h;
    fog.hash(h);
    return h.digest();
}

void test_order_independence() {
    const uint64_t a = run_sources(false), b = run_sources(true);
    std::printf("order independence: %016llx %016llx\n", static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
    CHECK(a == b);
}

}  // namespace

int main() {
    test_geometry();
    test_circle();
    test_counts();
    test_decay_window();
    test_dense();
    test_sources();
    test_queries();
    test_lua();
    test_setup_overrun();
    test_setup_manager();
    test_reinforce_sampler();
    test_order_independence();
    std::printf("alamo_tactical_fog_tests: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
