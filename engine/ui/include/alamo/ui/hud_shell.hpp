// P2 stage 2: command-bar shell models (docs/re/UI.md §2.2). A shell is a 3D .alo drawn with the shell ortho
// camera (HudCanvas kHudCameraShells); every non-shell component sits on the bone of its own name.
//
// Findings from the real models (i_tactical_controls.alo 157 bones / 153 meshes, i_galactic_controls.alo,
// i_main_reinforce.alo, i_galactic_organize.alo, ...), which settle the stage-1 [port] assumption:
//   * model units = HUD canvas units, +X right, +Y up, +Z towards the viewer; the model origin is the screen
//     corner / edge the shell is anchored to (hud_components.hpp shell_anchor_offset): the tactical bar's origin
//     is the bottom-left corner of the 1024 x 768 base canvas, not its centre (stage 1 assumed the centre);
//   * every component bone carries a HIDDEN placeholder mesh (alDefault.fx quad) whose box is the component's
//     rectangle (s_select_NN 50 x 50, s_health_NN 49 x 8, radar 175 x 175 MeshAdditive, special_button_NN 24 x 25,
//     special_border_NN 54 x 119, tqueueNN 54 x 54) - used for hit boxes;
//   * the visible geometry is per faction: meshes named <name>_ALT<n> (Empire_Faceplate_ALT0, Rebel_Faceplate_ALT1,
//     Underworld_Faceplate_ALT2, b_Help_Droid_*_ALT<n>, reinforcement_window_ALT0..4) - [port] exactly the
//     meshes whose ALT index equals the shell's alternate index are drawn (0 Empire, 1 Rebel, 2 Underworld for
//     the faceplates; the reinforcement window's index = reinforcement slots shown).
// The camera eye is at z = (near + far) / 2 looking -Z, so the shells' z range (-2..5) is mid-depth.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/ui/hud_components.hpp"

namespace alamo::assets {
struct ModelAsset;
}

namespace alamo::ui {

struct ShellMesh {
    std::string name;
    uint32_t bone = 0;
    bool hidden = false;   // the ALO's own flag (placeholders)
    int32_t alt = -1;      // n of a "_ALT<n>" suffix, else -1
    Vec2f min{}, max{};    // file bounds (bone space, x / y)
};

struct ShellModelInfo {
    std::vector<ShellBone> bones;  // every bone except the root, bind pose model space, file order
    std::vector<ShellMesh> meshes; // ModelAsset::meshes order
};

// "_ALT<n>" suffix (case-insensitive) -> n, else -1.
int32_t shell_alt_index(std::string_view mesh_name);
// Bones (bind_world translations) + placeholder rectangles (the first mesh attached to each bone) + meshes.
ShellModelInfo shell_model_info(const assets::ModelAsset& model);
// Per ModelAsset::meshes entry, 1 = hidden: the ALO-hidden placeholders and every _ALT<n> mesh with n != alt.
std::vector<uint8_t> shell_mesh_hidden(const ShellModelInfo& info, int32_t alt);

// The shells of a ComponentTable, loaded through a model loader (e.g. assets::AssetCache::model). Feeds the
// HudLayout bones and keeps the model + per-faction mesh masks for the GPU side (alamo_ui_draw ShellDrawer).
class ShellSet {
public:
    using ModelLoader = std::function<std::shared_ptr<const assets::ModelAsset>(std::string_view model_name)>;
    struct Shell {
        uint32_t component = 0;
        std::string model_name;
        std::shared_ptr<const assets::ModelAsset> model;
        ShellModelInfo info;
        int32_t alt = 0;            // faction / variant index of the visible _ALT meshes
        bool visible = true;        // drawn (and its bones place components)
        std::vector<uint8_t> mesh_hidden;
    };
    // Loads every Shell component whose name is in `names` (empty = every Shell component with a Model_Name).
    // Returns the number of shells loaded; missing models are listed in `missing`.
    size_t load(const ComponentTable& table, const ModelLoader& loader, std::span<const std::string> names = {},
                std::vector<std::string>* missing = nullptr);
    // Pushes the bones into the layout (and the visibility as shell activity).
    void apply(HudLayout& layout) const;
    void set_alt(uint32_t component, int32_t alt);
    void set_visible(uint32_t component, bool visible);
    const Shell* find(uint32_t component) const;
    std::span<const Shell> shells() const { return shells_; }
    bool empty() const { return shells_.empty(); }

private:
    std::vector<Shell> shells_;  // ascending component index
};

}  // namespace alamo::ui
