// P4 Lua GUI control hooks (docs/re/UI.md §2.3 last paragraph; docs/re/LUA_API.md GUI_* rows): scripts drive
// command-bar components by name. engine/lua records the calls in its PresentationLog (PresentationOp::Gui*, the L1
// presentation family); this consumer applies them to a HUD's ButtonRuntime table (TacticalHud / GalacticHud
// buttons()). The engine/lua -> GuiScriptCall conversion lives in the optional alamo_ui_lua library
// (gui_script_lua.hpp) so alamo_ui_hud does not depend on the VM.
//
// Argument counts from the binary's error strings (LUA_API.md): GUI_Button_Icon 6 (1 string, 3 float),
// GUI_Component_Blink 4 (string, bool, ...), GUI_Component_Color 5 (string, float x4), GUI_Component_Enable 2,
// GUI_Component_Flash 4 (string, bool, ...), GUI_Component_Play_Anim 4 (string, animation name, ...),
// GUI_Component_Stop_Anim 1, GUI_Component_Stop_Flash 1, GUI_Component_Text 2 (string, text id), GUI_Component_Visibility 2,
// GUI_Text_Color 5 (string, float x4).
// [port] meanings where the strings stop: GUI_Button_Icon argument 2 = the icon texture (other arguments ignored);
// Blink / Flash argument 2 = on / off (the two trailing arguments ignored); colours are 0..1 floats when every channel is
// <= 1, else 0..255; Play_Anim records the animation name and loop flag (argument 3) - frame animation is not drawn;
// an unknown component name is an error (returned, nothing applied).
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/ui/hud_buttons.hpp"
#include "alamo/ui/hud_components.hpp"

namespace alamo::ui {

enum class GuiScriptOp : uint8_t {
    ButtonIcon, ComponentBlink, ComponentColor, ComponentEnable, ComponentFlash, ComponentPlayAnim, ComponentStopAnim,
    ComponentStopFlash, ComponentText, ComponentVisibility, TextColor
};
const char* gui_script_op_name(GuiScriptOp op);  // Lua name, e.g. "GUI_Component_Visibility"

struct GuiScriptArg {
    enum class Kind : uint8_t { Nil, Bool, Number, String } kind = Kind::Nil;
    bool b = false;
    double number = 0.0;
    std::string text;
    static GuiScriptArg of(bool v) {
        GuiScriptArg a;
        a.kind = Kind::Bool;
        a.b = v;
        return a;
    }
    static GuiScriptArg of(double v) {
        GuiScriptArg a;
        a.kind = Kind::Number;
        a.number = v;
        return a;
    }
    static GuiScriptArg of(std::string v) {
        GuiScriptArg a;
        a.kind = Kind::String;
        a.text = std::move(v);
        return a;
    }
};

struct GuiScriptCall {
    GuiScriptOp op = GuiScriptOp::ComponentVisibility;
    std::vector<GuiScriptArg> args;  // as passed by the script (component name first)
    uint32_t frame = 0;              // sim frame of the call (ordering only)
};

// Applies one call. `text` resolves GUI_Component_Text keys (null: the key is shown). False + `error` on a bad call.
bool apply_gui_script(const GuiScriptCall& call, const ComponentTable& table, HudButtons& buttons, const TextDatabase* text,
                      double now, std::string* error = nullptr);
// Applies a batch in order; returns how many applied.
size_t apply_gui_scripts(std::span<const GuiScriptCall> calls, const ComponentTable& table, HudButtons& buttons,
                         const TextDatabase* text, double now, std::vector<std::string>* errors = nullptr);

}  // namespace alamo::ui
