// Planet portraits (see planet_portrait.hpp).
#include "alamo/render/galactic/planet_portrait.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <vector>

#include "alamo/render/camera.hpp"
#include "alamo/render/sim_view.hpp"
#include "alamo/rhi/context.hpp"
#include "alamo/rhi/sync.hpp"

namespace alamo::render {

namespace {
std::string upper_of(std::string_view s) {
    std::string o(s);
    for (char& c : o) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return o;
}
bool iprefix(std::string_view s, std::string_view p) {
    if (s.size() < p.size()) return false;
    for (size_t i = 0; i < p.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(p[i]))) return false;
    return true;
}
constexpr float kPortraitPitch = 0.34906585f;  // 20 degrees down [port]
constexpr float kPortraitFill = 0.9f;          // bounding sphere diameter / frame height [port]
constexpr float kLightYawOffset = -0.7853982f;  // sun 45 degrees round from the camera [port]
constexpr float kLightElevation = 0.5235988f;   // and 30 degrees up [port]

// The portrait is drawn with RecordOptions::coverage_alpha over (0, 0, 0, 0): colour premultiplied, alpha = the
// coverage of the opaque and alpha-blended passes, additive light (the planet's glow quad) in the colour only.
// The GUI blends straight alpha (SrcAlpha / InvSrcAlpha), so [port]: alpha = max(coverage, brightest channel) and
// the colour divided by it; the result composites as colour + dialog x (1 - alpha): opaque pixels unchanged, the
// glow added over the dialog (dimming it by the glow's brightness), black background fully transparent.
void straight_alpha(std::vector<uint8_t>& px) {
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        const uint32_t a = std::max<uint32_t>({px[i + 3], px[i], px[i + 1], px[i + 2]});
        px[i + 3] = static_cast<uint8_t>(a);
        if (a == 0 || a == 255) continue;
        for (size_t c = 0; c < 3; ++c)
            px[i + c] = static_cast<uint8_t>(std::min<uint32_t>(255u, (px[i + c] * 255u + a / 2u) / a));
    }
}
}  // namespace

bool is_planet_portrait_name(std::string_view name) {
    return iprefix(name, kPlanetPortraitPrefix) && name.size() > kPlanetPortraitPrefix.size();
}
std::string planet_portrait_name(std::string_view planet_type) { return std::string(kPlanetPortraitPrefix) + std::string(planet_type); }
std::string_view planet_portrait_type(std::string_view name) {
    return is_planet_portrait_name(name) ? name.substr(kPlanetPortraitPrefix.size()) : std::string_view{};
}

PlanetPortraits::PlanetPortraits(Renderer& r, rhi::Allocator& alloc, rhi::FrameRing& ring, GalacticModelSource& models,
                                 const GalacticTypeTable& types)
    : r_(&r), alloc_(&alloc), ring_(&ring), models_(&models), types_(&types), lighting_(galactic_lighting(GalacticLightSettings{}, 1.0f)) {}

PlanetPortraits::~PlanetPortraits() { destroy(); }

PlanetPortraits::Job* PlanetPortraits::find(std::string_view name) {
    const std::string_view t = is_planet_portrait_name(name) ? planet_portrait_type(name) : name;
    auto it = jobs_.find(upper_of(t));
    return it == jobs_.end() ? nullptr : it->second.get();
}
const PlanetPortraits::Job* PlanetPortraits::find(std::string_view name) const {
    const std::string_view t = is_planet_portrait_name(name) ? planet_portrait_type(name) : name;
    auto it = jobs_.find(upper_of(t));
    return it == jobs_.end() ? nullptr : it->second.get();
}

std::shared_ptr<const assets::TextureAsset> PlanetPortraits::texture(std::string_view name) {
    const std::string_view t = is_planet_portrait_name(name) ? planet_portrait_type(name) : name;
    if (t.empty()) return nullptr;
    auto [it, inserted] = jobs_.try_emplace(upper_of(t));
    if (inserted) {
        it->second = std::make_unique<Job>();
        it->second->type = std::string(t);
        const objects::TypeIndex ti = types_->find(t);
        if (ti == objects::kNone || !types_->valid(ti) || (*types_)[ti].model.empty()) it->second->state = State::Failed;
    }
    return it->second->state == State::Ready ? it->second->tex : nullptr;
}

bool PlanetPortraits::failed(std::string_view name) const {
    const Job* j = find(name);
    return j && j->state == State::Failed;
}

bool PlanetPortraits::pending() const {
    for (const auto& [k, j] : jobs_)
        if (j->state == State::Queued || j->state == State::InFlight) return true;
    return false;
}

void PlanetPortraits::free_gpu(Job& j) {
    if (j.color) alloc_->destroy(j.color);
    if (j.depth) alloc_->destroy(j.depth);
    if (j.readback.handle) alloc_->destroy(j.readback);
}

void PlanetPortraits::collect(Job& j) {
    if (j.state != State::InFlight || ring_->completed_value() < j.frame) return;
    if (j.incomplete) {  // pipelines were still compiling: draw again (images kept)
        j.state = State::Queued;
        return;
    }
    alloc_->invalidate(j.readback.alloc);
    const auto* px = static_cast<const uint8_t*>(j.readback.mapped());
    auto tex = std::make_shared<assets::TextureAsset>();
    const uint32_t n = kPlanetPortraitSize;
    tex->name = planet_portrait_name(j.type);
    tex->format = assets::TextureFormat::RGBA8;
    tex->width = tex->height = n;
    tex->mip_levels = 1;
    tex->data.assign(px, px + static_cast<size_t>(n) * n * 4);
    const rhi::Format f = j.color.format;
    if (f == rhi::Format::BGRA8 || f == rhi::Format::BGRA8_SRGB)
        for (size_t i = 0; i < tex->data.size(); i += 4) std::swap(tex->data[i], tex->data[i + 2]);
    straight_alpha(tex->data);
    assets::TextureLevel l;
    l.width = l.height = n;
    l.size = tex->data.size();
    tex->levels = {l};
    j.tex = std::move(tex);
    j.state = State::Ready;
    free_gpu(j);
}

