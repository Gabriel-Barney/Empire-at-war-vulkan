// alamo_ui_draw - command-bar shell models on the GPU (P2 stage 2, docs/re/UI.md §1.1 step 6, §2.2): the .alo
// shells of a ShellSet (hud_shell.hpp) uploaded through render::Renderer and drawn at the HUD's ShellModels step
// with the shell ortho camera (hud_ortho_view(canvas, kHudCameraShells)) through Renderer::draw - no engine/render
// change. Each shell instance = translation by HudLayout::shell_offset (model origin -> its anchored canvas corner),
// mesh mask = ShellSet's per-faction _ALT selection + the hidden placeholders, Instance::alt = the shell's alt (the
// renderer's own _ALT test would otherwise keep only the _ALT0 meshes).
//
// [port] lighting: Renderer::draw lights with the renderer's current Lighting; for the HUD the drawer swaps in
// hud_lighting() (white key light along +Z = towards the viewer, ambient 0.35, no fog) for the duration of the
// draw and restores the world's. Frame step split [port]: every visible shell draws in half 0 (after layers 0-1),
// half 1 draws nothing (which shells the original puts in the second half is not documented).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "alamo/render/lighting.hpp"
#include "alamo/render/renderer.hpp"
#include "alamo/ui/hud_shell.hpp"

namespace alamo::ui {

class ShellDrawer {
public:
    ShellDrawer() = default;
    ShellDrawer(const ShellDrawer&) = delete;
    ShellDrawer& operator=(const ShellDrawer&) = delete;

    // Uploads every shell model of `set` (textures through `textures`). False if any upload failed (`error`).
    bool upload(render::Renderer& r, const ShellSet& set, const render::TextureSource& textures, std::string* error = nullptr);
    void make_resident(render::Renderer& r);
    bool resident() const;
    // Blocks until every buffer / texture upload of the shells finished (loading screens, tests): the next frame's
    // record_acquires makes them drawable.
    void wait_uploads(const rhi::Uploader& up) const;
    // Instances of the visible shells for the current layout (valid while `set` / this drawer are unchanged).
    void instances(const ShellSet& set, const HudLayout& layout, std::vector<render::Instance>& out) const;
    // Records the shells of `half` into the open HUD pass (call from the HudRenderer ShellModels step).
    bool draw(render::Renderer& r, rhi::Frame& frame, VkCommandBuffer cmd, const HudCanvas& canvas, const ShellSet& set,
              const HudLayout& layout, uint32_t half, render::DrawStats* stats = nullptr);
    void destroy(render::Renderer& r);
    size_t model_count() const { return models_.size(); }
    static render::Lighting hud_lighting();

private:
    struct Entry {
        uint32_t component = 0;
        std::unique_ptr<render::GpuModel> model;
    };
    std::vector<Entry> models_;
    std::vector<render::Instance> scratch_;
};

}  // namespace alamo::ui
