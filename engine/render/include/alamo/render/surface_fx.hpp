// alamo_render - R5 runtime terrain decorations: SurfaceFX.xml / TerrainDecalFX.xml / DynamicTrackFX.xml data, the
// terrain surface type of a point, and the runtime decal list (docs/re/DECORATIONS.md §4.2-§4.4, §5.2; SPEC.md §14
// surface_type; TERRAIN_WATER_POST.md §1.7). Render-side only: nothing here is read by the sim; random choices draw
// from this module's own generator, never the sim RNG.
//
// SurfaceFX.xml: <SurfaceFX name="TRIGGER"> (name = a SurfaceFXTriggerType, e.g. Generic_Track, EXPLOSION_SCORCH_MARK_
// SMALL) with <Surface_Settings name="surface"> children holding Dynamic_Track_Name, Dynamic_Track_Left/Right_Bone,
// Decal_Name, Particle_Name, Particle_Min_Energy, SoundFX_Name (and terrain modifiers the renderer ignores). "NONE" (any
// case) means nothing. A settings block for the unit's surface is used, else "default". [port] Shipped files nest some
// <Surface_Settings name="water"> blocks inside "default" (unclosed tags); they are read as siblings.
// Object types name their triggers with SurfaceFX_Name (list) and, for PARTICLE types, Explosion_Scorch_Mark.
//
// Surface type of a terrain point: the material of the nearest grid vertex (TEM `14`); points under the map's water
// plane report 4 water / 8 lava / 7 ice by water type. Names (SurfaceFX settings): 0 default, 1 grass, 2 dirt, 3 snow,
// 4 water, 5 concrete, 6 mud, 7 ice, 8 lava; others -> default. [port] names 1..8 inferred from SPEC §14 / texture
// names; 9..11 (special cover types) are not mapped.
//
// Runtime decals (TerrainDecalFX.xml, §4.2): spawn = Intensity, Scale (w, h) exactly (Random_Size_Offset is parsed but
// has no effect: the 2026-10-31 capture drew every decal at its Scale), a random (column, row) pair from UV_Slot,
// Slot_Scale, Fadeout_Time, Permanent, Render_Mode, slot count 16 (Master_Decal_Slot_Count absent in vanilla); rotation = the spawning heading, or uniform in
// [-2pi, 2pi] with Random_Rotate; Z_Angle and Texture_Name have no effect (the atlas D_master_decal.tga is always bound).
// Rejected when |terrain height - spawn z| > 0.53 max(w, h), or when at least two live decals with current alpha >= 0.3
// already cover the point. Fade = intensity (L - age) / L for L = Fadeout_Time > 0; dead past L. [port] draw order of the
// random values: slot, rotation; a cap of kMaxRuntimeDecals live decals (oldest non-permanent dropped first, then
// the oldest) - the original's cap and removal are not traced (DECORATIONS §8 item 5).
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/terrain/decorations.hpp"
#include "alamo/terrain/ted.hpp"

