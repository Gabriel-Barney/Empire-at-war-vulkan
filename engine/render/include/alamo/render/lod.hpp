// alamo_render - per-model LOD / ALT mesh selection (docs/re/RENDER.md 5, SPACE_AND_OPEN.md B.4):
//   mesh LOD index = digit after "_LOD" in the mesh name (assets MeshInfo::lod, -1 if absent);
//   detail d = min(LOD_Bias x coverage, (MeshDetail + 1) / 2) (lod_detail()), LOD_Bias = the
//   GameObjectType's value (default 4.0);
//   chosen = clamp(int((maxLOD + 1) * d), 0, maxLOD), higher index = more detail; meshes with
//   index -1 or the chosen index are shown. ALT index = digit after "_ALT" (-1 if absent); meshes
//   whose ALT differs from the model's current alt are hidden (-1 always shown).
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/assets/model.hpp"

namespace alamo::render {

int max_lod(std::span<const assets::MeshInfo> meshes);  // -1 when no mesh has a _LOD suffix
// detail in [0,1] (1 = most detailed, the Mesh Detail option or screen-size factor).
int choose_lod(int max_lod_index, float detail);
// SPACE_AND_OPEN.md B.4: d = min(lod_bias x coverage, (mesh_detail + 1) / 2).
constexpr float kDefaultLodBias = 4.0f;  // GameObjectType LOD_Bias default
inline float lod_detail(float lod_bias, float coverage, float mesh_detail) {
    const float a = lod_bias * coverage, b = 0.5f * (mesh_detail + 1.0f);
    return a < b ? a : b;
}
// Digit after "_ALT" (case-insensitive), -1 if absent.
int mesh_alt_index(std::string_view mesh_name);

struct MeshSelection {
    int lod = -1;  // chosen LOD (-1: the model has no LOD meshes)
    int alt = 0;
    std::vector<uint8_t> mesh_visible;  // per ModelAsset::meshes entry
    uint32_t damage_hidden = 0;         // meshes hidden by the damage rule
};
// Meshes hidden at model setup (SPACE_AND_OPEN.md B.9 [H]): for each hardpoint of the object's type the
// mesh whose name equals its Damage_Decal (case-insensitive, e.g. HP_F-L_BLAST; shown again when the
// hardpoint is destroyed), and every mesh whose name contains STEALTH, buildup or charged
// (case-insensitive substring). The type's hardpoints come from the XML (GameTypeResolver::render_info,
// hidden_mesh_mask(model, decals)). Without XML a name rule stands in (fallback_damage_decal_rule).
using DamageDecalRule = bool (*)(std::string_view mesh_name);
// STEALTH / buildup / charged substring rule (always applied).
bool is_state_mesh(std::string_view mesh_name);
// Fallback when no hardpoint XML is available: the name contains "_BLAST" (case-insensitive); every such
// mesh in the install is an HP* Damage_Decal (193 names).
bool fallback_damage_decal_rule(std::string_view mesh_name);
// Default rule without XML (RendererDesc::damage_decal_rule): fallback_damage_decal_rule || is_state_mesh.
bool default_hidden_mesh_rule(std::string_view mesh_name);

// Visible = LOD matches (or none) && ALT matches (or none) && !hidden && (show_damage || !rule(name)).
MeshSelection select_meshes(const assets::ModelAsset& model, float detail = 1.0f, int current_alt = 0,
                            bool show_damage = true, DamageDecalRule rule = default_hidden_mesh_rule);

// current_alt = kAnyAlt: every _ALT mesh passes the ALT test (R4: the renderer uploads all damage alternates and
// picks per instance with Instance::alt); MeshSelection::alt is then 0 (the default alternate).
inline constexpr int kAnyAlt = -128;
// Every LOD at once (the renderer uploads all LOD meshes and picks per instance and frame):
// mesh_visible ignores the _LOD index; lod = max_lod(). ALT / hidden / damage rules as above.
MeshSelection select_meshes_all_lods(const assets::ModelAsset& model, int current_alt = 0, bool show_damage = true,
                                     DamageDecalRule rule = default_hidden_mesh_rule);

// Per-mesh hidden masks (1 = hidden; per ModelAsset::meshes entry) for Instance::mesh_hidden.
// Both include the ALO hidden flag (MeshInfo::hidden).
// With the type's Damage_Decal names (from the hardpoint XML): those meshes + is_state_mesh.
std::vector<uint8_t> hidden_mesh_mask(const assets::ModelAsset& model, std::span<const std::string> damage_decals);
// Without XML: meshes matching `rule` (null: only the ALO-hidden ones).
std::vector<uint8_t> hidden_mesh_mask(const assets::ModelAsset& model, DamageDecalRule rule);
// Per-instance hardpoint toggle: destroyed shows the Damage_Decal mesh(es) named `damage_decal`, false hides
// them again. Returns the number of meshes changed. Per-instance state with the full destroy rule:
// hardpoints.hpp (HardpointVisibility).
uint32_t set_hardpoint_destroyed(std::vector<uint8_t>& mask, const assets::ModelAsset& model, std::string_view damage_decal,
                                 bool destroyed);
// RENDER.md 5 screen coverage of a world AABB: max squared NDC distance between the projected
// centre and the centre offset by each half-extent, x 3.5 pi, clamped to [0, 1]. view_proj rows
// as in camera.hpp (column vectors).
float screen_coverage(const float (&view_proj)[4][4], const assets::Aabb& world_box);

// The submeshes the unlit colour pass draws: render geometry (is_render_geometry()), not hidden,
// on a mesh that select_meshes() shows. Indices into ModelAsset::submeshes, file order.
// include_hidden: ALO-hidden (kSubmeshHidden) render geometry too, and with shadow_volumes the ALO-hidden
// shadow volumes (the renderer uploads both masked by GpuModel::mesh_hidden so a destroyed hardpoint can show
// them, B.10; the Shadow bucket follows the same per-instance mask).
std::vector<uint32_t> drawable_submeshes(const assets::ModelAsset& model, const MeshSelection& sel,
                                         bool shadow_volumes = false,  // + kSubmeshShadowVolume submeshes
                                         bool include_hidden = false);

}  // namespace alamo::render
