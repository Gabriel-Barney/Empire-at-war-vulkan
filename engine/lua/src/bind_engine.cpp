// The implemented bindings (L1 stage 1). Everything else in the RE table is an "unbound" stub.
// Contracts and messages follow docs/re/LUA_API.md ("Binding map": the original's own
// argument-validation strings). Implementations follow the error discipline of vm_internal.hpp.
#include <algorithm>
#include <string>
#include <vector>

#include "alamo/core/log.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "vm_internal.hpp"

namespace alamo::lua {

using detail::Call;

namespace {

int push_nil(Call& c) {
    lua_pushnil(c.L);
    return 1;
}
int push_bool(Call& c, bool b) {
    lua_pushboolean(c.L, b ? 1 : 0);
    return 1;
}
int push_num(Call& c, double d) {
    lua_pushnumber(c.L, d);
    return 1;
}

GameRandom* rng(Call& c) { return c.vm->host.rng; }

// ---------------------------------------------------------------- objects and types

int g_find_object_type(Call& c) {
    if (!c.is_string(1)) return c.fail("LuaFindObjectType -- Expected type name for parameter 1");
    const objects::GameObjectTypes* types = c.vm->host.types;
    if (!types) return push_nil(c);
    const objects::TypeIndex t = types->raw().find(c.string(1));
    if (t == objects::kNone) return push_nil(c);
    detail::push_type(c.L, *c.vm, t);
    return 1;
}

// Find_All_Objects_Of_Type: bind_objects.cpp (refixf t03, with Find_First_Object / Find_Nearest).

objects::ObjectHandle self_object(Call& c) { return detail::object_handle(*c.self); }

int m_object_is_valid(Call& c) { return push_bool(c, detail::object_alive(*c.vm, *c.self)); }

int m_object_get_id(Call& c) {
    if (c.self->id == objects::kNoObjectId) return push_nil(c);
    return push_num(c, c.self->id);
}

int m_object_get_type(Call& c) {
    // refix t04: a company member view's type is its Company_Units entry (vm_internal.hpp).
    const uint32_t member = detail::member_view_type(*c.vm, *c.self);
    detail::push_type(c.L, *c.vm, member != objects::kNone ? member : c.vm->host.store->type(self_object(c)));
    return 1;
}

int m_object_get_parent_mode_id(Call& c) {
    const objects::ObjectId id = c.vm->host.store->parent_mode_id(self_object(c));
    if (id == objects::kNoObjectId) return push_nil(c);
    return push_num(c, id);
}

int m_object_get_owner(Call& c) {
    detail::push_player(c.L, *c.vm, c.vm->host.store->owner(self_object(c)));
    return 1;
}

int m_type_get_name(Call& c) {
    const objects::GameObjectTypes* types = c.vm->host.types;
    if (!types || c.self->value >= types->raw().type_count()) return push_nil(c);
    const std::string_view n = types->raw().types().name[c.self->value];
    lua_pushlstring(c.L, n.data(), n.size());
    return 1;
}

int m_true(Call& c) { return push_bool(c, true); }
int m_false(Call& c) { return push_bool(c, false); }

int m_player_get_id(Call& c) { return push_num(c, c.self->id); }

// Flush_G (PGBase.lua) drops userdata globals whose Is_Pool_Safe() is false. PROVISIONAL: wrappers
// of live game state (objects, players) are not pool safe; engine function objects are.
int m_is_pool_safe(Call& c) {
    return push_bool(c, c.self->cls != detail::cls_game_object() && c.self->cls != detail::cls_player());
}

// ---------------------------------------------------------------- randomness

// GameRandom(): int 0..65535; GameRandom(n): 0..n; GameRandom(a, b): [a, b] - all from the
// SYNCHRONIZED stream (docs/re/COMMANDS.md §8; 0x5423a0). The original int helper caps the span.
int g_game_random(Call& c) {
    GameRandom* r = rng(c);
    if (!r) return c.fail("GameRandom -- no game random attached to this script");
    int32_t v = 0;
    if (c.nargs == 0) {
        v = r->synced().range_int(0, 0xFFFF);
    } else if (c.nargs == 1) {
        if (!c.is_number(1)) return c.fail("GameRandom -- Invalid parameter 1, expected a number");
        v = r->synced().range_int(0, detail::to_int(c.number(1)));
    } else {
        if (!c.is_number(1)) return c.fail("GameRandom -- Invalid parameter 1, expected a number");
        if (!c.is_number(2)) return c.fail("GameRandom -- Invalid parameter 2, expected a number");
        v = r->synced().range_int(detail::to_int(c.number(1)), detail::to_int(c.number(2)));
    }
    return push_num(c, v);
}

// GameRandom.Free_Random (0x540770, docs/re/MAIN_LOOP.md §4.1): the no-argument form draws
// int(0, 0xFFFF) from the SYNCHRONIZED state (the quirk); ranged forms use the free state in
// single player and fall back to the synchronized state (with a warning) in multiplayer.
int m_free_random(Call& c) {
    GameRandom* r = rng(c);
    if (!r) return c.fail("GameRandom::Free_Random -- no game random attached to this script");
    const bool mp = c.vm->host.multiplayer;
    if (c.nargs == 0) return push_num(c, r->lua_free_random(mp));
    if (!c.is_number(1)) return c.fail("GameRandom::Free_Random -- Invalid parameter 1, expected a number");
    if (!c.is_number(2)) return c.fail("GameRandom::Free_Random -- Invalid parameter 2, expected a number");
    if (mp && !c.vm->mp_free_random_warned) {
        c.vm->mp_free_random_warned = true;
        log::warnf("lua", "GameRandom::Free_Random -- cannot generate non-synchronized random numbers from script in MP");
    }
    return push_num(c, r->lua_free_random(mp, detail::to_int(c.number(1)), detail::to_int(c.number(2))));
}

// GameRandom.Get_Float([lo, hi]) (0x5461b0): synchronized float helper; no arguments = [0, 1].
int m_get_float(Call& c) {
    GameRandom* r = rng(c);
    if (!r) return c.fail("GameRandom::Get_Float -- no game random attached to this script");
    float lo = 0.0f, hi = 1.0f;
    if (c.nargs >= 1) {
        if (!c.is_number(1)) return c.fail("GameRandom::Get_Float -- Invalid parameter 1, expected a number");
        if (!c.is_number(2)) return c.fail("GameRandom::Get_Float -- Invalid parameter 2, expected a number");
        lo = static_cast<float>(c.number(1));
        hi = static_cast<float>(c.number(2));
    }
    return push_num(c, static_cast<double>(r->synced().range_float(lo, hi)));
}

// ---------------------------------------------------------------- time, threads, events

int g_get_current_time(Call& c) { return push_num(c, c.vm->time); }
int m_time_frame(Call& c) { return push_num(c, c.vm->frame); }
int g_get_thread_id(Call& c) { return push_num(c, c.vm->current_thread); }

// Create_Thread("Function_Name", params...) / Thread.Create(...): returns the new thread id.
int g_create_thread(Call& c) {
    if (!c.has(1)) return c.fail("LuaCreateThread -- Expected a string for function name.");
    int fn_index = c.idx(1);
    std::string name;
    if (c.is_string(1)) {
        name = std::string(c.string(1));
        lua_pushvalue(c.L, c.idx(1));
        lua_rawget(c.L, LUA_GLOBALSINDEX);
        fn_index = lua_gettop(c.L);
    } else if (lua_type(c.L, c.idx(1)) != LUA_TFUNCTION) {
        return c.fail("LuaCreateThread -- Expected a string for function name.");
    } else {
        name = "<function>";
    }
    if (lua_type(c.L, fn_index) != LUA_TFUNCTION)
        return c.fail("LuaCreateThread -- Expected a LuaFunction parameter. (%s is not a global function)", name.c_str());
    const int32_t id = c.vm->start_thread(c.L, fn_index, c.idx(2), c.nargs - 1, std::move(name));
    return push_num(c, id);
}

int32_t thread_arg(Call& c) { return c.is_number(1) ? detail::to_int(c.number(1)) : -1; }

int m_thread_kill(Call& c) {
    if (!c.is_number(1)) return c.fail("LuaCreateThread::Kill -- Expected a thread id.");
    c.vm->kill_thread(thread_arg(c));
    return 0;
}

int m_thread_kill_all(Call& c) {
    for (auto& [id, t] : c.vm->threads) t.killed = true;
    return 0;
}

int m_thread_is_active(Call& c) {
    const auto it = c.vm->threads.find(thread_arg(c));
    return push_bool(c, it != c.vm->threads.end() && !it->second.killed);
}

int m_thread_get_name(Call& c) {
    const auto it = c.vm->threads.find(thread_arg(c));
    if (it == c.vm->threads.end()) return push_nil(c);
    lua_pushlstring(c.L, it->second.function.data(), it->second.function.size());
    return 1;
}

int m_thread_current_id(Call& c) { return push_num(c, c.vm->current_thread); }

// ThreadValue(key) / ThreadValue.Set(key, value) / ThreadValue.Reset(): values private to the
// running thread (-1 = outside any thread). Used by PGBase's Sleep / BlockOnCommand / PumpEvents.
void push_thread_values(Call& c) {
    lua_pushstring(c.L, "alamo.threadvalues");
    lua_rawget(c.L, LUA_REGISTRYINDEX);
    lua_pushnumber(c.L, c.vm->current_thread);
    lua_rawget(c.L, -2);
    if (lua_type(c.L, -1) != LUA_TTABLE) {
        lua_pop(c.L, 1);
        lua_newtable(c.L);
        lua_pushnumber(c.L, c.vm->current_thread);
        lua_pushvalue(c.L, -2);
        lua_rawset(c.L, -4);
    }
    lua_remove(c.L, -2);
}

int g_thread_value(Call& c) {
    if (!c.has(1)) return c.fail("ThreadValue -- Expected a key parameter.");
    push_thread_values(c);
    lua_pushvalue(c.L, c.idx(1));
    lua_rawget(c.L, -2);
    lua_remove(c.L, -2);
    return 1;
}

int m_thread_value_set(Call& c) {
    if (!c.has(1) || lua_isnil(c.L, c.idx(1))) return c.fail("ThreadValue::Set -- Expected a key parameter.");
    push_thread_values(c);
    lua_pushvalue(c.L, c.idx(1));
    if (c.has(2))
        lua_pushvalue(c.L, c.idx(2));
    else
        lua_pushnil(c.L);
    lua_rawset(c.L, -3);
    lua_pop(c.L, 1);
    return 0;
}

int m_thread_value_reset(Call& c) {
    lua_pushstring(c.L, "alamo.threadvalues");
    lua_rawget(c.L, LUA_REGISTRYINDEX);
    lua_pushnumber(c.L, c.vm->current_thread);
    lua_pushnil(c.L);
    lua_rawset(c.L, -3);
    lua_pop(c.L, 1);
    return 0;
}

// GetEvent(): the next queued event function, nil when none; GetEvent.Params(): the parameter table of
// the event GetEvent returned last (nil when it had none) - PGBase's PumpEvents calls
// CurrentEvent(unpack(EventParams)). Sources: LuaVm::post_event (a global function name, no
// parameters) and engine events queued with detail::queue_event (e.g. proximity events: the callback
// with {prox_object, trigger_object}). PROVISIONAL: one FIFO per VM (the original's queue owner and
// order across sources are not traced).
const char kEventQueueKey[] = "alamo.eventq";       // {h = first, t = last, [i] = {fn, params}}
const char kEventParamsKey[] = "alamo.eventparams";  // params of the last event returned

void push_event_queue(lua_State* L) {
    lua_pushstring(L, kEventQueueKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_type(L, -1) == LUA_TTABLE) return;
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushstring(L, "h");
    lua_pushnumber(L, 1);
    lua_rawset(L, -3);
    lua_pushstring(L, "t");
    lua_pushnumber(L, 0);
    lua_rawset(L, -3);
    lua_pushstring(L, kEventQueueKey);
    lua_pushvalue(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

double queue_field(lua_State* L, int q, const char* key) {  // q: absolute index of the queue table
    lua_pushstring(L, key);
    lua_rawget(L, q);
    const double v = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return v;
}
void set_queue_field(lua_State* L, int q, const char* key, double v) {
    lua_pushstring(L, key);
    lua_pushnumber(L, v);
    lua_rawset(L, q);
}

void set_event_params(lua_State* L, int params_index) {  // 0 = nil
    lua_pushstring(L, kEventParamsKey);
    if (params_index)
        lua_pushvalue(L, params_index);
    else
        lua_pushnil(L);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

int g_get_event(Call& c) {
    lua_State* L = c.L;
    push_event_queue(L);
    const int q = lua_gettop(L);
    for (;;) {
        const double h = queue_field(L, q, "h"), t = queue_field(L, q, "t");
        if (h > t) {
            lua_pop(L, 1);
            set_event_params(L, 0);
            return push_nil(c);
        }
        lua_rawgeti(L, q, static_cast<int>(h));  // entry at q + 1
        lua_pushnil(L);
        lua_rawseti(L, q, static_cast<int>(h));
        set_queue_field(L, q, "h", h + 1.0);
        lua_rawgeti(L, q + 1, 1);  // fn or name
        if (lua_type(L, -1) == LUA_TSTRING) {
            const std::string name(lua_tostring(L, -1), lua_strlen(L, -1));
            lua_rawget(L, LUA_GLOBALSINDEX);
            if (lua_type(L, -1) != LUA_TFUNCTION) {
                lua_settop(L, q);
                c.vm->message("[GetEvent] dropped event '" + name + "': not a global function");
                continue;
            }
        }
        lua_rawgeti(L, q + 1, 2);  // params or nil
        set_event_params(L, lua_type(L, -1) == LUA_TTABLE ? lua_gettop(L) : 0);
        lua_pop(L, 1);
        lua_replace(L, q);  // fn in place of the queue
        lua_settop(L, q);
        return 1;
    }
}
int m_get_event_params(Call& c) {
    lua_pushstring(c.L, kEventParamsKey);
    lua_rawget(c.L, LUA_REGISTRYINDEX);
    return 1;
}
int m_get_event_reset(Call& c) {
    lua_pushstring(c.L, kEventQueueKey);
    lua_pushnil(c.L);
    lua_rawset(c.L, LUA_REGISTRYINDEX);
    set_event_params(c.L, 0);
    return 0;
}

// ---------------------------------------------------------------- messages / debug

std::string arg_text(Call& c, int i) {
    if (!c.has(i)) return {};
    if (c.is_string(i) || c.is_number(i)) return std::string(c.string(i));
    return lua_typename(c.L, lua_type(c.L, c.idx(i)));
}

int g_output_debug(Call& c) {
    const std::string s = arg_text(c, 1);
    log::write(log::Level::Debug, "lua", s);
    c.vm->message("[OutputDebug] " + s);
    return 0;
}

int g_script_message(Call& c) {
    const std::string s = arg_text(c, 1);
    log::write(log::Level::Debug, "lua", s);
    c.vm->message("[ScriptMessage] " + s);
    return 0;
}

int g_custom_script_message(Call& c) {
    if (c.nargs != 2) return c.fail("LuaCustomScriptMessage -- Expected 2 parameters, got %d", c.nargs);
    if (!c.is_string(1) || !c.is_string(2)) return c.fail("LuaCustomScriptMessage -- expected two string parameters.");
    c.vm->message("[CustomScriptMessage:" + std::string(c.string(1)) + "] " + std::string(c.string(2)));
    return 0;
}

int g_message_popup(Call& c) {
    const std::string s = arg_text(c, 1);
    log::write(log::Level::Warn, "lua", s);
    c.vm->message("[MessagePopup] " + s);
    return 0;
}

int g_script_exit(Call& c) {
    c.vm->exit_requested = true;
    return 0;
}

int g_debug_break(Call& c) {
    c.vm->message("[DebugBreak]");
    return 0;
}

// DumpCallStack(): the script call stack as text (level 1 = the caller).
int g_dump_call_stack(Call& c) {
    std::string out = "Lua call stack:";
    lua_Debug ar;
    for (int level = 1; lua_getstack(c.L, level, &ar) != 0; ++level) {
        if (lua_getinfo(c.L, "Sln", &ar) == 0) break;
        out += "\n  ";
        out += ar.short_src;
        out += ":" + std::to_string(ar.currentline);
        if (ar.name) {
            out += " in function ";
            out += ar.name;
        }
    }
    lua_pushlstring(c.L, out.data(), out.size());
    return 1;
}

int g_is_multiplayer_mode(Call& c) { return push_bool(c, c.vm->host.multiplayer); }
int g_is_campaign_game(Call& c) { return push_bool(c, c.vm->host.campaign); }  // LuaHost::campaign

// PROVISIONAL strings: the mode kind names (docs/re/OBJECT_RUNTIME.md §7).
int g_get_game_mode(Call& c) {
    if (!c.vm->host.store) return push_nil(c);
    switch (c.vm->host.store->mode()) {
        case objects::ModeKind::Galactic: lua_pushstring(c.L, "Galactic"); break;
        case objects::ModeKind::Land: lua_pushstring(c.L, "Land"); break;
        case objects::ModeKind::Space: lua_pushstring(c.L, "Space"); break;
    }
    return 1;
}

// StringCompare(a, b) [inferred]: true when the two strings are equal ignoring ASCII case, else false
// (GameScoring.lua tests it with `if`, so it is a boolean, not a strcmp-style number); non-strings: false.
int g_string_compare(Call& c) {
    if (c.nargs != 2 || !c.is_string(1) || !c.is_string(2)) return push_bool(c, false);
    const std::string_view a = c.string(1), b = c.string(2);
    if (a.size() != b.size()) return push_bool(c, false);
    for (size_t i = 0; i < a.size(); ++i) {
        const char x = (a[i] >= 'a' && a[i] <= 'z') ? static_cast<char>(a[i] - 'a' + 'A') : a[i];
        const char y = (b[i] >= 'a' && b[i] <= 'z') ? static_cast<char>(b[i] - 'a' + 'A') : b[i];
        if (x != y) return push_bool(c, false);
    }
    return push_bool(c, true);
}

constexpr BindingKind G = BindingKind::Global;
constexpr BindingKind M = BindingKind::Method;

constexpr detail::ImplEntry kImpl[] = {
    {G, "", "StringCompare", g_string_compare},  // L1/S4 wave 1
    // globals
    {G, "", "Find_Object_Type", g_find_object_type},
    {G, "", "GameRandom", g_game_random},
    {G, "", "GetCurrentTime", g_get_current_time},
    {G, "", "GetThreadID", g_get_thread_id},
    {G, "", "Create_Thread", g_create_thread},
    {G, "", "Thread", g_create_thread},
    {G, "", "ThreadValue", g_thread_value},
    {G, "", "GetEvent", g_get_event},
    {G, "", "_OuputDebug", g_output_debug},
    {G, "", "_ScriptMessage", g_script_message},
    {G, "", "_CustomScriptMessage", g_custom_script_message},
    {G, "", "_MessagePopup", g_message_popup},
    {G, "", "_ScriptExit", g_script_exit},
    {G, "", "_DebugBreak", g_debug_break},
    {G, "", "DumpCallStack", g_dump_call_stack},
    {G, "", "Is_Multiplayer_Mode", g_is_multiplayer_mode},
    {G, "", "Is_Campaign_Game", g_is_campaign_game},
    {G, "", "Get_Game_Mode", g_get_game_mode},
    // methods
    {M, "GameObjectWrapper", "Is_Valid", m_object_is_valid},
    {M, "GameObjectWrapper", "Get_Object_ID", m_object_get_id},
    {M, "GameObjectWrapper", "Get_Type", m_object_get_type},
    {M, "GameObjectWrapper", "Get_Parent_Mode_Object_ID", m_object_get_parent_mode_id},
    {M, "GameObjectWrapper", "Get_Owner", m_object_get_owner},
    {M, "GameObjectTypeWrapper", "Get_Name", m_type_get_name},
    {M, "GameObjectTypeWrapper", "Is_Valid", m_true},
    {M, "PlayerWrapper", "Get_ID", m_player_get_id},
    {M, "PlayerWrapper", "Is_Valid", m_true},
    {M, "LuaGameRandom", "Free_Random", m_free_random},
    {M, "LuaGameRandom", "Get_Float", m_get_float},
    {M, "LuaGetTime", "Frame", m_time_frame},
    {M, "LuaCreateThread", "Create", g_create_thread},
    {M, "LuaCreateThread", "Kill", m_thread_kill},
    {M, "LuaCreateThread", "Kill_All", m_thread_kill_all},
    {M, "LuaCreateThread", "Is_Thread_Active", m_thread_is_active},
    {M, "LuaCreateThread", "Get_Name", m_thread_get_name},
    {M, "LuaCreateThread", "Get_Current_ID", m_thread_current_id},
    {M, "ThreadValue", "Reset", m_thread_value_reset},
    {M, "ThreadValue", "Set", m_thread_value_set},  // used by PGBase.lua; not in the RE table
    {M, "GetEvent", "Params", m_get_event_params},
    {M, "GetEvent", "Reset", m_get_event_reset},
    {M, "LuaScriptClass", "Debug_Should_Issue_Event_Alert", m_false},
    {M, "LuaUserVar", "Is_Pool_Safe", m_is_pool_safe},
};

}  // namespace

void detail::queue_event(lua_State* L, int fn_index, int params_index) {
    push_event_queue(L);
    const int q = lua_gettop(L);
    const double t = queue_field(L, q, "t") + 1.0;
    lua_newtable(L);
    lua_pushvalue(L, fn_index);
    lua_rawseti(L, -2, 1);
    if (params_index) {
        lua_pushvalue(L, params_index);
        lua_rawseti(L, -2, 2);
    }
    lua_rawseti(L, q, static_cast<int>(t));
    set_queue_field(L, q, "t", t);
    lua_pop(L, 1);
}

std::span<const detail::ImplEntry> detail::impl_core() { return kImpl; }

}  // namespace alamo::lua
