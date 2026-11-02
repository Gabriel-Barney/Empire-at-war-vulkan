// R5 sim-driven terrain decorations. See sim_decorations.hpp.
#include "alamo/render/sim_decorations.hpp"

#include <algorithm>
#include <cmath>

namespace alamo::render {

namespace {

std::string upper(std::string_view s) {
    std::string o(s);
    for (char& c : o)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return o;
}

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 'a' + 'A');
        if (y >= 'a' && y <= 'z') y = static_cast<char>(y - 'a' + 'A');
        if (x != y) return false;
    }
    return true;
}

constexpr double kTicksPerSecond = 30.0;

}  // namespace

SimDecorations::SimDecorations(const SurfaceFxLibrary& lib, const SnapshotTypeTable& table, const terrain::Terrain* terrain,
                               uint64_t seed)
    : lib_(&lib), table_(&table), terrain_(terrain && terrain->present && terrain->width >= 2 ? terrain : nullptr), decals_(seed) {}

void SimDecorations::update(const InterpolatedFrame& frame) {
    if (started_ && frame.tick <= last_tick_) return;
    const uint64_t ticks = started_ ? frame.tick - last_tick_ : 1;
    const float dt = static_cast<float>(std::min<double>(static_cast<double>(ticks) / kTicksPerSecond, 1.0));
    const float now = static_cast<float>(static_cast<double>(frame.tick) / kTicksPerSecond);
    started_ = true;
    last_tick_ = frame.tick;
    ++generation_;
    ++stats_.ticks;
    stats_.track_objects = stats_.faded_only = stats_.surface_particles = 0;
    effects_.clear();
    auto remember = [&] {
        prev_ids_.clear();
        prev_pos_.clear();
        prev_spin_.clear();
        for (const SnapshotObject& o : frame.objects) {  // frame objects ascend by id
            prev_ids_.push_back(o.id);
            prev_pos_.push_back(o.position);
            if (o.flags & kSnapSpinAway) prev_spin_.push_back(o);
        }
    };
    auto add_puff = [&](std::string asset, const Vec3& pos, const Vec3& facing) {
        Puff p;
        p.fx.key = (0xF00Full << 44) | (next_puff_++ & ((1ull << 44) - 1));
        p.fx.asset = std::move(asset);
        p.fx.position = pos;
        p.fx.facing = facing;
        p.until = now + kFootEffectSeconds;
        puffs_.push_back(std::move(p));
    };
    auto finish_effects = [&] {
        std::erase_if(puffs_, [now](const Puff& p) { return p.until <= now; });
        for (const Puff& p : puffs_) effects_.push_back(p.fx);
        std::stable_sort(effects_.begin(), effects_.end(), [](const SimWorldEffect& a, const SimWorldEffect& b) { return a.key < b.key; });
    };
    // ---- R3 spin-away corpses that ended this tick: Spin_Away_On_Death_Explosion at their last position (land and space).
    // Store death clones are skipped: the battle spawns their explosion as a sim PARTICLE object (R3 t04).
    for (const SnapshotObject& p : prev_spin_) {
        if (p.flags & kSnapDeathClone) continue;
        const auto it = std::lower_bound(frame.objects.begin(), frame.objects.end(), p.id,
                                         [](const SnapshotObject& o, objects::ObjectId v) { return o.id < v; });
        if (it != frame.objects.end() && it->id == p.id) continue;
        if (!table_->valid(p.type) || table_->operator[](p.type).spin_away_explosion.empty()) continue;
        add_puff(particle_asset(table_->operator[](p.type).spin_away_explosion), p.position, p.facing);
        ++stats_.spin_explosions;
    }
    if (!terrain_) {
        gens_.clear();
        finish_effects();
        remember();
        return;
    }
    const terrain::Terrain& t = *terrain_;
    for (const SnapshotObject& o : frame.objects) {
        if (!table_->valid(o.type)) continue;
        const SnapshotTypeInfo& ti = (*table_)[o.type];
        const std::string_view surface = surface_type_name(terrain_surface_type(t, o.position.x, o.position.y));
        // ---- SurfaceFX particles (wakes / splashes) while the object moves fast enough on the surface
        if (!ti.surface_fx.empty() && !(o.flags & (kSnapDying | kSnapContained))) {
            float speed = 0.0f;
            const auto pit = std::lower_bound(prev_ids_.begin(), prev_ids_.end(), o.id);
            if (pit != prev_ids_.end() && *pit == o.id && dt > 0.0f) {
                const Vec3& p0 = prev_pos_[static_cast<size_t>(pit - prev_ids_.begin())];
                const float dx = o.position.x - p0.x, dy = o.position.y - p0.y;
                speed = std::sqrt(dx * dx + dy * dy) / dt;
            }
            const float energy = ti.max_speed > 0.0f ? speed / ti.max_speed : (speed > 0.0f ? 1.0f : 0.0f);
            for (const std::string& name : ti.surface_fx) {
                const SurfaceFxDef* fx = lib_->find_surface_fx(name);
                const SurfaceSettings* ss = fx ? fx->find(surface) : nullptr;
                if (!ss || ss->particle.empty()) continue;
                if (energy >= ss->particle_min_energy) {
                    SimWorldEffect w;
                    w.key = (static_cast<uint64_t>(static_cast<uint32_t>(o.id)) << 8) | 0x01u;  // one wake per object
                    w.asset = particle_asset(ss->particle);
                    w.owner = o.id;
                    // R4 scalar setter (SurfaceFX: index 0 = intensity, s = 0.1) [port: intensity = energy clamped to
                    // [0, 1]; the energy -> intensity map of 0x5ee570 is not traced].
                    w.intensity = std::clamp(energy, 0.0f, 1.0f);
                    effects_.push_back(std::move(w));
                    ++stats_.surface_particles;
                }
                break;
            }
        }
        // ---- scorch marks (objects new this frame)
        if (options_.scorch && !ti.scorch_mark.empty() && !std::binary_search(prev_ids_.begin(), prev_ids_.end(), o.id)) {
            ++stats_.scorch_seen;
            const SurfaceFxDef* fx = lib_->find_surface_fx(ti.scorch_mark);
            const SurfaceSettings* ss = fx ? fx->find(surface) : nullptr;
            const DecalFxDef* dd = ss ? lib_->find_decal(ss->decal) : nullptr;
            if (!dd) {
                ++stats_.scorch_no_decal;
            } else {
                const float heading = std::atan2(o.facing.y, o.facing.x);
                if (decals_.spawn(t, *dd, o.position.x, o.position.y, o.position.z, heading, now) == DecalSpawn::Spawned)
                    ++stats_.scorch_spawned;
                else
                    ++stats_.scorch_rejected;
            }
        }
        // ---- dynamic tracks
        if (!options_.tracks || ti.surface_fx.empty()) continue;
        const SurfaceSettings* chosen = nullptr;
        const terrain::DynamicTrackParams* params = nullptr;
        for (const std::string& name : ti.surface_fx) {
            const SurfaceFxDef* fx = lib_->find_surface_fx(name);
            const SurfaceSettings* ss = fx ? fx->find(surface) : nullptr;
            if (!ss || ss->dynamic_track.empty()) continue;
            const terrain::DynamicTrackParams* p = lib_->find_track(ss->dynamic_track);
            if (!p || p->min_geometry_lod > options_.geometry_detail) continue;
            chosen = ss;
            params = p;
            break;
        }
        const bool dying = (o.flags & kSnapDying) != 0;
        // Track points: both tread bones, else the object position.
        Vec3 pts[2];
        int sides[2] = {0, 0};
        int n = 0;
        if (chosen && bones_ && !chosen->left_bone.empty() && !chosen->right_bone.empty() && bones_(o, chosen->left_bone, pts[0]) &&
            bones_(o, chosen->right_bone, pts[1])) {
            sides[0] = 1;
            sides[1] = 2;
            n = 2;
        } else {
            pts[0] = o.position;
            n = 1;
        }
        // Existing generators of this object fade when there is no track now (or the object is dying).
        bool any = false;
        for (int side = 0; side <= 2; ++side) {
            const auto it = gens_.find({o.id, side});
            if (it == gens_.end()) continue;
            it->second.seen = generation_;
            any = true;
            const bool active = params && !dying && ((n == 2 && side != 0) || (n == 1 && side == 0)) && it->second.track == upper(params->name);
            if (!active) {
                it->second.gen->advance(dt);
                ++stats_.faded_only;
            }
        }
        if (!params || dying) {
            stats_.track_objects += any ? 1u : 0u;
            continue;
        }
        ++stats_.track_objects;
        const std::string key = upper(params->name);
        for (int k = 0; k < n; ++k) {
            Gen& g = gens_[{o.id, sides[k]}];
            if (!g.gen || g.track != key) {
                g.gen = std::make_unique<terrain::DynamicTrackGenerator>(*params);
                g.track = key;
            }
            g.seen = generation_;
            g.gen->update(pts[k].x, pts[k].y, dt);
        }
    }
    for (auto it = gens_.begin(); it != gens_.end();) it = it->second.seen != generation_ ? gens_.erase(it) : std::next(it);
    stats_.generators = static_cast<uint32_t>(gens_.size());
    // ---- foot plants (this tick's events): decal + one-shot puff at the foot
    for (const SnapshotFootPlant& f : frame.foot_plants) {
        ++stats_.foot_plants;
        const auto it = std::lower_bound(frame.objects.begin(), frame.objects.end(), f.id,
                                         [](const SnapshotObject& o, objects::ObjectId v) { return o.id < v; });
        if (it == frame.objects.end() || it->id != f.id || !table_->valid(it->type)) {
            ++stats_.foot_unmatched;
            continue;
        }
        const std::string_view surface = surface_type_name(terrain_surface_type(t, f.position.x, f.position.y));
        const SurfaceSettings* ss = foot_settings((*table_)[it->type], f, surface);
        if (!ss) {
            ++stats_.foot_unmatched;
            continue;
        }
        const float heading = std::atan2(it->facing.y, it->facing.x);
        if (const DecalFxDef* dd = lib_->find_decal(ss->decal))
            if (decals_.spawn(t, *dd, f.position.x, f.position.y, f.position.z, heading, now) == DecalSpawn::Spawned) ++stats_.foot_decals;
        if (!ss->particle.empty()) {
            add_puff(particle_asset(ss->particle), f.position, it->facing);
            ++stats_.foot_puffs;
        }
    }
    finish_effects();
    decals_.update(now);
    stats_.live_decals = static_cast<uint32_t>(decals_.size());
    remember();
}

