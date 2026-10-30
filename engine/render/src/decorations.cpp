// alamo_render - terrain decorations and fog of war on the GPU (docs/re/DECORATIONS.md; CPU side in
// engine/terrain decorations.hpp / fow.hpp). See world.hpp for what draws where.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include "alamo/core/log.hpp"
#include "alamo/render/convert.hpp"
#include "alamo/render/world.hpp"
#include "render_internal.hpp"

namespace alamo::render {

namespace {

using terrain::RenderMode;

bool box_outside(const Mat4& vp, const assets::Aabb& b) {
    if (!b.valid()) return false;
    int out[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 8; ++i) {
        const Float4 p{(i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y, (i & 4) ? b.max.z : b.min.z, 1.0f};
        const Float4 c = vp * p;
        out[0] += c.x < -c.w;
        out[1] += c.x > c.w;
        out[2] += c.y < -c.w;
        out[3] += c.y > c.w;
        out[4] += c.z < 0.0f;
        out[5] += c.z > c.w;
    }
    for (int k = 0; k < 6; ++k)
        if (out[k] == 8) return true;
    return false;
}

// Engine prim effect of a render mode (DECORATIONS.md 1.1). The depth-sprite modes 4-6 draw with the
// effect of their blend (1-3): their DEPTH_SCALE soft-particle technique needs a scene depth texture the
// decoration pass does not have [choice]. 8-11 and 13 are no decoration modes: not drawn.
const char* prim_effect_name(uint32_t m) {
    switch (m) {
    case 0: return "PrimOpaque";
    case 1: case 4: return "PrimAdditive";
    case 2: case 5: return "PrimAlpha";
    case 3: case 6: return "PrimModulate";
    case 7: return "PrimDiffuseAlpha";
    case 12: return "PrimDecalBumpAlpha";
    default: return nullptr;
    }
}

// First drawing pass: translated (its program) or fixed function (FF ubershader).
const materials::ResolvedPass* usable_pass(const materials::MaterialEffect* e) {
    if (!e) return nullptr;
    for (const materials::ResolvedPass& p : e->passes) {
        if (p.restore_only) continue;
        if (p.program_status == materials::ProgramStatus::Translated) return &p;
        if (p.fixed_function && p.ff) return &p;
        return nullptr;
    }
    return nullptr;
}

std::unique_ptr<materials::FfDrawTemplate> make_template(const materials::MaterialEffect* e, const materials::ResolvedPass* p) {
    if (!e || !p || !p->fixed_function || !p->ff || !e->effect) return nullptr;
    auto t = std::make_unique<materials::FfDrawTemplate>();
    const materials::ParamStore store;
    std::string err;
    if (!t->build(*p->ff, *e->effect, store.lookup(), true, &err)) {
        log::warnf("render", "decorations: FF template of %s: %s", e->name.c_str(), err.c_str());
        return nullptr;
    }
    return t;
}

// The camera with near and far moved by +0.3 (DECORATIONS.md 1.3, bit-faithful variant).
ViewMatrices decoration_view(const ViewMatrices& v) {
    ViewMatrices d = v;
    const float m22 = v.proj.m[2][2], m23 = v.proj.m[2][3];
    if (m22 == 0.0f || m22 == -1.0f) return d;
    const float zn = m23 / m22, zf = m23 / (m22 + 1.0f);
    const float n2 = zn + terrain::kDecorationDepthOffset, f2 = zf + terrain::kDecorationDepthOffset;
    d.proj.m[2][2] = f2 / (n2 - f2);
    d.proj.m[2][3] = n2 * f2 / (n2 - f2);
    d.view_proj = d.proj * d.view;
    return d;
}

bool blended(RenderMode m) { return m != RenderMode::Opaque; }

template <class T>
void append_bytes(std::vector<uint8_t>& out, const std::vector<T>& v) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(v.data());
    out.insert(out.end(), p, p + v.size() * sizeof(T));
}

void align_to(std::vector<uint8_t>& v, size_t a) {
    while (v.size() % a) v.push_back(0);
}

}  // namespace

// ----------------------------------------------------------------------------------------- setup

bool WorldRenderer::init_decorations(const terrain::TerrainMesh& mesh, const WorldDecorations& d, std::string* error) {
    Renderer& r = *r_;
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    deco_cache_ = std::make_unique<Renderer::XConstCache>();
    fow_cache_ = std::make_unique<Renderer::XConstCache>();
    deco_time_ = 0.0f;
    if (r.desc_.effects) {
        for (uint32_t m = 0; m < terrain::kRenderModeCount; ++m) {
            const char* n = prim_effect_name(m);
            if (!n) continue;
            prim_fx_[m] = r.desc_.effects->get(n);
            prim_pass_[m] = usable_pass(prim_fx_[m].get());
            prim_tpl_[m] = make_template(prim_fx_[m].get(), prim_pass_[m]);
            if (!prim_pass_[m]) log::warnf("render", "decorations: %s has no usable pass (mode %u not drawn)", n, m);
        }
        if (!options_.space) {
            fow_fx_ = r.desc_.effects->get("TerrainFogOfWar");
            fow_pass_ = usable_pass(fow_fx_.get());
            if (fow_pass_ && fow_pass_->program_status != materials::ProgramStatus::Translated) fow_pass_ = nullptr;
        } else {
            space_fow_fx_ = r.desc_.effects->get("SpaceFogOfWar");
            space_fow_pass_ = usable_pass(space_fow_fx_.get());
            space_fow_tpl_ = make_template(space_fow_fx_.get(), space_fow_pass_);
        }
    }
    auto slot_tex = [&](const std::string& name, assets::TextureRole role) {
        const int t = add_texture(name, role);
        if (t >= 0 && std::find(slot_textures_.begin(), slot_textures_.end(), t) == slot_textures_.end()) slot_textures_.push_back(t);
        return t;
    };
    if (!options_.decorations) return true;

    // ---- Static geometry: land tracks, water tracks, then non-fading decals grouped by mode (ascending).
    std::vector<uint8_t> vbytes, ibytes;
    auto add_mesh = [&](const terrain::DecorationMesh& m, DecoDraw dd) {
        dd.vb_offset = vbytes.size();
        dd.first_index = static_cast<uint32_t>(ibytes.size() / 4);
        dd.index_count = static_cast<uint32_t>(m.indices.size());
        dd.bounds = m.bounds;
        append_bytes(vbytes, m.vertices);
        append_bytes(ibytes, m.indices);
        deco_static_.push_back(dd);
    };
    for (int pass = 0; pass < 2; ++pass)
        for (const terrain::TrackGeometry& t : d.tracks) {
            if (t.mesh.empty() || (t.type == terrain::DecorationType::WaterTrack) != (pass == 1)) continue;
            DecoDraw dd;
            dd.type = t.type;
            dd.mode = t.mode;
            dd.texture = slot_tex(t.texture, assets::TextureRole::Color);
            dd.clamp = t.clamp_u && t.clamp_v;  // engine samplers clamp or wrap both axes [choice: mixed -> WRAP]
            dd.u_speed = t.u_speed;
            dd.v_speed = t.v_speed;
            add_mesh(t.mesh, dd);
        }
    if (!d.decals.empty() || (d.runtime_decals && !options_.space))
        master_decal_ = slot_tex(terrain::kMasterDecalTexture, assets::TextureRole::Color);
    for (uint32_t m = 0; m < terrain::kRenderModeCount; ++m) {
        terrain::DecorationMesh merged;
        for (size_t i = 0; i < d.decals.size(); ++i) {
            const terrain::DecalGeometry& g = d.decals[i];
            if (static_cast<uint32_t>(g.mode) != m || g.mesh.empty()) continue;
            const bool fading = i < d.decal_records.size() && d.decal_records[i].fade_time > 0.0f;
            if (fading) {
                fading_decals_.push_back(g);
                fading_records_.push_back(d.decal_records[i]);
                continue;
            }
            const uint32_t base = static_cast<uint32_t>(merged.vertices.size());
            merged.vertices.insert(merged.vertices.end(), g.mesh.vertices.begin(), g.mesh.vertices.end());
            for (uint32_t ix : g.mesh.indices) merged.indices.push_back(base + ix);
            merged.bounds = base > 0 ? assets::merge(merged.bounds, g.mesh.bounds) : g.mesh.bounds;
        }
        if (merged.empty()) continue;
        DecoDraw dd;
        dd.type = terrain::DecorationType::Decal;
        dd.mode = static_cast<RenderMode>(m);
        dd.texture = master_decal_;
        add_mesh(merged, dd);
    }
    if (!vbytes.empty() && (!upload_buffer(deco_vb_, vbytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, "world.decorations_vb") ||
                            !upload_buffer(deco_ib_, ibytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, "world.decorations_ib")))
        return fail("decoration buffer upload failed");

    // ---- Per-frame decoration textures (dynamic tracks, blob materials).
    blob_materials_ = d.blob_materials.empty() ? terrain::builtin_blob_materials() : d.blob_materials;
    blob_texture_.clear();
    for (const terrain::ShadowBlobMaterial& m : blob_materials_) blob_texture_.push_back(slot_tex(m.texture, assets::TextureRole::Color));
    for (const std::string& t : d.extra_textures) slot_tex(t, assets::TextureRole::Color);

    // ---- FOW index lists (TerrainFogOfWar redraws whole patches, DECORATIONS.md 2.5).
    if (!options_.space && !mesh.patches.empty()) {
        std::vector<uint16_t> ix;
        for (uint32_t row = 0; row < terrain::kPatchQuads; ++row)
            for (uint32_t col = 0; col < terrain::kPatchQuads; ++col) {
                const uint32_t v = row * terrain::kPatchVerts + col;
                const uint16_t q[6] = {static_cast<uint16_t>(v), static_cast<uint16_t>(v + 1), static_cast<uint16_t>(v + 33),
                                       static_cast<uint16_t>(v), static_cast<uint16_t>(v + 33), static_cast<uint16_t>(v + 32)};
                ix.insert(ix.end(), q, q + 6);
            }
        const uint32_t full = static_cast<uint32_t>(ix.size());
        fow_patch_.clear();
        for (const terrain::TerrainPatch& p : mesh.patches) {
            if (!p.has_holes) {
                fow_patch_.push_back({0u, full});
                continue;
            }
            fow_patch_.push_back({static_cast<uint32_t>(ix.size()), static_cast<uint32_t>(p.rendered_indices.size())});
            ix.insert(ix.end(), p.rendered_indices.begin(), p.rendered_indices.end());
        }
        std::vector<uint8_t> b;
        append_bytes(b, ix);
        if (!upload_buffer(fow_ib_, b, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, "world.fow_ib")) return fail("FOW index buffer upload failed");
    }
    if (options_.space) {
        space_grid_ = slot_tex("W_Space_FOW_Grid.tga", assets::TextureRole::Color);
        space_reinforce_grid_ = slot_tex("W_Space_Reinforce_FOW_Grid.tga", assets::TextureRole::Color);  // R5 overlay
    }
    return true;
}

void WorldRenderer::shutdown_decorations() {
    if (!r_) return;
    rhi::Allocator& a = *r_->alloc_;
    auto kill = [&](rhi::Buffer& b) {
        if (!b) return;
        r_->up_->forget(b.handle);
        a.destroy(b);
    };
    kill(deco_vb_);
    kill(deco_ib_);
    kill(fow_ib_);
    for (uint32_t s = 0; s < kMaxSlots; ++s) {
        kill(space_plane_vb_[s]);
        space_plane_gen_[s] = 0;
    }
    fow_box_gen_ = 0;
    for (DynSlot& s : dyn_) {
        kill(s.buffer);
        s.size = 0;
    }
    if (fow_next_) {
        r_->up_->forget(fow_next_.handle);
        a.destroy(fow_next_);
    }
    if (fow_image_) r_->release_texture(fow_slot_, &fow_image_);
    fow_slot_ = 0;
    fow_image_ok_ = fow_next_pending_ = fow_enabled_ = false;
    deco_static_.clear();
    fading_decals_.clear();
    fading_records_.clear();
    blob_materials_.clear();
    blob_texture_.clear();
    dyn_tracks_.clear();
    blobs_.clear();
    runtime_decals_.clear();
    fow_patch_.clear();
    master_decal_ = space_grid_ = space_reinforce_grid_ = -1;
    space_reinforce_ = false;
    for (uint32_t m = 0; m < terrain::kRenderModeCount; ++m) {
        prim_fx_[m].reset();
        prim_pass_[m] = nullptr;
        prim_tpl_[m].reset();
    }
    fow_fx_.reset();
    space_fow_fx_.reset();
    fow_pass_ = space_fow_pass_ = nullptr;
    space_fow_tpl_.reset();
    deco_cache_.reset();
    fow_cache_.reset();
}

void WorldRenderer::set_fow_box(float x0, float y0, float x1, float y1) {
    const std::array<float, 4> u = terrain::fow_tex_u(x0, y0, x1, y1), v = terrain::fow_tex_v(x0, y0, x1, y1);
    fow_u_ = {u[0], u[1], u[2], u[3]};
    fow_v_ = {v[0], v[1], v[2], v[3]};
    if (fow_box_gen_ != 0 && fow_box_[0] == x0 && fow_box_[1] == y0 && fow_box_[2] == x1 && fow_box_[3] == y1) return;
    fow_box_[0] = x0;
    fow_box_[1] = y0;
    fow_box_[2] = x1;
    fow_box_[3] = y1;
    ++fow_box_gen_;  // the space plane buffers are rewritten lazily, each in its own frame slot
}

// Space FOW plane (DECORATIONS.md 2.7): the box at SpaceFOWHeight, PrimVert, white. Grid UV = 200 repeats across the
// box: SpaceFogOfWar.fxo's stage-1 texture transform is FOW_TEX_U / FOW_TEX_V x 200 without their translation over
// camera-space texgen, i.e. u = 200 x / (x1 - x0), v = -200 y / (y1 - y0) [M: FXO expression; the port feeds the grid
// through the plane UVs instead of texgen]. W_Space_FOW_Grid's cells carry alpha 18 / 255 with transparent borders,
// so fogged space gets a fine, faint wash (playtest 2026-10-28: the former 500-unit repeat drew a coarse, prominent
// grid over the whole battlefield). Called while recording frame slot `slot`, whose previous frame has completed, so
// its buffer is not read by the GPU.
bool WorldRenderer::write_space_plane(uint32_t slot) {
    rhi::Buffer& b = space_plane_vb_[slot];
    if (b && space_plane_gen_[slot] == fow_box_gen_) return true;
    if (!b && (!r_->alloc_->create_buffer(6 * sizeof(terrain::PrelitVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                          rhi::MemoryUsage::Dynamic, b, "world.space_fow_plane") ||
               !b.mapped())) {
        if (b) r_->alloc_->destroy(b);
        return false;
    }
    const float x0 = fow_box_[0], y0 = fow_box_[1], x1 = fow_box_[2], y1 = fow_box_[3];
    const float c[6][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y0}, {x1, y1}, {x0, y1}};
    const float gu = kSpaceFowGridRepeats / (x1 - x0), gv = -kSpaceFowGridRepeats / (y1 - y0);
    terrain::PrelitVertex pv[6];
    for (int i = 0; i < 6; ++i) {
        pv[i].position[0] = c[i][0];
        pv[i].position[1] = c[i][1];
        pv[i].position[2] = terrain::kSpaceFowHeight;
        pv[i].color[0] = pv[i].color[1] = pv[i].color[2] = pv[i].color[3] = 0xFF;
        pv[i].texcoord[0] = c[i][0] * gu;
        pv[i].texcoord[1] = c[i][1] * gv;
    }
    std::memcpy(b.mapped(), pv, sizeof pv);
    space_plane_gen_[slot] = fow_box_gen_;
    return true;
}

bool WorldRenderer::set_fog_of_war(const terrain::FowGrid& grid) {
    if (!r_ || grid.width() == 0 || grid.height() == 0 || grid.texels().size() != static_cast<size_t>(grid.width()) * grid.height() * 4)
        return false;
    // An upload still in flight is superseded. Integration fixes: its copy / acquire barrier may already be recorded
    // in the frame being built (a call after begin_frame), so the image goes through the renderer's deferred
    // release (forget + destroyed once the frame ring passes retire_value()) instead of being destroyed here.
    if (fow_next_) r_->release_texture(0, &fow_next_);
    rhi::ImageDesc d;
    d.format = rhi::Format::RGBA8;
    d.width = grid.width();
    d.height = grid.height();
    d.mips = 1;
    d.name = "world.fow";
    if (!r_->alloc_->create_image(d, fow_next_)) return false;
    fow_ticket_ = r_->up_->upload_image_chain(fow_next_, grid.texels());
    if (!fow_ticket_) {
        r_->alloc_->destroy(fow_next_);
        return false;
    }
    fow_next_pending_ = true;
    fow_enabled_ = true;
    const terrain::FowGridDesc& g = grid.desc();
    set_fow_box(g.x0, g.y0, g.x1, g.y1);
    return true;
}

bool WorldRenderer::upload_fow_pending() {
    if (!fow_next_pending_ || !r_ || !r_->up_->ready(fow_ticket_)) return false;
    if (fow_image_) r_->release_texture(fow_slot_, &fow_image_);  // deferred until frames using it retired
    fow_image_ = fow_next_;
    fow_next_ = rhi::Image{};
    fow_slot_ = r_->allocate_texture_slot(fow_image_.view, "world FOW_TEXTURE");
    fow_next_pending_ = false;
    fow_image_ok_ = true;
    return true;
}

// Per-frame decorations into this frame slot's host-visible buffer (the slot's previous frame finished).
bool WorldRenderer::build_dynamic(const rhi::Frame& frame, std::vector<DecoDraw>& out, VkBuffer& buffer, VkDeviceSize& index_base) {
    std::vector<uint8_t> vb, ib;
    std::vector<DecoDraw> draws;
    auto add = [&](const auto& vertices, const std::vector<uint32_t>& indices, DecoDraw dd) {
        if (indices.empty()) return;
        align_to(vb, 64);
        dd.vb_offset = vb.size();
        dd.first_index = static_cast<uint32_t>(ib.size() / 4);
        dd.index_count = static_cast<uint32_t>(indices.size());
        append_bytes(vb, vertices);
        append_bytes(ib, indices);
        draws.push_back(dd);
    };
    auto tex_of = [&](const std::string& name) {
        if (name.empty()) return -1;
        std::string k = name;
        for (char& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (int t : slot_textures_) {
            std::string n = textures_[static_cast<size_t>(t)].name;
            for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (n == k) return t;
        }
        return -1;
    };
    // Type 2: dynamic tracks.
    for (const DynamicTrackMesh& t : dyn_tracks_) {
        DecoDraw dd;
        dd.type = terrain::DecorationType::DynamicTrack;
        dd.mode = t.mode;
        dd.texture = tex_of(t.texture);
        dd.bounds = t.mesh.bounds;
        add(t.mesh.vertices, t.mesh.indices, dd);
    }
    // Type 1: fading decals, colours refreshed from their age; dead ones dropped (DECORATIONS.md 4.4).
    for (size_t i = 0; i < fading_decals_.size();) {
        const terrain::DecalFade f = terrain::decal_fade(fading_records_[i], deco_time_);
        if (f.dead) {
            fading_decals_.erase(fading_decals_.begin() + static_cast<std::ptrdiff_t>(i));
            fading_records_.erase(fading_records_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        terrain::set_decal_alpha(fading_decals_[i], f.alpha);
        DecoDraw dd;
        dd.type = terrain::DecorationType::Decal;
        dd.mode = fading_decals_[i].mode;
        dd.texture = master_decal_;
        dd.bounds = fading_decals_[i].mesh.bounds;
        add(fading_decals_[i].mesh.vertices, fading_decals_[i].mesh.indices, dd);
        ++i;
    }
    // Type 1 (R5): runtime decals of this frame (TerrainDecalFX), colours set by their owner.
    for (const terrain::DecalGeometry& g : runtime_decals_) {
        if (g.mesh.empty()) continue;
        DecoDraw dd;
        dd.type = terrain::DecorationType::Decal;
        dd.mode = g.mode;
        dd.texture = master_decal_;
        dd.bounds = g.mesh.bounds;
        add(g.mesh.vertices, g.mesh.indices, dd);
    }
    // Type 0: shadow blobs grouped per material (ascending material, stable).
    std::vector<uint32_t> order(blobs_.size());
    for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return blobs_[a].material < blobs_[b].material; });
    for (size_t k = 0; k < order.size();) {
        const uint32_t mat = blobs_[order[k]].material;
        std::vector<terrain::PrelitVertex> verts;
        std::vector<uint32_t> idx;
        assets::Aabb bounds;
        for (; k < order.size() && blobs_[order[k]].material == mat; ++k) {
            const terrain::BlobGeometry& g = blobs_[order[k]].geometry;
            const uint32_t base = static_cast<uint32_t>(verts.size());
            verts.insert(verts.end(), g.vertices.begin(), g.vertices.end());
            for (uint32_t ix : g.indices) idx.push_back(base + ix);
            bounds = base > 0 ? assets::merge(bounds, g.bounds) : g.bounds;
        }
        DecoDraw dd;
        dd.type = terrain::DecorationType::ShadowBlob;
        const uint32_t mi = mat < blob_materials_.size() ? mat : 0u;
        dd.mode = mi < blob_materials_.size() ? blob_materials_[mi].mode : RenderMode::Modulate;
        dd.texture = mi < blob_texture_.size() ? blob_texture_[mi] : -1;
        dd.clamp = true;  // blob textures: U and V CLAMP (DECORATIONS.md 3.4)
        dd.prelit = true;
        dd.bounds = bounds;
        add(verts, idx, dd);
    }
    if (draws.empty()) return true;
    align_to(vb, 64);
    const VkDeviceSize need = vb.size() + ib.size();
    DynSlot& s = dyn_[frame.slot() % kMaxSlots];
    if (s.size < need) {
        if (s.buffer) r_->alloc_->destroy(s.buffer);  // this slot's previous frame has completed
        const VkDeviceSize size = std::max<VkDeviceSize>(need + need / 2, 64 * 1024);
        if (!r_->alloc_->create_buffer(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                       rhi::MemoryUsage::Dynamic, s.buffer, "world.decorations_dynamic") ||
            !s.buffer.mapped()) {
            if (s.buffer) r_->alloc_->destroy(s.buffer);
            s.size = 0;
            return false;
        }
        s.size = size;
    }
    std::memcpy(s.buffer.mapped(), vb.data(), vb.size());
    std::memcpy(static_cast<uint8_t*>(s.buffer.mapped()) + vb.size(), ib.data(), ib.size());
    buffer = s.buffer.handle;
    index_base = vb.size();
    out = std::move(draws);
    return true;
}

// ----------------------------------------------------------------------------------- renderer side

uint32_t Renderer::ff_geometry_draws(rhi::Frame& frame, VkCommandBuffer cmd, const ViewMatrices& view, std::span<const FfGeoDraw> draws,
                                     uint32_t* pipeline_binds) {
    if (draws.empty()) return 0;
    const size_t n = draws.size();
    materials::FrameCB fcb;
    frame_constants(view, kFcbFog | kFcbWorld, fcb);  // the SH contract's single FrameCB builder
    float view_inv16[16];
    for (int rr = 0; rr < 4; ++rr)
        for (int cc = 0; cc < 4; ++cc) view_inv16[rr * 4 + cc] = view.view_inv.m[cc][rr];
    materials::ExprInputs ein;
    ein.time = time_;
    ein.view_inv = view_inv16;
    ein.fow_tex_u = &fow_u_;
    ein.fow_tex_v = &fow_v_;
    if (cloud_view_) {
        ein.cloud_tex_u = &cloud_u_;
        ein.cloud_tex_v = &cloud_v_;
    }
    rhi::BufferRange fcb_r;
    if (!upload_frame_cb(frame, fcb, fcb_r)) return 0;
    const rhi::UniformAlloc da = frame.alloc_storage(n * sizeof(materials::DrawData));
    const rhi::UniformAlloc ma = frame.alloc_storage(sizeof(materials::MeshMaterial));
    const rhi::UniformAlloc ba = frame.alloc_storage(sizeof(materials::BoneMatrix));
    const rhi::UniformAlloc xa = frame.alloc_storage(n * sizeof(materials::FfDrawData));
    const VkDescriptorSet s0 = frame.descriptors(0).allocate(hot_.buffers());
    if (!da || !ma || !ba || !xa || !s0) return 0;
    const materials::MeshMaterial mm{};
    const materials::BoneMatrix bm{};
    std::memcpy(ma.ptr, &mm, sizeof mm);
    std::memcpy(ba.ptr, &bm, sizeof bm);
    for (size_t i = 0; i < n; ++i) {
        const FfGeoDraw& d = draws[i];
        materials::DrawData dd{};
        store_rows(Mat3x4{}, dd.world);
        dd.light_scale = {1, 1, 1, 1};
        dd.material_index = static_cast<uint32_t>(i);
        std::memcpy(static_cast<uint8_t*>(da.ptr) + i * sizeof dd, &dd, sizeof dd);
        materials::FfDrawData f{};
        if (d.tpl) d.tpl->update(ein, f);
        f.tex.x = materials::pack_texture(d.tex[0], d.sampler[0]);
        f.tex.y = materials::pack_texture(d.tex[1], d.sampler[1]);
        f.tex.z = materials::pack_texture(d.tex[2], d.sampler[2]);
        if (d.scroll) {  // stage-0 texture matrix: identity + translation (frac(t u), frac(t v)) for (u, v, 1, 0)
            f.tex_transform[0][0] = {1, 0, d.uv_offset[0], 0};
            f.tex_transform[0][1] = {0, 1, d.uv_offset[1], 0};
            f.tex_transform[0][2] = {0, 0, 1, 0};
            f.tex_transform[0][3] = {0, 0, 0, 1};
            f.stage[0].transform_flags = 2;  // D3DTTFF_COUNT2
        }
        if (d.stage0_rows) {
            for (int rr = 0; rr < 4; ++rr) f.tex_transform[0][rr] = d.rows[rr];
            f.stage[0].transform_flags = 2;
            f.stage[0].texcoord_index = 0x20000u;  // D3DTSS_TCI_CAMERASPACEPOSITION
        }
        std::memcpy(static_cast<uint8_t*>(xa.ptr) + i * sizeof f, &f, sizeof f);
    }
    const rhi::BufferRange sto[rhi::kHotStorageBuffers] = {{da.buffer, da.offset, n * sizeof(materials::DrawData)},
                                                           {ma.buffer, ma.offset, sizeof mm},
                                                           {ba.buffer, ba.offset, sizeof bm},
                                                           {xa.buffer, xa.offset, n * sizeof(materials::FfDrawData)}};
    rhi::write_hot_buffers(*ctx_, s0, fcb_r, sto);
    const rhi::DeviceFns& fn = ctx_->vkd();
    const VkDescriptorSet sets[2] = {s0, bindless_};
    fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 2, sets, 0, nullptr);
    VkPipeline bound = VK_NULL_HANDLE;
    const materials::DynamicState* dyn_bound = nullptr;
    uint32_t drawn = 0;
    for (size_t i = 0; i < n; ++i) {
        const FfGeoDraw& d = draws[i];
        materials::PsoKey key = d.pass->pso[0];
        key.render_pass = rp_scene_;
        key.vertex_layout = d.layout;
        const materials::Program prog = materials::ff_program(materials::ff_inputs_for_layout(d.layout));
        if (prog.vs) key.vs_id = prog.vs->id;
        if (prog.ps) key.ps_id = prog.ps->id;
        if (d.cull_none) key.cull = materials::CullMode::None;
        if (d.no_depth_write && key.blend_enable) key.depth_write = 0;
        const VkPipeline pipe = psos_.get(key);
        if (!pipe) continue;
        if (pipe != bound) {
            fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            bound = pipe;
            if (pipeline_binds) ++*pipeline_binds;
        }
        if (&d.pass->dynamic != dyn_bound) {
            cmd_set_dynamic_state(fn, cmd, d.pass->dynamic, rhi::from_vk(ds_format_));
            dyn_bound = &d.pass->dynamic;
        }
        fn.vkCmdBindVertexBuffers(cmd, 0, 1, &d.vb, &d.vb_offset);
        materials::PushConstants pc = d.pass->push;
        pc.draw_index = static_cast<uint32_t>(i);
        if (d.fog && lighting_.fog_enabled) pc.flags |= materials::kPushFog;
        fn.vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        if (d.ib) {
            fn.vkCmdBindIndexBuffer(cmd, d.ib, 0, d.index_type);
            fn.vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, 0, 0);
        } else {
            fn.vkCmdDraw(cmd, d.index_count, 1, d.first_index, 0);
        }
        ++drawn;
    }
    return drawn;
}

