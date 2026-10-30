// alamo_ui_draw - selection circles as 3D scene primitives (docs/re/UI.md §1.3): the only world-space UI
// element that is not a projected 2D quad.
//   * Space (alBlob, flat): a unit quad ModelAsset in the XY plane with the ShadowBlobMaterials.xml material's
//     effect (Shader_Name, BlobStencilMasked.fx: render_phase Transparent) and textures Texture0 = Texture_Name
//     (i_selection_cutout_space.tga) / Texture1 = Texture_Name_2 (i_selection_<faction>.tga). Uploaded once per
//     material through Renderer::upload_model; each selected unit is one render::Instance (transform = scale
//     radius, translation = blob centre) appended to the frame's main instance list, so the renderer sorts it
//     into the Transparent bucket like any other transparent mesh.
//   * Land (alTerrainShadowBlob): terrain::ShadowBlob records (width = height = 2 radius) for
//     MapScene::set_shadow_blobs, which drapes them over the terrain grid (DECORATIONS.md 3.3).
// [port] land blobs go through the existing decoration path (blob materials with a custom shader draw in prim
// mode there: texture = Texture_Name), not through the BlobStencilMasked effect yet - see docs/PHASE4.md P2.
#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "alamo/assets/model.hpp"
#include "alamo/render/renderer.hpp"
#include "alamo/terrain/decorations.hpp"
#include "alamo/ui/hud_world.hpp"

namespace alamo::ui {

inline constexpr float kSpaceBlobLift = 0.5f;  // [port] z offset above the unit's plane (z-fighting guard)

// Unit quad (+-1 in X / Y, z = 0, normal +Z, uv (0,0) top-left) with `material`'s effect and textures; a material
// without Shader_Name gets the effect-less unlit stand-in with Texture_Name as BaseTexture.
std::shared_ptr<assets::ModelAsset> selection_blob_model(const terrain::ShadowBlobMaterial& material);
// Instance of an uploaded blob model for one blob (space).
render::Instance selection_blob_instance(const render::GpuModel& model, const SelectionBlob& blob);
// Land blobs -> terrain shadow blobs (material looked up by name, case-insensitive; unknown -> 0).
std::vector<terrain::ShadowBlob> land_selection_blobs(std::span<const SelectionBlob> blobs,
                                                      const std::vector<terrain::ShadowBlobMaterial>& materials);

}  // namespace alamo::ui
