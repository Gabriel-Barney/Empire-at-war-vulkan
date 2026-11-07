// alamo_render - ability beams, renderer side (docs/re/ABILITY_VISUALS.md 4.1 draw 0x37fa50): Engine\PrimAdditive batches per
// texture in the Transparent bucket. The CPU side (animation, geometry) is in beams.cpp.
#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "alamo/core/ascii.hpp"
#include "alamo/core/log.hpp"
#include "alamo/render/beams.hpp"
#include "alamo/render/renderer.hpp"
#include "render_internal.hpp"

namespace alamo::render {

struct Renderer::BeamRenderState {
    TextureSource source;
    LightningLibrary lightning;
    BeamAnimator anim;
    BeamGeometry geo;
    std::shared_ptr<const materials::MaterialEffect> fx;
    const materials::ResolvedPass* pass = nullptr;
    std::shared_ptr<const materials::FfDrawTemplate> tpl;
    std::map<std::string, GpuTexture> tex;  // lower-case texture name -> texture (loaded on first use)
    rhi::Buffer vb[4];                      // per frame-ring slot: vertices then 32-bit indices
    VkDeviceSize vb_size[4] = {};
};

bool Renderer::init_beams(const TextureSource& textures, const LightningLibrary* lightning) {
    if (!ctx_ || !desc_.effects) return false;
    shutdown_beams();
    auto st = std::make_shared<BeamRenderState>();
    st->fx = desc_.effects->get("PrimAdditive");
    if (st->fx)
        for (const materials::ResolvedPass& p : st->fx->passes) {
            if (p.restore_only) continue;
            if (p.fixed_function && p.ff) st->pass = &p;
            break;
        }
    if (!st->pass || !st->fx->effect) {
        log::warnf("render", "beams: Engine\\PrimAdditive has no fixed-function pass; ability beams not drawn");
        return false;
    }
    auto tpl = std::make_shared<materials::FfDrawTemplate>();
    const materials::ParamStore store;
    std::string err;
    if (!tpl->build(*st->pass->ff, *st->fx->effect, store.lookup(), true, &err)) {
        log::warnf("render", "beams: PrimAdditive template: %s", err.c_str());
        return false;
    }
    st->tpl = std::move(tpl);
    st->source = textures;
    st->lightning = lightning ? *lightning : LightningLibrary::builtin();
    st->anim.set_lightning(&st->lightning);
    beams_ = std::move(st);
    return true;
}

void Renderer::release_beams() { shutdown_beams(); }

bool Renderer::beams_ready() const { return beams_ && beams_->pass && beams_->tpl; }

const BeamGeometry& Renderer::last_beam_geometry() const {
    static const BeamGeometry kEmpty;
    return beams_ ? beams_->geo : kEmpty;
}

void Renderer::set_beams(std::span<const BeamInstance> beams, uint64_t tick) {
    if (!beams_) {
        beams_active_ = false;
        return;
    }
    beams_->anim.step(tick, beams);
    beams_active_ = !beams_->anim.draws().empty();
}

void Renderer::shutdown_beams() {
    if (beams_) {
        for (auto& [name, t] : beams_->tex)
            if (t.image) release_texture(t.resident ? t.slot : 0u, &t.image);
        for (rhi::Buffer& b : beams_->vb) release_buffer(b);
    }
    beams_.reset();
    beams_active_ = false;
}

void Renderer::draw_beams(rhi::Frame& frame, VkCommandBuffer cmd, const ViewMatrices& view, DrawStats& st) {
    if (!beams_ || !beams_->pass) return;
    BeamRenderState& bs = *beams_;
    bs.geo.build(bs.anim.draws(), view);
    if (bs.geo.batches.empty()) return;
    // Textures: loaded on first use, resident once the upload landed (a batch whose texture is not resident waits).
    struct Ready {
        const BeamGeometry::Batch* batch;
        const GpuTexture* tex;
    };
    std::vector<Ready> ready;
    for (const BeamGeometry::Batch& batch : bs.geo.batches) {
        const std::string key = ascii_lower(batch.texture);
        auto it = bs.tex.find(key);
        if (it == bs.tex.end()) {
            GpuTexture t;
            t.name = batch.texture;
            t.role = assets::TextureRole::Color;
            std::shared_ptr<const assets::TextureAsset> a = bs.source ? bs.source(t.name, t.role) : nullptr;
            assets::TextureAsset fb;
            if (!a) {
                fb = assets::make_fallback_texture(t.role);
                t.fallback = true;
                log::warnf("render", "beams: texture %s not found, using a fallback", t.name.c_str());
            }
            if (!upload_texture(t, a ? *a : fb)) {
                ++st.beam_pending;
                continue;
            }
            it = bs.tex.emplace(key, std::move(t)).first;
        }
        GpuTexture& t = it->second;
        if (t.image && !t.resident && up_->ready(t.ticket)) {
            t.slot = allocate_texture_slot(t.image.view, "beam");
            t.resident = true;
        }
        if (!t.resident) {
            ++st.beam_pending;
            continue;
        }
        ready.push_back({&batch, &t});
    }
    if (ready.empty()) return;
    const VkDeviceSize vbytes = bs.geo.vertices.size() * sizeof(PrimVertex);
    const VkDeviceSize ibytes = bs.geo.indices.size() * sizeof(uint32_t);
    const VkDeviceSize ib_off = (vbytes + 63) / 64 * 64, need = ib_off + ibytes;
    const uint32_t slot = frame.slot() % 4;
    rhi::Buffer& buf = bs.vb[slot];
    if (bs.vb_size[slot] < need) {
        if (buf) alloc_->destroy(buf);  // this slot's previous frame has completed
        const VkDeviceSize size = std::max<VkDeviceSize>(need + need / 2, 16 * 1024);
        if (!alloc_->create_buffer(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, rhi::MemoryUsage::Dynamic,
                                   buf, "render.beams") ||
            !buf.mapped()) {
            if (buf) alloc_->destroy(buf);
            bs.vb_size[slot] = 0;
            ++st.beam_pending;
            return;
        }
        bs.vb_size[slot] = size;
    }
    auto* base = static_cast<uint8_t*>(buf.mapped());
    std::memcpy(base, bs.geo.vertices.data(), static_cast<size_t>(vbytes));
    std::memcpy(base + ib_off, bs.geo.indices.data(), static_cast<size_t>(ibytes));
    alloc_->flush(buf.alloc);
    std::vector<FfGeoDraw> draws;
    draws.reserve(ready.size());
    const auto ib_first = static_cast<uint32_t>(ib_off / sizeof(uint32_t));
    for (const Ready& r : ready) {
        FfGeoDraw g;
        g.fx = bs.fx;
        g.pass = bs.pass;
        g.tpl = bs.tpl.get();
        g.layout = materials::VertexLayout::PrimVert;
        g.vb = buf.handle;
        g.vb_offset = 0;
        g.ib = buf.handle;
        g.first_index = ib_first + r.batch->first_index;
        g.index_count = r.batch->index_count;
        g.tex[0] = r.tex->slot;
        g.sampler[0] = materials::kSamplerWrapLinear;  // mode 1 tiles U (0 .. 200 per segment)
        g.cull_none = true;
        g.no_depth_write = true;
        g.fog = true;
        draws.push_back(std::move(g));
    }
    const uint32_t drawn = ff_geometry_draws(frame, cmd, view, std::span<const FfGeoDraw>(draws.data(), draws.size()));
    if (drawn == draws.size()) {
        st.beam_ribbons += bs.geo.ribbons;
        st.beam_dots += bs.geo.dots;
        st.beam_bolts += bs.geo.bolts;
        st.draws += drawn;
        st.ff_draws += drawn;
    } else {
        st.beam_pending += static_cast<uint32_t>(draws.size()) - drawn;
    }
}

}  // namespace alamo::render
