#include "alamo/ui/hud_model_draw.hpp"

#include "alamo/ui/hud_renderer.hpp"
#include "alamo/ui/hud_shell_draw.hpp"

namespace alamo::ui {

namespace {
std::string key_of(std::string_view n) {
    std::string k(n);
    for (char& c : k)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return k;
}
}  // namespace

void HudModelDrawer::set_loader(ShellSet::ModelLoader loader, render::TextureSource textures) {
    loader_ = std::move(loader);
    textures_ = std::move(textures);
}

bool HudModelDrawer::prepare(render::Renderer& r, std::span<const HudModelDraw> draws, std::string* error) {
    bool ok = true;
    for (const HudModelDraw& d : draws) {
        if (d.model.empty()) continue;
        const std::string k = key_of(d.model);
        if (models_.count(k)) continue;
        std::unique_ptr<render::GpuModel> gm;
        std::shared_ptr<const assets::ModelAsset> asset = loader_ ? loader_(d.model) : nullptr;
        std::string err = asset ? std::string() : std::string("not found");
        if (asset) {
            gm = std::make_unique<render::GpuModel>();
            if (!r.upload_model(asset, textures_, *gm, nullptr, &err)) gm.reset();
        }
        if (!gm) {
            ok = false;
            if (error) *error += d.model + ": " + err + "; ";
        }
        models_.emplace(k, std::move(gm));
    }
    return ok;
}

void HudModelDrawer::make_resident(render::Renderer& r) {
    for (auto& [k, m] : models_)
        if (m) r.make_resident(*m);
}

void HudModelDrawer::instances(std::span<const HudModelDraw> draws, std::vector<render::Instance>& out) const {
    for (const HudModelDraw& d : draws) {
        const auto it = models_.find(key_of(d.model));
        if (it == models_.end() || !it->second) continue;
        render::Instance in;
        in.model = it->second.get();
        in.transform.m[0][0] = in.transform.m[1][1] = in.transform.m[2][2] = d.scale;
        in.transform.m[0][3] = d.center.x;
        in.transform.m[1][3] = d.center.y;
        out.push_back(in);
    }
}

bool HudModelDrawer::draw(render::Renderer& r, rhi::Frame& frame, VkCommandBuffer cmd, const HudCanvas& canvas,
                          std::span<const HudModelDraw> draws, render::DrawStats* stats) {
    make_resident(r);
    scratch_.clear();
    instances(draws, scratch_);
    std::vector<render::Instance> ready;
    for (const render::Instance& in : scratch_)
        if (in.model->resident()) ready.push_back(in);
    if (ready.empty()) return true;
    const render::Lighting world = r.lighting();
    r.set_lighting(ShellDrawer::hud_lighting());
    const bool ok = r.draw(frame, cmd, hud_ortho_view(canvas, kHudCameraModels2), ready, render::DrawMode::Lit, stats);
    r.set_lighting(world);
    return ok;
}

void HudModelDrawer::destroy(render::Renderer& r) {
    for (auto& [k, m] : models_)
        if (m) r.destroy_model(*m);
    models_.clear();
}

}  // namespace alamo::ui
