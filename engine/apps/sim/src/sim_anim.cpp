#include "alamo/sim/sim_anim.hpp"

#include <algorithm>

#include "alamo/core/dmath.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/sim/sim_host.hpp"

namespace alamo::sim {

namespace {

std::string upper_key(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return out;
}

anim::FlinchParams flinch_of(const anim::TypeAnimParams& p) {
    anim::FlinchParams f;
    f.blend_seconds = p.max_flinch_blend_seconds;
    f.hold_seconds = p.max_flinch_hold_seconds;
    f.idle_interval_seconds = p.idle_flinch_interval_seconds;
    f.attack_interval_seconds = p.attack_flinch_interval_seconds;
    return f;
}

anim::DeathParams death_of(const anim::TypeAnimParams& p) {
    anim::DeathParams d;
    d.type = p.death_type;
    d.type_explicit = p.death_type_explicit;
    d.index = p.death_index;
    d.remove_upon_death = p.remove_upon_death;
    d.persistence_seconds = p.death_persistence_seconds;
    d.fade_seconds = p.death_fade_seconds;
    d.spin_away = p.spin_away_on_death;
    d.spin_away_chance = p.spin_away_chance;
    d.spin_away_seconds = p.spin_away_seconds;
    return d;
}

// Position, yaw facing (the model's nose -Y along it, z up) and Scale_Factor [port: no pitch / roll] - the battle's fire-bone rule.
anim::Mat34 object_matrix(const float p[3], const float f[3], float s) {
    if (!(s > 0.0f)) s = 1.0f;
    float fx = f[0], fy = f[1];
    const float len2 = fx * fx + fy * fy;
    if (!(len2 > 0.0f)) {
        fx = 1.0f;
        fy = 0.0f;
    } else {
        const float inv = 1.0f / dmath::sqrt(len2);
        fx = fx * inv;
        fy = fy * inv;
    }
    // Model columns: X = left (-fy, fx), Y = -facing (Alamo models face -Y), Z = up - render::object_transform's frame.
    anim::Mat34 m;
    m.m[0][0] = -fy * s;
    m.m[0][1] = -fx * s;
    m.m[0][2] = 0.0f;
    m.m[0][3] = p[0];
    m.m[1][0] = fx * s;
    m.m[1][1] = -fy * s;
    m.m[1][2] = 0.0f;
    m.m[1][3] = p[1];
    m.m[2][0] = 0.0f;
    m.m[2][1] = 0.0f;
    m.m[2][2] = s;
    m.m[2][3] = p[2];
    return m;
}

}  // namespace

SimAnimation::SimAnimation(const objects::GameObjectTypes& types, const FileProvider& files, anim::AnimTunables tunables)
    : types_(&types), files_(&files), tunables_(tunables) {}

void SimAnimation::attach(SimHost& host, bool crc) {
    host.set_object_animation_hook([this](SimHost& h) { service(h); });
    if (crc) host.crc_registry().add("anim", [this, &host](StateHasher& h) { hash(host, h); });
    // N1 (owner: n2-host): chunk "anim" (sim_anim.hpp save layout).
    SaveParticipant p;
    p.name = "anim";
    p.write = [this, &host](ByteWriter& w) { write(host, w); };
    p.read = [this, &host](ByteReader& r) { return read(host, r); };
    host.remove_save_participant(p.name);
    host.add_save_participant(std::move(p));
}

// ------------------------------------------------------------------------------------ N1 save / load
namespace {
constexpr uint32_t kAnimChunkVersion = 1;
constexpr uint32_t kMaxAnimCount = 1u << 24;

void write_id_list(ByteWriter& w, const std::vector<ObjectId>& ids) {
    w.u32(static_cast<uint32_t>(ids.size()));
    for (const ObjectId id : ids) w.i32(id);
}

bool read_id_list(ByteReader& r, std::vector<ObjectId>& ids) {
    const uint32_t n = r.u32();
    if (!r.ok() || n > kMaxAnimCount || static_cast<uint64_t>(n) * 4u > r.remaining()) return false;
    ids.assign(n, 0);
    for (ObjectId& id : ids) id = r.i32();
    return r.ok();
}
}  // namespace

void SimAnimation::write(const SimHost& host, ByteWriter& w) const {
    const GameModeManager& modes = host.modes();
    w.u32(kAnimChunkVersion);
    const auto depth = static_cast<uint32_t>(modes.depth());
    w.u32(depth);
    for (uint32_t i = depth; i-- > 0;) {  // bottom -> top (the CRC order)
        const ModeWorld* mw = find_mode_world(modes.at(i));
        w.boolean(mw != nullptr);
        if (!mw) continue;
        write_section(w, [mw](ByteWriter& sub) { mw->behaviors->write(sub); });
        write_section(w, [mw](ByteWriter& sub) { mw->world->write(sub); });
        w.i32(mw->max_seen);
        write_id_list(w, mw->pending_clone_deaths);
        write_id_list(w, mw->event_rows);
        w.u32(static_cast<uint32_t>(mw->muzzles.size()));
        for (const Muzzle& m : mw->muzzles) w.i32(m.id).i32(m.hardpoint).i32(m.bone).u64(m.until);
    }
}

bool SimAnimation::read(SimHost& host, ByteReader& r) {
    GameModeManager& modes = host.modes();
    if (r.u32() != kAnimChunkVersion) return false;
    const uint32_t depth = r.u32();
    if (!r.ok() || depth != modes.depth()) return false;
    struct ModeIn {
        bool has = false;
        std::span<const uint8_t> behaviors, world;
        ObjectId max_seen = INT32_MIN;
        std::vector<ObjectId> clone_deaths, event_rows;
        std::vector<Muzzle> muzzles;
    };
    std::vector<ModeIn> in(depth);
    for (uint32_t k = 0; k < depth; ++k) {  // k = 0: bottom
        ModeIn& m = in[k];
        m.has = r.boolean();
        if (!r.ok()) return false;
        if (!m.has) continue;
        m.behaviors = r.raw(r.u32());
        m.world = r.raw(r.u32());
        m.max_seen = r.i32();
        if (!r.ok() || !read_id_list(r, m.clone_deaths) || !read_id_list(r, m.event_rows)) return false;
        const uint32_t nm = r.u32();
        if (!r.ok() || nm > kMaxAnimCount || static_cast<uint64_t>(nm) * 20u > r.remaining()) return false;
        m.muzzles.resize(nm);
        for (Muzzle& mz : m.muzzles) {
            mz.id = r.i32();
            mz.hardpoint = r.i32();
            mz.bone = r.i32();
            mz.until = r.u64();
        }
    }
    if (!r.ok() || !r.done()) return false;
    // The model of a saved row: the store object's type (the creation rule of sync()), else the corpse record's.
    auto model_of = [this](const GameMode& mode, const anim::AnimBehaviors& beh) {
        return [this, &mode, &beh](ObjectId id) -> const anim::AnimModel* {
            const objects::ObjectStore& store = mode.primary();
            const ObjectHandle h = store.find(id);
            if (store.exists(h)) return model_for(store.type(h), mode.kind());
            if (const anim::Corpse* c = beh.corpse(id)) return model_for(c->info.object_type, mode.kind());
            return nullptr;
        };
    };
    // Validated on scratch copies first (the live worlds are referenced by battle hooks / script bridges, so they are
    // read in place afterwards and must not be replaced).
    for (uint32_t k = 0; k < depth; ++k) {
        const ModeIn& m = in[k];
        if (!m.has) continue;
        const GameMode& mode = modes.at(depth - 1 - k);
        anim::AnimBehaviors beh;
        ByteReader br(m.behaviors);
        if (!beh.read(br) || !br.ok() || !br.done()) return false;
        anim::AnimWorld world(tunables_);
        ByteReader wr(m.world);
        if (!world.read(wr, model_of(mode, beh)) || !wr.ok() || !wr.done()) return false;
    }
    for (uint32_t k = 0; k < depth; ++k) {
        const ModeIn& m = in[k];
        const GameMode& mode = modes.at(depth - 1 - k);
        if (!m.has) {
            worlds_.erase(std::remove_if(worlds_.begin(), worlds_.end(), [&mode](const ModeWorld& w) { return w.mode == &mode; }),
                          worlds_.end());
            continue;
        }
        ModeWorld& mw = mode_world(mode);
        ByteReader br(m.behaviors), wr(m.world);
        if (!mw.behaviors->read(br) || !mw.world->read(wr, model_of(mode, *mw.behaviors))) return false;  // validated above
        mw.max_seen = m.max_seen;  // sync() creates rows above it only: the saved rows are not re-created (no synced draws)
        mw.pending_clone_deaths = m.clone_deaths;
        mw.event_rows = m.event_rows;
        mw.muzzles = m.muzzles;
        mw.events.clear();
        // The secondary (cosmetic) store is not saved: its rows are created again at the next service, with the FREE RNG.
        mw.max_seen_secondary = INT32_MIN;
    }
    return true;
}

void SimAnimation::detach(SimHost& host) {
    host.set_object_animation_hook({});
    host.remove_save_participant("anim");
}

SimAnimation::ModeWorld& SimAnimation::mode_world(const GameMode& mode) {
    for (ModeWorld& w : worlds_)
        if (w.mode == &mode) return w;
    ModeWorld w;
    w.mode = &mode;
    w.world = std::make_unique<anim::AnimWorld>(tunables_);
    w.behaviors = std::make_unique<anim::AnimBehaviors>();
    w.secondary = std::make_unique<anim::AnimWorld>(tunables_);
    worlds_.push_back(std::move(w));
    return worlds_.back();
}

const SimAnimation::ModeWorld* SimAnimation::find_mode_world(const GameMode& mode) const {
    for (const ModeWorld& w : worlds_)
        if (w.mode == &mode) return &w;
    return nullptr;
}

anim::AnimWorld& SimAnimation::world(const GameMode& mode) { return *mode_world(mode).world; }

const anim::AnimWorld* SimAnimation::find_world(const GameMode& mode) const {
    const ModeWorld* w = find_mode_world(mode);
    return w ? w->world.get() : nullptr;
}

const anim::AnimWorld* SimAnimation::find_secondary_world(const GameMode& mode) const {
    const ModeWorld* w = find_mode_world(mode);
    return w ? w->secondary.get() : nullptr;
}

anim::AnimBehaviors& SimAnimation::behaviors(const GameMode& mode) { return *mode_world(mode).behaviors; }

const anim::AnimBehaviors* SimAnimation::find_behaviors(const GameMode& mode) const {
    const ModeWorld* w = find_mode_world(mode);
    return w ? w->behaviors.get() : nullptr;
}

std::span<const anim::Corpse> SimAnimation::corpses(const GameMode& mode) const {
    const anim::AnimBehaviors* b = find_behaviors(mode);
    return b ? b->corpses() : std::span<const anim::Corpse>{};
}

std::span<const SimAnimation::Muzzle> SimAnimation::muzzles(const GameMode& mode) const {
    const ModeWorld* w = find_mode_world(mode);
    return w ? std::span<const Muzzle>(w->muzzles) : std::span<const Muzzle>{};
}

std::span<const SimAnimation::FrameEvent> SimAnimation::events(const GameMode& mode) const {
    const ModeWorld* w = find_mode_world(mode);
    return w ? std::span<const FrameEvent>(w->events) : std::span<const FrameEvent>{};
}

const anim::TypeAnimParams& SimAnimation::params_for(objects::TypeIndex type, objects::ModeKind mode) {
    model_for(type, mode);
    return types_cache_[(static_cast<uint64_t>(mode) << 32) | type].params;
}

void SimAnimation::set_model(objects::TypeIndex type, objects::ModeKind mode, const anim::AnimModel* model) {
    TypeEntry e;
    if (type < types_->size()) e.params = anim::type_anim_params(*types_, type, mode);
    e.model = model;
    types_cache_[(static_cast<uint64_t>(mode) << 32) | type] = std::move(e);
}

const anim::AnimModel* SimAnimation::model_for(objects::TypeIndex type, objects::ModeKind mode) {
    const uint64_t key = (static_cast<uint64_t>(mode) << 32) | type;
    const auto it = types_cache_.find(key);
    if (it != types_cache_.end()) return it->second.model;
    TypeEntry e;
    if (type < types_->size()) e.params = anim::type_anim_params(*types_, type, mode);
    if (!e.params.model.empty()) {
        const std::string mk = upper_key(e.params.model) + "|" + upper_key(e.params.anim_override);
        auto& slot = models_[mk];
        if (!slot) {
            slot = std::make_unique<anim::AnimModel>();
            anim::load_anim_model(*files_, e.params.model, e.params.anim_override, *slot, &clips_);
            ++stats_.models;
            if (slot->has_any_clip()) ++stats_.models_with_clips;
        }
        // Particle ALOs and models without bones or clips get no row (nothing sim-visible to animate).
        if (slot->has_skeleton || slot->has_any_clip()) e.model = slot.get();
    }
    const anim::AnimModel* m = e.model;
    types_cache_.emplace(key, std::move(e));
    return m;
}

void SimAnimation::prune(const SimHost& host) {
    const GameModeManager& modes = host.modes();
    worlds_.erase(std::remove_if(worlds_.begin(), worlds_.end(),
                                 [&modes](const ModeWorld& w) {
                                     for (size_t i = 0; i < modes.depth(); ++i)
                                         if (&modes.at(i) == w.mode) return false;
                                     return true;
                                 }),
                  worlds_.end());
}

anim::IdleParams SimAnimation::idle_params(const GameMode& mode, ObjectId id, objects::TypeIndex t, const anim::AnimModel* m) {
    const anim::TypeAnimParams& p = params_for(t, mode.kind());
    anim::IdleParams i;
    i.idle_type = anim::kAnimIdle;
    i.loop_idle = p.loop_idle;
    i.idle_rate = p.idle_rate;
    // [port] Space Idle (doc §C item 4: where it is selected is unknown): a space-mode model without IDLE clips but
    // with SPACE_IDLE clips idles on Space Idle.
    if (mode.kind() == objects::ModeKind::Space && m && m->variant_count(anim::kAnimIdle) == 0 && m->variant_count(anim::kAnimSpaceIdle) > 0)
        i.idle_type = anim::kAnimSpaceIdle;
    // Garrison container mapping (0x395920) through the hook (S4 garrison system).
    if (idle_hook_) {
        const int32_t g = idle_hook_(mode, id);
        if (g != anim::kNoAnimType && m && m->variant_count(g) > 0) i.idle_type = g;
    }
    return i;
}

// ---- R3 behaviours -------------------------------------------------------------------------------------

bool SimAnimation::hit(const GameMode& mode, ObjectId victim, Vec3 facing, Vec3 travel, SimRng& rng, uint64_t frame) {
    ModeWorld& mw = mode_world(mode);
    if (!mw.world->contains(victim)) return false;
    const objects::ObjectStore& store = mode.primary();
    const ObjectHandle h = store.find(victim);
    if (!store.alive(h)) return false;
    const objects::TypeIndex t = store.type(h);
    const anim::TypeAnimParams& p = params_for(t, mode.kind());
    const bool played = mw.behaviors->on_hit(*mw.world, victim, flinch_of(p), idle_params(mode, victim, t, mw.world->model(victim)), facing.x,
                                             facing.y, travel.x, travel.y, rng, frame);
    if (played) ++stats_.flinches;
    return played;
}

bool SimAnimation::service_idle(const GameMode& mode, ObjectId id, SimRng& rng, uint64_t frame) {
    ModeWorld& mw = mode_world(mode);
    if (!mw.world->contains(id)) return false;
    const objects::ObjectStore& store = mode.primary();
    const ObjectHandle h = store.find(id);
    if (!store.exists(h)) return false;
    return mw.behaviors->service_idle(*mw.world, id, idle_params(mode, id, store.type(h), mw.world->model(id)), rng, frame);
}

uint32_t SimAnimation::service_locomotor(const GameMode& mode, ObjectId id, anim::LocomotorInput in, float walk_ratio, float min_ratio, SimRng& rng,
                                         uint64_t frame) {
    ModeWorld& mw = mode_world(mode);
    if (!mw.world->contains(id)) return 0;
    const objects::ObjectStore& store = mode.primary();
    const ObjectHandle h = store.find(id);
    if (!store.exists(h)) return 0;
    const objects::TypeIndex t = store.type(h);
    const anim::TypeAnimParams& tp = params_for(t, mode.kind());
    anim::LocomotorParams lp;
    lp.movement_anim_speed = tp.movement_anim_speed;
    lp.deployed_walk_anim_speed = tp.deployed_walk_anim_speed;
    lp.walk_speed_ratio = walk_ratio;
    lp.min_move_ratio = min_ratio;
    lp.idle = idle_params(mode, id, t, mw.world->model(id));
    if (idle_hook_ && in.idle_type == anim::kNoAnimType) in.idle_type = idle_hook_(mode, id);
    const uint32_t n = mw.behaviors->service_locomotor(*mw.world, id, lp, in, rng, frame);
    if (n > 0 && anim::is_turn(mw.world->current_type(id))) ++stats_.turns;
    return n;
}

bool SimAnimation::deploy(const GameMode& mode, ObjectId id, bool undeploy, SimRng& rng, uint64_t frame) {
    ModeWorld& mw = mode_world(mode);
    const objects::ObjectStore& store = mode.primary();
    const ObjectHandle h = store.find(id);
    if (!mw.world->contains(id) || !store.exists(h)) return false;
    const bool played = mw.behaviors->deploy(*mw.world, id, undeploy, params_for(store.type(h), mode.kind()).deployment_anim_rate, rng, frame);
    if (played) ++stats_.deploys;
    return played;
}

void SimAnimation::set_berserker(const GameMode& mode, ObjectId id, bool active) { mode_world(mode).world->set_berserker(id, active); }

void SimAnimation::set_frozen(const GameMode& mode, ObjectId id, bool frozen) { mode_world(mode).world->set_frozen(id, frozen); }

void SimAnimation::muzzle_flash(const GameMode& mode, ObjectId id, int32_t hardpoint, int32_t bone, uint64_t frame) {
    // Shown now, re-hidden after 4 ticks (§A.7 attack row [M]).
    std::vector<Muzzle>& v = mode_world(mode).muzzles;
    const auto less = [](const Muzzle& m, ObjectId i, int32_t hp, int32_t b) {
        if (m.id != i) return m.id < i;
        if (m.hardpoint != hp) return m.hardpoint < hp;
        return m.bone < b;
    };
    auto it = v.begin();
    while (it != v.end() && less(*it, id, hardpoint, bone)) ++it;
    if (it != v.end() && it->id == id && it->hardpoint == hardpoint && it->bone == bone) {
        it->until = frame + 4;
    } else {
        Muzzle m;
        m.id = id;
        m.hardpoint = hardpoint;
        m.bone = bone;
        m.until = frame + 4;
        v.insert(it, m);
    }
    ++stats_.muzzle_flashes;
}

anim::DeathResult SimAnimation::start_death(ModeWorld& mw, const GameMode& mode, ObjectId id, SimRng& rng, uint64_t frame, uint8_t damage_kind) {
    const objects::ObjectStore& store = mode.primary();
    const ObjectHandle h = store.find(id);
    const objects::TypeIndex t = store.type(h);
    anim::CorpseInfo info;
    info.object_type = static_cast<uint32_t>(t);
    info.owner = store.owner(h);
    const Vec3 p = store.position(h), f = store.facing(h);
    info.position[0] = p.x;
    info.position[1] = p.y;
    info.position[2] = p.z;
    info.facing[0] = f.x;
    info.facing[1] = f.y;
    info.facing[2] = f.z;
    const anim::TypeAnimParams& tp = params_for(t, mode.kind());
    const anim::DeathResult r = mw.behaviors->begin_death(*mw.world, id, death_of(tp), info, rng, frame, damage_kind);
    ++stats_.deaths;
    if (r == anim::DeathResult::RemoveNow) {
        ++stats_.removed_at_death;
    } else if (const anim::Corpse* c = mw.behaviors->corpse(id)) {
        if (c->death_type != anim::kNoAnimType) ++stats_.death_clips;
        if (c->death_type != anim::kNoAnimType && c->death_type != tp.death_type) ++stats_.kind_death_types;
        if (c->spin_away) ++stats_.spin_aways;
    }
    return r;
}

anim::DeathResult SimAnimation::begin_death(const GameMode& mode, ObjectId id, SimRng& rng, uint64_t frame, uint8_t damage_kind) {
    const objects::ObjectStore& store = mode.primary();
    if (!store.exists(store.find(id))) return anim::DeathResult::NoRow;
    return start_death(mode_world(mode), mode, id, rng, frame, damage_kind);
}

void SimAnimation::death_clone_spawned(const GameMode& mode, ObjectId clone) { mode_world(mode).pending_clone_deaths.push_back(clone); }

// ---- animation events ----------------------------------------------------------------------------------

const anim::AnimEventMap& SimAnimation::event_map() {
    if (!events_loaded_) {
        events_loaded_ = true;
        std::vector<uint8_t> bytes;
        if (files_->read("Data\\XML\\AnimationSFXMaps.txt", bytes))
            events_map_.parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    }
    return events_map_;
}

void SimAnimation::set_event_map(anim::AnimEventMap map) {
    events_map_ = std::move(map);
    events_loaded_ = true;
    model_events_.clear();
    clip_events_.clear();
}

bool SimAnimation::model_has_events(const anim::AnimModel* m) {
    if (!m) return false;
    const auto it = model_events_.find(m);
    if (it != model_events_.end()) return it->second;
    const anim::AnimEventMap& map = event_map();
    bool any = false;
    for (int32_t t = 0; t < static_cast<int32_t>(anim::kAnimTypeCount) && !any; ++t)
        for (const anim::ClipRef& c : m->clips[static_cast<size_t>(t)])
            if (c.clip && map.find(c.clip->name)) {
                any = true;
                break;
            }
    model_events_.emplace(m, any);
    return any;
}

void SimAnimation::collect_events(ModeWorld& mw, const GameMode& mode) {
    // After the advance: compare each snapshot with the slot now (same clip only; a play since the snapshot
    // cannot happen inside the advance).
    mw.events.clear();
    const objects::ObjectStore& store = mode.primary();
    const auto& scale = types_->columns().scale_factor;
    for (const EventSnap& s : snaps_) {
        const anim::SlotView now = mw.world->slot(s.id);
        if (now.clip != s.clip || now.type != s.type || now.variant != s.variant) continue;
        const auto ce = clip_events_.find(s.clip->clip.get());
        if (ce == clip_events_.end() || !ce->second) continue;
        scratch_hits_.clear();
        const bool wrapped = s.loop && now.frame < s.frame;
        anim::crossed_events(*ce->second, s.clip->clip->fps, s.clip->clip->frame_count, s.frame, now.frame, wrapped, scratch_hits_);
        if (scratch_hits_.empty()) continue;
        // Object transform: the live store object, else the corpse record.
        float pos[3] = {0.0f, 0.0f, 0.0f}, fac[3] = {1.0f, 0.0f, 0.0f};
        uint32_t type = 0xFFFFFFFFu;
        bool corpse = false;
        const ObjectHandle h = store.find(s.id);
        if (store.exists(h)) {
            const Vec3 p = store.position(h), f = store.facing(h);
            pos[0] = p.x, pos[1] = p.y, pos[2] = p.z;
            fac[0] = f.x, fac[1] = f.y, fac[2] = f.z;
            type = static_cast<uint32_t>(store.type(h));
        } else if (const anim::Corpse* c = mw.behaviors->corpse(s.id)) {
            for (int k = 0; k < 3; ++k) {
                pos[k] = c->info.position[k];
                fac[k] = c->info.facing[k];
            }
            type = c->info.object_type;
            corpse = true;
        } else {
            continue;
        }
        const anim::Mat34 m = object_matrix(pos, fac, type < scale.size() ? scale[type] : 1.0f);
        const anim::AnimModel* model = mw.world->model(s.id);
        for (uint32_t i : scratch_hits_) {
            const anim::AnimEventDef& d = (*ce->second)[i];
            FrameEvent e;
            e.id = s.id;
            e.def = &d;
            e.corpse = corpse || mw.behaviors->is_corpse(s.id);
            e.position = Vec3{pos[0], pos[1], pos[2]};
            if (!d.bone.empty() && model) {
                e.bone = model->skeleton.find(d.bone);
                anim::Float3 bp;
                if (e.bone >= 0 && mw.world->bone_world_position(s.id, e.bone, m, bp)) e.position = Vec3{bp.x, bp.y, bp.z};
            }
            mw.events.push_back(e);
            ++stats_.events;
            if (d.kind == anim::AnimEventDef::Kind::Surface && d.name == "INFANTRY_FOOT_PLANT") ++stats_.foot_plants;
        }
    }
}

// ---- service -------------------------------------------------------------------------------------------

void SimAnimation::sync(const GameMode& mode, SimRng& rng, uint64_t frame) {
    ModeWorld& mw = mode_world(mode);
    anim::AnimWorld& w = *mw.world;
    anim::AnimBehaviors& beh = *mw.behaviors;
    const objects::ObjectStore& store = mode.primary();
    // Removals: rows whose object left the store (flushed); corpse rows stay (detached).
    scratch_remove_.clear();
    for (ObjectId id : w.ids())
        if (!store.exists(store.find(id))) scratch_remove_.push_back(id);
    for (ObjectId id : scratch_remove_) {
        if (beh.is_corpse(id)) {
            beh.detach(id);
            continue;
        }
        w.remove(id);
        beh.remove(id);
        ++stats_.removed;
    }
    scratch_remove_.clear();  // row-less corpses (death clones without a model) whose object left too
    for (const anim::Corpse& c : beh.corpses())
        if (!c.detached && !store.exists(store.find(c.id))) scratch_remove_.push_back(c.id);
    for (ObjectId id : scratch_remove_) beh.detach(id);
    // Creations in ascending id (store order is not guaranteed to be id order after flushes). Ids come from the
    // session's monotonic IdCounter, so only ids above the last sync's maximum can be new: older objects either
    // have a row already or never get one (type without an animated model) - same result as rescanning all.
    scratch_ids_.clear();
    ObjectId max_id = mw.max_seen;
    for (ObjectHandle h : store.all_objects()) {
        if (!store.alive(h)) continue;
        const ObjectId id = store.id(h);
        if (id > mw.max_seen) scratch_ids_.push_back(id);
        max_id = std::max(max_id, id);
    }
    mw.max_seen = max_id;
    std::sort(scratch_ids_.begin(), scratch_ids_.end());
    for (ObjectId id : scratch_ids_) {
        if (w.contains(id) || beh.is_corpse(id)) continue;
        const ObjectHandle h = store.find(id);
        const objects::TypeIndex t = store.type(h);
        const anim::AnimModel* m = model_for(t, mode.kind());
        if (!m) continue;
        const anim::IdleParams ip = idle_params(mode, id, t, m);
        anim::AnimCreateParams cp;
        cp.idle_type = ip.idle_type;  // default idle (Space Idle / garrison mapping above; crouch: not at creation)
        cp.loop_idle = ip.loop_idle;
        cp.idle_rate = ip.idle_rate;
        w.create(id, m, cp, &rng, frame);
        ++stats_.created;
        if (model_has_events(m)) mw.event_rows.push_back(id);  // ascending: ids above every earlier one
    }
    // Death clones spawned since the last sync die now (request order), after their model creation.
    for (ObjectId id : mw.pending_clone_deaths) {
        if (!store.alive(store.find(id)) || beh.is_corpse(id)) continue;
        ++stats_.clone_deaths;
        if (start_death(mw, mode, id, rng, frame) == anim::DeathResult::RemoveNow) scratch_destroy_.push_back(id);
    }
    mw.pending_clone_deaths.clear();
    stats_.rows = static_cast<uint32_t>(w.size());
}

void SimAnimation::sync_secondary(ModeWorld& mw, const GameMode& mode, SimRng& rng, uint64_t frame) {
    // Secondary (local) store: model created with the FREE RNG (§A.7 / §A.8), removed with the object.
    anim::AnimWorld& w = *mw.secondary;
    const objects::ObjectStore& store = mode.secondary();
    scratch_remove_.clear();
    for (ObjectId id : w.ids())
        if (!store.exists(store.find(id))) scratch_remove_.push_back(id);
    for (ObjectId id : scratch_remove_) w.remove(id);
    scratch_ids_.clear();
    ObjectId max_id = mw.max_seen_secondary;
    for (ObjectHandle h : store.all_objects()) {
        if (!store.alive(h)) continue;
        const ObjectId id = store.id(h);
        if (id > mw.max_seen_secondary) scratch_ids_.push_back(id);
        max_id = std::max(max_id, id);
    }
    mw.max_seen_secondary = max_id;
    std::sort(scratch_ids_.begin(), scratch_ids_.end());
    for (ObjectId id : scratch_ids_) {
        if (w.contains(id)) continue;
        const objects::TypeIndex t = store.type(store.find(id));
        const anim::AnimModel* m = model_for(t, mode.kind());
        if (!m) continue;
        const anim::TypeAnimParams& p = params_for(t, mode.kind());
        anim::AnimCreateParams cp;
        cp.loop_idle = p.loop_idle;
        cp.idle_rate = p.idle_rate;
        w.create(id, m, cp, &rng, frame);
    }
    stats_.secondary_rows = static_cast<uint32_t>(w.size());
}

void SimAnimation::service(SimHost& host) {
    prune(host);
    GameMode* top = host.modes().top();
    if (!top) return;
    const uint64_t frame = host.frame();
    scratch_destroy_.clear();
    sync(*top, host.random().synced(), frame);
    ModeWorld& mw = mode_world(*top);
    sync_secondary(mw, *top, host.random().free(), frame);
    // [port] frozen mode: while the top mode is paused (galactic pause counter while a conflict is decided,
    // Lua pause) the GOM services objects "frozen" (MAIN_LOOP R0): no behaviour service and no clip advance
    // (object +0x3a7 [L] read as this state); the first advance after it catches up (dt = t - last).
    if (top->paused()) {
        mw.events.clear();
        return;
    }
    const size_t at_death = scratch_destroy_.size();
    mw.behaviors->service(*mw.world, frame, scratch_destroy_);  // flinch holds, corpse countdown
    stats_.corpses_removed += static_cast<uint32_t>(scratch_destroy_.size() - at_death);
    // DeathBehavior removes finished corpses whose object is still in the store (death clones), and clones
    // removed at once (Remove_Upon_Death without a death clip).
    objects::ObjectStore& store = top->primary();
    TacticalMode* tm = host.modes().top_tactical();
    for (ObjectId id : scratch_destroy_) {
        const ObjectHandle h = store.find(id);
        if (!store.alive(h)) continue;
        if (tm && static_cast<GameMode*>(tm) == top) tm->destroy(h);
        else store.destroy(h);
    }
    // Muzzles re-hidden after their 4 ticks.
    mw.muzzles.erase(std::remove_if(mw.muzzles.begin(), mw.muzzles.end(), [frame](const Muzzle& m) { return m.until <= frame; }),
                     mw.muzzles.end());
    stats_.corpses = static_cast<uint32_t>(mw.behaviors->corpses().size());
    stats_.rows = static_cast<uint32_t>(mw.world->size());
    // Event snapshot of the rows whose clip has events, then the advance, then the crossings.
    snaps_.clear();
    {
        size_t keep = 0;
        for (size_t i = 0; i < mw.event_rows.size(); ++i) {
            const ObjectId id = mw.event_rows[i];
            if (!mw.world->contains(id)) continue;
            mw.event_rows[keep++] = id;
            const anim::SlotView s = mw.world->slot(id);
            if (!s.clip || s.mode == anim::SlotMode::Finished) continue;
            auto ce = clip_events_.find(s.clip->clip.get());
            if (ce == clip_events_.end()) ce = clip_events_.emplace(s.clip->clip.get(), event_map().find(s.clip->clip->name)).first;
            if (!ce->second) continue;
            snaps_.push_back({id, s.clip, s.type, s.variant, s.frame, s.mode == anim::SlotMode::Loop});
        }
        mw.event_rows.resize(keep);
    }
    mw.world->advance(frame, host.parallel_for());
    mw.secondary->advance(frame, host.parallel_for());
    collect_events(mw, *top);
}

void SimAnimation::hash(const SimHost& host, StateHasher& h) const {
    // Primary worlds + behaviours only: secondary stores, muzzles and events are local / visual.
    const GameModeManager& modes = host.modes();
    const auto depth = static_cast<uint32_t>(modes.depth());
    h.u32(depth);
    for (uint32_t i = depth; i-- > 0;) {  // bottom -> top
        const ModeWorld* w = find_mode_world(modes.at(i));
        h.boolean(w != nullptr);
        if (w) {
            w->world->hash_state(h);
            w->behaviors->hash(h);
        }
    }
}

}  // namespace alamo::sim
