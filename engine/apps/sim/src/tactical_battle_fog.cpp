// Fog of war hookup of TacticalBattle (owner: fog agent; docs/re/FOG_OF_WAR.md, engine/tactical/fog).
// Kept in its own file so the other battle hooks (A1, L1/S4) stay separate. Without ALAMO_SIM_HAVE_FOG every
// entry point is a no-op. Fog never draws RNG and is not hashed into the frame CRC.
#include "alamo/sim/tactical_battle.hpp"

#ifdef ALAMO_SIM_HAVE_FOG
#include <algorithm>
#include <cmath>

#include "alamo/core/dmath.hpp"

#include "alamo/objects/behaviors.hpp"
#include "alamo/objects/game_object_types.hpp"
#endif

namespace alamo::sim {

#ifndef ALAMO_SIM_HAVE_FOG

void TacticalBattle::fog_begin(TacticalMode&, uint32_t) {}
void TacticalBattle::fog_after_deploy(TacticalMode&) {}
void TacticalBattle::fog_decay() {}
void TacticalBattle::fog_service(TacticalMode&) {}
void TacticalBattle::fog_owner_changed(TacticalMode&, objects::ObjectHandle, int32_t) {}
void TacticalBattle::fog_ping(objects::ObjectId) {}
void TacticalBattle::fog_setup_tick() {}
commands::RejectReason TacticalBattle::fog_setup_command(TacticalMode&, uint8_t, const commands::EventData&) {
    return commands::RejectReason::NotAllowed;
}

#else

namespace fw = alamo::tactical::fog;

void TacticalBattle::fog_begin(TacticalMode& mode, uint32_t players) {
    fog_on_ = false;
    fog_mode_ = &mode;
    if (!cfg_.fog) return;
    fog_reveal_beh_ = objects::behavior_type_id("REVEAL");
    fog_hide_beh_ = objects::behavior_type_id("HIDE_WHEN_FOGGED");
    fog_team_beh_ = objects::beh::Team;
    fw::FogSetup s;
    s.mode = space_ ? fw::FogMode::Space : fw::FogMode::Land;
    // [port] The playable box stands in for the mode's map extents (vf +0x1f8). Traced (FOG_OF_WAR §1.2 [H], FOWF t01):
    // space uses the playable box, land the whole terrain box [0, 20 W] x [0, 20 H]. The land switch waits for the AI
    // threat grid, which must use the same box (AI_TACTICAL_PERCEPTION §4.1; BattleAi builds it from bounds()): switching
    // the fog alone stalled the FoC Utapau skirmish on 4 of 5 seeds (2026-11-01).
    const tactical::Aabb& b = mode.bounds();
    s.box = {b.min.x, b.min.y, b.max.x, b.max.y};
    s.constants = catalog_->fog_constants();
    s.fow_active = fog_global_active_;
    s.render_option = fog_global_render_;
    const int32_t neutral = galaxy_->neutral_player();  // first player of an Is_Neutral faction: no grid
    for (uint32_t p = 0; p < players; ++p) {
        fw::FogPlayer fp;
        fp.id = static_cast<int32_t>(p);
        fp.grid = fp.id != neutral;
        fp.human = p < galaxy_->players().size() && galaxy_->players()[p].human;
        s.players.push_back(fp);
    }
    fog_.start(s);
    fog_global_active_ = fog_.fow_active();  // land forces both flags on at every battle start
    fog_global_render_ = fog_.render_option();
    for (uint32_t a = 0; a < players; ++a)
        for (uint32_t o = 0; o < players; ++o)
            if (a != o)
                fog_.set_relation(static_cast<int32_t>(a), static_cast<int32_t>(o),
                                  combat_->is_enemy(static_cast<int32_t>(a), static_cast<int32_t>(o)) ? 1 : 0);
    fog_on_ = true;
    // FOWF t03: the setup reveal's "powered" = the target player's base power (TACTICAL_GAPS §7.3, Base_Power_Ability).
    fog_.set_base_power([this](int32_t player) { return combat_ && combat_->has_base_power(player); });
    // fog follow-up (S9): Force Sight channelling = Reveal_All for the owner, undone at its end (FOG_OF_WAR §3.3).
    fog_pings_.clear();
    abilities_->set_fog_reveal_all([this](int32_t player, bool on) {
        if (!fog_on_) return;
        if (on) fog_.reveal_all(player);
        else fog_.undo_reveal_all(player);
    });
}

void TacticalBattle::fog_after_deploy(TacticalMode& mode) {
    if (!fog_on_) return;
    const objects::ObjectStore& store = mode.primary();
    const objects::GameObjectTypes& types = catalog_->types();
    const objects::BehaviorTypeId obstacle = objects::behavior_type_id("SPACE_OBSTACLE");
    // Dense mask (0x35e870): every object (GOM order) whose type is a nebula / asteroid field / ion storm with a
    // SPACE_OBSTACLE behaviour: circle at position + Space_Obstacle_Offset, radius Space_Obstacle_Radius.
    std::vector<fw::FogOfWar::DenseCircle> dense;
    // Battle scale (0x35cd90): 1 + sum of Reveal_Range_Modifier over one side's objects whose type has
    // Modifies_Reveal_Range (GOM order), clamped [0.25, 1]. TACTICAL_GAPS §5.3: land sums the attacker's objects and
    // scales the defender's reveals, space sums the defender's and scales the attacker's.
    const fw::BattleScaleSides sides = fw::battle_scale_sides(fog_.mode(),req_.info.attacker, req_.info.defender);
    std::vector<float> mods;
    for (const objects::ObjectHandle h : store.all_objects()) {
        if (!store.active(h)) continue;  // refix t01: death clones skipped
        const objects::TypeIndex t = store.type(h);
        if (t >= types.size()) continue;
        if (obstacle != objects::kNoBehavior && store.has_behavior(h, obstacle) &&
            (types.get_bool(t, "Is_Asteroid_Field") || types.get_bool(t, "Is_Impassable_Asteroid") || types.get_bool(t, "Is_Ion_Storm") ||
             types.get_bool(t, "Is_Nebula"))) {
            const Vec3 p = store.position(h);
            const std::array<float, 4> off = types.get_vec(t, "Space_Obstacle_Offset");
            dense.push_back({p.x + off[0], p.y + off[1], types.get_float(t, "Space_Obstacle_Radius")});
        }
        if (store.owner(h) == sides.summed && types.get_bool(t, "Modifies_Reveal_Range"))
            mods.push_back(types.get_float(t, "Reveal_Range_Modifier"));
    }
    fog_.set_dense_obstacles(dense);
    fog_.set_battle_scale(sides.scaled, fw::battle_reveal_scale(mods));
    // OI fog: land setup phase (SetupPhaseEnabled, False in FoC / TR) starts with the battle.
    if (!space_ && fog_.constants().setup_phase_enabled) fog_begin_setup_phase();
}

fw::FogObject TacticalBattle::fog_object_of(const objects::ObjectStore& store, objects::ObjectHandle h) const {
    const objects::GameObjectTypes& types = catalog_->types();
    const objects::TypeIndex t = store.type(h);
    fw::FogObject o;
    o.owner = store.owner(h);
    const Vec3 p = store.position(h);
    o.x = p.x;
    o.y = p.y;
    o.z = p.z;
    if (t < types.size()) {
        // Land: Land_FOW_Reveal_Range x weather factor (Int-final: the L1 weather system's Sight_Range_Modifier for the
        // type's Weather_Category, 1.0 while I <= 0.5 / no weather; every owner weather-affected [port]); space:
        // Space_FOW_Reveal_Range.
        o.sight_range = space_ ? types.columns().space_fow_reveal_range[t]
                               : types.columns().land_fow_reveal_range[t] * l1_weather_.sight_modifier(types.get_string(t, "Weather_Category"));
        o.dense_multiplier = std::clamp(types.get_float(t, "Dense_FOW_Reveal_Range_Multiplier"), 0.0f, 1.0f);
    }
    const objects::ObjectId id = store.id(h);
    if (combat_ && combat_->has_unit(id)) o.object_modifier = combat_->modifier(id, tactical::combat::ModList::RevealRange);
    // FOWF t02 (FOG_OF_WAR §3.1 [H]): the footprint box is the type model's local collision bound (ModelClass vf +0x58 =
    // alHModel +0x120: centre offset and half extents, not scaled or rotated); no model -> radius 10 at the position.
    // [port] Without model data for the type (no model files, synthetic data): the mode's per-type half extent around the
    // position, as before.
    if (fog_mode_) {
        const TacticalMode::ModelRef m = fog_mode_->model_ref(t, true);
        if (m.known) {
            o.has_model = m.model != nullptr;
            if (m.model) {
                o.box_cx = m.model->bound_c.x;
                o.box_cy = m.model->bound_c.y;
                o.box_hx = m.model->bound_h.x;
                o.box_hy = m.model->bound_h.y;
            }
        } else {
            const tactical::Aabb bb = fog_mode_->extent_box(h);
            o.has_model = true;
            o.box_cx = 0.5f * (bb.min.x + bb.max.x) - p.x;
            o.box_cy = 0.5f * (bb.min.y + bb.max.y) - p.y;
            o.box_hx = 0.5f * (bb.max.x - bb.min.x);
            o.box_hy = 0.5f * (bb.max.y - bb.min.y);
        }
    }
    return o;
}

// FOWF t02 (FOG_OF_WAR §5.2 [H]): the 8 points of the oriented 2-D footprint rectangle 0x38de80 of Multisample_FOW_Check.
// Half extents 0x371be0: Scale_Factor x Custom_Hard_X/YExtent when both > 0, else each non-positive one from the type
// model's |collision bound half extent| x Scale_Factor (0 without a model); centre = position + the rotated Scale_Factor x
// Custom_Hard_X/YExtent_Offset. [port] The axes come from the normalised facing vector (the original takes cos / sin of
// the yaw in degrees); without model data for the type the mode's per-type half extent stands in for the model part; TEAM
// objects use their type's extents (the team box 0x404450 is not modelled) and the _Deployed extents are not modelled.
void TacticalBattle::fog_footprint_samples(const objects::ObjectStore& store, objects::ObjectHandle h, float* xy) const {
    const objects::GameObjectTypes& types = catalog_->types();
    const objects::TypeIndex t = store.type(h);
    const float sf = types.get_float(t, "Scale_Factor");
    const float scale = sf > 0.0f ? sf : 1.0f;
    const float ex = types.get_float(t, "Custom_Hard_XExtent"), ey = types.get_float(t, "Custom_Hard_YExtent");
    float hx = scale * ex, hy = scale * ey;
    if (!(ex > 0.0f) || !(ey > 0.0f)) {
        float mx = 0.0f, my = 0.0f;
        const TacticalMode::ModelRef m = fog_mode_->model_ref(t, false);
        if (m.known) {
            if (m.model) {
                mx = std::fabs(m.model->bound_h.x);
                my = std::fabs(m.model->bound_h.y);
            }
        } else {
            const tactical::Aabb bb = fog_mode_->extent_box(h);
            mx = 0.5f * (bb.max.x - bb.min.x) / scale;
            my = 0.5f * (bb.max.y - bb.min.y) / scale;
        }
        if (!(ex > 0.0f)) hx = scale * mx;
        if (!(ey > 0.0f)) hy = scale * my;
    }
    const Vec3 f = store.facing(h);
    float ax = 1.0f, ay = 0.0f;
    if (const float len2 = f.x * f.x + f.y * f.y; len2 > 0.0f) {
        const float inv = 1.0f / std::sqrt(len2);
        ax = f.x * inv;
        ay = f.y * inv;
    }
    const float bx = -ay, by = ax;  // b = (-sin, cos)
    const float ox = scale * types.get_float(t, "Custom_Hard_XExtent_Offset");
    const float oy = scale * types.get_float(t, "Custom_Hard_YExtent_Offset");
    const Vec3 p = store.position(h);
    const float cx = p.x + ox * ax + oy * bx, cy = p.y + ox * ay + oy * by;
    // c +- hx a +- hy b (corners), then c + hx a, c - hy b, c - hx a, c + hy b.
    static constexpr float k[8][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}, {1, 0}, {0, -1}, {-1, 0}, {0, 1}};
    for (size_t i = 0; i < 8; ++i) {
        xy[2 * i] = cx + k[i][0] * hx * ax + k[i][1] * hy * bx;
        xy[2 * i + 1] = cy + k[i][0] * hx * ay + k[i][1] * hy * by;
    }
}

