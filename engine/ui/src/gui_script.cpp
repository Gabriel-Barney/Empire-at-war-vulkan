#include "alamo/ui/gui_script.hpp"

#include <algorithm>
#include <cmath>

namespace alamo::ui {

const char* gui_script_op_name(GuiScriptOp op) {
    switch (op) {
        case GuiScriptOp::ButtonIcon: return "GUI_Button_Icon";
        case GuiScriptOp::ComponentBlink: return "GUI_Component_Blink";
        case GuiScriptOp::ComponentColor: return "GUI_Component_Color";
        case GuiScriptOp::ComponentEnable: return "GUI_Component_Enable";
        case GuiScriptOp::ComponentFlash: return "GUI_Component_Flash";
        case GuiScriptOp::ComponentPlayAnim: return "GUI_Component_Play_Anim";
        case GuiScriptOp::ComponentStopAnim: return "GUI_Component_Stop_Anim";
        case GuiScriptOp::ComponentStopFlash: return "GUI_Component_Stop_Flash";
        case GuiScriptOp::ComponentText: return "GUI_Component_Text";
        case GuiScriptOp::ComponentVisibility: return "GUI_Component_Visibility";
        case GuiScriptOp::TextColor: return "GUI_Text_Color";
    }
    return "?";
}

namespace {

uint32_t expected_args(GuiScriptOp op) {
    switch (op) {
        case GuiScriptOp::ButtonIcon: return 6;
        case GuiScriptOp::ComponentBlink:
        case GuiScriptOp::ComponentFlash:
        case GuiScriptOp::ComponentPlayAnim: return 4;
        case GuiScriptOp::ComponentColor:
        case GuiScriptOp::TextColor: return 5;
        case GuiScriptOp::ComponentEnable:
        case GuiScriptOp::ComponentText:
        case GuiScriptOp::ComponentVisibility: return 2;
        case GuiScriptOp::ComponentStopAnim:
        case GuiScriptOp::ComponentStopFlash: return 1;
    }
    return 1;
}

bool truthy(const GuiScriptArg& a) {
    if (a.kind == GuiScriptArg::Kind::Bool) return a.b;
    if (a.kind == GuiScriptArg::Kind::Number) return a.number != 0.0;
    return a.kind != GuiScriptArg::Kind::Nil;
}

Rgba8 color_of(const std::vector<GuiScriptArg>& a) {
    double c[4] = {1, 1, 1, 1};
    bool unit = true;
    for (size_t i = 0; i < 4; ++i) {
        if (i + 1 < a.size() && a[i + 1].kind == GuiScriptArg::Kind::Number) c[i] = a[i + 1].number;
        if (c[i] > 1.0) unit = false;
    }
    auto ch = [&](double v) {
        const double s = unit ? v * 255.0 : v;
        return static_cast<uint8_t>(std::clamp(std::lround(s), 0L, 255L));
    };
    return {ch(c[0]), ch(c[1]), ch(c[2]), ch(c[3])};
}

}  // namespace

bool apply_gui_script(const GuiScriptCall& call, const ComponentTable& table, HudButtons& buttons, const TextDatabase* text,
                      double now, std::string* error) {
    auto fail = [&](std::string m) {
        if (error) *error = std::string(gui_script_op_name(call.op)) + ": " + std::move(m);
        return false;
    };
    const auto& a = call.args;
    if (a.size() != expected_args(call.op))
        return fail("expected " + std::to_string(expected_args(call.op)) + " arguments, got " + std::to_string(a.size()));
    if (a[0].kind != GuiScriptArg::Kind::String) return fail("argument 1 must be a component name");
    const int32_t ci = table.find(a[0].text);
    if (ci < 0) return fail("unknown component '" + a[0].text + "'");
    ButtonRuntime& r = buttons.state(static_cast<uint32_t>(ci));
    switch (call.op) {
        case GuiScriptOp::ComponentVisibility:
            r.hidden = !truthy(a[1]);
            r.script_visible = r.hidden ? int8_t{0} : int8_t{1};
            break;
        case GuiScriptOp::ComponentEnable:
            r.disabled = !truthy(a[1]);
            r.script_enabled = r.disabled ? int8_t{0} : int8_t{1};
            break;
        case GuiScriptOp::ComponentText:
            if (a[1].kind != GuiScriptArg::Kind::String) return fail("argument 2 must be a text id");
            r.text = text ? utf16_to_utf32(text->text(a[1].text)) : utf8_to_utf32(a[1].text);
            break;
        case GuiScriptOp::ButtonIcon:
            if (a[1].kind != GuiScriptArg::Kind::String) return fail("argument 2 must be a texture name");
            r.icon_override = a[1].text;
            break;
        case GuiScriptOp::ComponentBlink: r.blink_start = truthy(a[1]) ? now : -1.0; break;
        case GuiScriptOp::ComponentFlash: r.flash = truthy(a[1]); break;
        case GuiScriptOp::ComponentStopFlash: r.flash = false; break;
        case GuiScriptOp::ComponentColor: r.tint = color_of(a); break;
        case GuiScriptOp::TextColor:
            r.text_color = color_of(a);
            r.text_color_set = true;
            break;
        case GuiScriptOp::ComponentPlayAnim:
            if (a[1].kind != GuiScriptArg::Kind::String) return fail("argument 2 must be an animation name");
            r.anim = a[1].text;
            r.anim_loop = truthy(a[2]);
            r.anim_start = now;
            break;
        case GuiScriptOp::ComponentStopAnim:
            r.anim.clear();
            r.anim_start = -1.0;
            break;
    }
    return true;
}

size_t apply_gui_scripts(std::span<const GuiScriptCall> calls, const ComponentTable& table, HudButtons& buttons,
                         const TextDatabase* text, double now, std::vector<std::string>* errors) {
    size_t n = 0;
    for (const GuiScriptCall& c : calls) {
        std::string e;
        if (apply_gui_script(c, table, buttons, text, now, &e)) ++n;
        else if (errors) errors->push_back(std::move(e));
    }
    return n;
}

}  // namespace alamo::ui