void Renderer::world_draw_decorations(rhi::Frame& frame, VkCommandBuffer cmd, const ViewMatrices& view, uint32_t width,
                                      uint32_t height) {
    WorldRenderer& w = *world_;
    if (w.options_.space) return;  // space: only the FOW plane (background pass)
    DecorationStats& ds = w.stats_.decorations;
    ds = {};
    if (!w.resident_ || !w.options_.decorations) return;
    // A run of decoration draws through their prim effects, in order: consecutive FF draws share one
    // ff_geometry_draws call (one set 0), translated ones go through post_translated.
    struct DecoBatch {
        const WorldRenderer::DecoDraw* d;
        VkBuffer vb, ib;
        VkDeviceSize ib_base;
    };
    const ViewMatrices dv = decoration_view(view);
    std::vector<WorldRenderer::DecoDraw> dyn;
    VkBuffer dyn_buf = VK_NULL_HANDLE;
    VkDeviceSize dyn_ib = 0;
    if (!w.build_dynamic(frame, dyn, dyn_buf, dyn_ib)) log::errorf("render", "decorations: per-frame buffer allocation failed");
    // Order 3, 2, 1 (by mode), 0 (DECORATIONS.md 0 item 2), each culled by its bounds.
    std::vector<DecoBatch> list;
    auto push = [&](const WorldRenderer::DecoDraw& d, bool dynamic) {
        if (box_outside(dv.view_proj, d.bounds)) return;
        if (dynamic) list.push_back({&d, dyn_buf, dyn_buf, dyn_ib});
        else list.push_back({&d, w.deco_vb_.handle, w.deco_ib_.handle, 0});
    };
    for (const auto& d : w.deco_static_)
        if (d.type == terrain::DecorationType::LandTrack) push(d, false), ++ds.land_tracks;
    for (const auto& d : dyn)
        if (d.type == terrain::DecorationType::DynamicTrack) push(d, true), ++ds.dynamic_tracks;
    for (uint32_t m = 0; m < terrain::kRenderModeCount; ++m) {
        for (const auto& d : w.deco_static_)
            if (d.type == terrain::DecorationType::Decal && static_cast<uint32_t>(d.mode) == m) push(d, false), ++ds.decals;
        for (const auto& d : dyn)
            if (d.type == terrain::DecorationType::Decal && static_cast<uint32_t>(d.mode) == m) push(d, true), ++ds.decals;
    }
    for (const auto& d : dyn)
        if (d.type == terrain::DecorationType::ShadowBlob) push(d, true), ++ds.blobs;
    if (!w.deco_visible_) list.clear();

    auto tex_view = [&](int t) {
        return t >= 0 && w.textures_[static_cast<size_t>(t)].image ? w.textures_[static_cast<size_t>(t)].image.view : white_.view;
    };
    auto tex_slot = [&](int t) { return t >= 0 ? w.textures_[static_cast<size_t>(t)].slot : materials::kNoTexture; };
    std::vector<FfGeoDraw> ff;
    auto flush = [&]() {
        if (ff.empty()) return;
        const uint32_t n = ff_geometry_draws(frame, cmd, dv, ff, &ds.pipeline_binds);
        ds.ff_draws += n;
        ds.draws += n;
        ds.pending += static_cast<uint32_t>(ff.size()) - n;
        ff.clear();
    };
    for (const DecoBatch& b : list) {
        const WorldRenderer::DecoDraw& d = *b.d;
        const uint32_t m = static_cast<uint32_t>(d.mode);
        const materials::ResolvedPass* pass = m < terrain::kRenderModeCount ? w.prim_pass_[m] : nullptr;
        if (!pass || !b.vb) {
            ++ds.pending;
            continue;
        }
        // Track texture matrix (DECORATIONS.md 5.1): (frac(t u_speed), frac(t v_speed)).
        const float tu = d.u_speed * w.deco_time_, tv = d.v_speed * w.deco_time_;
        if (pass->fixed_function) {
            FfGeoDraw g;
            g.fx = w.prim_fx_[m];
            g.pass = pass;
            g.tpl = w.prim_tpl_[m].get();
            g.layout = d.prelit ? materials::VertexLayout::PrimVert : materials::VertexLayout::PrimVertLit;
            g.vb = b.vb;
            g.vb_offset = d.vb_offset;
            g.ib = b.ib;
            g.first_index = static_cast<uint32_t>(b.ib_base / 4) + d.first_index;
            g.index_count = d.index_count;
            g.tex[0] = tex_slot(d.texture);
            g.sampler[0] = d.clamp ? materials::kSamplerClampLinear : materials::kSamplerWrapLinear;
            g.scroll = d.u_speed != 0.0f || d.v_speed != 0.0f;
            g.uv_offset[0] = tu - std::floor(tu);
            g.uv_offset[1] = tv - std::floor(tv);
            g.no_depth_write = blended(d.mode);
            ff.push_back(g);
            ds.triangles += d.index_count / 3;
            continue;
        }
        flush();
        PostSpec ps;
        ps.fx = w.prim_fx_[m];
        ps.pass = pass;
        ps.render_pass = rp_scene_;
        ps.width = width;
        ps.height = height;
        ps.view = &dv;
        ps.layout = d.prelit ? materials::VertexLayout::PrimVert : materials::VertexLayout::PrimVertLit;
        ps.vb = b.vb;
        ps.vb_offset = d.vb_offset;
        ps.ib = b.ib;
        ps.ib_offset = b.ib_base + static_cast<VkDeviceSize>(d.first_index) * 4;
        ps.index_count = d.index_count;
        ps.index_type = VK_INDEX_TYPE_UINT32;
        ps.fog = true;
        ps.cull_none = true;
        ps.no_depth_write = blended(d.mode);
        ps.slot_default = tex_view(d.texture);  // the prim effects' samplers have no texture parameter
        ps.override_sampler = true;
        ps.sampler_override.u = ps.sampler_override.v = d.clamp ? materials::AddressMode::ClampToEdge : materials::AddressMode::Repeat;
        if (post_translated(frame, cmd, ps, nullptr)) {
            ++ds.translated_draws;
            ++ds.draws;
            ++ds.pipeline_binds;
            ds.triangles += d.index_count / 3;
        } else {
            ++ds.pending;
        }
    }
    flush();

    // ---- Type 4: fog of war, every visible patch again with TerrainFogOfWar (DESTCOLOR / ZERO), batched like water.
    ds.fow = w.fow_enabled() && fow_view_ != VK_NULL_HANDLE;
    if (!ds.fow || !w.fow_pass_ || !w.fow_ib_ || !w.terrain_vb_ || !w.fow_cache_) return;
    PostSpec ps;
    ps.fx = w.fow_fx_;
    ps.pass = w.fow_pass_;
    ps.render_pass = rp_scene_;
    ps.width = width;
    ps.height = height;
    ps.view = &dv;
    ps.layout = materials::VertexLayout::PrimVertLit;
    ps.vb = w.terrain_vb_.handle;
    ps.ib = w.fow_ib_.handle;
    ps.fog = true;
    ps.no_depth_write = true;
    const PostTexture fow_tex[2] = {{"m_FOWTexture", fow_view_}, {"FOW_TEXTURE", fow_view_}};
    ps.textures = fow_tex;
    std::vector<uint32_t>& vis = w.fow_vis_scratch_;  // Q3: kept across frames
    vis.clear();
    for (uint32_t pi = 0; pi < w.patches_.size() && pi < w.fow_patch_.size(); ++pi)
        if (!box_outside(dv.view_proj, w.patches_[pi].bounds) && w.fow_patch_[pi].second > 0) vis.push_back(pi);
    if (vis.empty()) return;
    GeoState g;
    Renderer::XConstCache& cc = *w.fow_cache_;
    cc.generation = ++w.draw_generation_;
    const WorldRenderer::ConstRegion reg = translated_state(ps, nullptr, g) ? w.const_region(frame, g.int_bool) : WorldRenderer::ConstRegion{};
    const VkDescriptorSet tex = reg.ptr ? frame.descriptors(0).allocate(std_layouts_.textures()) : VK_NULL_HANDLE;
    if (!tex) {
        ds.pending += static_cast<uint32_t>(vis.size());
        return;
    }
    translated_fill(ps, cc, true, g.int_bool);
    WorldRenderer::copy_region(reg.ptr, cc, g.int_bool);
    rhi::TextureBinding tb[rhi::kTextureSlots];
    const uint32_t ntb = translated_textures(ps, g, tb);
    if (ntb) rhi::write_textures(*ctx_, tex, std::span<const rhi::TextureBinding>(tb, ntb));
    bind_geometry_state(cmd, ps, g, w.terrain_vb_.handle, w.fow_ib_.handle);
    ++ds.pipeline_binds;
    const rhi::DeviceFns& fn = ctx_->vkd();
    const VkDescriptorSet xs[2] = {reg.set, tex};
    fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 2, xs, 0, nullptr);
    for (uint32_t pi : vis) {
        const auto [first, count] = w.fow_patch_[pi];
        fn.vkCmdDrawIndexed(cmd, count, 1, first, w.patches_[pi].vertex_offset, 0);
        ++ds.fow_draws;
        ++ds.draws;
        ds.triangles += count / 3;
    }
}

