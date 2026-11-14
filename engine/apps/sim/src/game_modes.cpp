#include "alamo/sim/game_modes.hpp"
#include "alamo/core/sim_crc.hpp"

#include <algorithm>
#include <cmath>

#include "alamo/core/dmath.hpp"
#include "alamo/core/profile.hpp"
#include "alamo/galactic/campaign.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/tactical/movement/space_avoid.hpp"

namespace alamo::sim {

namespace {

constexpr uint32_t kNoStep = 0xFFFFFFFFu;

bool finite(float f) { return f - f == 0.0f; }

void hash_vec(StateHasher& h, const Vec3& v) {
    h.f32(v.x);
    h.f32(v.y);
    h.f32(v.z);
}

}  // namespace

// ---------------------------------------------------------------------------------------- GameMode
GameMode::GameMode(ModeKind kind, const objects::GameObjectTypes& types, objects::IdCounter& ids)
    : kind_(kind),
      types_(&types),
      primary_(kind, objects::StoreKind::Primary, types, &ids),
      secondary_(kind, objects::StoreKind::Secondary, types) {
    // --- OI objects: behaviour Initialise reads the mode frame (+0x10) for the first due frame (TACTICAL_GAPS 6.4) ---
    primary_.bind_mode_frame(&frame_);
    secondary_.bind_mode_frame(&frame_);
    // --- end OI objects ---
}

const char* GameMode::name() const {
    switch (kind_) {
        case ModeKind::Galactic: return "galactic";
        case ModeKind::Land: return "land";
        case ModeKind::Space: return "space";
    }
    return "?";
}

void GameMode::trace(const ModeTickContext& ctx, const char* step) {
    if (ctx.trace) ctx.trace->emplace_back(step);
}

void GameMode::common_tick(const ModeTickContext& ctx) {
    trace(ctx, "common.begin");
    // 4. FOW: players whose id = F (mod 16) update their grid this tick [stub: counted only].
    if (tactical()) {
        for (uint32_t p = 0; p < ctx.players; ++p)
            if (p % 16u == static_cast<uint32_t>(frame_ % 16u)) ++fow_updates_;
        trace(ctx, "common.fow");
    }
    // 8. GOM service: primary store, then secondary store (OBJECT_RUNTIME.md §5.3).
    const auto mode_frame = static_cast<uint32_t>(frame_);
    primary_.update(
        mode_frame, [this, &ctx](objects::ObjectStore& s, ObjectHandle h) { service_object(s, h, ctx); },
        [this, &ctx](objects::ObjectStore& s) { pre_service(s, ctx); });
    trace(ctx, "common.store.primary");
    if (ctx.object_scripts && !paused()) {  // q6: object scripts after the store update (OBJECT_SCRIPTS.md §3.1) [port]
        ctx.object_scripts(*this);
        trace(ctx, "common.object_scripts");
    }
    secondary_.update(mode_frame, {}, {});
    trace(ctx, "common.store.secondary");
    after_store_update(ctx);
    // 9-16: retreat coordinator, victory monitor, bombing / bombard managers [stubs].
    trace(ctx, "common.end");
}

void GameMode::hash(StateHasher& h) const {
    h.u8(static_cast<uint8_t>(kind_));
    h.u64(frame_);
    h.u64(ticks_);
    h.u64(fow_updates_);
    h.boolean(paused_);
}

// N1 save: kind (checked on read), mode frame, tick / FOW counters, paused + suspended flags.
void GameMode::write_state(ByteWriter& w) const {
    w.u8(static_cast<uint8_t>(kind_)).u64(frame_).u64(ticks_).u64(fow_updates_).boolean(paused_).boolean(suspended_);
}

bool GameMode::read_state(ByteReader& r) {
    const uint8_t kind = r.u8();
    const uint64_t frame = r.u64(), ticks = r.u64(), fow = r.u64();
    const bool paused = r.boolean(), suspended = r.boolean();
    if (!r.ok() || kind != static_cast<uint8_t>(kind_)) return false;
    frame_ = frame;
    ticks_ = ticks;
    fow_updates_ = fow;
    paused_ = paused;
    suspended_ = suspended;
    return true;
}

// ------------------------------------------------------------------------------------ GalacticMode
GalacticMode::GalacticMode(const objects::GameObjectTypes& types, objects::IdCounter& ids)
    : GameMode(ModeKind::Galactic, types, ids) {}

GalacticMode::~GalacticMode() = default;

void GalacticMode::attach_sim(std::unique_ptr<galactic::GalacticSim> sim) {
    sim_ = std::move(sim);
    if (sim_)
        for (uint32_t i = 0; i < pause_count_; ++i) sim_->push_pause();
}

void GalacticMode::push_pause() {
    ++pause_count_;
    if (sim_) sim_->push_pause();
}

void GalacticMode::pop_pause() {
    if (pause_count_ == 0) return;
    --pause_count_;
    if (sim_) sim_->pop_pause();
}

bool GalacticMode::paused() const { return paused_ || pause_count_ > 0 || (sim_ && sim_->paused()); }

void GalacticMode::tick(const ModeTickContext& ctx) {
    ++ticks_;
    trace(ctx, "galactic.begin");
    if (sim_) {
        // S3: GalacticSim::update is the galactic mode tick (GALACTIC_ECONOMY.md 5.1); it services
        // the primary store itself and does nothing while the pause counter is raised.
        sim_->update();
        profile::lap("gal.sim");
        trace(ctx, "galactic.sim");
        if (ctx.object_scripts && !paused()) {  // q6: object scripts after the sim's store service
            ctx.object_scripts(*this);
            trace(ctx, "common.object_scripts");
        }
        secondary_.update(static_cast<uint32_t>(frame_), {}, {});
        trace(ctx, "galactic.end");
        return;
    }
    // No GalacticSim attached (tests only): the common tick FIRST in galactic mode (MAIN_LOOP.md §3.2), nothing else
    // (cu9 t14 / cu9f t01: the pre-S3 income stand-in and its hashed / saved state are gone; GalacticSim owns the fiscal cycle).
    common_tick(ctx);
    trace(ctx, "galactic.end");
}

void GalacticMode::hash(StateHasher& h) const {
    GameMode::hash(h);
    h.u32(pause_count_);
}

void GalacticMode::write_state(ByteWriter& w) const {
    GameMode::write_state(w);
    w.u32(pause_count_).boolean(sim_ != nullptr);
}

bool GalacticMode::read_state(ByteReader& r) {
    // The galactic tail is parsed on a copy first (GameMode::read_state commits), so bad data leaves the mode unchanged.
    constexpr size_t kGameModeStateBytes = 1 + 8 + 8 + 8 + 1 + 1;  // GameMode::write_state
    ByteReader probe = r;
    (void)probe.raw(kGameModeStateBytes);
    const uint32_t pauses = probe.u32();
    const bool has_sim = probe.boolean();
    if (!probe.ok() || has_sim != (sim_ != nullptr)) return false;
    if (!GameMode::read_state(r)) return false;
    r = probe;
    pause_count_ = pauses;  // not re-pushed into the GalacticSim (its pause state is its own chunk)
    return true;
}

// ------------------------------------------------------------------------------------ TacticalMode
TacticalMode::TacticalMode(ModeKind kind, const objects::GameObjectTypes& types, objects::IdCounter& ids,
                           uint32_t players)
    : GameMode(kind, types, ids), trees_(players) {
    const float base = kind == ModeKind::Space ? 40.0f : 10.0f;
    half_extent_.resize(types.size());
    scale_.resize(types.size());
    box_modifier_.resize(types.size());
    team_.resize(types.size());
    shielded_.resize(types.size());
    for (objects::TypeIndex t = 0; t < types.size(); ++t) {
        float sc = types.columns().scale_factor[t];
        if (!(sc > 0.0f) || !finite(sc)) sc = 1.0f;
        scale_[t] = sc;
        const float hx = types.get_float(t, "Custom_Hard_XExtent");
        const float hy = types.get_float(t, "Custom_Hard_YExtent");
        float e = hx > hy ? hx : hy;
        if (!(e > 0.0f) || !finite(e)) e = base * sc;
        half_extent_[t] = e;
        box_modifier_[t] = types.get_float(t, "Collision_Box_Modifier");
        team_[t] = types.has_behavior(t, objects::BehaviorSet::Union, objects::beh::Team) ? 1 : 0;
        shielded_[t] = types.has_behavior(t, objects::BehaviorSet::Union, objects::beh::Shielded) ? 1 : 0;
    }
}

tactical::Aabb TacticalMode::extent_box(ObjectHandle h) const {
    const Vec3 p = primary_.position(h);
    const objects::TypeIndex t = primary_.type(h);
    const float e = t < half_extent_.size() ? half_extent_[t] : 1.0f;
    return {{p.x - e, p.y - e, p.z - e}, {p.x + e, p.y + e, p.z + e}};
}

void TacticalMode::set_model_source(ModelSource src) {
    model_source_ = std::move(src);
    models_.assign(types_->size(), ModelRef{});
    model_done_.assign(types_->size(), 0);
    models_dirty_ = true;
}

TacticalMode::ModelRef TacticalMode::model_ref(objects::TypeIndex t, bool resolve) const {
    if (!model_source_ || t >= model_done_.size()) return {};
    if (!model_done_[t]) {
        if (!resolve) return {};
        models_[t] = model_source_(t);
        model_done_[t] = 1;
    }
    return models_[t];
}

void TacticalMode::resolve_models() {
    if (!models_dirty_ || !model_source_) return;
    models_dirty_ = false;
    for (const ObjectHandle h : primary_.all_objects())
        if (primary_.exists(h)) (void)model_ref(primary_.type(h), true);
}

bool TacticalMode::obstacle_of(ObjectHandle h, tactical::movement::AvoidObstacle& o, bool resolve) const {
    const objects::TypeIndex t = primary_.type(h);
    const ModelRef m = model_ref(t, resolve);
    if (!m.known) return false;
    const Vec3 p = primary_.position(h), f = primary_.facing(h);
    o.id = primary_.id(h);
    o.pos = {p.x, p.y, p.z};
    o.facing = {f.x, f.y};
    o.scale = scale_[t];
    o.model = m.model;
    o.skip_shield = shielded_[t] != 0;
    o.collide = true;
    tactical::movement::avoid_place(o);
    return true;
}

void TacticalMode::prepare_bounds(ObjectHandle h) const {
    const objects::TypeIndex t = primary_.type(h);
    if (model_source_ && t < team_.size() && !team_[t]) (void)model_ref(t, true);
}

tactical::Aabb TacticalMode::bounds_of(ObjectHandle h) const {
    const objects::TypeIndex t = primary_.type(h);
    if (!model_source_ || t >= team_.size() || team_[t]) return extent_box(h);
    tactical::movement::AvoidObstacle o;
    if (!obstacle_of(h, o, true)) return extent_box(h);
    return {{o.box_c.x - o.box_h.x, o.box_c.y - o.box_h.y, o.box_c.z - o.box_h.z},
            {o.box_c.x + o.box_h.x, o.box_c.y + o.box_h.y, o.box_c.z + o.box_h.z}};
}

bool TacticalMode::movable(ObjectHandle h) const { return primary_.has_behavior(h, objects::beh::Locomotor); }

uint32_t TacticalMode::category_of(ObjectHandle h) const {
    if (movable(h)) return kCatUnit;
    if (primary_.has_behavior(h, objects::beh::Selectable)) return kCatStructure;
    return kCatProp;
}

ObjectHandle TacticalMode::spawn(const objects::SpawnParams& p, float health_fraction) {
    const ObjectHandle h = primary_.create(p);
    if (!finite(health_fraction)) health_fraction = 1.0f;
    const objects::TypeIndex t = p.type;
    const float max_health = t < types_->size() ? types_->columns().tactical_health[t] : 0.0f;
    primary_.set_health(h, health_fraction * max_health);
    tactical::TreeMembership m;
    m.owner = p.owner;
    m.projectile_collidable = t < types_->size() && types_->get_bool(t, "Collidable_By_Projectile_Living");
    const tactical::Aabb box = bounds_of(h);
    if (tactical::valid(box)) trees_.add(static_cast<tactical::EntityHandle>(primary_.id(h)), box, category_of(h), m);
    return h;
}

namespace {
constexpr objects::BehaviorTypeId kCloneKeepHideWhenFogged = 0x19, kCloneKeepImposingPresence = 0x40, kCloneDeathBehavior = 0x44;
}  // namespace

ObjectHandle TacticalMode::spawn_death_clone(const objects::SpawnParams& p) {
    // q6 (OBJECT_SCRIPTS.md §1.2): a death clone's script is released at once, so it never runs; [port] the create hook
    // is held back for the clone (no attach at all, so its Base_Definitions does not run either).
    auto created_hook = std::move(primary_.on_created);
    primary_.on_created = nullptr;
    const ObjectHandle h = primary_.create(p);
    primary_.on_created = std::move(created_hook);
    primary_.set_flag(h, objects::kFlagDeathClone);
    const objects::TypeIndex t = p.type;
    primary_.set_health(h, t < types_->size() ? types_->columns().tactical_health[t] : 0.0f);
    tactical::TreeMembership m;
    m.owner = p.owner;
    m.projectile_collidable = t < types_->size() && types_->get_bool(t, "Collidable_By_Projectile_Dead");
    const tactical::Aabb box = bounds_of(h);
    if (tactical::valid(box)) trees_.add(static_cast<tactical::EntityHandle>(primary_.id(h)), box, category_of(h), m);
    // S8 t09 (S9_DEATH_CLONE_SHIELDS §1.6 [H], MOVEMENT §1.3): right after the create, 0x29f270 detaches and removes every
    // behaviour of the clone, last to first, except HIDE_WHEN_FOGGED (0x19), IMPOSING_PRESENCE (0x40) and LAND_OBSTACLE
    // (0x14; only when the clone type's Death_Clone_Is_Obstacle, default true, is set), then attaches DEATH (0x44). So a
    // clone never targets, moves, spawns, shields or holds a space obstacle entry, and it is a land obstacle (stamped by
    // the host's land_obstacles_sync) iff it kept LAND_OBSTACLE. The attach draws of the full list were made by the create,
    // as in Create_Object. [port] the tree category above is taken from the full list (the registration order inside
    // Create_Object was not traced); the detach side effects (vf15) are nothing here: a clone has no system state yet.
    const bool keep_obstacle = t < types_->size() && types_->get_bool(t, "Death_Clone_Is_Obstacle");
    for (size_t k = primary_.behaviors(h).size(); k-- > 0;) {
        const objects::BehaviorTypeId b = primary_.behaviors(h)[k];
        if (b == kCloneKeepHideWhenFogged || b == kCloneKeepImposingPresence || (b == objects::beh::LandObstacle && keep_obstacle))
            continue;
        primary_.remove_behavior(h, static_cast<uint32_t>(k));
    }
    primary_.add_behavior(h, kCloneDeathBehavior);
    return h;
}

bool TacticalMode::destroy(ObjectHandle h) {
    if (!primary_.destroy(h)) return false;
    const ObjectId id = primary_.id(h);
    trees_.set_dead(static_cast<tactical::EntityHandle>(id), true);
    dying_.push_back(id);
    return true;
}

void TacticalMode::finish_load(const ParallelFor& pf) {
    models_dirty_ = true;  // refixf t07: the loaded objects' types resolve at the next resolve_models()
    trees_.rebuild_all_with_threshold(tactical::SpatialTreeConfig{}.large_threshold, pf);
}

void TacticalMode::set_order(ObjectId id, const UnitOrder& o) {
    if (o.kind == OrderKind::Idle) orders_.erase(id);
    else orders_[id] = o;
}

const UnitOrder* TacticalMode::order(ObjectId id) const {
    const auto it = orders_.find(id);
    return it == orders_.end() ? nullptr : &it->second;
}

void TacticalMode::tick(const ModeTickContext& ctx) {
    ++ticks_;
    trace(ctx, "tactical.begin");
    // 1. Cinematic manager step [stub]. 2. Per AI player: tactical AI data service [stub].
    trace(ctx, "tactical.ai_data");
    // 3. Movement / pathfinding scratch reset; the stub locomotor plans this tick's steps here
    // from start-of-tick positions.
    plan_motion(ctx);
    trace(ctx, "tactical.motion_plan");
    // 4. Land: weather + hero clash (LandMode::mode_specific; the weather itself runs in TacticalBattle).
    mode_specific(ctx);
    // 5. Tactical frame counter. 6. Victory-monitor event 0x11 per player [stub].
    ++tactical_frame_;
    // 7. Common tick LAST.
    common_tick(ctx);
    trace(ctx, "tactical.end");
}

void TacticalMode::compute_step(Step& s) const {
    const UnitOrder& o = *s.order;
    const Vec3 pos = primary_.position(s.h);
    s.pos = pos;
    s.facing = primary_.facing(s.h);
    Vec3 goal = o.goal;
    float stop = 0.0f;
    const objects::TypeIndex t = primary_.type(s.h);
    if (o.kind != OrderKind::Move) {
        const ObjectHandle th = primary_.find(o.target);
        if (!primary_.alive(th)) {
            s.lost_target = 1;
            return;
        }
        goal = primary_.position(th);
        if (o.kind == OrderKind::Attack) {
            stop = t < types_->size() ? types_->columns().targeting_max_attack_distance[t] : 0.0f;
            if (!(stop > 0.0f)) stop = 100.0f;
        } else {
            stop = 50.0f;
        }
    }
    const float dx = goal.x - pos.x, dy = goal.y - pos.y, dz = goal.z - pos.z;
    const float dist = dmath::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist <= stop) {
        if (o.kind == OrderKind::Attack) s.in_range = 1;
        else s.arrived = 1;
        return;
    }
    if (!movable(s.h)) return;  // turrets / structures: attack orders hold, moves are no-ops
    const float speed = t < types_->size() ? types_->columns().max_speed[t] : 0.0f;
    if (!(speed > 0.0f)) return;
    const float travel = dist - stop;
    const float inv = 1.0f / dist;
    s.facing = {dx * inv, dy * inv, dz * inv};
    s.moved = 1;
    if (travel <= speed) {
        if (o.kind == OrderKind::Move) {
            s.pos = goal;
            s.arrived = 1;
        } else {
            const float k = travel * inv;
            s.pos = {pos.x + dx * k, pos.y + dy * k, pos.z + dz * k};
            if (o.kind == OrderKind::Attack) s.in_range = 1;
            else s.arrived = 1;
        }
    } else {
        const float k = speed * inv;
        s.pos = {pos.x + dx * k, pos.y + dy * k, pos.z + dz * k};
    }
}

