// P2 stage 2: command-bar button behaviour (docs/re/UI.md §2.1 "textures (state)", "behaviour", "animation";
// §2.3 tooltips; §3.6 UI sounds).
//
// Per component the host / Lua sets a ButtonRuntime (hidden, disabled, selected, flash, blink, icon / text
// overrides, dial fills, tooltip state index). HudButtons tracks the pointer (InputRouter hover + HUD pointer
// events) and gives each component a visual state:
//   Disabled  > Pressed (left button held on it) > Selected (toggle / tab state) > MouseOver > Normal,
// draws it from the component's textures, fires the SFX hook (Mouse_Over_SFX on hover enter, Click_SFX /
// Right_Click_SFX on a click; P3 audio attaches a callback - nothing here plays sound) and reports clicks.
// Toggle components flip `selected` on a left click.
//
// [port] drawing rules (the original's per-state composition is not in the docs):
//   * layers bottom -> top (submission order in the PrimAlpha list; an additive lower effect goes to the PrimAdditive
//     list, drawn after the layer's alpha list - hud_draw.hpp): lower effect (Lower_Effect_Texture_Name) ->
//     Blank_Texture_Name[alt] -> icon (override, else Icon_Texture_Name, else Icon_Alternate_Texture_Name[alt]) ->
//     state texture -> flash -> build dial -> overlay -> text;
//   * a state texture (Mouse_Over_ / Selected_ / Disabled_Texture_Name) with the icon's atlas size REPLACES the
//     icon (full-art variants such as i_button_command_bar_options_over); otherwise it is drawn over it as a frame
//     (i_button_heros_over on b_retreat); a frame with no atlas alpha (i_button_unit_frame_select*) goes to the additive
//     list and never replaces an icon override; Disabled_Darken / Disable_Darken greys the icon (128) when disabled;
//   * Flash_Texture_Name alternates on / off every 0.25 s while flashing;
//   * blink: Blink_Rate = seconds per on / off half period, for Blink_Duration seconds (65536 = forever); with
//     Blink_Fade the alpha follows a cosine instead of on / off;
//   * component offsets (Icon_Offset, Build_Dial_Offset, ...) are canvas units, +Y down (the stage-1 convention).
// Tooltips (TooltipState): after GameConstants Tooltip_Delay (330 ms) of hover on a component with Tooltip_Text,
// the text of key Tooltip_Text[tooltip index] is shown on layer 10 (tooltip_back frame, tooltip_left_text font),
// wrapped at Max_Text_Width (or 200 units), [port] above-right of the cursor and kept inside the canvas.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/ui/hud_components.hpp"
#include "alamo/ui/hud_draw.hpp"
#include "alamo/ui/hud_input.hpp"