void TacticalBattle::fog_decay() {
    if (!fog_on_) return;
    fog_overrun();  // common tick step 3 (OI fog)
    fog_.decay_step(frame_);  // step 4
}

// OI fog: overrun reveal (FOG_OF_WAR §3.3, 0x364920). A = sum of AI_Combat_Power over the record attacker's (+4) live
// battle objects, B = the defender's (+8), unit order [port: the 0x40 / 0x80 / 0x02 flags and the "counting types"
// filter are not modelled; garrisoned units count as contained]. The library gates game type / cadence / time. Game types
// 1 / 2 / 4 are the networked ones (FOG_OF_WAR §3.3 [H]): campaign (0) and local skirmish (3) never run it, and
// multiplayer is out of scope, so the default fog_game_type 0 is the port's only case.
void TacticalBattle::fog_overrun() {
    const int32_t gt = cfg_.fog_game_type;
    if (!cfg_.fog_overrun || !(gt == 1 || gt == 2 || gt == 4) || frame_ % 15u != 0 || !fog_mode_) return;
    const objects::ObjectStore& store = fog_mode_->primary();
    const objects::GameObjectTypes& types = catalog_->types();
    const int32_t p4 = req_.info.attacker, p8 = req_.info.defender;
    float power4 = 0.0f, power8 = 0.0f;
    for (const Unit& u : units_) {
        if (!u.alive || u.retreated || !store.alive(u.h)) continue;
#ifdef ALAMO_SIM_HAVE_GARRISON
        if (garrison_.garrisoned(u.id)) continue;
#endif
        const objects::TypeIndex t = store.type(u.h);
        if (t >= types.size()) continue;
        const int32_t o = store.owner(u.h);
        if (o == p4) power4 += types.get_float(t, "AI_Combat_Power");
        else if (o == p8) power8 += types.get_float(t, "AI_Combat_Power");
    }
    fog_.service_overrun(frame_, gt, cfg_.fog_overrun, p4, power4, p8, power8);
}

