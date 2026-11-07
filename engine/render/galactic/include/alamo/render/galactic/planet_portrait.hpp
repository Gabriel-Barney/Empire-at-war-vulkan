// alamo_render_galactic - planet portraits: a planet type's galactic model rendered once into an offscreen
// 512 x 512 RGBA image, served to the GUI as an image page by name ("planet_portrait:<type name>").
//
// Users: the Planet Info dialog's IDC_PLANET_ICON (GALACTIC_DIALOGS §9: "the planet's own model rendered into a
// 512 x 512 target (0xd6120), same camera / light set-up as the battle load screen [M for the exact camera]"; the ui
// draws load_overlay.tga over it) and the battle load screen's LoadScreenInfo::planet_icon.
//
// [port] The load-screen camera and lights are not documented (GALACTIC_DIALOGS §9 marks the camera [M]); the
// portrait uses a centred camera fitted to the model's drawn bounding sphere (GpuModel::drawn_sphere), looking north
// (+Y) 20 degrees down, horizontal FOV kGalacticFovX, the largest half-extent of the drawn bounds filling 90 % of the
// frame, and the galactic zoomed-in light record (galactic_lighting(settings, 1): colour, intensity, ambient) with
// the sun turned to the camera side (45 degrees round, 30 degrees up) so the portrait shows the day side. Background transparent (clear 0, 0, 0, 0; drawn with
// RecordOptions::coverage_alpha so the additive glow quad does not write alpha; its light becomes straight alpha on
// readback). Bind pose, no team colour
// override (the type's own planet materials, as the galaxy view draws an owned planet minus the owner tint).
//
// Use (frame thread):
//   PlanetPortraits portraits(renderer, alloc, ring, models, types);   // models: a GalacticModelSource that streams
//   portraits.set_lighting(galactic_lighting(settings, 1.0f));         // real ALOs (GalacticModels)
//   per frame, after models.update() / Uploader::record_acquires and OUTSIDE any render pass, into frame.cmd():
//     portraits.record(frame, cmd);
//   when the GUI asks for an image page whose name is_portrait_name():
//     auto tex = portraits.texture(name);   // null while pending (call again next frame); failed(name) -> give up
//     hud.set_image_page(page, *tex);
// Each portrait is rendered once per type (cached; the images are freed after readback, the CPU texture is kept).
// A portrait whose pipelines were still compiling (drawn unlit) is re-rendered on a later frame.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "alamo/assets/texture.hpp"
#include "alamo/render/galactic/galactic_view.hpp"
#include "alamo/render/renderer.hpp"
#include "alamo/rhi/frame.hpp"
#include "alamo/rhi/memory.hpp"

namespace alamo::render {

inline constexpr std::string_view kPlanetPortraitPrefix = "planet_portrait:";
inline constexpr uint32_t kPlanetPortraitSize = 512;

// "planet_portrait:<type>" (case-insensitive prefix) / the type name in it.
bool is_planet_portrait_name(std::string_view name);
std::string planet_portrait_name(std::string_view planet_type);
std::string_view planet_portrait_type(std::string_view name);  // "" when not a portrait name

class PlanetPortraits {
public:
    PlanetPortraits(Renderer& r, rhi::Allocator& alloc, rhi::FrameRing& ring, GalacticModelSource& models,
                    const GalacticTypeTable& types);
    ~PlanetPortraits();
    PlanetPortraits(const PlanetPortraits&) = delete;
    PlanetPortraits& operator=(const PlanetPortraits&) = delete;

    // Light record used for the portraits (default galactic_lighting(GalacticLightSettings{}, 1.0f)).
    void set_lighting(const Lighting& l) { lighting_ = l; }
    // The portrait texture (RGBA8, kPlanetPortraitSize square, 1 mip) of `name` (a portrait name or a bare type
    // name). Null while pending: the first call queues it. Unknown types / types without a galactic model / models
    // that fail to load: failed(name) becomes true and the result stays null.
    std::shared_ptr<const assets::TextureAsset> texture(std::string_view name);
    bool failed(std::string_view name) const;
    // Frame thread, outside a render pass: collects finished readbacks and records the queued portraits whose model
    // is resident (offscreen pass + copy to a readback buffer) into `cmd` of `frame`.
    void record(rhi::Frame& frame, VkCommandBuffer cmd);
    void destroy();  // GPU idle

private:
    enum class State : uint8_t { Queued, InFlight, Ready, Failed };
    struct Job {
        std::string type;
        State state = State::Queued;
        uint32_t waited = 0;
        uint64_t frame = 0;      // timeline value of the frame that rendered it
        bool incomplete = false; // drawn while pipelines / translations were pending: render again
        rhi::Image color, depth;
        rhi::Buffer readback;
        std::shared_ptr<const assets::TextureAsset> tex;
    };
    const Job* find(std::string_view name) const;
    bool render_job(Job& j, rhi::Frame& frame, VkCommandBuffer cmd);
    void collect(Job& j);
    void free_gpu(Job& j);

    static constexpr uint32_t kModelWaitFrames = 1200;  // frames a queued portrait waits for its model before it is marked failed
    Renderer* r_;
    rhi::Allocator* alloc_;
    rhi::FrameRing* ring_;
    GalacticModelSource* models_;
    const GalacticTypeTable* types_;
    Lighting lighting_;
    std::map<std::string, std::unique_ptr<Job>> jobs_;  // upper-case type name
};

}  // namespace alamo::render
