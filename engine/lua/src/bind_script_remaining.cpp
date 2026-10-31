// Binding family: L1 final - the last unbound globals and methods of the RE binding table (alamo/lua/script_remaining.hpp
// has the list, the host contract and the error discipline). Specs: LUA_API.md (error strings), L1_S4_REMAINING.md,
// AI_TACTICAL.md, AI_GALACTIC.md, TACTICAL_COMBAT.md, OBJECT_TYPES.md, FOG_OF_WAR.md. Port choices are marked [port];
// the docs' [H] / [M] / [L] keep their mark.
//
// Determinism: walks use the store's insertion lists and the task forces' unit lists (host order); picks that need a
// random number draw from the SYNCED GameRandom stream; ties go to the earliest entry; per-object state lives in
// ObjectBridge (ordered by id); the GlobalValue fallback table lives in the VM registry (string keys, lookups only).
// Floating-point: distances in double with dmath::sqrt only where a length is needed (comparisons use squares).
#include <algorithm>
#include <array>
#include <bit>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/core/dmath.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/lua/ai_bridge.hpp"
#include "alamo/lua/ai_tactical_bridge.hpp"
#include "alamo/lua/object_bridge.hpp"
#include "alamo/lua/script_gaps.hpp"
#include "alamo/lua/script_remaining.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "vm_internal.hpp"

#if ALAMO_LUA_PLAYERS
#include "alamo/galactic/campaign.hpp"
#include "alamo/lua/players.hpp"
#endif