// REVEAL services (object-service step): live units in ascending id; dead / retreated ones leave (vf15), garrisoned
// ones undo (vf6 step 2). Order does not matter (FogOfWar is order-independent).
void TacticalBattle::fog_service_sources(const objects::ObjectStore& store) {
    for (const Unit& u : units_) {
        const bool live = u.alive && !u.retreated && store.alive(u.h);
        if (!live) {
            if (fog_.has_source(u.id)) fog_.source_remove(u.id);
            continue;
        }
        if (fog_reveal_beh_ == objects::kNoBehavior || !store.has_behavior(u.h, fog_reveal_beh_)) continue;
        const objects::TypeIndex t = store.type(u.h);
        const bool setup_only = t < catalog_->types().size() && catalog_->types().get_bool(t, "Reveal_During_Setup_Phase_Only");
        bool contained = false;
#ifdef ALAMO_SIM_HAVE_GARRISON
        contained = garrison_.garrisoned(u.id);  // OI fog: obj +0x3a7 (garrisoned / contained)
#endif
        fog_.source_service(u.id, fog_object_of(store, u.h), contained, setup_only);
    }
}

void TacticalBattle::fog_service(TacticalMode& mode) {
    if (!fog_on_) return;
    const objects::ObjectStore& store = mode.primary();
    fog_.set_frame(frame_);
    // Int-final: weather FowDirty (intensity crossed 0.5 / phase type changed, L1_S4_REMAINING §3.2 step 2): every REVEAL
    // source re-reveals with its new sight radius on this service.
    if (fog_weather_dirty_) {
        fog_weather_dirty_ = false;
        fog_.mark_sources_dirty();
    }
    // OI fog: REVEAL vf26 from the post-shot hook (TACTICAL_GAPS §5.1): every shot this tick flashes the shooter's
    // footprint on the target owner's grid (owner at fire time). The shooter needs a REVEAL behaviour (vf26 is its).
    for (const tactical::combat::ShotEvent& e : combat_->shot_events()) {
        const objects::ObjectHandle sh = store.find(e.shooter);
        if (!store.exists(sh) || fog_reveal_beh_ == objects::kNoBehavior || !store.has_behavior(sh, fog_reveal_beh_)) continue;
        fog_.source_fired(fog_object_of(store, sh), e.target_owner);
    }
    fog_service_sources(store);
    // a1h t01: REVEAL (0x18) is a behaviour of the object, serviced every frame whether or not it is a battle unit
    // (FOG_OF_WAR §3.2): map markers and capture points such as Reinforcement_Point (REVEAL, Land_FOW_Reveal_Range 275)
    // reveal for their owner's allies too. A live non-unit REVEAL object without a source joins the list below.
    if (fog_reveal_beh_ != objects::kNoBehavior)
        for (const objects::ObjectHandle h : store.all_objects()) {
            if (!store.alive(h) || !store.has_behavior(h, fog_reveal_beh_)) continue;
            const objects::ObjectId id = store.id(h);
            if (!fog_.has_source(id) && !unit_by_id(id) && std::find(fog_pings_.begin(), fog_pings_.end(), id) == fog_pings_.end())
                fog_pings_.push_back(id);
        }
    // fog follow-up: FOW_REVEAL_PING objects (S9 SpawnObject, e.g. Proj_Special_Land_FOW_Reveal_Ping_Blast) and the
    // other non-unit REVEAL objects reveal through their own REVEAL behaviour while they live (FOG_OF_WAR §3.3 [M]);
    // removed sources are undone, so the cells regrow ~6.4-6.9 s later.
    size_t w = 0;
    for (const objects::ObjectId id : fog_pings_) {
        const objects::ObjectHandle h = store.find(id);
        if (!store.alive(h)) {
            if (fog_.has_source(id)) fog_.source_remove(id);
            continue;
        }
        if (fog_reveal_beh_ != objects::kNoBehavior && store.has_behavior(h, fog_reveal_beh_))
            fog_.source_service(id, fog_object_of(store, h));
        fog_pings_[w++] = id;
    }
    fog_pings_.resize(w);
}

