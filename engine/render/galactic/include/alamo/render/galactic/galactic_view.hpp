// alamo_render_galactic - R2 GalacticFrame -> render::Instance lists (galactic map scene). Render side only:
// reads a GalacticFrame (galactic_snapshot.hpp), never the sim.
//
// What is drawn (row / route / id order, so instance lists are deterministic):
//   background  In_Background rows (Galaxy_Core_Art_Model = w_galaxy00.alo) - the galactic background scene
//               (RENDER.md 1.2: background scenes with near 10 / far 100000, depth cleared after; draw with
//               Renderer::render's `sky` + RecordOptions::space). [port] no galactic star dome: the docs name none
//               (open item).
//   planets     Galactic_Model_Name (Destroyed_Galactic_Model_Name when destroyed) at Galactic_Position, uniform
//               Scale_Factor, owner colour as Colorization (B.3); spin = the model's <model>_IDLE_00 clip on the
//               galactic mode frame (SPACE_AND_OPEN.md D.7 4: Loop_Idle_Anim_00, Idle_Anim_00_Rate_Mod, frames on
//               30 Hz ticks; the galactic frame freezes while paused, Animate_During_Galactic_Mode_Pause = no).
//   starbases   the starbase object's Galactic_Model_Name (i_eb_station_level1.alo ...) next to its planet
//               [port: offset; the orbit geometry 0x569d50 is [M] in GALACTIC_ECONOMY.md 3.2].
//   ownership   [port] a flat ring round each owned planet in the owner colour (the original's marker is not
//               documented; open item).
//   routes      trade routes as flat additive ribbons between the planets (TradeRouteLines.xml "Default": Texture,
//               Width, Color_Zoomed_Out to Color_Zoomed_In with the zoom, also drawn in the close-up) [port: geometry].
//   fleets      Galactic_Fleet container model (w_planet_fleet.alo) or a member's Galactic_Fleet_Override_Model_Name,
//               owner colour, only while in transit (GALAXY_MAP_PRESENTATION §2.1: no fleet model at orbiting planets,
//               the HUD buttons stand for those): at the sim position, model +Y along the hop (the three Fleet_ALT0
//               darts taper to +Y [inferred]). One _ALT only (Instance::alt): ALT1 (gold scrolling dot + glow) when
//               kGalFleetStealthed, else ALT0 (dotted, Colorization = owner colour). [port] a thin line in the owner
//               colour from a moving fleet to its hop target (Fleet_Movement_Line_Texture_Name exists, its geometry is
//               not documented).
//   selection   GALAXY_MAP_PRESENTATION §3: the planet-selection model w_planet_select_neutral.alo (notched ring r 26.4,
//               dotted ring r 77.42, darken disc r 146; bones tilted, so the rings read as ellipses) at the selected
//               planet (GalacticSelection, set by the host every frame), axes X = screen right, Z = toward the
//               camera, IDLE_00 on the galactic frame, tinted with the owner colour through LIGHT_SCALE (its meshes are
//               MeshAdditive / MeshAlpha: colour = Color x LIGHT_SCALE.rgb x LIGHT_SCALE.w); a fogged planet gets
//               GalacticViewOptions::selection_unrevealed_color. Decorative, not a range ring.
// Procedural geometry (ribbons, rings, synthetic planets / fleets) is effect-less (unlit stand-in, vertex colour).
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "alamo/assets/skeleton.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/render/galactic/galactic_snapshot.hpp"
#include "alamo/render/lighting.hpp"
#include "alamo/render/renderer.hpp"

namespace alamo::assets {
class AssetCache;
}

