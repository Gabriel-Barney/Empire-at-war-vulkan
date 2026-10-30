#include "alamo/ui/gui_script_lua.hpp"

#include "alamo/lua/presentation.hpp"
#include "alamo/lua/story_model.hpp"

namespace alamo::ui {

namespace {
bool to_op(lua::PresentationOp op, GuiScriptOp& out) {
    using P = lua::PresentationOp;
    switch (op) {
        case P::GuiButtonIcon: out = GuiScriptOp::ButtonIcon; return true;
        case P::GuiComponentBlink: out = GuiScriptOp::ComponentBlink; return true;
        case P::GuiComponentColor: out = GuiScriptOp::ComponentColor; return true;
        case P::GuiComponentEnable: out = GuiScriptOp::ComponentEnable; return true;
        case P::GuiComponentFlash: out = GuiScriptOp::ComponentFlash; return true;
        case P::GuiComponentPlayAnim: out = GuiScriptOp::ComponentPlayAnim; return true;
        case P::GuiComponentStopAnim: out = GuiScriptOp::ComponentStopAnim; return true;
        case P::GuiComponentStopFlash: out = GuiScriptOp::ComponentStopFlash; return true;
        case P::GuiComponentText: out = GuiScriptOp::ComponentText; return true;
        case P::GuiComponentVisibility: out = GuiScriptOp::ComponentVisibility; return true;
        case P::GuiTextColor: out = GuiScriptOp::TextColor; return true;
        default: return false;
    }
}
}  // namespace

size_t gui_script_calls(const lua::PresentationLog& log, std::vector<GuiScriptCall>& out) {
    size_t n = 0;
    for (const lua::PresentationEvent& e : log.events()) {
        GuiScriptCall c;
        if (!to_op(e.op, c.op)) continue;
        c.frame = e.frame;
        for (const lua::PresentationArg& a : e.args) {
            switch (a.kind) {
                case lua::PresentationArg::Kind::Bool: c.args.push_back(GuiScriptArg::of(a.b)); break;
                case lua::PresentationArg::Kind::Number: c.args.push_back(GuiScriptArg::of(a.number)); break;
                case lua::PresentationArg::Kind::String: c.args.push_back(GuiScriptArg::of(a.text)); break;
                default: c.args.push_back(GuiScriptArg{}); break;
            }
        }
        out.push_back(std::move(c));
        ++n;
    }
    return n;
}

std::vector<HudObjective> hud_objectives(const lua::StoryModel& story) {
    std::vector<HudObjective> out;
    for (const lua::StoryObjective& o : story.objectives()) {
        HudObjective h;
        h.text_key = o.text;
        h.state = o.state == lua::StoryObjectiveState::Complete ? HudObjective::State::Complete
                  : o.state == lua::StoryObjectiveState::Failed ? HudObjective::State::Failed
                                                                 : HudObjective::State::Active;
        out.push_back(std::move(h));
    }
    return out;
}

}  // namespace alamo::ui