namespace alamo::lua {

using detail::Call;
using detail::Ud;
using objects::ObjectHandle;
using objects::ObjectId;
using objects::TypeIndex;

namespace {

constexpr char kGlobalValueStoreKey[] = "alamo.gv.store";   // light userdata: GlobalValueStore*
constexpr char kScriptWrapperHostKey[] = "alamo.script.host";  // light userdata: ScriptWrapperHost*
constexpr char kAiScriptKey[] = "alamo.ai.script";          // bind_ai.cpp: the calling script id

// ---------------------------------------------------------------- small helpers

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
bool is_bool(const Call& c, int i) { return c.has(i) && lua_type(c.L, c.idx(i)) == LUA_TBOOLEAN; }
bool to_bool(const Call& c, int i) { return lua_toboolean(c.L, c.idx(i)) != 0; }
bool is_table(const Call& c, int i) { return c.has(i) && lua_type(c.L, c.idx(i)) == LUA_TTABLE; }

// The original's script error / warning helpers (SCRIPT_GAPS.md conventions): logged, no unwind, nil result.
int vlog(Call& c, const char* prefix, const char* fmt, va_list ap) {
    char buf[512];
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    c.vm->message(std::string(prefix) + buf);
    return 0;
}
int script_error(Call& c, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(c, "SCRIPT ERROR: ", fmt, ap);
    va_end(ap);
    return 0;
}
int script_warning(Call& c, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(c, "SCRIPT WARNING: ", fmt, ap);
    va_end(ap);
    return 0;
}

uint32_t cls_of(const char* name) { return detail::class_id(name); }
uint32_t cls_position() {
    static const uint32_t c = cls_of("PositionWrapper");
    return c;
}
uint32_t cls_target() {
    static const uint32_t c = cls_of("AITargetLocationWrapper");
    return c;
}
uint32_t cls_blocking() {
    static const uint32_t c = cls_of("BlockingStatus");
    return c;
}
uint32_t cls_ai_status() {
    static const uint32_t c = cls_of("AIBlockingStatus");
    return c;
}
uint32_t cls_script_wrapper() {
    static const uint32_t c = cls_of("LuaScriptWrapper");
    return c;
}
bool is_tf_class(uint32_t c) {
    static const uint32_t a = cls_of("TaskForceClass"), b = cls_of("GalacticTaskForceClass"), s = cls_of("SpaceTaskForceClass"),
                          l = cls_of("LandTaskForceClass");
    return c == a || c == b || c == s || c == l;
}

ObjectBridge* bridge(Call& c) { return c.vm->owner ? attached_object_bridge(*c.vm->owner) : nullptr; }
RemainingScriptHooks* hooks(Call& c) {
    ObjectBridge* b = bridge(c);
    return b ? b->remaining : nullptr;
}
AiScriptHost* ai(Call& c) { return c.vm->owner ? ai_host(*c.vm->owner) : nullptr; }
AiTacticalScriptHost* ai_tactical(Call& c) { return c.vm->owner ? ai_tactical_host(*c.vm->owner) : nullptr; }
int32_t script_id(lua_State* L) {
    lua_pushstring(L, kAiScriptKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    const int32_t id = lua_type(L, -1) == LUA_TNUMBER ? detail::to_int(lua_tonumber(L, -1)) : -1;
    lua_pop(L, 1);
    return id;
}
objects::ObjectStore* store(Call& c) { return c.vm->host.store; }
const objects::GameObjectTypes* types(Call& c) { return c.vm->host.types; }
ObjectHandle self_object(Call& c) { return detail::object_handle(*c.self); }
SimRng* synced(Call& c) { return c.vm->host.rng ? &c.vm->host.rng->synced() : nullptr; }

char upper(char ch) { return (ch >= 'a' && ch <= 'z') ? static_cast<char>(ch - 'a' + 'A') : ch; }
bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (upper(a[i]) != upper(b[i])) return false;
    return true;
}

bool valid_type(Call& c, TypeIndex t) {
    const objects::GameObjectTypes* ty = types(c);
    return ty && t != objects::kNone && t < ty->size();
}
std::string_view type_name(Call& c, TypeIndex t) {
    const objects::GameObjectTypes* ty = types(c);
    if (!ty || t >= ty->raw().type_count()) return {};
    return ty->raw().types().name[t];
}
std::string object_name(Call& c, ObjectHandle h) { return std::string(type_name(c, store(c)->type(h))); }

// A type argument: a type name or a GameObjectTypeWrapper (kNone when unknown).
TypeIndex type_arg(Call& c, int i, std::string& name) {
    if (const Ud* t = c.userdata(i, detail::cls_type())) {
        name = std::string(type_name(c, static_cast<TypeIndex>(t->value)));
        return static_cast<TypeIndex>(t->value);
    }
    if (!c.is_string(i)) return objects::kNone;
    name = std::string(c.string(i));
    return types(c) ? types(c)->raw().find(name) : objects::kNone;
}
int push_type(Call& c, TypeIndex t) {
    if (!valid_type(c, t)) return push_nil(c);
    detail::push_type(c.L, *c.vm, t);
    return 1;
}
int push_object(Call& c, ObjectHandle h) {
    if (!h || !store(c)->alive(h)) return push_nil(c);
    detail::push_object(c.L, *c.vm, h);
    return 1;
}

// PositionWrapper layout of bind_objects.cpp (x / y float bits in Ud::value, z float bits in Ud::id).
void push_position(lua_State* L, Vec3 p) {
    Ud u;
    u.cls = cls_position();
    u.value = static_cast<uint64_t>(std::bit_cast<uint32_t>(p.x)) | (static_cast<uint64_t>(std::bit_cast<uint32_t>(p.y)) << 32);
    u.id = std::bit_cast<int32_t>(p.z);
    detail::push_engine_userdata(L, u);
}
Vec3 position_of(const Ud& u) {
    return {std::bit_cast<float>(static_cast<uint32_t>(u.value & 0xFFFFFFFFu)), std::bit_cast<float>(static_cast<uint32_t>(u.value >> 32)),
            std::bit_cast<float>(u.id)};
}

// The live units of a task force (the AI host's list order).
std::vector<ObjectHandle> tf_units(Call& c, int32_t tf) {
    std::vector<ObjectHandle> out;
    AiScriptHost* h = ai(c);
    if (!h) return out;
    for (const ObjectHandle u : h->tf_units(tf))
        if (u && store(c)->alive(u)) out.push_back(u);
    return out;
}

// "Something which defines a position": a live game object, a PositionWrapper, an AI target with an object, or a task
// force (its first live unit [port]). `object` receives the object when there is one.
bool position_arg(Call& c, int i, Vec3& out, ObjectHandle* object = nullptr) {
    if (!c.has(i) || lua_type(c.L, c.idx(i)) != LUA_TUSERDATA) return false;
    const auto* ud = static_cast<const Ud*>(lua_touserdata(c.L, c.idx(i)));
    if (!ud) return false;
    ObjectHandle h;
    if (ud->cls == detail::cls_game_object()) {
        if (detail::object_alive(*c.vm, *ud)) h = detail::object_handle(*ud);
    } else if (ud->cls == cls_position()) {
        out = position_of(*ud);
        return true;
    } else if (ud->cls == cls_target()) {
        if (AiScriptHost* a = ai(c)) h = a->target_object(ud->id);
    } else if (is_tf_class(ud->cls)) {
        const std::vector<ObjectHandle> units = tf_units(c, ud->id);
        if (!units.empty()) h = units.front();
    }
    if (!h || !store(c)->alive(h)) return false;
    out = store(c)->position(h);
    if (object) *object = h;
    return true;
}

// Any wrapper argument -> AiArg (bind_ai.cpp's rule).
AiArg ai_arg(Call& c, int i) {
    AiArg a;
    if (!c.has(i) || lua_type(c.L, c.idx(i)) != LUA_TUSERDATA) return a;
    const auto* ud = static_cast<const Ud*>(lua_touserdata(c.L, c.idx(i)));
    if (!ud) return a;
    if (ud->cls == detail::cls_game_object()) {
        if (detail::object_alive(*c.vm, *ud)) {
            a.present = true;
            a.object = detail::object_handle(*ud);
        }
    } else if (ud->cls == cls_target()) {
        a.present = true;
        a.target = ud->id;
    } else if (is_tf_class(ud->cls)) {
        a.present = true;
        a.task_force = ud->id;
    } else if (ud->cls == detail::cls_player()) {
        a.present = true;
        a.player = ud->id;
    }
    return a;
}
// An "object or AI target" argument -> its object (null when the AI target has none or the object is dead).
ObjectHandle object_or_target(Call& c, const AiArg& a) {
    if (a.object) return a.object;
    if (a.target >= 0)
        if (AiScriptHost* h = ai(c)) {
            const ObjectHandle o = h->target_object(a.target);
            if (o && store(c)->alive(o)) return o;
        }
    return {};
}

double dist_sq(Vec3 a, Vec3 b) {
    const double dx = static_cast<double>(a.x) - static_cast<double>(b.x);
    const double dy = static_cast<double>(a.y) - static_cast<double>(b.y);
    const double dz = static_cast<double>(a.z) - static_cast<double>(b.z);
    return dx * dx + dy * dy + dz * dz;
}

RemainingObjectState* edit_state(Call& c, ObjectId id) {
    ObjectBridge* b = bridge(c);
    return b ? &b->remaining_state(id) : nullptr;
}

// A BlockingStatus that is finished at once (serial 0, bind_objects.cpp) [port: the host accepted a fire-and-forget order].
int push_finished_block(Call& c) {
    Ud u;
    u.cls = cls_blocking();
    detail::push_engine_userdata(c.L, u);
    return 1;
}
int push_ai_status(Call& c, int32_t status) {
    if (status < 0) return push_nil(c);
    Ud u;
    u.cls = cls_ai_status();
    u.id = status;
    detail::push_engine_userdata(c.L, u);
    return 1;
}

using detail::call_object_method;  // vm_internal.hpp; defined below (shared with bind_ai.cpp)

// ---------------------------------------------------------------- space fields (data fallback)

struct FieldHit {
    ObjectHandle h;
    Vec3 centre{};
    float radius = 0.0f;
    uint32_t kind = 0;
};
// The space-field objects of the mode (insertion order) matching `mask` [port: FOG_OF_WAR.md dense-mask rule].
std::vector<FieldHit> space_fields(Call& c, uint32_t mask) {
    std::vector<FieldHit> out;
    const objects::ObjectStore* st = store(c);
    const objects::GameObjectTypes* ty = types(c);
    if (!st || !ty) return out;
    static const objects::BehaviorTypeId kObstacle = objects::behavior_type_id("SPACE_OBSTACLE");
    for (const ObjectHandle h : st->all_objects()) {
        if (!st->alive(h)) continue;
        const TypeIndex t = st->type(h);
        if (t >= ty->size()) continue;
        uint32_t kind = 0;
        if (ty->get_bool(t, "Is_Asteroid_Field")) kind |= kFieldAsteroid;  // §1.6: Is_Impassable_Asteroid ignored
        if (ty->get_bool(t, "Is_Ion_Storm")) kind |= kFieldIonStorm;
        if (ty->get_bool(t, "Is_Nebula")) kind |= kFieldNebula;
        if ((kind & mask) == 0) continue;
        if (kObstacle != objects::kNoBehavior && !st->has_behavior(h, kObstacle)) continue;
        const Vec3 p = st->position(h);
        const std::array<float, 4> off = ty->get_vec(t, "Space_Obstacle_Offset");
        out.push_back({h, {p.x + off[0], p.y + off[1], p.z}, ty->get_float(t, "Space_Obstacle_Radius"), kind});
    }
    return out;
}
double xy_dist_sq(Vec3 a, Vec3 b) {
    const double dx = static_cast<double>(a.x) - static_cast<double>(b.x);
    const double dy = static_cast<double>(a.y) - static_cast<double>(b.y);
    return dx * dx + dy * dy;
}
bool point_in_field(Call& c, Vec3 p, uint32_t mask) {
    if (RemainingScriptHooks* hk = hooks(c); hk && hk->point_in_field) return hk->point_in_field(p, mask);
    for (const FieldHit& f : space_fields(c, mask)) {
        const double r = static_cast<double>(f.radius);
        if (r > 0.0 && xy_dist_sq(p, f.centre) <= r * r) return true;
    }
    return false;
}
ObjectHandle nearest_field(Call& c, Vec3 p, uint32_t mask) {
    if (RemainingScriptHooks* hk = hooks(c); hk && hk->nearest_field) return hk->nearest_field(p, mask);
    ObjectHandle best;
    double best_d = 0.0;
    for (const FieldHit& f : space_fields(c, mask)) {
        const double d = xy_dist_sq(p, f.centre);
        if (!best || d < best_d) {
            best = f.h;
            best_d = d;
        }
    }
    return best;
}

// ================================================================ globals

// The singleton objects' call (Budget, DiscreteDistribution, WeightedTypeList, SFXManager, GlobalValue): nothing
// [port: LuaUserVar's default Function_Call].
int g_noop(Call&) { return 0; }
// SFXManager() returns the object itself (§1.7).
int g_sfx_manager(Call& c) {
    lua_pushvalue(c.L, 1);
    return 1;
}

int g_point_in(Call& c, uint32_t mask, const char* name) {
    if (c.nargs != 1) return script_error(c, "%s -- invalid number of parameters. Expected 1, got %d.", name, c.nargs);
    Vec3 p{};
    if (!position_arg(c, 1, p)) return script_error(c, "%s -- could not extract a position from parameter 1.", name);
    return push_bool(c, point_in_field(c, p, mask));
}
int g_is_point_in_asteroid_field(Call& c) { return g_point_in(c, kFieldAsteroid, "Is_Point_In_Asteroid_Field"); }
int g_is_point_in_ion_storm(Call& c) { return g_point_in(c, kFieldIonStorm, "Is_Point_In_Ion_Storm"); }
int g_is_point_in_nebula(Call& c) { return g_point_in(c, kFieldNebula, "Is_Point_In_Nebula"); }

int g_find_nearest_space_field(Call& c) {
    constexpr const char* kName = "Find_Nearest_Space_Field";
    if (!store(c) || store(c)->mode() != objects::ModeKind::Space)
        return script_error(c, "%s -- may only be used in space mode", kName);  // checked first (§1.6)
    if (c.nargs != 1 && c.nargs != 2)
        return script_error(c, "%s -- invalid number of paraneters. Expected 1 or 21, got %d.", kName, c.nargs);  // [H] text
    Vec3 p{};
    if (!position_arg(c, 1, p)) return script_error(c, "%s -- could not deduce source position from parameter 1", kName);
    uint32_t mask = kFieldAsteroid | kFieldIonStorm | kFieldNebula;  // default 0x1c (§1.6)
    if (c.nargs == 2) {
        if (!c.is_string(2)) return script_error(c, "%s -- invalid type for parameter 2. Expected string.", kName);
        mask = parse_space_field_mask(c.string(2));
        if (mask == 0) return script_error(c, "%s -- unknown field type %s", kName, std::string(c.string(2)).c_str());
    }
    const ObjectHandle h = nearest_field(c, p, mask);
    if (!h || !store(c)->alive(h)) return 0;  // nothing
    detail::push_object(c.L, *c.vm, h);
    return 1;
}

int g_are_on_opposite_sides_of_shield(Call& c) {
    constexpr const char* kName = "Are_On_Opposite_Sides_Of_Shield";
    if (c.nargs != 2 && c.nargs != 4)
        return script_error(c, "%s -- invalid number of parameters. Expected 2 or 4, got %d.", kName, c.nargs);
    Vec3 a{}, b{};
    if (!position_arg(c, 1, a)) return script_error(c, "%s -- could not extract a position from parameter 1.", kName);
    if (!position_arg(c, 2, b)) return script_error(c, "%s -- could not extract a position from parameter 2.", kName);
    int32_t player = -1;
    bool friendly = false;
    if (c.nargs == 4) {
        const Ud* pl = c.userdata(3, detail::cls_player());
        if (!pl) return script_error(c, "%s -- invalid type for parameter 3. Expected player.", kName);
        if (!is_bool(c, 4)) return script_error(c, "%s -- invalid type for parameter 4. Expected boolean.", kName);
        player = pl->id;
        friendly = to_bool(c, 4);
    }
    // §1.2: needs a land mode in the stack [port: the VM's store is that land mode, or its parent store is one].
    const objects::ObjectStore* ps = detail::parent_store(*c.vm);
    if (store(c)->mode() != objects::ModeKind::Land && !(ps && ps->mode() == objects::ModeKind::Land))
        return script_error(c, "%s -- only valid in land mode", kName);
    RemainingScriptHooks* hk = hooks(c);
    if (hk && c.nargs == 4 && hk->opposite_sides_of_shield4) return push_bool(c, hk->opposite_sides_of_shield4(a, b, player, friendly));
    return push_bool(c, hk && hk->opposite_sides_of_shield && hk->opposite_sides_of_shield(a, b, player));
}

int g_force_weather(Call& c) {
    RemainingScriptHooks* hk = hooks(c);
    if (!(hk && hk->force_weather)) return script_warning(c, "Force_Weather -- no weather system in this mode");
    hk->force_weather();
    return 0;
}

// Find_Best_Local_Threat_Center(objects, radius) (L1_S4_REMAINING §1.1 [H]):
//   * entries that are not objects: warning, skipped; dead objects skipped; the list sorted by object id;
//     empty -> warning "no objects supplied", nothing;
//   * k = min(10, ceil(n / 4)) centres: k <= 1 -> the mean 2D position, no draw; else per centre (index order) two
//     synced draws y = float(minY, maxY) then x = float(minX, maxX), then 5 Lloyd iterations (nearest centre by 2D
//     squared distance, strict <; empty centres keep their position);
//   * score = sum of AI_Combat_Power of the objects within `radius` (2D, compared in double) [port: the type's value,
//     no squad-fill scaling]; best = highest, ties to the later centre; best <= 0 -> nothing;
//   * returns Position(x, y, 0) and the score.
int g_find_best_local_threat_center(Call& c) {
    constexpr const char* kName = "Find_Best_Local_Threat_Center";
    if (c.nargs != 2) return script_error(c, "%s -- invalid number of parameters. Expected 2, got %d.", kName, c.nargs);
    if (!is_table(c, 1)) return script_error(c, "%s -- invalid type for parameter 1. Expected table.", kName);
    if (!c.is_number(2)) return script_error(c, "%s -- invalid type for parameter 2. Expected number.", kName);
    const double radius = c.number(2);
    std::vector<ObjectHandle> units;
    const int t = c.idx(1);
    const int n = luaL_getn(c.L, t);
    for (int k = 1; k <= n; ++k) {
        lua_rawgeti(c.L, t, k);
        const auto* ud = lua_type(c.L, -1) == LUA_TUSERDATA ? static_cast<const Ud*>(lua_touserdata(c.L, -1)) : nullptr;
        if (ud && ud->cls == detail::cls_game_object()) {
            if (detail::object_alive(*c.vm, *ud)) units.push_back(detail::object_handle(*ud));
        } else {
            script_warning(c, "%s -- invalid type for entry at index %d in object table.", kName, k);
        }
        lua_pop(c.L, 1);
    }
    const objects::ObjectStore* st = store(c);
    if (units.empty()) return script_warning(c, "%s -- no objects supplied", kName);
    std::stable_sort(units.begin(), units.end(), [st](ObjectHandle a, ObjectHandle b) { return st->id(a) < st->id(b); });
    const size_t count = units.size();
    std::vector<double> px(count), py(count), power(count, 0.0);
    const objects::GameObjectTypes* ty = types(c);
    for (size_t i = 0; i < count; ++i) {
        const Vec3 p = st->position(units[i]);
        px[i] = p.x;
        py[i] = p.y;
        const TypeIndex ti = st->type(units[i]);
        if (ty && ti < ty->size()) power[i] = static_cast<double>(ty->get_float(ti, "AI_Combat_Power"));
    }
    const size_t k = std::min<size_t>(10, (count + 3) / 4);
    std::vector<double> cx, cy;
    if (k <= 1) {
        double sx = 0.0, sy = 0.0;
        for (size_t i = 0; i < count; ++i) {
            sx += px[i];
            sy += py[i];
        }
        cx.push_back(sx / static_cast<double>(count));
        cy.push_back(sy / static_cast<double>(count));
    } else {
        float min_x = static_cast<float>(px[0]), max_x = min_x, min_y = static_cast<float>(py[0]), max_y = min_y;
        for (size_t i = 1; i < count; ++i) {
            min_x = std::min(min_x, static_cast<float>(px[i]));
            max_x = std::max(max_x, static_cast<float>(px[i]));
            min_y = std::min(min_y, static_cast<float>(py[i]));
            max_y = std::max(max_y, static_cast<float>(py[i]));
        }
        SimRng* r = synced(c);
        for (size_t j = 0; j < k; ++j) {
            const float y = r ? r->range_float(min_y, max_y) : min_y;
            const float x = r ? r->range_float(min_x, max_x) : min_x;
            cx.push_back(x);
            cy.push_back(y);
        }
        std::vector<size_t> owner(count, 0);
        for (int iter = 0; iter < 5; ++iter) {
            for (size_t i = 0; i < count; ++i) {
                size_t best = 0;
                double best_d = 0.0;
                for (size_t j = 0; j < k; ++j) {
                    const double dx = px[i] - cx[j], dy = py[i] - cy[j];
                    const double d = dx * dx + dy * dy;
                    if (j == 0 || d < best_d) {
                        best = j;
                        best_d = d;
                    }
                }
                owner[i] = best;
            }
            for (size_t j = 0; j < k; ++j) {
                double sx = 0.0, sy = 0.0, m = 0.0;
                for (size_t i = 0; i < count; ++i)
                    if (owner[i] == j) {
                        sx += px[i];
                        sy += py[i];
                        m += 1.0;
                    }
                if (m > 0.0) {
                    cx[j] = sx / m;
                    cy[j] = sy / m;
                }
            }
        }
    }
    const double r2 = radius * radius;
    double best_score = 0.0;
    size_t best = cx.size();
    for (size_t j = 0; j < cx.size(); ++j) {
        double s = 0.0;
        for (size_t i = 0; i < count; ++i) {
            const double dx = px[i] - cx[j], dy = py[i] - cy[j];
            if (dx * dx + dy * dy <= r2) s += power[i];
        }
        if (best == cx.size() || s >= best_score) {
            best = j;
            best_score = s;
        }
    }
    if (best == cx.size() || best_score <= 0.0) return 0;
    push_position(c.L, {static_cast<float>(cx[best]), static_cast<float>(cy[best]), 0.0f});
    lua_pushnumber(c.L, best_score);
    return 2;
}

// _ProduceObject(player, type, planet) (§1.9): a wrong argument count is logged and the call goes on; missing
// arguments -> "Invalid parameter, Player: Ok/Invalid, ..." and nothing. The galactic production check + the
// Production event on the direct path = GalacticSim::enqueue (the players family's sim) [port: the plan Budget's
// reservation 0x6bd600 is not modelled]. Refused -> "Scheduling Production Event Failed!", nothing. Queued -> a
// BlockingStatus [port: finished at once; the original ProductionBlockStatus 0x696830 follows the build].
int g_produce_object(Call& c) {
    if (c.nargs != 3) script_error(c, "ProduceObjectClass::Function_Call -- Invalid Number of parameters: %d should be 3.", c.nargs);
    const Ud* player = c.userdata(1, detail::cls_player());
    std::string tname;
    const TypeIndex t = type_arg(c, 2, tname);
    const Ud* planet = c.userdata(3, detail::cls_game_object());
    const bool planet_ok = planet && detail::object_alive(*c.vm, *planet);
    if (!player || !valid_type(c, t) || !planet_ok)
        return script_error(c, "ProduceObjectClass::Function_Call -- Invalid parameter, Player: %s, Object: %s, Planet: %s",
                            player ? "Ok" : "Invalid", valid_type(c, t) ? "Ok" : "Invalid", planet_ok ? "Ok" : "Invalid");
#if ALAMO_LUA_PLAYERS
    LuaPlayerWorld* w = c.vm->owner ? player_world(*c.vm->owner) : nullptr;
    galactic::GalacticSim* sim = w ? w->galactic() : nullptr;
    if (sim && sim->enqueue(player->id, sim->planet_of(detail::object_handle(*planet)), t) == commands::RejectReason::None)
        return push_finished_block(c);
#endif
    return script_error(c, "ProduceObjectClass::Function_Call -- Scheduling Production Event Failed!");
}

// ================================================================ GlobalValue (GlobalValueClass extras, §1.5)

void push_value(lua_State* L, const AiGlobalValue& v, int depth = 0) {
    switch (v.kind) {
        case AiGlobalValue::Number: lua_pushnumber(L, v.number); break;
        case AiGlobalValue::String: lua_pushlstring(L, v.text.data(), v.text.size()); break;
        case AiGlobalValue::Boolean: lua_pushboolean(L, v.number != 0.0 ? 1 : 0); break;
        case AiGlobalValue::Table:
            lua_newtable(L);
            if (depth < 16)
                for (size_t i = 0; i < v.keys.size() && i < v.values.size(); ++i) {
                    push_value(L, v.keys[i], depth + 1);
                    push_value(L, v.values[i], depth + 1);
                    if (lua_isnil(L, -2)) lua_pop(L, 2);
                    else lua_rawset(L, -3);
                }
            break;
        default: lua_pushnil(L); break;
    }
}
// A storable value: nil, number, string, boolean, or a table of those (keys number / string / boolean; depth 16).
// False for userdata (LuaUserVar), functions and threads, anywhere in the value.
bool value_at(lua_State* L, int i, AiGlobalValue& v, int depth = 0) {
    if (i < 0) i = lua_gettop(L) + i + 1;
    v = AiGlobalValue{};
    switch (lua_type(L, i)) {
        case LUA_TNIL: return true;
        case LUA_TNUMBER:
            v.kind = AiGlobalValue::Number;
            v.number = lua_tonumber(L, i);
            return true;
        case LUA_TSTRING:
            v.kind = AiGlobalValue::String;
            v.text.assign(lua_tostring(L, i), lua_strlen(L, i));
            return true;
        case LUA_TBOOLEAN:
            v.kind = AiGlobalValue::Boolean;
            v.number = lua_toboolean(L, i) ? 1.0 : 0.0;
            return true;
        case LUA_TTABLE: {
            if (depth >= 16) return false;
            v.kind = AiGlobalValue::Table;
            lua_pushnil(L);
            while (lua_next(L, i) != 0) {
                AiGlobalValue key, val;
                if (!value_at(L, -2, key, depth + 1) || !value_at(L, -1, val, depth + 1) || key.kind == AiGlobalValue::Table) {
                    lua_pop(L, 2);
                    return false;
                }
                v.keys.push_back(std::move(key));
                v.values.push_back(std::move(val));
                lua_pop(L, 1);
            }
            return true;
        }
        default: return false;
    }
}
GlobalValueStore* gv_store(lua_State* L) {
    lua_pushstring(L, kGlobalValueStoreKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    void* p = lua_type(L, -1) == LUA_TLIGHTUSERDATA ? lua_touserdata(L, -1) : nullptr;
    lua_pop(L, 1);
    return static_cast<GlobalValueStore*>(p);
}
// GlobalValue.Get(name): exactly 1 string; the stored value, nothing when unknown.
int m_gv_get(Call& c) {
    if (c.nargs != 1) return script_error(c, "GlobalValue::Get -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    if (!c.is_string(1)) return script_error(c, "GlobalValue::Get -- invalid type for parameter 1. Expected string.");
    const std::string_view name = c.string(1);
    AiGlobalValue v;
    if (GlobalValueStore* s = gv_store(c.L)) v = s->get(name);
    else if (AiScriptHost* h = ai(c)) v = h->global_value(name);
    else v = process_global_values().get(name);
    if (v.kind == AiGlobalValue::Nil) return 0;
    push_value(c.L, v);
    return 1;
}
// GlobalValue.Set(name, value): exactly 2 args, name a string; userdata refused; nil erases.
int m_gv_set(Call& c) {
    if (c.nargs != 2) return script_error(c, "GlobalValue::Set -- invalid number of parameters. Expected 2, got %d.", c.nargs);
    if (!c.is_string(1)) return script_error(c, "GlobalValue::Set -- invalid type for parameter 1. Expected string.");
    const std::string_view name = c.string(1);
    AiGlobalValue v;
    if (!value_at(c.L, c.idx(2), v)) return script_error(c, "GlobalValue::Set -- value cannot be a Lua User Variable");
    if (GlobalValueStore* s = gv_store(c.L)) s->set(name, v);
    else if (AiScriptHost* h = ai(c)) h->set_global_value(name, v);
    else process_global_values().set(name, v);
    return 0;
}

// ================================================================ GameObjectWrapper

#if ALAMO_LUA_PLAYERS
LuaPlayerWorld* players(Call& c) { return c.vm->owner ? player_world(*c.vm->owner) : nullptr; }
#endif
// The faction name of a player (players world), "" when unknown.
std::string faction_of(Call& c, int32_t player) {
#if ALAMO_LUA_PLAYERS
    LuaPlayerWorld* w = players(c);
    if (w && w->valid(player)) return w->faction_name(player);
#else
    (void)c;
    (void)player;
#endif
    return {};
}
// Affiliation (the type's Affiliation list) contains `faction` (case-insensitive).
bool affiliated(Call& c, TypeIndex t, std::string_view faction) {
    const objects::GameObjectTypes* ty = types(c);
    if (!ty || faction.empty() || !valid_type(c, t)) return false;
    for (const std::string_view a : ty->get_list(t, "Affiliation"))
        if (iequal(a, faction)) return true;
    return iequal(ty->get_string(t, "Affiliation"), faction);
}

bool has_beh(Call& c, ObjectHandle h, const char* name) {
    const objects::BehaviorTypeId id = objects::behavior_type_id(name);
    return id != objects::kNoBehavior && store(c)->has_behavior(h, id);
}

// ---- build pads (§2.2, §3.6) ----

// The pad's contents [port without a host]: the recorded under-construction object, else the first live object
// attached to / contained by the pad (insertion order).
ObjectHandle pad_contents(Call& c, ObjectHandle pad) {
    if (RemainingScriptHooks* hk = hooks(c); hk && hk->build_pad_contents) return hk->build_pad_contents(pad);
    const objects::ObjectStore* st = store(c);
    if (const ObjectBridge* b = bridge(c))
        if (const RemainingObjectState* s = b->find_remaining_state(st->id(pad)); s && s->pad_uc != objects::kNoObjectId) {
            const ObjectHandle uc = st->find(s->pad_uc);
            if (uc && st->alive(uc)) return uc;
        }
    for (const ObjectHandle h : st->all_objects())
        if (st->alive(h) && h != pad && (st->attach_parent(h) == pad || st->container(h) == pad)) return h;
    return {};
}

// The pad's buildable list for `faction` (Tactical_Buildable_Objects_Multiplayer in skirmish, else _Campaign [port:
// the VM's store decides "skirmish" = no parent mode]): rows "Faction, Type, Type, ..." (a faction name starts a row).
bool pad_lists(Call& c, TypeIndex pad_type, std::string_view faction, TypeIndex t) {
    const objects::GameObjectTypes* ty = types(c);
    if (!ty || !valid_type(c, pad_type)) return false;
    const bool skirmish = detail::parent_store(*c.vm) == nullptr;
    const char* tag = skirmish ? "Tactical_Buildable_Objects_Multiplayer" : "Tactical_Buildable_Objects_Campaign";
    const std::string_view name = type_name(c, t);
    bool in_row = false;
    for (const std::string_view tok : ty->get_list(pad_type, tag)) {
        if (ty->raw().find(tok) == objects::kNone) {  // not a type: a faction name opens a row
            in_row = iequal(tok, faction);
            continue;
        }
        if (in_row && iequal(tok, name)) return true;
    }
    return false;
}
// Build rule 0x42e400 (§3.6): the pad is empty and no blocking object (alive, Influences_Capture_Point, not allied with
// the pad owner) lies within Capture_Point_Radius (3D) of the pad.
bool pad_rule(Call& c, ObjectHandle pad) {
    objects::ObjectStore* st = store(c);
    const objects::GameObjectTypes* ty = types(c);
    if (pad_contents(c, pad)) return false;
    const TypeIndex pt = st->type(pad);
    if (!ty || pt >= ty->size()) return true;
    const double r = static_cast<double>(ty->get_float(pt, "Capture_Point_Radius"));
    const Vec3 p = st->position(pad);
    const int32_t owner = st->owner(pad);
    ObjectBridge* b = bridge(c);
    for (const ObjectHandle h : st->all_objects()) {
        if (!st->alive(h) || h == pad) continue;
        const TypeIndex t = st->type(h);
        if (t >= ty->size() || !ty->get_bool(t, "Influences_Capture_Point")) continue;
        const int32_t o = st->owner(h);
        const bool enemy = (b && b->is_enemy) ? b->is_enemy(owner, o) : (o != owner && o >= 0 && owner >= 0);
        if (enemy && dist_sq(p, st->position(h)) <= r * r) return false;
    }
    return true;
}

// Build(type [, check_rules]) on a build pad (§2.2).
int m_build(Call& c) {
    constexpr const char* kName = "GameObjectWrapper::Build";
    if (c.nargs != 1 && c.nargs != 2) return script_error(c, "%s -- invalid number of parameters. Expected 1 or 2, got %d.", kName, c.nargs);
    std::string name;
    const TypeIndex t = type_arg(c, 1, name);
    if (!valid_type(c, t)) {
        if (c.is_string(1)) return script_error(c, "%s -- unknown game object type %s", kName, name.c_str());
        return script_error(c, "%s -- invalid type for parameter 1. Expected game object type.", kName);
    }
    const ObjectHandle pad = self_object(c);
    if (!has_beh(c, pad, "TACTICAL_BUILD_OBJECTS")) return script_error(c, "%s -- %s is not a valid build location", kName, object_name(c, pad).c_str());
    if (pad_contents(c, pad)) return script_error(c, "%s -- %s already has something built on it", kName, object_name(c, pad).c_str());
    bool check = false;
    if (c.nargs == 2) {
        if (!is_bool(c, 2)) return script_error(c, "%s -- invalid type for parameter 2. Expected boolean.", kName);
        check = to_bool(c, 2);
    }
    objects::ObjectStore* st = store(c);
    if (RemainingScriptHooks* hk = hooks(c); hk && hk->build) {
        const int r = hk->build(pad, t, check);
        if (r == 0) {
            script_warning(c, "%s -- game rules prevent building %s at %s", kName, name.c_str(), object_name(c, pad).c_str());
            return push_bool(c, false);
        }
        if (r < 0) return script_error(c, "%s -- unknown error starting build of %s", kName, name.c_str());
        return push_bool(c, true);
    }
    if (check && !(pad_lists(c, st->type(pad), faction_of(c, st->owner(pad)), t) && pad_rule(c, pad))) {
        script_warning(c, "%s -- game rules prevent building %s at %s", kName, name.c_str(), object_name(c, pad).c_str());
        return push_bool(c, false);
    }
    // Start build 0x42dbd0 (charge 0, sfx 0) [port]: the type at the pad position with the pad's facing, owned by the
    // pad owner, recorded as the pad's under-construction object (construction is not serviced without a host).
    objects::SpawnParams sp;
    sp.type = t;
    sp.owner = st->owner(pad);
    sp.position = st->position(pad);
    sp.facing = st->facing(pad);
    ObjectBridge* b = bridge(c);
    TacticalScriptHooks* th = b ? b->tactical : nullptr;
    const ObjectHandle uc = (th && th->spawn) ? th->spawn(sp) : st->create(sp);
    if (!uc || !st->alive(uc)) return script_error(c, "%s -- unknown error starting build of %s", kName, name.c_str());
    if (RemainingObjectState* s = edit_state(c, st->id(pad))) s->pad_uc = st->id(uc);
    return push_bool(c, true);
}

int m_get_build_pad_contents(Call& c) {
    const ObjectHandle pad = self_object(c);
    if (!has_beh(c, pad, "TACTICAL_BUILD_OBJECTS"))
        return script_warning(c, "GameObjectWrapper::Get_Build_Pad_Contents -- object %s is not a build pad", object_name(c, pad).c_str());
    const ObjectHandle h = pad_contents(c, pad);
    if (!h || !store(c)->alive(h)) return 0;
    detail::push_object(c.L, *c.vm, h);
    return 1;
}
int m_lock_build_pad_contents(Call& c) {
    constexpr const char* kName = "GameObjectWrapper::Lock_Build_Pad_Contents";
    if (c.nargs != 1) return script_error(c, "%s -- invalid number of parameters. Expected 1, got %d.", kName, c.nargs);
    if (!is_bool(c, 1)) return script_error(c, "%s -- invalid type for parameter 1. Expected boolean", kName);
    const ObjectHandle pad = self_object(c);
    if (!has_beh(c, pad, "TACTICAL_BUILD_OBJECTS")) return script_error(c, "%s -- %s is not a build pad", kName, object_name(c, pad).c_str());
    if (!pad_contents(c, pad)) return script_warning(c, "%s -- cannot lock an empty build pad", kName);
    const bool lock = to_bool(c, 1);
    if (RemainingObjectState* s = edit_state(c, c.self->id)) s->build_pad_locked = lock;
    if (RemainingScriptHooks* hk = hooks(c); hk && hk->lock_build_pad) hk->lock_build_pad(pad, lock);
    return 0;
}

// Can_Garrison_Fire(): the S4 garrison system, else the object's Property_Flags GarrisonCanFire [port].
int m_can_garrison_fire(Call& c) {
    if (RemainingScriptHooks* hk = hooks(c); hk && hk->can_garrison_fire) return push_bool(c, hk->can_garrison_fire(self_object(c)));
    const objects::GameObjectTypes* ty = types(c);
    const TypeIndex t = store(c)->type(self_object(c));
    if (!ty || t >= ty->size()) return push_bool(c, false);
    const auto m = ty->enums().get(objects::EnumId::GameObjectProperties).convert("GarrisonCanFire");
    return push_bool(c, m && *m != 0 && (ty->get_mask(t, "Property_Flags") & static_cast<uint64_t>(*m)) != 0);
}

// ---- diversion (§2.3) ----

bool has_locomotor(Call& c, ObjectHandle h) { return store(c)->has_behavior(h, objects::beh::Locomotor); }

int m_divert(Call& c) {
    constexpr const char* kName = "GameObjectWrapper::Divert";
    const ObjectHandle self = self_object(c);
    if (!has_locomotor(c, self)) return script_error(c, "%s -- this object cannot move", kName);
    RemainingScriptHooks* hk = hooks(c);
    if (!(hk && hk->move_to)) {  // no movement host: the formation is missing ("between moves")
        script_warning(c, "%s -- unable to divert - object %s is between moves", kName, object_name(c, self).c_str());
        return push_bool(c, false);
    }
    if (c.nargs != 1 && c.nargs != 2) return script_error(c, "%s -- invalid number of parameters. Expected 1 or 2, got %d.", kName, c.nargs);
    // Movement locked (0x39ad40) [port: Lock_Current_Orders / Suspend_Locomotor of the script state].
    if (const ObjectBridge* b = bridge(c))
        if (const ScriptObjectState* s = b->find_state(c.self->id); s && (s->orders_locked || s->locomotor_suspended)) {
            script_warning(c, "%s -- %s has had its movement locked", kName, object_name(c, self).c_str());
            return push_bool(c, false);
        }
    Vec3 p{};
    if (!position_arg(c, 1, p)) return script_error(c, "%s -- invalid type for parameter 1; expected something which defines a position", kName);
    if (c.nargs == 2 && !c.is_number(2)) return script_error(c, "%s -- invalid type for parameter 2; expected number.", kName);
    // [port] the distance limit (move params +0x54) is not modelled by the movement hosts.
    if (!hk->move_to(self, p)) {
        script_warning(c, "%s -- unable to divert - object %s is between moves", kName, object_name(c, self).c_str());
        return push_bool(c, false);
    }
    if (RemainingObjectState* s = edit_state(c, c.self->id)) {
        Vec3 g{};
        s->diverted = !hk->move_goal || hk->move_goal(self, g);
        s->divert_goal = s->diverted && hk->move_goal ? g : p;
    }
    return push_bool(c, true);
}
// Is_On_Diversion(): the current (last) move carries the diversion flag 0x40 - this port: the goal recorded by Divert
// is still the unit's current goal.
int m_is_on_diversion(Call& c) {
    const ObjectHandle self = self_object(c);
    if (!has_locomotor(c, self)) return script_error(c, "GameObjectWrapper::Is_On_Diversion -- this object cannot move");
    ObjectBridge* b = bridge(c);
    const RemainingObjectState* s = b ? b->find_remaining_state(c.self->id) : nullptr;
    if (!s || !s->diverted) return push_bool(c, false);
    RemainingScriptHooks* hk = hooks(c);
    Vec3 g{};
    const bool on = hk && hk->move_goal && hk->move_goal(self, g) && g.x == s->divert_goal.x && g.y == s->divert_goal.y &&
                    g.z == s->divert_goal.z;
    if (!on)
        if (RemainingObjectState* e = edit_state(c, c.self->id)) e->diverted = false;  // replaced or finished
    return push_bool(c, on);
}

int m_enable_behavior(Call& c) {
    constexpr const char* kName = "GameObjectWrapper::Enable_Behavior";
    if (c.nargs != 2) return script_error(c, "%s -- invalid number of parameters. Expected 2, got %d.", kName, c.nargs);
    if (!c.is_number(1)) return script_error(c, "%s -- invalid type for behavior. Expected number.", kName);
    const int32_t id = detail::to_int(c.number(1));
    if (id < 0 || id > 0x69) return script_error(c, "%s -- invalid behavior %d", kName, id);
    if (!is_bool(c, 2)) return script_error(c, "%s -- invalid type for state. Expected a bool.", kName);
    const bool on = to_bool(c, 2);
    // A missing behaviour: silently nothing (§2.6).
    if (!store(c)->has_behavior(self_object(c), static_cast<objects::BehaviorTypeId>(id))) return 0;
    if (RemainingObjectState* s = edit_state(c, c.self->id)) s->behaviors[id] = on;
    if (RemainingScriptHooks* hk = hooks(c); hk && hk->enable_behavior) hk->enable_behavior(self_object(c), id, on);
    return 0;
}

int m_enable_dynamic_lod(Call& c) {
    if (c.nargs != 1) return script_error(c, "GameObjectWrapper::Enable_Dynamic_LOD -- command takes a single boolean value");
    if (!is_bool(c, 1)) return script_error(c, "GameObjectWrapper::Enable_Dynamic_LOD -- invalid type for parameter 1. Expected boolean.");
    if (RemainingObjectState* s = edit_state(c, c.self->id)) s->dynamic_lod = to_bool(c, 1);
    return 0;
}

int m_explore_area(Call& c) {
    if (c.nargs != 1)
        return script_error(c, "GameObjectWrapper::Explore_Area -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    const Ud* t = c.userdata(1, cls_target());
    if (!t) return script_error(c, "GameObjectWrapper::Explore_Area -- invalid type for parameter 1. Expected ai target location.");
    AiScriptHost* h = ai(c);
    if (h && !h->target_valid(t->id)) return script_error(c, "GameObjectWrapper::Explore_Area -- target is dead");
    return push_ai_status(c, h ? h->unit_explore_area(script_id(c.L), self_object(c), t->id) : -1);
}

// Force_Test_Space_Conflict() (§2.12): fleets and planets only; a planet tests its first orbiting fleet. A started
// conflict returns a BlockingStatus [port: finished at once; the original waits for signal 0x12 on the planet].
int m_force_test_space_conflict(Call& c) {
    constexpr const char* kName = "GameObjectWrapper::Force_Test_Space_Conflict";
    ObjectHandle self = self_object(c);
    const bool fleet = has_beh(c, self, "FLEET"), planet = has_beh(c, self, "PLANET");
    if (!fleet && !planet) return script_error(c, "%s -- %s is not a fleet or a planet", kName, object_name(c, self).c_str());
#if ALAMO_LUA_PLAYERS
    LuaPlayerWorld* w = players(c);
    galactic::GalacticSim* sim = w ? w->galactic() : nullptr;
    if (planet && sim) {
        const PlanetIndex pi = sim->planet_of(self);
        self = {};
        if (pi != kInvalidPlanet)
            for (const uint32_t o : sim->planets()[pi].orbit)
                if (sim->fleets()[o].alive) {
                    self = sim->fleets()[o].object;
                    break;
                }
        if (!self) return 0;
    }
#endif
    RemainingScriptHooks* hk = hooks(c);
    if (hk && hk->test_space_conflict) return hk->test_space_conflict(self) ? push_finished_block(c) : 0;
#if ALAMO_LUA_PLAYERS
    // The arrival test [port]: a stationary fleet whose planet holds a fleet hostile to its owner -> the conflict hooks.
    if (sim && sim->conflict_hooks()) {
        const uint32_t f = sim->fleet_of(self);
        if (f != galactic::kNoFleet) {
            const galactic::FleetState& fs = sim->fleets()[f];
            if (!fs.moving && fs.planet != kInvalidPlanet)
                for (const uint32_t o : sim->planets()[fs.planet].orbit) {
                    const galactic::FleetState& other = sim->fleets()[o];
                    if (other.alive && sim->is_hostile(fs.owner, other.owner)) {
                        sim->conflict_hooks()->on_space_conflict(fs.planet, fs.owner);
                        return push_finished_block(c);
                    }
                }
        }
    }
#endif
    return 0;
}

// AI combat power (AI_Combat_Power) and health fraction of an object.
double type_power(Call& c, ObjectHandle u) {
    const objects::GameObjectTypes* ty = types(c);
    const TypeIndex t = store(c)->type(u);
    return (ty && t < ty->size()) ? static_cast<double>(ty->get_float(t, "AI_Combat_Power")) : 0.0;
}
double health_fraction(Call& c, ObjectHandle u) {
    const objects::GameObjectTypes* ty = types(c);
    const TypeIndex t = store(c)->type(u);
    const float max = (ty && t < ty->columns().tactical_health.size()) ? ty->columns().tactical_health[t] : 0.0f;
    if (!(max > 0.0f)) return 1.0;
    return static_cast<double>(store(c)->health(u)) / static_cast<double>(max);
}
double power_weight(Call& c, ObjectHandle u, ObjectHandle other) {
    if (RemainingScriptHooks* hk = hooks(c); hk && hk->ai_power_vs) {
        const double v = hk->ai_power_vs(u, other);
        if (v >= 0.0) return v;
    }
    return 1.0;  // [port] no power-versus table
}
// Get_AI_Power_Vs_Unit(target) (§2.12 [M]): P_s x H_s x M_s - P_t x H_t x M_t.
int m_get_ai_power_vs_unit(Call& c) {
    if (c.nargs != 1) return script_error(c, "Get_AI_Power_Vs_Unit -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    const AiArg other = ai_arg(c, 1);
    if (!other.present || (!other.object && other.target < 0))
        return script_error(c, "Get_AI_Power_Vs_Unit -- invalid type for parameter 1. Expected object or ai target.");
    const ObjectHandle self = self_object(c), target = object_or_target(c, other);
    if (!target) return script_error(c, "Get_AI_Power_Vs_Unit -- target is dead");
    return push_num(c, type_power(c, self) * health_fraction(c, self) * power_weight(c, self, target) -
                           type_power(c, target) * health_fraction(c, target) * power_weight(c, target, self));
}

// Get_Affiliated_Indigenous_Type(player) (§2.12): planets only; over the planet type's Potential_Indigenous_Power
// entries ("Type, power"), the highest power (strict >) whose type is affiliated with the faction of a player allied
// with the argument (the argument itself [port] without a relation table).
int m_get_affiliated_indigenous_type(Call& c) {
    constexpr const char* kName = "GameObjectWrapper::Get_Affiliated_Indigenous_Type";
    const ObjectHandle self = self_object(c);
    if (!has_beh(c, self, "PLANET")) return script_error(c, "%s -- %s is not a planet", kName, object_name(c, self).c_str());
    if (c.nargs != 1) return script_error(c, "%s -- invalid number of parameters. Expected 1, got %d.", kName, c.nargs);
    const Ud* player = c.userdata(1, detail::cls_player());
    if (!player) return script_error(c, "%s -- invalid type for parameter 1. Expected player.", kName);
    const objects::GameObjectTypes* ty = types(c);
    const TypeIndex t = store(c)->type(self);
    if (!ty || t >= ty->size()) return 0;
    std::vector<std::string> factions;
#if ALAMO_LUA_PLAYERS
    if (LuaPlayerWorld* w = players(c))
        for (int32_t p = 0; w->valid(p); ++p)
            if (p == player->id || w->relation(player->id, p) == PlayerRelation::Ally) factions.push_back(w->faction_name(p));
#endif
    TypeIndex best = objects::kNone;
    int64_t best_power = 0;
    const std::span<const std::string_view> list = ty->get_list(t, "Potential_Indigenous_Power");
    for (size_t i = 0; i + 1 < list.size(); i += 2) {
        const TypeIndex u = ty->raw().find(list[i]);
        const int64_t power = std::atoll(std::string(list[i + 1]).c_str());
        bool ok = false;
        for (const std::string& f : factions) ok = ok || affiliated(c, u, f);
        if (ok && valid_type(c, u) && (best == objects::kNone || power > best_power)) {
            best = u;
            best_power = power;
        }
    }
    return best == objects::kNone ? 0 : push_type(c, best);
}

// Projectile types (§2.1): the combat hook (hard points first, then the type's list; duplicates kept), else [port]
// the type's Projectile_Types (hard-point data is not in GameObjectTypes).
std::vector<TypeIndex> projectile_types(Call& c, ObjectHandle u) {
    std::vector<TypeIndex> out;
    if (RemainingScriptHooks* hk = hooks(c); hk && hk->projectile_types) {
        for (const TypeIndex p : hk->projectile_types(u))
            if (valid_type(c, p)) out.push_back(p);
        return out;
    }
    const objects::GameObjectTypes* ty = types(c);
    const TypeIndex t = store(c)->type(u);
    if (!ty || t >= ty->size()) return out;
    for (const std::string_view p : ty->get_list(t, "Projectile_Types"))
        if (valid_type(c, ty->raw().find(p))) out.push_back(ty->raw().find(p));
    return out;
}
int m_get_all_projectile_types(Call& c) {
    const std::vector<TypeIndex> v = projectile_types(c, self_object(c));
    lua_newtable(c.L);
    int n = 0;
    for (const TypeIndex t : v) {
        detail::push_type(c.L, *c.vm, t);
        lua_rawseti(c.L, -2, ++n);
    }
    return 1;
}
// Get_Current_Projectile_Type() (§2.1): the current weapon index into the type's projectile list (the combat hook);
// [port] without one, index 0. Out of range -> nothing.
int m_get_current_projectile_type(Call& c) {
    if (RemainingScriptHooks* hk = hooks(c); hk && hk->current_projectile) {
        const TypeIndex t = hk->current_projectile(self_object(c));
        return t == objects::kNone ? 0 : push_type(c, t);
    }
    const objects::GameObjectTypes* ty = types(c);
    const TypeIndex t = store(c)->type(self_object(c));
    if (!ty || t >= ty->size()) return 0;
    const std::span<const std::string_view> list = ty->get_list(t, "Projectile_Types");
    if (list.empty()) return 0;
    const TypeIndex p = ty->raw().find(list.front());
    return valid_type(c, p) ? push_type(c, p) : 0;
}

// Get_Energy() (§2.12): tactical only; the energy fraction clamp(energy / max, 0, 1), 0 without a pool.
int m_get_energy(Call& c) {
    if (store(c)->mode() == objects::ModeKind::Galactic)
        return script_error(c, "GameObjectWrapper::Get_Energy -- only valid in tactical mode");
    const ObjectHandle self = self_object(c);
    RemainingScriptHooks* hk = hooks(c);
    if (hk && hk->energy_fraction) {
        const float f = hk->energy_fraction(self);
        if (f >= 0.0f) return push_num(c, std::clamp(f, 0.0f, 1.0f));
    }
    const objects::GameObjectTypes* ty = types(c);
    const TypeIndex t = store(c)->type(self);
    const float cap = (ty && t < ty->size()) ? ty->get_float(t, "Energy_Capacity") : 0.0f;
    if (!(cap > 0.0f)) return push_num(c, 0.0);
    if (hk && hk->energy) {
        const float e = hk->energy(self);
        if (e >= 0.0f) return push_num(c, std::clamp(e / cap, 0.0f, 1.0f));
    }
    return push_num(c, 1.0);  // [port] a full pool
}

// The neutral player id: the galactic sim's, else the first player whose faction is neutral; -1 when unknown.
int32_t neutral_player(Call& c) {
#if ALAMO_LUA_PLAYERS
    if (LuaPlayerWorld* w = players(c)) {
        if (galactic::GalacticSim* sim = w->galactic()) return sim->neutral_player();
        for (int32_t p = 0; w->valid(p); ++p)
            if (w->is_neutral(p)) return p;
    }
#else
    (void)c;
#endif
    return -1;
}
// Get_Final_Blow_Player() (§2.4): obj +0x330 (the hook), -1 -> the neutral player; nothing without such a player.
int m_get_final_blow_player(Call& c) {
    RemainingScriptHooks* hk = hooks(c);
    int32_t p = (hk && hk->final_blow_player) ? hk->final_blow_player(self_object(c)) : -1;
    if (p < 0) p = neutral_player(c);
    if (p < 0) return 0;
    detail::push_player(c.L, *c.vm, p);
    return 1;
}

int m_get_rate_of_damage_taken(Call& c) {
    RemainingScriptHooks* hk = hooks(c);
    const double v = (hk && hk->rate_of_damage) ? hk->rate_of_damage(self_object(c)) : -1.0;
    return push_num(c, v >= 0.0 ? v : 0.0);  // no tracker: 0 (§2.4)
}
int m_get_time_till_dead(Call& c) {
    RemainingScriptHooks* hk = hooks(c);
    const double v = (hk && hk->time_till_dead) ? hk->time_till_dead(self_object(c)) : -1.0;
    return push_num(c, v >= 0.0 ? v : 1.0e18);  // no tracker / rate 0: 1e18 (§2.4)
}

// Is_In_* (§2.7): a TEAM container is in when any member is (recursive, members = objects it contains, store order);
// else the host's per-object answer (victim-behaviour stamps), else the point test at its position.
bool object_in_field(Call& c, ObjectHandle h, uint32_t mask, int depth) {
    const objects::ObjectStore* st = store(c);
    if (st->has_behavior(h, objects::beh::Team) && depth < 8) {
        for (const ObjectHandle m : st->all_objects())
            if (st->alive(m) && st->container(m) == h && object_in_field(c, m, mask, depth + 1)) return true;
        return false;
    }
    if (RemainingScriptHooks* hk = hooks(c); hk && hk->object_in_field) {
        const int32_t v = hk->object_in_field(h, mask);
        if (v >= 0) return v != 0;
    }
    return point_in_field(c, st->position(h), mask);
}
int m_is_in_field(Call& c, uint32_t mask) { return push_bool(c, object_in_field(c, self_object(c), mask, 0)); }
int m_is_in_asteroid_field(Call& c) { return m_is_in_field(c, kFieldAsteroid); }
int m_is_in_ion_storm(Call& c) { return m_is_in_field(c, kFieldIonStorm); }
int m_is_in_nebula(Call& c) { return m_is_in_field(c, kFieldNebula); }

// Release() (§2.12): the wrapper drops its object (it becomes dead) and leaves the script's wrapper cache. Returns
// nothing.
int m_release(Call& c) {
    lua_State* L = c.L;
    lua_pushstring(L, detail::store_tag_of(*c.self) == detail::kParentStoreTag ? "alamo.objcache.parent" : "alamo.objcache");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_type(L, -1) == LUA_TTABLE) {
        const lua_Number key = detail::object_cache_key(*c.self);  // refix t04: member views have their own key
        lua_pushnumber(L, key);
        lua_rawget(L, -2);
        const bool same = lua_touserdata(L, -1) == static_cast<const void*>(c.self);
        lua_pop(L, 1);
        if (same) {
            lua_pushnumber(L, key);
            lua_pushnil(L);
            lua_rawset(L, -3);
        }
    }
    lua_pop(L, 1);
    Ud* ud = const_cast<Ud*>(c.self);  // the userdata block is the script's own mutable memory
    ud->value = 0;
    ud->id = objects::kNoObjectId;
    return 0;
}

int m_set_prefer_ground_over_space(Call& c) {
    constexpr const char* kName = "GameObjectWrapper::Lua_Set_Prefer_Ground_Over_Space";
    if (!is_bool(c, 1)) return script_error(c, "%s -- Missing required boolean parameter.", kName);
    static const objects::BehaviorTypeId kUnitAi = objects::behavior_type_id("UNIT_AI");
    if (kUnitAi != objects::kNoBehavior && !store(c)->has_behavior(self_object(c), kUnitAi))
        return script_error(c, "%s -- Object invalid or missing Unit_AI behavior.", kName);
    const bool on = to_bool(c, 1);
    if (RemainingObjectState* s = edit_state(c, c.self->id)) s->prefer_ground = on ? 1 : 0;
    if (RemainingScriptHooks* hk = hooks(c); hk && hk->set_prefer_ground) hk->set_prefer_ground(self_object(c), on);
    return 0;
}

void set_stickiness(Call& c, ObjectHandle u, float seconds) {
    if (RemainingObjectState* s = edit_state(c, store(c)->id(u))) s->stickiness = seconds;
    if (RemainingScriptHooks* hk = hooks(c); hk && hk->set_stickiness) hk->set_stickiness(u, seconds);
}
int m_set_targeting_stickiness(Call& c) {
    constexpr const char* kName = "GameObjectWrapper::Set_Targeting_Stickiness_Time_Threshold";
    if (c.nargs != 1) return script_error(c, "%s - invalid number of parameters. Expected 1, got %d.", kName, c.nargs);
    if (!c.is_number(1)) return script_error(c, "%s - invalid type for parameter 1. Expected number.", kName);
    set_stickiness(c, self_object(c), static_cast<float>(c.number(1)));
    return 0;
}

// ================================================================ GameObjectTypeWrapper

// Get_Bribe_Cost([briber]) (§2.8): Tactical_Bribe_Cost; with a battle planet in the parent galactic mode, x the
// briber type's Bribe_Modifier_On_Uncorrupted_Planets / _On_Corrupted_Planets.
int m_type_get_bribe_cost(Call& c) {
    const auto t = static_cast<TypeIndex>(c.self->value);
    if (!valid_type(c, t)) return push_num(c, 0.0);
    const double base = static_cast<double>(types(c)->get_int(t, "Tactical_Bribe_Cost"));
    RemainingScriptHooks* hk = hooks(c);
    const int corrupted = (hk && hk->battle_planet_corruption) ? hk->battle_planet_corruption() : -1;
    if (c.nargs == 0 || corrupted < 0) return push_num(c, base);
    const Ud* o = c.userdata(1, detail::cls_game_object());
    if (!o) return script_error(c, "GameObjectTypeWrapper::Get_Bribe_Cost -- invalid type for parameter 1. Expected game object wrapper.");
    if (!detail::object_alive(*c.vm, *o)) return script_error(c, "GameObjectTypeWrapper::Get_Bribe_Cost -- bribing object is dead");
    const TypeIndex bt = store(c)->type(detail::object_handle(*o));
    if (!valid_type(c, bt)) return push_num(c, base);
    const double f = static_cast<double>(
        types(c)->get_float(bt, corrupted ? "Bribe_Modifier_On_Corrupted_Planets" : "Bribe_Modifier_On_Uncorrupted_Planets"));
    return push_num(c, base * f);
}
// Is_Obsolete(player) (§2.8; STRATEGIC_GAPS §5.1 [H]): the type is in the player's LOCKED list (+0x1C8, the list
// Is_Build_Locked / buildability step 8 read; writers LOCK_UNIT, Lua Lock_Tech, the super-weapon hack abilities - all
// GalacticSim::lock_type). No automatic obsolescence. Without a sim: the world's Lock_Tech overrides.
int m_type_is_obsolete(Call& c) {
    if (c.nargs != 1)
        return script_error(c, "GameObjectTypeWrapper::Is_Obsolete -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    const Ud* player = c.userdata(1, detail::cls_player());
    if (!player) return script_error(c, "GameObjectTypeWrapper::Is_Obsolete -- invalid type for parameter 1. Expected player");
    const auto t = static_cast<TypeIndex>(c.self->value);
    if (RemainingScriptHooks* hk = hooks(c); hk && hk->type_obsolete) return push_bool(c, hk->type_obsolete(t, player->id));
#if ALAMO_LUA_PLAYERS
    if (LuaPlayerWorld* w = players(c); w && w->valid(player->id)) {
        if (const galactic::GalacticSim* sim = w->galactic()) return push_bool(c, sim->type_locked(player->id, t));
        return push_bool(c, w->tech_lock(player->id, t) == 1);
    }
#endif
    return push_bool(c, false);
}

// ================================================================ PlayerWrapper

int m_get_gamespy_stats_player_id(Call& c) { return push_num(c, -1.0); }  // [port] no GameSpy stats id (player +0x398)

// Give_Random_Sliceable_Tech() (§2.9).
int m_give_random_sliceable_tech(Call& c) {
    const int32_t player = c.self->id;
    if (RemainingScriptHooks* hk = hooks(c); hk && hk->give_random_sliceable_tech) {
        const TypeIndex t = hk->give_random_sliceable_tech(player);
        return t == objects::kNone ? 0 : push_type(c, t);
    }
#if ALAMO_LUA_PLAYERS
    LuaPlayerWorld* w = players(c);
    const objects::GameObjectTypes* ty = types(c);
    if (!w || !ty || !w->valid(player)) return 0;
    constexpr int32_t kMaxTechLevel = 5;  // without a sim; with one, 0x282190 (GalacticSim::raise_tech_level) uses P +0x88
    const std::string faction = w->faction_name(player);
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::vector<TypeIndex> cand;
        std::vector<float> cumulative;
        float total = 0.0f;
        for (TypeIndex t = 0; t < ty->size(); ++t) {
            if (!affiliated(c, t, faction) || ty->get_int(t, "Tech_Level") > w->tech_level(player)) continue;
            if (!ty->get_bool(t, "Build_Initially_Locked") || !ty->get_bool(t, "Build_Can_Be_Unlocked_By_Slicer")) continue;
            if (w->tech_lock(player, t) == 0) continue;  // already in the unlocked list
            if (const galactic::GalacticSim* gs = w->galactic(); gs && gs->type_unlocked(player, t)) continue;  // OI lua
            const float cost = ty->get_float(t, "Slice_Cost_Credits");
            if (!(cost > 0.0f)) continue;  // w = 1 / cost must be > 0
            total += 1.0f / cost;
            cand.push_back(t);
            cumulative.push_back(total);
        }
        if (cand.empty()) {
            if (w->tech_level(player) == 0 && attempt == 0) {  // raise and retry
                w->set_tech_level(player, 1);
                continue;
            }
            return 0;
        }
        if (!(total > 0.0f)) return 0;
        SimRng* r = synced(c);
        const float x = r ? r->range_float(0.0f, total) : 0.0f;
        const size_t i = static_cast<size_t>(std::lower_bound(cumulative.begin(), cumulative.end(), x) - cumulative.begin());
        const TypeIndex pick = cand[std::min(i, cand.size() - 1)];
        if (cand.size() == 1) {
            if (galactic::GalacticSim* gs = w->galactic()) gs->raise_tech_level(player);
            else if (w->tech_level(player) < kMaxTechLevel) w->set_tech_level(player, w->tech_level(player) + 1);
        } else {
            w->states()[static_cast<size_t>(player)].tech_override[pick] = false;
            // OI lua: the sim's unlocked list (P +0x1B0, read by build_locked when initial build locks are on).
            if (galactic::GalacticSim* gs = w->galactic()) gs->unlock_type(player, static_cast<uint32_t>(pick));
        }
        return push_type(c, pick);
    }
#endif
    return 0;
}

// ================================================================ task forces

int m_tf_add_force(Call& c) {
    const Ud* o = c.userdata(1, detail::cls_game_object());
    if (!o || !detail::object_alive(*c.vm, *o)) return c.fail("TaskForceClass::Add_Force -- Invalid Parameter 1, expected a GameObjectClass");
    if (AiScriptHost* h = ai(c)) h->tf_add_unit(c.self->id, detail::object_handle(*o));
    return 0;  // returns nothing (§2.12)
}

// HardPointType names (index = value; the weapon types are 5..10).
constexpr std::string_view kHardPointTypes[] = {
    "HARD_POINT_ENGINE",          "HARD_POINT_SHIELD_GENERATOR", "HARD_POINT_GRAVITY_WELL",       "HARD_POINT_FIGHTER_BAY",
    "HARD_POINT_TRACTOR_BEAM",    "HARD_POINT_WEAPON_LASER",     "HARD_POINT_WEAPON_MISSILE",     "HARD_POINT_WEAPON_TORPEDO",
    "HARD_POINT_WEAPON_ION_CANNON", "HARD_POINT_WEAPON_MASS_DRIVER", "HARD_POINT_WEAPON_SPECIAL", "HARD_POINT_ENABLE_SPECIAL_ABILITY",
    "HARD_POINT_DUMMY_ART"};
// Add_ / Remove_ / Clear_Opportunity_Fire_Event_Subscription(s) (§2.12): a weapon hard-point type name.
int tf_subscription(Call& c, int op, const char* name) {
    int32_t type = -1;
    if (op != 2) {
        if (c.nargs != 1) return script_error(c, "%s -- invalid number of paramters. Expected 1, got %d.", name, c.nargs);  // [H] text
        if (!c.is_string(1)) return script_error(c, "%s -- invalid type for parameter 1. Expected string", name);
        for (size_t i = 0; i < std::size(kHardPointTypes); ++i)
            if (iequal(kHardPointTypes[i], c.string(1))) type = static_cast<int32_t>(i);
        if (type < 0) return script_error(c, "%s -- unknown hard point type %s", name, std::string(c.string(1)).c_str());
        if (type < 5 || type > 10) return script_error(c, "%s -- %s is not a weapon hard point type", name, std::string(c.string(1)).c_str());
    }
    AiScriptHost* h = ai(c);
    if (h && h->tf_opportunity_fire_subscription(c.self->id, op, type) != 0)
        script_warning(c, "Add_Opportunity_Fire_Event_Subscription -- %s %s", std::string(c.string(1)).c_str(),
                       op == 0 ? "is already subscribed" : "is not subscribed");  // [H] the remove warning says "Add_..."
    return 0;
}
int m_tf_add_subscription(Call& c) { return tf_subscription(c, 0, "Add_Opportunity_Fire_Event_Subscription"); }
int m_tf_remove_subscription(Call& c) { return tf_subscription(c, 1, "Remove_Opportunity_Fire_Event_Subscription"); }
int m_tf_clear_subscriptions(Call& c) { return tf_subscription(c, 2, "Clear_Opportunity_Fire_Event_Subscriptions"); }

// Pushes the object of an "object or AI target" argument (nil when none); returns its stack index.
int push_target_object(Call& c, int i) {
    const ObjectHandle o = object_or_target(c, ai_arg(c, i));
    push_object(c, o);
    return lua_gettop(c.L);
}

// Can_Garrison(host) (§2.5): true when some unit can garrison it (each unit through the object binding, which applies
// the host / mask / capacity rules).
int m_tf_can_garrison(Call& c) {
    if (c.nargs != 1) return script_error(c, "TaskForce::Can_Garrison -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    const AiArg a = ai_arg(c, 1);
    if (!a.present || (!a.object && a.target < 0))
        return script_error(c, "TaskForce::Can_Garrison -- invalid type for parameter 1. Expected game object or AI target.");
    const int top = lua_gettop(c.L);
    const int arg = push_target_object(c, 1);
    if (lua_isnil(c.L, arg)) {
        lua_settop(c.L, top);
        return script_error(c, "TaskForce::Can_Garrison -- target is already dead");
    }
    bool any = false;
    for (const ObjectHandle u : tf_units(c, c.self->id))
        if (call_object_method(c, u, "Can_Garrison", arg, 1)) {
            any = true;
            break;
        }
    lua_settop(c.L, top);
    return push_bool(c, any);
}
// Garrison(host) (§2.5): GameObjectWrapper.Garrison for each unit (unit order). Returns nothing.
int m_tf_garrison(Call& c) {
    const int top = lua_gettop(c.L);
    const int arg = push_target_object(c, 1);
    if (!lua_isnil(c.L, arg))
        for (const ObjectHandle u : tf_units(c, c.self->id)) call_object_method(c, u, "Garrison", arg, 1);
    lua_settop(c.L, top);
    return 0;
}
int m_tf_leave_garrison(Call& c) {
    for (const ObjectHandle u : tf_units(c, c.self->id)) call_object_method(c, u, "Leave_Garrison", c.first, 0);
    return 0;
}
// TaskForceClass.Get_AI_Power_Vs_Unit(target) (§2.12): tactical only; the AI host (sum of P_u x weight minus
// P_target), else [port] the same with weight 1 from type data.
int m_tf_get_ai_power_vs_unit(Call& c) {
    if (store(c)->mode() == objects::ModeKind::Galactic) return script_error(c, "Get_AI_Power_Vs_Unit -- only valid in tactical mode");
    if (c.nargs != 1) return script_error(c, "Get_AI_Power_Vs_Unit -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    const AiArg other = ai_arg(c, 1);
    if (!other.present || (!other.object && other.target < 0))
        return script_error(c, "Get_AI_Power_Vs_Unit -- invalid type for parameter 1. Expected object or ai target.");
    AiArg self;
    self.present = true;
    self.task_force = c.self->id;
    if (AiScriptHost* h = ai(c)) {
        const double v = h->ai_power_vs(self, other);
        if (v > -1.0 || v < -1.0) return push_num(c, v);
    }
    const ObjectHandle o = object_or_target(c, other);
    double sum = 0.0;
    for (const ObjectHandle u : tf_units(c, c.self->id)) sum += type_power(c, u) * power_weight(c, u, o);
    if (o) sum -= type_power(c, o);
    return push_num(c, sum);
}
// Get_Type_Of_Unit(i) (§2.12): element int(i) (0-based) of the TF's type vector [M: the definition's unit types; port:
// the types of the TF's units in list order]; nothing out of range.
int m_tf_get_type_of_unit(Call& c) {
    if (!c.is_number(1)) return c.fail("TaskForceClass::Get_Type_Of_Unit -- Invalid Parameter 1, expected an integer index");
    const int32_t i = detail::to_int(c.number(1));
    const std::vector<ObjectHandle> units = tf_units(c, c.self->id);
    if (i < 0 || static_cast<size_t>(i) >= units.size()) return 0;
    return push_type(c, store(c)->type(units[static_cast<size_t>(i)]));
}
int m_tf_set_all_abilities_autofire(Call& c) {
    for (const ObjectHandle u : tf_units(c, c.self->id)) call_object_method(c, u, "Set_All_Abilities_Autofire", c.first, c.nargs);
    return 0;
}
int m_tf_set_single_ability_autofire(Call& c) {
    for (const ObjectHandle u : tf_units(c, c.self->id)) call_object_method(c, u, "Set_Single_Ability_Autofire", c.first, c.nargs);
    return 0;
}
// Set_Targeting_Stickiness_Time_Threshold(seconds [, filter]) (§2.12): arg 1 a number; arg 2 a GameObjectCategoryType
// name or a type name. Every unit with UNIT_AI whose type matches gets the override.
int m_tf_set_targeting_stickiness(Call& c) {
    constexpr const char* kName = "Set_Targeting_Stickiness_Time_Threshold";
    if (c.nargs != 1 && c.nargs != 2) return script_error(c, "%s - invalid number of parameters. Expected 1 or 2, got %d.", kName, c.nargs);
    if (!c.is_number(1)) return script_error(c, "%s - invalid type for parameter 1. Expected string.", kName);  // [H] text
    const objects::GameObjectTypes* ty = types(c);
    uint64_t mask = ~uint64_t{0};
    TypeIndex only = objects::kNone;
    if (c.nargs == 2) {
        const auto m = (ty && c.is_string(2)) ? ty->enums().get(objects::EnumId::GameObjectCategory).convert(c.string(2)) : std::nullopt;
        std::string tn;
        if (m) mask = static_cast<uint64_t>(*m);
        else if ((only = type_arg(c, 2, tn)) == objects::kNone || !valid_type(c, only))
            return script_error(c, "%s -- invalid type for parameter 2. Expected string.", kName);
    }
    const float seconds = static_cast<float>(c.number(1));
    static const objects::BehaviorTypeId kUnitAi = objects::behavior_type_id("UNIT_AI");
    for (const ObjectHandle u : tf_units(c, c.self->id)) {
        if (kUnitAi != objects::kNoBehavior && !store(c)->has_behavior(u, kUnitAi)) continue;
        const TypeIndex t = store(c)->type(u);
        if (only != objects::kNone && t != only) continue;
        if (mask != ~uint64_t{0} && (!ty || t >= ty->size() || (ty->get_mask(t, "CategoryMask") & mask) == 0)) continue;
        set_stickiness(c, u, seconds);
    }
    return 0;
}

int32_t tf_owner(Call& c, const std::vector<ObjectHandle>& units) { return units.empty() ? -1 : store(c)->owner(units.front()); }

// Find_Closest_Enemy([category]) (§2.12): 0 or 1 argument (a GameObjectCategoryType name; default all bits); the
// nearest live top-level enemy of the TF player with Is_Valid_Target and a CategoryMask bit of the mask, measured from
// the TF position (the mean of its units [port]); [port] no fog test. Nothing when none.
int m_tf_find_closest_enemy(Call& c) {
    if (c.nargs > 1) return script_error(c, "Find_Closest_Enemy - Invalid number of parameters - expected 1 or 2, got %d.", c.nargs);  // [H] text
    const objects::GameObjectTypes* ty = types(c);
    objects::ObjectStore* st = store(c);
    if (!ty || !st) return 0;
    uint64_t mask = ~uint64_t{0};
    if (c.nargs == 1) {
        if (!c.is_string(1)) return script_error(c, "Find_Closest_Enemy - Invalid type for parameter 1. Expected string.");
        const auto m = ty->enums().get(objects::EnumId::GameObjectCategory).convert(c.string(1));
        if (!m) return script_error(c, "Find_Closest_Enemy - unknown category %s", std::string(c.string(1)).c_str());
        mask = static_cast<uint64_t>(*m);
    }
    const std::vector<ObjectHandle> units = tf_units(c, c.self->id);
    if (units.empty()) return 0;
    const int32_t me = tf_owner(c, units);
    double cx = 0.0, cy = 0.0, cz = 0.0;
    for (const ObjectHandle u : units) {
        const Vec3 p = st->position(u);
        cx += p.x;
        cy += p.y;
        cz += p.z;
    }
    const double n = static_cast<double>(units.size());
    const Vec3 centre{static_cast<float>(cx / n), static_cast<float>(cy / n), static_cast<float>(cz / n)};
    ObjectBridge* b = bridge(c);
    ObjectHandle best;
    double best_d = 0.0;
    for (const ObjectHandle h : st->all_objects()) {
        if (!st->alive(h) || st->container(h) || st->attach_parent(h)) continue;
        const int32_t owner = st->owner(h);
        const bool enemy = (b && b->is_enemy) ? b->is_enemy(me, owner) : (owner != me && owner >= 0 && me >= 0);
        if (!enemy) continue;
        const TypeIndex t = st->type(h);
        if (t >= ty->size() || !ty->get_bool(t, "Is_Valid_Target") || (ty->get_mask(t, "CategoryMask") & mask) == 0) continue;
        const double d = dist_sq(centre, st->position(h));
        if (!best || d < best_d) {
            best = h;
            best_d = d;
        }
    }
    if (!best) return 0;
    detail::push_object(c.L, *c.vm, best);
    return 1;
}

// Bombing_Run(target) (§2.10): object or AI target (dead -> warning); the tactical AI host, else the mode's bombing
// run manager hook at the target position: started -> a BombingBlockStatus [port: a BlockingStatus finished at once],
// otherwise a warning and nothing.
int m_tf_bombing_run(Call& c) {
    constexpr const char* kName = "LandTaskForceClass::Bombing_Run";
    if (c.nargs != 1) return script_error(c, "%s -- invalid number of parameters. Expected 1, got %d.", kName, c.nargs);
    const AiArg a = ai_arg(c, 1);
    if (!c.userdata(1, detail::cls_game_object()) && !c.userdata(1, cls_target()))
        return script_error(c, "%s -- invalid type for parameter 1. Exepcted game objecdt or AI target", kName);  // [H] text
    Vec3 pos{};
    if (!position_arg(c, 1, pos)) return script_warning(c, "%s -- target is already dead", kName);
    if (AiTacticalScriptHost* t = ai_tactical(c)) {
        const int32_t s = t->tf_bombing_run(c.self->id, a, AiPoint{});
        if (s != -2) return push_ai_status(c, s);
    }
    RemainingScriptHooks* hk = hooks(c);
    if (!(hk && hk->bombing_run)) return script_warning(c, "%s -- No bombing run available", kName);
    const int32_t player = tf_owner(c, tf_units(c, c.self->id));
    if (!hk->bombing_run(player, pos)) {
        if (hk->bombing_run_available && hk->bombing_run_available(player))
            return script_warning(c, "%s -- Target position is within an invalid area. Aborting.", kName);
        return script_warning(c, "%s -- No bombing run available", kName);
    }
    Ud u;  // BombingBlockStatus over the player's run (bind_objects.cpp m_block_is_finished / m_block_result)
    u.cls = cls_blocking();
    u.id = player;
    u.reserved = kBlockBombingRun;
    detail::push_engine_userdata(c.L, u);
    return 1;
}
// Fire_Orbital_Bombardment(pos) (§2.10): exactly 1 position-convertible argument; the bombard manager hook. Returns
// nothing; failures are silent.
int m_tf_fire_orbital_bombardment(Call& c) {
    constexpr const char* kName = "LandTaskForceClass::Fire_Orbital_Bombardment";
    if (c.nargs != 1) return script_error(c, "%s -- invalid number of parameters. Expected 1, got %d.", kName, c.nargs);
    Vec3 pos{};
    if (!position_arg(c, 1, pos)) return script_error(c, "%s -- could not extract a position from parameter 1.", kName);
    if (AiTacticalScriptHost* t = ai_tactical(c)) {
        AiPoint pt;
        pt.present = true;
        pt.x = pos.x;
        pt.y = pos.y;
        pt.z = pos.z;
        if (t->tf_orbital_bombardment(c.self->id, ai_arg(c, 1), pt) != -2) return 0;
    }
    if (RemainingScriptHooks* hk = hooks(c); hk && hk->orbital_bombardment) hk->orbital_bombardment(tf_owner(c, tf_units(c, c.self->id)), pos);
    return 0;
}

// Refit_To_Definition(planet, n) (§2.12, AI_GALACTIC.md 2.4): the AI host's refit; returns its status.
int m_tf_refit_to_definition(Call& c) {
    if (c.nargs != 2) return script_error(c, "Refit_To_Definition: Invalid number of parameters. Expected 2, got %d.", c.nargs);
    const Ud* planet = c.userdata(1, detail::cls_game_object());
    if (!planet || !detail::object_alive(*c.vm, *planet))
        return script_error(c, "Refit_To_Definition: Invalid type for parameter 1. Expected planet.");
    if (!c.is_number(2)) return script_error(c, "Instantiate_Force: Invalid type for parameter 2. Expected number.");  // [H] text
    AiScriptHost* h = ai(c);
    const int32_t s = h ? h->tf_refit(c.self->id, detail::object_handle(*planet), c.number(2)) : -1;
    if (s == -3) return script_error(c, "Refit_To_Definition: the task force definition does not use percentages");
    return push_ai_status(c, s);
}

// FindTarget.Best_Of(taskforce, targets, perception) (§2.12): the entry with the highest perception value > 0 (the
// host evaluates); non-object entries and dead objects: warning, skipped. Nothing when none.
int m_best_of(Call& c) {
    if (c.nargs != 3) return script_error(c, "Find_Best_Of -- invalid number of parameters. Expected 3, got %d.", c.nargs);
    const AiArg tf = ai_arg(c, 1);
    if (tf.task_force < 0) return script_error(c, "Find_Best_Of -- invalid type for parameter 1. Expected taskforce.");
    if (!is_table(c, 2)) return script_error(c, "Find_Best_Of -- invalid type for parameter 2. Expected table of targets.");
    if (!c.is_string(3)) return script_error(c, "Find_Best_Of -- unrecognized perception function");
    const std::string perception(c.string(3));
    std::vector<AiArg> targets;
    std::vector<int> index;
    const int t = c.idx(2);
    const int n = luaL_getn(c.L, t);
    for (int k = 1; k <= n; ++k) {
        lua_rawgeti(c.L, t, k);
        AiArg a;
        const auto* ud = lua_type(c.L, -1) == LUA_TUSERDATA ? static_cast<const Ud*>(lua_touserdata(c.L, -1)) : nullptr;
        if (ud && ud->cls == cls_target()) {
            a.present = true;
            a.target = ud->id;
        } else if (ud && ud->cls == detail::cls_game_object()) {
            if (detail::object_alive(*c.vm, *ud)) {
                a.present = true;
                a.object = detail::object_handle(*ud);
            } else {
                script_warning(c, "Find_Best_Of -- target %d is dead", k);
            }
        } else {
            script_warning(c, "Find_Best_Of -- invalid type for target %d", k);
        }
        lua_pop(c.L, 1);
        if (a.present) {
            targets.push_back(a);
            index.push_back(k);
        }
    }
    AiScriptHost* h = ai(c);
    const int32_t best = h ? h->best_of(script_id(c.L), tf.task_force, targets, perception) : -1;
    if (best < 0 || static_cast<size_t>(best) >= index.size()) return 0;
    lua_rawgeti(c.L, t, index[static_cast<size_t>(best)]);
    return 1;
}

// ================================================================ LuaScriptWrapper

ScriptWrapperHost* script_host(lua_State* L) {
    lua_pushstring(L, kScriptWrapperHostKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    void* p = lua_type(L, -1) == LUA_TLIGHTUSERDATA ? lua_touserdata(L, -1) : nullptr;
    lua_pop(L, 1);
    return static_cast<ScriptWrapperHost*>(p);
}
LuaVm* target_vm(Call& c) {
    ScriptWrapperHost* h = script_host(c.L);
    LuaVm* vm = h ? h->script_vm(c.self->id) : nullptr;
    return (vm && vm != c.vm->owner && vm->on_owner_thread()) ? vm : nullptr;
}

// The LuaScriptWrapper of script `id` in `to` (STRATEGIC_GAPS §5.2: cached per (script, target VM), so the same script
// gives the same userdata in one VM).
void push_script_wrapper(lua_State* to, int32_t id) {
    Ud u;
    u.cls = cls_script_wrapper();
    u.id = id;
    detail::push_cached_userdata(to, u, static_cast<lua_Number>(id));
}

// Copies the value at `from_index` of `from` onto `to`'s stack [port]: nil, booleans, numbers, strings, game objects
// (re-wrapped by handle in the target VM, nil when not alive there), types, players, other engine userdata as values,
// tables (recursively, depth 8, copyable keys only; Lua 5.0 next() order - deterministic with the serial patch).
// OI lua (STRATEGIC_GAPS §5.2 [M]): a script object (the source VM's `Script`) becomes a LuaScriptWrapper of that script
// in the target VM - the only place wrappers are created besides set_script_wrapper_global; nil when the source VM's
// ScriptWrapperHost does not know the script. A wrapper is re-pushed through the target VM's wrapper cache.
void copy_value(lua_State* from, LuaVm::Impl& from_vm, int from_index, lua_State* to, LuaVm::Impl& to_vm, int depth) {
    if (from_index < 0) from_index = lua_gettop(from) + from_index + 1;
    switch (lua_type(from, from_index)) {
        case LUA_TBOOLEAN: lua_pushboolean(to, lua_toboolean(from, from_index)); return;
        case LUA_TNUMBER: lua_pushnumber(to, lua_tonumber(from, from_index)); return;
        case LUA_TSTRING: lua_pushlstring(to, lua_tostring(from, from_index), lua_strlen(from, from_index)); return;
        case LUA_TUSERDATA: {
            const auto* ud = static_cast<const Ud*>(lua_touserdata(from, from_index));
            if (!ud || ud->cls == detail::kNoClass) break;
            if (ud->cls == detail::cls_script()) {
                ScriptWrapperHost* h = script_host(from);
                const int32_t id = h && from_vm.owner ? h->script_id(*from_vm.owner) : -1;
                if (id < 0) break;
                push_script_wrapper(to, id);
                return;
            }
            if (ud->cls == cls_script_wrapper()) {
                push_script_wrapper(to, ud->id);
                return;
            }
            if (ud->cls == detail::cls_game_object()) {
                const ObjectHandle h = detail::object_handle(*ud);
                if (to_vm.host.store && to_vm.host.store->alive(h) && to_vm.host.store->id(h) == ud->id) {
                    detail::push_object(to, to_vm, h);
                    return;
                }
                break;
            }
            if (ud->cls == detail::cls_type()) {
                detail::push_type(to, to_vm, static_cast<uint32_t>(ud->value));
                return;
            }
            if (ud->cls == detail::cls_player()) {
                detail::push_player(to, to_vm, ud->id);
                return;
            }
            detail::push_engine_userdata(to, *ud);
            return;
        }
        case LUA_TTABLE: {
            if (depth >= 8) break;
            lua_newtable(to);
            lua_pushnil(from);
            while (lua_next(from, from_index) != 0) {
                const int kt = lua_type(from, -2);
                if (kt == LUA_TNUMBER || kt == LUA_TSTRING || kt == LUA_TBOOLEAN) {
                    copy_value(from, from_vm, -2, to, to_vm, depth + 1);
                    copy_value(from, from_vm, -1, to, to_vm, depth + 1);
                    if (lua_isnil(to, -2)) lua_pop(to, 2);
                    else lua_rawset(to, -3);
                }
                lua_pop(from, 1);
            }
            return;
        }
        default: break;
    }
    lua_pushnil(to);
}

LuaVm::Impl* impl_of(LuaVm& vm);

int m_script_is_valid(Call& c) { return push_bool(c, target_vm(c) != nullptr); }

int m_script_get_variable(Call& c) {
    if (c.nargs != 1) return script_error(c, "LuaScriptWrapper::Get_Variable -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    if (!c.is_string(1)) return script_error(c, "LuaScriptWrapper::Get_Variable -- invalid type for parameter 1. Expected string.");
    LuaVm* vm = target_vm(c);
    LuaVm::Impl* ti = vm ? impl_of(*vm) : nullptr;
    if (!ti) return push_nil(c);
    lua_State* T = vm->state();
    const int top = lua_gettop(T);
    lua_pushlstring(T, c.string(1).data(), c.string(1).size());
    lua_rawget(T, LUA_GLOBALSINDEX);
    copy_value(T, *ti, -1, c.L, *c.vm, 0);
    lua_settop(T, top);
    return 1;
}
int m_script_set_variable(Call& c) {
    if (c.nargs != 2) return script_error(c, "LuaScriptWrapper::Set_Variable -- invalid number of parameters. Expected 2, got %d.", c.nargs);
    if (!c.is_string(1)) return script_error(c, "LuaScriptWrapper::Set_Variable -- invalid type for parameter 1. Expected string.");
    LuaVm* vm = target_vm(c);
    if (!vm) return 0;
    lua_State* T = vm->state();
    LuaVm::Impl* ti = impl_of(*vm);
    if (!ti) return 0;
    const int top = lua_gettop(T);
    lua_pushlstring(T, c.string(1).data(), c.string(1).size());
    copy_value(c.L, *c.vm, c.idx(2), T, *ti, 0);
    lua_rawset(T, LUA_GLOBALSINDEX);
    lua_settop(T, top);
    return 0;
}
// Call_Function(name, args...) -> the function's results (copied back); nothing when it is missing or fails (the
// error is logged in the calling VM).
int m_script_call_function(Call& c) {
    if (c.nargs < 1) return script_error(c, "LuaScriptWrapper::Call_Function -- invalid number of parameters. Expected at least 1, got %d.", c.nargs);
    if (!c.is_string(1)) return script_error(c, "LuaScriptWrapper::Call_Function -- invalid type for parameter 1. Expected string.");
    LuaVm* vm = target_vm(c);
    LuaVm::Impl* ti = vm ? impl_of(*vm) : nullptr;
    if (!ti) return 0;
    lua_State* T = vm->state();
    const int top = lua_gettop(T);
    lua_pushlstring(T, c.string(1).data(), c.string(1).size());
    lua_rawget(T, LUA_GLOBALSINDEX);
    if (lua_type(T, -1) != LUA_TFUNCTION) {
        lua_settop(T, top);
        return script_error(c, "LuaScriptWrapper::Call_Function -- %s is not a function", std::string(c.string(1)).c_str());
    }
    for (int i = 2; i <= c.nargs; ++i) copy_value(c.L, *c.vm, c.idx(i), T, *ti, 0);
    if (lua_pcall(T, c.nargs - 1, LUA_MULTRET, 0) != 0) {
        const std::string msg = lua_tostring(T, -1) ? lua_tostring(T, -1) : "error";
        lua_settop(T, top);
        return script_error(c, "LuaScriptWrapper::Call_Function -- %s", msg.c_str());
    }
    const int nres = lua_gettop(T) - top;
    for (int i = 1; i <= nres; ++i) copy_value(T, *ti, top + i, c.L, *c.vm, 0);
    lua_settop(T, top);
    return nres;
}

constexpr BindingKind G = BindingKind::Global;
constexpr BindingKind M = BindingKind::Method;
constexpr const char* GO = "GameObjectWrapper";
constexpr const char* TF = "TaskForceClass";

constexpr detail::ImplEntry kImpl[] = {
    // globals
    {G, "", "Are_On_Opposite_Sides_Of_Shield", g_are_on_opposite_sides_of_shield},
    {G, "", "Budget", g_noop},
    {G, "", "DiscreteDistribution", g_noop},
    {G, "", "Find_Best_Local_Threat_Center", g_find_best_local_threat_center},
    {G, "", "Find_Nearest_Space_Field", g_find_nearest_space_field},
    {G, "", "Force_Weather", g_force_weather},
    {G, "", "GlobalValue", g_noop},
    {G, "", "Is_Point_In_Asteroid_Field", g_is_point_in_asteroid_field},
    {G, "", "Is_Point_In_Ion_Storm", g_is_point_in_ion_storm},
    {G, "", "Is_Point_In_Nebula", g_is_point_in_nebula},
    {G, "", "SFXManager", g_sfx_manager},
    {G, "", "WeightedTypeList", g_noop},
    {G, "", "_ProduceObject", g_produce_object},
    {M, "GlobalValueClass", "Get", m_gv_get},
    {M, "GlobalValueClass", "Set", m_gv_set},
    // GameObjectWrapper
    {M, GO, "Build", m_build},
    {M, GO, "Can_Garrison_Fire", m_can_garrison_fire},
    {M, GO, "Divert", m_divert},
    {M, GO, "Is_On_Diversion", m_is_on_diversion},
    {M, GO, "Enable_Behavior", m_enable_behavior},
    {M, GO, "Enable_Dynamic_LOD", m_enable_dynamic_lod},
    {M, GO, "Explore_Area", m_explore_area},
    {M, GO, "Force_Test_Space_Conflict", m_force_test_space_conflict},
    {M, GO, "Get_AI_Power_Vs_Unit", m_get_ai_power_vs_unit},
    {M, GO, "Get_Affiliated_Indigenous_Type", m_get_affiliated_indigenous_type},
    {M, GO, "Get_All_Projectile_Types", m_get_all_projectile_types},
    {M, GO, "Get_Current_Projectile_Type", m_get_current_projectile_type},
    {M, GO, "Get_Build_Pad_Contents", m_get_build_pad_contents},
    {M, GO, "Lock_Build_Pad_Contents", m_lock_build_pad_contents},
    {M, GO, "Get_Energy", m_get_energy},
    {M, GO, "Get_Final_Blow_Player", m_get_final_blow_player},
    {M, GO, "Get_Rate_Of_Damage_Taken", m_get_rate_of_damage_taken},
    {M, GO, "Get_Time_Till_Dead", m_get_time_till_dead},
    {M, GO, "Is_In_Asteroid_Field", m_is_in_asteroid_field},
    {M, GO, "Is_In_Ion_Storm", m_is_in_ion_storm},
    {M, GO, "Is_In_Nebula", m_is_in_nebula},
    {M, GO, "Release", m_release},
    {M, GO, "Set_Prefer_Ground_Over_Space", m_set_prefer_ground_over_space},
    {M, GO, "Set_Targeting_Stickiness_Time_Threshold", m_set_targeting_stickiness},
    // GameObjectTypeWrapper / PlayerWrapper
    {M, "GameObjectTypeWrapper", "Get_Bribe_Cost", m_type_get_bribe_cost},
    {M, "GameObjectTypeWrapper", "Is_Obsolete", m_type_is_obsolete},
    {M, "PlayerWrapper", "Get_GameSpy_Stats_Player_ID", m_get_gamespy_stats_player_id},
    {M, "PlayerWrapper", "Give_Random_Sliceable_Tech", m_give_random_sliceable_tech},
    // task forces
    {M, TF, "Add_Force", m_tf_add_force},
    {M, TF, "Add_Opportunity_Fire_Event_Subscription", m_tf_add_subscription},
    {M, TF, "Remove_Opportunity_Fire_Event_Subscription", m_tf_remove_subscription},
    {M, TF, "Clear_Opportunity_Fire_Event_Subscriptions", m_tf_clear_subscriptions},
    {M, TF, "Can_Garrison", m_tf_can_garrison},
    {M, TF, "Garrison", m_tf_garrison},
    {M, TF, "Leave_Garrison", m_tf_leave_garrison},
    {M, TF, "Get_AI_Power_Vs_Unit", m_tf_get_ai_power_vs_unit},
    {M, TF, "Get_Type_Of_Unit", m_tf_get_type_of_unit},
    {M, TF, "Set_All_Abilities_Autofire", m_tf_set_all_abilities_autofire},
    {M, TF, "Set_Single_Ability_Autofire", m_tf_set_single_ability_autofire},
    {M, TF, "Set_Targeting_Stickiness_Time_Threshold", m_tf_set_targeting_stickiness},
    {M, "LandTaskForceClass", "Find_Closest_Enemy", m_tf_find_closest_enemy},
    {M, "SpaceTaskForceClass", "Find_Closest_Enemy", m_tf_find_closest_enemy},
    {M, "LandTaskForceClass", "Bombing_Run", m_tf_bombing_run},
    {M, "LandTaskForceClass", "Fire_Orbital_Bombardment", m_tf_fire_orbital_bombardment},
    {M, "GalacticTaskForceClass", "Refit_To_Definition", m_tf_refit_to_definition},
    {M, "FindTargetClass", "Best_Of", m_best_of},
    // LuaScriptWrapper
    {M, "LuaScriptWrapper", "Call_Function", m_script_call_function},
    {M, "LuaScriptWrapper", "Get_Variable", m_script_get_variable},
    {M, "LuaScriptWrapper", "Set_Variable", m_script_set_variable},
    {M, "LuaScriptWrapper", "Is_Valid", m_script_is_valid},
};

// The VM's Impl from the shared metatable's __index closure (upvalue 1 = &Impl, bindings.cpp; bind_ai.cpp's rule).
LuaVm::Impl* impl_of(LuaVm& vm) {
    lua_State* L = vm.state();
    lua_pushstring(L, "alamo.meta");
    lua_rawget(L, LUA_REGISTRYINDEX);
    LuaVm::Impl* impl = nullptr;
    if (lua_type(L, -1) == LUA_TTABLE) {
        lua_pushstring(L, "__index");
        lua_rawget(L, -2);
        if (lua_type(L, -1) == LUA_TFUNCTION && lua_getupvalue(L, -1, 1)) {
            if (lua_type(L, -1) == LUA_TLIGHTUSERDATA) impl = static_cast<LuaVm::Impl*>(lua_touserdata(L, -1));
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return impl;
}

}  // namespace

std::span<const detail::ImplEntry> detail::impl_script_remaining() { return kImpl; }

bool detail::call_object_method(Call& c, ObjectHandle unit, const char* name, int first, int nargs) {
    const uint32_t slot = detail::find_method(detail::cls_game_object(), name);
    if (slot == detail::kNoBinding) return false;
    const detail::MethodSlot& s = detail::method_slots()[slot];
    if (!s.fn) return false;
    const int top = lua_gettop(c.L);
    detail::push_object(c.L, *c.vm, unit);
    const auto* ud = static_cast<const Ud*>(lua_touserdata(c.L, -1));
    bool result = false;
    if (ud && detail::object_alive(*c.vm, *ud)) {
        char err[256];
        err[0] = 0;
        Call sub{c.L, c.vm, ud, first, nargs, err, sizeof err};
        const int n = s.fn(sub);
        if (n > 0) result = lua_toboolean(c.L, top + 2) != 0;
    }
    lua_settop(c.L, top);
    return result;
}

// ================================================================ host-side API

uint32_t parse_space_field_mask(std::string_view text) {
    uint32_t mask = 0;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == '|' || text[i] == ' ' || text[i] == ',' || text[i] == '\t')) ++i;
        size_t j = i;
        while (j < text.size() && text[j] != '|' && text[j] != ' ' && text[j] != ',' && text[j] != '\t') ++j;
        const std::string_view w = text.substr(i, j - i);
        if (iequal(w, "Asteroid") || iequal(w, "Asteroid_Field") || iequal(w, "Asteroids")) mask |= kFieldAsteroid;
        else if (iequal(w, "Ion_Storm") || iequal(w, "IonStorm")) mask |= kFieldIonStorm;
        else if (iequal(w, "Nebula") || iequal(w, "Nebulae")) mask |= kFieldNebula;
        i = j;
    }
    return mask;
}

AiGlobalValue GlobalValueStore::get(std::string_view name) const {
    const auto it = values_.find(name);
    return it == values_.end() ? AiGlobalValue{} : it->second;
}
void GlobalValueStore::set(std::string_view name, const AiGlobalValue& v) {
    if (v.kind == AiGlobalValue::Nil) {
        const auto it = values_.find(name);
        if (it != values_.end()) values_.erase(it);
        return;
    }
    values_[std::string(name)] = v;
}
void hash_global_value(StateHasher& h, const AiGlobalValue& v) {
    h.u8(static_cast<uint8_t>(v.kind));
    h.f64(v.number);
    h.str(v.text);
    if (v.kind != AiGlobalValue::Table) return;
    h.u32(static_cast<uint32_t>(v.keys.size()));
    for (size_t i = 0; i < v.keys.size() && i < v.values.size(); ++i) {
        hash_global_value(h, v.keys[i]);
        hash_global_value(h, v.values[i]);
    }
}
void GlobalValueStore::hash(StateHasher& h) const {
    h.u32(static_cast<uint32_t>(values_.size()));
    for (const auto& [name, v] : values_) {
        h.str(name);
        hash_global_value(h, v);
    }
}

namespace {
void write_str(ByteWriter& w, std::string_view s) {
    w.u32(static_cast<uint32_t>(s.size()));
    for (const char ch : s) w.u8(static_cast<uint8_t>(ch));
}
bool read_str(ByteReader& r, std::string& s) {
    const uint32_t n = r.u32();
    if (!r.ok() || n > (1u << 24)) return false;
    s.clear();
    for (uint32_t i = 0; i < n && r.ok(); ++i) s.push_back(static_cast<char>(r.u8()));
    return r.ok();
}
void write_value(ByteWriter& w, const AiGlobalValue& v) {
    w.u8(static_cast<uint8_t>(v.kind));
    w.f64(v.number);
    write_str(w, v.text);
    if (v.kind != AiGlobalValue::Table) return;
    w.u32(static_cast<uint32_t>(v.keys.size()));
    for (size_t i = 0; i < v.keys.size() && i < v.values.size(); ++i) {
        write_value(w, v.keys[i]);
        write_value(w, v.values[i]);
    }
}
bool read_value(ByteReader& r, AiGlobalValue& v, int depth) {
    const uint8_t k = r.u8();
    if (!r.ok() || k > AiGlobalValue::Table || depth > 16) return false;
    v = AiGlobalValue{};
    v.kind = static_cast<AiGlobalValue::Kind>(k);
    v.number = r.f64();
    if (!read_str(r, v.text)) return false;
    if (v.kind != AiGlobalValue::Table) return true;
    const uint32_t n = r.u32();
    if (!r.ok() || n > (1u << 20)) return false;
    for (uint32_t i = 0; i < n; ++i) {
        AiGlobalValue key, val;
        if (!read_value(r, key, depth + 1) || !read_value(r, val, depth + 1)) return false;
        v.keys.push_back(std::move(key));
        v.values.push_back(std::move(val));
    }
    return true;
}
}  // namespace

// Save format: u32 version (1), u32 count, then per entry (name order) the name and the value.
void GlobalValueStore::write(ByteWriter& w) const {
    w.u32(1);
    w.u32(static_cast<uint32_t>(values_.size()));
    for (const auto& [name, v] : values_) {
        write_str(w, name);
        write_value(w, v);
    }
}
bool GlobalValueStore::read(ByteReader& r) {
    std::map<std::string, AiGlobalValue, std::less<>> in;
    if (r.u32() != 1 || !r.ok()) return false;
    const uint32_t n = r.u32();
    if (!r.ok()) return false;
    for (uint32_t i = 0; i < n; ++i) {
        std::string name;
        AiGlobalValue v;
        if (!read_str(r, name) || !read_value(r, v, 0)) return false;
        in[name] = std::move(v);
    }
    values_ = std::move(in);
    return true;
}

GlobalValueStore& process_global_values() {
    static GlobalValueStore store;  // one per process (§1.5), owner: the sim thread
    return store;
}

void attach_global_values(LuaVm& vm, GlobalValueStore* s) {
    lua_State* L = vm.state();
    lua_pushstring(L, kGlobalValueStoreKey);
    if (s) lua_pushlightuserdata(L, s);
    else lua_pushnil(L);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

void attach_script_wrapper_host(LuaVm& vm, ScriptWrapperHost* host) {
    lua_State* L = vm.state();
    lua_pushstring(L, kScriptWrapperHostKey);
    if (host) lua_pushlightuserdata(L, host);
    else lua_pushnil(L);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

void set_script_wrapper_global(LuaVm& vm, std::string_view name, int32_t id) {
    lua_State* L = vm.state();
    lua_pushlstring(L, name.data(), name.size());
    push_script_wrapper(L, id);
    lua_rawset(L, LUA_GLOBALSINDEX);
}

}  // namespace alamo::lua