namespace alamo::render {

// ---- procedural geometry ---------------------------------------------------------------------------------
enum class GalacticPrimitive : uint8_t {
    Ribbon = 0,  // x 0..1, y -0.5..0.5, z 0, two-sided (routes, fleet lines)
    Ring = 1,    // annulus r 0.86..1 in XY, two-sided (ownership)
    Sphere = 2,  // radius kPlanetSphereRadius, shaded (synthetic planets)
    Fleet = 3,   // arrow head along +X, ~3 units long (synthetic fleets)
    Station = 4, // box (synthetic starbases)
    Disc = 5,    // radius 1, bright centre fading out (synthetic galaxy core)
    Globe = 6,   // wireframe lattice (7 parallels, 12 meridians) of radius kPlanetSphereRadius (fogged planets)
    // Textured additive ribbon (FIXPASS_GALACTIC §3.2 path lines): Ribbon geometry with uv (u along, v across),
    // MeshAdditive.fx, BaseTexture = the primitive's texture (GalacticModelSource::line), Color = rgba. Without effects
    // (synthetic world) it falls back to the unlit stand-in in vertex colour rgba.
    Line = 7,
    Count
};
// SPACE_AND_OPEN.md D.7: every w_planet_* model shares the Hoth sphere of radius 4.92, so a planet's world
// radius is Scale_Factor x 4.92 (the galactic zoom computes exactly that).
inline constexpr float kPlanetSphereRadius = 4.92f;
inline constexpr uint32_t rgba8(uint32_t r, uint32_t g, uint32_t b, uint32_t a = 255) {
    return (r & 0xFFu) | ((g & 0xFFu) << 8) | ((b & 0xFFu) << 16) | ((a & 0xFFu) << 24);
}
uint32_t rgba8_of(const materials::float4& c);  // clamped, rounded
// Model asset of a primitive in colour `rgba` (rgba8 packing). Name = primitive_model_name(kind, rgba).
std::shared_ptr<assets::ModelAsset> galactic_primitive_model(GalacticPrimitive kind, uint32_t rgba);
// GalacticPrimitive::Line with its MeshAdditive.fx material on `texture`.
// Colour white (Color = 1,1,1): the tint comes per instance through LIGHT_SCALE (MeshAdditive: Color x LIGHT_SCALE.rgb x .w).
std::shared_ptr<assets::ModelAsset> galactic_line_model(const std::string& texture);
// "ALAMO_GAL_<KIND>_<RRGGBBAA>.ALO": a model name the synthetic world's XML uses so GalacticModels builds the
// primitive instead of loading a file. parse returns false for any other name.
std::string primitive_model_name(GalacticPrimitive kind, uint32_t rgba);
bool parse_primitive_model_name(std::string_view name, GalacticPrimitive& kind, uint32_t& rgba);

// ---- model source ------------------------------------------------------------------------------------------
class GalacticModelSource {
public:
    virtual ~GalacticModelSource() = default;
    // Model by .ALO name (null: empty, still loading or failed).
    virtual const GpuModel* model(const std::string& name) = 0;
    // A procedural model in `rgba`.
    virtual const GpuModel* primitive(GalacticPrimitive kind, uint32_t rgba) = 0;
    // GalacticPrimitive::Line with `texture`, tinted `rgba`. The caller ALSO sets Instance::light_scale = rgba / 255, so
    // a textured MeshAdditive implementation may ignore `rgba` and share one model per texture (GalacticModels does).
    // Default: the plain untextured primitive in vertex colour `rgba` (path-line colours are (int)(36 x f): <= 37 values).
    virtual const GpuModel* line(const std::string& texture, uint32_t rgba) {
        (void)texture;
        return primitive(GalacticPrimitive::Line, rgba);
    }
    // True once `name` (requested through model()) can never load (planet portraits give up on it).
    virtual bool model_failed(const std::string& name) {
        (void)name;
        return false;
    }
    // "<model stem>_IDLE_00" clip, null if the model has none.
    virtual std::shared_ptr<const assets::AnimationClip> idle_clip(const std::string& model) {
        (void)model;
        return nullptr;
    }
};

// Models for the app / GPU tests: ALOs through a ModelStreamer (one request per distinct name, case-insensitive,
// in first-ask order = frame order: deterministic), primitive names and primitive() uploaded directly. Frame
// thread only. `streamer`, `cache`, `files` may be null (synthetic world: primitives only).
class GalacticModels final : public GalacticModelSource {
public:
    GalacticModels(Renderer& r, ModelStreamer* streamer, assets::AssetCache* cache, const FileProvider* files)
        : renderer_(&r), streamer_(streamer), cache_(cache), files_(files) {}
    ~GalacticModels() override = default;
    const GpuModel* model(const std::string& name) override;
    const GpuModel* primitive(GalacticPrimitive kind, uint32_t rgba) override;
    std::shared_ptr<const assets::AnimationClip> idle_clip(const std::string& model) override;
    const GpuModel* line(const std::string& texture, uint32_t rgba) override;
    bool model_failed(const std::string& name) override;
    // Per frame after Uploader::record_acquires: residency of the primitives (+ streamer update).
    void update();
    void destroy();  // GPU idle
    uint32_t requested() const { return static_cast<uint32_t>(handles_.size() + prims_.size() + lines_.size()); }
    uint32_t ready() const;
    uint32_t failed() const;
    bool all_finished() const { return ready() + failed() == requested(); }

private:
    struct Prim {
        GpuModel model;
        bool ok = false;
    };
    Renderer* renderer_;
    ModelStreamer* streamer_;
    assets::AssetCache* cache_;
    const FileProvider* files_;
    std::unordered_map<std::string, uint32_t> by_name_;  // upper-cased name -> streamer handle (lookup only)
    std::vector<uint32_t> handles_;
    std::map<uint64_t, std::unique_ptr<Prim>> prims_;   // (kind << 32 | rgba) -> model (stable addresses)
    std::map<std::string, std::unique_ptr<Prim>> lines_;  // upper texture -> line model (tint per instance)
    const GpuModel* resident(Prim& p);
    std::map<std::string, std::shared_ptr<const assets::AnimationClip>> clips_;  // upper-cased model -> clip
};

// ---- trade route lines --------------------------------------------------------------------------------------
enum class RouteClass : uint8_t { Neutral = 0, Ally = 1, Enemy = 2, Corrupt = 3, Good = 4, Evil = 5 };
// TradeRouteLines.xml settings, parser layout of STRATEGIC_GAPS §6.1 (0x5c20b0 / 0x5c2a10) [H]: one 0x90-byte block per
// <Settings_For_Faction Name=...>, indexed ENEMY 0, ALLY 1, any other name 2 (neutral), GOOD 3, EVIL 4, CORRUPT 5.
enum RouteSettingsIndex : uint32_t { kRouteEnemy = 0, kRouteAlly = 1, kRouteNeutral = 2, kRouteGood = 3, kRouteEvil = 4, kRouteCorrupt = 5 };
struct RouteLineSettings {
    // Field defaults of a parsed block (the parser's): grey (0.5, 0.5, 0.5, 1) both zooms, width 0, ADDITIVE, flag 1,
    // W_galaxy_line_alpha.tga / spark w_galaxy_dot.tga.
    materials::float4 zoomed_out{0.5f, 0.5f, 0.5f, 1}, zoomed_in{0.5f, 0.5f, 0.5f, 1};
    float width = 0.0f;  // <Width> (unit not documented)
    std::string render_mode = "ADDITIVE", texture = "W_galaxy_line_alpha.tga", spark = "w_galaxy_dot.tga";
    bool flag = true;
};
struct RouteLineStyle {
    // Blocks the file does not define keep the vanilla FoC "Default" line (so a data-free world still draws routes).
    RouteLineStyle();
    RouteLineSettings settings[6];
    // Settings block of a route class (Neutral -> 2, Ally -> 1, Enemy -> 0, Good -> 3, Evil -> 4, Corrupt -> 5).
    static uint32_t index(RouteClass c);
    const RouteLineSettings& of(RouteClass c) const { return settings[index(c)]; }
    // FIXPASS_GALACTIC §3.1 (0x5c31a0): Color_Zoomed_Out x (1 - f) + Color_Zoomed_In x f, f = the camera zoom factor
    // (0 zoomed out, z while zooming in, 1 - z while zooming out, 1 zoomed in; clamped, non-finite -> 0). The rebuild's
    // swapped weights (0x5c2590 quirk: the other zoom colour after an ownership change until the zoom moves) are NOT
    // copied [port; capture C4].
    materials::float4 color(RouteClass c, float f = 0.0f) const;
    float width(RouteClass c) const { return of(c).width; }
};
// Reads Data\XML\TradeRouteLines.xml (the "Default" line, else the first; missing file / blocks keep the defaults).
RouteLineStyle load_route_line_style(const FileProvider& files);
// The settings index of a Settings_For_Faction name (case-insensitive; unknown names -> kRouteNeutral).
uint32_t route_settings_index(std::string_view name);
// FIXPASS_GALACTIC §3.1 slot rule 0x5c28e0 (per frame, so an ownership change recolours at once; no blend between
// slots): Neutral when either owner is none / Is_Neutral or the two owners differ; else (same owner) Ally (viewpoint
// player) / Enemy when `multiplayer` (global 0x140a7c475), else by the owner's faction from `sides`: 1 Good_Side_Name
// (REBEL) -> Good, 2 Evil_Side_Name (EMPIRE) -> Evil, 3 any other faction (UNDERWORLD) -> Corrupt. The Corrupt slot is the
// Underworld FACTION, not the corruption state. Side 0 / no entry -> Ally / Enemy.
// [port] either end kGalFogged -> Neutral (the original uses the last-seen owner PD +0x9C, not in the snapshot).
enum : uint8_t { kRouteSideNone = 0, kRouteSideGood = 1, kRouteSideEvil = 2, kRouteSideOther = 3 };
RouteClass classify_route(std::span<const GalacticPlanetSnap> planets, const GalacticRouteSnap& r, int32_t viewpoint,
                          std::span<const uint8_t> neutral_players, std::span<const uint8_t> sides = {}, bool multiplayer = false);

// FIXPASS_GALACTIC §3.2 corruption path lines (0x3f4b50; GameConstants, FoC values as defaults).
struct CorruptionPathSettings {
    std::string texture = "w_galaxy_line.tga";       // Fleet_Hyperspace_Band_Texture_Name
    float width = 16.0f;                              // Corruption_Path_Width
    uint8_t color[4] = {36, 36, 0, 255};              // Corruption_Path_Color
    Vec3 offset{0, 0, 0};                             // Corruption_Path_Offset
    float neighbour_distance = 100.0f;                // MaximumFleetMovementDistance (0x3ef2d0 A* neighbour)
};
CorruptionPathSettings load_corruption_path_settings(const FileProvider& files);
// One path line (galactic_corruption_paths): owned by planet row `owner`, towards row `other`; rgba = the tinted colour
// (channel = (int)(c x f), alpha 255).
struct CorruptionPathLine {
    uint32_t owner = 0, other = 0;
    float factor = 1.0f;
    uint8_t rgba[4] = {0, 0, 0, 255};
};
// The 0x3ff230 build rules recomputed per frame from the snapshot [port: the original keeps persistent per-planet
// lists PD +0x338 rebuilt at wrapper begin / completion / load]. Planet X takes part when it is a planet, not
// kGalFogged [port], and corrupted (corruption_type != -1, also during any removal) or gaining through a fading source
// (kGalCorruptionTransition + kGalCorruptionFading with corruption_type == -1; an ownership-change gain shows no line
// until it completes). Pairs (row order) of A* neighbours (a trade route, or distance <= neighbour_distance) that both
// take part get: one line owned by the lower row at factor 1 when neither runs a fading transition; else one line per
// endpoint that runs one, at its own factor [port: the idle side's own line is not drawn]. Factor (every planet service
// of the owner, 0x3fe1b0): fraction while fading-gaining, 1 - fraction while fading-removing, else 1. Lines whose
// tinted colour is black are dropped (additive: invisible).
void galactic_corruption_paths(const GalacticFrame& frame, const CorruptionPathSettings& s, std::vector<CorruptionPathLine>& out);

// ---- scene builder ------------------------------------------------------------------------------------------
struct GalacticViewOptions {
    int32_t viewpoint = 0;              // local player (route colours)
    std::vector<uint8_t> neutral;       // per player id: neutral faction (route rule)
    std::vector<uint8_t> side;          // per player id: kRouteSide* (classify_route)
    bool multiplayer = false;           // 0x140a7c475: same-owner routes Ally / Enemy instead of Good / Evil / Corrupt
    // World width = <Width> x this. 3 world units with W_galaxy_line_alpha.tga's 9/64 core match the 2-3 px line of
    // ui_005_gc_running.png (1 world unit is about 4.2 px there) [inferred].
    float route_width_scale = 1.0f;
    float corruption_width_scale = 0.25f;  // [port] world width = Corruption_Path_Width x this
    float fleet_line_width = 0.4f;      // [port]
    // [port] Route sparks (the block's spark texture, motion untraced): one bright dash per route moving a -> b at
    // spark_speed world units per galactic frame, spark_length long, <Width> x spark_width_scale wide.
    float spark_speed = 0.6f, spark_length = 2.0f, spark_width_scale = 0.5f;
    float ring_brightness = 0.55f;      // [port] ownership ring colour scale (thin faint ellipse in the original)
    // kGalFogged planets (19.webp, C2_galactic_silent.png): the model's grid_ALT2 mesh; no starbase, ring, orbiting fleets
    // or corruption smoke. fog_globe_color: [port] procedural globe of a model without that mesh (the W_Space_FOW_Grid blue).
    materials::float4 fog_globe_color{40 / 255.0f, 150 / 255.0f, 1.0f, 1.0f};
    // Planet-selection model (HUD +0x2248; the HUD loads only the neutral variant) and the tint of a planet the local
    // player has not revealed (0x50bf10 "neutral" colour [port: value not documented, grey]).
    std::string selection_model = "w_planet_select_neutral.alo";
    materials::float4 selection_unrevealed_color{0.55f, 0.55f, 0.55f, 1.0f};
    CorruptionPathSettings corruption_path;  // FIXPASS_GALACTIC §3.2 (host: load_corruption_path_settings)
    bool routes = true, rings = true, fleet_lines = true, starbases = true, route_sparks = true, selection = true;
    bool corruption_paths = true;
};

// Per-frame host input (GalacticSceneBuilder::set_selection): the selected planet and the camera the selection model
// faces. planet = the planet OBJECT id (GalacticPlanetSnap::id; kNoObjectId = nothing selected). eye / target / up =
// the frame's camera (ui::CameraParams eye, target, up); X = normalize((target - eye) x up), Z = normalize(eye - target).
struct GalacticSelection {
    objects::ObjectId planet = objects::kNoObjectId;
    Vec3 eye{0, 0, 1}, target{0, 0, 0}, up{0, 1, 0};  // default: looking straight down, screen up = +Y
};

struct GalacticViewStats {
    uint32_t planets = 0, background = 0, starbases = 0, rings = 0, routes = 0, fleets = 0, fleet_lines = 0, route_sparks = 0;
    uint32_t no_model = 0, animated = 0, fogged = 0, fogged_fleets = 0, hidden_routes = 0;
    uint32_t orbit_fleets = 0;     // in orbit, not drawn (§2.1); fogged_fleets are counted separately
    uint32_t stealth_fleets = 0;   // drawn with ALT1
    uint32_t selection = 0;        // selection model instances (0 or 1)
    uint32_t corruption_paths = 0; // corruption path line instances (§3.2)
};

class GalacticSceneBuilder {
public:
    explicit GalacticSceneBuilder(const GalacticTypeTable& types, RouteLineStyle style = {}, GalacticViewOptions options = {})
        : types_(&types), style_(style), opt_(std::move(options)) {}
    // Instances of `frame`: main list (planets, starbases, rings, routes, fleets, fleet lines) and the background
    // list. Instance::bone_world spans point into the builder and stay valid until the next build().
    void build(const GalacticFrame& frame, GalacticModelSource& models, std::vector<Instance>& main,
               std::vector<Instance>& background, GalacticViewStats* stats = nullptr);
    GalacticViewOptions& options() { return opt_; }
    const RouteLineStyle& style() const { return style_; }
    // Host input for the next build(): selected planet + camera (kept until changed).
    void set_selection(const GalacticSelection& s) { sel_ = s; }
    // Host input for the next build(): the camera zoom factor f of FIXPASS_GALACTIC §3.1 (route colour blend; sparkles
    // off at f == 1). Kept until changed; default 0 (zoomed out).
    void set_route_zoom(float f) { route_zoom_ = f; }
    // [port] (QA D6) galaxy-layer overlays (ownership rings, route sparkles, fleet lines, selection model) are world-space
    // and huge from the close-up camera: 1 at f == 0, 0 from f >= 0.25 on. Sparks and lines also narrow with it (their
    // primitives are opaque), the rest fades or is dropped at 0. Routes and corruption paths stay (FIXPASS_GALACTIC §3).
    static float overlay_fade(float zoom);
    const GalacticSelection& selection() const { return sel_; }