void TacticalMode::plan_motion(const ModeTickContext& ctx) {
    steps_.clear();
    for (const auto& [id, o] : orders_) {  // ascending object id
        Step s;
        s.id = id;
        s.h = primary_.find(id);
        s.order = &o;
        if (!primary_.alive(s.h)) s.lost_target = 1;  // the unit itself is gone: drop the order
        steps_.push_back(s);
    }
    step_of_slot_.assign(primary_.capacity(), kNoStep);
    for (uint32_t i = 0; i < steps_.size(); ++i)
        if (!steps_[i].lost_target) step_of_slot_[steps_[i].h.slot()] = i;
    // Per-index outputs only: identical for every worker count.
    auto body = [this](uint32_t b, uint32_t e) {
        for (uint32_t i = b; i < e; ++i)
            if (!steps_[i].lost_target) compute_step(steps_[i]);
    };
    const auto n = static_cast<uint32_t>(steps_.size());
    if (ctx.pf && *ctx.pf) (*ctx.pf)(n, 64, body);
    else serial_for(n, 64, body);
}

void TacticalMode::pre_service(objects::ObjectStore& store, const ModeTickContext& ctx) {
    if (&store != &primary_) return;
    // Store update step 1: refit the spatial trees for boxes changed since the last refit.
    std::sort(moved_.begin(), moved_.end());
    moved_.erase(std::unique(moved_.begin(), moved_.end()), moved_.end());
    for (ObjectId id : moved_) {
        const ObjectHandle h = primary_.find(id);
        if (primary_.exists(h)) trees_.update(static_cast<tactical::EntityHandle>(id), bounds_of(h));
    }
    moved_.clear();
    trace(ctx, "store.refit");
}