void Renderer::world_draw_water_tracks(rhi::Frame& frame, VkCommandBuffer cmd, const ViewMatrices& view, uint32_t width,
                                       uint32_t height) {
    (void)width;
    (void)height;
    WorldRenderer& w = *world_;
    if (!w.resident_ || !w.options_.decorations || !w.deco_vb_ || !w.deco_visible_) return;
    DecorationStats& ds = w.stats_.decorations;
    std::vector<FfGeoDraw> ff;
    for (const auto& d : w.deco_static_) {
        if (d.type != terrain::DecorationType::WaterTrack) continue;
        ++ds.water_tracks;
        const uint32_t m = static_cast<uint32_t>(d.mode);
        const materials::ResolvedPass* pass = m < terrain::kRenderModeCount ? w.prim_pass_[m] : nullptr;
        if (!pass || !pass->fixed_function || box_outside(view.view_proj, d.bounds)) {  // tracks are DIFFUSE_ALPHA (FF)
            ds.pending += pass ? 0u : 1u;
            continue;
        }
        const float tu = d.u_speed * w.deco_time_, tv = d.v_speed * w.deco_time_;
        FfGeoDraw g;
        g.fx = w.prim_fx_[m];
        g.pass = pass;
        g.tpl = w.prim_tpl_[m].get();
        g.vb = w.deco_vb_.handle;
        g.vb_offset = d.vb_offset;
        g.ib = w.deco_ib_.handle;
        g.first_index = d.first_index;
        g.index_count = d.index_count;
        g.tex[0] = d.texture >= 0 ? w.textures_[static_cast<size_t>(d.texture)].slot : materials::kNoTexture;
        g.sampler[0] = d.clamp ? materials::kSamplerClampLinear : materials::kSamplerWrapLinear;
        g.scroll = d.u_speed != 0.0f || d.v_speed != 0.0f;
        g.uv_offset[0] = tu - std::floor(tu);
        g.uv_offset[1] = tv - std::floor(tv);
        ff.push_back(g);
        ds.triangles += d.index_count / 3;
    }
    const uint32_t n = ff_geometry_draws(frame, cmd, view, ff, &ds.pipeline_binds);  // unmodified camera (type 5)
    ds.water_track_draws = n;
    ds.draws += n;
    ds.ff_draws += n;
    ds.pending += static_cast<uint32_t>(ff.size()) - n;
}

