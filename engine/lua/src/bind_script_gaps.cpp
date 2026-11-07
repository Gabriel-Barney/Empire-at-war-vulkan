// Binding family: the script gaps left by L1/S4 wave 2a (L1/S4 wave 2b; docs/re/SCRIPT_GAPS.md, section numbers
// below). Host contract and error discipline: alamo/lua/script_gaps.hpp. [H] rules are the doc's; port choices are
// marked [port], the doc's [M] / [L] keep their mark.
//
// Determinism: walks use the store's insertion lists (owned_by / all_objects, copies taken before anything is
// destroyed or created); sorts are stable with explicit keys; the markup records live in the VM's registry (integer
// keys, read in index order); the landing machines are a vector sorted by id. No pointer-keyed container.
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/core/dmath.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/lua/ai_bridge.hpp"
#include "alamo/lua/object_bridge.hpp"
#include "alamo/lua/script_gaps.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "vm_internal.hpp"

namespace alamo::lua {

using detail::Call;
using detail::Ud;
using objects::ObjectHandle;
using objects::ObjectId;
using objects::TypeIndex;

namespace {

constexpr char kMarkupKey[] = "alamo.markup";            // {next = n, [handle] = {amount = x, [1..k] = target ids}}
constexpr char kMarkupHostKey[] = "alamo.markup.host";   // light userdata: ScriptMarkupHost*

// Behaviour type ids (OBJECT_RUNTIME.md §4.3, behaviors.cpp; the tests check them against the name table).
constexpr objects::BehaviorTypeId kBehSpecialWeapon = 0x2F, kBehLobbing = 0x37, kBehStarBase = 0x5C,
                                  kBehWeaponSource = 0x63, kBehDestroyAfterFire = 0x67;

// ---------------------------------------------------------------- small helpers

int push_nil(Call& c) {
    lua_pushnil(c.L);
    return 1;
}
int push_bool(Call& c, bool b) {
    lua_pushboolean(c.L, b ? 1 : 0);
    return 1;
}

// Errors and warnings: Call::script_error_stop / Call::script_warning (the original's helpers: logged, no unwind; the
// binding returns nothing).
uint32_t cls_of(const char* name) { return detail::class_id(name); }
using detail::cls_position;
using detail::cls_target;
uint32_t cls_markup() {
    static const uint32_t c = cls_of("MarkupBlockStatus");
    return c;
}

ObjectBridge* bridge(Call& c) { return c.vm->owner ? attached_object_bridge(*c.vm->owner) : nullptr; }
TacticalScriptHooks* hooks(Call& c) {
    ObjectBridge* b = bridge(c);
    return b ? b->tactical : nullptr;
}
objects::ObjectStore* store(Call& c) { return c.vm->host.store; }
const objects::GameObjectTypes* types(Call& c) { return c.vm->host.types; }
ObjectHandle self_object(Call& c) { return detail::object_handle(*c.self); }

char upper(char ch) { return (ch >= 'a' && ch <= 'z') ? static_cast<char>(ch - 'a' + 'A') : ch; }
bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (upper(a[i]) != upper(b[i])) return false;
    return true;
}

std::string_view type_name(Call& c, TypeIndex t) {
    const objects::GameObjectTypes* ty = types(c);
    if (!ty || t >= ty->raw().type_count()) return {};
    return ty->raw().types().name[t];
}
std::string object_name(Call& c, ObjectHandle h) { return std::string(type_name(c, store(c)->type(h))); }

bool type_has(Call& c, TypeIndex t, objects::BehaviorTypeId id) {
    const objects::GameObjectTypes* ty = types(c);
    return ty && t < ty->size() && ty->has_behavior(t, objects::BehaviorSet::Union, id);
}

// A type argument: a type name (string) or a GameObjectTypeWrapper. kNone when unknown.
TypeIndex type_arg(Call& c, int i, std::string& name) {
    if (const Ud* t = c.userdata(i, detail::cls_type())) {
        name = std::string(type_name(c, static_cast<TypeIndex>(t->value)));
        return static_cast<TypeIndex>(t->value);
    }
    if (!c.is_string(i)) return objects::kNone;
    name = std::string(c.string(i));
    return types(c) ? types(c)->raw().find(name) : objects::kNone;
}

using detail::position_of;

// A target argument (§1.1 step 2): a game object or an AI target (its object through the attached AI host).
enum class TargetArg : uint8_t { Object, AiTargetObject, AiTargetNoObject, Dead, Invalid };
TargetArg target_arg(Call& c, int i, ObjectHandle& out) {
    out = {};
    if (const Ud* o = c.userdata(i, detail::cls_game_object())) {
        if (!detail::object_alive(*c.vm, *o)) return TargetArg::Dead;
        out = detail::object_handle(*o);
        return TargetArg::Object;
    }
    if (const Ud* t = c.userdata(i, cls_target())) {
        AiScriptHost* ai = c.vm->owner ? ai_host(*c.vm->owner) : nullptr;
        const ObjectHandle h = ai ? ai->target_object(t->id) : ObjectHandle{};
        if (h && store(c)->alive(h)) {
            out = h;
            return TargetArg::AiTargetObject;
        }
        return TargetArg::AiTargetNoObject;
    }
    return TargetArg::Invalid;
}

bool destroy(Call& c, ObjectHandle h) {
    const ObjectBridge* b = bridge(c);
    if (b && b->destroy_object) return b->destroy_object(h);
    return store(c)->destroy(h);
}

// Destroy-after sweep (SCRIPT_GAPS.md conventions): every object of `player` with
// DUMMY_DESTROY_AFTER_SPECIAL_WEAPON_FIRED is destroyed (insertion order, a copy of the list).
void destroy_after_sweep(Call& c, int32_t player) {
    objects::ObjectStore* st = store(c);
    const auto owned = st->owned_by(player);
    const std::vector<ObjectHandle> list(owned.begin(), owned.end());
    for (const ObjectHandle h : list)
        if (st->alive(h) && st->has_behavior(h, kBehDestroyAfterFire)) destroy(c, h);
}

float readiness(TacticalScriptHooks* hk, ObjectHandle h) { return (hk && hk->weapon_readiness) ? hk->weapon_readiness(h) : -1.0f; }

// §1.1 upgrade gate: SpecialWeaponsAllowed for the player when a space mode exists (this port: the VM's store is a
// space mode - a land mode's stack holds no space mode) [port]. Unset hook: allowed.
bool upgrade_gate(Call& c, int32_t player) {
    if (store(c)->mode() != objects::ModeKind::Space) return true;
    TacticalScriptHooks* hk = hooks(c);
    return !(hk && hk->special_weapons_allowed) || hk->special_weapons_allowed(player);
}

// ---------------------------------------------------------------- §1.1 Fire_Special_Weapon

int m_fire_special_weapon(Call& c) {
    constexpr const char* kName = "GameObjectWrapper::Fire_Special_Weapon";
    if (c.nargs != 2) return c.script_error_stop("%s -- invalid number of parameters. Expected 2, got %d.", kName, c.nargs);
    objects::ObjectStore* st = store(c);
    const ObjectHandle self = self_object(c);
    const bool sw = st->has_behavior(self, kBehSpecialWeapon), lob = st->has_behavior(self, kBehLobbing),
               station = st->has_behavior(self, kBehStarBase);
    if (!sw && !lob && !station) return c.script_error_stop("%s -- object %s is not a special weapon", kName, object_name(c, self).c_str());
    ObjectHandle target;
    const TargetArg ta = target_arg(c, 1, target);
    if (ta == TargetArg::Dead) return c.script_error_stop("%s -- target is already dead", kName);
    if (ta == TargetArg::Invalid) return c.script_error_stop("%s -- invalid target", kName);
    const Ud* player = c.userdata(2, detail::cls_player());
    if (!player) return c.script_error_stop("%s -- invalid type for parameter 2. Expected player.", kName);
    TacticalScriptHooks* hk = hooks(c);
    if (station) {
        // §1.1 step 5: the station's special-weapon hard point.
        const float r = (hk && hk->station_readiness) ? hk->station_readiness(self) : -1.0f;
        if (r < 0.0f) return c.script_warning("%s -- %s has no special weapon hard point", kName, object_name(c, self).c_str());
        if (r < 1.0f) return c.script_warning("%s -- special weapon not ready", kName);
        if (!upgrade_gate(c, player->id))
            return c.script_warning("%s -- special weapons not available. Purchase a special weapon upgrade", kName);
        if (!target || !(hk->station_fire && hk->station_fire(self, target, player->id)))
            return c.script_warning("%s -- unable to fire at target", kName);
    } else if (lob && !sw) {
        // §1.1 step 3: the lobbing superweapon fires at the target's position (no range / ally / fog check).
        const float r = readiness(hk, self);
        if (r < 0.0f) return c.script_error_stop("%s -- %s has no special weapon data", kName, object_name(c, self).c_str());
        if (r < 1.0f) return c.script_warning("%s -- special weapon not ready", kName);
        // [port] an AI target without an object has no position through the AI bridge (AiScriptHost): refused.
        if (!target) return c.script_error_stop("%s -- invalid target", kName);
        if (!(hk->fire_lobbing_at && hk->fire_lobbing_at(self, st->position(target))))
            return c.script_warning("%s -- unable to fire at target", kName);  // [port] 0x4ea3f0 refused (base power)
    } else {
        // §1.1 step 4: special weapon at an object.
        if (ta == TargetArg::AiTargetNoObject) return c.script_error_stop("%s -- special weapons may only be fired at objects", kName);
        const float r = readiness(hk, self);
        if (r < 1.0f) return c.script_warning("%s -- special weapon not ready", kName);
        if (!(hk->begin_targeting && hk->begin_targeting(self))) return c.script_error_stop("%s -- unable to begin targeting", kName);
        if (!upgrade_gate(c, player->id))
            return c.script_warning("%s -- special weapons not available. Purchase a special weapon upgrade", kName);
        if (!(hk->fire_at_object && hk->fire_at_object(self, target))) return c.script_error_stop("%s -- Unknown and confusing error", kName);
    }
    destroy_after_sweep(c, player->id);  // §1.1 step 6: for the player argument
    detail::push_object(c.L, *c.vm, self);
    return 1;
}

// ---------------------------------------------------------------- §1.2 tactical superweapon

int m_fire_tactical_superweapon(Call& c) {
    const ObjectHandle self = self_object(c);
    if (!store(c)->has_behavior(self, objects::beh::TacticalSuperWeapon))
        return c.script_error_stop("GameObjectWrapper::Fire_Tactical_Superweapon -- %s is not a tactical superweapon", object_name(c, self).c_str());
    TacticalScriptHooks* hk = hooks(c);
    return push_bool(c, hk && hk->tsw_fire && hk->tsw_fire(self));
}

int m_is_tactical_superweapon_ready(Call& c) {
    const ObjectHandle self = self_object(c);
    if (!store(c)->has_behavior(self, objects::beh::TacticalSuperWeapon))
        return c.script_error_stop("GameObjectWrapper::Is_Tactical_Superweapon_Ready -- %s is not a tactical superweapon",
                            object_name(c, self).c_str());
    TacticalScriptHooks* hk = hooks(c);
    return push_bool(c, hk && hk->tsw_state && hk->tsw_state(self) == 4);  // state 4 = ready [H]
}

// ---------------------------------------------------------------- §1.3 Spawn_Special_Weapon

ObjectHandle spawn(Call& c, const objects::SpawnParams& sp) {
    TacticalScriptHooks* hk = hooks(c);
    if (hk && hk->spawn) return hk->spawn(sp);
    return store(c)->create(sp);
}

int g_spawn_special_weapon(Call& c) {
    constexpr const char* kName = "Spawn_Special_Weapon";
    objects::ObjectStore* st = store(c);
    if (!st || st->mode() != objects::ModeKind::Space) return c.script_error_stop("%s -- only available in space tactical mode", kName);
    if (c.nargs != 2) return c.script_error_stop("%s -- invalid number of parameters. Expected 2, got %d.", kName, c.nargs);
    std::string name;
    const TypeIndex t = type_arg(c, 1, name);
    const objects::GameObjectTypes* ty = types(c);
    if (t == objects::kNone || !ty || t >= ty->size()) return c.script_error_stop("%s -- unknown object type %s", kName, name.c_str());
    if (!ty->get_bool(t, "Is_Special_Weapon_In_Space"))
        return c.script_error_stop("%s -- %s is not a space special weapon", kName, name.c_str());
    if (ty->get_int(t, "Special_Weapon_Index") < 0)
        return c.script_error_stop("%s -- %s has no special weapon index", kName, name.c_str());
    const Ud* player = c.userdata(2, detail::cls_player());
    if (!player || player->id < 0) return c.script_error_stop("%s -- invalid type for parameter 2. Expected player.", kName);
    // The first object of the mode with DUMMY_SPECIAL_WEAPON_SOURCE (insertion order [M]).
    ObjectHandle source;
    for (const ObjectHandle h : st->all_objects())
        if (st->alive(h) && st->has_behavior(h, kBehWeaponSource)) {
            source = h;
            break;
        }
    if (!source)
        return c.script_error_stop("%s -- unable to find an object in the mode with SPECIAL_WEAPON_SOURCE behavior", kName);
    objects::SpawnParams sp;
    sp.type = t;
    sp.owner = player->id;
    sp.position = st->position(source);
    sp.facing = st->facing(source);
    detail::push_object(c.L, *c.vm, spawn(c, sp));
    return 1;
}

// ---------------------------------------------------------------- §1.7 Is_Affected_By_Laser_Defense

int m_type_is_affected_by_laser_defense(Call& c) {
    const objects::GameObjectTypes* ty = types(c);
    const auto t = static_cast<TypeIndex>(c.self->value);
    if (!ty || t >= ty->size()) return push_bool(c, false);
    // Projectile_Category MISSILE (1), ROCKET (4), MPTL_ROCKET (6): mask 0x52 [H]. The field's text, else its enum.
    const std::string_view cat = ty->get_string(t, "Projectile_Category");
    if (!cat.empty()) return push_bool(c, iequal(cat, "MISSILE") || iequal(cat, "ROCKET") || iequal(cat, "MPTL_ROCKET"));
    const int32_t v = ty->get_enum(t, "Projectile_Category");
    return push_bool(c, v >= 0 && v < 32 && ((0x52u >> v) & 1u) != 0);
}

// ---------------------------------------------------------------- §5.1 Get_Parent_Mode_Object

int m_get_parent_mode_object(Call& c) {
    if (c.vm->store_tag == detail::kParentStoreTag) return push_nil(c);  // [port] no grand-parent store reachable
    const ObjectId pid = store(c)->parent_mode_id(self_object(c));
    objects::ObjectStore* ps = detail::parent_store(*c.vm);
    if (pid == objects::kNoObjectId || !ps) return push_nil(c);
    const ObjectHandle h = ps->find(pid);
    if (!h || !ps->alive(h)) return push_nil(c);
    detail::push_parent_object(c.L, *c.vm, h);
    return 1;
}

// ---------------------------------------------------------------- §3.1 Create_Cinematic_Transport

int g_create_cinematic_transport(Call& c) {
    constexpr const char* kName = "Create_Cinematic_Transport";
    objects::ObjectStore* st = store(c);
    if (!st || st->mode() != objects::ModeKind::Land) return c.script_error_stop("%s -- only available in land tactical mode", kName);
    if (c.nargs < 8) return c.script_error_stop("%s -- invalid number of parameters. Expected 8 or 9, got %d.", kName, c.nargs);
    if (!c.is_string(1)) return c.script_error_stop("%s -- invalid type for parameter 1. Expected type name.", kName);
    // Arguments 2 and 4..8: a wrong type logs the error and keeps the default (§3.1 table).
    int32_t player = 0;
    if (c.is_number(2)) player = detail::to_int(c.number(2));
    else c.script_error_stop("%s -- invalid type for parameter 2. Expected player id.", kName);
    Vec3 pos{};
    bool have_pos = false;
    if (const Ud* o = c.userdata(3, detail::cls_game_object()); o && detail::object_alive(*c.vm, *o)) {
        pos = st->position(detail::object_handle(*o));
        have_pos = true;
    } else if (const Ud* p = c.userdata(3, cls_position())) {
        pos = position_of(*p);
        have_pos = true;
    }
    if (!have_pos) return c.script_warning("%s -- unable to get a position from parameter 3", kName);
    auto number = [&c](int i, double fallback) {
        if (c.is_number(i)) return c.number(i);
        c.script_error_stop("Create_Cinematic_Transport -- invalid type for parameter %d. Expected number.", i);
        return fallback;
    };
    const float z_angle = static_cast<float>(number(4, 0.0));
    const int32_t landing_mode = detail::to_int(number(5, 1.0));
    const float anim_delta = static_cast<float>(number(6, 0.0));
    const float idle = static_cast<float>(number(7, 2.0));
    const bool persist = number(8, 0.0) > 0.0;
    std::string hint;
    if (c.has(9)) {
        if (c.is_string(9)) hint = std::string(c.string(9));
        else c.script_error_stop("%s -- invalid type for parameter 9. Expected hint name.", kName);
    }
    const objects::GameObjectTypes* ty = types(c);
    const TypeIndex t = ty ? ty->raw().find(c.string(1)) : objects::kNone;
    if (t == objects::kNone || t >= ty->size()) return push_nil(c);
    // 0x2cb7a0: TRANSPORT and type byte +0x65 (Has_Space_Evaluator [M]) clear, else nothing is created.
    if (!type_has(c, t, objects::beh::Transport) || ty->get_bool(t, "Has_Space_Evaluator")) return push_nil(c);
    TacticalScriptHooks* hk = hooks(c);
    float z = pos.z;
    if (hk && hk->ground_height) z = std::max(z, hk->ground_height(pos.x, pos.y));
    z += ty->get_float(t, "Hover_Offset");
    // Facing (0, 0, z_angle): a yaw in degrees [M] (shipped scripts pass 0) as the store's facing vector.
    const float yaw = z_angle * 0.017453292519943295f;
    objects::SpawnParams sp;
    sp.type = t;
    sp.owner = player;
    sp.position = {pos.x, pos.y, z};
    sp.facing = {dmath::cos(yaw), dmath::sin(yaw), 0.0f};
    const ObjectHandle h = spawn(c, sp);
    if (!h || !st->alive(h)) return push_nil(c);
    if (!st->has_behavior(h, objects::beh::TransportLanding)) st->add_behavior(h, objects::beh::TransportLanding);
    st->set_landing_persist(h, persist);  // a1i t05: pack +0xc9, read by the land drop test (TACTICAL_GAPS §4.2.2 item 3)
    if (hk && hk->transports) hk->transports->add(st->id(h), landing_mode, anim_delta, idle, persist);
    if (!hint.empty()) {
        if (!st->has_behavior(h, objects::beh::Hint)) st->add_behavior(h, objects::beh::Hint);
        if (ObjectBridge* b = bridge(c)) b->set_hint(st->id(h), hint);
    }
    detail::push_object(c.L, *c.vm, h);
    return 1;
}

// ---------------------------------------------------------------- §3.2 Start_Cinematic_Space_Retreat

int g_start_cinematic_space_retreat(Call& c) {
    constexpr const char* kName = "Start_Cinematic_Space_Retreat";
    objects::ObjectStore* st = store(c);
    if (!st || st->mode() != objects::ModeKind::Space) return c.script_error_stop("%s -- only available in space tactical mode", kName);
    int32_t player = 0;
    if (c.is_number(1)) player = detail::to_int(c.number(1));
    else c.script_error_stop("%s -- invalid type for parameter 1. Expected player id.", kName);
    int32_t delay = 0;
    if (c.is_number(2)) delay = detail::to_int(c.number(2) * 30.0);
    const objects::GameObjectTypes* ty = types(c);
    TacticalScriptHooks* hk = hooks(c);
    // 1. The retreating units (vf8) [port]: the player's live top-level objects with a locomotor, insertion order.
    struct Entry {
        ObjectHandle h;
        float speed = 0.0f;
        double dist = 0.0;
    };
    std::vector<Entry> units;
    for (const ObjectHandle h : st->owned_by(player)) {
        if (!st->active(h) || !st->has_behavior(h, objects::beh::Locomotor) || st->attach_parent(h) || st->container(h)) continue;
        const TypeIndex t = st->type(h);
        units.push_back({h, (ty && t < ty->size()) ? ty->get_float(t, "Max_Speed") : 0.0f, 0.0});
    }
    // 2. Sort by speed, descending (0x3727a0 [M]: Max_Speed), stable.
    std::stable_sort(units.begin(), units.end(), [](const Entry& a, const Entry& b) { return a.speed > b.speed; });
    ObjectHandle marker;
    for (const ObjectHandle h : st->all_objects())
        if (st->active(h) && iequal(type_name(c, st->type(h)), "Attacker Entry Position")) {
            marker = h;
            break;
        }
    if (marker) {
        const Vec3 m = st->position(marker), f = st->facing(marker);
        // Facing away: the marker's facing + 180 degrees (XY).
        double ax = -static_cast<double>(f.x), ay = -static_cast<double>(f.y);
        const double len = dmath::sqrt(ax * ax + ay * ay);
        if (len > 0.0) {
            ax /= len;
            ay /= len;
        } else {
            ax = -1.0;
            ay = 0.0;
        }
        const Vec3 away{static_cast<float>(ax), static_cast<float>(ay), 0.0f};
        // Formation slot (0x2a9670, spacing 2500) [port layout]: rows of 5 across the facing, centred on the
        // marker, further rows behind it.
        constexpr float kSpacing = 2500.0f;
        for (size_t i = 0; i < units.size(); ++i) {
            const float col = static_cast<float>(static_cast<int32_t>(i % 5) - 2), row = static_cast<float>(i / 5);
            const Vec3 p{m.x - away.y * col * kSpacing - away.x * row * kSpacing, m.y + away.x * col * kSpacing - away.y * row * kSpacing,
                         m.z};
            if (hk && hk->teleport) hk->teleport(units[i].h, p, away);
            else {
                st->set_position(units[i].h, p);
                st->set_facing(units[i].h, away);
            }
        }
        // Re-sort by distance to the point 10000 out along that facing, ascending (stable).
        const double px = m.x + ax * 10000.0, py = m.y + ay * 10000.0, pz = m.z;
        for (Entry& e : units) {
            const Vec3 p = st->position(e.h);
            const double dx = p.x - px, dy = p.y - py, dz = p.z - pz;
            e.dist = dx * dx + dy * dy + dz * dz;
        }
        std::stable_sort(units.begin(), units.end(), [](const Entry& a, const Entry& b) { return a.dist < b.dist; });
    } else {
        c.script_warning("%s -- no ATTACKER ENTRY POSITION marker: units are not lined up", kName);  // [port]
    }
    // 3. Staggered hyperspace out.
    const int32_t increment = (hk && hk->retreat_increment_frames) ? hk->retreat_increment_frames(player) : 0;
    if (!(hk && hk->hyperspace_out_link) && !units.empty())
        c.script_warning("%s -- no hyperspace host: %d units lined up, none jumps", kName, static_cast<int>(units.size()));
    for (const Entry& e : units) {
        if (hk && hk->hyperspace_out_link) hk->hyperspace_out_link(e.h, delay, false);  // L1 final: keep = false (§3.4)
        delay += increment;
    }
    return 0;
}

// ---------------------------------------------------------------- §5.2 Apply_Markup

ScriptMarkupHost* markup_of(lua_State* L) { return detail::registry_ptr<ScriptMarkupHost>(L, kMarkupHostKey); }

// Pushes the markup record table of the VM (created on first use).
void push_markup_table(lua_State* L) {
    lua_pushstring(L, kMarkupKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_type(L, -1) == LUA_TTABLE) return;
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushstring(L, kMarkupKey);
    lua_pushvalue(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

double table_number(lua_State* L, int t, const char* key) {
    lua_pushstring(L, key);
    lua_rawget(L, t);
    const double v = lua_type(L, -1) == LUA_TNUMBER ? lua_tonumber(L, -1) : 0.0;
    lua_pop(L, 1);
    return v;
}
void set_table_number(lua_State* L, int t, const char* key, double v) {
    lua_pushstring(L, key);
    lua_pushnumber(L, v);
    lua_rawset(L, t);
}

// One target value at stack index `i` -> AI target ids (game object, AI target, player).
void markup_targets_of(Call& c, int i, int32_t caller, ScriptMarkupHost* host, std::vector<int32_t>& out) {
    lua_State* L = c.L;
    if (lua_type(L, i) != LUA_TUSERDATA) return;
    const auto* ud = static_cast<const Ud*>(lua_touserdata(L, i));
    if (!ud) return;
    if (ud->cls == detail::cls_game_object()) {
        if (host && detail::object_alive(*c.vm, *ud)) out.push_back(host->markup_target_of_object(caller, detail::object_handle(*ud)));
    } else if (ud->cls == cls_target()) {
        out.push_back(ud->id);
    } else if (ud->cls == detail::cls_player()) {
        if (host) out.push_back(host->markup_target_of_player(caller, ud->id));
    }
}

int g_apply_markup(Call& c) {
    constexpr const char* kName = "Apply_Markup";
    if (c.nargs != 3 && c.nargs != 4) return c.script_error_stop("%s -- invalid number of parameters. Expected 3 or 4, got %d.", kName, c.nargs);
    const Ud* player = c.userdata(1, detail::cls_player());
    if (!player) return c.script_error_stop("%s -- invalid type for parameter 1. Expected player.", kName);
    if (!c.is_number(3)) return c.script_error_stop("%s -- invalid type for parameter 3. Expected number.", kName);
    const double amount = c.number(3);
    lua_State* L = c.L;
    ScriptMarkupHost* host = markup_of(L);
    push_markup_table(L);
    const int mt = lua_gettop(L);
    // The handle: argument 4 when it is a MarkupBlockStatus (exactly 4 arguments), else a new one.
    int32_t handle = -1;
    if (c.nargs == 4)
        if (const Ud* hd = c.userdata(4, cls_markup())) handle = hd->id;
    lua_rawgeti(L, mt, handle);
    if (handle < 0 || lua_type(L, -1) != LUA_TTABLE) {
        lua_pop(L, 1);
        if (handle < 0) {
            handle = detail::to_int(table_number(L, mt, "next")) + 1;
            set_table_number(L, mt, "next", handle);
        }
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_rawseti(L, mt, handle);
    } else {
        // Reusing a handle removes its previous markup from every recorded target, then clears the records.
        const int rec = lua_gettop(L);
        const double previous = table_number(L, rec, "amount");
        const int n = detail::to_int(table_number(L, rec, "count"));
        for (int k = 1; k <= n; ++k) {
            lua_rawgeti(L, rec, k);
            const int32_t target = detail::to_int(lua_tonumber(L, -1));
            lua_pop(L, 1);
            if (host) host->add_markup(target, -previous);
            lua_pushnil(L);
            lua_rawseti(L, rec, k);
        }
        set_table_number(L, rec, "count", 0.0);
    }
    const int rec = lua_gettop(L);
    set_table_number(L, rec, "amount", amount);
    // The targets of argument 2: game object, AI target, player (checked in this order), or a table of them.
    std::vector<int32_t> targets;
    const int arg2 = c.idx(2);
    if (lua_type(L, arg2) == LUA_TTABLE) {
        const int n = luaL_getn(L, arg2);
        for (int k = 1; k <= n; ++k) {
            lua_rawgeti(L, arg2, k);
            markup_targets_of(c, lua_gettop(L), player->id, host, targets);
            lua_pop(L, 1);
        }
    } else if (lua_type(L, arg2) == LUA_TUSERDATA &&
               (c.userdata(2, detail::cls_game_object()) || c.userdata(2, cls_target()) || c.userdata(2, detail::cls_player()))) {
        markup_targets_of(c, arg2, player->id, host, targets);
    } else {
        lua_settop(L, mt - 1);
        return c.script_error_stop("%s -- invalid type for parameter 2.", kName);
    }
    int count = 0;
    for (const int32_t t : targets) {
        if (t < 0 || !host) continue;
        host->add_markup(t, amount);
        lua_pushnumber(L, t);
        lua_rawseti(L, rec, ++count);
    }
    set_table_number(L, rec, "count", count);
    lua_settop(L, mt - 1);
    Ud u;
    u.cls = cls_markup();
    u.id = handle;
    detail::push_engine_userdata(L, u);
    return 1;
}

int m_markup_is_finished(Call& c) { return push_bool(c, true); }

constexpr BindingKind G = BindingKind::Global;
constexpr BindingKind M = BindingKind::Method;

constexpr detail::ImplEntry kImpl[] = {
    {M, "GameObjectWrapper", "Fire_Special_Weapon", m_fire_special_weapon},
    {M, "GameObjectWrapper", "Fire_Tactical_Superweapon", m_fire_tactical_superweapon},
    {M, "GameObjectWrapper", "Is_Tactical_Superweapon_Ready", m_is_tactical_superweapon_ready},
    {M, "GameObjectWrapper", "Get_Parent_Mode_Object", m_get_parent_mode_object},
    {M, "GameObjectTypeWrapper", "Is_Affected_By_Laser_Defense", m_type_is_affected_by_laser_defense},
    {G, "", "Spawn_Special_Weapon", g_spawn_special_weapon},
    {G, "", "Create_Cinematic_Transport", g_create_cinematic_transport},
    {G, "", "Start_Cinematic_Space_Retreat", g_start_cinematic_space_retreat},
    {G, "", "Apply_Markup", g_apply_markup},
    {M, "MarkupBlockStatus", "IsFinished", m_markup_is_finished},
};

}  // namespace

std::span<const detail::ImplEntry> detail::impl_script_gaps() { return kImpl; }

// ---------------------------------------------------------------- §1.4 / §1.5 task-force variants

namespace {

// LandTaskForce.Fire_Special_Weapon(type_name, target) 0x6eb7f0 (AI_TACTICAL §7.3.4, s78f t04) [H unless tagged]. No
// block status. Exactly 2 arguments. type_name: a Lua string naming a type with LOBBING_SUPERWEAPON in its Land behaviour
// set. target: an AITargetLocationWrapper (point = the location's position: a unit target's object position [M: its
// getter], a region target's stored centre - accepted without an object) or a GameObjectWrapper (the raw position, no
// Ranged_Target_Z_Adjust); anything else, a plain position included, is an error. Then the FIRST task-force unit (list
// order) whose representative type (0x3973b0) is the type, that has LOBBING_SUPERWEAPON and whose readiness is >= 1.0
// gets Fire_Lobbing_At_Position(point) (stored as given), the owner's destroy-after sweep runs and the unit is returned
// - even when 0x4ea3f0 silently refused the order (no tactical player data, Requires_Base_Power without base power):
// later units are not tried. No ready unit: false. Errors / warnings return nothing.
int land_task_force_fire_special_weapon(Call& c, const char* kName, const std::vector<ObjectHandle>& units) {
    objects::ObjectStore* st = store(c);
    const objects::GameObjectTypes* ty = types(c);
    TacticalScriptHooks* hk = hooks(c);
    if (c.nargs != 2) return c.script_error_stop("%s -- invalid number of parameters: expected 2 got %d", kName, c.nargs);
    if (!c.is_string(1)) return c.script_error_stop("%s -- invalid type for parameter 1. Expected string", kName);
    const std::string name(c.string(1));
    const TypeIndex t = ty ? ty->raw().find(name) : objects::kNone;
    // 0x370320(type, 0x37, 1): the Land behaviour set only (a type with it in <Behavior> alone is refused).
    const bool lobbing = t != objects::kNone && t < ty->size() && ty->has_behavior(t, objects::BehaviorSet::Land, kBehLobbing);
    if (!lobbing) return c.script_error_stop("%s -- %s is not a land special weapon!", kName, name.c_str());
    Vec3 point;
    if (const Ud* a = c.userdata(2, cls_target())) {
        AiScriptHost* ai = c.vm->owner ? ai_host(*c.vm->owner) : nullptr;
        const ObjectHandle h = ai && ai->target_valid(a->id) ? ai->target_object(a->id) : ObjectHandle{};
        if (h && st->alive(h)) {
            point = st->position(h);  // [M] the unit target's position getter (vf +0x68): the object's position
        } else if (!(ai && ai->target_valid(a->id) && ai->target_position(a->id, point))) {
            return c.script_warning("%s -- target is already dead.  Aborting.", kName);
        }
    } else if (const Ud* o = c.userdata(2, detail::cls_game_object())) {
        if (!detail::object_alive(*c.vm, *o)) return c.script_warning("%s -- target is already dead.  Aborting.", kName);
        point = st->position(detail::object_handle(*o));
    } else {
        return c.script_error_stop("%s -- Invalid type for parameter 1.  Expected game object or target location", kName);
    }
    const ObjectBridge* b = bridge(c);
    for (const ObjectHandle u : units) {
        if (!st->alive(u) || !st->has_behavior(u, kBehLobbing) || readiness(hk, u) < 1.0f) continue;
        TypeIndex rep = b && b->representative_type ? b->representative_type(u) : objects::kNone;
        if (rep == objects::kNone) rep = st->type(u);
        if (rep != t) continue;
        if (hk && hk->fire_lobbing_at) (void)hk->fire_lobbing_at(u, point);  // the result is not checked
        destroy_after_sweep(c, st->owner(u));
        detail::push_object(c.L, *c.vm, u);
        return 1;
    }
    return push_bool(c, false);
}

}  // namespace

int detail::task_force_fire_special_weapon(Call& c, bool space, const std::vector<ObjectHandle>& units, int32_t player) {
    const char* kName = space ? "SpaceTaskForceClass::Fire_Special_Weapon" : "LandTaskForceClass::Fire_Special_Weapon";
    objects::ObjectStore* st = store(c);
    if (!st) return push_bool(c, false);
    const objects::GameObjectTypes* ty = types(c);
    TacticalScriptHooks* hk = hooks(c);
    if (!space) return land_task_force_fire_special_weapon(c, kName, units);
    std::string name;
    const TypeIndex t = type_arg(c, 1, name);
    ObjectHandle target;
    (void)target_arg(c, 2, target);
    if (t == objects::kNone || !ty || t >= ty->size() || !ty->get_bool(t, "Is_Special_Weapon_In_Space"))
        return c.script_error_stop("%s -- %s is not a space special weapon", kName, name.c_str());
    if (!target) return c.script_error_stop("%s -- Expected game object", kName);
    if (player < 0)
        for (const ObjectHandle u : units)
            if (st->alive(u)) {
                player = st->owner(u);
                break;
            }
    if (!upgrade_gate(c, player)) {
        c.script_warning("%s -- special weapons not available. Purchase a special weapon upgrade", kName);
        return push_bool(c, false);
    }
    const TypeIndex tt = st->type(target);
    // Special_Weapon_Valid_Targets (+0x1df0): a list of GameObjectCategoryType names -> CategoryMask bits.
    uint64_t valid = ty->get_mask(t, "Special_Weapon_Valid_Targets");
    if (valid == 0)
        for (const std::string_view n : ty->get_list(t, "Special_Weapon_Valid_Targets"))
            if (const auto m = ty->enums().get(objects::EnumId::GameObjectCategory).convert(n)) valid |= static_cast<uint64_t>(*m);
    if (tt >= ty->size() || (ty->get_mask(tt, "CategoryMask") & valid) == 0) return push_bool(c, false);
    for (const ObjectHandle u : units) {
        if (!st->alive(u) || st->type(u) != t || !st->has_behavior(u, kBehSpecialWeapon) || readiness(hk, u) < 1.0f) continue;
        if (!(hk && hk->begin_targeting && hk->begin_targeting(u))) return c.script_error_stop("%s -- unable to begin targeting", kName);
        if (!(hk->fire_at_object && hk->fire_at_object(u, target))) return c.script_error_stop("%s -- unable to fire", kName);
        destroy_after_sweep(c, st->owner(u));
        detail::push_object(c.L, *c.vm, u);
        return 1;
    }
    return push_bool(c, false);
}

// ---------------------------------------------------------------- markup host

void attach_markup_host(LuaVm& vm, ScriptMarkupHost* host) {
    detail::set_registry_ptr(vm.state(), kMarkupHostKey, host);
}
ScriptMarkupHost* markup_host(LuaVm& vm) { return markup_of(vm.state()); }

// ---------------------------------------------------------------- cinematic landing machines (§3.1)

void CinematicTransports::add(ObjectId id, int32_t landing_mode, float anim_delta, float idle_seconds, bool persist) {
    CinematicLanding l;
    l.id = id;
    l.landing_mode = landing_mode;
    l.anim_delta = anim_delta;
    l.idle_seconds = idle_seconds;
    l.persist = persist;
    const auto it = std::lower_bound(list_.begin(), list_.end(), id, [](const CinematicLanding& a, ObjectId b) { return a.id < b; });
    if (it != list_.end() && it->id == id) *it = l;
    else list_.insert(it, l);
}

void CinematicTransports::remove(ObjectId id) {
    list_.erase(std::remove_if(list_.begin(), list_.end(), [id](const CinematicLanding& l) { return l.id == id; }), list_.end());
}

const CinematicLanding* CinematicTransports::find(ObjectId id) const {
    for (const CinematicLanding& l : list_)
        if (l.id == id) return &l;
    return nullptr;
}

void CinematicTransports::service(uint32_t frame, const std::function<uint32_t(ObjectId, uint8_t)>& anim_frames,
                                  std::vector<CinematicStep>& out) {
    auto frames_of = [&anim_frames](ObjectId id, uint8_t anim) {
        const uint32_t n = anim_frames ? anim_frames(id, anim) : 0u;
        return n ? n : kDefaultLandingAnimFrames;
    };
    auto play = [&](CinematicLanding& l, uint8_t anim, float fraction, float speed) {
        const uint32_t n = frames_of(l.id, anim);
        l.anim = anim;
        l.anim_start = frame;
        l.anim_offset = static_cast<int32_t>(static_cast<float>(n - 1) * fraction);
        l.anim_speed = speed;
        out.push_back({anim == kAnimLand ? CinematicStep::Kind::PlayLand : CinematicStep::Kind::PlayTakeoff, l.id, fraction, speed});
    };
    auto progress = [&](const CinematicLanding& l) {
        const uint32_t n = frames_of(l.id, l.anim);
        const float played = static_cast<float>(l.anim_offset) + static_cast<float>(frame - l.anim_start) * l.anim_speed;
        return played / static_cast<float>(n);
    };
    auto idle_frames = [](float seconds) { return seconds > 0.0f ? static_cast<uint32_t>(seconds * 30.0f) : 0u; };
    std::vector<ObjectId> gone;
    for (CinematicLanding& l : list_) {
        if (!l.initialised) {  // 0x644040
            switch (l.landing_mode) {
                case 0: case 1: case 4:
                    l.state = 1;
                    play(l, kAnimLand, l.anim_delta, 1.0f);
                    break;
                case 2:  // already landed: Land at its last frame, idle timer
                    l.state = 2;
                    play(l, kAnimLand, 1.0f, 0.0f);
                    l.timer_end = frame + idle_frames(l.idle_seconds);
                    out.push_back({CinematicStep::Kind::Landed, l.id, 1.0f, 0.0f});
                    break;
                case 3:  // taking off
                    l.state = 3;
                    play(l, kAnimTakeoff, l.anim_delta, 1.0f);
                    l.timer_end = frame;
                    break;
                default: l.state = 0; break;
            }
            l.initialised = true;
            continue;
        }
        switch (l.state) {
            case 1:  // landing: past 98 % -> landed, idle timer (cinematic)
                if (progress(l) > 0.98f) {
                    l.state = 2;
                    l.timer_end = frame + idle_frames(l.idle_seconds);
                    out.push_back({CinematicStep::Kind::Landed, l.id, 1.0f, 0.0f});
                }
                break;
            case 2:  // landed: timer expired and not persistent -> take off at speed 1.15
                if (!l.persist && frame >= l.timer_end) {
                    l.state = 3;
                    play(l, kAnimTakeoff, 0.0f, 1.15f);
                }
                break;
            case 3: {  // taking off: past 50 % the cargo link is released, past 98 % the transport is destroyed
                const float p = progress(l);
                if (!l.cargo_released && p > 0.5f) {
                    l.cargo_released = true;
                    out.push_back({CinematicStep::Kind::CargoReleased, l.id, 0.0f, 0.0f});
                }
                if (p > 0.98f) {
                    out.push_back({CinematicStep::Kind::Destroy, l.id, 0.0f, 0.0f});
                    gone.push_back(l.id);
                }
                break;
            }
            default: break;
        }
    }
    for (const ObjectId id : gone) remove(id);
}

void CinematicTransports::hash(StateHasher& h) const {
    h.u32(static_cast<uint32_t>(list_.size()));
    for (const CinematicLanding& l : list_) {
        h.i32(l.id);
        h.i32(l.landing_mode);
        h.f32(l.anim_delta);
        h.f32(l.idle_seconds);
        h.boolean(l.persist);
        h.boolean(l.initialised);
        h.u8(l.state);
        h.u8(l.anim);
        h.u32(l.anim_start);
        h.i32(l.anim_offset);
        h.f32(l.anim_speed);
        h.u32(l.timer_end);
        h.boolean(l.cargo_released);
    }
}

namespace {
constexpr uint32_t kTransportsSaveVersion = 1;
}  // namespace

void CinematicTransports::write(ByteWriter& w) const {
    w.u32(kTransportsSaveVersion).u32(static_cast<uint32_t>(list_.size()));
    for (const CinematicLanding& l : list_) {
        w.i32(l.id).i32(l.landing_mode).f32(l.anim_delta).f32(l.idle_seconds).boolean(l.persist).boolean(l.initialised);
        w.u8(l.state).u8(l.anim).u32(l.anim_start).i32(l.anim_offset).f32(l.anim_speed).u32(l.timer_end);
        w.boolean(l.cargo_released);
    }
}

bool CinematicTransports::read(ByteReader& r) {
    if (r.u32() != kTransportsSaveVersion || !r.ok()) return false;
    const uint32_t n = r.u32();
    if (!r.ok() || n > r.remaining() / 37) return false;
    std::vector<CinematicLanding> list(n);
    for (CinematicLanding& l : list) {
        l.id = r.i32();
        l.landing_mode = r.i32();
        l.anim_delta = r.f32();
        l.idle_seconds = r.f32();
        l.persist = r.boolean();
        l.initialised = r.boolean();
        l.state = r.u8();
        l.anim = r.u8();
        l.anim_start = r.u32();
        l.anim_offset = r.i32();
        l.anim_speed = r.f32();
        l.timer_end = r.u32();
        l.cargo_released = r.boolean();
        if (!r.ok() || l.state > 3 || (&l != list.data() && l.id <= (&l - 1)->id)) return false;
    }
    list_ = std::move(list);
    return true;
}

}  // namespace alamo::lua