void TacticalBattle::fog_ping(objects::ObjectId id) {
    if (fog_on_) fog_pings_.push_back(id);
}

void TacticalBattle::fog_owner_changed(TacticalMode& mode, objects::ObjectHandle h, int32_t old_owner) {
    if (!fog_on_) return;
    const objects::ObjectStore& store = mode.primary();
    const objects::ObjectId id = store.id(h);
    if (fog_.has_source(id)) fog_.source_change_owner(id, old_owner, fog_object_of(store, h));
}

void TacticalBattle::set_fog_refog(std::function<void(int32_t, uint32_t, float, float)> fn) {
    if (!fn) {
        fog_.set_refog({});
        return;
    }
    fog_.set_refog([this, f = std::move(fn)](int32_t player, uint32_t cell) {
        const fw::FogGrid* g = fog_.grid(player);
        float x = 0.0f, y = 0.0f;
        if (g) g->cell_centre(cell, x, y);
        f(player, cell, x, y);
    });
}

bool TacticalBattle::fog_point_fogged(int32_t player, float x, float y, bool force) const {
    return fog_on_ && fog_.point_fogged(player, x, y, force);
}

bool TacticalBattle::fog_object_hidden(int32_t player, objects::ObjectId id, bool force) const {
    if (!fog_on_ || !fog_mode_) return false;
    const objects::ObjectStore& store = fog_mode_->primary();
    const objects::ObjectHandle h = store.find(id);
    if (!store.alive(h)) return false;
    fw::FogHideQuery q;
    q.owner = store.owner(h);
    q.team_or_hide_when_fogged = store.has_behavior(h, fog_team_beh_) ||
                                 (fog_hide_beh_ != objects::kNoBehavior && store.has_behavior(h, fog_hide_beh_));
    q.has_reveal = fog_reveal_beh_ != objects::kNoBehavior && store.has_behavior(h, fog_reveal_beh_);
    const Vec3 p = store.position(h);
    q.x = p.x;
    q.y = p.y;
    q.z = p.z;
    const objects::TypeIndex t = store.type(h);
    q.multisample = t < catalog_->types().size() && catalog_->types().get_bool(t, "Multisample_FOW_Check");
    if (q.multisample) fog_footprint_samples(store, h, q.corners.data());  // FOWF t02: the footprint rectangle (z = 0)
    return fog_.object_hidden(player, q, force);
}