namespace alamo::ui {

class FontCache;

// UI SFX hook point (UI.md §3.6): `event` is the SFXEventsGUI.xml name (Mouse_Over_SFX, Click_SFX, ...).
using HudSfxHook = std::function<void(std::string_view event, int32_t component)>;

struct ButtonRuntime {
    bool hidden = false, disabled = false, selected = false;
    bool flash = false;
    double blink_start = -1.0;     // < 0: not blinking (Lua Blink / host): started at this time
    int32_t alt = -1;              // alternate texture index (-1: the HUD's faction index)
    int32_t tooltip_index = 0;     // Tooltip_Text entry (< 0: no tooltip)
    std::string icon_override;     // replaces the icon texture ("" = data)
    std::u32string text;           // drawn with the component's font ("" = none)
    std::u32string text2;          // second text (Text_Offset2)
    float build_fill = -1.0f;      // Build_Texture_Name dial 0..100 (< 0: none)
    float build_fill2 = -1.0f;     // second dial (Build_Dial2_Offset)
    bool overlay = false, overlay2 = false, lower_effect = false, upper_effect = false;
    Rgba8 tint{255, 255, 255, 255};
    // P4 Lua GUI control (gui_script.hpp): GUI_Text_Color override, GUI_Component_Play_Anim state (anim_start >= 0: the
    // Can_Animate frame animation plays, anim_frame / HudButtons::draw).
    bool text_color_set = false;
    Rgba8 text_color{255, 255, 255, 255};
    std::string anim;
    bool anim_loop = false;
    double anim_start = -1.0;
    // Lua GUI_Component_Visibility / _Enable as a MOD override (HUD_GAPS §5.2: vanilla ships no GUI Lua): -1 = no script
    // call, 0 / 1 = the last script value. Components whose state the engine recomputes every frame (the tactical order
    // buttons) apply these over the engine state; other components just keep the hidden / disabled the script wrote.
    int8_t script_visible = -1, script_enabled = -1;
};

enum class ButtonVisual : uint8_t { Normal, MouseOver, Pressed, Selected, Disabled };

struct ButtonClick {
    int32_t component = -1;
    Button button = Button::Left;
    bool shift = false, ctrl = false;
    int32_t x = 0, y = 0;  // pixel
};

class HudButtons {
public:
    void bind(const ComponentTable& table) { table_ = &table; }
    // Runtime state of a component; created on first use with hidden / disabled = the component's Hidden /
    // Disabled data (components without a runtime entry also use those).
    ButtonRuntime& state(uint32_t component);
    const ButtonRuntime* find_state(uint32_t component) const;
    void set_sfx_hook(HudSfxHook hook) { sfx_ = std::move(hook); }
    // Can_Animate frame animation (anim_frame): start (restarts when already playing) / stop.
    void play_anim(uint32_t component, double now, bool loop = false, std::string name = "play");
    void stop_anim(uint32_t component);

    // One frame of pointer input: `hover` = RoutedInput::hover_component, `events` = RoutedInput::hud.
    // Hidden / disabled components never click (a disabled click is swallowed). Clicks are appended to `clicks`.
    void update(int32_t hover, std::span<const HudPointerEvent> events, bool shift, bool ctrl, double now,
                std::vector<ButtonClick>& clicks);
    ButtonVisual visual(uint32_t component) const;
    int32_t hover() const { return hover_; }
    double hover_since() const { return hover_since_; }
    int32_t pressed() const { return pressed_; }