namespace alamo::render {

// ------------------------------------------------------------------------------------------- SurfaceFX
struct SurfaceSettings {
    std::string surface;          // name attribute, lower case
    std::string dynamic_track;    // Dynamic_Track_Name ("" = none)
    std::string left_bone, right_bone;  // Dynamic_Track_Left_Bone / Right_Bone
    std::string decal;            // Decal_Name ("" = none)
    std::string particle;         // Particle_Name ("" = none)
    float particle_min_energy = 0.0f;
    std::string sound;            // SoundFX_Name (not played by the renderer)
};
struct SurfaceFxDef {
    std::string name;             // as written
    std::vector<SurfaceSettings> settings;  // file order (nested blocks flattened)
    // Settings for a surface name: exact (case-insensitive), else "default", else null.
    const SurfaceSettings* find(std::string_view surface) const;
};
// Appends every <SurfaceFX>. False without a root element.
bool parse_surface_fx(std::span<const uint8_t> xml, std::vector<SurfaceFxDef>& inout, std::string* error = nullptr);

// ------------------------------------------------------------------------------------------- TerrainDecalFX
struct DecalFxDef {
    std::string name;
    terrain::RenderMode mode = terrain::RenderMode::Modulate;  // Render_Mode (ctor default MODULATE)
    float intensity = 1.0f;
    float scale_w = 1.0f, scale_h = 1.0f;                       // Scale "w, h"
    std::vector<std::array<uint32_t, 2>> uv_slots;              // UV_Slot pairs (column, row) [M]
    uint32_t slot_scale = 1;
    float fadeout_time = 0.0f;
    bool permanent = false, random_rotate = false;
    float random_size_offset = 0.0f;  // parsed only: the captured original applies no size jitter (factor 1)
};
bool parse_terrain_decal_fx(std::span<const uint8_t> xml, std::vector<DecalFxDef>& inout, std::string* error = nullptr);

// "none" / "NONE" / empty -> true.
bool is_none_name(std::string_view s);

// All three databases of an install (Data\XML\SurfaceFX.xml, TerrainDecalFX.xml, DynamicTrackFX.xml).
class SurfaceFxLibrary {
public:
    // Missing files leave their list empty. Returns true when at least one file was read.
    bool load(const FileProvider& files);
    std::vector<SurfaceFxDef>& surface_fx() { return surface_fx_; }
    std::vector<DecalFxDef>& decals() { return decals_; }
    std::vector<terrain::DynamicTrackParams>& tracks() { return tracks_; }
    const std::vector<SurfaceFxDef>& surface_fx() const { return surface_fx_; }
    const std::vector<DecalFxDef>& decals() const { return decals_; }
    const std::vector<terrain::DynamicTrackParams>& tracks() const { return tracks_; }
    // Case-insensitive lookups (first entry of a name); null for unknown / none names.
    const SurfaceFxDef* find_surface_fx(std::string_view name) const;
    const DecalFxDef* find_decal(std::string_view name) const;
    const terrain::DynamicTrackParams* find_track(std::string_view name) const;
    // Distinct dynamic-track textures (WorldDecorations::extra_textures).
    std::vector<std::string> track_textures() const;

private:
    std::vector<SurfaceFxDef> surface_fx_;
    std::vector<DecalFxDef> decals_;
    std::vector<terrain::DynamicTrackParams> tracks_;
};

// ------------------------------------------------------------------------------------------- surface type
uint32_t terrain_surface_type(const terrain::Terrain& terrain, float x, float y);
std::string_view surface_type_name(uint32_t surface_type);

// ------------------------------------------------------------------------------------------- runtime decals
// Render-side generator (SplitMix64): deterministic for a seed, independent of the sim.
class RenderRng {
public:
    explicit RenderRng(uint64_t seed = 0x52355235ull) : s_(seed) {}
    uint64_t next();
    float unit();                               // [0, 1)
    float range(float lo, float hi);            // lo + unit() (hi - lo)
    uint32_t below(uint32_t n);                 // [0, n) (0 for n == 0)

private:
    uint64_t s_;
};

enum class DecalSpawn : uint8_t { Spawned, NoTerrain, HeightMismatch, Covered, NoGeometry };

struct RuntimeDecalStats {
    uint64_t spawned = 0, rejected_height = 0, rejected_covered = 0, no_geometry = 0, expired = 0, evicted = 0;
    uint32_t live = 0;
};

inline constexpr size_t kMaxRuntimeDecals = 256;

class RuntimeDecals {
public:
    explicit RuntimeDecals(uint64_t seed = 0x52355235ull) : rng_(seed) {}
    // §4.2 spawn at (x, y, z) with the spawning heading (radians about Z). `now` = seconds on the decal clock.
    DecalSpawn spawn(const terrain::Terrain& terrain, const DecalFxDef& fx, float x, float y, float z, float heading, float now);
    // Fades every decal to `now`, drops dead ones (§4.4).
    void update(float now);
    // Current geometry (colours faded at the last update), oldest first.
    std::vector<terrain::DecalGeometry> geometry() const;
    size_t size() const { return live_.size(); }
    void clear() { live_.clear(); }
    // Current alpha of decal i (oldest first).
    float alpha(size_t i) const { return i < live_.size() ? live_[i].alpha : 0.0f; }
    const terrain::TerrainDecal& record(size_t i) const { return live_[i].record; }
    const RuntimeDecalStats& stats() const { return stats_; }
    size_t max_decals = kMaxRuntimeDecals;

private:
    struct Live {
        terrain::TerrainDecal record;   // fade_time / intensity / mode; position / size / rotation
        terrain::DecalGeometry geometry;
        terrain::DecorationFrame frame;
        float birth = 0.0f, alpha = 1.0f;
    };
    RenderRng rng_;
    std::vector<Live> live_;
    RuntimeDecalStats stats_;
};

}  // namespace alamo::render
