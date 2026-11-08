#include "alamo/render/world.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "alamo/core/ascii.hpp"
#include "alamo/core/log.hpp"
#include "alamo/render/convert.hpp"
#include "render_internal.hpp"

namespace alamo::render {

using detail::begin_target;

namespace {

bool patch_outside(const Mat4& vp, const assets::Aabb& b) {
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

std::vector<float> vec4(const std::array<float, 4>& a) { return {a[0], a[1], a[2], a[3]}; }

const materials::ResolvedPass* first_draw_pass(const materials::MaterialEffect* e) {
    if (!e) return nullptr;
    for (const materials::ResolvedPass& p : e->passes)
        if (!p.restore_only) return p.program_status == materials::ProgramStatus::Translated ? &p : nullptr;
    return nullptr;
}

}  // namespace

// ------------------------------------------------------------------------------------ CPU helpers

void WorldRenderer::cloud_matrices(float scale, float ox, float oy, materials::float4& u, materials::float4& v) {
    const float is = scale != 0.0f ? 1.0f / scale : 0.0f;
    u = {is, 0.0f, 0.0f, -ox * is};
    v = {0.0f, is, 0.0f, -oy * is};
}

Mat4 WorldRenderer::reflection_matrix(float height) {
    Mat4 m;
    m.m[2][2] = -1.0f;
    m.m[2][3] = 2.0f * height;
    return m;
}

ViewMatrices WorldRenderer::mirrored_view(const ViewMatrices& v, float height) {
    ViewMatrices o = v;
    const Mat4 r = reflection_matrix(height);
    o.view = v.view * r;
    o.view_proj = o.proj * o.view;
    o.view_inv = r * v.view_inv;  // (V R)^-1 = R^-1 V^-1, R is its own inverse
    o.eye = {v.eye.x, v.eye.y, 2.0f * height - v.eye.z};
    return o;
}

bool WorldRenderer::clip_projection(ViewMatrices& v, const Float4& p, float far_scale) {
    // Plane into view space: p_view = p_world * V^-1 (row vector).
    const float pw[4] = {p.x, p.y, p.z, p.w};
    float c[4];
    for (int j = 0; j < 4; ++j) {
        c[j] = 0.0f;
        for (int i = 0; i < 4; ++i) c[j] += pw[i] * v.view_inv.m[i][j];
    }
    if (c[3] >= 0.0f) return false;  // the eye is on the kept side: an oblique near plane cannot clip
    Mat4& m = v.proj;
    if (far_scale > 1.0f && m.m[2][2] != 0.0f && m.m[2][2] != -1.0f) {
        // zn / zf back from the 0..1 RH projection, far plane pushed out.
        const float zn = m.m[2][3] / m.m[2][2], zf = m.m[2][3] / (m.m[2][2] + 1.0f) * far_scale;
        m.m[2][2] = zf / (zn - zf);
        m.m[2][3] = zn * zf / (zn - zf);
    }
    const float xs = m.m[0][0], ys = m.m[1][1], a = m.m[2][2], b = m.m[2][3];
    if (xs == 0.0f || ys == 0.0f || b == 0.0f) return false;
    auto sgn = [](float x) { return x > 0.0f ? 1.0f : (x < 0.0f ? -1.0f : 0.0f); };
    // The far-plane frustum corner on the plane's positive side, in view space (homogeneous): clip
    // (sgn cx, sgn cy, 1, 1) through the inverse of the 0..1 RH projection.
    const float q[4] = {sgn(c[0]) / xs, sgn(c[1]) / ys, -1.0f, (1.0f + a) / b};
    const float d = c[0] * q[0] + c[1] * q[1] + c[2] * q[2] + c[3] * q[3];
    if (!(d > 1e-30f)) return false;
    for (int j = 0; j < 4; ++j) m.m[2][j] = c[j] / d;  // z_clip = plane distance: 0 on the plane, w at that corner
    v.view_proj = m * v.view;
    return true;
}

// --------------------------------------------------------------------------------------- setup

WorldRenderer::WorldRenderer() = default;
WorldRenderer::~WorldRenderer() { shutdown(); }

int WorldRenderer::add_texture(const std::string& name, assets::TextureRole role) {
    if (name.empty()) return -1;
    const std::string key = ascii_lower(name);
    for (size_t i = 0; i < textures_.size(); ++i)
        if (ascii_lower(textures_[i].name) == key && textures_[i].role == role) return static_cast<int>(i);
    GpuTexture t;
    t.name = name;
    t.role = role;
    textures_.push_back(std::move(t));
    return static_cast<int>(textures_.size() - 1);
}

bool WorldRenderer::upload_buffer(rhi::Buffer& b, std::span<const uint8_t> bytes, VkBufferUsageFlags usage, const char* name) {
    if (bytes.empty()) return false;
    if (!r_->alloc_->create_buffer(bytes.size(), usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::MemoryUsage::GpuOnly, b, name))
        return false;
    const bool index = (usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) != 0;
    const rhi::UploadTicket t =
        r_->up_->upload_buffer(b, 0, bytes, index ? VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT : VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT,
                               index ? VK_ACCESS_2_INDEX_READ_BIT : VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
    if (!t) return false;
    tickets_.push_back(t);
    return true;
}

bool WorldRenderer::init(Renderer& r, const terrain::TerrainMesh& mesh, const terrain::WaterSurface* water,
                         const terrain::CloudParams& clouds, const TextureSource& textures, const WorldOptions& options,
                         std::string* error, const WorldDecorations* decorations) {
    shutdown();
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    r_ = &r;
    textures_src_ = textures;
    options_ = options;
    caches_ = std::make_unique<Renderer::XConstCache[]>(terrain::kMaterialSlots);
    water_cache_ = std::make_unique<Renderer::XConstCache>();
    cloud_scale_ = clouds.scale;
    cloud_vel_[0] = clouds.velocity[0];
    cloud_vel_[1] = clouds.velocity[1];
    cloud_texture_ = add_texture(clouds.texture, assets::TextureRole::Color);
    if (r.desc_.effects) {
        terrain_fx_ = r.desc_.effects->get("TerrainRenderBump");
        terrain_pass_ = first_draw_pass(terrain_fx_.get());
        if (!terrain_pass_) log::warnf("render", "world: TerrainRenderBump has no translated draw pass; terrain not drawn");
    }

    // ---- Terrain: materials, patch vertex buffers, pass index buffers + blend textures.
    for (uint32_t m = 0; m < terrain::kMaterialSlots; ++m) {
        const terrain::TerrainMaterialGpu& tm = mesh.materials[m];
        if (!tm.used) continue;
        MaterialGpu& g = materials_[m];
        g.diffuse = add_texture(tm.diffuse_texture, assets::TextureRole::Color);
        g.normal = add_texture(tm.normal_texture.empty() ? std::string("Generic_Flat_B.tga") : tm.normal_texture,
                               assets::TextureRole::Normal);
        g.tex_u = vec4(tm.tex_u);
        g.tex_v = vec4(tm.tex_v);
        g.diffuse_color = vec4(tm.material_diffuse);
        g.specular_color = vec4(tm.material_specular);
        g.shininess = {tm.shininess};
    }
    if (!sets_.init(*r.ctx_)) return fail("world descriptor pool");
    if (options_.terrain) {
        // One vertex buffer and one index buffer for every patch (draws use vertexOffset / firstIndex).
        std::vector<uint8_t> vbytes, ibytes;
        patches_.reserve(mesh.patches.size());
        for (const terrain::TerrainPatch& p : mesh.patches) {
            PatchGpu g;
            g.bounds = p.bounds;
            g.blend_scale[0] = p.blend_tex_scale[0];
            g.blend_scale[1] = p.blend_tex_scale[1];
            g.blend_offset[0] = p.blend_tex_offset[0];
            g.blend_offset[1] = p.blend_tex_offset[1];
            g.vertex_offset = static_cast<int32_t>(vbytes.size() / sizeof(terrain::TerrainVertex));
            if (p.vertices.empty() || p.vertices.size() % sizeof(terrain::TerrainVertex) != 0) {
                patches_.push_back(std::move(g));
                return fail("terrain patch without vertex data");
            }
            vbytes.insert(vbytes.end(), p.vertices.begin(), p.vertices.end());
            for (const terrain::TerrainPass& tp : p.passes) {
                PassGpu pg;
                pg.material = tp.material;
                pg.index_count = static_cast<uint32_t>(tp.indices.size());
                pg.first_index = static_cast<uint32_t>(ibytes.size() / 2);
                const std::span<const uint8_t> ix = tp.index_bytes();
                ibytes.insert(ibytes.end(), ix.begin(), ix.end());
                bool ok = pg.index_count > 0;
                rhi::ImageDesc d;
                d.format = rhi::Format::B4G4R4A4;
                d.width = d.height = terrain::kBlendTextureSize;
                d.mips = 1;
                d.swizzle = rhi::Swizzle::ARGB4_IN_BGRA4;
                d.name = "world.blend";
                ok = ok && tp.blend_texture.size() == terrain::kBlendTextureBytes && r.alloc_->create_image(d, pg.blend);
                if (ok) {
                    const rhi::UploadTicket t = r.up_->upload_image_chain(pg.blend, tp.blend_texture);
                    ok = static_cast<bool>(t);
                    if (ok) tickets_.push_back(t);
                }
                g.passes.push_back(std::move(pg));
                if (!ok) {
                    patches_.push_back(std::move(g));
                    return fail("terrain pass upload failed");
                }
            }
            patches_.push_back(std::move(g));
        }
        if (!vbytes.empty() && !upload_buffer(terrain_vb_, vbytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, "world.terrain_vb"))
            return fail("terrain vertex buffer upload failed");
        if (!ibytes.empty() && !upload_buffer(terrain_ib_, ibytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, "world.terrain_ib"))
            return fail("terrain index buffer upload failed");
    }

    // ---- Water.
    if (water && water->enabled && options_.water && r.desc_.effects) {
        water_ = *water;
        water_.patches.clear();
        water_fx_ = r.desc_.effects->get(water->effect);
        water_pass_ = first_draw_pass(water_fx_.get());
        if (!water_pass_) log::warnf("render", "world: water effect %s has no translated draw pass", water->effect.c_str());
        std::vector<uint8_t> vbytes, ibytes;
        for (const terrain::WaterPatch& wp : water->patches) {
            WaterPatchGpu g;
            g.bounds = wp.bounds;
            g.index_count = static_cast<uint32_t>(wp.indices.size());
            g.vertex_offset = static_cast<int32_t>(vbytes.size() / sizeof(terrain::WaterVertex));
            g.first_index = static_cast<uint32_t>(ibytes.size() / 2);
            const bool ok = g.index_count > 0 && !wp.vertices.empty() && wp.vertices.size() % sizeof(terrain::WaterVertex) == 0;
            water_patches_.push_back(std::move(g));
            if (!ok) return fail("water patch without geometry");
            const uint8_t* ib = reinterpret_cast<const uint8_t*>(wp.indices.data());
            vbytes.insert(vbytes.end(), wp.vertices.begin(), wp.vertices.end());
            ibytes.insert(ibytes.end(), ib, ib + wp.indices.size() * 2);
        }
        if (!vbytes.empty() && (!upload_buffer(water_vb_, vbytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, "world.water_vb") ||
                                !upload_buffer(water_ib_, ibytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, "world.water_ib")))
            return fail("water buffer upload failed");
        for (const terrain::WaterTextureBinding& b : water->bindings)
            water_textures_.push_back({b.parameter, add_texture(b.texture, assets::texture_role_for_param(b.parameter))});
        water_on_ = water_pass_ != nullptr && !water_patches_.empty();
        if (!create_targets()) return fail("water targets");
    }

    // ---- Decorations (static geometry, effects, FOW index lists, decoration textures).
    if (!init_decorations(mesh, decorations ? *decorations : WorldDecorations{}, error)) return false;

    // ---- Textures (materials, clouds, water) through the caller's source; missing ones get fallbacks.
    for (GpuTexture& t : textures_) {
        std::shared_ptr<const assets::TextureAsset> a = textures_src_ ? textures_src_(t.name, t.role) : nullptr;
        assets::TextureAsset fb;
        if (!a) {
            fb = assets::make_fallback_texture(t.role);
            t.fallback = true;
            log::warnf("render", "world: texture %s not found, using a fallback", t.name.c_str());
        }
        if (!r.upload_texture(t, a ? *a : fb)) return fail("texture upload failed: " + t.name);
        tickets_.push_back(t.ticket);
    }
    stats_ = {};
    stats_.patches = static_cast<uint32_t>(patches_.size());
    stats_.water_patches = static_cast<uint32_t>(water_patches_.size());
    stats_.textures = static_cast<uint32_t>(textures_.size());
    resident_ = false;
    return true;
}

// Targets in the scene formats, so the scene pipelines draw into them.
bool WorldRenderer::create_targets() {
    if (!r_) return false;
    Renderer& r = *r_;
    reflection_.reset();
    refraction_.reset();
    sky_rt_.shutdown();
    if (sky_cube_) {
        r.up_->forget(sky_cube_.handle);
        r.alloc_->destroy(sky_cube_);
    }
    sky_cube_valid_ = false;
    sky_dirty_ = true;
    sky_cube_state_ = rhi::ImageState::Undefined;
    if (!water_on_ || !options_.water_targets || r.ds_format_ == VK_FORMAT_UNDEFINED) return true;
    const terrain::WaterTargets& wt = water_.targets;
    auto make = [&](std::unique_ptr<WaterTarget>& out, uint32_t size, const char* name) {
        out = std::make_unique<WaterTarget>();
        rhi::RenderTargetDesc d;
        d.color = rhi::from_vk(r.desc_.color_format);
        d.depth = rhi::from_vk(r.ds_format_);
        d.width = d.height = size;
        d.extra_color_usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        d.name = name;
        return out->rt.init(*r.alloc_, d, {size, size});
    };
    const uint32_t n = std::clamp<uint32_t>(wt.size, 256, 1024);
    if (wt.reflection && !make(reflection_, n, "world.reflection")) return false;
    if (wt.refraction && !make(refraction_, n, "world.refraction")) return false;
    if (wt.sky_cube) {
        rhi::RenderTargetDesc d;
        d.color = rhi::from_vk(r.desc_.color_format);
        d.depth = rhi::from_vk(r.ds_format_);
        d.width = d.height = wt.sky_cube_size;
        d.extra_color_usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        d.name = "world.sky_face";
        if (!sky_rt_.init(*r.alloc_, d, {wt.sky_cube_size, wt.sky_cube_size})) return false;
        rhi::ImageDesc cd;
        cd.format = rhi::from_vk(r.desc_.color_format);
        cd.width = cd.height = wt.sky_cube_size;
        cd.mips = 1;
        cd.layers = 6;
        cd.cube = true;
        cd.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        cd.name = "world.sky_cube";
        if (!r.alloc_->create_image(cd, sky_cube_)) return false;
    }
    return true;
}

bool WorldRenderer::make_resident() {
    swap_pending_cloud();  // cu8g t03
    if (resident_ || !r_) return resident_;
    uint32_t pending = 0;
    for (const rhi::UploadTicket& t : tickets_) pending += r_->up_->ready(t) ? 0u : 1u;
    stats_.uploads_pending = pending;
    if (pending) return false;
    for (GpuTexture& t : textures_) t.resident = true;
    // The cloud texture also gets a bindless slot: hot-set / FF mesh passes sample CLOUD_TEXTURE through it.
    if (cloud_texture_ >= 0 && textures_[static_cast<size_t>(cloud_texture_)].image) {
        GpuTexture& t = textures_[static_cast<size_t>(cloud_texture_)];
        t.slot = cloud_slot_ = r_->allocate_texture_slot(t.image.view, "world CLOUD_TEXTURE");  // 0 (white) when full
    }
    // Decoration textures drawn by FF prim passes sample through bindless slots too.
    for (int ti : slot_textures_) {
        GpuTexture& t = textures_[static_cast<size_t>(ti)];
        if (t.image && t.slot == 0 && ti != cloud_texture_) t.slot = r_->allocate_texture_slot(t.image.view, "world decoration");
    }
    resident_ = true;
    return true;
}

WorldRenderer::ConstRegion WorldRenderer::const_region(const rhi::Frame& frame, bool int_bool) {
    const uint32_t slot = frame.slot() % kMaxSlots;
    if (cursor_frame_[slot] != frame.number()) {  // first region of this frame in this slot
        cursor_frame_[slot] = frame.number();
        cursor_[slot][0] = cursor_[slot][1] = 0;
    }
    const int k = int_bool ? 1 : 0;
    std::vector<ConstChunk>& chunks = chunks_[slot][k];
    const uint32_t index = cursor_[slot][k];
    const uint32_t ci = index / kRegionsPerChunk, ri = index % kRegionsPerChunk;
    if (ci >= chunks.size()) {
        ConstChunk c;
        if (!r_->alloc_->create_buffer(kRegionBytes * kRegionsPerChunk, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, rhi::MemoryUsage::Dynamic,
                                       c.buffer, "world.constants") ||
            !c.buffer.mapped()) {
            if (c.buffer) r_->alloc_->destroy(c.buffer);
            return {};
        }
        const VkDescriptorSetLayout layout = r_->std_layouts_.constants(int_bool);
        for (uint32_t i = 0; i < kRegionsPerChunk; ++i) {
            c.sets[i] = sets_.allocate(layout);
            if (!c.sets[i]) {
                r_->alloc_->destroy(c.buffer);
                return {};
            }
            const VkDeviceSize base = kRegionBytes * i;
            rhi::write_constants(*r_->ctx_, c.sets[i], c.buffer.handle, base, rhi::kVSConstantsBytes, c.buffer.handle,
                                 base + 4096, rhi::kPSConstantsBytes);
            if (int_bool)
                rhi::write_int_bool_constants(*r_->ctx_, c.sets[i], c.buffer.handle, base + 4608, c.buffer.handle, base + 5120);
        }
        chunks.push_back(std::move(c));
    }
    ++cursor_[slot][k];
    ConstChunk& c = chunks[ci];
    return {static_cast<uint8_t*>(c.buffer.mapped()) + kRegionBytes * ri, c.sets[ri]};
}

void WorldRenderer::update(float dt) {
    const float d = std::clamp(dt, 0.0f, 1.0f);
    water_time_ += d;
    deco_time_ += d;
    cloud_off_[0] += cloud_vel_[0] * d;
    cloud_off_[1] += cloud_vel_[1] * d;
}

void WorldRenderer::set_sky(std::span<const Instance> sky) {
    sky_.assign(sky.begin(), sky.end());
    sky_dirty_ = true;
}

const std::string* WorldRenderer::cloud_texture_name() const {
    const int t = pending_cloud_ >= 0 ? pending_cloud_ : cloud_texture_;
    return t >= 0 && static_cast<size_t>(t) < textures_.size() ? &textures_[static_cast<size_t>(t)].name : nullptr;
}

void WorldRenderer::set_clouds(const terrain::CloudParams& clouds) {
    cloud_scale_ = clouds.scale;
    cloud_vel_[0] = clouds.velocity[0];
    cloud_vel_[1] = clouds.velocity[1];
    if (!r_ || clouds.texture.empty()) return;  // [port] an environment without a cloud texture keeps the drawn one
    const std::string* now = cloud_texture_name();
    if (now && ascii_lower(*now) == ascii_lower(clouds.texture)) return;
    const int t = add_texture(clouds.texture, assets::TextureRole::Color);
    if (t < 0) return;
    if (t == cloud_texture_) {  // back to the drawn texture while another one was pending
        pending_cloud_ = -1;
        return;
    }
    GpuTexture& g = textures_[static_cast<size_t>(t)];
    if (!g.image) {  // a texture the world did not have yet: upload it as init() does
        std::shared_ptr<const assets::TextureAsset> a = textures_src_ ? textures_src_(g.name, g.role) : nullptr;
        assets::TextureAsset fb;
        if (!a) {
            fb = assets::make_fallback_texture(g.role);
            g.fallback = true;
            log::warnf("render", "world: cloud texture %s not found, using a fallback", g.name.c_str());
        }
        if (!r_->upload_texture(g, a ? *a : fb)) {
            log::warnf("render", "world: cloud texture %s upload failed; clouds unchanged", g.name.c_str());
            return;
        }
        tickets_.push_back(g.ticket);
    }
    pending_cloud_ = t;
}

// The pending cloud texture replaces the drawn one once its upload is done (called by make_resident every frame).
void WorldRenderer::swap_pending_cloud() {
    if (pending_cloud_ < 0 || !resident_ || !r_) return;
    GpuTexture& g = textures_[static_cast<size_t>(pending_cloud_)];
    if (!g.image || !r_->up_->ready(g.ticket)) return;
    g.resident = true;
    if (cloud_texture_ >= 0 && cloud_slot_ != 0) {
        r_->release_texture(cloud_slot_, nullptr);  // the old image stays in textures_ (destroyed at shutdown)
        GpuTexture& old = textures_[static_cast<size_t>(cloud_texture_)];
        if (old.slot == cloud_slot_) old.slot = 0;
    }
    cloud_texture_ = pending_cloud_;
    pending_cloud_ = -1;
    cloud_slot_ = r_->allocate_texture_slot(g.image.view, "world CLOUD_TEXTURE");  // 0 (white) when full
    if (g.slot == 0) g.slot = cloud_slot_;
    tex_sets_tp_ = nullptr;  // the batched terrain path writes new texture sets (CLOUD_TEXTURE) on its next draw
}

void WorldRenderer::shutdown() {
    if (!r_) return;
    rhi::Allocator& a = *r_->alloc_;
    rhi::Uploader& up = *r_->up_;
    auto kill_buffer = [&](rhi::Buffer& b) {
        if (!b) return;
        up.forget(b.handle);
        a.destroy(b);
    };
    auto kill_image = [&](rhi::Image& i) {
        if (!i) return;
        up.forget(i.handle);
        a.destroy(i);
    };
    for (PatchGpu& p : patches_)
        for (PassGpu& q : p.passes) kill_image(q.blend);
    patches_.clear();
    water_patches_.clear();
    kill_buffer(terrain_vb_);
    kill_buffer(terrain_ib_);
    kill_buffer(water_vb_);
    kill_buffer(water_ib_);
    for (auto& per_slot : chunks_)
        for (auto& list : per_slot) {
            for (ConstChunk& c : list) a.destroy(c.buffer);
            list.clear();
        }
    for (auto& c : cursor_) c[0] = c[1] = 0;
    for (uint64_t& f : cursor_frame_) f = 0;
    sets_.shutdown();
    tex_sets_tp_ = nullptr;
    visible_.clear();
    if (cloud_texture_ >= 0 && static_cast<size_t>(cloud_texture_) < textures_.size() && cloud_slot_ != 0)
        r_->release_texture(cloud_slot_, &textures_[static_cast<size_t>(cloud_texture_)].image);  // deferred like model textures
    cloud_slot_ = 0;
    cloud_texture_ = -1;
    pending_cloud_ = -1;
    for (int ti : slot_textures_) {
        GpuTexture& t = textures_[static_cast<size_t>(ti)];
        if (t.slot != 0 && t.image) r_->release_texture(t.slot, &t.image);
        t.slot = 0;
    }
    slot_textures_.clear();
    shutdown_decorations();
    for (GpuTexture& t : textures_) kill_image(t.image);
    textures_.clear();
    reflection_.reset();
    refraction_.reset();
    sky_rt_.shutdown();
    kill_image(sky_cube_);
    sky_cube_state_ = rhi::ImageState::Undefined;
    tickets_.clear();
    water_textures_.clear();
    for (MaterialGpu& m : materials_) m = MaterialGpu{};
    terrain_fx_.reset();
    water_fx_.reset();
    terrain_pass_ = water_pass_ = nullptr;
    water_on_ = resident_ = sky_cube_valid_ = false;
    sky_.clear();
    caches_.reset();
    water_cache_.reset();
    if (r_->world_ == this) r_->set_world(nullptr);  // also drops the cloud / FOW views and slots
    r_ = nullptr;
}

// ------------------------------------------------------------------------------- renderer side

// Copies one draw's register images into a persistent constant region (layout of kRegionBytes).
void WorldRenderer::copy_region(uint8_t* dst, const Renderer::XConstCache& cc, bool int_bool) {
    std::memcpy(dst, &cc.vs, sizeof cc.vs);
    std::memcpy(dst + 4096, &cc.ps, sizeof cc.ps);
    if (int_bool) {
        std::memcpy(dst + 4608, &cc.vib, sizeof cc.vib);
        std::memcpy(dst + 5120, &cc.pib, sizeof cc.pib);
    }
}

void Renderer::world_draw_terrain(rhi::Frame& frame, VkCommandBuffer cmd, const ViewMatrices& view, uint32_t width,
                                  uint32_t height, bool mirrored, bool count) {
    WorldRenderer& w = *world_;
    if (!w.resident_ || !w.terrain_pass_ || !w.options_.terrain || !w.caches_ || !w.terrain_vb_ || !w.terrain_ib_) return;
    const uint64_t gen = ++w.draw_generation_;
    auto view_of = [&](int t) { return t >= 0 && w.textures_[static_cast<size_t>(t)].image ? w.textures_[static_cast<size_t>(t)].image.view : white_.view; };
    const VkImageView cloud = view_of(w.cloud_texture_);
    uint32_t visible = 0, draws = 0, pending = 0;
    uint64_t tris = 0;
    PostSpec ps;
    ps.fx = w.terrain_fx_;
    ps.pass = w.terrain_pass_;
    ps.render_pass = rp_scene_;
    ps.width = width;
    ps.height = height;
    ps.view = &view;
    ps.layout = materials::VertexLayout::PrimVertLit;
    ps.alpha_blend = true;
    ps.mirrored = mirrored;
    ps.fog = true;
    ps.vb = w.terrain_vb_.handle;
    ps.ib = w.terrain_ib_.handle;
    const bool preshader = w.terrain_pass_->vs_plan->preshader.has_value() || w.terrain_pass_->ps_plan->preshader.has_value();
    // Visible draws: patches in row-major order, each patch's passes in list order (the blend order).
    w.visible_.clear();
    for (uint32_t pi = 0; pi < w.patches_.size(); ++pi) {
        const WorldRenderer::PatchGpu& p = w.patches_[pi];
        if (patch_outside(view.view_proj, p.bounds)) continue;
        ++visible;
        for (uint32_t qi = 0; qi < p.passes.size(); ++qi) w.visible_.push_back({pi, qi});
    }
    // Per draw: patch values (blendTexScale / Offset), material values only when its register images are
    // (re)filled this call.
    // Q3: spec values / textures in arrays that outlive each draw (PostSpec holds views).
    PostValue patch_vals[2], mat_vals[5];
    PostTexture tex_list[4];
    auto draw_values = [&](const WorldRenderer::PatchGpu& p, const WorldRenderer::PassGpu& q, XConstCache& cc) {
        patch_vals[0] = {"blendTexScale", std::span<const float>(p.blend_scale)};
        patch_vals[1] = {"blendTexOffset", std::span<const float>(p.blend_offset)};
        ps.patch_values = patch_vals;
        const WorldRenderer::MaterialGpu& m = w.materials_[q.material];
        if (cc.generation != gen) cc.generation = gen;
        if (cc.filled_generation != gen || preshader) {
            mat_vals[0] = {"diffuseTexU", m.tex_u};
            mat_vals[1] = {"diffuseTexV", m.tex_v};
            mat_vals[2] = {"materialDiffuse", m.diffuse_color};
            mat_vals[3] = {"materialSpecular", m.specular_color};
            mat_vals[4] = {"shininess", m.shininess};
            ps.values = mat_vals;
        } else {
            ps.values = {};
        }
    };
    auto draw_textures = [&](const WorldRenderer::PassGpu& q) {
        const WorldRenderer::MaterialGpu& m = w.materials_[q.material];
        tex_list[0] = {"diffuseTexture", view_of(m.diffuse)};
        tex_list[1] = {"normalTexture", view_of(m.normal)};
        tex_list[2] = {"blendTexture", q.blend.view};
        tex_list[3] = {"CLOUD_TEXTURE", cloud};
        ps.textures = tex_list;
    };
    WorldBindStats& bs = w.stats_.binds;
    if (!w.options_.batch_draws) {
        // Reference path: every draw through post_translated (own uniforms, sets and binds).
        bind_stats_ = &bs;
        for (const auto& [pi, qi] : w.visible_) {
            const WorldRenderer::PatchGpu& p = w.patches_[pi];
            const WorldRenderer::PassGpu& q = p.passes[qi];
            XConstCache& cc = w.caches_[q.material];
            draw_values(p, q, cc);
            draw_textures(q);
            ps.vb_offset = static_cast<VkDeviceSize>(p.vertex_offset) * sizeof(terrain::TerrainVertex);
            ps.ib_offset = static_cast<VkDeviceSize>(q.first_index) * 2;
            ps.index_count = q.index_count;
            if (post_translated(frame, cmd, ps, &cc)) {
                ++draws;
                tris += q.index_count / 3;
            } else {
                ++pending;
            }
        }
        bind_stats_ = nullptr;
    } else if (!w.visible_.empty()) {
        GeoState g;
        if (!translated_state(ps, nullptr, g)) {
            pending = static_cast<uint32_t>(w.visible_.size());
        } else {
            // Texture sets are written once per (patch, pass); a new program or sampler set (the anisotropy setting)
            // starts new ones (the old sets may still be in use by frames in flight, so they are not rewritten).
            if (w.tex_sets_tp_ != g.tp || std::memcmp(w.tex_sets_samplers_, g.samplers, sizeof g.samplers) != 0) {
                for (WorldRenderer::PatchGpu& p : w.patches_)
                    for (WorldRenderer::PassGpu& q : p.passes) q.textures = VK_NULL_HANDLE;
                w.tex_sets_tp_ = g.tp;
                std::memcpy(w.tex_sets_samplers_, g.samplers, sizeof g.samplers);
            }
            bind_stats_ = &bs;
            bind_geometry_state(cmd, ps, g, w.terrain_vb_.handle, w.terrain_ib_.handle);
            bind_stats_ = nullptr;
            const rhi::DeviceFns& fn = ctx_->vkd();
            for (size_t k = 0; k < w.visible_.size(); ++k) {
                const auto [pi, qi] = w.visible_[k];
                const WorldRenderer::PatchGpu& p = w.patches_[pi];
                WorldRenderer::PassGpu& q = w.patches_[pi].passes[qi];
                XConstCache& cc = w.caches_[q.material];
                draw_values(p, q, cc);
                translated_fill(ps, cc, true, g.int_bool);
                const WorldRenderer::ConstRegion reg = w.const_region(frame, g.int_bool);
                if (!q.textures && reg.ptr) {
                    q.textures = w.sets_.allocate(std_layouts_.textures());
                    if (q.textures) {
                        draw_textures(q);
                        rhi::TextureBinding tb[rhi::kTextureSlots];
                        const uint32_t ntb = translated_textures(ps, g, tb);
                        if (ntb) rhi::write_textures(*ctx_, q.textures, std::span<const rhi::TextureBinding>(tb, ntb));
                        ++bs.set_allocs;
                        bs.set_writes += ntb ? 1u : 0u;
                    }
                }
                if (!reg.ptr || !q.textures) {
                    log::errorf("render", "world: constant regions / descriptor pool exhausted");
                    pending += static_cast<uint32_t>(w.visible_.size() - k);
                    break;
                }
                WorldRenderer::copy_region(reg.ptr, cc, g.int_bool);
                const VkDescriptorSet xs[2] = {reg.set, q.textures};
                fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 2, xs, 0, nullptr);
                fn.vkCmdDrawIndexed(cmd, q.index_count, 1, q.first_index, p.vertex_offset, 0);
                ++bs.set_binds;
                ++bs.draws;
                ++draws;
                tris += q.index_count / 3;
            }
        }
    }
    if (count) {
        w.stats_.visible_patches = visible;
        w.stats_.terrain_draws = draws;
        w.stats_.terrain_pending = pending;
        w.stats_.terrain_triangles = tris;
    } else {
        if (mirrored) w.stats_.reflection_draws += draws;
        else w.stats_.refraction_draws += draws;
    }
}

void Renderer::world_draw_water(rhi::Frame& frame, VkCommandBuffer cmd, const ViewMatrices& view, uint32_t width, uint32_t height) {
    WorldRenderer& w = *world_;
    if (!w.resident_ || !w.water_on_ || !w.water_cache_ || !w.water_vb_ || !w.water_ib_) return;
    const terrain::WaterConstants wc = terrain::water_constants(w.water_, w.water_time_);
    PostSpec ps;
    ps.fx = w.water_fx_;
    ps.pass = w.water_pass_;
    ps.render_pass = rp_scene_;
    ps.width = width;
    ps.height = height;
    ps.view = &view;
    ps.layout = kWaterLayout;
    ps.fog = true;
    ps.vb = w.water_vb_.handle;
    ps.ib = w.water_ib_.handle;
    const float ice_rd[1] = {wc.ice_reflection_distortion};
    const PostValue water_vals[] = {{"BaseTexU", wc.base_u},
                                    {"BaseTexV", wc.base_v},
                                    {"Bump0TexU", wc.bump0_u},
                                    {"Bump0TexV", wc.bump0_v},
                                    {"Bump1TexU", wc.bump1_u},
                                    {"Bump1TexV", wc.bump1_v},
                                    {"ReflectionDistortionScale", wc.reflection_distortion},
                                    {"RefractionDistortionScale", wc.refraction_distortion},
                                    {"WaterReflectC", wc.reflect_c},
                                    {"WaterRefractC", wc.refract_c},
                                    {"WaterColor", wc.water_color},
                                    {"CrustTexU", wc.crust_u},
                                    {"CrustTexV", wc.crust_v},
                                    {"CrustSpecular", wc.crust_specular},
                                    {"BumpTexU", wc.ice_bump_u},
                                    {"BumpTexV", wc.ice_bump_v},
                                    {"IceDiffuse", wc.ice_diffuse},
                                    {"IceReflectionDistortion", ice_rd}};
    ps.values = water_vals;
    PostTexture texs[24];  // Q3: no per-frame list allocation (water effects bind a handful of textures)
    size_t nt = 0;
    auto add_tex = [&](std::string_view n, VkImageView v) {
        if (nt < std::size(texs)) texs[nt++] = {n, v};
    };
    for (const auto& [param, t] : w.water_textures_)
        add_tex(param, t >= 0 && w.textures_[static_cast<size_t>(t)].image ? w.textures_[static_cast<size_t>(t)].image.view : white_.view);
    // Only the targets recorded this frame (water detail may have skipped one; a stale image must not show).
    if (w.reflection_ && w.stats_.reflection && w.reflection_->rt.color_state() == rhi::ImageState::ShaderRead)
        add_tex("ReflectionTexture", w.reflection_->rt.color().view);
    if (w.refraction_ && w.stats_.refraction && w.refraction_->rt.color_state() == rhi::ImageState::ShaderRead)
        add_tex("RefractionTexture", w.refraction_->rt.color().view);
    if (w.sky_cube_valid_) add_tex("SKY_CUBE_TEXTURE", w.sky_cube_.view);
    ps.textures = std::span<const PostTexture>(texs, nt);
    w.water_cache_->generation = ++w.draw_generation_;
    uint32_t draws = 0, pending = 0;
    uint64_t tris = 0;
    std::vector<uint32_t>& vis = w.water_vis_scratch_;  // Q3: kept across frames
    vis.clear();
    for (uint32_t i = 0; i < w.water_patches_.size(); ++i)
        if (!patch_outside(view.view_proj, w.water_patches_[i].bounds)) vis.push_back(i);
    WorldBindStats& bs = w.stats_.binds;
    if (!w.options_.batch_draws) {
        bind_stats_ = &bs;
        for (uint32_t i : vis) {
            const WorldRenderer::WaterPatchGpu& p = w.water_patches_[i];
            ps.vb_offset = static_cast<VkDeviceSize>(p.vertex_offset) * sizeof(terrain::WaterVertex);
            ps.ib_offset = static_cast<VkDeviceSize>(p.first_index) * 2;
            ps.index_count = p.index_count;
            if (post_translated(frame, cmd, ps, w.water_cache_.get())) {
                ++draws;
                tris += p.index_count / 3;
            } else {
                ++pending;
            }
        }
        bind_stats_ = nullptr;
    } else if (!vis.empty()) {
        // Every water draw has the same constants and textures: one region, one texture set, one bind.
        GeoState g;
        XConstCache& cc = *w.water_cache_;
        const WorldRenderer::ConstRegion reg = translated_state(ps, nullptr, g) ? w.const_region(frame, g.int_bool)
                                                                               : WorldRenderer::ConstRegion{};
        const VkDescriptorSet tex = reg.ptr ? frame.descriptors(0).allocate(std_layouts_.textures()) : VK_NULL_HANDLE;
        if (!tex) {
            pending = static_cast<uint32_t>(vis.size());
        } else {
            translated_fill(ps, cc, true, g.int_bool);
            WorldRenderer::copy_region(reg.ptr, cc, g.int_bool);
            rhi::TextureBinding tb[rhi::kTextureSlots];
            const uint32_t ntb = translated_textures(ps, g, tb);
            if (ntb) rhi::write_textures(*ctx_, tex, std::span<const rhi::TextureBinding>(tb, ntb));
            ++bs.set_allocs;
            bs.set_writes += ntb ? 1u : 0u;
            bind_stats_ = &bs;
            bind_geometry_state(cmd, ps, g, w.water_vb_.handle, w.water_ib_.handle);
            bind_stats_ = nullptr;
            const rhi::DeviceFns& fn = ctx_->vkd();
            const VkDescriptorSet xs[2] = {reg.set, tex};
            fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 2, xs, 0, nullptr);
            ++bs.set_binds;
            for (uint32_t i : vis) {
                const WorldRenderer::WaterPatchGpu& p = w.water_patches_[i];
                fn.vkCmdDrawIndexed(cmd, p.index_count, 1, p.first_index, p.vertex_offset, 0);
                ++bs.draws;
                ++draws;
                tris += p.index_count / 3;
            }
        }
    }
    w.stats_.water_draws = draws;
    w.stats_.water_pending = pending;
    w.stats_.water_triangles = tris;
}

void Renderer::reset_world_constants() {
    cloud_view_ = VK_NULL_HANDLE;
    cloud_slot_ = materials::kNoTexture;
    cloud_u_ = cloud_v_ = materials::float4{0, 0, 0, 0};
    fow_u_ = fow_v_ = materials::float4{0, 0, 0, 0};
    fow_view_ = VK_NULL_HANDLE;
    fow_slot_ = materials::kNoTexture;
}

void Renderer::world_frame_constants() {
    WorldRenderer& w = *world_;
    w.upload_fow_pending();  // a finished FOW upload replaces the previous texture
    WorldRenderer::cloud_matrices(w.cloud_scale_, w.cloud_off_[0], w.cloud_off_[1], cloud_u_, cloud_v_);
    const int t = w.cloud_texture_;
    cloud_view_ = w.resident_ && t >= 0 && w.textures_[static_cast<size_t>(t)].image ? w.textures_[static_cast<size_t>(t)].image.view
                                                                                    : VK_NULL_HANDLE;
    cloud_slot_ = cloud_view_ ? w.cloud_slot_ : materials::kNoTexture;
    // FOW_TEX_U / V of the box always; FOW_TEXTURE only while the fog of war is on (else white).
    fow_u_ = w.fow_u_;
    fow_v_ = w.fow_v_;
    const bool fow = w.resident_ && w.fow_enabled();
    fow_view_ = fow ? w.fow_image_.view : VK_NULL_HANDLE;
    fow_slot_ = fow ? w.fow_slot_ : materials::kNoTexture;
}

bool Renderer::world_prepass(rhi::Frame& frame, VkCommandBuffer cmd, const ViewMatrices& view, std::span<const Instance> instances,
                             std::span<const Instance> sky, DrawMode mode, const RecordOptions& opts) {
    (void)sky;
    WorldRenderer& w = *world_;
    WorldStats& ws = w.stats_;
    ws.visible_patches = ws.terrain_draws = ws.terrain_pending = 0;
    ws.terrain_triangles = ws.water_triangles = 0;
    ws.water_draws = ws.water_pending = ws.reflection_draws = ws.refraction_draws = 0;
    ws.reflection = ws.refraction = ws.sky_cube_rendered = false;
    ws.binds = {};
    if (!w.resident_ || !w.water_on_) return true;
    const rhi::DeviceFns& fn = ctx_->vkd();
    const float env_clear[4] = {lighting_.clear_color.x, lighting_.clear_color.y, lighting_.clear_color.z, 1.0f};
    RecordOptions xo = opts;
    xo.shadows = false;
    xo.post = false;

    // ---- Sky cube: the skydome objects into the 6 faces (re-rendered when the environment / sky list changed).
    if (w.sky_cube_) {
        bool sky_ready = true;
        for (const Instance& s : w.sky_) sky_ready = sky_ready && s.model && s.model->resident();
        if (!w.sky_cube_valid_ || (w.sky_dirty_ && sky_ready)) {
            const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
            // From the tracked state: waits for the previous frame's fragment reads before overwriting.
            rhi::cmd_barrier(fn, cmd, rhi::transition(w.sky_cube_.handle, w.sky_cube_state_, rhi::ImageState::TransferDst, all));
            const uint32_t n = w.sky_rt_.extent().width;
            // Face f looks along its cube axis with the D3D/Vulkan face orientation (right = +s, up = -t),
            // so a lookup with world direction d returns the sky seen along d. Those frames are mirrored.
            static const float kFace[6][3][3] = {
                // right, up, forward
                {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}},   {{0, 0, 1}, {0, 1, 0}, {-1, 0, 0}},
                {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},   {{1, 0, 0}, {0, 0, 1}, {0, -1, 0}},
                {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},    {{-1, 0, 0}, {0, 1, 0}, {0, 0, -1}},
            };
            bool pending = false;
            for (int f = 0; f < 6; ++f) {
                ViewMatrices fv;
                fv.width = fv.height = n;
                for (int j = 0; j < 3; ++j) {
                    fv.view.m[0][j] = kFace[f][0][j];
                    fv.view.m[1][j] = kFace[f][1][j];
                    fv.view.m[2][j] = -kFace[f][2][j];
                }
                fv.proj = perspective_rh(1.5707963f, 1.0f, 10.0f, 100000.0f);
                fv.view_proj = fv.proj * fv.view;
                fv.view_inv = fv.view.inverse_affine();
                fv.eye = {0, 0, 0};
                Built& sb = built(kBuiltCube);
                sb.items.clear();
                DrawStats ds;
                RecordOptions so = xo;
                so.mirrored = true;  // every face frame (right, up, -forward) has determinant -1
                if (!w.sky_.empty() && sky_ready && !build_sky(frame, fv, w.sky_, mode, so, sb, sky_storage_[2], ds)) return false;
                pending = pending || ds.pending_pipeline > 0 || ds.translation_pending > 0;
                begin_target(fn, cmd, w.sky_rt_, true, env_clear, true, false);
                if (!sb.items.empty()) record_range(cmd, sb, 0, sb.items.size(), n, n, true);
                fn.vkCmdEndRendering(cmd);
                w.sky_rt_.transition_color(fn, cmd, rhi::ImageState::TransferSrc);
                auto region = rhi::vk_struct<VkImageCopy2>(VK_STRUCTURE_TYPE_IMAGE_COPY_2);
                region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, static_cast<uint32_t>(f), 1};
                region.extent = {n, n, 1};
                auto ci = rhi::vk_struct<VkCopyImageInfo2>(VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2);
                ci.srcImage = w.sky_rt_.color().handle;
                ci.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                ci.dstImage = w.sky_cube_.handle;
                ci.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                ci.regionCount = 1;
                ci.pRegions = &region;
                fn.vkCmdCopyImage2(cmd, &ci);
            }
            rhi::cmd_barrier(fn, cmd, rhi::transition(w.sky_cube_.handle, rhi::ImageState::TransferDst, rhi::ImageState::ShaderRead, all));
            w.sky_cube_state_ = rhi::ImageState::ShaderRead;
            w.sky_cube_valid_ = true;
            w.sky_dirty_ = !sky_ready || pending;
            ws.sky_cube_rendered = true;
        }
    }

    const float h = w.water_.height, off = w.water_.targets.clip_plane_offset;
    // Mesh buckets of the extra passes (alScene bucket mask 0x12B: Terrain, TerrainMesh, Opaque,
    // OpaqueNonOccluding, Transparent).
    auto record_masked = [&](const Built& b, uint32_t n) {
        const PhaseBucket ranges[4][2] = {{PhaseBucket::Terrain, PhaseBucket::Fissure},
                                          {PhaseBucket::Opaque, PhaseBucket::Occluded},
                                          {PhaseBucket::OpaqueNonOccluding, PhaseBucket::Shadow},
                                          {PhaseBucket::Transparent, PhaseBucket::Lasers}};
        uint32_t drawn = 0;
        for (const auto& rg : ranges) {
            const size_t b0 = detail::bucket_begin(b.items, rg[0]), b1 = detail::bucket_begin(b.items, rg[1]);
            if (b1 > b0) {
                record_range(cmd, b, b0, b1, n, n, true);
                drawn += static_cast<uint32_t>(b1 - b0);
            }
        }
        return drawn;
    };
    // Water detail (RecordOptions::water_detail) capped by shader detail (RENDER.md 1.5): 2 both targets, 1 reflection only, 0 none.
    int water_detail = std::clamp(opts.water_detail, 0, 2);
    if (opts.shader_detail < 3) water_detail = std::min(water_detail, 1);
    if (opts.shader_detail < 1) water_detail = 0;
    // ---- Reflection: mirrored camera, keep z >= height - offset; white clear + the skydome objects first.
    if (w.reflection_ && water_detail >= 1) {
        rhi::RenderTarget& rt = w.reflection_->rt;
        const uint32_t n = rt.extent().width;
        ViewMatrices mv = WorldRenderer::mirrored_view(view, h);
        WorldRenderer::clip_projection(mv, Float4{0.0f, 0.0f, 1.0f, -(h - off)}, WorldRenderer::kObliqueFarScale);
        RecordOptions ro = xo;
        ro.mirrored = true;
        Built& rb = built(kBuiltReflect);
        Built& skb = built(kBuiltReflectSky);
        skb.items.clear();
        DrawStats rs, ss;
        if (!build(frame, mv, instances, mode, ro, rb, rs, false)) return false;
        if (!w.sky_.empty() && !build_sky(frame, mv, w.sky_, mode, ro, skb, sky_storage_[1], ss)) return false;
        const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        begin_target(fn, cmd, rt, true, white, true, false);
        if (!skb.items.empty()) record_range(cmd, skb, 0, skb.items.size(), n, n, true);
        fn.vkCmdEndRendering(cmd);
        begin_target(fn, cmd, rt, true, nullptr, true, false);
        world_draw_terrain(frame, cmd, mv, n, n, true, false);
        ws.reflection_draws += record_masked(rb, n);
        fn.vkCmdEndRendering(cmd);
        rt.transition_color(fn, cmd, rhi::ImageState::ShaderRead);
        ws.reflection = true;
    }
    // ---- Refraction: the camera, keep z <= height + offset (what is under the surface).
    if (w.refraction_ && water_detail >= 2) {
        rhi::RenderTarget& rt = w.refraction_->rt;
        const uint32_t n = rt.extent().width;
        ViewMatrices rv = view;
        WorldRenderer::clip_projection(rv, Float4{0.0f, 0.0f, -1.0f, h + off}, WorldRenderer::kObliqueFarScale);
        Built& rb = built(kBuiltRefract);
        DrawStats rs;
        if (!build(frame, rv, instances, mode, xo, rb, rs, false)) return false;
        begin_target(fn, cmd, rt, true, env_clear, true, false);
        world_draw_terrain(frame, cmd, rv, n, n, false, false);
        ws.refraction_draws += record_masked(rb, n);
        fn.vkCmdEndRendering(cmd);
        rt.transition_color(fn, cmd, rhi::ImageState::ShaderRead);
        ws.refraction = true;
    }
    return true;
}

}  // namespace alamo::render