bool TacticalBattle::fog_object(objects::ObjectId id, fw::FogObject& out) const {
    if (!fog_mode_) return false;
    const objects::ObjectStore& store = fog_mode_->primary();
    const objects::ObjectHandle h = store.find(id);
    if (!store.alive(h)) return false;
    out = fog_object_of(store, h);
    return true;
}

// ---- OI fog: host-level FOW-active flag ------------------------------------------------------------------------

void TacticalBattle::set_fog_active(bool fow_active, bool render_option) {
    if (fog_on_) fog_.set_flags(fow_active, render_option);
    fog_global_active_ = fow_active;
    fog_global_render_ = render_option;
}

bool TacticalBattle::fog_active() const { return fog_on_ && fog_.fow_active(); }
bool TacticalBattle::fog_render_option() const { return fog_on_ && fog_.render_option(); }

// ---- OI fog: land setup phase ----------------------------------------------------------------------------------

// Setup reveal inputs (0x3c01e0), GOM order: every live object of the battle's store.
std::vector<fw::FogSetupObject> TacticalBattle::fog_setup_objects() const {
    std::vector<fw::FogSetupObject> out;
    if (!fog_mode_) return out;
    const objects::ObjectStore& store = fog_mode_->primary();
    const objects::GameObjectTypes& types = catalog_->types();
    const objects::BehaviorTypeId powered = objects::behavior_type_id("POWERED");
    for (const objects::ObjectHandle h : store.all_objects()) {
        if (!store.active(h)) continue;  // refix t01: death clones skipped
        const objects::TypeIndex t = store.type(h);
        if (t >= types.size()) continue;
        fw::FogSetupObject so;
        so.obj = fog_object_of(store, h);
        so.reveal_during_setup = types.get_bool(t, "Reveal_During_Setup_Phase") || types.get_bool(t, "Reveal_During_Setup_Phase_Only");
        so.reveal_for_attacker = types.get_bool(t, "Reveal_For_Attacker");
        so.reveal_for_defender = types.get_bool(t, "Reveal_For_Defender");
        so.has_locomotor = store.has_behavior(h, objects::beh::Locomotor);
        so.has_reveal = fog_reveal_beh_ != objects::kNoBehavior && store.has_behavior(h, fog_reveal_beh_);
        so.reinforcement_prevention_radius = types.get_float(t, "Reinforcement_Prevention_Radius");
        // FOWF t03: obj +0x341 (POWERED slot); the base power itself is the target player's (set_base_power in fog_begin).
        so.has_powered = powered != objects::kNoBehavior && store.has_behavior(h, powered);
        out.push_back(so);
    }
    return out;
}

