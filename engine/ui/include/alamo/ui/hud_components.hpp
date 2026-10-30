// P2 command-bar data (docs/re/UI.md §2.1-2.2, §3.5; docs/formats SPEC.md §5 MTD, DAT):
//   * CommandBarComponents.xml -> ComponentTable (one entry per component, file order, lookup by name);
//   * the MT_CommandBar MTD atlas (texture name -> pixel rectangle in the atlas texture);
//   * MasterTextFile_<LANGUAGE>.DAT string tables (key CRC-32 lookup, UTF-16 values, English fallback);
//   * HudLayout: component placement on the canvas - component name = bone name in the parent shell .alo
//     (ShellBone lists come from the shell models), shells anchored to the screen edges per Model_Offset_X/Y.
//
// Shipped data note: the vanilla file stores components as <CommandBarComponent Name="..."> elements (the
// Name attribute is the component / bone name); a child whose tag is not CommandBarComponent is accepted
// with its tag as the name (UI.md §2.1 wording).
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/ui/hud_canvas.hpp"

namespace alamo {
class FileProvider;
}
namespace alamo::data {
class XmlNode;
}

namespace alamo::ui {

struct Rgba8 {
    uint8_t r = 255, g = 255, b = 255, a = 255;
    friend bool operator==(const Rgba8&, const Rgba8&) = default;
};

enum class ComponentType : uint8_t { Unknown = 0, Shell, Button, TextButton, Bar, Icon };
const char* component_type_name(ComponentType t);

// Number of HUD layers (UI.md §0 item 4) and the per-layer sub-lists (§2.5).
inline constexpr uint32_t kHudLayers = 13;

struct CommandBarComponent {
    std::string name;
    ComponentType type = ComponentType::Unknown;
    std::string group;
    // shell
    std::string model_name, mega_texture;
    bool model_offset_x = false, model_offset_y = false, manual_offset = false;
    bool hidden = false, disabled = false, no_shell = false;
    // textures (state lists; most hold one name)
    std::vector<std::string> blank_texture, icon_texture, icon_alternate_texture, mouse_over_texture, selected_texture,
        disabled_texture, flash_texture, build_texture, bar_texture, bar_overlay, lower_effect_texture,
        upper_effect_texture, overlay_texture, overlay2_texture;
    // placement
    int32_t base_layer = 0;  // 0..10 in data; HUD layer = base_layer [port: identity mapping, UI.md §6]
    bool has_size = false;
    Vec2f size{};            // Size (canvas units; "171 175" for the radar)
    float scale = 1.0f;
    Vec2f offset{}, default_offset{}, default_offset_widescreen{}, icon_offset{}, text_offset{}, text_offset2{},
        build_dial_offset{}, build_dial2_offset{}, overlay_offset{}, overlay2_offset{};
    bool has_default_offset_widescreen = false;
    bool pixel_align = true;  // Pixel_Align (default on [port]; the bars set False)
    // text
    std::string font_name, alternate_font_name;
    float font_point_size = 0.0f;  // 0 = no font given
    Rgba8 color{}, text_color{}, text_color2{};
    bool has_color = false, has_text_color = false, has_text_color2 = false;
    bool text_outline = false, text_emboss = false, left_justified = false, right_justified = false;
    float max_text_width = 0.0f;
    std::vector<std::string> tooltip_text;  // MasterTextFile keys, one per state
    // bars / animation
    int32_t max_bar_level = 10;
    bool smooth_bar = false, outlined_bar = false, lower_effect_additive = false;
    // behaviour (stage 2: button states, SFX hooks)
    std::string mouse_over_sfx, click_sfx, right_click_sfx;
    bool toggle = false, tab = false, selected_alpha = false, disabled_darken = false, click_shift = false;
    bool no_hidden_collision = false, can_animate = false, loop_anim = false;
    float anim_fps = 0.0f, blink_duration = 0.0f, blink_rate = 0.0f;
    bool blink_fade = false;
    Vec2f mouse_over_offset{}, disabled_offset{}, upper_effect_offset{}, lower_effect_offset{};
    // Every tag as written (upper-case tag -> trimmed text), for fields not modelled above.
    std::map<std::string, std::string> raw;
    std::string_view raw_value(std::string_view tag) const;
};

class ComponentTable {
public:
    // Appends every component under `root` (<CommandBarComponents>). Later duplicates replace earlier
    // entries in place (same index). Returns how many elements were read.
    size_t parse(const data::XmlNode& root);
    // CommandBarComponentFiles.xml -> each listed file (Data\XML\<name>) parsed in order. False if the list
    // file is missing; files that fail are reported in `error` and skipped.
    bool load(const FileProvider& files, std::string* error = nullptr);

