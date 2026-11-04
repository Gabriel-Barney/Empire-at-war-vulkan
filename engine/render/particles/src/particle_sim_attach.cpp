#include "alamo/render/particles/particle_sim_attach.hpp"

#include <algorithm>

#include "alamo/render/hardpoints.hpp"

namespace alamo::render::particles {

SimAttachStats sync_sim_frame(ProxyAttachments& attach, const InterpolatedFrame& frame, const SnapshotTypeTable& table,
                              SimModelSource& models, const SimInstanceBuilder& builder, const SimInstanceBuilder::PoseSource* pose,
                              const SimAttachOptions* options, std::span<const SimWorldEffect> world_effects) {
    static const SimAttachOptions kDefaults;
    const SimAttachOptions& opt = options ? *options : kDefaults;
    SimAttachStats st;
    // Per-object gates shared by the proxies and the attached effects: false = not synced (systems released).
    struct Gate {
        const SnapshotTypeInfo* ti = nullptr;
        float fade = 0.0f;
        bool culled = false;
    };
    auto gate = [&](const SnapshotObject& o, Gate& g, bool count) {
        // R3: inside a garrison / transport; q6 t19: Lua Hide(true) hides the model's scene node, its particles with it
        // (OBJECT_SCRIPTS §10.3) [port M: the spec names the model switch, the attached systems follow the model].
        if ((o.flags & kSnapContained) || o.script_hidden) {
            if (count) ++st.contained;
            return false;
        }
        if (!(o.fade > 0.0f)) {
            if (count) ++st.faded_out;
            return false;
        }
        g.ti = table.valid(o.type) ? &table[o.type] : nullptr;
        if (g.ti && g.ti->no_particle_below_detail > opt.detail) {
            if (count) ++st.below_detail;
            return false;
        }
        // Allow_Particle_Model_Culling types: the host's local-visibility predicate, else the snapshot's fog mark (R5).
        const bool locally_hidden = opt.locally_visible ? !opt.locally_visible(o.id) : (o.flags & kSnapFogHidden) != 0;
        g.culled = g.ti && g.ti->allow_particle_model_culling && locally_hidden;
        // R5 HideWhenFogged: the model (and so its proxies) is hidden below the builder's fog alpha threshold.
        const float fog_alpha = builder.fog_alpha(o.id);
        if (fog_alpha < SimInstanceBuilder::kFogHideAlpha) g.culled = true;
        if (count) st.culled += g.culled ? 1u : 0u;
        g.fade = g.culled ? 0.0f : (o.fade < 1.0f ? o.fade : 1.0f) * fog_alpha;
        return true;
    };
    attach.begin();
    for (const SnapshotObject& o : frame.objects) {
        ++st.objects;
        Gate g;
        if (!gate(o, g, true)) continue;
        const float scale0 = g.ti ? g.ti->scale : 1.0f;
        // A type whose model is a particle file (PARTICLE objects, B.1.1): the object is the system.
        if (g.ti && !g.ti->model.empty()) {
            if (SystemPtr fx = attach.library().get(g.ti->model)) {
                attach.sync_effect(sim_owner_key(o.id), std::move(fx), object_transform(o, scale0), g.fade, !g.culled);
                ++st.effects;
                continue;
            }
        }
        const GpuModel* m = models.type_model(o.type);
        if (!m || !m->asset) {
            ++st.no_model;
            continue;
        }
        Instance in;
        in.model = m;
        in.transform = object_transform(o, scale0);
        in.light_scale = {1, 1, 1, g.fade};
        in.mesh_hidden = builder.mesh_mask(o.id);  // hardpoints + R3 bone visibility
        in.alt = builder.applied_alt(o.id, o.alt);  // R1: the alternate the model shows (0x12dca0 rule)
        if (pose && *pose) in.bone_world = (*pose)(o.id, *m);
        attach.sync(sim_owner_key(o.id), in, false, in.alt);
        ++st.synced;
    }
    // Int-B: effects the sim attached to objects (damaged smoke): one system per (object, percent) at the object's
    // transform [port: the attachment bone of 0x3ab890 is not traced; unit centre], same fade / detail / culling gates.
    for (const SnapshotEffect& e : frame.effects) {
        const auto it = std::lower_bound(frame.objects.begin(), frame.objects.end(), e.id,
                                         [](const SnapshotObject& o, objects::ObjectId v) { return o.id < v; });
        if (it == frame.objects.end() || it->id != e.id) continue;
        Gate g;
        if (!gate(*it, g, false)) continue;
        SystemPtr fx = attach.library().get(e.asset);
        if (!fx) {
            ++st.attached_missing;
            continue;
        }
        Mat3x4 xf = object_transform(*it, g.ti ? g.ti->scale : 1.0f);
        // R5 Lua Attach_Particle_Effect: at the named bone of the object's model (current pose) when it has one.
        if (!e.bone.empty())
            if (const GpuModel* m = models.type_model(it->type); m && m->asset) {
                const int32_t b = m->asset->skeleton.find(e.bone);
                if (b >= 0) {
                    const std::span<const Mat3x4> bw = pose && *pose ? (*pose)(it->id, *m) : std::span<const Mat3x4>();
                    xf = attachment_transform(*m->asset, xf, bw, b);
                    ++st.lua_bones;
                }
            }
        const uint64_t key = e.lua_seq ? lua_effect_key(e.lua_seq) : attached_effect_key(e.id, e.percent);
        attach.sync_effect(key, std::move(fx), xf, g.fade, !g.culled);
        ++st.attached;
        st.lua += e.lua_seq ? 1u : 0u;
    }
    // Projectiles (missile trails / particle projectiles), after the objects' effects.
    for (const SnapshotProjectile& p : frame.projectiles) {
        const SnapshotTypeInfo* ti = table.valid(p.type) ? &table[p.type] : nullptr;
        if (!ti || ti->model.empty()) continue;
        const bool visible = !projectile_fogged(frame, p);
        if (SystemPtr fx = attach.library().get(ti->model)) {
            attach.sync_effect(projectile_key(p.serial), std::move(fx), projectile_transform(p, ti, nullptr), 1.0f, visible);
            ++st.projectiles;
            continue;
        }
        const GpuModel* m = models.type_model(p.type);
        if (!m || !m->asset || m->asset->proxies.empty()) continue;
        Instance in;
        in.model = m;
        in.transform = projectile_transform(p, ti, m);
        in.light_scale = {1, 1, 1, visible ? 1.0f : 0.0f};
        attach.sync(projectile_key(p.serial), in);
        ++st.projectiles;
    }
    // R5 world effects (footprint puffs, SurfaceFX wakes).
    for (const SimWorldEffect& w : world_effects) {
        Mat3x4 xf;
        float fade = 1.0f;
        bool visible = true;
        if (w.owner != objects::kNoObjectId) {
            const auto it = std::lower_bound(frame.objects.begin(), frame.objects.end(), w.owner,
                                             [](const SnapshotObject& o, objects::ObjectId v) { return o.id < v; });
            if (it == frame.objects.end() || it->id != w.owner) continue;
            Gate g;
            if (!gate(*it, g, false)) continue;
            xf = object_transform(it->position, it->facing, 1.0f);
            fade = g.fade;
            visible = !g.culled;
        } else {
            xf = object_transform(w.position, w.facing, 1.0f);
        }
        SystemPtr fx = attach.library().get(w.asset);
        if (!fx) {
            ++st.world_missing;
            continue;
        }
        attach.sync_effect(world_effect_key(w.key), std::move(fx), xf, fade, visible, w.intensity);
        ++st.world;
    }
    attach.end();
    return st;
}

}  // namespace alamo::render::particles