void TacticalMode::service_object(objects::ObjectStore& store, ObjectHandle h, const ModeTickContext&) {
    if (&store != &primary_) return;
    // Object Service: locomotion (stub) for objects with a planned step.
    const uint32_t slot = h.slot();
    if (slot >= step_of_slot_.size() || step_of_slot_[slot] == kNoStep) return;
    const Step& s = steps_[step_of_slot_[slot]];
    if (!s.moved) return;
    primary_.set_position(h, s.pos);
    primary_.set_facing(h, s.facing);
    moved_.push_back(s.id);
}

void TacticalMode::after_store_update(const ModeTickContext&) {
    // Order bookkeeping in ascending id order (steps_ is in id order).
    for (const Step& s : steps_) {
        auto it = orders_.find(s.id);
        if (it == orders_.end()) continue;
        if (s.lost_target || s.arrived) orders_.erase(it);
        else it->second.in_range = s.in_range;
    }
    steps_.clear();
    // Objects unlinked by the flush leave the trees.
    for (ObjectId id : dying_) {
        if (primary_.exists(primary_.find(id))) continue;
        trees_.remove(static_cast<tactical::EntityHandle>(id));
        orders_.erase(id);
    }
    dying_.erase(std::remove_if(dying_.begin(), dying_.end(),
                                [this](ObjectId id) { return !primary_.exists(primary_.find(id)); }),
                 dying_.end());
}

