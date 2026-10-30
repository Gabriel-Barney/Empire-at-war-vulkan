// alamo_ui_draw - HUD 3D models through an ortho camera (P2, docs/re/UI.md §1.7: "event models are drawn as 3D models
// through an ortho camera"): the radar event .alo models (RadarMap.xml Event_Model_Name, TacticalHud::radar_models()).
// Like ShellDrawer, everything goes through render::Renderer (upload_model / draw) - no engine/render change.
//
// Use: set_loader(model loader, texture source) once; per frame prepare(renderer, draws) BEFORE the HUD pass (uploads
// models seen for the first time; a model that fails to load is remembered and skipped) and, in the HudRenderer
// step callback at HudStepKind::Models2, draw(renderer, frame, cmd, canvas, draws). Instance transform = translate(
// center) x uniform scale (HudModelDraw), HUD camera kHudCameraModels2, HUD lighting (ShellDrawer::hud_lighting).
// [port] the event models' animations are not played (bind pose); the age is passed for a later animation hook.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "alamo/render/renderer.hpp"
#include "alamo/ui/hud_shell.hpp"
#include "alamo/ui/tactical_hud.hpp"

namespace alamo::ui {

class HudModelDrawer {
public:
    HudModelDrawer() = default;
    HudModelDrawer(const HudModelDrawer&) = delete;
    HudModelDrawer& operator=(const HudModelDrawer&) = delete;

    void set_loader(ShellSet::ModelLoader loader, render::TextureSource textures);
    // Uploads the models of `draws` not uploaded yet. False when one failed (listed in `error`); it is not retried.
    bool prepare(render::Renderer& r, std::span<const HudModelDraw> draws, std::string* error = nullptr);
    // Makes the uploaded models resident (call outside the HUD pass, like ShellDrawer::make_resident; draw() also does).
    void make_resident(render::Renderer& r);
    // Records the draws whose model is resident into the open HUD pass (HudStepKind::Models2). Returns false on a
    // renderer error.
    bool draw(render::Renderer& r, rhi::Frame& frame, VkCommandBuffer cmd, const HudCanvas& canvas,
              std::span<const HudModelDraw> draws, render::DrawStats* stats = nullptr);
    void destroy(render::Renderer& r);
    size_t model_count() const { return models_.size(); }
    // Instances of `draws` with an uploaded model (tests).
    void instances(std::span<const HudModelDraw> draws, std::vector<render::Instance>& out) const;

private:
    ShellSet::ModelLoader loader_;
    render::TextureSource textures_;
    std::map<std::string, std::unique_ptr<render::GpuModel>> models_;  // by upper-case name; null = failed
    std::vector<render::Instance> scratch_;
};

}  // namespace alamo::ui