std::string SimDecorations::particle_asset(std::string_view name) const {
    const objects::TypeIndex ti = table_->find(name);
    if (ti != objects::kNone && table_->valid(ti) && !(*table_)[ti].model.empty()) return (*table_)[ti].model;
    return std::string(name);
}

const SurfaceSettings* SimDecorations::foot_settings(const SnapshotTypeInfo& ti, const SnapshotFootPlant& f, std::string_view surface) const {
    auto usable = [&](const SurfaceFxDef* fx) -> const SurfaceSettings* {
        const SurfaceSettings* ss = fx ? fx->find(surface) : nullptr;
        return ss && (!ss->decal.empty() || !ss->particle.empty()) ? ss : nullptr;
    };
    if (!f.trigger.empty()) return usable(lib_->find_surface_fx(f.trigger));
    const std::string_view side = f.foot == 0 ? "_LEFT" : "_RIGHT";
    const std::string_view other = f.foot == 0 ? "_RIGHT" : "_LEFT";
    const SurfaceSettings* any = nullptr;
    for (const std::string& name : ti.surface_fx) {
        const std::string up = upper(name);
        if (up.size() >= other.size() && up.compare(up.size() - other.size(), other.size(), other) == 0) continue;  // the other foot
        const SurfaceSettings* ss = usable(lib_->find_surface_fx(name));
        if (!ss) continue;
        if (up.size() >= side.size() && up.compare(up.size() - side.size(), side.size(), side) == 0) return ss;  // this foot's trigger
        if (!any) any = ss;
    }
    return any;
}