void TacticalMode::hash(StateHasher& h) const {
    GameMode::hash(h);
    h.u64(tactical_frame_);
    h.u64(ai_services_);
    h.u32(static_cast<uint32_t>(orders_.size()));
    for (const auto& [id, o] : orders_) {
        h.i32(id);
        h.u8(static_cast<uint8_t>(o.kind));
        h.u8(o.player);
        h.u8(o.in_range);
        h.u64(o.issued_frame);
        hash_vec(h, o.goal);
        h.i32(o.target);
    }
}

// N1 stage 2 (owner: n2-host). The trees are the host's "spatial" chunk; steps_ / step_of_slot_ are per-tick scratch
// (cleared by after_store_update), half_extent_ is a definition.
void TacticalMode::write_state(ByteWriter& w) const {
    GameMode::write_state(w);
    w.u64(tactical_frame_).u64(ai_services_);
    for (const tactical::Vec3& c : {bounds_.min, bounds_.max}) w.f32(c.x).f32(c.y).f32(c.z);
    w.u32(static_cast<uint32_t>(orders_.size()));
    for (const auto& [id, o] : orders_) {
        w.i32(id).u8(static_cast<uint8_t>(o.kind)).u8(o.player).u8(o.in_range).u64(o.issued_frame);
        w.f32(o.goal.x).f32(o.goal.y).f32(o.goal.z).i32(o.target);
    }
    for (const std::vector<ObjectId>* ids : {&moved_, &dying_}) {
        w.u32(static_cast<uint32_t>(ids->size()));
        for (const ObjectId id : *ids) w.i32(id);
    }
}