void TacticalBattle::fog_begin_setup_phase() {
    if (!fog_on_ || space_ || fog_.setup_phase() || !fog_mode_) return;
    // Behaviour Initialise services REVEAL at once; setup start then disables it (TACTICAL_GAPS §5.4).
    fog_.set_frame(frame_);
    fog_service_sources(fog_mode_->primary());
    const std::vector<fw::FogSetupObject> objs = fog_setup_objects();
    fog_.begin_setup(objs, req_.info.attacker, req_.info.defender);
}

void TacticalBattle::fog_end_setup_phase() {
    if (!fog_on_ || !fog_.setup_phase()) return;
    fog_.set_frame(frame_);
    const std::vector<fw::FogSetupObject> objs = fog_setup_objects();
    fog_.end_setup(objs, req_.info.attacker, req_.info.defender);
}

bool TacticalBattle::fog_setup_phase() const { return fog_on_ && fog_.setup_phase(); }

// FOWF t05: the setup manager's end test, common-tick step 11 (after the GOM service): every human ready, or the countdown
// expired.
void TacticalBattle::fog_setup_tick() {
    if (fog_on_ && fog_.setup_end_due(frame_)) fog_end_setup_phase();
}

// FOWF t05 (FOG_OF_WAR §3.4 "Manager details"): command 32 SetupPhaseTriggerEnd (0x5aef60) and command 31 SetupPhaseMove
// (0x5aec70).
commands::RejectReason TacticalBattle::fog_setup_command(TacticalMode& mode, uint8_t player, const commands::EventData& ev) {
    using R = commands::RejectReason;
    if (!fog_on_) return R::NotAllowed;
    if (ev.type() == commands::EventType::SetupPhaseTriggerEnd) {  // ready byte, then the ready handler (no phase test)
        fog_.set_frame(frame_);
        fog_.setup_player_ready(static_cast<int32_t>(player));
        return R::None;
    }
    // Command 31: the object to the point, facing the angle (degrees). [port] Only while the setup phase runs and only the
    // issuer's own live objects (the original's execute tests neither; its UI only drags own units during setup); z from
    // the ground when the map has terrain (terrain snap 0x42bb40, tilt not modelled); the setup group's pick-up / drop
    // presentation (0x3bfb10 / 0x3c0020) is not modelled.
    if (!fog_.setup_phase()) return R::NotAllowed;
    const uint32_t w = ev.u("object");
    if (w == commands::kNoObject || w > 0x7FFFFFFFu) return R::InvalidUnit;
    const objects::ObjectStore& st = mode.primary();
    const objects::ObjectHandle h = st.find(static_cast<objects::ObjectId>(w));
    if (!st.active(h) || st.owner(h) != static_cast<int32_t>(player)) return R::InvalidUnit;
    const float x = ev.f("x"), y = ev.f("y"), angle = ev.f("angle");
    float z = ev.f("z");
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(angle)) return R::InvalidPosition;
    if (const float g = ground_height(x, y); g > -1.0e38f) z = g;
    float sn = 0.0f, cs = 1.0f;
    dmath::sincos(angle * (dmath::kPiF / 180.0f), sn, cs);
    return teleport(mode, st.id(h), {x, y, z}, {cs, sn, 0.0f}) ? R::None : R::InvalidUnit;
}

