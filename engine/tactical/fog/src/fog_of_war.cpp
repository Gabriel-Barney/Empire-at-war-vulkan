#include "alamo/tactical/fog/fog_of_war.hpp"

#include <algorithm>
#include <climits>
#include <cmath>

#include "alamo/core/sim_crc.hpp"

namespace alamo::tactical::fog {

float weather_sight_factor(float intensity, bool owner_affected, bool has_modifier, float modifier) {
    if (!(intensity > 0.5f) || !owner_affected || !has_modifier) return 1.0f;
    return modifier <= 0.0001f ? 1.0f : modifier;
}

float battle_reveal_scale(std::span<const float> mods) {
    float sum = 0.0f;
    for (float m : mods) sum += m;
    return std::clamp(1.0f + sum, 0.25f, 1.0f);
}

BattleScaleSides battle_scale_sides(FogMode mode, int32_t attacker, int32_t defender) {
    BattleScaleSides s;
    if (mode == FogMode::Land) {
        s.scaled = defender;
        s.summed = attacker;
    } else {
        s.scaled = attacker;
        s.summed = defender;
    }
    return s;
}

// ---- setup ---------------------------------------------------------------------------------------------------

void FogOfWar::start(const FogSetup& s) {
    started_ = true;
    mode_ = s.mode;
    k_ = s.constants;
    players_ = s.players;
    const bool land = mode_ == FogMode::Land;
    int32_t max_id = -1;
    for (const FogPlayer& p : players_) max_id = std::max(max_id, p.id);
    grids_.assign(static_cast<size_t>(max_id + 1), FogGrid{});
    const float cell = land ? k_.desired_land_cell_size : k_.desired_space_cell_size;
    threshold_ = 100.0f;
    bool have_geometry = false;
    for (const FogPlayer& p : players_) {
        if (p.id < 0 || !p.grid) continue;
        FogGrid& g = grids_[static_cast<size_t>(p.id)];
        g.init(s.box, cell, cell, p.id);
        if (g.valid() && !have_geometry) {
            // REVEAL move threshold: ((cellW + cellH) / 2)^2 (grids share geometry; open item 6).
            const float avg = (g.cell_w() + g.cell_h()) / 2.0f;
            threshold_ = avg * avg;
            have_geometry = true;
        }
    }
    step_ = regrow_step(k_.fps, land ? k_.land_regrow_time : k_.space_regrow_time);
    fow_active_ = land ? true : s.fow_active;
    render_option_ = land ? true : s.render_option;
    rel_n_ = max_id + 1;
    relation_.assign(static_cast<size_t>(rel_n_) * static_cast<size_t>(rel_n_), 1);
    for (int32_t i = 0; i < rel_n_; ++i) relation_[static_cast<size_t>(i) * static_cast<size_t>(rel_n_) + static_cast<size_t>(i)] = 0;
    player_bonus_.assign(static_cast<size_t>(rel_n_), 0.0f);
    player_list_.assign(static_cast<size_t>(rel_n_), {});
    mask_.clear();
    sources_.clear();
    frame_ = 0;
    setup_ = false;
    overrun4_ = overrun8_ = false;
    scale_player_ = -1;
    scale_ = 1.0f;
    for (int32_t p : s.saved_reveal_all) reveal_all(p);
}

void FogOfWar::set_dense_obstacles(std::span<const DenseCircle> circles) {
    FogGrid* first = nullptr;
    for (FogGrid& g : grids_)
        if (g.valid()) {
            first = &g;
            break;
        }
    if (!first) return;
    mask_.assign(first->cells(), 0);
    CellList tmp;
    for (const DenseCircle& c : circles) {
        tmp.clear();
        first->collect_circle(c.x, c.y, c.radius, tmp, true, true);
        for (uint32_t i : tmp) mask_[i] = 1;
    }
    for (FogGrid& g : grids_)
        if (g.valid()) g.set_dense_mask(mask_.data());
}

void FogOfWar::set_relation(int32_t p, int32_t owner, uint8_t relation) {
    if (p < 0 || owner < 0 || p >= rel_n_ || owner >= rel_n_) return;
    relation_[static_cast<size_t>(p) * static_cast<size_t>(rel_n_) + static_cast<size_t>(owner)] = relation;
}

bool FogOfWar::allied(int32_t p, int32_t owner) const {
    if (p < 0 || owner < 0 || p >= rel_n_ || owner >= rel_n_) return p == owner;
    return relation_[static_cast<size_t>(p) * static_cast<size_t>(rel_n_) + static_cast<size_t>(owner)] == 0;
}

void FogOfWar::reduce_player_list(int32_t player) {
    const std::vector<RevealEntry>& list = player_list_[static_cast<size_t>(player)];
    // Categories ascending (std::map order); MostExtreme within one (strictly larger |v| replaces); sum in order.
    float total = 0.0f;
    bool seeded = false;
    int32_t prev = INT32_MIN;
    bool have_prev = false;
    for (;;) {
        bool found = false;
        int32_t cat = 0;
        for (const RevealEntry& e : list)
            if ((!have_prev || e.category > prev) && (!found || e.category < cat)) {
                cat = e.category;
                found = true;
            }
        if (!found) break;
        bool first = true;
        float best = 0.0f;
        for (const RevealEntry& e : list) {
            if (e.category != cat) continue;
            if (first || std::fabs(e.value) > std::fabs(best)) best = e.value;
            first = false;
        }
        total = seeded ? total + best : best;
        seeded = true;
        prev = cat;
        have_prev = true;
    }
    player_bonus_[static_cast<size_t>(player)] = total;
}

void FogOfWar::add_player_reveal_modifier(int32_t player, int32_t category, uint64_t source, float value) {
    if (player < 0 || static_cast<size_t>(player) >= player_list_.size()) return;
    player_list_[static_cast<size_t>(player)].push_back({category, source, value});
    reduce_player_list(player);
}

void FogOfWar::add_player_reveal_multiplier(int32_t player, int32_t category, uint64_t source, float multiplier) {
    if (multiplier == 1.0f) return;  // skipped at 1.0 (TACTICAL_GAPS §2.3.11)
    add_player_reveal_modifier(player, category, source, multiplier - 1.0f);
}

void FogOfWar::remove_player_reveal_modifier(int32_t player, uint64_t source) {
    if (player < 0 || static_cast<size_t>(player) >= player_list_.size()) return;
    std::vector<RevealEntry>& list = player_list_[static_cast<size_t>(player)];
    list.erase(std::remove_if(list.begin(), list.end(), [source](const RevealEntry& e) { return e.source == source; }), list.end());
    reduce_player_list(player);
}

float FogOfWar::player_reveal_bonus(int32_t player) const {
    if (player < 0 || static_cast<size_t>(player) >= player_bonus_.size()) return 0.0f;
    return player_bonus_[static_cast<size_t>(player)];
}

void FogOfWar::set_battle_scale(int32_t player, float scale) {
    scale_player_ = player;
    scale_ = scale;
}

void FogOfWar::set_human(int32_t player, bool human) {
    for (FogPlayer& p : players_)
        if (p.id == player) p.human = human;
}

const FogGrid* FogOfWar::grid(int32_t p) const {
    if (p < 0 || static_cast<size_t>(p) >= grids_.size()) return nullptr;
    const FogGrid& g = grids_[static_cast<size_t>(p)];
    return g.valid() ? &g : nullptr;
}

FogGrid* FogOfWar::grid(int32_t p) {
    if (p < 0 || static_cast<size_t>(p) >= grids_.size()) return nullptr;
    FogGrid& g = grids_[static_cast<size_t>(p)];
    return g.valid() ? &g : nullptr;
}

// ---- decay ---------------------------------------------------------------------------------------------------

void FogOfWar::decay_step(uint32_t frame) {
    frame_ = frame;
    if (!started_ || setup_) return;
    for (const FogPlayer& p : players_) {
        if (p.id < 0 || static_cast<uint32_t>(p.id) % 16u != frame % 16u) continue;
        if (FogGrid* g = grid(p.id)) g->decay(frame, step_, refog_);
    }
}

// ---- reveal object -------------------------------------------------------------------------------------------

void FogOfWar::sight_radii(int32_t p, const FogObject& o, bool force, float& cx, float& cy, float& r, float& r_dense) const {
    if (!force && !allied(p, o.owner)) {
        cx = o.x + o.box_cx;
        cy = o.y + o.box_cy;
        r = o.has_model ? std::max(o.box_hx, o.box_hy) : kNoModelFootprint;
        r_dense = r;
    } else {
        const float bonus = o.owner >= 0 && static_cast<size_t>(o.owner) < player_bonus_.size()
                                ? player_bonus_[static_cast<size_t>(o.owner)]
                                : 0.0f;
        float v = o.sight_range * (1.0f + bonus);
        v = v * (1.0f + o.object_modifier);
        r = std::max(kNoModelFootprint, v);
        r_dense = r * o.dense_multiplier;
        cx = o.x;
        cy = o.y;
    }
    if (p == scale_player_) {
        r = r * scale_;
        r_dense = r_dense * scale_;
    }
}

void FogOfWar::reveal_object(int32_t p, const FogObject& o, CellList& out, bool force) {
    FogGrid* g = grid(p);
    if (!started_ || !g) return;
    float cx = 0.0f, cy = 0.0f, r = 0.0f, rd = 0.0f;
    sight_radii(p, o, force, cx, cy, r, rd);
    g->reveal_dense(cx, cy, r, rd, out, frame_);
}

// ---- sources -------------------------------------------------------------------------------------------------

FogOfWar::Source* FogOfWar::find_source(int32_t id) {
    auto it = std::lower_bound(sources_.begin(), sources_.end(), id, [](const Source& s, int32_t v) { return s.id < v; });
    return it != sources_.end() && it->id == id ? &*it : nullptr;
}

const FogOfWar::Source* FogOfWar::find_source(int32_t id) const {
    auto it = std::lower_bound(sources_.begin(), sources_.end(), id, [](const Source& s, int32_t v) { return s.id < v; });
    return it != sources_.end() && it->id == id ? &*it : nullptr;
}

bool FogOfWar::has_source(int32_t id) const { return find_source(id) != nullptr; }

void FogOfWar::undo_for_allies(const Source& s, int32_t owner) {
    for (const FogPlayer& p : players_)
        if (FogGrid* g = grid(p.id); g && allied(p.id, owner)) g->undo(s.cells);
}

void FogOfWar::reveal_for_allies(Source& s, const FogObject& o) {
    // Per allied player (list order): undo the stored list, reveal into a fresh list; the LAST player's list
    // is stored (quirk kept: allies with different radii undo the wrong cells, clamped at 0).
    CellList tmp;
    for (const FogPlayer& p : players_) {
        FogGrid* g = grid(p.id);
        if (!g || !allied(p.id, o.owner)) continue;
        g->undo(s.cells);
        tmp.clear();
        reveal_object(p.id, o, tmp, false);
    }
    s.cells = std::move(tmp);
    s.x0 = o.x;
    s.y0 = o.y;
    s.owner = o.owner;
    s.dirty = false;
    s.revealed = true;
}

void FogOfWar::source_service(int32_t id, const FogObject& o, bool contained, bool setup_only) {
    if (!started_) return;
    Source* s = find_source(id);
    if (!s) {
        auto it = std::lower_bound(sources_.begin(), sources_.end(), id, [](const Source& a, int32_t v) { return a.id < v; });
        Source ns;
        ns.id = id;
        ns.owner = o.owner;
        s = &*sources_.insert(it, std::move(ns));
    }
    if (setup_) return;  // REVEAL disabled during the setup phase (TACTICAL_GAPS §5.4)
    if (setup_only) return;  // Reveal_During_Setup_Phase_Only outside the setup phase
    if (contained) {
        // vf6 step 2: undo, clear the list; dirty and the last reveal position are kept. The exit (0x37a8d0) only
        // clears the contained flag, so the source re-reveals once it is one cell from where it last revealed
        // (FOG_OF_WAR §3.2 [H]).
        undo_for_allies(*s, s->owner);
        s->cells.clear();
        return;
    }
    if (!s->dirty) {
        const float dx = o.x - s->x0, dy = o.y - s->y0;
        if (dx * dx + dy * dy < threshold_) return;
    }
    reveal_for_allies(*s, o);
}

void FogOfWar::source_change_owner(int32_t id, int32_t old_owner, const FogObject& o) {
    Source* s = find_source(id);
    if (!s) return;
    if (!s->revealed) {
        s->owner = o.owner;
        s->dirty = true;
        return;
    }
    undo_for_allies(*s, old_owner);
    s->cells.clear();
    reveal_for_allies(*s, o);
}

void FogOfWar::source_remove(int32_t id) {
    auto it = std::lower_bound(sources_.begin(), sources_.end(), id, [](const Source& a, int32_t v) { return a.id < v; });
    if (it == sources_.end() || it->id != id) return;
    undo_for_allies(*it, it->owner);
    sources_.erase(it);
}

void FogOfWar::source_fired(const FogObject& a, int32_t target_owner) {
    FogGrid* g = grid(target_owner);
    if (!started_ || !g) return;
    const float r = a.has_model ? std::max(a.box_hx, a.box_hy) : kNoModelFootprint;
    g->flash(a.x + a.box_cx, a.y + a.box_cy, r);
}

void FogOfWar::mark_sources_dirty() {
    for (Source& s : sources_) s.dirty = true;
}

// ---- other sources -------------------------------------------------------------------------------------------

void FogOfWar::reveal_all(int32_t p) {
    if (FogGrid* g = grid(p)) g->reveal_all(frame_);
}

void FogOfWar::undo_reveal_all(int32_t p) {
    if (FogGrid* g = grid(p)) g->undo_reveal_all();
}

std::vector<int32_t> FogOfWar::reveal_all_players() const {
    std::vector<int32_t> out;
    for (const FogPlayer& p : players_)
        if (const FogGrid* g = grid(p.id); g && g->reveal_all_active()) out.push_back(p.id);
    return out;
}

int32_t FogOfWar::service_overrun(uint32_t frame, int32_t game_type, bool enabled, int32_t p4, float power4, int32_t p8, float power8) {
    if (!started_ || !enabled || !(game_type == 1 || game_type == 2 || game_type == 4)) return -1;
    if (!(k_.tactical_overrun_multiple > 0.0f) || frame % 15u != 0) return -1;
    if (static_cast<float>(frame) < 30.0f * k_.min_overrun_seconds) return -1;
    const float lo = std::min(power4, power8), hi = std::max(power4, power8);
    if (!(lo * k_.tactical_overrun_multiple < hi)) return -1;
    if (power4 > power8) {
        if (overrun4_) return -1;
        overrun4_ = true;
        reveal_all(p4);
        return p4;
    }
    if (overrun8_) return -1;
    overrun8_ = true;
    reveal_all(p8);
    return p8;
}

// ---- setup phase ---------------------------------------------------------------------------------------------

void FogOfWar::setup_reveal(std::span<const FogSetupObject> objects, int32_t p4, int32_t p8) {
    CellList kept;  // static list, never undone
    CellList tmp;
    for (const FogSetupObject& so : objects) {
        int32_t target = so.obj.owner;
        bool force = false;
        if (so.reveal_for_attacker) {
            target = p4;
            force = true;
        } else if (so.reveal_for_defender) {
            target = p8;
            force = true;
        }
        if (so.reveal_during_setup || (so.has_locomotor && (force || so.has_reveal))) reveal_object(target, so.obj, kept, force);
        if (p8 >= 0 && so.obj.owner == p8 && so.reinforcement_prevention_radius != 0.0f && so.powered_or_unpowered) {
            if (FogGrid* g4 = grid(p4)) {
                tmp.clear();
                g4->reveal(so.obj.x, so.obj.y, so.reinforcement_prevention_radius, &tmp, RevealFlags{}, frame_);
                g4->reset_counts(tmp);
            }
        }
    }
    if (!setup_) return;
    FogGrid* g8 = grid(p8);
    FogGrid* g4 = grid(p4);
    if (g8 && g4) {
        g8->binarise(g4);
    } else {
        for (const FogPlayer& p : players_)
            if (FogGrid* g = grid(p.id)) g->binarise(nullptr);
    }
}

void FogOfWar::begin_setup(std::span<const FogSetupObject> objects, int32_t p4, int32_t p8) {
    if (!started_) return;
    setup_ = true;
    setup_reveal(objects, p4, p8);
}

void FogOfWar::end_setup(std::span<const FogSetupObject> objects, int32_t p4, int32_t p8) {
    // Behaviours re-enabled (sources keep their lists and positions, not dirty: TACTICAL_GAPS §5.4 quirk).
    setup_ = false;
    for (const FogPlayer& p : players_)
        if (FogGrid* g = grid(p.id)) g->reset(false);
    setup_reveal(objects, p4, p8);
    for (const FogPlayer& p : players_)
        if (FogGrid* g = grid(p.id)) g->reset(true);
}

// ---- space reinforcement sampler -----------------------------------------------------------------------------

uint8_t FogOfWar::reinforce_sample(int32_t p, float x, float y, std::span<const PreventionCircle> circles) const {
    for (const PreventionCircle& c : circles) {
        if (allied(p, c.owner)) continue;
        const float dx = x - c.x, dy = y - c.y;
        if (dx * dx + dy * dy < c.radius * c.radius) return 0;
    }
    return 0xFF;
}

bool FogOfWar::reinforce_overlay(int32_t p, std::span<const PreventionCircle> circles, std::vector<uint8_t>& out) const {
    out.clear();
    const FogGrid* g = grid(p);
    if (!started_ || !g) return false;
    out.resize(g->cells());
    for (uint32_t i = 0; i < g->cells(); ++i) {
        float x = 0.0f, y = 0.0f;
        g->cell_centre(i, x, y);
        out[i] = reinforce_sample(p, x, y, circles);
    }
    return true;
}

bool FogOfWar::reinforce_point_ok(int32_t p, float x, float y, std::span<const PreventionCircle> circles) const {
    if (point_fogged(p, x, y, true)) return false;
    return reinforce_sample(p, x, y, circles) != 0;
}

// ---- queries -------------------------------------------------------------------------------------------------

bool FogOfWar::is_human(int32_t p) const {
    for (const FogPlayer& q : players_)
        if (q.id == p) return q.human;
    return false;
}

bool FogOfWar::ai_gate(int32_t p, bool force) const {
    if (force || is_human(p)) return true;
    return mode_ == FogMode::Land ? k_.ai_uses_fog_land : k_.ai_uses_fog_space;
}

bool FogOfWar::point_fogged(int32_t p, float x, float y, bool force) const {
    if (!started_ || !fow_active_ || !ai_gate(p, force)) return false;
    const FogGrid* g = grid(p);
    return g ? g->fogged_at(x, y) : false;
}

bool FogOfWar::object_hidden(int32_t p, const FogHideQuery& q, bool force) const {
    if (!started_ || !fow_active_ || !q.team_or_hide_when_fogged) return false;
    if (q.has_reveal && allied(p, q.owner)) return false;
    const FogGrid* g = grid(p);
    if (!g || g->reveal_all_active()) return false;
    if (!point_fogged(p, q.x, q.y, force)) return false;
    if (q.multisample)
        for (size_t i = 0; i < 8; ++i)
            if (!point_fogged(p, q.corners[2 * i], q.corners[2 * i + 1], force)) return false;
    return true;
}

// ---- Lua / story ---------------------------------------------------------------------------------------------

FogCells FogOfWar::lua_reveal_object(int32_t p, const FogObject& o) {
    FogCells c;
    c.player = p;
    reveal_object(p, o, c.cells, false);
    return c;
}

FogCells FogOfWar::lua_reveal_position(int32_t p, float x, float y, float radius, float dense_radius) {
    FogCells c;
    c.player = p;
    FogGrid* g = grid(p);
    if (!started_ || !g) return c;
    if (p == scale_player_) {
        radius = radius * scale_;
        dense_radius = dense_radius * scale_;
    }
    g->reveal_dense(x, y, radius, dense_radius, c.cells, frame_);
    return c;
}

void FogOfWar::undo_reveal(FogCells& c) {
    if (FogGrid* g = grid(c.player)) g->undo(c.cells);
    c.cells.clear();
}

void FogOfWar::recount(const FogCells& c) {
    if (FogGrid* g = grid(c.player)) g->count_cells(c.cells, frame_);
}

void FogOfWar::temporary_reveal(int32_t p, const FogObject& o, float radius) {
    FogGrid* g = grid(p);
    if (!started_ || !g) return;
    const float r = radius != 0.0f ? radius : (o.has_model ? std::max(o.box_hx, o.box_hy) : kNoModelFootprint);
    g->flash(o.x + o.box_cx, o.y + o.box_cy, r);
}

void FogOfWar::disable_rendering(bool disable) {
    fow_active_ = !disable;
    render_option_ = !disable;
}

void FogOfWar::reward_enable_fow(bool has_param, int32_t value) {
    if (!has_param) {
        fow_active_ = false;
    } else if (value == 0) {
        fow_active_ = false;
        render_option_ = false;
    } else {
        fow_active_ = true;
        render_option_ = true;
    }
}

// ---- views ---------------------------------------------------------------------------------------------------

FogRenderView FogOfWar::render_view(int32_t p) const {
    FogRenderView v;
    v.player = p;
    v.frame = frame_;
    v.fow_active = fow_active_;
    v.render_option = render_option_;
    v.setup_phase = setup_;
    const FogGrid* g = grid(p);
    if (!g) return v;
    v.valid = true;
    v.width = g->width();
    v.height = g->height();
    v.box = g->box();
    v.cell_w = g->cell_w();
    v.cell_h = g->cell_h();
    v.state = g->state();
    v.fade = g->fade();
    v.last_decay = g->last_decay();
    v.regrow_step = g->regrow();
    v.reveal_all = g->reveal_all_active();
    return v;
}

void FogOfWar::hash(StateHasher& h) const {
    h.boolean(started_);
    h.u8(static_cast<uint8_t>(mode_));
    h.boolean(fow_active_);
    h.boolean(render_option_);
    h.boolean(setup_);
    h.boolean(overrun4_);
    h.boolean(overrun8_);
    h.u32(static_cast<uint32_t>(grids_.size()));
    for (const FogGrid& g : grids_) {
        h.boolean(g.valid());
        if (g.valid()) g.hash(h);
    }
    h.u32(static_cast<uint32_t>(mask_.size()));
    if (!mask_.empty()) h.bytes(mask_.data(), mask_.size());
    for (const float b : player_bonus_) h.f32(b);
    h.i32(scale_player_);
    h.f32(scale_);
    h.u32(static_cast<uint32_t>(sources_.size()));
    for (const Source& s : sources_) {
        h.i32(s.id);
        h.i32(s.owner);
        h.f32(s.x0);
        h.f32(s.y0);
        h.boolean(s.dirty);
        h.boolean(s.revealed);
        h.u32(static_cast<uint32_t>(s.cells.size()));
        for (uint32_t c : s.cells) h.u32(c);
    }
}

}  // namespace alamo::tactical::fog