bool TacticalMode::read_state(ByteReader& r) {
    // The tactical tail is parsed on a copy first (GameMode::read_state commits), so bad data leaves the mode unchanged.
    constexpr size_t kGameModeStateBytes = 1 + 8 + 8 + 8 + 1 + 1;  // GameMode::write_state
    ByteReader probe = r;
    (void)probe.raw(kGameModeStateBytes);
    const uint64_t tframe = probe.u64(), services = probe.u64();
    tactical::Aabb bounds;
    for (tactical::Vec3* c : {&bounds.min, &bounds.max}) {
        c->x = probe.f32();
        c->y = probe.f32();
        c->z = probe.f32();
    }
    std::map<ObjectId, UnitOrder> orders;
    const uint32_t n = probe.u32();
    if (!probe.ok() || n > probe.remaining()) return false;
    for (uint32_t i = 0; i < n && probe.ok(); ++i) {
        const ObjectId id = probe.i32();
        UnitOrder o;
        const uint8_t kind = probe.u8();
        if (kind > static_cast<uint8_t>(OrderKind::Attack)) probe.fail();
        o.kind = static_cast<OrderKind>(kind);
        o.player = probe.u8();
        o.in_range = probe.u8();
        o.issued_frame = probe.u64();
        o.goal.x = probe.f32();
        o.goal.y = probe.f32();
        o.goal.z = probe.f32();
        o.target = probe.i32();
        orders[id] = o;
    }
    std::vector<ObjectId> lists[2];
    for (std::vector<ObjectId>& ids : lists) {
        const uint32_t m = probe.u32();
        if (!probe.ok() || m > probe.remaining()) return false;
        ids.resize(m);
        for (ObjectId& id : ids) id = probe.i32();
    }
    if (!probe.ok()) return false;
    if (!GameMode::read_state(r)) return false;
    r = probe;
    tactical_frame_ = tframe;
    ai_services_ = services;
    bounds_ = bounds;
    orders_ = std::move(orders);
    moved_ = std::move(lists[0]);
    dying_ = std::move(lists[1]);
    steps_.clear();
    return true;
}