    // Draws `component` centred at `center` (canvas) on its Base_Layer (+ layer_bias). `size` = its rectangle
    // (from the shell placeholder; {0,0} = the blank / icon atlas size). Returns the drawn rectangle size.
    Vec2f draw(HudDrawList& dl, const TextureAtlas& atlas, FontCache* fonts, const HudCanvas& canvas, uint32_t component,
               Vec2f center, Vec2f size, int32_t faction_alt, double now, int32_t layer_bias = 0) const;

private:
    const ComponentTable* table_ = nullptr;
    std::map<uint32_t, ButtonRuntime> runtime_;  // ordered: deterministic
    HudSfxHook sfx_;
    int32_t hover_ = -1, pressed_ = -1;
    double hover_since_ = 0.0;
};

// Blink alpha factor 0..1 for a component's Blink_* fields started at `start` (UI.md §2.1 animation, [port]).
float blink_alpha(const CommandBarComponent& c, double start, double now);

// Can_Animate frame animation (UI.md §2.1 "animation"; [port] rules): the frames are the component's
// Icon_Alternate_Texture_Name list played at Anim_FPS (<= 0: 10) from `start`; Animate_Back plays forward then back
// (ping-pong, the end frames once per turn); Loop_Anim (or the runtime's anim_loop, e.g. Lua Play_Anim) repeats, else
// the animation ends after one pass and the button shows its normal icon again. Returns the frame index, or -1 when
// not playing / finished. HudButtons::draw draws the frame in place of the icon, or over it (later in the Alpha list) when
// Animate_Upper_Effect is set (the autofire outline of special_button_NN). ButtonRuntime::anim_start >= 0 = playing;
// HudButtons::play_anim / stop_anim (and Lua GUI_Component_Play_Anim / Stop_Anim) start / stop it.
int32_t anim_frame(const CommandBarComponent& c, double start, double now, size_t frames, bool loop);

struct TooltipConfig {
    double delay = 0.330;              // GameConstants Tooltip_Delay (ms / 1000)
    double encyclopedia_delay = 0.750; // Encyclopedia_Delay
    double long_encyclopedia_delay = 4.0;  // Long_Encyclopedia_Delay: the encyclopedia delay with delay_tooltips
    float default_wrap_units = 200.0f;
    // HUD_GAPS §2.2: the gameplay option IDC_DELAY_TOOLTIPS ("delay tooltips", player profile) - host feed.
    bool delay_tooltips = false;
    float encyclopedia_fade_rate = 4.0f;       // Encyclopedia_Fade_Rate: alpha per second, in and out
    double encyclopedia_min_display = 1.0;     // Encyclopedia_Min_Display_Time (s): shown at least this long
    double encyclopedia_wait() const { return delay_tooltips ? long_encyclopedia_delay : encyclopedia_delay; }
};
// GameConstants Tooltip_Delay / Encyclopedia_Delay / Long_Encyclopedia_Delay (ms), Encyclopedia_Fade_Rate (/s) and
// Encyclopedia_Min_Display_Time (s) from a parsed GameConstants root.
TooltipConfig tooltip_config_from(const data::XmlNode& game_constants_root);

class TooltipState {
public:
    // Shows a tooltip when `buttons` has hovered a component with Tooltip_Text for `delay`.
    void update(const HudButtons& buttons, const ComponentTable& table, const TooltipConfig& cfg, double now);
    bool active() const { return component_ >= 0; }
    int32_t component() const { return component_; }
    // Text key for the hovered component (Tooltip_Text[tooltip index], clamped), "" when none.
    std::string key(const HudButtons& buttons, const ComponentTable& table) const;
    // Draws the tooltip near the cursor pixel. Returns false when nothing was drawn.
    bool draw(HudDrawList& dl, const TextureAtlas& atlas, FontCache& fonts, const HudCanvas& canvas,
              const ComponentTable& table, const HudButtons& buttons, const TextDatabase* text, Vec2f cursor_px,
              int32_t faction_alt, const TooltipConfig& cfg) const;
    // The same tooltip box for a given text (e.g. a hero type's name on the hero bar). Returns false for "".
    static bool draw_text(HudDrawList& dl, const TextureAtlas& atlas, FontCache& fonts, const HudCanvas& canvas,
                          const ComponentTable& table, const std::u32string& body, Vec2f cursor_px, int32_t faction_alt,
                          const TooltipConfig& cfg);

private:
    int32_t component_ = -1;
};

// Back + faction frame of a tooltip / encyclopedia box whose inner rectangle is `center`, `size`. `back` = tooltip_back /
// encyclopedia_back: Blank_Texture_Name at `back_color`, Icon_Alternate_Texture_Name[faction_alt] the frame.
// [inferred, base-game captures C3_*tooltip*] the frame textures are filled light-blue rectangles (empire: soft rim,
// rebel: flat, translucent), so the frame goes UNDER the back and kTooltipFrameMargin larger on every side: only a rim
// shows. A missing back texture draws a flat dark box (e_background's colour). Both quads on the Alpha list, frame first.
inline constexpr float kTooltipFrameMargin = 6.0f;  // canvas units (the captures' 8 px rim at 1080p)
void add_tooltip_box(HudDrawList& dl, uint32_t layer, const TextureAtlas& atlas, const CommandBarComponent* back,
                     int32_t faction_alt, Vec2f center, Vec2f size, Rgba8 back_color);

// GUIDialogs.xml <Tooltips>: dialog control id (IDC_*) -> MasterTextFile key (<TextID>).
std::map<std::string, std::string> parse_dialog_tooltips(const data::XmlNode& gui_dialogs_root);

}  // namespace alamo::ui
