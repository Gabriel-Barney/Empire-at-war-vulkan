// alamo_terrain - terrain decorations, CPU side (docs/re/DECORATIONS.md): decals, shadow blobs, map
// spline tracks, dynamic unit tracks. Fog of war is in fow.hpp. No Vulkan: everything here is plain
// vertex/index arrays plus the draw parameters a renderer needs.
//
// Decoration types (one alD3dTerrainDecorationRenderTask each, DECORATIONS.md 0):
//   0 shadow blobs   1 decals   2 dynamic tracks   3 map tracks on land   4 fog of war
//   5 map tracks on the water plane.
//
// ---------------------------------------------------------------- what the renderer must do
// Common (types 0-4): draw in bucket 0 right after the terrain patches, in the order 3, 2, 1, 0, 4,
// normal passes only. Depth test LESS_OR_EQUAL, no depth write for the blended modes. Projection
// for these draws = the scene camera with near + 0.3 and far + 0.3 (kDecorationDepthOffset,
// bit-faithful) or a small constant depth bias (visually equivalent). World matrix = identity:
// every vertex here is in world space.
//   * Type 3 (TrackGeometry, !on_water): lit LitVertex triangle list, texture `texture` with
//     U/V addressing from clamp_u/clamp_v (false = REPEAT), texture matrix = translation by
//     track_uv_offset(track, t) (t = seconds since the map was attached), render mode
//     DIFFUSE_ALPHA (Engine\PrimDiffuseAlpha: SRC_ALPHA/ONE_MINUS_SRC_ALPHA, fixed-function
//     lighting with vertex colour as the diffuse material, texture x lit colour x 2).
//   * Type 2 (DynamicTrackGenerator::build): lit LitVertex triangle list, texture
//     DynamicTrackParams::texture REPEAT/REPEAT, the params' render mode (DIFFUSE_ALPHA or
//     DECAL_BUMP_ALPHA), culled by the mesh bounds.
//   * Type 1 (DecalGeometry): bind kMasterDecalTexture (REPEAT/REPEAT) once, then one lit draw per
//     render mode 0..13 ascending containing the decals of that mode (DECAL_BUMP_ALPHA decals only
//     when the bump effect exists). Refresh vertex colours each frame with set_decal_alpha(
//     decal_fade(...).alpha); drop the decal when decal_fade(...).dead.
//   * Type 0 (BlobGeometry): pre-lit PrelitVertex triangle list, grouped per material: bind the
//     material texture (U CLAMP, V CLAMP), draw with the material's render mode; materials with a
//     custom shader (ShadowBlobMaterial::shader, BlobStencilMasked.fx) use that effect instead.
//   * Type 4: see fow.hpp.
// Type 5 (TrackGeometry with on_water()): as type 3 but in bucket 7 right after the water, with the
//   unmodified camera (no 0.3 offset), and not fogged.
//
// Render mode blend states (DECORATIONS.md 1.1): OPAQUE none; ADDITIVE ONE/ONE (colour x texture);
// ALPHA SRC_ALPHA/ONE_MINUS_SRC_ALPHA; MODULATE ZERO/SRC_COLOR with fragment = saturate(texture +
// vertex colour); DIFFUSE_ALPHA as above; DECAL_BUMP_ALPHA DST_COLOR/SRC_COLOR (2 x src x dst,
// 0.5 neutral) with the vertex RGB holding a packed tangent.
//
// Determinism: all builders are serial, use only the inputs given, and produce identical output
// on every machine for identical input (no hashing, no pointer-keyed containers). Nothing here is
// read by the sim today; dynamic-track and decal state could be, so keep it that way.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/assets/math.hpp"
#include "alamo/terrain/mesh.hpp"
#include "alamo/terrain/ted.hpp"