    // Geometry (tests / picking).
    float planet_radius(objects::TypeIndex t) const;  // Scale_Factor x 4.92
    // [port] orbit slot k: 8 slots on a ring of 1.9 planet radii, starting at -45 deg, raised 0.5 (Galactic_Fleet
    // Layer_Z_Adjust, used as a render offset only here).
    static Vec3 orbit_slot_offset(uint32_t slot, float planet_radius);
    static Vec3 orbit_slot_facing(uint32_t slot);
    static Vec3 starbase_offset(float planet_radius);  // [port] 1.5 radii towards -X,+Y
    Vec3 fleet_draw_position(const GalacticFleetSnap& f, std::span<const GalacticPlanetSnap> planets) const;
    std::string planet_model(const GalacticPlanetSnap& p) const;
    std::string fleet_model(const GalacticFleetSnap& f) const;
    // Model -> world of a ribbon from a to b of `width` (flat in the plane through a, b and +Z normal).
    static assets::Mat3x4 ribbon_transform(const Vec3& a, const Vec3& b, float width);
    static assets::Mat3x4 ring_transform(const Vec3& centre, float radius);
    // §2.1: drawn only in transit (moving, or no orbit planet); the _ALT index it shows.
    static bool fleet_in_transit(const GalacticFleetSnap& f, size_t planet_rows);
    static constexpr int kFogPlanetAlt = 2;  // grid_ALT2 of the planet models (ALT0 / ALT1 hold the planet itself)
    static int fleet_alt(const GalacticFleetSnap& f) { return (f.flags & kGalFleetStealthed) ? 1 : 0; }
    // §3: the selection model's transform at `centre`: X = screen right, Y = screen up, Z = toward the camera, scale 1.
    static assets::Mat3x4 selection_transform(const Vec3& centre, const GalacticSelection& s);

private:
    struct RowAnim {
        std::shared_ptr<const assets::AnimationClip> clip;
        const assets::ModelAsset* asset = nullptr;
        std::string model;
        std::vector<assets::Mat3x4> world;
    };
    std::span<const assets::Mat3x4> pose(RowAnim& a, const GpuModel* m, const std::string& model, const GalacticTypeInfo& ti,
                                         float frames, GalacticModelSource& models);
    const GalacticTypeTable* types_;
    RouteLineStyle style_;
    GalacticViewOptions opt_;
    std::vector<RowAnim> anims_;  // per planet row
    RowAnim sel_anim_;            // selection model IDLE_00
    GalacticSelection sel_;
    float route_zoom_ = 0.0f;
    std::vector<CorruptionPathLine> paths_;
};

// Underworld corruption smoke (STRATEGIC_GAPS §6.1 scenes [H data]): GameConstants Corruption_Particle_Name (FoC
// Heavy_Galactic_Corruption_Particles, p_planet_corrupt_large.alo) on corrupted planets in the midground scene and
// Corruption_Particle_Line_Name (FoC Galactic_Corruption_Line_Particles, p_planet_corrupt2.alo) on routes in the
// midforeground scene. Particle_Brightness_Per_Corruption_Level is kept for reference: the per-planet pairs come
// from the sim (GalacticPlanetSnap::corruption_pairs).
struct GalacticCorruptionSettings {
    std::string planet_particle = "Heavy_Galactic_Corruption_Particles";
    std::string line_particle = "Galactic_Corruption_Line_Particles";
    float brightness[2] = {0.3f, 1.0f};
};
GalacticCorruptionSettings load_galactic_corruption_settings(const FileProvider& files);
// One corruption effect to keep alive this frame (the host syncs it as a particle effect by `key`, from the Particle
// type `type`'s Galactic_Model_Name).
struct GalacticCorruptionEffect {
    uint64_t key = 0;        // planet: bit 58 | row; route: bit 57 | route index
    const std::string* type = nullptr;
    bool line = false;       // route effect (midforeground scene) vs planet (midground)
    assets::Mat3x4 transform{};
    // Planet effects with the particle object (kGalCorruptionParticles), GRC 4.5: the host snaps the system scalars
    // every frame (ProxyAttachments::set_effect_scalars): colour RGBA = brightness (setter index 4), emission scale =
    // scale (index 1), detail = energy (index 0). Each is start + (end - start) x fraction. scalars false: leave them.
    bool scalars = false;
    float brightness = 1.0f, scale = 1.0f, energy = 1.0f;
    int8_t corruption_type = -1;  // planet: effective type (0x4b7e90; the pending type during a transition)
};
inline uint64_t galactic_corruption_planet_key(uint32_t row) { return (1ull << 58) | row; }
inline uint64_t galactic_corruption_route_key(uint32_t route) { return (1ull << 57) | route; }
// GRC 4.2 helpers 0x4b7ed0 / 0x4b7f50 / 0x4b7f10: a + (b - a) x fraction.
inline float galactic_corruption_lerp(float start, float end, float fraction) { return start + (end - start) * fraction; }
// Planet effects (row order, planets that are not kGalFogged and have the particle object kGalCorruptionParticles, or
// without one are kGalCorrupted - the snapshot of a sim that builds no pairs), then route effects (route order, routes
// not kGalRouteHidden and classed Corrupt): planets at the centre, unrotated; [port] routes at the midpoint with local
// +X along the route, scale 1. GRC 4.6: a particle object exists while corrupted or gaining through a wrapper and keeps
// ramping down during a wrapper removal; an ownership-change gain has none until it completes (no smoke, no ramp).
void galactic_corruption_effects(const GalacticFrame& frame, const GalacticCorruptionSettings& s,
                                 std::vector<GalacticCorruptionEffect>& out);

// Galactic light record (STRATEGIC_GAPS §6.1, main scene +0x320, set by 0x4b1d60 at mode init) [H]: ambient (0.05, 0.05,
// 0.05); sun = light 0, colour (1, 1, 1), intensity 0.5, direction from Galactic_Zoom_Out_Light_Angle elements [1] = yaw,
// [2] = elevation (FoC 160, 58; element [0] unused) as light_direction(yaw, elevation) [M for sin / cos]; fill lights 1
// and 2 intensity 0. During a planet zoom (wall-clock ms / GMC_ZoomTime, 0..1) the intensity lerps to
// Galactic_Zoom_Light_Level and the angles to Galactic_Zoom_In_Light_Angle [1] / [2] (FoC 235, 18).
struct GalacticLightSettings {
    float out_yaw_deg = 160.0f, out_elevation_deg = 58.0f;  // Galactic_Zoom_Out_Light_Angle [1], [2]
    float in_yaw_deg = 235.0f, in_elevation_deg = 18.0f;    // Galactic_Zoom_In_Light_Angle [1], [2]
    float zoom_light_level = 0.5f;                          // Galactic_Zoom_Light_Level
    float intensity = 0.5f;                                 // the record's sun intensity
};
// GameConstants.xml values (missing / malformed tags keep the FoC defaults above).
GalacticLightSettings load_galactic_light_settings(const FileProvider& files);
// The record at planet-zoom fraction `zoom` (0 = zoomed out, 1 = zoomed in; clamped); black clear colour, no fog.
Lighting galactic_lighting(const GalacticLightSettings& s, float zoom = 0.0f);
Lighting galactic_lighting();  // FoC defaults, zoomed out

}  // namespace alamo::render
