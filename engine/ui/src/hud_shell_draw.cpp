#include "alamo/ui/hud_shell_draw.hpp"

#include "alamo/rhi/upload.hpp"
#include "alamo/ui/hud_renderer.hpp"

namespace alamo::ui {

render::Lighting ShellDrawer::hud_lighting() {
    render::Lighting l = render::Lighting::engine_default();
    l.sun.direction = {0.0f, 0.0f, 1.0f};
    l.sun.color = {1.0f, 1.0f, 1.0f};
    l.sun.specular = {0.0f, 0.0f, 0.0f};
    l.sun.intensity = 1.0f;
    for (render::DirLight& f : l.fill) f.intensity = 0.0f;
    l.ambient = {0.35f, 0.35f, 0.35f};
    l.fog_enabled = false;
    l.space = true;
    l.environment = "hud";
    return l;
}

bool ShellDrawer::upload(render::Renderer& r, const ShellSet& set, const render::TextureSource& textures, std::string* error) {
    destroy(r);
    bool ok = true;
    for (const ShellSet::Shell& s : set.shells()) {
        Entry e;
        e.component = s.component;
        e.model = std::make_unique<render::GpuModel>();
        std::string err;
        if (!r.upload_model(s.model, textures, *e.model, nullptr, &err)) {
            ok = false;
            if (error) *error += s.model_name + ": " + err + "; ";
            continue;
        }
        models_.push_back(std::move(e));
    }
    return ok;
}

void ShellDrawer::make_resident(render::Renderer& r) {
    for (Entry& e : models_) r.make_resident(*e.model);
}

bool ShellDrawer::resident() const {
    for (const Entry& e : models_)
        if (!e.model->resident()) return false;
    return true;
}

void ShellDrawer::wait_uploads(const rhi::Uploader& up) const {
    for (const Entry& e : models_) {
        for (const rhi::UploadTicket& t : e.model->buffer_tickets)
            if (t) up.wait(t);
        for (const render::GpuTexture& t : e.model->textures)
            if (t.ticket) up.wait(t.ticket);
    }
}

void ShellDrawer::instances(const ShellSet& set, const HudLayout& layout, std::vector<render::Instance>& out) const {
    for (const Entry& e : models_) {
        const ShellSet::Shell* s = set.find(e.component);
        if (!s || !s->visible) continue;
        const Vec2f off = layout.shell_offset(e.component);
        render::Instance in;
        in.model = e.model.get();
        in.transform.m[0][3] = off.x;
        in.transform.m[1][3] = off.y;
        in.mesh_hidden = s->mesh_hidden;
        // The renderer's R4 alternate test (GpuSubmesh::alt != Instance::alt -> skipped) runs on top of mesh_hidden:
        // with the default Instance::alt 0 every _ALT1 / _ALT2 mesh (Rebel / Underworld faceplate and help droid,
        // reinforcement_window_ALT<n>) was dropped even though the faction mask showed it (playtest: no galactic
        // faceplate, trade routes through the radar box). The shell's faction alternate is the instance's.
        in.alt = static_cast<int>(s->alt);
        out.push_back(in);
    }
}

bool ShellDrawer::draw(render::Renderer& r, rhi::Frame& frame, VkCommandBuffer cmd, const HudCanvas& canvas,
                       const ShellSet& set, const HudLayout& layout, uint32_t half, render::DrawStats* stats) {
    if (half != 0) return true;
    make_resident(r);  // after this frame's record_acquires (the HUD pass is recorded after the world)
    scratch_.clear();
    instances(set, layout, scratch_);
    if (scratch_.empty()) return true;
    const render::Lighting world = r.lighting();
    r.set_lighting(hud_lighting());
    const bool ok = r.draw(frame, cmd, hud_ortho_view(canvas, kHudCameraShells), scratch_, render::DrawMode::Lit, stats);
    r.set_lighting(world);
    return ok;
}

void ShellDrawer::destroy(render::Renderer& r) {
    for (Entry& e : models_) r.destroy_model(*e.model);
    models_.clear();
}

}  // namespace alamo::ui