namespace alamo::terrain {

// ---------------------------------------------------------------- common
enum class DecorationType : uint32_t {
    ShadowBlob = 0, Decal = 1, DynamicTrack = 2, LandTrack = 3, FogOfWar = 4, WaterTrack = 5,
};

// Engine prim modes (DECORATIONS.md 1.1). Values are the file / XML values.
enum class RenderMode : uint32_t {
    Opaque = 0, Additive = 1, Alpha = 2, Modulate = 3, DepthSpriteAdditive = 4, DepthSpriteAlpha = 5,
    DepthSpriteModulate = 6, DiffuseAlpha = 7, StencilDarken = 8, FinalBlur = 9, Heat = 10,
    ParticleBumpAlpha = 11, DecalBumpAlpha = 12, AlphaScanlines = 13,
};
constexpr uint32_t kRenderModeCount = 14;
// "OPAQUE", "ADDITIVE", ... (ASCII case-insensitive [M], surrounding whitespace ignored); any unknown
// string -> Opaque, as the engine does.
RenderMode render_mode_from_string(std::string_view s);
// Out-of-range values (a corrupt map) -> Opaque.
RenderMode render_mode_from_u32(uint32_t v);
// Name as used in the XML ("" for the modes without a string: 9, 10, 11, 13).
std::string_view render_mode_name(RenderMode m);

// Lit vertex (36 bytes, FVF XYZ|NORMAL|DIFFUSE|TEX1): same layout as TerrainVertex, see mesh.hpp
// for the Vulkan attribute formats (colour bytes B, G, R, A = VK_FORMAT_B8G8R8A8_UNORM).
using LitVertex = TerrainVertex;
// Pre-lit vertex (24 bytes, FVF XYZ|DIFFUSE|TEX1), used by shadow blobs.
//   position offset 0 R32G32B32_SFLOAT, color offset 12 B8G8R8A8_UNORM, texcoord offset 16 R32G32_SFLOAT
struct PrelitVertex {
    float position[3];
    uint8_t color[4];  // B, G, R, A
    float texcoord[2];
};
static_assert(sizeof(PrelitVertex) == 24);

constexpr float kDecorationDepthOffset = 0.3f;  // added to near and far for types 0-4
constexpr float kBlobLift = 0.1f;
constexpr float kDecalLift = 1.0f;
constexpr float kDynamicTrackLift = 0.1f;
constexpr float kWaterTrackLift = 1.0f;          // height mode 0: water height + 1
constexpr const char* kMasterDecalTexture = "D_master_decal.tga";

// Triangle list in world space (CCW seen from above, like the terrain).
struct DecorationMesh {
    std::vector<LitVertex> vertices;
    std::vector<uint32_t> indices;
    assets::Aabb bounds;                 // of the vertices (invalid/zero when empty)
    bool empty() const { return indices.empty(); }
    uint32_t triangle_count() const { return static_cast<uint32_t>(indices.size() / 3); }
};

// Blob/decal rectangle frame (DECORATIONS.md 3.2): X axis = R(rot) (width, 0), Y axis =
// R(rot) (0, -height), origin = centre - (X + Y)/2; local (lx, ly) in [0,1]^2 covers the rectangle.
struct DecorationFrame {
    float origin[2] = {0, 0}, x_axis[2] = {0, 0}, y_axis[2] = {0, 0};
    float inv_x[3] = {0, 0, 0}, inv_y[3] = {0, 0, 0};  // lx = inv_x . (x, y, 1), ly = inv_y . (x, y, 1)
    float corners[4][2] = {};                          // origin, +X, +X+Y, +Y
    float min_xy[2] = {0, 0}, max_xy[2] = {0, 0};      // AABB of the corners
    bool valid = false;                                // false for a zero-area rectangle
    static DecorationFrame make(float cx, float cy, float width, float height, float rotation);
    std::array<float, 2> local(float x, float y) const;
};

// Packs a unit vector component pair into tangent bytes: (t + 1) * 127.5 (DECORATIONS.md 4.3).
uint8_t pack_unit(float t);

// ---------------------------------------------------------------- decals (type 1)
struct DecalGeometry {
    DecorationMesh mesh;                 // colours written for alpha = intensity (age 0)
    RenderMode mode = RenderMode::Modulate;
    // Atlas UV rows applied to the lifted world position (x, y, z, 1) (DECORATIONS.md 4.3 step 2).
    std::array<float, 4> atlas_u{}, atlas_v{};
    uint8_t tangent_rgb[3] = {0x7F, 0x7F, 0x7F};  // packed decal X axis on the terrain plane (bump)
};
// Clips every terrain triangle under the rectangle to its four edges (fan-triangulated), lifts by
// kDecalLift, interpolates the smooth terrain normal, de-duplicates vertices by exact position.
// False (empty output) without terrain, for a degenerate rectangle or one entirely off the grid.
// Budget (hostile .ted input): a decal whose rectangle AABB spans more than kMaxDecalCells grid
// cells (counted before clipping to the grid, so a huge mostly-off-map decal is rejected too) or
// has non-finite corners is rejected with a warning on channel "terrain". Each cell yields at most
// 2 triangles x a 7-gon (7 vertices, 15 indices), hence kMaxDecalVertices / kMaxDecalIndices.
// Caps vs the 65 vanilla decals (UM11, the only map with decals): see kMaxMapDecalVertices.
inline constexpr uint32_t kMaxDecalCells = 1024;  // e.g. 32 x 32 cells = 640 x 640 world units
inline constexpr uint32_t kMaxDecalVertices = 14u * kMaxDecalCells;
inline constexpr uint32_t kMaxDecalIndices = 30u * kMaxDecalCells;
bool build_decal_geometry(const Terrain& terrain, const TerrainDecal& decal, DecalGeometry& out);
// Every map decal (out[i] <-> terrain.decals[i]) under a per-map budget: a decal whose worst-case
// size (cells under its AABB x 14 vertices / x 30 indices) would push the running totals over
// kMaxMapDecalVertices / kMaxMapDecalIndices is skipped before it is built (empty mesh, logged),
// as are oversized decals. Returns the number of decals built. Renderers should use this instead
// of calling build_decal_geometry per decal. Caps vs the 65 vanilla decals (UM11: 64 x 15x15, one
// 19x19 mode 6): largest decal 4 cells / 14 vertices / 36 indices, map 402 vertices / 804 indices
// -> 256x (cells) and ~160x (map vertices) margin.
inline constexpr uint32_t kMaxMapDecalVertices = 65536;
inline constexpr uint32_t kMaxMapDecalIndices = 6u * kMaxMapDecalVertices;
size_t build_map_decal_geometry(const Terrain& terrain, std::vector<DecalGeometry>& out);
struct DecalFade {
    float alpha = 1.0f;
    bool dead = false;                   // age > fade time: no longer drawn, remove
};
// alpha = intensity * (L - age) / L for L = fade_time > 0, else intensity (DECORATIONS.md 4.4).
DecalFade decal_fade(const TerrainDecal& decal, float age_seconds);
// Vertex colour (B, G, R, A) for a mode, alpha and tangent (white decal colour, 4.4).
std::array<uint8_t, 4> decal_vertex_color(RenderMode mode, float alpha, const uint8_t (&tangent_rgb)[3]);
void set_decal_alpha(DecalGeometry& g, float alpha);

// ---------------------------------------------------------------- shadow blobs (type 0)
struct ShadowBlobMaterial {
    std::string name;
    std::string texture;                 // Texture_Name
    std::string texture2;                // Texture_Name_2 (custom effect only)
    std::string shader;                  // Shader_Name (custom effect; empty = prim mode draw)
    RenderMode mode = RenderMode::Modulate;  // Render_Mode (default MODULATE)
};
// The two hard-coded entries: 0 w_shadow_blob.tga MODULATE, 1 w_light_blob.tga ADDITIVE
// (DECORATIONS.md 3.1; their names are not established [L], "Default_Shadow"/"Default_Light").
std::vector<ShadowBlobMaterial> builtin_blob_materials();
// Appends ShadowBlobMaterials.xml <Material name=...> entries. False if the XML has no root or the
// module was built without alamo_data.
bool parse_shadow_blob_materials(std::span<const uint8_t> xml, std::vector<ShadowBlobMaterial>& inout,
                                 std::string* error = nullptr);
// Index of a material by name (case-insensitive), or 0 (the default shadow) if missing.
uint32_t find_blob_material(const std::vector<ShadowBlobMaterial>& materials, std::string_view name);

// Placement state the owner sets per frame (DECORATIONS.md 3.2).
struct ShadowBlob {
    float position[2] = {0, 0};
    float rotation = 0.0f;               // radians
    float width = 10.0f, height = 10.0f; // world units [L defaults]
    float intensity = 1.0f;              // alpha
    float color[4] = {1, 1, 1, 1};       // RGBA
    uint32_t material = 0;               // index into the material table
};
struct BlobGeometry {
    std::vector<PrelitVertex> vertices;  // one per terrain grid vertex in the AABB, +kBlobLift
    std::vector<uint32_t> indices;
    assets::Aabb bounds;
    float max_height = 0.0f;             // highest terrain height under the blob (water test)
};
// Geometry per DECORATIONS.md 3.3 (no clipping: the texture must be sampled with CLAMP).
bool build_blob_geometry(const Terrain& terrain, const ShadowBlob& blob, RenderMode mode, BlobGeometry& out);
// Colour rules of 3.3 (0x12ff60), returned as B, G, R, A.
std::array<uint8_t, 4> blob_vertex_color(RenderMode mode, const float (&rgba)[4], float alpha);

// ---------------------------------------------------------------- map tracks (types 3 / 5)
struct SplineSample {
    float position[2] = {0, 0};
    float tangent[2] = {1, 0};           // unit, horizontal
    float arc = 0.0f;                    // arc length from the first control point
    float width = 0.0f;                  // interpolated control-point width
};
// Samples the track centre line every sample_spacing units of arc length (first and last control
// point always included). Spline = uniform Catmull-Rom through the control points with the end
// points repeated [L: "piecewise cubic" in the doc, kind not established]; widths are linear in
// the segment parameter [L]. At most max_samples samples (spacing is widened to fit).
// Hostile input: a track with more than kMaxTrackPoints finite control points (checked before
// anything is allocated), or whose arc length or any sample is non-finite (e.g. coordinates near
// FLT_MAX overflowing the spline), yields no samples (logged on channel "terrain").
inline constexpr uint32_t kMaxTrackPoints = 4096;  // vanilla max 37 (509 tracks): ~110x margin
std::vector<SplineSample> sample_track_spline(const TerrainTrack& track, uint32_t max_samples = 16384);

struct TrackGeometry {
    DecorationMesh mesh;
    DecorationType type = DecorationType::LandTrack;  // WaterTrack when height mode 0
    RenderMode mode = RenderMode::DiffuseAlpha;
    std::string texture;
    bool clamp_u = false, clamp_v = false;            // false = REPEAT
    float u_speed = 0.0f, v_speed = 0.0f;
    uint32_t rows = 0, columns = 0;                   // samples along x profile entries across
    float length = 0.0f;                              // total arc length
};
// Ribbon per DECORATIONS.md 5.1: per sample a row of `profile.size()` vertices from +width/2 to
// -width/2 along the left perpendicular (-ty, tx) of the tangent [L side]; z per height mode
// (0 water + 1, 1 terrain, 2 terrain + offset) sampled at each vertex; normal = smooth terrain
// normal (water: up); u = arc / repeat_length (or arc / length when 0), v = k / (n - 1); colour
// white, alpha = profile[k] * opacity, x first/last-row multiplier on the first/last row; quads
// between rows with the diagonal alternating per column. Tracks without the ribbon flag (4),
// with < 2 points or < 2 profile entries produce no geometry (returns false).
// Budget (hostile .ted input): the profile is truncated to kMaxTrackProfile entries and the row
// count to kMaxTrackVertices / columns (the spline spacing is widened to fit, so the ribbon still
// spans the whole track, coarser); either clip logs a warning on channel "terrain". Caps vs the
// 509 vanilla tracks (24 maps): largest track 642 vertices / 2556 indices (214 rows, profile 3),
// largest map 3096 vertices / 9612 indices -> ~10-25x margin.
inline constexpr uint32_t kMaxTrackProfile = 32;
inline constexpr uint32_t kMaxTrackVertices = 16384;
inline constexpr uint32_t kMaxTrackIndices = 6u * kMaxTrackVertices;
bool build_track_geometry(const Terrain& terrain, const TerrainTrack& track, TrackGeometry& out);
// Every map track (out[i] <-> terrain.tracks[i]) under a per-map budget: a track whose ribbon
// would push the running totals over kMaxMapTrackVertices / kMaxMapTrackIndices is skipped (empty
// mesh, rows = columns = 0, logged); the check runs after sampling the spline and before the
// ribbon is allocated. Returns the number of ribbons built.
inline constexpr uint32_t kMaxMapTrackVertices = 65536;
inline constexpr uint32_t kMaxMapTrackIndices = 6u * kMaxMapTrackVertices;
size_t build_map_track_geometry(const Terrain& terrain, std::vector<TrackGeometry>& out);
// Texture-matrix translation (frac(t * u_speed), frac(t * v_speed)); t = seconds since attach.
std::array<float, 2> track_uv_offset(const TerrainTrack& track, float seconds);

// ---------------------------------------------------------------- dynamic tracks (type 2)
// One DynamicTrackFX.xml entry; defaults = the FX constructor (DECORATIONS.md 5.2).
struct DynamicTrackParams {
    std::string name;
    std::string texture;                 // texture_name
    float opacity = 1.0f;
    float width = 20.0f;                 // offset of each edge from the centre line
    float segment_length = 10.0f;        // point spacing and texture repeat
    RenderMode mode = RenderMode::Alpha; // Render_Mode
    float fade_begin_distance = 40.0f;
    float fade_end_distance = 80.0f;
    float fade_distance_per_second = 20.0f;
    float min_geometry_lod = 0.0f;       // not used by the track (detail gate only)
};
// Appends every <DynamicTrack name=...>. False if the XML has no root or no alamo_data.
bool parse_dynamic_track_fx(std::span<const uint8_t> xml, std::vector<DynamicTrackParams>& inout,
                            std::string* error = nullptr);
const DynamicTrackParams* find_dynamic_track(const std::vector<DynamicTrackParams>& all, std::string_view name);

// smoothstep(t) = (3 - 2t) t^2 on t clamped to [0, 1].
float smoothstep01(float t);

struct DynamicTrackBuildOptions {
    // Point colour RGB = direction of the following segment ((dx+1)*127.5, (dy+1)*127.5, 0x7F),
    // as read in the code for every mode (DECORATIONS.md 5.2). For DIFFUSE_ALPHA that tints the
    // track [L, capture pending]; false writes white RGB for non-bump modes instead.
    bool direction_in_color = true;
    // Long segments are split so no quad spans more than this along the track, to follow the
    // terrain ([L]: the original's subdivision rule is not established; one grid cell).
    float max_quad_length = kVertexSpacing;
};

// Per-unit trail generator. Feed the unit position each tick; build() when drawing.
class DynamicTrackGenerator {
public:
    struct Point {
        float position[2] = {0, 0};
        float distance = 0.0f;           // head distance when the point was dropped
        bool gap = false;                // drawn with alpha 0
    };
    static constexpr size_t kMaxPoints = 4096;