std::vector<DynamicTrackMesh> SimDecorations::track_meshes() const {
    std::vector<DynamicTrackMesh> out;
    if (!terrain_) return out;
    for (const auto& [key, g] : gens_) {
        DynamicTrackMesh m;
        if (!g.gen || !g.gen->build(*terrain_, m.mesh)) continue;
        m.texture = g.gen->params().texture;
        m.mode = g.gen->params().mode;
        out.push_back(std::move(m));
    }
    return out;
}

void SimDecorations::apply(WorldRenderer& world) const {
    world.set_dynamic_tracks(track_meshes());
    world.set_runtime_decals(decal_geometry());
}

SimDecorations::BoneWorldFn model_bone_source(const SnapshotTypeTable& table, SimModelSource& models,
                                              const SimInstanceBuilder::PoseSource* pose) {
    return [&table, &models, pose](const SnapshotObject& o, std::string_view bone, Vec3& out) {
        const GpuModel* m = models.type_model(o.type);
        if (!m || !m->asset) return false;
        const assets::Skeleton& sk = m->asset->skeleton;
        size_t b = sk.names.size();
        for (size_t i = 0; i < sk.names.size(); ++i)
            if (ieq(sk.names[i], bone)) {
                b = i;
                break;
            }
        if (b >= sk.names.size()) return false;
        std::span<const Mat3x4> world = sk.bind_world;
        if (pose && *pose) {
            const std::span<const Mat3x4> p = (*pose)(o.id, *m);
            if (p.size() == sk.names.size()) world = p;
        }
        if (b >= world.size()) return false;
        const float scale = table.valid(o.type) ? table[o.type].scale : 1.0f;
        const Mat3x4 obj = object_transform(o, scale);  // the drawn model frame (roll / pitch included)
        const Mat3x4 bw = obj * world[b];
        out = {bw.m[0][3], bw.m[1][3], bw.m[2][3]};
        return true;
    };
}

}  // namespace alamo::render