void LandMode::mode_specific(const ModeTickContext& ctx) {
    // Trace step only (cu9 t15): the land weather service runs in the battle (TacticalBattle::l1_weather_tick, before
    // the common tick, with its synced draws), then the hero clashes (TacticalBattle::hero_clash_tick, s78n t06).
    trace(ctx, "land.weather");
}

// ------------------------------------------------------------------------------- GameModeManager
GameMode& GameModeManager::push(ModeKind kind) {
    if (!stack_.empty()) stack_.front()->suspend();
    std::unique_ptr<GameMode> m;
    switch (kind) {
        case ModeKind::Galactic: m = std::make_unique<GalacticMode>(*types_, ids_); break;
        case ModeKind::Land: m = std::make_unique<LandMode>(*types_, ids_, players_); break;
        case ModeKind::Space: m = std::make_unique<SpaceMode>(*types_, ids_, players_); break;
    }
    GameMode& ref = *m;
    stack_.insert(stack_.begin(), std::move(m));
    ref.init();
    if (ref.tactical() && stack_.size() > 1) {
        objects::ObjectStore* parent = &stack_[1]->primary();
        ref.primary().on_parent_mode_death = [parent](const objects::ObjectStore& child, ObjectHandle dying) {
            objects::mark_parent_mode_object_for_death(*parent, child, dying);
        };
    }
    return ref;
}

bool GameModeManager::pop() {
    if (stack_.empty()) return false;
    stack_.front()->shutdown();
    stack_.erase(stack_.begin());
    if (!stack_.empty()) stack_.front()->resume();
    return true;
}

TacticalMode* GameModeManager::top_tactical() {
    GameMode* t = top();
    return t && t->tactical() ? static_cast<TacticalMode*>(t) : nullptr;
}

const TacticalMode* GameModeManager::top_tactical() const {
    const GameMode* t = top();
    return t && t->tactical() ? static_cast<const TacticalMode*>(t) : nullptr;
}

void GameModeManager::update(const ModeTickContext& ctx) {
    // 1. countdown [stub]; 2. scoring-manager service (q6 t09: ModeTickContext::scoring); 3. ONLY the current mode ticks.
    if (ctx.scoring) ctx.scoring();
    if (ctx.trace) ctx.trace->emplace_back("gmm.scoring");
    if (GameMode* t = top()) t->tick(ctx);
    // 4-5. vote manager, MP battle-vote timeout [stubs].
}

void GameModeManager::post_tick() {
    if (GameMode* t = top()) t->post_tick();
}

}  // namespace alamo::sim