    explicit DynamicTrackGenerator(const DynamicTrackParams& params);
    // Moves the head to (x, y): head distance += travelled length + dt * fade_distance_per_second;
    // a point is dropped every segment_length along the move (the head is the last point and is
    // replaced while closer than segment_length to the previous one); tail points more than
    // fade_end_distance behind the head are removed. `gap` marks the head (and points dropped in
    // this update) as gaps [M]. A move that would drop more than kMaxPoints points restarts the
    // trail at (x, y) [L].
    void update(float x, float y, float dt, bool gap = false);
    // Time only (unit not reporting a position): the trail fades in place.
    void advance(float dt);
    void clear();

    const DynamicTrackParams& params() const { return params_; }
    const std::vector<Point>& points() const { return points_; }
    float head_distance() const { return head_distance_; }
    // opacity * (1 - smoothstep(begin, end, head - distance)); 0 for gaps.
    float point_alpha(size_t i) const;
    // Ribbon (DECORATIONS.md 5.2): edges at +-width (mitred at joints, miter length capped at
    // 4 x width [L]), z = terrain + kDynamicTrackLift, smooth terrain normals, u = 0 left / 1
    // right, v = 0.5 -+ (edge length / segment_length) / 2 per segment, alpha from the end points.
    // False (empty) with fewer than 2 points.
    bool build(const Terrain& terrain, DecorationMesh& out, const DynamicTrackBuildOptions& options = {}) const;

private:
    void trim_tail();
    DynamicTrackParams params_;
    std::vector<Point> points_;
    float head_distance_ = 0.0f;
};

// ---------------------------------------------------------------- shared helper
// Smooth terrain normal at a world position: barycentric over the mesh triangle containing the
// point (same triangulation as Terrain::sample_height) of the per-vertex terrain_normal().
std::array<float, 3> terrain_normal_at(const Terrain& terrain, float wx, float wy);

}  // namespace alamo::terrain