// ---- OI fog: player reveal-range list --------------------------------------------------------------------------

void TacticalBattle::fog_add_reveal_multiplier(int32_t player, int32_t category, uint64_t source, float multiplier) {
    if (fog_on_) fog_.add_player_reveal_multiplier(player, category, source, multiplier);
}

void TacticalBattle::fog_remove_reveal_modifier(int32_t player, uint64_t source) {
    if (fog_on_) fog_.remove_player_reveal_modifier(player, source);
}

// ---- OI fog: space reinforcement sampler -----------------------------------------------------------------------

// GOM list +0x2f0 (FOWF t04, FOG_OF_WAR §9 item 10 [H]): the objects registered in the store whose type has a
// Reinforcement_Enabling / Prevention / Base_Shield radius > 0, until their flush (dead objects not yet flushed and death
// clones included); the prevention test keeps those with a prevention radius > 0. GOM order.
std::vector<fw::PreventionCircle> TacticalBattle::fog_prevention_circles() const {
    std::vector<fw::PreventionCircle> out;
    if (!fog_mode_) return out;
    const objects::ObjectStore& store = fog_mode_->primary();
    const objects::GameObjectTypes& types = catalog_->types();
    for (const objects::ObjectHandle h : store.all_objects()) {
        if (!store.exists(h)) continue;
        const objects::TypeIndex t = store.type(h);
        if (t >= types.size()) continue;
        const float r = types.get_float(t, "Reinforcement_Prevention_Radius");
        if (!(r > 0.0f)) continue;
        const Vec3 p = store.position(h);
        out.push_back({store.owner(h), p.x, p.y, r});
    }
    return out;
}

bool TacticalBattle::fog_reinforce_overlay(int32_t player, std::vector<uint8_t>& out) const {
    if (!fog_on_) {
        out.clear();
        return false;
    }
    const std::vector<fw::PreventionCircle> c = fog_prevention_circles();
    return fog_.reinforce_overlay(player, c, out);
}

bool TacticalBattle::fog_reinforce_point_ok(int32_t player, float x, float y) const {
    if (!fog_on_) return true;
    const std::vector<fw::PreventionCircle> c = fog_prevention_circles();
    return fog_.reinforce_point_ok(player, x, y, c);
}

#endif

}  // namespace alamo::sim