    size_t size() const { return items_.size(); }
    const CommandBarComponent& operator[](size_t i) const { return items_[i]; }
    std::span<const CommandBarComponent> items() const { return items_; }
    // Index by name (ASCII case-insensitive), or -1.
    int32_t find(std::string_view name) const;
    const CommandBarComponent* get(std::string_view name) const {
        const int32_t i = find(name);
        return i < 0 ? nullptr : &items_[static_cast<size_t>(i)];
    }

private:
    std::vector<CommandBarComponent> items_;
    std::map<std::string, uint32_t> index_;  // upper-cased name -> index (lookup only)
};

// ---- MTD atlas (SPEC.md §5): u32 count, then count x { char name[64]; u32 x, y, w, h; u8 alpha } (81 bytes).
struct AtlasEntry {
    std::string name;
    uint32_t x = 0, y = 0, width = 0, height = 0;
    bool alpha = false;
};
class TextureAtlas {
public:
    bool parse(std::span<const uint8_t> bytes, std::string* error = nullptr);
    void add(AtlasEntry e);  // tests / synthetic atlases
    // Lookup by texture name, case-insensitive, extension ignored ("i_icon_select_small_l.tga" ==
    // "I_ICON_SELECT_SMALL_L"). Null if absent.
    const AtlasEntry* find(std::string_view texture_name) const;
    size_t size() const { return entries_.size(); }
    std::span<const AtlasEntry> entries() const { return entries_; }
    // Size of the atlas texture (for UVs); set by the owner of the texture.
    uint32_t texture_width = 1, texture_height = 1;

private:
    std::vector<AtlasEntry> entries_;
    std::map<std::string, uint32_t> index_;
};
std::string atlas_key(std::string_view texture_name);  // upper-case, no directory, no extension

// ---- MasterTextFile DAT: u32 count; count x { u32 crc, u32 value_chars, u32 key_chars }; UTF-16LE values;
// ASCII keys. crc = CRC-32 of the key bytes as stored. Lookup by key CRC (binary search when sorted), exact key
// compare, then an ASCII case-insensitive scan.
class TextDatabase {
public:
    bool parse(std::span<const uint8_t> bytes, std::string* error = nullptr);
    void add(std::string key, std::u16string value);  // tests; keeps the table sorted by (crc, key)
    const std::u16string* find(std::string_view key) const;
    // Text for a key, or the key itself (as UTF-16) when missing [port: the original shows the key].
    std::u16string text(std::string_view key) const;
    size_t size() const { return keys_.size(); }
    // [port] dev tool (ALAMO_PSEUDO_LOC=1, game_ui.cpp): every value through pseudo_localize(), so every lookup sees it.
    void pseudo_localize_all();
    // [port] t47: an English string the port adds (no MasterTextFile key): as written, pseudo-localized like the table when
    // pseudo_localize_all() ran, so the overflow check sees it too.
    std::u32string port_text(std::u32string_view english) const;
    std::string language;  // what load_master_text() loaded

private:
    void sort();
    std::vector<uint32_t> crcs_;
    std::vector<std::string> keys_;
    std::vector<std::u16string> values_;
    bool pseudo_ = false;
};
// [port] dev tool, not original behaviour: the string in [brackets] with accents, to catch overflow before a real translation does:
// brackets show clipping, Latin-1 accents show glyph / height problems, doubled letters inside the words (spaces stay, so
// wrapping still works) make it 35 % longer (30-40 % from 10 UTF-16 units on; shorter strings grow by at least the brackets).
// printf specs (%d, %.0f, %ls, %%), ##N inserts and \n-style escapes are kept. No ASCII letter: returned unchanged.
std::u16string pseudo_localize(std::u16string_view s);
// Data\Text\MasterTextFile_<LANGUAGE>.DAT, falling back to ENGLISH (UI.md §3.5). False if neither exists.
bool load_master_text(const FileProvider& files, std::string_view language, TextDatabase& out, std::string* log = nullptr);
std::string master_text_path(std::string_view language);

// UTF helpers (text is UTF-16 in the DAT files; the HUD lays out code points).
std::u32string utf16_to_utf32(std::u16string_view s);
std::u32string utf8_to_utf32(std::string_view s);

// ---- Layout -------------------------------------------------------------------------------------
// A bone of a shell model in shell MODEL space (bind pose, model units). Verified against the real
// i_tactical_controls.alo / i_galactic_controls.alo / i_main_reinforce.alo (stage 2): model units = canvas
// units, +Y up, +Z towards the viewer (bars z 5 over portraits z 4 over borders z 1), and the model origin is
// the screen CORNER the shell is anchored to - the tactical bar's bones run x 0..985, y -70..309 (origin =
// bottom-left of the 1024 x 768 base canvas), the land reinforcement window's x 0..161, y -270..0 (origin =
// top-left). `half_size` is the half extent of the (usually hidden) placeholder mesh attached to the bone:
// the component's rectangle (s_select_NN 50 x 50, radar 175 x 175, special_border_NN 54 x 119).
struct ShellBone {
    std::string name;
    float x = 0, y = 0, z = 0;
    Vec2f half_size{};      // 0 = no placeholder mesh
    Vec2f rect_center{};    // placeholder rectangle centre relative to the bone (model units)
};

struct ComponentPlacement {
    bool placed = false;     // false: no bone, no fallback (component not shown)
    bool from_bone = false;  // true: bone of a shell; false: [port] fallback position
    int32_t shell = -1;      // component index of the shell whose bone placed it
    Vec2f center{};          // canvas units, +Y up
    Vec2f size{};            // canvas units: the bone's placeholder rectangle (0 = unknown; fallbacks)
    Vec2f rect_center{};     // canvas centre of that rectangle (== center unless the mesh is off-centre)
    float z = 0.0f;          // bone z (model units, + = towards the viewer)
};

class HudLayout {
public:
    // Bones of a shell component's model (replaces earlier ones for that shell). Coordinates in model space
    // (see ShellBone); HudLayout::update moves them onto the canvas with shell_offset().
    void set_shell_bones(uint32_t shell_component, std::vector<ShellBone> bones);
    void clear_shell(uint32_t shell_component) { shells_.erase(shell_component); }
    // Shells whose bones place components (default: all with bones). An inactive shell keeps its bones but
    // places nothing (e.g. i_main_reinforce outside land battles).
    void set_shell_active(uint32_t shell_component, bool active);
    bool shell_active(uint32_t shell_component) const;
    // [port] fallback canvas position (+Y up) for a component without a bone (no shell .alo loaded, e.g.
    // without game data). Bone placements win.
    void set_fallback(uint32_t component, Vec2f center) { fallback_[component] = center; }
    // Shell anchoring + placement of every component for this canvas.
    void update(const HudCanvas& canvas, const ComponentTable& table);
    const ComponentPlacement& placement(uint32_t component) const;
    // Model -> canvas translation of a shell for this canvas (Model_Offset_X/Y anchoring, shell_anchor_offset).
    Vec2f shell_offset(uint32_t shell_component) const;
    std::span<const ShellBone> shell_bones(uint32_t shell_component) const;
    // Components (indices) of every shell with bones, ascending.
    std::vector<uint32_t> shells() const;

private:
    struct Shell {
        std::vector<ShellBone> bones;
        Vec2f offset{};
        bool active = true;
    };
    std::map<uint32_t, Shell> shells_;      // by component index (ordered: deterministic bone search order)
    std::map<uint32_t, Vec2f> fallback_;
    std::vector<ComponentPlacement> placements_;
};

// Model -> canvas translation of a shell (UI.md §2.2 [M]; rule fixed against the real models, [port]): on an axis
// with Model_Offset_X / _Y the model origin sits on the canvas edge the bones extend away from (mean bone
// coordinate > +1 unit: left / bottom edge; < -1: right / top edge; in between: centre); on an axis without
// the flag the origin is the canvas centre. At 4:3 the tactical bar's origin is (-512, -384). Dialog_Scene shells
// (full-screen FoC dialog scenes whose meshes span x -512..512) are always centred [port].
Vec2f shell_anchor_offset(const CommandBarComponent& shell, std::span<const ShellBone> bones, const HudCanvas& canvas);

// Default [port] fallback positions for the tactical HUD skeleton when the shell models are not loaded:
// radar bottom-left, selection panel grid bottom centre (s_select_NN / s_health_NN / s_shield_NN), ability
// buttons above it (special_button_NN), build queue (tqueueNN) above those, command buttons (c_buttonNN) and
// game buttons (b_retreat, b_reinforcement, ...) next to the radar, credits top-right, option / pause buttons
// top-left. Fills `layout` fallbacks for the components that exist.
void apply_tactical_fallback_layout(const ComponentTable& table, const HudCanvas& canvas, HudLayout& layout);

}  // namespace alamo::ui
