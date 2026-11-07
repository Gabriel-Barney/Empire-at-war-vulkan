// Binding family: story events and flags, objectives, radar blips, planet highlights (L1 stage 2).
// They act on the StoryModel attached to the VM (attach_story, include/alamo/lua/story_model.hpp);
// without one, queries return nil/false and commands do nothing (a message is logged).
// Contracts and messages: docs/re/LUA_API.md "Binding map" (the original's validation strings).
// Error discipline: vm_internal.hpp (no Lua error raised from here; Call::script_error_stop).
#include <cstdio>
#include <string>

#include "alamo/lua/object_bridge.hpp"
#include "alamo/lua/story_model.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_store.hpp"
#include "vm_internal.hpp"

namespace alamo::lua {

using detail::Call;

namespace {

const char kStoryKey[] = "alamo.story";
const char kStoryGroupKey[] = "alamo.story.group";  // q6 t03: the StoryTable (attach_story_table)
constexpr uint32_t kNone = 0xFFFFFFFFu;

StoryModel* story(lua_State* L) { return detail::registry_ptr<StoryModel>(L, kStoryKey); }
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

// q6 t03 (STORY.md §2.4): the story manager's plot table - every player's plot set of the VM's game mode
// (attach_story_table; nullptr = the VM's own model is the whole table).
StoryTable* story_table(lua_State* L) { return detail::registry_ptr<StoryTable>(L, kStoryGroupKey); }
// The model a plot / event wrapper refers to: Ud::reserved = 0 the VM's own model (attach_story), k > 0 the table's
// model k - 1 (a plot of another player's plot set found by Get_Story_Plot; the same slots as the q6 t03 plot group,
// so saved wrappers keep their meaning). nullptr when that model is gone.
StoryModel* model_of(Call& c, int32_t slot) {
    if (slot == 0) return story(c);
    const StoryTable* t = story_table(c.L);
    return (t && slot > 0) ? t->model(static_cast<size_t>(slot) - 1) : nullptr;
}
StoryModel* self_model(Call& c) { return model_of(c, c.self->reserved); }

void push_plot(Call& c, uint32_t plot, int32_t slot = 0) {
    detail::Ud ud;
    ud.cls = cls_plot();
    ud.id = static_cast<int32_t>(plot);
    ud.reserved = slot;
    // Same plot -> same userdata; key slot * 2^24 + plot (slot 0: the plot index, as before q6 t03).
    detail::push_cached_userdata(c.L, ud, static_cast<lua_Number>(slot) * 16777216.0 + static_cast<lua_Number>(plot));
}

void push_event(Call& c, uint32_t plot, uint32_t event, int32_t slot = 0) {
    detail::Ud ud;
    ud.cls = cls_event();
    ud.id = static_cast<int32_t>(plot);
    ud.value = event;
    ud.reserved = slot;
    // Key (slot * 2^24 + plot) * 2^24 + event: exact in a double for any loadable plot set and fewer than 32 sets.
    const lua_Number p = static_cast<lua_Number>(slot) * 16777216.0 + static_cast<lua_Number>(plot);
    detail::push_cached_userdata(c.L, ud, p * 16777216.0 + static_cast<lua_Number>(event));
}

bool self_plot_valid(Call& c) {
    const StoryModel* m = self_model(c);
    return m && c.self->id >= 0 && static_cast<size_t>(c.self->id) < m->plot_count();
}
bool self_event_valid(Call& c) {
    return self_plot_valid(c) && c.self->value < self_model(c)->plot(static_cast<uint32_t>(c.self->id)).events.size();
}
StoryEventDef& self_event(Call& c) {
    return self_model(c)->plot(static_cast<uint32_t>(c.self->id)).events[c.self->value];
}
StoryEventState& self_event_state(Call& c) {
    return self_model(c)->plot(static_cast<uint32_t>(c.self->id)).state[c.self->value];
}

void no_story(Call& c, const char* what) { c.vm->message(std::string("[story] ") + what + " ignored: no story attached"); }

std::string upper_ascii(std::string_view s) {
    std::string out(s);
    for (char& ch : out) ch = (ch >= 'a' && ch <= 'z') ? static_cast<char>(ch - 'a' + 'A') : ch;
    return out;
}

// `(int)(n + 0.5)` (C truncation, so -1.7 -> -1; saturating) printed with "%d" (LUA_API.md "StoryEventWrapper dialog and
// parameter setters" 2d / the shared converter).
std::string rounded_int_text(double n) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d", static_cast<int>(detail::to_int(n + 0.5)));
    return buf;
}

// The representative type (0x3973b0: company / transport / team container type, else its own) of a live game object
// wrapper: the host's ObjectBridge::representative_type, else the store type. objects::kNone when unknown.
objects::TypeIndex representative_type(Call& c, const detail::Ud& o) {
    if (!c.vm->host.store || !c.vm->host.types) return objects::kNone;
    const objects::ObjectHandle h = detail::object_handle(o);
    objects::TypeIndex t = objects::kNone;
    if (const ObjectBridge* b = c.vm->owner ? attached_object_bridge(*c.vm->owner) : nullptr; b && b->representative_type)
        t = b->representative_type(h);
    if (t == objects::kNone) t = c.vm->host.store->type(h);
    return t < c.vm->host.types->raw().type_count() ? t : objects::kNone;
}

// The game text table (0x140a7bc58, add-if-missing): the text of `id`, a missing id shows as itself. [port] Without a
// table hook (StoryModel::text_lookup, unset in the headless sim) every id is "missing", so it shows verbatim.
std::string story_text(const StoryModel* m, std::string_view id) {
    std::string out;
    if (m && m->text_lookup && m->text_lookup(id, out)) return out;
    return std::string(id);
}

// A type's localized display name (type +0x140, from its Text_ID through the text table). [M] an empty Text_ID gives "".
std::string type_display_name(Call& c, const StoryModel* m, objects::TypeIndex t) {
    const std::string_view id = c.vm->host.types->get_string(t, "Text_ID");
    return id.empty() ? std::string() : story_text(m, id);
}

// ---------------------------------------------------------------- globals

// Story_Event(name [, game object]) - dispatches STORY_AI_NOTIFICATION (docs/re/STORY.md §5): events
// whose Event_Param2 names `name`; with an object, its type name must be in Event_Param1, without
// one Event_Param1 must be empty. q6 t03 (STORY.md §2.4 [H]): to every ACTIVE plot of every player in the mode's table
// (StoryTable::story_event, table order; no player filter, no first-match stop); a dead object goes as "no object".
int g_story_event(Call& c) {
    if (c.nargs < 1 || c.nargs > 2)
        return c.script_error_stop("StoryEvent -- invalid number of parameters. Expected 1 or 2, got %d.", c.nargs);
    if (!c.is_string(1)) return c.script_error_stop("StoryEvent -- invalid type for parameter 1. Expected string");
    const detail::Ud* obj = c.nargs == 2 ? c.userdata(2, detail::cls_game_object()) : nullptr;
    if (c.nargs == 2 && !lua_isnil(c.L, c.idx(2)) && !obj)
        return c.script_error_stop("StoryEvent -- invalid type for parameter 2. Expected game object.");
    StoryModel* m = story(c);
    if (!m) {
        no_story(c, "Story_Event");
        return 0;
    }
    std::string type_name;
    const bool alive = obj && detail::object_alive(*c.vm, *obj);
    if (alive && c.vm->host.types) {
        const objects::TypeIndex t = c.vm->host.store->type(detail::object_handle(*obj));
        if (t < c.vm->host.types->raw().type_count()) type_name = std::string(c.vm->host.types->raw().types().name[t]);
    }
    if (StoryTable* t = story_table(c.L); t && t->set_count() != 0) t->story_event(c.string(1), type_name, alive);
    else m->story_event(c.string(1), type_name, alive);
    return 0;
}

// Get_Story_Plot("File.xml") -> StoryPlotWrapper, or nothing. q6 t03 (STORY.md §2.1 / §2.4 [H]): ONE lookup in the
// mode's table - every player's plots, active or suspended (StoryTable::find_plot; the VM's own model alone without a
// table) - by the CRC-32 of the name as passed: case and path must match the Story_Plots text, no ".xml" is added. The
// table is unique, so at most one plot matches. Not found: warning "Get_Story_Plot -- plot %s not found.", no value.
int g_get_story_plot(Call& c) {
    if (c.nargs != 1) return c.script_error_stop("Get_Story_Plot -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    if (!c.is_string(1)) return c.script_error_stop("Get_Story_Plot -- invalid type for parameter 1. Expected string.");
    const StoryModel* m = story(c);
    const std::string_view name = c.string(1);
    if (const StoryTable* t = story_table(c.L); t && t->set_count() != 0) {
        const StoryTable::PlotRef r = t->find_plot(name);
        if (r.valid()) {
            const StoryModel* owner = t->model(r.set);
            push_plot(c, r.plot, owner == m ? 0 : static_cast<int32_t>(r.set + 1));
            return 1;
        }
    } else if (m) {
        if (const uint32_t p = m->find_plot(name); p != kNone) {
            push_plot(c, p);
            return 1;
        }
    } else {
        no_story(c, "Get_Story_Plot");
        return 0;
    }
    c.vm->message("SCRIPT WARNING: Get_Story_Plot -- plot " + std::string(name) + " not found.");
    return 0;
}

// Check_Story_Flag(player, flag_name, location, reset) -> boolean. [inferred] AI story flags are
// raised by the host (StoryModel::raise_ai_flag) per (player, name, location); reset clears on read.
int g_check_story_flag(Call& c) {
    if (c.nargs != 4)
        return c.script_error_stop("Check_AI_Story_Flag -- invalid number of parameters. Expected between 4, got %d.", c.nargs);
    const detail::Ud* player = c.userdata(1, detail::cls_player());
    if (!player) return c.script_error_stop("Check_AI_Story_Flag -- invalid type for parameter 1. Expected player");
    if (!c.is_string(2)) return c.script_error_stop("Check_AI_Story_Flag -- invalid type for parameter 2. Expected string");
    const detail::Ud* loc = c.userdata(3, detail::cls_game_object());
    const bool reset = lua_toboolean(c.L, c.idx(4)) != 0;
    StoryModel* m = story(c);
    if (!m) return push_bool(c, false);
    return push_bool(c, m->check_ai_flag(player->id, c.string(2), loc ? loc->id : -1, reset));
}

// Add_Objective(text_id, boolean). [inferred] the boolean is not modelled.
int g_add_objective(Call& c) {
    if (c.nargs != 2) return c.script_error_stop("Add_Objective -- invalid number of parameters. Expected 2, got %d.", c.nargs);
    if (!c.is_string(1)) return c.script_error_stop("Add_Objective -- invalid type for parameter 1. Expected string.");
    if (lua_type(c.L, c.idx(2)) != LUA_TBOOLEAN)
        return c.script_error_stop("Add_Objective -- invalid type for parameter 2. Expected boolean.");
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
    if (c.nargs != 2) return c.script_error_stop("Add_Radar_Blip -- invalid number of parameters. Expected 2, got %d.", c.nargs);
    const detail::Ud* o = c.userdata(1, detail::cls_game_object());
    if (!o || !detail::object_alive(*c.vm, *o))
        return c.script_error_stop("Add_Radar_Blip -- could not deduce a position from parameter 1.");
    if (!c.is_string(2)) return c.script_error_stop("Add_Radar_Blip -- invalid type for parameter 2. Expected string.");
    if (StoryModel* m = story(c)) m->add_blip(c.string(2), o->id);
    return 0;
}

int g_remove_radar_blip(Call& c) {
    if (c.nargs != 1) return c.script_error_stop("Remove_Radar_Blip -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    if (!c.is_string(1)) return c.script_error_stop("Remove_Radar_Blip -- invalid type for parameter 1. Expected strng.");
    if (StoryModel* m = story(c)) m->remove_blip(c.string(1));
    return 0;
}

int g_add_planet_highlight(Call& c) {
    if (c.nargs != 2)
        return c.script_error_stop("Add_Planet_Highlight -- invalid number of parameters. Expected 2, got %d.", c.nargs);
    const detail::Ud* o = c.userdata(1, detail::cls_game_object());
    if (!o || !detail::object_alive(*c.vm, *o))
        return c.script_error_stop("Add_Planet_Highlight -- invalid type for parameter 1. Expected game object.");
    if (!c.is_string(2)) return c.script_error_stop("Add_Planet_Highlight -- invalid type for parameter 2. Expected string.");
    if (StoryModel* m = story(c)) m->add_highlight(c.string(2), o->id);
    return 0;
}

int g_remove_planet_highlight(Call& c) {
    if (c.nargs != 1)
        return c.script_error_stop("Remove_Planet_Highlight -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    if (!c.is_string(1)) return c.script_error_stop("Remove_Planet_Highlight -- invalid type for parameter 1. Expected strng.");
    if (StoryModel* m = story(c)) m->remove_highlight(c.string(1));
    return 0;
}

// ---------------------------------------------------------------- StoryPlotWrapper

int m_plot_is_valid(Call& c) { return push_bool(c, self_plot_valid(c)); }

int m_plot_get_event(Call& c) {
    if (c.nargs != 1)
        return c.script_error_stop("StoryPlotWrapper::Get_Event -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    if (!c.is_string(1)) return c.script_error_stop("StoryPlotWrapper::Get_Event -- invalid type for parameter 1. Expected string.");
    if (!self_plot_valid(c)) return push_nil(c);
    const uint32_t plot = static_cast<uint32_t>(c.self->id);
    const uint32_t e = self_model(c)->find_event(plot, c.string(1));
    // q6 t03 (STORY.md §2.4 [H]): only this plot's events; a missing one is a script error (the calling script ends at the
    // end of its service pass), no value.
    if (e == kNone) {
        const std::string event(c.string(1));
        const std::string& plot_name = self_model(c)->plot(plot).name;
        return c.script_error_stop("StoryPlotWrapper::Get_Event -- cannot find event %s in plot %s.", event.c_str(),
                                   plot_name.c_str());
    }
    push_event(c, plot, e, c.self->reserved);
    return 1;
}

int m_plot_activate(Call& c) {
    if (self_plot_valid(c)) self_model(c)->set_plot_active(static_cast<uint32_t>(c.self->id), true);
    return 0;
}
int m_plot_suspend(Call& c) {
    if (self_plot_valid(c)) self_model(c)->set_plot_active(static_cast<uint32_t>(c.self->id), false);
    return 0;
}
int m_plot_reset(Call& c) {
    if (self_plot_valid(c)) self_model(c)->reset_plot(static_cast<uint32_t>(c.self->id));
    return 0;
}

// ---------------------------------------------------------------- StoryEventWrapper

int m_event_is_valid(Call& c) { return push_bool(c, self_event_valid(c)); }

// q6 t03 (LUA_API.md "StoryEventWrapper dialog and parameter setters" [H]). Every method returns silently, before any
// argument check, when the wrapper's event is gone; none returns a value. Errors use the script error model
// (Call::script_error_stop: logged, nothing returned, the calling script ends at the end of its service pass).
//
// Set_Event_Parameter / Set_Reward_Parameter go through the event's vf2 (STORY.md 0.2: "set parameter i (0-based) from a
// list of strings"), the same setter the XML loader uses, so a Lua-set parameter is matched by the event type's own rule
// (name lists, per-name counts, thresholds, comparisons) exactly like the XML text. The index is (int)(i + 0.5), 0-based:
// 0 = Event_Param1 / Reward_Param1. Setting an event parameter restarts the per-name countdowns (re-read from Param2 /
// Param3 at the next evaluation).
//
// The shared value converter 0x73e610: a game object -> its representative type's NAME (upper-cased XML name); a type ->
// its name; a string as is (no text lookup); a number -> (int)(n + 0.5) as "%d"; anything else (nil, boolean, table,
// player, a dead object, a null type) -> nothing, no error. False = nothing produced.
bool convert_value(Call& c, int i, std::string& out) {
    if (!c.has(i)) return false;
    if (const detail::Ud* o = c.userdata(i, detail::cls_game_object())) {
        if (!detail::object_alive(*c.vm, *o)) return false;
        const objects::TypeIndex t = representative_type(c, *o);
        if (t == objects::kNone) return false;
        out = upper_ascii(c.vm->host.types->raw().types().name[t]);
        return true;
    }
    if (const detail::Ud* t = c.userdata(i, detail::cls_type())) {
        if (!c.vm->host.types || t->value >= c.vm->host.types->raw().type_count()) return false;
        out = upper_ascii(c.vm->host.types->raw().types().name[static_cast<size_t>(t->value)]);
        return true;
    }
    const int lt = lua_type(c.L, c.idx(i));
    if (lt == LUA_TSTRING) {
        out = std::string(c.string(i));
        return true;
    }
    if (lt == LUA_TNUMBER) {
        out = rounded_int_text(c.number(i));
        return true;
    }
    return false;
}

constexpr int32_t kMaxParamIndex = 64;  // [port] the parameter slots the port keeps (Event_Param1..64)

StoryParam* param_slot(std::vector<StoryParam>& v, int32_t index) {
    if (index < 0 || index >= kMaxParamIndex) return nullptr;
    if (v.size() <= static_cast<size_t>(index)) v.resize(static_cast<size_t>(index) + 1);
    return &v[static_cast<size_t>(index)];
}

// Set_Event_Parameter(i, value [, value ...]) - 0x73eaa0: 2 or more arguments; every value after the index goes through
// the converter into ONE string list for vf2 (one call can set a whole name list). [port] the list is stored as one
// parameter text, the names joined by a space (the generic list parser's separator); a value the converter drops is left
// out, so the parameter may become empty. The binding does not range-check the index: vf2 ignores indices it has no
// parameter for ([port] 0..63 kept, others ignored silently).
int m_event_set_event_parameter(Call& c) {
    static const char kWho[] = "StoryEventWrapper::Set_Event_Parameter -- ";
    if (!self_event_valid(c)) return 0;
    if (c.nargs < 2) return c.script_error_stop("%sinvalid number of parameters. Expected 2, got %d.", kWho, c.nargs);
    if (!c.is_number(1)) return c.script_error_stop("%sinvalid type for parameter 1. Expected number.", kWho);
    std::string list;
    for (int i = 2; i <= c.nargs; ++i) {
        std::string v;
        if (!convert_value(c, i, v)) continue;
        if (!list.empty()) list += ' ';
        list += v;
    }
    StoryParam* slot = param_slot(self_event(c).event_params, detail::to_int(c.number(1) + 0.5));
    if (!slot) return 0;
    *slot = StoryParam::of_text(list);
    StoryEventState& st = self_event_state(c);
    st.countdowns.clear();
    ++st.lua_edits;
    return 0;
}

// Set_Reward_Parameter(i, value) - 0x73eca0: exactly 2 arguments; the converter must give exactly one string (nil, a
// boolean, a table, a player, a dead object or a null type give "must set exactly one parameter per parameter index.").
// The original does not range-check i ([port] 0..63, else the same error: stricter, which shipped and TR data never hit).
int m_event_set_reward_parameter(Call& c) {
    static const char kWho[] = "StoryEventWrapper::Set_Reward_Parameter -- ";
    if (!self_event_valid(c)) return 0;
    if (c.nargs != 2) return c.script_error_stop("%sinvalid number of parameters. Expected 2, got %d.", kWho, c.nargs);
    if (!c.is_number(1)) return c.script_error_stop("%sinvalid type for parameter 1. Expected number.", kWho);
    std::string v;
    StoryParam* slot = nullptr;
    if (convert_value(c, 2, v)) slot = param_slot(self_event(c).reward_params, detail::to_int(c.number(1) + 0.5));
    if (!slot) return c.script_error_stop("%smust set exactly one parameter per parameter index.", kWho);
    *slot = StoryParam::of_text(v);
    ++self_event_state(c).lua_edits;
    return 0;
}

// Set_Reward_Type(name) - 0x73eef0: exactly 1 string. [port] the reward type is kept as its upper-cased name (the
// original stores the story enum value; an unknown name is reward 0, none: here it reaches the host's reward hook, which
// ignores names it does not know).
int m_event_set_reward_type(Call& c) {
    static const char kWho[] = "StoryEventWrapper::Set_Reward_Type -- ";
    if (!self_event_valid(c)) return 0;
    if (c.nargs != 1) return c.script_error_stop("%sinvalid number of parameters. Expected 1, got %d.", kWho, c.nargs);
    if (!c.is_string(1)) return c.script_error_stop("%sinvalid type for parameter 1. Expected string.", kWho);
    self_event(c).reward_type = upper_ascii(c.string(1));
    ++self_event_state(c).lua_edits;
    return 0;
}

// Set_Dialog(name) - 0x73e9a0: exactly 1 string; it becomes the event's Story_Dialog (+0x230; Fire step 6 adds the
// holocron record). The chapter, the tag and the Add_Dialog_Text list are untouched. The dialog script
// Data\Scripts\Story\<name>.txt loads at the call (0x47d9c0, cached by the exact name): StoryModel::dialog_loads, which
// the presentation host loads.
int m_event_set_dialog(Call& c) {
    static const char kWho[] = "StoryEventWrapper::Set_Dialog -- ";
    if (!self_event_valid(c)) return 0;
    if (c.nargs != 1) return c.script_error_stop("%sinvalid number of parameters. Expected 1, got %d.", kWho, c.nargs);
    if (!c.is_string(1)) return c.script_error_stop("%sinvalid type for parameter 1. Expected string.", kWho);
    self_event(c).story_dialog = std::string(c.string(1));
    self_model(c)->request_dialog_load(c.string(1));
    ++self_event_state(c).lua_edits;
    return 0;
}

// The line printf (0x200c00, a wide vswprintf with both slots as arguments): the first two "%s" take slot 1 and slot 2,
// later ones nothing; "%%" -> "%". [port] any other conversion is kept as written (the original's behaviour for them was
// not checked: a "%d" would print the string pointer).
std::string format_dialog_line(std::string_view fmt, const std::string (&slot)[2]) {
    std::string out;
    out.reserve(fmt.size() + slot[0].size() + slot[1].size());
    size_t next = 0;
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] == '%' && i + 1 < fmt.size() && (fmt[i + 1] == 's' || fmt[i + 1] == '%')) {
            if (fmt[i + 1] == '%') out += '%';
            else if (next < 2) out += slot[next++];
            ++i;
            continue;
        }
        out += fmt[i];
    }
    return out;
}

// Add_Dialog_Text(text_id [, a [, b]]) - 0x73df60: 1 to 3 arguments (4+ -> "invalid number of parameters. Expected 2.",
// a stale message). Argument 1 must be a string. Arguments 2 and 3 fill substitution slots 1 and 2, tested in this order:
// a game object -> the display name of its representative type (a dead object ends the whole call silently: nothing
// added); a type -> its display name (a null type: the same silent end); a string -> its text in the text table (a
// missing id shows as itself); a number -> (int)(n + 0.5) as "%d"; anything else -> error "invalid type for parameter %d."
// with the slot number, the slot stays empty and the line is still added. Line = the text of argument 1 printed with both
// slots, appended to the event's dialog text list (event +0x2c0). Fire and every reset leave the list alone: lines pile
// up until Clear_Dialog_Text (the holocron entry of a fired event points at this list, so it shows later edits too).
int m_event_add_dialog_text(Call& c) {
    static const char kWho[] = "StoryEventWrapper::Add_Dialog_Text -- ";
    if (!self_event_valid(c)) return 0;
    if (c.nargs >= 4) return c.script_error_stop("%sinvalid number of parameters. Expected 2.", kWho);
    if (!c.is_string(1)) return c.script_error_stop("%sinvalid type for parameter 1. Expected string.", kWho);
    const StoryModel* m = self_model(c);
    std::string slot[2];
    for (int i = 2; i <= c.nargs; ++i) {
        std::string& s = slot[i - 2];
        if (const detail::Ud* o = c.userdata(i, detail::cls_game_object())) {
            if (!detail::object_alive(*c.vm, *o)) return 0;
            const objects::TypeIndex t = representative_type(c, *o);
            if (t == objects::kNone) return 0;
            s = type_display_name(c, m, t);
        } else if (const detail::Ud* ty = c.userdata(i, detail::cls_type())) {
            if (!c.vm->host.types || ty->value >= c.vm->host.types->raw().type_count()) return 0;
            s = type_display_name(c, m, static_cast<objects::TypeIndex>(ty->value));
        } else if (lua_type(c.L, c.idx(i)) == LUA_TSTRING) {
            s = story_text(m, c.string(i));
        } else if (lua_type(c.L, c.idx(i)) == LUA_TNUMBER) {
            s = rounded_int_text(c.number(i));
        } else {
            c.script_error_stop("%sinvalid type for parameter %d.", kWho, i - 1);
        }
    }
    self_event(c).dialog.push_back(format_dialog_line(story_text(m, c.string(1)), slot));
    ++self_event_state(c).lua_edits;
    return 0;
}

// Clear_Dialog_Text() - 0x73e4f0: ignores its arguments; empties the event's dialog text list.
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
    detail::set_registry_ptr(vm.state(), kStoryKey, model);
}

StoryModel* attached_story(LuaVm& vm) { return story(vm.state()); }

void attach_story_table(LuaVm& vm, StoryTable* table) {
    detail::set_registry_ptr(vm.state(), kStoryGroupKey, table);
}

}  // namespace alamo::lua
