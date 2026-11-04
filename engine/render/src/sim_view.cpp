#include "alamo/render/sim_view.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iterator>
#include <string_view>
#include <utility>

#include "alamo/render/lod.hpp"

namespace alamo::render {

namespace {

// Case-insensitive substring test (R5 stealth mesh names: BASEMESH / STEALTH).
bool icontains(std::string_view s, std::string_view needle) {
    if (needle.size() > s.size()) return false;
    for (size_t i = 0; i + needle.size() <= s.size(); ++i) {
        size_t k = 0;
        while (k < needle.size() && std::toupper(static_cast<unsigned char>(s[i + k])) == std::toupper(static_cast<unsigned char>(needle[k]))) ++k;
        if (k == needle.size()) return true;
    }
    return false;
}

// R3 muzzle bone test: the named bone, else the bone index, else (own weapon) every bone named Muzzle*.
bool muzzle_bone(const assets::Skeleton& sk, const SnapshotMuzzle& m, int32_t bone) {
    if (bone <= 0 || static_cast<size_t>(bone) >= sk.names.size()) return false;
    if (!m.bone.empty()) return sk.find(m.bone) == bone;
    if (m.bone_index >= 0) return m.bone_index == bone;
    const std::string& n = sk.names[static_cast<size_t>(bone)];
    if (n.size() < 6) return false;
    for (size_t i = 0; i < 6; ++i)
        if (std::toupper(static_cast<unsigned char>(n[i])) != "MUZZLE"[i]) return false;
    return true;
}
std::string upper(const std::string& s) {
    std::string u(s);
    for (char& c : u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return u;
}

}  // namespace

// --------------------------------------------------------------------------------- streamed models
uint32_t StreamedSimModels::handle(const std::string& name) {
    auto [it, inserted] = by_name_.try_emplace(upper(name), 0u);
    if (inserted) {
        it->second = streamer_->request(name);
        handles_.push_back(it->second);
    }
    return it->second;
}

const GpuModel* StreamedSimModels::type_model(objects::TypeIndex type) {
    if (!table_->valid(type) || (*table_)[type].model.empty()) return nullptr;
    const uint32_t h = handle((*table_)[type].model);
    return streamer_->state(h) == ModelStreamer::State::Ready ? streamer_->model(h) : nullptr;
}

const GpuModel* StreamedSimModels::named_model(const std::string& name) {
    if (name.empty()) return nullptr;
    const uint32_t h = handle(name);
    return streamer_->state(h) == ModelStreamer::State::Ready ? streamer_->model(h) : nullptr;
}

void StreamedSimModels::request(const InterpolatedFrame& frame) {
    for (const SnapshotObject& o : frame.objects) (void)type_model(o.type);
    for (const SnapshotProjectile& p : frame.projectiles) (void)type_model(p.type);
}

uint32_t StreamedSimModels::ready() const {
    uint32_t n = 0;
    for (uint32_t h : handles_) n += streamer_->state(h) == ModelStreamer::State::Ready ? 1u : 0u;
    return n;
}

uint32_t StreamedSimModels::failed() const {
    uint32_t n = 0;
    for (uint32_t h : handles_) n += streamer_->state(h) == ModelStreamer::State::Failed ? 1u : 0u;
    return n;
}

// --------------------------------------------------------------------------------------- instances
const TypeRenderInfo& SimInstanceBuilder::render_info(objects::TypeIndex type) {
    auto [it, inserted] = type_info_.try_emplace(type);
    if (inserted) {
        if (resolver_ && table_->valid(type)) it->second = resolver_->render_info((*table_)[type].name_crc);
        if (table_->valid(type)) it->second.lod_bias = (*table_)[type].lod_bias;
    }
    return it->second;
}

const HardpointVisibility* SimInstanceBuilder::hardpoint_state(objects::ObjectId id) const {
    const auto it = states_.find(id);
    return it == states_.end() ? nullptr : &it->second.vis;
}

std::span<const uint8_t> SimInstanceBuilder::mesh_mask(objects::ObjectId id) const {
    const auto it = states_.find(id);
    if (it == states_.end()) return {};
    return it->second.use_combined ? std::span<const uint8_t>(it->second.combined) : it->second.vis.mask();
}

float SimInstanceBuilder::fog_alpha(objects::ObjectId id) const {
    const auto it = states_.find(id);
    return it == states_.end() ? 1.0f : it->second.fog_alpha;
}

float SimInstanceBuilder::stealth_value(objects::ObjectId id) const {
    const auto it = states_.find(id);
    return it == states_.end() ? 0.0f : it->second.stealth.value();
}

bool SimInstanceBuilder::stealth_shadow_dropped(objects::ObjectId id) const {
    const auto it = states_.find(id);
    return it != states_.end() && it->second.stealth.shadow_dropped();
}

int SimInstanceBuilder::applied_alt(objects::ObjectId id, int fallback) const {
    const auto it = states_.find(id);
    return it == states_.end() ? fallback : it->second.alt;
}

void SimInstanceBuilder::build(const InterpolatedFrame& frame, SimModelSource& models, std::vector<Instance>& main,
                               std::vector<Instance>& background, SimInstanceStats* stats) {
    SimInstanceStats st;
    ++generation_;
    main.clear();
    background.clear();
    const FogFrameInput* fog = ghosts_enabled_ && frame.fog && frame.fog->usable() ? frame.fog.get() : nullptr;
    if (!fog) ghosts_.clear();
    size_t mz = 0;  // cursor into frame.muzzles (ascending id, like the objects)
    for (const SnapshotObject& o : frame.objects) {
        ++st.objects;
        auto [sit, fresh] = states_.try_emplace(o.id);
        ObjState& s = sit->second;
        s.seen = generation_;
        if (fresh) s.vis.reset(o.hardpoint_count);
        // R5: this tick's destruction events first, in event order (B.10 order of overlapping subtrees).
        for (const SnapshotHardpointEvent& e : frame.hardpoint_events)
            if (e.id == o.id && e.index < o.hardpoint_count && !s.vis.destroyed(e.index)) {
                s.vis.set_destroyed(e.index, true);
                ++st.hardpoint_events;
            }
        // Destroyed hardpoints are sticky (B.10: never repaired); applied before the model is ready too.
        for (uint32_t k = 0; k < o.hardpoint_count; ++k) {
            const size_t idx = static_cast<size_t>(o.hardpoint_first) + k;
            if (idx < frame.hardpoint_destroyed.size() && frame.hardpoint_destroyed[idx] && !s.vis.destroyed(k))
                s.vis.set_destroyed(k, true);
        }
        for (uint32_t k = 0; k < o.hardpoint_count; ++k) st.hardpoints_destroyed += s.vis.destroyed(k) ? 1u : 0u;
        // R3 muzzles of this object (frame.muzzles ascends by id).
        while (mz < frame.muzzles.size() && frame.muzzles[mz].id < o.id) ++mz;
        size_t mz_end = mz;
        while (mz_end < frame.muzzles.size() && frame.muzzles[mz_end].id == o.id) ++mz_end;
        const std::span<const SnapshotMuzzle> muzzles(frame.muzzles.data() + mz, mz_end - mz);
        // R5 HideWhenFogged fade (local player), stepped once per new sim tick.
        const bool fog_hidden = (o.flags & kSnapFogHidden) != 0;
        st.fog_hidden += fog_hidden ? 1u : 0u;
        // R5 last-seen ghosts.
        const SnapshotTypeInfo* tinfo = table_->valid(o.type) ? &(*table_)[o.type] : nullptr;
        const bool ghost_type = fog && tinfo && (tinfo->ghost_last_state || tinfo->ghost_initial_state);
        bool ghosted = false;
        if (o.script_hidden) {
            ghosts_.erase(o.id);  // q6 t19 (OBJECT_SCRIPTS §10.3): a Lua-hidden model leaves no last-seen ghost (0x4eb020)
        } else if (ghost_type) {
            if (!fog_hidden) {
                if (ghosts_.erase(o.id)) {  // the ghost stood at full alpha: no fade-in pop
                    s.fog_alpha = 1.0f;
                    s.fog_vel = 0.0f;
                }
            } else if (!ghosts_.count(o.id)) {
                if (s.have_last && tinfo->ghost_last_state) ghosts_[o.id] = Ghost{s.last, s.last_mask};
                else if (fresh && tinfo->ghost_initial_state) ghosts_[o.id] = Ghost{o, {}};
            }
            ghosted = fog_hidden && ghosts_.count(o.id) != 0;
        }
        {
            // 0x53ddc0: the critically damped spring of smooth time 0.25 s, once per sim tick; instant for types with
            // Last_State_Visible_Under_FOW (type +0x88, they leave a ghost instead).
            const float target = fog_hidden ? 0.0f : 1.0f;
            if (fresh || fog_smooth_time_ <= 0.0f || (tinfo && tinfo->ghost_last_state)) {
                s.fog_alpha = target;
                s.fog_vel = 0.0f;
            } else if (frame.tick > s.fog_tick) {
                SmoothSpring sp{s.fog_alpha, s.fog_vel};
                smooth_damp_steps(sp, target, fog_smooth_time_, frame.tick - s.fog_tick);
                s.fog_alpha = std::clamp(sp.value, 0.0f, 1.0f);
                s.fog_vel = sp.velocity;
            }
            s.fog_tick = frame.tick;
        }
        // R5 stealth spring (ABILITY_VISUALS 1.1, 0x592e80 / 0x714b40): target 1 while the stealth flag is set, else 0; smooth
        // time = the snapshot's override (the 0.3 s break), else the type's Stealth_Transition_Time /
        // Force_Cloak_Transition_In_Secs (no value: instant). Stepped once per new sim tick; a new object starts converged.
        const bool stealth_on = (o.flags & kSnapStealthMeshes) != 0;
        {
            const bool cloak_now = stealth_on ? o.ability_active(kAbilityForceCloak) : s.stealth.cloak();
            if (!s.stealth_init) {
                s.stealth.snap(stealth_on, cloak_now);
                s.stealth_init = true;
            } else if (frame.tick > s.stealth_tick) {
                const float type_time = tinfo ? (cloak_now ? tinfo->cloak_time : tinfo->stealth_time) : 0.0f;
                s.stealth.step(stealth_on, cloak_now, o.stealth_time > 0.0f ? o.stealth_time : type_time, frame.tick - s.stealth_tick);
            }
            s.stealth_tick = frame.tick;
        }
        s.use_combined = false;
        if (o.flags & kSnapContained) {
            ++st.contained;
            continue;
        }
        if (o.script_hidden) {  // q6 t19: the model hide switch (0x264c80)
            ++st.script_hidden;
            continue;
        }
        if (!(o.fade > 0.0f)) {
            ++st.faded_out;
            continue;
        }
        if (ghosted) continue;  // drawn as its ghost below
        if (s.fog_alpha < kFogHideAlpha) {
            ++st.fog_faded_out;
            continue;
        }
        const GpuModel* m = models.type_model(o.type);
        if (!m) {
            ++st.no_model;
            continue;
        }
        if (m->asset && s.bound != m) {  // bind the (type, model) hardpoint layout once the model is ready
            const TypeRenderInfo& ti = render_info(o.type);
            HardpointLayoutPtr& lay = layouts_[{o.type, m->uid}];
            if (!lay) {
                if (ti.hardpoints_known)
                    lay = std::make_shared<const HardpointLayout>(build_hardpoint_layout(*m->asset, ti.hardpoint_defs, show_damage_));
                else
                    lay = std::make_shared<const HardpointLayout>(fallback_hardpoint_layout(
                        *m->asset, show_damage_ ? hidden_mesh_mask(*m->asset, nullptr) : m->mesh_hidden, o.hardpoint_count));
            }
            s.vis.init(lay);
            s.bound = m;
        }
        Instance in;
        in.model = m;
        const float scale = tinfo ? tinfo->scale : 1.0f;
        in.transform = object_transform(o, scale);  // S7 t05: + movement roll / pitch
        in.motion_id = motion_key(1, static_cast<uint64_t>(o.id));
        in.light_scale = {1, 1, 1, (o.fade < 1.0f ? o.fade : 1.0f) * s.fog_alpha};
        if (s.stealth.tinted() && tinfo) {  // R5: LIGHT_SCALE *= lerp(white, Stealth_Color, v) (0x3a0c20)
            const bool cloak = s.stealth.cloak();
            if (cloak ? tinfo->has_cloak_color : tinfo->has_stealth_color) {
                const materials::float4 tint = s.stealth.tint(cloak ? tinfo->cloak_color : tinfo->stealth_color);
                in.light_scale = {in.light_scale.x * tint.x, in.light_scale.y * tint.y, in.light_scale.z * tint.z,
                                  in.light_scale.w * tint.w};
                ++st.stealth_tinted;
            }
        }
        in.no_shadow = s.stealth.shadow_dropped();  // R5: the model's stencil-shadow bit is cleared while stealthed
        st.stealth_no_shadow += in.no_shadow ? 1u : 0u;
        in.team_color_override = true;
        in.team_color = o.team_color;
        in.lod_bias = o.lod_bias;
        in.mesh_hidden = s.vis.mask();
        // R1/R4: damage-state model alternate (_ALTn meshes), applied only while the model has that alternate (0x12dca0).
        if (o.alt <= 0 || o.alt <= m->max_alt) s.alt = o.alt;
        in.alt = s.alt;
        st.alt_nonzero += s.alt != 0 ? 1u : 0u;
        if (pose_) in.bone_world = pose_(o.id, *m);  // R3: animated pose (empty = bind)
        // R3: the clip's per-bone visibility, then the muzzle flashes, on top of the hardpoint mask (meshes and proxies by
        // their own bone).
        if (m->asset) {
            const assets::ModelAsset& ma = *m->asset;
            const std::span<const uint8_t> bv = visibility_ ? visibility_(o.id, *m) : std::span<const uint8_t>();
            const bool use_bv = !bv.empty() && bv.size() == ma.skeleton.names.size();
            bool own_muzzle = false;
            for (const SnapshotMuzzle& mzl : muzzles) own_muzzle = own_muzzle || mzl.hardpoint < 0;
            const bool stealth = (o.flags & kSnapStealthMeshes) != 0;  // R5: BASEMESH hidden, STEALTH shown (§3.7)
            if (use_bv || own_muzzle || stealth) {
                const std::span<const uint8_t> base = s.vis.mask();
                const size_t n = std::max(base.size(), ma.meshes.size());
                s.combined.assign(n, 0);
                std::copy(base.begin(), base.end(), s.combined.begin());
                if (base.size() < ma.meshes.size())  // no layout yet: the model's default mask
                    for (size_t i = 0; i < ma.meshes.size() && i < m->mesh_hidden.size(); ++i) s.combined[i] = m->mesh_hidden[i];
                const HardpointLayout* lay = s.vis.layout();
                uint32_t hidden = 0;
                bool changed = false;
                if (use_bv) {
                    for (size_t i = 0; i < ma.meshes.size(); ++i) {
                        const uint32_t b = ma.meshes[i].attach_bone;
                        if (b < bv.size() && bv[b] == 0 && s.combined[i] == 0) {
                            s.combined[i] = 1;
                            ++hidden;
                        }
                    }
                    for (size_t p = 0; lay && p < ma.proxies.size() && p < lay->proxies; ++p) {
                        const size_t sub = lay->proxy_index(static_cast<uint32_t>(p));
                        const uint32_t b = ma.proxies[p].bone;
                        if (sub < s.combined.size() && b < bv.size() && bv[b] == 0 && s.combined[sub] == 0) {
                            s.combined[sub] = 1;
                            changed = true;
                        }
                    }
                }
                for (const SnapshotMuzzle& mzl : muzzles) {
                    if (mzl.hardpoint >= 0) continue;
                    for (size_t i = 0; i < ma.meshes.size(); ++i)
                        if (muzzle_bone(ma.skeleton, mzl, static_cast<int32_t>(ma.meshes[i].attach_bone)) && s.combined[i] != 0) {
                            s.combined[i] = 0;
                            changed = true;
                            ++st.muzzle_meshes;
                        }
                    for (size_t p = 0; lay && p < ma.proxies.size() && p < lay->proxies; ++p) {
                        const size_t sub = lay->proxy_index(static_cast<uint32_t>(p));
                        if (muzzle_bone(ma.skeleton, mzl, static_cast<int32_t>(ma.proxies[p].bone)) && sub < s.combined.size() && s.combined[sub] != 0) {
                            s.combined[sub] = 0;
                            changed = true;
                            ++st.muzzle_meshes;
                        }
                    }
                }
                if (stealth) {
                    // R5 stealth mesh swap (ABILITIES_HARDPOINTS §3.7 apply: BASEMESH hidden-flag set, STEALTH mesh shown)
                    // [port: the swap is instant; the Stealth_Transition_Time fade spring is render-side and untraced].
                    for (size_t i = 0; i < ma.meshes.size(); ++i) {
                        const std::string& nm = ma.meshes[i].name;
                        if (icontains(nm, "BASEMESH") && s.combined[i] == 0) {
                            s.combined[i] = 1;
                            ++hidden;
                        } else if (icontains(nm, "STEALTH") && s.combined[i] != 0) {
                            s.combined[i] = 0;
                            changed = true;
                        }
                    }
                    // Hardpoint meshes too (0x3a8140): the sub-objects named like each hardpoint's attached model.
                    if (lay) {
                        const uint32_t hp_hidden = hide_hardpoint_meshes(ma, *lay, s.combined);
                        if (hp_hidden) {
                            changed = true;
                            st.stealth_hardpoints_hidden += hp_hidden;
                        }
                    }
                    ++st.stealthed;
                }
                if (hidden || changed) {
                    s.use_combined = true;
                    in.mesh_hidden = s.combined;
                    st.bone_hidden_meshes += hidden;
                }
            }
        }
        const bool bg = (o.flags & kSnapBackground) != 0;
        std::vector<Instance>& dst = bg ? background : main;
        dst.push_back(in);
        ++st.drawn;
        st.background += bg ? 1u : 0u;
        // R5: the last visible state (ghost source) - position / facing / alternate / effective mask of this frame.
        if (ghost_type && !fog_hidden) {
            s.have_last = true;
            s.last = o;
            const std::span<const uint8_t> mk = s.use_combined ? std::span<const uint8_t>(s.combined) : s.vis.mask();
            s.last_mask.assign(mk.begin(), mk.end());
        }
        // Model_To_Attach at Attachment_Bone, HardPoints order, after the owner.
        const HardpointLayout* lay = s.vis.layout();
        if (lay && s.attach_masks.size() != lay->hardpoints) s.attach_masks.assign(lay->hardpoints, {});
        for (uint32_t h = 0; lay && m->asset && h < lay->hardpoints && h < lay->attach_model.size(); ++h) {
            if (lay->attach_model[h].empty() || !s.vis.attachment_visible(h)) continue;
            if (stealth_on) {  // R5 stealth: the hardpoint's model is hidden with its mesh (ABILITY_VISUALS 1.3)
                ++st.stealth_hardpoints_hidden;
                continue;
            }
            const GpuModel* am = models.named_model(lay->attach_model[h]);
            if (!am) continue;
            Instance at = in;
            at.model = am;
            at.mesh_hidden = {};
            at.motion_id = motion_key(1, static_cast<uint64_t>(o.id), h + 1);
            at.bone_world = {};
            at.alt = 0;
            at.transform = attachment_transform(*m->asset, in.transform, in.bone_world, lay->attach_bone[h]);  // R3: posed bone
            // R3 T6: the attached model's own idle clip (scene model).
            if (attach_pose_) {
                at.bone_world = attach_pose_(*am, frame.tick);
                st.attachment_posed += at.bone_world.empty() ? 0u : 1u;
            }
            // R3: muzzles on a hardpoint's attached model (fire bones of Model_To_Attach).
            bool hp_muzzle = false;
            for (const SnapshotMuzzle& mzl : muzzles) hp_muzzle = hp_muzzle || mzl.hardpoint == static_cast<int32_t>(h);
            if (hp_muzzle && am->asset) {
                const assets::ModelAsset& aa = *am->asset;
                std::vector<uint8_t>& mk = s.attach_masks[h];
                mk.assign(am->mesh_hidden.begin(), am->mesh_hidden.end());
                mk.resize(aa.meshes.size(), 0);
                bool changed = false;
                for (const SnapshotMuzzle& mzl : muzzles) {
                    if (mzl.hardpoint != static_cast<int32_t>(h)) continue;
                    for (size_t i = 0; i < aa.meshes.size(); ++i)
                        if (muzzle_bone(aa.skeleton, mzl, static_cast<int32_t>(aa.meshes[i].attach_bone)) && mk[i] != 0) {
                            mk[i] = 0;
                            changed = true;
                            ++st.muzzle_meshes;
                        }
                }
                if (changed) at.mesh_hidden = mk;
            }
            dst.push_back(at);
            ++st.attachments;
        }
    }
    // R5 ghosts: drawn while their position stays fogged for the local player; dropped once it is seen.
    for (auto it = ghosts_.begin(); it != ghosts_.end();) {
        const SnapshotObject& g = it->second.obj;
        if (!fog || fog_point_visible(*fog, g.position.x, g.position.y)) {
            it = ghosts_.erase(it);
            continue;
        }
        if (const GpuModel* m = models.type_model(g.type)) {
            Instance in;
            in.model = m;
            const float scale = table_->valid(g.type) ? (*table_)[g.type].scale : 1.0f;
            in.transform = object_transform(g, scale);
            in.team_color_override = true;
            in.team_color = g.team_color;
            in.lod_bias = g.lod_bias;
            in.alt = g.alt;
            const std::vector<uint8_t>& mk = it->second.mask;
            in.mesh_hidden = mk.empty() ? std::span<const uint8_t>() : std::span<const uint8_t>(mk);
            ((g.flags & kSnapBackground) ? background : main).push_back(in);
            ++st.ghosts;
        }
        ++it;
    }
    // Projectiles after the objects (ascending serial): lasers into lasers_, model projectiles into the main list.
    lasers_.clear();
    for (const SnapshotProjectile& p : frame.projectiles) {
        ++st.projectiles;
        if (projectile_fogged(frame, p)) {
            ++st.projectiles_fogged;
            continue;
        }
        Laser laser;
        if (table_->valid(p.type) && projectile_laser(p, (*table_)[p.type], laser)) {
            lasers_.push_back(laser);
            ++st.projectiles_drawn;
            ++st.projectiles_lasers;
            continue;
        }
        const GpuModel* m = models.type_model(p.type);
        if (!m) continue;
        Instance in;
        in.model = m;
        in.transform = projectile_transform(p, table_->valid(p.type) ? &(*table_)[p.type] : nullptr, m);
        in.motion_id = motion_key(2, p.serial);
        main.push_back(in);
        ++st.projectiles_drawn;
    }
    for (auto it = states_.begin(); it != states_.end();) it = it->second.seen != generation_ ? states_.erase(it) : std::next(it);
    if (stats) *stats = st;
}

assets::Mat3x4 projectile_transform(const SnapshotProjectile& p, const SnapshotTypeInfo* type, const GpuModel* model) {
    (void)model;
    return object_transform(p.position, p.direction, type ? type->scale : 1.0f);
}

bool projectile_laser(const SnapshotProjectile& p, const SnapshotTypeInfo& type, Laser& out) {
    if (type.projectile_custom_render != 1 && type.projectile_custom_render != 2) return false;
    if (!(type.projectile_width > 0.0f) || !(type.projectile_length >= 0.0f)) return false;
    out = Laser{};
    out.kind = type.projectile_custom_render == 1 ? LaserKind::Beam : LaserKind::Kite;
    out.position = {p.position.x, p.position.y, p.position.z};
    out.direction = {p.direction.x, p.direction.y, p.direction.z};
    out.width = type.projectile_width;
    out.length = type.projectile_length * type.scale;  // the render object's world transform carries Scale_Factor [M]
    out.slot_col = type.projectile_slot[0];
    out.slot_row = type.projectile_slot[1];
    for (int k = 0; k < 4; ++k) out.color[k] = type.projectile_color[k];
    return true;
}

bool projectile_fogged(const InterpolatedFrame& frame, const SnapshotProjectile& p) {
    return frame.fog && frame.fog->drawn() && !fog_point_visible(*frame.fog, p.position.x, p.position.y);
}

// ------------------------------------------------------------------------------------------ camera
CameraParams camera_params(const OrbitCamera& cam, float znear, float zfar) {
    CameraParams p;
    p.target = cam.target;
    p.yaw = cam.yaw;
    p.pitch = cam.pitch;
    p.distance = cam.distance;
    p.fov_x = cam.fov_x;
    p.znear = znear;
    p.zfar = zfar;
    return p;
}

OrbitCamera orbit_camera(const CameraParams& p) {
    OrbitCamera c;
    c.target = p.target;
    c.yaw = p.yaw;
    c.pitch = p.pitch;
    c.distance = p.distance;
    c.fov_x = p.fov_x;
    c.radius = p.distance;
    return c;
}

ViewMatrices camera_view(const CameraParams& p, uint32_t width, uint32_t height) {
    return make_view(orbit_camera(p), width, height, p.znear, p.zfar);
}

// --------------------------------------------------------------------------------------- debug box
std::shared_ptr<assets::ModelAsset> debug_box_model(const std::string& name, float e) {
    auto m = std::make_shared<assets::ModelAsset>();
    m->name = name;
    assets::Skeleton& sk = m->skeleton;
    sk.names = {"ROOT"};
    sk.parents = {-1};
    sk.visible = {1};
    sk.billboard_mode = {0};
    sk.bind_local = {assets::Mat3x4{}};
    sk.finalize();
    assets::MeshInfo mi;
    mi.name = "BOX";
    mi.attach_bone = 0;
    mi.first_submesh = 0;
    mi.submesh_count = 1;
    m->meshes.push_back(mi);
    assets::Submesh sm;
    sm.mesh = 0;
    sm.vertex_format = "alD3dVertNU2C";
    sm.layout = assets::VertexLayout::Static;
    sm.attach_bone = 0;
    // Per face: outward normal n, in-plane axis u, v = n x u; corners (-u-v, +u-v, +u+v, -u+v) are
    // counter-clockwise seen from outside. Grey levels per face so the shape reads without lighting.
    struct Face {
        float n[3], u[3];
        uint8_t grey;
    };
    const Face faces[6] = {{{1, 0, 0}, {0, 1, 0}, 200},  {{-1, 0, 0}, {0, -1, 0}, 200}, {{0, 1, 0}, {-1, 0, 0}, 170},
                           {{0, -1, 0}, {1, 0, 0}, 170}, {{0, 0, 1}, {1, 0, 0}, 255},  {{0, 0, -1}, {-1, 0, 0}, 110}};
    sm.vertex_count = 24;
    sm.vertices.resize(24 * 64);
    std::vector<uint16_t> idx;
    for (int f = 0; f < 6; ++f) {
        const Face& fc = faces[f];
        const float v[3] = {fc.n[1] * fc.u[2] - fc.n[2] * fc.u[1], fc.n[2] * fc.u[0] - fc.n[0] * fc.u[2],
                            fc.n[0] * fc.u[1] - fc.n[1] * fc.u[0]};
        const float su[4] = {-1, 1, 1, -1}, sv[4] = {-1, -1, 1, 1};
        for (int k = 0; k < 4; ++k) {
            assets::StaticVertex st{};
            for (int a = 0; a < 3; ++a) {
                st.position[a] = e * (fc.n[a] + su[k] * fc.u[a] + sv[k] * v[a]);
                st.normal[a] = fc.n[a];
                st.tangent[a] = fc.u[a];
                st.binormal[a] = v[a];
            }
            st.uv0[0] = su[k] > 0 ? 1.0f : 0.0f;
            st.uv0[1] = sv[k] > 0 ? 0.0f : 1.0f;
            st.color[0] = st.color[1] = st.color[2] = fc.grey;
            st.color[3] = 255;
            std::memcpy(sm.vertices.data() + static_cast<size_t>(f * 4 + k) * 64, &st, 64);
        }
        const auto b = static_cast<uint16_t>(f * 4);
        for (uint16_t i : {uint16_t{0}, uint16_t{1}, uint16_t{2}, uint16_t{0}, uint16_t{2}, uint16_t{3}})
            idx.push_back(static_cast<uint16_t>(b + i));
    }
    sm.index_type = assets::IndexType::U16;
    sm.index_count = static_cast<uint32_t>(idx.size());
    sm.indices.resize(idx.size() * 2);
    std::memcpy(sm.indices.data(), idx.data(), idx.size() * 2);
    sm.bounds = {{-e, -e, -e}, {e, e, e}};
    m->submeshes.push_back(std::move(sm));
    m->bounds = {{-e, -e, -e}, {e, e, e}};
    m->sphere = {{0, 0, 0}, e * 1.7320508f};
    return m;
}

}  // namespace alamo::render
