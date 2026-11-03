// Binding family: story events and flags, objectives, radar blips, planet highlights (L1 stage 2).
// They act on the StoryModel attached to the VM (attach_story, include/alamo/lua/story_model.hpp);
// without one, queries return nil/false and commands do nothing (a message is logged).
// Contracts and messages: docs/re/LUA_API.md "Binding map" (the original's validation strings).
// Error discipline: vm_internal.hpp (no Lua error raised from here; Call::fail).
#include <cstdio>
#include <string>

#include "alamo/lua/story_model.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "vm_internal.hpp"

namespace alamo::lua {

using detail::Call;

namespace {

const char kStoryKey[] = "alamo.story";
constexpr uint32_t kNone = 0xFFFFFFFFu;

StoryModel* story(lua_State* L) {
    lua_pushstring(L, kStoryKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* m = static_cast<StoryModel*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return m;
}
StoryModel* story(Call& c) { return story(c.L); }

uint32_t cls_plot() {
    static const uint32_t c = detail::class_id("StoryPlotWrapper");
    return c;
}
uint32_t cls_event() {
    static const uint32_t c = detail::class_id("StoryEventWrapper");
    return c;
}

int push_nil(Call& c) {
    lua_pushnil(c.L);
    return 1;
}
int push_bool(Call& c, bool b) {
    lua_pushboolean(c.L, b ? 1 : 0);
    return 1;
}

void push_plot(Call& c, uint32_t plot) {
    detail::Ud ud;
    ud.cls = cls_plot();
    ud.id = static_cast<int32_t>(plot);
    detail::push_cached_userdata(c.L, ud, static_cast<lua_Number>(plot));  // same plot -> same userdata
}

void push_event(Call& c, uint32_t plot, uint32_t event) {
    detail::Ud ud;
    ud.cls = cls_event();
    ud.id = static_cast<int32_t>(plot);
    ud.value = event;
    // Key plot * 2^24 + event: exact in a double for any loadable plot set.
    detail::push_cached_userdata(c.L, ud, static_cast<lua_Number>(plot) * 16777216.0 + static_cast<lua_Number>(event));
}

bool self_plot_valid(Call& c) {
    const StoryModel* m = story(c);
    return m && c.self->id >= 0 && static_cast<size_t>(c.self->id) < m->plot_count();
}
bool self_event_valid(Call& c) {
    const StoryModel* m = story(c);
    return self_plot_valid(c) && c.self->value < m->plot(static_cast<uint32_t>(c.self->id)).events.size();
}
StoryEventDef& self_event(Call& c) {
    return story(c)->plot(static_cast<uint32_t>(c.self->id)).events[c.self->value];
}
StoryEventState& self_event_state(Call& c) {
    return story(c)->plot(static_cast<uint32_t>(c.self->id)).state[c.self->value];
}

void no_story(Call& c, const char* what) { c.vm->message(std::string("[story] ") + what + " ignored: no story attached"); }

// Lua value -> StoryParam (string/number/game object/player/bool). False for nil, tables, functions.
bool to_param(Call& c, int i, StoryParam& out) {
    if (!c.has(i)) return false;
    const int t = lua_type(c.L, c.idx(i));
    if (t == LUA_TNUMBER) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.14g", c.number(i));  // Lua 5.0 tostring format
        out = StoryParam::of_text(buf);
        return true;
    }
    if (t == LUA_TSTRING) {
        out = StoryParam::of_text(c.string(i));
        return true;
    }
    if (t == LUA_TBOOLEAN) {
        out = StoryParam::of_text(lua_toboolean(c.L, c.idx(i)) ? "1" : "0");
        return true;
    }
    if (const detail::Ud* o = c.userdata(i, detail::cls_game_object())) {
        out = StoryParam::of_object(o->id);
        return true;
    }
    if (const detail::Ud* p = c.userdata(i, detail::cls_player())) {
        out = StoryParam::of_player(p->id);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- globals

// Story_Event(name [, game object]) - dispatches STORY_AI_NOTIFICATION (docs/re/STORY.md §5): events
// whose Event_Param2 names `name`; with an object, its type name must be in Event_Param1, without
// one Event_Param1 must be empty.
int g_story_event(Call& c) {
    if (c.nargs < 1 || c.nargs > 2)
        return c.fail("StoryEvent -- invalid number of parameters. Expected 1 or 2, got %d.", c.nargs);
    if (!c.is_string(1)) return c.fail("StoryEvent -- invalid type for parameter 1. Expected string");
    const detail::Ud* obj = c.nargs == 2 ? c.userdata(2, detail::cls_game_object()) : nullptr;
    if (c.nargs == 2 && !lua_isnil(c.L, c.idx(2)) && !obj)
        return c.fail("StoryEvent -- invalid type for parameter 2. Expected game object.");
    StoryModel* m = story(c);
    if (!m) {
        no_story(c, "Story_Event");
        return 0;
    }
    std::string type_name;
    if (obj && detail::object_alive(*c.vm, *obj) && c.vm->host.types) {
        const objects::TypeIndex t = c.vm->host.store->type(detail::object_handle(*obj));
        if (t < c.vm->host.types->raw().type_count()) type_name = std::string(c.vm->host.types->raw().types().name[t]);
    }
    m->story_event(c.string(1), type_name, obj != nullptr);
    return 0;
}

// Get_Story_Plot("File.xml") -> StoryPlotWrapper or nil.
int g_get_story_plot(Call& c) {
    if (c.nargs != 1) return c.fail("Get_Story_Plot -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    if (!c.is_string(1)) return c.fail("Get_Story_Plot -- invalid type for parameter 1. Expected string.");
    const StoryModel* m = story(c);
    const uint32_t p = m ? m->find_plot(c.string(1)) : kNone;
    if (p == kNone) return push_nil(c);
    push_plot(c, p);
    return 1;
}

// Check_Story_Flag(player, flag_name, location, reset) -> boolean. [inferred] AI story flags are
// raised by the host (StoryModel::raise_ai_flag) per (player, name, location); reset clears on read.
int g_check_story_flag(Call& c) {
    if (c.nargs != 4)
        return c.fail("Check_AI_Story_Flag -- invalid number of parameters. Expected between 4, got %d.", c.nargs);
    const detail::Ud* player = c.userdata(1, detail::cls_player());
    if (!player) return c.fail("Check_AI_Story_Flag -- invalid type for parameter 1. Expected player");
    if (!c.is_string(2)) return c.fail("Check_AI_Story_Flag -- invalid type for parameter 2. Expected string");
    const detail::Ud* loc = c.userdata(3, detail::cls_game_object());
    const bool reset = lua_toboolean(c.L, c.idx(4)) != 0;
    StoryModel* m = story(c);
    if (!m) return push_bool(c, false);
    return push_bool(c, m->check_ai_flag(player->id, c.string(2), loc ? loc->id : -1, reset));
}

// Add_Objective(text_id, boolean). [inferred] the boolean is not modelled.
int g_add_objective(Call& c) {
    if (c.nargs != 2) return c.fail("Add_Objective -- invalid number of parameters. Expected 2, got %d.", c.nargs);
    if (!c.is_string(1)) return c.fail("Add_Objective -- invalid type for parameter 1. Expected string.");
    if (lua_type(c.L, c.idx(2)) != LUA_TBOOLEAN)
        return c.fail("Add_Objective -- invalid type for parameter 2. Expected boolean.");
    StoryModel* m = story(c);
    if (!m) {
        no_story(c, "Add_Objective");
        return 0;
    }
    m->add_objective(c.string(1), true);
    return 0;
}

// Add_Radar_Blip(object, name). [inferred] a blip of the same name is replaced.
int g_add_radar_blip(Call& c) {
    if (c.nargs != 2) return c.fail("Add_Radar_Blip -- invalid number of parameters. Expected 2, got %d.", c.nargs);
    const detail::Ud* o = c.userdata(1, detail::cls_game_object());
    if (!o || !detail::object_alive(*c.vm, *o))
        return c.fail("Add_Radar_Blip -- could not deduce a position from parameter 1.");
    if (!c.is_string(2)) return c.fail("Add_Radar_Blip -- invalid type for parameter 2. Expected string.");
    if (StoryModel* m = story(c)) m->add_blip(c.string(2), o->id);
    return 0;
}

int g_remove_radar_blip(Call& c) {
    if (c.nargs != 1) return c.fail("Remove_Radar_Blip -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    if (!c.is_string(1)) return c.fail("Remove_Radar_Blip -- invalid type for parameter 1. Expected strng.");
    if (StoryModel* m = story(c)) m->remove_blip(c.string(1));
    return 0;
}

int g_add_planet_highlight(Call& c) {
    if (c.nargs != 2)
        return c.fail("Add_Planet_Highlight -- invalid number of parameters. Expected 2, got %d.", c.nargs);
    const detail::Ud* o = c.userdata(1, detail::cls_game_object());
    if (!o || !detail::object_alive(*c.vm, *o))
        return c.fail("Add_Planet_Highlight -- invalid type for parameter 1. Expected game object.");
    if (!c.is_string(2)) return c.fail("Add_Planet_Highlight -- invalid type for parameter 2. Expected string.");
    if (StoryModel* m = story(c)) m->add_highlight(c.string(2), o->id);
    return 0;
}

int g_remove_planet_highlight(Call& c) {
    if (c.nargs != 1)
        return c.fail("Remove_Planet_Highlight -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    if (!c.is_string(1)) return c.fail("Remove_Planet_Highlight -- invalid type for parameter 1. Expected strng.");
    if (StoryModel* m = story(c)) m->remove_highlight(c.string(1));
    return 0;
}

// ---------------------------------------------------------------- StoryPlotWrapper

int m_plot_is_valid(Call& c) { return push_bool(c, self_plot_valid(c)); }

int m_plot_get_event(Call& c) {
    if (c.nargs != 1)
        return c.fail("StoryPlotWrapper::Get_Event -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    if (!c.is_string(1)) return c.fail("StoryPlotWrapper::Get_Event -- invalid type for parameter 1. Expected string.");
    if (!self_plot_valid(c)) return push_nil(c);
    const uint32_t plot = static_cast<uint32_t>(c.self->id);
    const uint32_t e = story(c)->find_event(plot, c.string(1));
    if (e == kNone) return push_nil(c);  // [inferred] nil for an unknown event
    push_event(c, plot, e);
    return 1;
}

int m_plot_activate(Call& c) {
    if (self_plot_valid(c)) story(c)->set_plot_active(static_cast<uint32_t>(c.self->id), true);
    return 0;
}
int m_plot_suspend(Call& c) {
    if (self_plot_valid(c)) story(c)->set_plot_active(static_cast<uint32_t>(c.self->id), false);
    return 0;
}
int m_plot_reset(Call& c) {
    if (self_plot_valid(c)) story(c)->reset_plot(static_cast<uint32_t>(c.self->id));
    return 0;
}

// ---------------------------------------------------------------- StoryEventWrapper

int m_event_is_valid(Call& c) { return push_bool(c, self_event_valid(c)); }

// story t02: Set_Event_Parameter / Set_Reward_Parameter go through the event's vf2 (STORY.md 0.2: "set parameter i
// (0-based) from a list of strings"), the same setter the XML loader uses, so a Lua-set parameter is matched by the
// event type's own rule (name lists, per-name counts, thresholds, comparisons) exactly like the XML text.
//   * The index is 0-based: 0 = Event_Param1 / Reward_Param1. [H by the shipped scripts: Intervention_Build_Units
//     (0 = the CONSTRUCT type, 1 = the count), Empire Act IV (1 = the SPACE_TACTICAL planet, Param1 is the plots file),
//     Set_Reward_Parameter(0, credits) for CREDITS.]
//   * The value becomes text [port: the conversion is not traced]: a number as Lua's tostring, a string as is, a
//     boolean 1 / 0, a game object its type name (planets are named by their type in every story rule), a game object
//     type its name. A player stays a player value (compared by id; no shipped script passes one).
// Setting an event parameter restarts the per-name countdowns (re-read from Param2 / Param3 at the next evaluation).
bool to_story_text(Call& c, int i, StoryParam& out) {
    if (const detail::Ud* o = c.userdata(i, detail::cls_game_object())) {
        if (!detail::object_alive(*c.vm, *o) || !c.vm->host.store || !c.vm->host.types) return false;
        const objects::TypeIndex t = c.vm->host.store->type(detail::object_handle(*o));
        if (t >= c.vm->host.types->raw().type_count()) return false;
        out = StoryParam::of_text(c.vm->host.types->raw().types().name[t]);
        return true;
    }
    if (const detail::Ud* t = c.userdata(i, detail::cls_type())) {
        if (!c.vm->host.types || t->value >= c.vm->host.types->raw().type_count()) return false;
        out = StoryParam::of_text(c.vm->host.types->raw().types().name[static_cast<size_t>(t->value)]);
        return true;
    }
    return to_param(c, i, out);
}

int set_param_common(Call& c, bool reward) {
    const char* who = reward ? "StoryEventWrapper::Set_Reward_Parameter" : "StoryEventWrapper::Set_Event_Parameter";
    if (c.nargs != 2) return c.fail("%s -- invalid number of parameters. Expected 2, got %d.", who, c.nargs);
    if (!c.is_number(1)) return c.fail("%s -- invalid type for parameter 1. Expected number.", who);
    const int32_t index = detail::to_int(c.number(1));  // 0-based (vf2)
    StoryParam p;
    if (index < 0 || index >= 64 || !to_story_text(c, 2, p))
        return c.fail("%s -- must set exactly one parameter per parameter index.", who);
    if (!self_event_valid(c)) return 0;
    StoryEventDef& def = self_event(c);
    std::vector<StoryParam>& v = reward ? def.reward_params : def.event_params;
    if (v.size() <= static_cast<size_t>(index)) v.resize(static_cast<size_t>(index) + 1);
    v[static_cast<size_t>(index)] = std::move(p);
    StoryEventState& st = self_event_state(c);
    if (!reward) st.countdowns.clear();
    ++st.lua_edits;
    return 0;
}
int m_event_set_event_parameter(Call& c) { return set_param_common(c, false); }
int m_event_set_reward_parameter(Call& c) { return set_param_common(c, true); }

int m_event_set_reward_type(Call& c) {
    if (c.nargs != 1)
        return c.fail("StoryEventWrapper::Set_Reward_Type -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    if (!c.is_string(1))
        return c.fail("StoryEventWrapper::Set_Reward_Type -- invalid type for parameter 1. Expected string.");
    if (!self_event_valid(c)) return 0;
    std::string t(c.string(1));
    for (char& ch : t) ch = (ch >= 'a' && ch <= 'z') ? static_cast<char>(ch - 'a' + 'A') : ch;
    self_event(c).reward_type = std::move(t);
    ++self_event_state(c).lua_edits;
    return 0;
}

// Set_Dialog(name): [inferred] replaces the dialog lines with one entry "dialog:<name>".
int m_event_set_dialog(Call& c) {
    if (c.nargs != 1)
        return c.fail("StoryEventWrapper::Set_Dialog -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    if (!c.is_string(1)) return c.fail("StoryEventWrapper::Set_Dialog -- invalid type for parameter 1. Expected string.");
    if (!self_event_valid(c)) return 0;
    self_event(c).dialog.assign(1, "dialog:" + std::string(c.string(1)));
    ++self_event_state(c).lua_edits;
    return 0;
}

// Add_Dialog_Text(text_id, value): [inferred] appends "text_id|value" (value: string/number/object).
int m_event_add_dialog_text(Call& c) {
    if (c.nargs != 2)
        return c.fail("StoryEventWrapper::Add_Dialog_Text -- invalid number of parameters. Expected 2.");
    if (!c.is_string(1))
        return c.fail("StoryEventWrapper::Add_Dialog_Text -- invalid type for parameter 1. Expected string.");
    StoryParam p;
    if (!to_param(c, 2, p)) return c.fail("StoryEventWrapper::Add_Dialog_Text -- invalid type for parameter %d.", 2);
    if (!self_event_valid(c)) return 0;
    std::string line(c.string(1));
    line += "|";
    line += p.kind == StoryParam::Kind::Text ? p.text : ((p.kind == StoryParam::Kind::Object ? "object:" : "player:") + std::to_string(p.id));
    self_event(c).dialog.push_back(std::move(line));
    ++self_event_state(c).lua_edits;
    return 0;
}

int m_event_clear_dialog_text(Call& c) {
    if (!self_event_valid(c)) return 0;
    self_event(c).dialog.clear();
    ++self_event_state(c).lua_edits;
    return 0;
}

constexpr BindingKind G = BindingKind::Global;
constexpr BindingKind M = BindingKind::Method;

constexpr detail::ImplEntry kImpl[] = {
    {G, "", "Story_Event", g_story_event},
    {G, "", "Get_Story_Plot", g_get_story_plot},
    {G, "", "Check_Story_Flag", g_check_story_flag},
    {G, "", "Add_Objective", g_add_objective},
    {G, "", "Add_Radar_Blip", g_add_radar_blip},
    {G, "", "Remove_Radar_Blip", g_remove_radar_blip},
    {G, "", "Add_Planet_Highlight", g_add_planet_highlight},
    {G, "", "Remove_Planet_Highlight", g_remove_planet_highlight},
    {M, "StoryPlotWrapper", "Is_Valid", m_plot_is_valid},
    {M, "StoryPlotWrapper", "Get_Event", m_plot_get_event},
    {M, "StoryPlotWrapper", "Activate", m_plot_activate},
    {M, "StoryPlotWrapper", "Suspend", m_plot_suspend},
    {M, "StoryPlotWrapper", "Reset", m_plot_reset},
    {M, "StoryEventWrapper", "Is_Valid", m_event_is_valid},
    {M, "StoryEventWrapper", "Set_Event_Parameter", m_event_set_event_parameter},
    {M, "StoryEventWrapper", "Set_Reward_Parameter", m_event_set_reward_parameter},
    {M, "StoryEventWrapper", "Set_Reward_Type", m_event_set_reward_type},
    {M, "StoryEventWrapper", "Set_Dialog", m_event_set_dialog},
    {M, "StoryEventWrapper", "Add_Dialog_Text", m_event_add_dialog_text},
    {M, "StoryEventWrapper", "Clear_Dialog_Text", m_event_clear_dialog_text},
};

}  // namespace

std::span<const detail::ImplEntry> detail::impl_story() { return kImpl; }

void attach_story(LuaVm& vm, StoryModel* model) {
    lua_State* L = vm.state();
    lua_pushstring(L, kStoryKey);
    if (model)
        lua_pushlightuserdata(L, model);
    else
        lua_pushnil(L);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

StoryModel* attached_story(LuaVm& vm) { return story(vm.state()); }

}  // namespace alamo::lua