bool PlanetPortraits::render_job(Job& j, rhi::Frame& frame, VkCommandBuffer cmd) {
    const objects::TypeIndex ti = types_->find(j.type);
    const std::string& model = (*types_)[ti].model;
    const GpuModel* m = models_->model(model);
    if (!m || !m->resident()) {
        if (models_->model_failed(model) || ++j.waited > wait_frames_) {
            j.state = State::Failed;
            free_gpu(j);
        }
        return true;
    }
    const uint32_t n = kPlanetPortraitSize;
    if (!j.color) {
        rhi::ImageDesc cd;
        cd.format = rhi::from_vk(r_->color_format());
        cd.width = cd.height = n;
        cd.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        cd.name = "planet_portrait";
        rhi::ImageDesc dd = cd;
        dd.format = rhi::from_vk(r_->depth_format());
        dd.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        dd.name = "planet_portrait_depth";
        if (!alloc_->create_image(cd, j.color) || !alloc_->create_image(dd, j.depth) ||
            !alloc_->create_buffer(static_cast<VkDeviceSize>(n) * n * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::MemoryUsage::Readback,
                                   j.readback)) {
            j.state = State::Failed;
            free_gpu(j);
            return false;
        }
    }
    // Camera fitted to the drawn bounding sphere [port].
    // Radius = the largest half-extent of the drawn bounds (the circumscribed sphere of the box leaves a ringed
    // planet at half the frame).
    const assets::Aabb b = m->drawn_bounds();
    assets::Sphere s = m->drawn_sphere();
    if (b.valid())
        s.radius = std::max({b.max.x - b.min.x, b.max.y - b.min.y, b.max.z - b.min.z}) * 0.5f;
    const float rad = std::max(s.radius, 1e-3f);
    CameraParams cp;
    cp.target = s.center;
    cp.yaw = -1.5707963f;  // looking north (+Y)
    cp.pitch = kPortraitPitch;
    cp.fov_x = kGalacticFovX;
    cp.distance = rad / (kPortraitFill * std::tan(0.5f * cp.fov_x));
    cp.znear = std::max(0.01f, cp.distance - 2.0f * rad);
    cp.zfar = cp.distance + 2.0f * rad;
    const ViewMatrices vm = camera_view(cp, n, n);
    Instance in;
    in.model = m;
    in.transform = object_transform({0, 0, 0}, kIdentityFacing, 1.0f);
    r_->prepare_pipelines(*m);

    const rhi::DeviceFns& fn = alloc_->context().vkd();
    PassTarget pt;
    pt.color = j.color.handle;
    pt.color_view = j.color.attachment_view ? j.color.attachment_view : j.color.view;
    pt.depth = j.depth.handle;
    pt.depth_view = j.depth.attachment_view ? j.depth.attachment_view : j.depth.view;
    pt.extent = {n, n};
    pt.clear[0] = pt.clear[1] = pt.clear[2] = pt.clear[3] = 0.0f;
    pt.color_after = rhi::ImageState::TransferSrc;
    // Sun from the camera side, kLightYawOffset to the left and kLightElevation up (lit three-quarter view) [port].
    Lighting lit = lighting_;
    const float ly = cp.yaw + kLightYawOffset, le = kLightElevation;
    lit.sun.direction = {std::cos(ly) * std::cos(le), std::sin(ly) * std::cos(le), std::sin(le)};
    const Lighting saved = r_->lighting();
    r_->set_lighting(lit);
    begin_pass(fn, cmd, pt);
    DrawStats st;
    RecordOptions ro;
    ro.coverage_alpha = true;  // alpha = coverage; additive light becomes alpha in collect() [port]
    const bool ok = r_->draw(frame, cmd, vm, std::span<const Instance>(&in, 1), DrawMode::Lit, ro, &st);
    end_pass(fn, cmd, pt);
    r_->set_lighting(saved);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {n, n, 1};
    fn.vkCmdCopyImageToBuffer(cmd, j.color.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, j.readback.handle, 1, &copy);
    const VkBufferMemoryBarrier2 hb = rhi::buffer_barrier(j.readback.handle, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                                          VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    rhi::cmd_barrier(fn, cmd, {}, std::span<const VkBufferMemoryBarrier2>(&hb, 1));
    j.frame = frame.number();
    j.incomplete = !ok || st.pending_pipeline != 0 || st.translation_pending != 0 || st.draws == 0;
    j.state = State::InFlight;
    return ok;
}

void PlanetPortraits::record(rhi::Frame& frame, VkCommandBuffer cmd) {
    for (auto& [k, j] : jobs_) collect(*j);
    for (auto& [k, j] : jobs_)
        if (j->state == State::Queued) (void)render_job(*j, frame, cmd);
}

void PlanetPortraits::destroy() {
    for (auto& [k, j] : jobs_) free_gpu(*j);
    jobs_.clear();
}

}  // namespace alamo::render