void Renderer::world_draw_space_fow(rhi::Frame& frame, VkCommandBuffer cmd, const ViewMatrices& view, uint32_t width, uint32_t height) {
    (void)width;
    (void)height;
    WorldRenderer& w = *world_;
    DecorationStats& ds = w.stats_.decorations;
    ds = {};
    if (!w.resident_ || !w.space_fow_pass_ || !w.space_fow_tpl_ || w.fow_box_gen_ == 0 || !fow_view_) return;
    const uint32_t slot = frame.slot() % WorldRenderer::kMaxSlots;
    if (!w.write_space_plane(slot)) return;
    ds.fow = true;
    FfGeoDraw g;
    g.fx = w.space_fow_fx_;
    g.pass = w.space_fow_pass_;
    g.tpl = w.space_fow_tpl_.get();
    g.layout = materials::VertexLayout::PrimVert;
    g.vb = w.space_plane_vb_[slot].handle;
    g.index_count = 6;  // non-indexed
    g.tex[0] = fow_slot_;
    g.sampler[0] = materials::kSamplerClampLinear;
    const int grid = w.space_reinforce_ && w.space_reinforce_grid_ >= 0 ? w.space_reinforce_grid_ : w.space_grid_;  // R5
    g.tex[1] = grid >= 0 ? w.textures_[static_cast<size_t>(grid)].slot : materials::kNoTexture;
    g.sampler[1] = materials::kSamplerWrapLinear;
    g.fog = false;  // background scene: fog off
    // Stage 0 = FOW texture by camera-space texgen. The FXO's TEXTURE0 expression scales FOW_TEX_U / V by 200
    // and drops their translation; with this port's FOW_TEX_U / V (texture spans the playable box) that maps
    // the view outside [0,1]. The matrix used here maps camera space -> world -> FOW_TEX_U / V instead, so the
    // texture covers the box exactly [choice; the original space mapping is not traced, DECORATIONS.md 2.7].
    g.stage0_rows = true;
    const materials::float4 U = fow_u_, V = fow_v_;
    const float uu[4] = {U.x, U.y, U.z, U.w}, vv[4] = {V.x, V.y, V.z, V.w};
    float ru[4] = {0, 0, 0, 0}, rv[4] = {0, 0, 0, 0};
    for (int j = 0; j < 4; ++j)
        for (int k = 0; k < 4; ++k) {
            ru[j] += uu[k] * view.view_inv.m[k][j];
            rv[j] += vv[k] * view.view_inv.m[k][j];
        }
    g.rows[0] = {ru[0], ru[1], ru[2], ru[3]};
    g.rows[1] = {rv[0], rv[1], rv[2], rv[3]};
    g.rows[2] = {0, 0, 1, 0};
    g.rows[3] = {0, 0, 0, 1};
    const uint32_t n = ff_geometry_draws(frame, cmd, view, std::span<const FfGeoDraw>(&g, 1), &ds.pipeline_binds);
    ds.space_fow_draws = n;
    ds.draws = n;
    ds.pending = 1u - n;
}

}  // namespace alamo::render
