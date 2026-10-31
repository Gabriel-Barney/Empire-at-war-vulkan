// Binding family: finding, spawning and commanding game objects (L1 stage 2).
//
// Sources: docs/re/LUA_API.md (binding map: argument-validation strings, reproduced verbatim),
// docs/re/OBJECT_RUNTIME.md (§1.5 wrappers / dead objects, §3 containment idioms, §6 newest-first
// order), docs/re/COMMANDS.md §5 (Lua orders bypass the queue: execute_direct), docs/re/OBJECT_TYPES.md
// (§9.1 category / property masks, §9.1.4 UnitAbilityType ids). Behaviour the docs do not pin is
// marked [inferred]; the host side (orders, hints, script-only flags) is alamo/lua/object_bridge.hpp.
//
// Determinism: every search walks the store's canonical NEWEST-FIRST list (objects_newest_first) or
// its insertion list, ties keep the first candidate in that order, distances use alamo::dmath, and the
// per-object script state is an ordered map. Nothing iterates a pointer-keyed or hashed container.
//
// Error discipline (vm_internal.hpp): implementations never raise Lua errors; they return c.fail().
#include <algorithm>
#include <array>
#include <bit>
#include <cstdarg>
#include <cstdio>
#include <iterator>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/core/dmath.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/lua/object_bridge.hpp"
#include "alamo/lua/players.hpp"
#include "alamo/lua/script_gaps.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "vm_internal.hpp"

#if ALAMO_LUA_HAVE_COMMANDS
#include "alamo/commands/pipeline.hpp"
#endif
#if ALAMO_LUA_HAVE_ABILITIES
#include "alamo/tactical/abilities/ability_system.hpp"
#include "alamo/tactical/abilities/ability_types.hpp"
#endif
#if ALAMO_LUA_HAVE_ANIM
#include "alamo/anim/anim_state.hpp"
#include "alamo/anim/anim_types.hpp"
#endif

namespace alamo::lua {

using detail::Call;
using detail::Ud;
using objects::ObjectHandle;
using objects::ObjectId;
using objects::TypeIndex;

namespace {

constexpr char kBridgeKey[] = "alamo.objects.bridge";

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
int push_str(Call& c, std::string_view s) {
    lua_pushlstring(c.L, s.data(), s.size());
    return 1;
}

bool is_bool(const Call& c, int i) { return c.has(i) && lua_type(c.L, c.idx(i)) == LUA_TBOOLEAN; }
bool to_bool(const Call& c, int i) { return lua_toboolean(c.L, c.idx(i)) != 0; }

char upper(char ch) { return (ch >= 'a' && ch <= 'z') ? static_cast<char>(ch - 'a' + 'A') : ch; }
bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (upper(a[i]) != upper(b[i])) return false;
    return true;
}

uint32_t cls_position() {
    static const uint32_t c = detail::class_id("PositionWrapper");
    return c;
}
uint32_t cls_blocking() {
    static const uint32_t c = detail::class_id("BlockingStatus");
    return c;
}

ObjectBridge* bridge(Call& c) {
    lua_pushstring(c.L, kBridgeKey);
    lua_rawget(c.L, LUA_REGISTRYINDEX);
    auto* b = static_cast<ObjectBridge*>(lua_touserdata(c.L, -1));
    lua_pop(c.L, 1);
    return b;
}

objects::ObjectStore* store(Call& c) { return c.vm->host.store; }
const objects::GameObjectTypes* types(Call& c) { return c.vm->host.types; }
ObjectHandle self_object(Call& c) { return detail::object_handle(*c.self); }

// Containment / removal through the bridge's hooks when the host keeps membership outside the store
// (object_bridge.hpp, L1/S4 wave 1); else the store's container links.
ObjectHandle container_of(Call& c, ObjectHandle h) {
    const ObjectBridge* b = bridge(c);
    if (b && b->container_of) return b->container_of(h);
    return store(c)->container(h);
}
// Live contained objects: the hook's order, else the store's insertion order.
std::vector<ObjectHandle> contents_of(Call& c, ObjectHandle h) {
    const ObjectBridge* b = bridge(c);
    const objects::ObjectStore* st = store(c);
    std::vector<ObjectHandle> out;
    if (b && b->contents_of) {
        for (const ObjectHandle m : b->contents_of(h))
            if (st->alive(m)) out.push_back(m);
        return out;
    }
    for (const ObjectHandle m : st->all_objects())
        if (st->alive(m) && st->container(m) == h) out.push_back(m);
    return out;
}
bool destroy_object(Call& c, ObjectHandle h) {
    const ObjectBridge* b = bridge(c);
    if (b && b->destroy_object) return b->destroy_object(h);
    return store(c)->destroy(h);
}

std::string_view type_name(Call& c, TypeIndex t) {
    const objects::GameObjectTypes* ty = types(c);
    if (!ty || t >= ty->raw().type_count()) return {};
    return ty->raw().types().name[t];
}
std::string_view object_type_name(Call& c, ObjectHandle h) { return type_name(c, store(c)->type(h)); }

// Script state of an object: the bridge's, or the defaults when no bridge is attached.
const ScriptObjectState& state_of(Call& c, ObjectId id) {
    static const ScriptObjectState kDefault;
    const ObjectBridge* b = bridge(c);
    const ScriptObjectState* s = b ? b->find_state(id) : nullptr;
    return s ? *s : kDefault;
}
// Mutable script state; nullptr without a bridge (setters are then no-ops).
ScriptObjectState* edit_state(Call& c, ObjectId id) {
    ObjectBridge* b = bridge(c);
    return b ? &b->state(id) : nullptr;
}
ScriptObjectState* edit_self(Call& c) { return edit_state(c, c.self->id); }

// ---------------------------------------------------------------- positions

// PositionWrapper userdata: x / y float bits in Ud::value, z float bits in Ud::id (a value, not a
// cached wrapper; __eq compares the bits).
void push_position(lua_State* L, Vec3 p) {
    Ud u;
    u.cls = cls_position();
    u.value = static_cast<uint64_t>(std::bit_cast<uint32_t>(p.x)) |
              (static_cast<uint64_t>(std::bit_cast<uint32_t>(p.y)) << 32);
    u.id = std::bit_cast<int32_t>(p.z);
    detail::push_engine_userdata(L, u);
}
Vec3 position_of(const Ud& u) {
    return {std::bit_cast<float>(static_cast<uint32_t>(u.value & 0xFFFFFFFFu)),
            std::bit_cast<float>(static_cast<uint32_t>(u.value >> 32)), std::bit_cast<float>(u.id)};
}

enum class PosArg : uint8_t { Ok, Dead, NotPosition };
// "Something which defines a position": a live GameObjectWrapper or a PositionWrapper.
PosArg position_arg(Call& c, int i, Vec3& out, ObjectHandle* object = nullptr) {
    if (const Ud* o = c.userdata(i, detail::cls_game_object())) {
        if (!detail::object_alive(*c.vm, *o)) return PosArg::Dead;
        out = store(c)->position(detail::object_handle(*o));
        if (object) *object = detail::object_handle(*o);
        return PosArg::Ok;
    }
    if (const Ud* p = c.userdata(i, cls_position())) {
        out = position_of(*p);
        return PosArg::Ok;
    }
    // OI lua (A1 ask 2) [port]: an AITargetLocationWrapper stands for its game object, a task force for its first live
    // unit (no live object = a dead argument), a region target its host position (target_position).
    ObjectHandle ai{};
    if (detail::ai_wrapper_object(c, i, ai)) {
        if (!ai) return detail::ai_target_position(c, i, out) ? PosArg::Ok : PosArg::Dead;
        out = store(c)->position(ai);
        if (object) *object = ai;
        return PosArg::Ok;
    }
    return PosArg::NotPosition;
}

double distance(Vec3 a, Vec3 b) {
    const double dx = static_cast<double>(a.x) - static_cast<double>(b.x);
    const double dy = static_cast<double>(a.y) - static_cast<double>(b.y);
    const double dz = static_cast<double>(a.z) - static_cast<double>(b.z);
    return dmath::sqrt(dx * dx + dy * dy + dz * dz);
}
double distance_sq(Vec3 a, Vec3 b) {
    const double dx = static_cast<double>(a.x) - static_cast<double>(b.x);
    const double dy = static_cast<double>(a.y) - static_cast<double>(b.y);
    const double dz = static_cast<double>(a.z) - static_cast<double>(b.z);
    return dx * dx + dy * dy + dz * dz;
}

// Heading (degrees, atan2 convention: 0 = +X, counter-clockwise) from `from` towards `to` in the XY plane.
float heading_deg(Vec3 from, Vec3 to) {
    const float a = dmath::atan2(to.y - from.y, to.x - from.x);
    return a * 57.29577951308232f;
}

// ---------------------------------------------------------------- types, players

struct TypeArg {
    bool given = false;
    TypeIndex index = objects::kNone;
    std::string_view name;
};
// GameObjectTypeWrapper or type name. Objects match by type NAME (case-insensitive), so every
// duplicate definition of a name matches (the raw database's find() only returns the first one).
TypeArg type_arg(Call& c, int i) {
    TypeArg a;
    if (const Ud* t = c.userdata(i, detail::cls_type())) {
        a.given = true;
        a.index = static_cast<TypeIndex>(t->value);
        a.name = type_name(c, a.index);
    } else if (c.is_string(i)) {
        a.given = true;
        a.name = c.string(i);
        a.index = types(c) ? types(c)->raw().find(a.name) : objects::kNone;
    }
    return a;
}
bool type_matches(Call& c, ObjectHandle h, std::string_view name) {
    return !name.empty() && iequal(object_type_name(c, h), name);
}

const Ud* player_arg(Call& c, int i) { return c.userdata(i, detail::cls_player()); }

bool is_enemy(Call& c, int32_t a, int32_t b) {
    const ObjectBridge* br = bridge(c);
    if (br && br->is_enemy) return br->is_enemy(a, b);
    return a != b && a >= 0 && b >= 0;  // PROVISIONAL (object_bridge.hpp)
}

// ---------------------------------------------------------------- object finders (refixf t03)
//
// docs/re/LUA_API.md "Find_All_Objects_Of_Type and the sibling finders" (0x540b20 / 0x6970a0 / 0x69e7c0). Their errors
// go through the original's script error helper 0x248190: logged, no unwind, the binding returns no value (nil).

int script_error(Call& c, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    c.vm->message(std::string("SCRIPT ERROR: ") + buf);
    return 0;
}

// The filters of Find_All_Objects_Of_Type / Find_Nearest; all set filters must pass (AND).
struct ObjectFilter {
    bool have_owner = false;
    int32_t owner = -1;
    bool have_type = false;
    TypeIndex type = objects::kNone;
    bool have_props = false;  // GameObjectPropertiesType mask (any bit of the representative type's Property_Flags)
    uint64_t props = 0;
    bool have_cat = false;    // GameObjectCategoryType mask (any bit of the representative type's CategoryMask)
    uint64_t cat = 0;
    bool any() const { return have_owner || have_type || have_props || have_cat; }
};

// The string chain [H]: a GameObjectPropertiesType bitfield (while no properties mask is set), else a
// GameObjectCategoryType bitfield (while no category mask is set), else an object type name (while no type is set;
// case-insensitive, first definition of a name). The bitfields split on '|', space, ',', tab and LF, every token must
// be known (case-insensitive), and an all-delimiter string is mask 0 (EnumConverter::convert, same rules as the loader).
// False: no step takes the string.
bool parse_filter_string(Call& c, std::string_view s, ObjectFilter& f) {
    const objects::GameObjectTypes* ty = types(c);
    if (!ty) return false;
    if (!f.have_props)
        if (const auto m = ty->enums().get(objects::EnumId::GameObjectProperties).convert(s)) {
            f.have_props = true;
            f.props = static_cast<uint64_t>(*m);
            return true;
        }
    if (!f.have_cat)
        if (const auto m = ty->enums().get(objects::EnumId::GameObjectCategory).convert(s)) {
            f.have_cat = true;
            f.cat = static_cast<uint64_t>(*m);
            return true;
        }
    if (!f.have_type) {
        const TypeIndex t = ty->raw().find(s);
        if (t != objects::kNone) {
            f.have_type = true;
            f.type = t;
            return true;
        }
    }
    return false;
}

// Representative type 0x3973b0: the host's (ObjectBridge::representative_type), else the store type.
TypeIndex representative_type(Call& c, const ObjectBridge* b, ObjectHandle h) {
    if (b && b->representative_type) {
        const TypeIndex t = b->representative_type(h);
        if (t != objects::kNone) return t;
    }
    return store(c)->type(h);
}

// Type / properties / category filters on representative type `rep` (the owner filter is the caller's).
bool type_filters_pass(const objects::GameObjectTypes& ty, const ObjectFilter& f, TypeIndex rep) {
    if (f.have_type && rep != f.type) return false;
    if (!f.have_props && !f.have_cat) return true;
    if (rep >= ty.raw().type_count()) return false;
    if (f.have_props && (f.props & ty.get_mask(rep, "Property_Flags")) == 0) return false;
    if (f.have_cat && (f.cat & ty.get_mask(rep, "CategoryMask")) == 0) return false;
    return true;
}

// ---------------------------------------------------------------- abilities

// UnitAbilityType (docs/re/OBJECT_TYPES.md §9.1.4), id = index.
constexpr std::string_view kAbilityNames[] = {
    "",                        "DEFEND",          "DEPLOY",          "INTERDICT",
    "HARASS",                  "BARRAGE",         "HUNT",            "SPREAD_OUT",
    "AFTERBURNER",             "TURBO",           "LURE",            "ROCKET_ATTACK",
    "AVOID_DANGER",            "FORCE_TELEKINESIS", "FORCE_WHIRLWIND", "FORCE_LIGHTNING",
    "FLAME_THROWER",           "JET_PACK",        "CAPTURE_VEHICLE", "TARGETED_INVULNERABILITY",
    "TARGETED_HACK",           "TARGETED_REPAIR", "STICKY_BOMB",     "AREA_EFFECT_STUN",
    "AREA_EFFECT_HEAL",        "AREA_EFFECT_CONVERT", "DISTRACT",    "CABLE_ATTACK",
    "FOW_REVEAL_PING",         "POWER_TO_WEAPONS", "TRACTOR_BEAM",   "ENERGY_WEAPON",
    "MISSILE_SHIELD",          "INVULNERABILITY", "LUCKY_SHOT",      "HARMONIC_BOMB",
    "UNTARGETED_STICKY_BOMB",  "CONCENTRATE_FIRE", "DEPLOY_TROOPERS", "SELF_DESTRUCT",
    "SPRINT",                  "ION_CANNON_SHOT", "WEAKEN_ENEMY",    "SPOILER_LOCK",
    "REPLENISH_WINGMEN",       "MAXIMUM_FIREPOWER", "EJECT_VEHICLE_THIEF", "FIRE_LOBBING_SUPERWEAPON",
    "STEALTH",                 "STUN",            "RADIOACTIVE_CONTAMINATE", "BERSERKER",
    "FORCE_SIGHT",             "SABER_THROW",     "FORCE_CLOAK",     "SUPER_LASER",
    "LASER_DEFENSE",           "FORCE_CONFUSE",   "LEECH_SHIELDS",   "TACTICAL_BRIBE",
    "SWAP_WEAPONS",            "CLUSTER_BOMB",    "FULL_SALVO",      "SENSOR_JAMMING",
    "PLACE_REMOTE_BOMB",       "DETONATE_REMOTE_BOMB", "INFECTION",  "PROXIMITY_MINES",
    "STIM_PACK",               "DRAIN_LIFE",      "BLAST",           "BUZZ_DROIDS",
    "SHIELD_FLARE",            "SUMMON",          "CORRUPT_SYSTEMS", "DEPLOY_SQUAD",
};
static_assert(std::size(kAbilityNames) == 76);

uint8_t ability_id(std::string_view name) { return unit_ability_id(name); }

// Unit ability ids of a type: the <Type> of every <Unit_Ability> in its Unit_Abilities_Data
// sub-object list, in list order.
std::vector<uint8_t> type_abilities(Call& c, TypeIndex t) {
    std::vector<uint8_t> out;
    const objects::GameObjectTypes* ty = types(c);
    if (!ty || t >= ty->raw().type_count()) return out;
    const objects::ObjectTypeDatabase& raw = ty->raw();
    const objects::FieldColumns& fc = raw.fields();
    for (const objects::FieldIndex sub : ty->get_sub_objects(t, "Unit_Abilities_Data")) {
        for (objects::FieldIndex f = sub + 1; f < fc.size() && fc.depth[f] > fc.depth[sub]; ++f) {
            if (fc.parent[f] != sub || !iequal(raw.tag_name(f), "Type")) continue;
            const uint8_t id = ability_id(fc.text[f]);
            if (id != 0) out.push_back(id);
        }
    }
    return out;
}
bool has_ability(Call& c, ObjectHandle h, uint8_t id) {
    for (const uint8_t a : type_abilities(c, store(c)->type(h)))
        if (a == id) return true;
    return false;
}

// P4 lua: the campaign Black Market gate (ABILITIES_HARDPOINTS 2.4, Black_Market_Allows_Ability 0x39bbd0) on the
// stage-2 path (no S9 ability system for the object; with one, AbilitySystem::is_ready / the commands apply it): the
// type's first <Unit_Ability> of that Type with Must_Be_Bought_On_Black_Market = yes is blocked unless the owner bought
// an item listing its GUI_Activated_Ability_Name (GalacticSim::bm_allows_unit_ability, P4_SIM_SYSTEMS 1.5). Campaign
// scripts only (skirmish always passes); team containers are checked on their own type [port].
bool black_market_ok(Call& c, ObjectHandle h, uint8_t id) {
    if (!c.vm->host.campaign) return true;
    const objects::GameObjectTypes* ty = types(c);
    const TypeIndex t = store(c)->type(h);
    if (!ty || t >= ty->raw().type_count()) return true;
    const objects::ObjectTypeDatabase& raw = ty->raw();
    const objects::FieldColumns& fc = raw.fields();
    auto trim = [](std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
        return s;
    };
    for (const objects::FieldIndex sub : ty->get_sub_objects(t, "Unit_Abilities_Data")) {
        bool match = false, must = false;
        std::string_view name;
        for (objects::FieldIndex f = sub + 1; f < fc.size() && fc.depth[f] > fc.depth[sub]; ++f) {
            if (fc.parent[f] != sub) continue;
            const std::string_view tag = raw.tag_name(f);
            if (iequal(tag, "Type")) match = ability_id(fc.text[f]) == id;
            else if (iequal(tag, "Must_Be_Bought_On_Black_Market")) {
                const std::string_view v = trim(fc.text[f]);
                must = iequal(v, "yes") || iequal(v, "true") || v == "1";
            } else if (iequal(tag, "GUI_Activated_Ability_Name")) {
                name = trim(fc.text[f]);
            }
        }
        if (match) return detail::black_market_allows(c, store(c)->owner(h), name, must);
    }
    return true;
}

// L1/S4 wave 2a: the S9 AbilitySystem of a tactical host (object_bridge.hpp), when it knows the object.
#if ALAMO_LUA_HAVE_ABILITIES
namespace ab = tactical::abilities;
ab::AbilitySystem* ability_system(Call& c, ObjectId id) {
    ObjectBridge* b = bridge(c);
    return (b && b->abilities && b->abilities->has_object(id)) ? b->abilities : nullptr;
}
uint32_t ability_frame(Call& c) {
    const ObjectBridge* b = bridge(c);
    return (b && b->ability_frame) ? b->ability_frame() : c.vm->frame;
}
// The battle's CombatSystem when it has the unit (Get_Hull / Get_Shield).
const tactical::combat::CombatSystem* combat_of(Call& c, ObjectId id) {
    const ObjectBridge* b = bridge(c);
    if (!b || !b->abilities) return nullptr;
    const tactical::combat::CombatSystem& cs = b->abilities->combat();
    return cs.has_unit(id) ? &cs : nullptr;
}
#endif

// ---------------------------------------------------------------- orders

#if ALAMO_LUA_HAVE_COMMANDS
using Event = commands::EventData;
uint32_t wire(ObjectId id) { return static_cast<uint32_t>(id); }
#else
struct Event {};
#endif

void put_u32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
}

// Lua-only orders (object_bridge.hpp LuaOrder): PortExtension with the 24-byte payload.
Event lua_order(LuaOrder kind, ObjectId object, ObjectId target, Vec3 p, uint32_t arg) {
    std::vector<uint8_t> data;
    put_u32(data, static_cast<uint32_t>(object));
    put_u32(data, static_cast<uint32_t>(target));
    put_u32(data, std::bit_cast<uint32_t>(p.x));
    put_u32(data, std::bit_cast<uint32_t>(p.y));
    put_u32(data, std::bit_cast<uint32_t>(p.z));
    put_u32(data, arg);
#if ALAMO_LUA_HAVE_COMMANDS
    return commands::ev::port_extension(static_cast<uint16_t>(kLuaOrderSubtype + static_cast<uint16_t>(kind)), data);
#else
    (void)kind;
    return {};
#endif
}

Event ev_move_to_object(objects::ModeKind mode, ObjectId self, ObjectId target) {
#if ALAMO_LUA_HAVE_COMMANDS
    // Galactic: the explicit single-object hop (MoveObjectToObject, COMMANDS.md §2.1 "AI/Lua moves").
    if (mode == objects::ModeKind::Galactic) return commands::ev::move_object_to_object(wire(self), wire(target));
    return commands::ev::move_to_object(wire(target), {wire(self)});
#else
    (void)mode; (void)self; (void)target;
    return {};
#endif
}
Event ev_attack(ObjectId self, ObjectId target) {
#if ALAMO_LUA_HAVE_COMMANDS
    return commands::ev::attack(wire(target), {wire(self)});
#else
    (void)self; (void)target;
    return {};
#endif
}
Event ev_facing(ObjectId self, float angle) {
#if ALAMO_LUA_HAVE_COMMANDS
    commands::EventData e(commands::EventType::Facing);
    e.set_f("angle", angle).set_ids("units", {wire(self)});
    return e;
#else
    (void)self; (void)angle;
    return {};
#endif
}
Event ev_garrison(ObjectId unit, ObjectId garrison) {
#if ALAMO_LUA_HAVE_COMMANDS
    commands::EventData e(commands::EventType::Garrison);
    e.set("unit", wire(unit)).set("garrison", wire(garrison));
    return e;
#else
    (void)unit; (void)garrison;
    return {};
#endif
}
Event ev_ability_mode(ObjectId self, uint8_t ability, bool on, ObjectId target, Vec3 p) {
#if ALAMO_LUA_HAVE_COMMANDS
    commands::EventData e(commands::EventType::SetUnitAbilityMode);
    e.set("ability", ability).set("on", on ? 1u : 0u).set("object", wire(target));
    e.set_f("x", p.x).set_f("y", p.y).set_f("z", p.z).set_ids("units", {wire(self)});
    return e;
#else
    (void)self; (void)ability; (void)on; (void)target; (void)p;
    return {};
#endif
}
Event ev_autofire(ObjectId self, uint8_t ability, bool on) {
#if ALAMO_LUA_HAVE_COMMANDS
    commands::EventData e(commands::EventType::SetAbilityAutofire);
    e.set_ids("units", {wire(self)}).set("ability", ability).set("on", on ? 1u : 0u);
    return e;
#else
    (void)self; (void)ability; (void)on;
    return {};
#endif
}
Event ev_sell(objects::ModeKind mode, ObjectId self) {
#if ALAMO_LUA_HAVE_COMMANDS
    commands::EventData e(mode == objects::ModeKind::Galactic ? commands::EventType::GalacticSell
                                                              : commands::EventType::TacticalSell);
    e.set("object", wire(self));
    return e;
#else
    (void)mode; (void)self;
    return {};
#endif
}

// Runs an order on the direct path. Returns the order serial (0 = no bridge: nothing issued).
// Issuer: the ordered object's owner (0 when it has none) [inferred: the original's Lua path sets
// the player of the event to the unit's owner].
uint32_t issue(Call& c, ObjectHandle h, const Event& e) {
    ObjectBridge* b = bridge(c);
    if (!b) return 0;
    const uint32_t serial = ++b->orders_issued;
    b->state(store(c)->id(h)).has_orders = true;
#if ALAMO_LUA_HAVE_COMMANDS
    if (b->pipeline) {
        const int32_t owner = store(c)->owner(h);
        const uint8_t player = (owner >= 0 && owner < commands::kMaxPlayerSlots) ? static_cast<uint8_t>(owner) : 0;
        b->pipeline->execute_direct(c.vm->frame, player, commands::CommandSource::Script, e);
    }
#else
    (void)e;
#endif
    return serial;
}

// BlockingStatus for BlockOnCommand: id = object, value = order serial (0 = nothing to wait for).
int push_block(Call& c, ObjectId object, uint32_t serial) {
    Ud u;
    u.cls = cls_blocking();
    u.id = object;
    u.value = serial;
    detail::push_engine_userdata(c.L, u);
    return 1;
}
int issue_and_block(Call& c, const Event& e) {
    const uint32_t serial = issue(c, self_object(c), e);
    return push_block(c, c.self->id, serial);
}

// ---------------------------------------------------------------- spawning

// Creates `type` (and, for a company, its Company_Units as contained members, list order) at `pos`.
// [inferred] creation is direct (no reinforcement / landing sequence); in galactic mode an object
// location becomes the container (fleet / planet). Facing: the location object's, else +X.
ObjectHandle spawn(Call& c, TypeIndex type, int32_t owner, Vec3 pos, ObjectHandle location, bool company) {
    objects::ObjectStore* st = store(c);
    objects::SpawnParams sp;
    sp.type = type;
    sp.owner = owner;
    sp.position = pos;
    sp.facing = location ? st->facing(location) : Vec3{1.0f, 0.0f, 0.0f};
    if (location && st->mode() == objects::ModeKind::Galactic) sp.container = location;
    const ObjectHandle h = st->create(sp);
    if (company && types(c)) {
        for (const TypeIndex m : types(c)->get_type_refs(type, "Company_Units")) {
            if (m == objects::kNone) continue;
            objects::SpawnParams ms = sp;
            ms.type = m;
            ms.container = h;
            st->create(ms);
        }
    }
    return h;
}

// ---------------------------------------------------------------- globals

// Find_Hint(type or type name, hint): the first object (newest first) of that type whose map hint
// matches (case-insensitive [inferred]); nil if none.
int g_find_hint(Call& c) {
    if (c.nargs != 2) return c.fail("LuaFindMarkerCommandClass -- Invalid number of parameters.");
    const TypeArg t = type_arg(c, 1);
    if (!t.given) return c.fail("LuaFindMarkerCommandClass -- Expected type or name for parameter 1");
    if (!c.is_string(2)) return c.fail("LuaFindMarkerCommandClass -- Expected a hint name for parameter 2");  // [inferred]
    const std::string_view hint = c.string(2);
    const objects::ObjectStore* st = store(c);
    const ObjectBridge* b = bridge(c);
    if (!st || !b) return push_nil(c);
    for (const ObjectHandle h : st->objects_newest_first()) {
        if (!st->active(h) || !type_matches(c, h, t.name)) continue;
        const ScriptObjectState* s = b->find_state(st->id(h));
        if (s && iequal(s->hint, hint)) {
            detail::push_object(c.L, *c.vm, h);
            return 1;
        }
    }
    return push_nil(c);
}

// Find_All_Objects_With_Hint(hint): every live object with that hint, newest first.
int g_find_all_with_hint(Call& c) {
    if (c.nargs != 1)
        return c.fail("Find_All_Objects_With_Hint -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    if (!c.is_string(1)) return c.fail("Find_All_Objects_With_Hint -- invalid type for parameter 1. Expected string.");
    const std::string_view hint = c.string(1);
    lua_newtable(c.L);
    const objects::ObjectStore* st = store(c);
    const ObjectBridge* b = bridge(c);
    if (!st || !b) return 1;
    int n = 0;
    for (const ObjectHandle h : st->objects_newest_first()) {
        if (!st->active(h)) continue;
        const ScriptObjectState* s = b->find_state(st->id(h));
        if (!s || !iequal(s->hint, hint)) continue;
        detail::push_object(c.L, *c.vm, h);
        lua_rawseti(c.L, -2, ++n);
    }
    return 1;
}

// Find_All_Objects_Of_Type(filter, [filter, ...]) (0x540b20, refixf t03). Arguments in any order: a PlayerWrapper
// sets the owner filter (a later one replaces it), a GameObjectTypeWrapper the type filter (a second type is an
// error), a string goes through parse_filter_string, anything else is an error. Errors ("unknown filter parameter %d"
// with the 1-based index, no argument, no filter at all) return nothing. Result: a table (1..n) of every object of the
// current mode's primary store, newest-added first, whose representative type passes the set filters (AND) and whose
// owner is the player when one is given. No alive-state, death-clone, hidden, parent, selectable or neutral tests:
// objects in the dying / death-clone state are returned like any other [H].
// [port] Objects already killed (ObjectStore::destroy, kFlagDead) but not yet flushed are skipped; the original wraps
// them as its shared dead wrapper in their slot (LUA_API.md [M] on how that reaches Lua).
// refix t04: in the galactic store a company object also stands for its Company_Units members (the original's cargo
// objects; vm_internal.hpp member views): each entry that passes the filters gives a member view right after the
// company's place in the walk [port order], with the company's owner and the entry type as its representative type.
int g_find_all_objects_of_type(Call& c) {
    if (c.nargs < 1) return script_error(c, "FindAllObjectsOfType -- Expected a filter parameter.");
    const objects::GameObjectTypes* ty = types(c);
    ObjectFilter f;
    for (int i = 1; i <= c.nargs; ++i) {
        if (const Ud* p = player_arg(c, i)) {
            if (p->id < 0) continue;  // a null player wrapper sets nothing [port: the port has none]
            f.have_owner = true;
            f.owner = p->id;
        } else if (const Ud* t = c.userdata(i, detail::cls_type())) {
            if (!ty || t->value >= ty->raw().type_count()) continue;  // a null type wrapper sets nothing
            if (f.have_type) return script_error(c, "FindAllObjectsOfType -- unknown filter parameter %d.", i);
            f.have_type = true;
            f.type = static_cast<TypeIndex>(t->value);
        } else if (!c.is_string(i) || !parse_filter_string(c, c.string(i), f)) {
            return script_error(c, "FindAllObjectsOfType -- unknown filter parameter %d.", i);
        }
    }
    if (!f.any()) return script_error(c, "FindAllObjectsOfType -- Invalid parameters.  You must pass a filter.");
    lua_newtable(c.L);
    const objects::ObjectStore* st = store(c);
    if (!st || !ty) return 1;
    const ObjectBridge* b = bridge(c);
    const bool members = st->mode() == objects::ModeKind::Galactic;
    int n = 0;
    for (const ObjectHandle h : st->objects_newest_first()) {
        if (!st->alive(h)) continue;  // [port] killed, not yet flushed (see above)
        if (f.have_owner && st->owner(h) != f.owner) continue;
        if (type_filters_pass(*ty, f, representative_type(c, b, h))) {
            detail::push_object(c.L, *c.vm, h);
            lua_rawseti(c.L, -2, ++n);
        }
        if (!members) continue;
        const std::vector<TypeIndex> refs = ty->get_type_refs(st->type(h), "Company_Units");
        for (uint32_t i = 0; i < refs.size(); ++i) {
            if (refs[i] == objects::kNone || !type_filters_pass(*ty, f, refs[i])) continue;
            detail::push_member_view(c.L, *c.vm, h, i);
            lua_rawseti(c.L, -2, ++n);
        }
    }
    return 1;
}

// Find_First_Object(type name) / FindPlanet(name) (one class, FindPlanetClass 0x6970a0, refixf t03): the name is a
// type name only (case-insensitive, first definition; no category / property names); an unknown name returns nothing
// without an error. The first object in the newest-first walk of the current mode's primary store whose OWN type is
// that type (not the representative type); no alive / death-clone / parent tests [H]. Extra arguments are ignored.
// FindPlanet is the same function: no planet test [H].
// [port] Killed objects pending the flush are skipped (Find_All_Objects_Of_Type above).
// refix t04: in the galactic store a company object also stands for its Company_Units members (vm_internal.hpp member
// views): a member type name finds the company's first matching entry.
int find_first(Call& c) {
    if (c.nargs < 1) return script_error(c, "FindPlanet -- No planet name set");
    if (!c.is_string(1)) return script_error(c, "FindPlanet -- Parameter 1 is not a valid string");
    const objects::ObjectStore* st = store(c);
    const objects::GameObjectTypes* ty = types(c);
    if (!st || !ty) return push_nil(c);
    const TypeIndex t = ty->raw().find(c.string(1));
    if (t == objects::kNone) return push_nil(c);
    const bool members = st->mode() == objects::ModeKind::Galactic;
    for (const ObjectHandle h : st->objects_newest_first()) {
        if (!st->alive(h)) continue;
        if (st->type(h) == t) {
            detail::push_object(c.L, *c.vm, h);
            return 1;
        }
        if (!members) continue;
        const std::vector<TypeIndex> refs = ty->get_type_refs(st->type(h), "Company_Units");
        for (uint32_t i = 0; i < refs.size(); ++i)
            if (refs[i] == t) {
                detail::push_member_view(c.L, *c.vm, h, i);
                return 1;
            }
    }
    return push_nil(c);
}
int g_find_first_object(Call& c) { return find_first(c); }
int g_find_planet(Call& c) { return find_first(c); }

// FindPlanet.Get_All_Planets(): every live object with a PLANET behaviour, newest first [inferred order].
int m_get_all_planets(Call& c) {
    lua_newtable(c.L);
    const objects::ObjectStore* st = store(c);
    if (!st) return 1;
    int n = 0;
    for (const ObjectHandle h : st->objects_newest_first()) {
        if (!st->active(h) || !st->has_behavior(h, objects::beh::Planet)) continue;
        detail::push_object(c.L, *c.vm, h);
        lua_rawseti(c.L, -2, ++n);
    }
    return 1;
}

// Find_Nearest(source, [filter_string], [player, friendly_bool]) (0x69e7c0, refixf t03).
//   * 1..4 arguments; tactical modes only; source = a live game object, an AI target or a task force (OI lua: the
//     target's object / the task force's first live unit, excluded from the candidates [port for a task force]).
//   * Argument 2 is the filter only when it is a string (parse_filter_string; unknown -> error, nothing returned);
//     the player (PlayerWrapper) and the bool are read only when BOTH slots exist, so (obj, "Infantry", player) and
//     (obj, player) ignore the player. Wrong types in those slots: error [inferred].
//   * Candidates [H]: the objects of each player in player order (owner id ascending), skipping players whose faction
//     Is_Neutral and, with a player P, keeping only players whose "allied or self" relation to P equals the bool; per
//     player the UNIT_AI objects (the perception grid list) newest first. Each must not be dying (+0x3A0 bit 0x40;
//     ObjectStore::active(), which also drops killed objects, no longer in the grid [port]), not be the source, have
//     the representative type if a type was given, a properties-mask hit if one was given, and a CategoryMask hit
//     against the category filter, which DEFAULTS TO ALL ONES (a representative type with CategoryMask 0 is never
//     found); with a player, not fogged for P (FOW_Object_Hidden, force = 1).
//   * Smallest squared 3-D distance, strict <: ties keep the lowest player, then the newest object.
// [port] Ownerless objects (owner -1) are in no player's list and never found; the player list is the owner ids; the
// "allied or self" relation is !ObjectBridge::is_enemy (self always); without a player world no player is neutral.
int g_find_nearest(Call& c) {
    if (c.nargs < 1 || c.nargs > 4)
        return script_error(c, "Find_Nearest - invalid number of parameters. Expected between 1 and 4, got %d.", c.nargs);
    objects::ObjectStore* st = store(c);
    if (!st || st->mode() == objects::ModeKind::Galactic)
        return script_error(c, "Find_Nearest - this function may only be used in tactical modes.");
    const Ud* src = c.userdata(1, detail::cls_game_object());
    // OI lua (A1 ask 2): an AI target -> its game object, a task force -> its first live unit; none -> nil (the rule
    // of the former Find_Nearest shim in the AI VMs).
    ObjectHandle ai_src{};
    if (!src && detail::ai_wrapper_object(c, 1, ai_src) && !ai_src) return push_nil(c);
    if (!src && !ai_src)
        return script_error(c, "Find_Nearest - invalid type for parameter 1; expected game object, ai target or taskforce.");
    if (src && !detail::object_alive(*c.vm, *src))
        return script_error(c, "Find_Nearest - parameter 1 is already dead; cannot extract a position from it.");
    ObjectFilter f;
    int player_index = 2;
    if (c.is_string(2)) {
        if (!parse_filter_string(c, c.string(2), f))
            return script_error(c, "Find_Nearest - unknown game object type, properties mask or catefory %s.",
                                std::string(c.string(2)).c_str());
        player_index = 3;
    }
    bool have_player = false, friendly = true;
    int32_t player = -1;
    if (player_index + 1 <= c.nargs) {
        const Ud* p = player_arg(c, player_index);
        if (!p) return script_error(c, "Find_Nearest - invalid type for parameter %d.", player_index);  // [inferred]
        if (!is_bool(c, player_index + 1))
            return script_error(c, "Find_Nearest - invalid type for parameter %d.", player_index + 1);  // [inferred]
        have_player = true;
        player = p->id;
        friendly = to_bool(c, player_index + 1);
    }
    const objects::GameObjectTypes* ty = types(c);
    if (!ty) return push_nil(c);
    const ObjectBridge* b = bridge(c);
#if ALAMO_LUA_PLAYERS
    const LuaPlayerWorld* world = c.vm->owner ? player_world(*c.vm->owner) : nullptr;
#endif
    static const objects::BehaviorTypeId unit_ai = objects::behavior_type_id("UNIT_AI");
    const uint64_t cat = f.have_cat ? f.cat : ~uint64_t{0};
    const ObjectHandle self = src ? detail::object_handle(*src) : ai_src;
    const Vec3 from = st->position(self);
    ObjectHandle best{};
    int32_t best_owner = 0;
    double best_d = 0.0;
    for (const ObjectHandle h : st->objects_newest_first()) {
        if (h == self || !st->active(h)) continue;
        const int32_t q = st->owner(h);
        if (q < 0 || !st->has_behavior(h, unit_ai)) continue;
#if ALAMO_LUA_PLAYERS
        if (world && world->is_neutral(q)) continue;
#endif
        if (have_player && (q == player || !is_enemy(c, player, q)) != friendly) continue;
        const TypeIndex rep = representative_type(c, b, h);
        if (rep >= ty->raw().type_count() || !type_filters_pass(*ty, f, rep)) continue;
        if ((cat & ty->get_mask(rep, "CategoryMask")) == 0) continue;
        if (have_player && detail::object_fogged_for(c, player, h, true)) continue;
        const double d = distance_sq(from, st->position(h));
        if (!best || d < best_d || (d == best_d && q < best_owner)) {
            best = h;
            best_owner = q;
            best_d = d;
        }
    }
    if (!best) return push_nil(c);
    detail::push_object(c.L, *c.vm, best);
    return 1;
}

// Spawn_Unit(type, location, player): a table with the spawned unit (a company: its container).
int g_spawn_unit(Call& c) {
    if (c.nargs != 3) return c.fail("LuaSpawnUnitCommandClass -- Invalid number of parameters.");
    const Ud* t = c.userdata(1, detail::cls_type());
    if (!t) return c.fail("LuaSpawnUnitCommandClass -- Invalid parameter for type.");
    const Ud* p = player_arg(c, 3);
    if (!p) return c.fail("LuaSpawnUnitCommandClass -- Invalid parameter for player.");
    Vec3 pos{};
    ObjectHandle loc{};
    if (position_arg(c, 2, pos, &loc) != PosArg::Ok)
        return c.fail("LuaSpawnUnitCommandClass -- Invalid parameter for location.");  // [inferred]
    lua_newtable(c.L);
    if (!store(c)) return 1;
    const ObjectHandle h = spawn(c, static_cast<TypeIndex>(t->value), p->id, pos, loc, true);
    detail::push_object(c.L, *c.vm, h);
    lua_rawseti(c.L, -2, 1);
    return 1;
}

// Create_Generic_Object(type or type name, position, player): one object, no company expansion
// [inferred]; nil for an unknown type name [inferred].
int g_create_generic_object(Call& c) {
    if (c.nargs != 3)
        return c.fail("Create_Generic_Object -- Invalid number of parameters. Expected 3, got %d.", c.nargs);
    const TypeArg t = type_arg(c, 1);
    if (!t.given) return c.fail("Create_Generic_Object -- Invalid type for parameter 1. Expected type or type name.");  // [inferred]
    Vec3 pos{};
    ObjectHandle loc{};
    if (position_arg(c, 2, pos, &loc) != PosArg::Ok)
        return c.fail("Create_Generic_Object -- Could not deduce a creation position from parameter 2.");
    const Ud* p = player_arg(c, 3);
    if (!p) return c.fail("Create_Generic_Object -- Invalid type for parameter 3. Expected player.");
    if (!store(c) || t.index == objects::kNone || !types(c) || t.index >= types(c)->raw().type_count())
        return push_nil(c);
    detail::push_object(c.L, *c.vm, spawn(c, t.index, p->id, pos, loc, false));
    return 1;
}

// Hide_Object(object, number): non-zero hides [inferred], like GameObjectWrapper.Hide.
int g_hide_object(Call& c) {
    if (c.nargs != 2) return c.fail("LuaHideObject -- Expected gameobject pointer parameter 1, float parameter 2");
    const Ud* o = c.userdata(1, detail::cls_game_object());
    if (!o || !detail::object_alive(*c.vm, *o)) return c.fail("LuaHideObject -- Invalid parameter 1, expected a game object");
    if (!c.is_number(2)) return c.fail("LuaHideObject -- Invalid parameter 2, expected a float");
    if (ScriptObjectState* s = edit_state(c, o->id)) s->hidden = c.number(2) != 0.0;
    return 0;
}

int g_create_position(Call& c) {
    if (c.nargs != 3) return c.fail("LuaCreatePosition -- Expected 3 float parameters");
    if (!c.is_number(1) || !c.is_number(2) || !c.is_number(3))
        return c.fail("LuaCreatePosition -- Invalid parameter, expected 3 floats");
    push_position(c.L, {static_cast<float>(c.number(1)), static_cast<float>(c.number(2)), static_cast<float>(c.number(3))});
    return 1;
}

// ---------------------------------------------------------------- PositionWrapper / BlockingStatus

int m_position_get_xyz(Call& c) {  // three numbers [inferred]
    const Vec3 p = position_of(*c.self);
    lua_pushnumber(c.L, static_cast<double>(p.x));
    lua_pushnumber(c.L, static_cast<double>(p.y));
    lua_pushnumber(c.L, static_cast<double>(p.z));
    return 3;
}
int m_true(Call& c) { return push_bool(c, true); }

// Ud::reserved of a BlockingStatus: what it waits on (vm_internal.hpp kinds). 0 (default): an order,
// value = order serial, id = ordered object; kBlockReinforcement: value = ObjectBridge reinforcement
// request id; kBlockForever: never finishes (BlockForever); galactic waits: the players family.
using detail::kBlockForever;
using detail::kBlockReinforcement;

int m_block_is_finished(Call& c) {
    const ObjectBridge* b = bridge(c);
    if (c.self->reserved == kBlockForever) return push_bool(c, false);
    if (c.self->reserved == kBlockBombingRun) {  // L1 final: BombingBlockStatus (script_remaining.hpp)
        const RemainingScriptHooks* hk = b ? b->remaining : nullptr;
        return push_bool(c, !(hk && hk->bombing_run_finished) || hk->bombing_run_finished(c.self->id));
    }
    if (c.self->reserved > kBlockForever) return push_bool(c, detail::galactic_block_finished(c));
    if (c.self->reserved == kBlockReinforcement) {
        const ReinforceState* r = b ? b->reinforcement(static_cast<uint32_t>(c.self->value)) : nullptr;
        return push_bool(c, !r || r->finished);
    }
    const auto serial = static_cast<uint32_t>(c.self->value);
    if (serial == 0) return push_bool(c, true);
    if (b && b->order_finished) return push_bool(c, b->order_finished(c.self->id, serial));
    return push_bool(c, true);  // PROVISIONAL: orders complete at once (object_bridge.hpp)
}
// Result(): orders give nil; a reinforcement gives the table of its new units once finished (live
// ones, creation order) and nil before.
int m_block_result(Call& c) {
    const ObjectBridge* b = bridge(c);
    if (c.self->reserved == kBlockBombingRun) {  // L1 final: undamaged / total bombers ([port] 1 without a host)
        const RemainingScriptHooks* hk = b ? b->remaining : nullptr;
        return push_num(c, (hk && hk->bombing_run_result) ? static_cast<double>(hk->bombing_run_result(c.self->id)) : 1.0);
    }
    if (c.self->reserved != kBlockReinforcement) return push_nil(c);
    const ReinforceState* r = b ? b->reinforcement(static_cast<uint32_t>(c.self->value)) : nullptr;
    if (!r || !r->finished) return push_nil(c);
    lua_newtable(c.L);
    const objects::ObjectStore* st = store(c);
    int n = 0;
    for (const ObjectId id : r->units) {
        const ObjectHandle h = st ? st->find(id) : ObjectHandle{};
        if (!h || !st->alive(h)) continue;
        detail::push_object(c.L, *c.vm, h);
        lua_rawseti(c.L, -2, ++n);
    }
    return 1;
}

// ---------------------------------------------------------------- GameObjectWrapper: queries

int m_get_position(Call& c) {
    push_position(c.L, store(c)->position(self_object(c)));
    return 1;
}

int m_get_distance(Call& c) {
    if (c.nargs != 1)
        return c.fail("GameObjectWrapper::Get_Distance -- invalid number of parameters. Expceted 1, got %d.", c.nargs);
    Vec3 p{};
    if (position_arg(c, 1, p) != PosArg::Ok)
        return c.fail("GameObjectWrapper::Get_Distance -- could not extract a position from parameter 1.");
    return push_num(c, distance(store(c)->position(self_object(c)), p));
}

int m_get_name(Call& c) { return push_str(c, object_type_name(c, self_object(c))); }  // [inferred] type name

// Hull: health / Tactical_Health (1 when the type has no health) [inferred]; Get_Health the same.
// Get_Hull: S9 Get_Min_Health_Fraction (CombatSystem::script_hull, ABILITIES_HARDPOINTS.md §1.3) for units of
// the host's combat system (wave 2a); else store health / Tactical_Health [inferred].
int m_get_hull(Call& c) {
#if ALAMO_LUA_HAVE_ABILITIES
    if (const tactical::combat::CombatSystem* cs = combat_of(c, c.self->id))
        return push_num(c, static_cast<double>(cs->script_hull(c.self->id)));
#endif
    const ObjectHandle h = self_object(c);
    const objects::GameObjectTypes* ty = types(c);
    const TypeIndex t = store(c)->type(h);
    const float max = (ty && t < ty->columns().tactical_health.size()) ? ty->columns().tactical_health[t] : 0.0f;
    if (!(max > 0.0f)) return push_num(c, 1.0);
    return push_num(c, static_cast<double>(store(c)->health(h)) / static_cast<double>(max));
}

// Shield fraction: CombatSystem::shield_fraction for combat units (§5: current / max clamped to [0, 1], 0
// without shields; wave 2a); else script state, else full when the type has shields [inferred].
int m_get_shield(Call& c) {
#if ALAMO_LUA_HAVE_ABILITIES
    if (const tactical::combat::CombatSystem* cs = combat_of(c, c.self->id))
        return push_num(c, static_cast<double>(cs->shield_fraction(c.self->id)));
#endif
    const ScriptObjectState& s = state_of(c, c.self->id);
    if (s.shield >= 0.0f) return push_num(c, static_cast<double>(s.shield));
    const objects::GameObjectTypes* ty = types(c);
    const TypeIndex t = store(c)->type(self_object(c));
    const bool shields = ty && t < ty->columns().shield_points.size() && ty->columns().shield_points[t] > 0.0f;
    return push_num(c, shields ? 1.0 : 0.0);
}

int m_is_category(Call& c) {
    if (c.nargs != 1)
        return c.fail("GameObjectWrapper::Is_Category -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    if (!c.is_string(1)) return c.fail("GameObjectWrapper::Is_Category -- parameter 1 is not a valid string.");
    const objects::GameObjectTypes* ty = types(c);
    if (!ty) return push_bool(c, false);
    const auto m = ty->enums().get(objects::EnumId::GameObjectCategory).convert(c.string(1));
    if (!m || *m == 0) return push_bool(c, false);  // unknown category: false [inferred]
    return push_bool(c, (ty->get_mask(store(c)->type(self_object(c)), "CategoryMask") & static_cast<uint64_t>(*m)) != 0);
}

int m_has_property(Call& c) {
    if (c.nargs != 1)
        return c.fail("GameObjectWrapper::Has_Property -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    if (!c.is_string(1)) return c.fail("GameObjectWrapper::Has_Property -- invalid type for parameter 1. Expected string.");
    const objects::GameObjectTypes* ty = types(c);
    if (!ty) return push_bool(c, false);
    const auto m = ty->enums().get(objects::EnumId::GameObjectProperties).convert(c.string(1));
    if (!m || *m == 0) return push_bool(c, false);
    return push_bool(c, (ty->get_mask(store(c)->type(self_object(c)), "Property_Flags") & static_cast<uint64_t>(*m)) != 0);
}

int m_get_game_scoring_type(Call& c) {  // [inferred] the object's own type
    detail::push_type(c.L, *c.vm, store(c)->type(self_object(c)));
    return 1;
}

// ParentContainerObject unless it is dead (OBJECT_RUNTIME.md §3).
int m_get_parent_object(Call& c) {
    const objects::ObjectStore* st = store(c);
    // refix t04 (L1P4_LUA_GAPS 1.3): a company member view's parent is the company object itself (the original's
    // transport carrying that member), so smuggler.Get_Parent_Object().Activate_Ability() fires only that company.
    if (detail::is_member_view(*c.self)) {
        detail::push_object(c.L, *c.vm, self_object(c));
        return 1;
    }
    const ObjectHandle p = container_of(c, self_object(c));
    if (!p || !st->alive(p)) return push_nil(c);
    detail::push_object(c.L, *c.vm, p);
    return 1;
}

// Climbs the container chain to the first object with a PLANET behaviour; galactic only (§3).
int m_get_planet_location(Call& c) {
    const objects::ObjectStore* st = store(c);
    if (st->mode() != objects::ModeKind::Galactic) return push_nil(c);
    ObjectHandle h = container_of(c, self_object(c));
    for (uint32_t guard = 0; h && guard < 64; ++guard, h = container_of(c, h)) {
        if (!st->alive(h)) return push_nil(c);
        if (st->has_behavior(h, objects::beh::Planet)) {
            detail::push_object(c.L, *c.vm, h);
            return 1;
        }
    }
    return push_nil(c);
}

int m_get_hint(Call& c) {
    const ScriptObjectState& s = state_of(c, c.self->id);
    if (s.hint.empty()) return push_nil(c);
    return push_str(c, s.hint);
}

int m_is_selectable(Call& c) {
    const ScriptObjectState& s = state_of(c, c.self->id);
    if (s.selectable >= 0) return push_bool(c, s.selectable != 0);
    return push_bool(c, store(c)->has_behavior(self_object(c), objects::beh::Selectable));
}

int m_is_transport(Call& c) { return push_bool(c, store(c)->has_behavior(self_object(c), objects::beh::Transport)); }

int m_can_move(Call& c) {  // [inferred] has a locomotor and it is not suspended
    return push_bool(c, store(c)->has_behavior(self_object(c), objects::beh::Locomotor) &&
                            !state_of(c, c.self->id).locomotor_suspended);
}

int m_get_contained_object_count(Call& c) {
    return push_num(c, static_cast<double>(contents_of(c, self_object(c)).size()));
}

int m_contains_object_type(Call& c) {
    if (c.nargs != 1)
        return c.fail("GameObjectWrapper::Contains_Object_Type -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    const Ud* t = c.userdata(1, detail::cls_type());
    if (!t) return c.fail("GameObjectWrapper::Contains_Object_Type -- invalid type for parameter 1. Expected game object type.");
    const std::string_view name = type_name(c, static_cast<TypeIndex>(t->value));
    for (const ObjectHandle h : contents_of(c, self_object(c)))
        if (type_matches(c, h, name)) return push_bool(c, true);
    return push_bool(c, false);
}

int m_has_active_orders(Call& c) { return push_bool(c, state_of(c, c.self->id).has_orders); }

int m_get_attack_target(Call& c) {
    const objects::ObjectStore* st = store(c);
    const ObjectHandle t = st->find(state_of(c, c.self->id).attack_target);
    if (!t || !st->alive(t)) return push_nil(c);
    detail::push_object(c.L, *c.vm, t);
    return 1;
}
int m_has_attack_target(Call& c) {
    const objects::ObjectStore* st = store(c);
    const ObjectHandle t = st->find(state_of(c, c.self->id).attack_target);
    return push_bool(c, t && st->alive(t));
}

// ---------------------------------------------------------------- abilities

uint8_t ability_param(Call& c, int i) { return c.is_string(i) ? ability_id(c.string(i)) : 0; }

int m_has_ability(Call& c) {
    if (c.nargs != 1)
        return c.fail("GameObjectWrapper::Has_Ability -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    if (!c.is_string(1)) return c.fail("GameObjectWrapper::Has_Ability -- invalid type for ability 1. Expected string.");
    const uint8_t id = ability_param(c, 1);
#if ALAMO_LUA_HAVE_ABILITIES
    if (ab::AbilitySystem* a = ability_system(c, c.self->id)) return push_bool(c, id != 0 && a->has_ability(c.self->id, id));
#endif
    return push_bool(c, id != 0 && has_ability(c, self_object(c), id));
}

const AbilityState* ability_state(Call& c, uint8_t id) {
    const ScriptObjectState& s = state_of(c, c.self->id);
    const auto it = s.abilities.find(id);
    return it == s.abilities.end() ? nullptr : &it->second;
}

// S9: Can_Activate(obj, t, 1) or pending as a targeted ability (ABILITIES_HARDPOINTS.md §5). Without an
// ability system: the type has it and it is not running [inferred].
int m_is_ability_ready(Call& c) {
    if (c.nargs != 1)
        return c.fail("GameObjectWrapper::Is_Ability_Ready -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    if (!c.is_string(1)) return c.fail("GameObjectWrapper::Is_Ability_Ready -- invalid type for ability 1. Expected string.");
    const uint8_t id = ability_param(c, 1);
#if ALAMO_LUA_HAVE_ABILITIES
    if (ab::AbilitySystem* a = ability_system(c, c.self->id))
        return push_bool(c, id != 0 && a->is_ready(c.self->id, id, ability_frame(c)));
#endif
    if (id == 0 || !has_ability(c, self_object(c), id)) return push_bool(c, false);
    if (!black_market_ok(c, self_object(c), id)) return push_bool(c, false);  // P4 lua (0x57b580)
    const AbilityState* a = ability_state(c, id);
    return push_bool(c, !(a && a->active));
}

int m_is_ability_active(Call& c) {
    if (c.nargs != 1)
        return c.fail("GameObjectWrapper::Is_Ability_Active -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    if (!c.is_string(1)) return c.fail("GameObjectWrapper::Is_Ability_Active -- invalid type for parameter 1. Expected string.");
#if ALAMO_LUA_HAVE_ABILITIES
    if (ab::AbilitySystem* a = ability_system(c, c.self->id)) {
        const uint8_t id = ability_param(c, 1);
        return push_bool(c, id != 0 && a->is_active(c.self->id, id));
    }
#endif
    const AbilityState* a = ability_state(c, ability_param(c, 1));
    return push_bool(c, a && a->active);
}

int m_is_ability_autofire(Call& c) {
    if (c.nargs != 1)
        return c.fail("GameObjectWrapper::Is_Ability_Autofire -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    if (!c.is_string(1))
        return c.fail("GameObjectWrapper::Set_Single_Ability_Autofire -- invalid type for parameter 1. Expected string.");
#if ALAMO_LUA_HAVE_ABILITIES
    if (ab::AbilitySystem* a = ability_system(c, c.self->id)) {
        const uint8_t id = ability_param(c, 1);
        return push_bool(c, id != 0 && a->is_autofire(c.self->id, id));
    }
#endif
    const AbilityState* a = ability_state(c, ability_param(c, 1));
    return push_bool(c, a && a->autofire);
}

// Autofire setters write the slot byte directly (§5, 0x3a78b0); without an ability system: script state +
// the SetAbilityAutofire event on the direct path.
void set_autofire(Call& c, uint8_t id, bool on) {
#if ALAMO_LUA_HAVE_ABILITIES
    if (ab::AbilitySystem* a = ability_system(c, c.self->id)) {
        const ObjectId self[] = {c.self->id};
        a->set_autofire(self, id, on);
        return;
    }
#endif
    if (ScriptObjectState* s = edit_self(c)) s->abilities[id].autofire = on;
    issue(c, self_object(c), ev_autofire(c.self->id, id, on));
}

int m_set_single_ability_autofire(Call& c) {
    if (c.nargs != 2)
        return c.fail("GameObjectWrapper::Set_Single_Ability_Autofire -- invalid number of parameters. Expected 2, got %d.", c.nargs);
    if (!c.is_string(1))
        return c.fail("GameObjectWrapper::Set_Single_Ability_Autofire -- invalid type for ability 1. Expected string.");
    if (!is_bool(c, 2))
        return c.fail("GameObjectWrapper::Set_Single_Ability_Autofire -- invalid type for parameter 2. Expected bool.");
    const uint8_t id = ability_param(c, 1);
    if (id == 0) return 0;
    set_autofire(c, id, to_bool(c, 2));
    return 0;
}

// Activate_Ability argument families (ABILITIES_HARDPOINTS.md §2.9 table) [H for the lists].
// Only the S9 path of Activate_Ability uses it: built with ALAMO_LUA_HAVE_ABILITIES only (no unused function
// in builds without engine/tactical).
#if ALAMO_LUA_HAVE_ABILITIES
enum class AbilityArg : uint8_t { Toggle, Object, Position, ModePosition, OptionalTarget };
AbilityArg ability_arg_family(uint8_t t) {
    static constexpr uint8_t kObject[] = {13, 15, 16, 18, 19, 20, 21, 22, 27, 30, 31, 34, 37, 41, 45, 51, 53, 55, 58, 59, 64, 66};
    static constexpr uint8_t kPosition[] = {14, 17, 23, 24, 25, 35, 36, 38, 44, 46, 49, 57, 61, 72};
    static constexpr uint8_t kModePosition[] = {5, 26, 28, 42};
    static constexpr uint8_t kOptional[] = {6, 10, 12};
    for (const uint8_t x : kObject)
        if (x == t) return AbilityArg::Object;
    for (const uint8_t x : kPosition)
        if (x == t) return AbilityArg::Position;
    for (const uint8_t x : kModePosition)
        if (x == t) return AbilityArg::ModePosition;
    for (const uint8_t x : kOptional)
        if (x == t) return AbilityArg::OptionalTarget;
    return AbilityArg::Toggle;  // the none / bool list, DEPLOY, and types the table does not list [inferred]
}
#endif

// Activate_Ability([name], [on | target object | target position]) -> BlockingStatus.
// With the S9 ability system (executed at once, §2.9; the BlockingStatus is finished): toggle family ->
// SetUnitAbilityMode(t, on); object family -> command 40 (targeted_ability; no live target object: nothing);
// position family -> command 57 on the host's Dummy_Barrage_Target (spawn_ability_target; an object argument
// gives its position) [inferred: without the hook, the event below]; position-by-43 and optional-target
// families -> SetUnitAbilityMode(t, on, target object if any).
// Otherwise: SetUnitAbilityMode on the direct path (COMMANDS.md §2.2 / §5). No name: the type's first unit
// ability [inferred]. An ability the unit does not have is a no-op [inferred].
int m_activate_ability(Call& c) {
    // P4 lua: in a galactic script the Ground_Activated hero trigger (Activate_Ability_Galactic 0x570670,
    // P4_SIM_SYSTEMS 6.1) through the players family; returns nothing (L1P4_LUA_GAPS 1.2 item 4).
    if (detail::galactic_activate_ability(c)) return 0;
    const ObjectHandle self = self_object(c);
    uint8_t id = 0;
    if (c.has(1) && c.is_string(1)) {
        id = ability_param(c, 1);
    } else if (!c.has(1)) {
        const std::vector<uint8_t> list = type_abilities(c, store(c)->type(self));
        if (!list.empty()) id = list.front();
    } else {
        return c.fail("GameObjectWrapper::Activate_Ability -- invalid type for parameter 1. Expected string.");  // [inferred]
    }
    bool on = true;
    ObjectId target = objects::kNoObjectId;
    Vec3 p{};
    bool has_point = false;
    if (c.has(2)) {
        if (is_bool(c, 2)) {
            on = to_bool(c, 2);
        } else {
            ObjectHandle th{};
            if (position_arg(c, 2, p, &th) != PosArg::Ok) return push_block(c, c.self->id, 0);  // dead target: no-op
            if (th) target = store(c)->id(th);
            has_point = true;
        }
    }
#if ALAMO_LUA_HAVE_ABILITIES
    if (ab::AbilitySystem* a = ability_system(c, c.self->id)) {
        if (id == 0 || !a->has_ability(c.self->id, id)) return push_block(c, c.self->id, 0);
        const uint32_t frame = ability_frame(c);
        const ObjectId users[] = {c.self->id};
        switch (ability_arg_family(id)) {
            case AbilityArg::Object:
                if (target != objects::kNoObjectId) a->targeted_ability(users, id, target, -1, frame);
                break;
            case AbilityArg::Position: {
                if (!has_point) break;
                const ObjectBridge* b = bridge(c);
                if (b && b->spawn_ability_target) {
                    const ObjectId dummy = b->spawn_ability_target(p, store(c)->owner(self));
                    if (dummy != objects::kNoObjectId) a->targeted_ability(users, id, dummy, -1, frame);
                    break;
                }
                return issue_and_block(c, ev_ability_mode(c.self->id, id, true, target, p));
            }
            case AbilityArg::Toggle:
            case AbilityArg::ModePosition:
            case AbilityArg::OptionalTarget:
                a->set_unit_ability_mode(users, id, on, frame, target);
                break;
        }
        return push_block(c, c.self->id, 0);
    }
#else
    (void)has_point;
#endif
    if (id == 0 || !has_ability(c, self, id)) return push_block(c, c.self->id, 0);
    if (!black_market_ok(c, self, id)) return push_block(c, c.self->id, 0);  // P4 lua: BM gate (0x570a50)
    if (ScriptObjectState* s = edit_self(c)) s->abilities[id].active = on;
    return issue_and_block(c, ev_ability_mode(c.self->id, id, on, target, p));
}

// Force_Ability_Recharge(name [, seconds]): without seconds the recharge starts (§2.6); with seconds
// counter = maximum = int(FPS x s), phase unchanged (§5). No ability system: nothing to recharge.
int m_force_ability_recharge(Call& c) {
    if (c.nargs < 1 || c.nargs > 2)
        return c.fail("GameObjectWrapper::Force_Ability_Recharge -- invalid number of parameters. Expected 1 or 2, got %d.", c.nargs);
    if (!c.is_string(1)) return c.fail("GameObjectWrapper::Force_Ability_Recharge -- invalid type for parameter 1. Expected string.");
    if (c.nargs == 2 && !c.is_number(2))
        return c.fail("GameObjectWrapper::Force_Ability_Recharge -- invalid type for parameter 2. Expected number.");
#if ALAMO_LUA_HAVE_ABILITIES
    if (ab::AbilitySystem* a = ability_system(c, c.self->id)) {
        const uint8_t id = ability_param(c, 1);
        if (id != 0 && a->has_ability(c.self->id, id))
            a->force_recharge(c.self->id, id, c.nargs == 2 ? static_cast<float>(c.number(2)) : -1.0f);
    }
#endif
    return 0;
}

// Reset_Ability_Counter(name): counter = maximum = 1 frame when the counter is zero [M] (§5).
int m_reset_ability_counter(Call& c) {
    if (c.nargs != 1)  // [inferred] message format (the doc lists none)
        return c.fail("GameObjectWrapper::Reset_Ability_Counter -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    if (!c.is_string(1)) return c.fail("GameObjectWrapper::Reset_Ability_Counter -- invalid type for parameter 1. Expected string.");
#if ALAMO_LUA_HAVE_ABILITIES
    if (ab::AbilitySystem* a = ability_system(c, c.self->id)) {
        const uint8_t id = ability_param(c, 1);
        if (id != 0) a->reset_counter(c.self->id, id);
    }
#endif
    return 0;
}

// Is_Under_Effects_Of_Ability(name) (L1/S4 wave 2b, SCRIPT_GAPS.md §2.1 [H]; replaces the wave-2a [inferred]
// mapping): only two abilities are supported - TRACTOR_BEAM (30): the victim's tractor flag (CD +0x132, S9
// tractored); ION_CANNON_SHOT (41): the ION_STUN_EFFECT behaviour is active (S9 / combat ion_stunned; without the
// effect false). Any other UnitAbilityType name, or an unknown one, is a script error (logged, no value).
constexpr uint8_t kTractorBeamId = 30, kIonCannonShotId = 41;
int m_is_under_effects_of_ability(Call& c) {
    if (c.nargs != 1)
        return c.fail("GameObjectWrapper::Is_Under_Effects_Of_Ability -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    if (!c.is_string(1))
        return c.fail("GameObjectWrapper::Is_Under_Effects_Of_Ability -- invalid type for parameter 1. Expected string.");
    const uint8_t id = ability_param(c, 1);
    if (id == 0) {
        c.vm->message("SCRIPT ERROR: GameObjectWrapper::Is_Under_Effects_Of_Ability -- unknown ability type " + std::string(c.string(1)));
        return 0;
    }
    if (id != kTractorBeamId && id != kIonCannonShotId) {
        c.vm->message("SCRIPT ERROR: GameObjectWrapper::Is_Under_Effects_Of_Ability -- ability " + std::string(c.string(1)) +
                      " is not yet supported");
        return 0;
    }
#if ALAMO_LUA_HAVE_ABILITIES
    static_assert(ab::kTractorBeam == kTractorBeamId && ab::kIonCannonShot == kIonCannonShotId);
    if (ab::AbilitySystem* a = ability_system(c, c.self->id)) {
        const ObjectId self = c.self->id;
        if (id == kTractorBeamId) return push_bool(c, a->tractored(self));
        return push_bool(c, a->combat().ion_stunned(self, ability_frame(c)));
    }
#endif
    return push_bool(c, false);  // no ability system: never tractored, no ion-stun effect
}

// Are_Engines_Online() (wave 2b, SCRIPT_GAPS.md §2.2 [H]): no locomotor data -> true; else the locomotor engines
// flag (+0xd0), false while the engines are disabled - permanently once an engine hard point is destroyed, or for
// a timed disable. Sources in order: the host's locomotor flag (TacticalScriptHooks::engines_online), then S9: the
// last ENGINE hard point destroyed (the permanent case); otherwise online.
int m_are_engines_online(Call& c) {
    if (const ObjectBridge* b = bridge(c); b && b->tactical && b->tactical->engines_online) {
        const int32_t v = b->tactical->engines_online(self_object(c));
        if (v >= 0) return push_bool(c, v != 0);
    }
#if ALAMO_LUA_HAVE_ABILITIES
    if (ab::AbilitySystem* a = ability_system(c, c.self->id)) {
        const tactical::combat::CombatSystem& cs = a->combat();
        const ObjectId self = c.self->id;
        if (!cs.has_unit(self)) return push_bool(c, true);
        bool any = false;
        for (uint32_t i = 0; i < cs.hardpoint_count(self); ++i)
            any = any || cs.hardpoint_kind(self, i) == tactical::combat::HardpointKind::Engine;
        return push_bool(c, !any || cs.live_hardpoints(self, tactical::combat::HardpointKind::Engine) > 0);
    }
#endif
    return push_bool(c, true);
}

// ---------------------------------------------------------------- orders

int m_move_to(Call& c) {
    Vec3 p{};
    ObjectHandle target{};
    if (position_arg(c, 1, p, &target) != PosArg::Ok)
        return c.fail("GameObjectWrapper::Move_To -- could not extract a position from parameter 1.");  // [inferred]
    // REMAINING_GAPS §4.2: Move_Internal's optional numeric threat threshold T (argument 2 [M]), carried as the order's
    // arg (float bits; 0 = off). [port] with T an object target becomes a move to its current position.
    if (c.is_number(2)) {
        const ObjectId tid = target ? store(c)->id(target) : objects::kNoObjectId;
        const float t = static_cast<float>(c.number(2));
        return issue_and_block(c, lua_order(LuaOrder::MoveTo, c.self->id, tid, p, t != 0.0f ? std::bit_cast<uint32_t>(t) : 0u));
    }
    if (target) return issue_and_block(c, ev_move_to_object(store(c)->mode(), c.self->id, store(c)->id(target)));
    return issue_and_block(c, lua_order(LuaOrder::MoveTo, c.self->id, objects::kNoObjectId, p, 0));
}

int m_attack_move(Call& c) {
    Vec3 p{};
    ObjectHandle target{};
    if (position_arg(c, 1, p, &target) != PosArg::Ok)
        return c.fail("GameObjectWrapper::Attack_Move -- could not extract a position from parameter 1.");  // [inferred]
    const ObjectId tid = target ? store(c)->id(target) : objects::kNoObjectId;
    return issue_and_block(c, lua_order(LuaOrder::AttackMove, c.self->id, tid, p, 0));
}

int m_attack_target(Call& c) {
    if (c.nargs != 1)
        return c.fail("GameObjectWrapper::Attack_Target -- invalid number of parameters. Expected %d, got %d.", 1, c.nargs);
    const Ud* t = c.userdata(1, detail::cls_game_object());
    if (!t || !detail::object_alive(*c.vm, *t)) {
        const std::string name(object_type_name(c, self_object(c)));
        return c.fail("GameObjectWrapper::Attack_Target -- Parameter 1 is not a valid destination for unit %s", name.c_str());
    }
    if (ScriptObjectState* s = edit_self(c)) s->attack_target = t->id;
    return issue_and_block(c, ev_attack(c.self->id, t->id));
}

int m_guard_target(Call& c) {
    Vec3 p{};
    ObjectHandle target{};
    if (position_arg(c, 1, p, &target) != PosArg::Ok)
        return c.fail("GameObjectWrapper::Guard_Target -- could not extract a position from parameter 1.");  // [inferred]
    const ObjectId tid = target ? store(c)->id(target) : objects::kNoObjectId;
    return issue_and_block(c, lua_order(LuaOrder::Guard, c.self->id, tid, p, 0));
}

int m_stop(Call& c) {
    const uint32_t serial = issue(c, self_object(c), lua_order(LuaOrder::Stop, c.self->id, objects::kNoObjectId, {}, 0));
    if (ScriptObjectState* s = edit_self(c)) {
        s->has_orders = false;
        s->attack_target = objects::kNoObjectId;
    }
    return push_block(c, c.self->id, serial);
}

int m_turn_to_face(Call& c) {
    if (c.nargs != 1)
        return c.fail("GameObjectWrapper::Turn_To_Face -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    Vec3 p{};
    if (position_arg(c, 1, p) != PosArg::Ok)
        return c.fail("GameObjectWrapper::Turn_To_Face -- could not extract a position from parameter 1.");  // [inferred]
    return issue_and_block(c, ev_facing(c.self->id, heading_deg(store(c)->position(self_object(c)), p)));
}

int m_hyperspace_away(Call& c) {
    if (c.nargs > 1)
        return c.fail("GameObjectWrapper::Hyperspace_Away -- invalid number of parameters. Expected 0 or 1, got %d.", c.nargs);
    // L1 final (L1_S4_REMAINING §3.4): with the host's locomotor call, Hyperspace_Away([bool = true]) = +0x2a8(bool, 0)
    // at once (space only, a locomotor required); returns nothing.
    if (const ObjectBridge* b = bridge(c); b && b->tactical && b->tactical->hyperspace_out_link) {
        if (store(c)->mode() != objects::ModeKind::Space) return c.fail("GameObjectWrapper::Hyperspace_Away -- only valid in space mode");
        if (!store(c)->has_behavior(self_object(c), objects::beh::Locomotor))
            return c.fail("GameObjectWrapper::Hyperspace_Away -- object has no locomotor");
        b->tactical->hyperspace_out_link(self_object(c), 0, c.nargs == 1 ? lua_toboolean(c.L, c.idx(1)) != 0 : true);
        return 0;
    }
    const uint32_t arg = (c.nargs == 1 && to_bool(c, 1)) ? 1u : 0u;
    return issue_and_block(c, lua_order(LuaOrder::HyperspaceAway, c.self->id, objects::kNoObjectId, {}, arg));
}

// Cancel_Hyperspace() (wave 2a): a Lua order on the direct path [inferred: no argument, returns nothing]; the
// host's locomotor / reinforcement code stops a running hyperspace jump (none modelled yet).
int m_cancel_hyperspace(Call& c) {
    issue(c, self_object(c), lua_order(LuaOrder::CancelHyperspace, c.self->id, objects::kNoObjectId, {}, 0));
    return 0;
}

int m_garrison(Call& c) {
    if (c.nargs != 1)
        return c.fail("GameObjectWrapper::Garrison -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    const Ud* t = c.userdata(1, detail::cls_game_object());
    if (!t || !detail::object_alive(*c.vm, *t))
        return c.fail("GameObjectWrapper::Garrison -- invalid type for parameter 1. Expected game object or AI target.");
    return issue_and_block(c, ev_garrison(c.self->id, t->id));
}

int m_leave_garrison(Call& c) { return issue_and_block(c, ev_garrison(c.self->id, objects::kNoObjectId)); }

int m_sell(Call& c) { return issue_and_block(c, ev_sell(store(c)->mode(), c.self->id)); }

// ---------------------------------------------------------------- direct state changes

// Despawn: an object with a TEAM behaviour despawns its members (reverse member order) instead of
// itself (OBJECT_RUNTIME.md §3); otherwise the object is destroyed (no death effects) [inferred].
int m_despawn(Call& c) {
    objects::ObjectStore* st = store(c);
    const ObjectHandle self = self_object(c);
    if (st->has_behavior(self, objects::beh::Team)) {
        std::vector<ObjectHandle> members;
        for (const ObjectHandle h : st->all_objects())
            if (st->alive(h) && st->container(h) == self) members.push_back(h);
        for (auto it = members.rbegin(); it != members.rend(); ++it) st->destroy(*it);
        return 0;
    }
    destroy_object(c, self);  // the host's removal when it keeps membership (galactic: GalacticSim)
    return 0;
}

// Take_Damage(amount, [hardpoint type name]): direct health change [inferred]; with a name the
// attached child of that type takes it (none: no-op). Invulnerable: nothing; Cannot_Be_Killed:
// health stops at min(current, 1); <= 0 destroys.
int m_take_damage(Call& c) {
    if (c.nargs < 1 || c.nargs > 2)
        return c.fail("GameObjectWrapper::Take_Damage -- invalid number of parameters. Expected 1 or 2, got %d.", c.nargs);
    if (!c.is_number(1)) return c.fail("GameObjectWrapper::Take_Damage -- invalid type for parameter 1. Expected number.");
    if (c.nargs == 2 && !c.is_string(2))
        return c.fail("GameObjectWrapper::Take_Damage -- invalid type for parameter 2. Expected string.");
    objects::ObjectStore* st = store(c);
    ObjectHandle target = self_object(c);
    if (c.nargs == 2) {
        const std::string_view hp = c.string(2);
        ObjectHandle found{};
        for (const ObjectHandle h : st->all_objects())
            if (st->alive(h) && st->attach_parent(h) == target && type_matches(c, h, hp)) {
                found = h;
                break;
            }
        if (!found) return 0;
        target = found;
    }
    const ScriptObjectState& s = state_of(c, st->id(target));
    if (s.invulnerable) return 0;
    const float cur = st->health(target);
    float next = cur - static_cast<float>(c.number(1));
    const objects::GameObjectTypes* ty = types(c);
    const TypeIndex t = st->type(target);
    const float max = (ty && t < ty->columns().tactical_health.size()) ? ty->columns().tactical_health[t] : 0.0f;
    if (max > 0.0f && next > max) next = max;
    if (next <= 0.0f) {
        if (s.cannot_be_killed) {
            st->set_health(target, cur < 1.0f ? cur : 1.0f);
            return 0;
        }
        st->set_health(target, 0.0f);
        st->destroy(target);
        return 0;
    }
    st->set_health(target, next);
    return 0;
}

int m_teleport(Call& c) {
    if (c.nargs != 1) return c.fail("GameObjectWrapper::Teleport -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    Vec3 p{};
    if (position_arg(c, 1, p) != PosArg::Ok)
        return c.fail("GameObjectWrapper:: Teleport -- unable to deduce a position from parameter 1.");
    store(c)->set_position(self_object(c), p);
    return 0;
}

// Teleport_And_Face(object): the target's position and facing.
int m_teleport_and_face(Call& c) {
    if (c.userdata(1, cls_position()))
        return c.fail("GameObjectWrapper:: Teleport_And_Face -- Expected game object for parameter 1, got position instead.");
    const Ud* t = c.userdata(1, detail::cls_game_object());
    if (!t) return c.fail("GameObjectWrapper:: Teleport_And_Face -- Expected game object for parameter 1.");
    if (!detail::object_alive(*c.vm, *t))
        return c.fail("GameObjectWrapper::Teleport_And_Face -- Invalid game object given for parameter 1.");
    objects::ObjectStore* st = store(c);
    const ObjectHandle th = detail::object_handle(*t);
    st->set_position(self_object(c), st->position(th));
    st->set_facing(self_object(c), st->facing(th));
    return 0;
}

// Face_Immediate(target): facing set at once to the unit XY direction towards the target [inferred].
int m_face_immediate(Call& c) {
    if (c.nargs < 1) return c.fail("GameObjectWrapper::Face_Immediate -- Missing required position or gameobject parameter.");
    Vec3 p{};
    if (position_arg(c, 1, p) != PosArg::Ok)
        return c.fail("GameObjectWrapper::Face_Immediate -- could not extract a position from parameter 1.");
    objects::ObjectStore* st = store(c);
    const Vec3 from = st->position(self_object(c));
    const float dx = p.x - from.x, dy = p.y - from.y;
    const float len = dmath::sqrt(dx * dx + dy * dy);
    if (len > 0.0f) st->set_facing(self_object(c), {dx / len, dy / len, 0.0f});
    return 0;
}

int m_change_owner(Call& c) {
    if (c.nargs != 1)
        return c.fail("GameObjectWrapper::Change_Owner -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    const Ud* p = player_arg(c, 1);
    if (!p) return c.fail("GameObjectWrapper::Change_Owner -- invalid type for parameter 1. Expected PlayerWrapper.");
    const ObjectBridge* b = bridge(c);
    if (b && b->change_owner)
        b->change_owner(self_object(c), p->id);
    else
        store(c)->set_owner(self_object(c), p->id);  // no host system caches owners: the store alone
    return 0;
}

int m_mark_parent_mode_object_for_death(Call& c) {
    const ObjectBridge* b = bridge(c);
    if (b && b->parent_store) objects::mark_parent_mode_object_for_death(*b->parent_store, *store(c), self_object(c));
    return 0;
}

// Boolean setters: fn(Call&, ScriptObjectState&, bool). `count_msg` = wrong count, `type_msg` = not a bool.
using BoolField = bool ScriptObjectState::*;
int set_bool(Call& c, BoolField field, const char* count_msg, const char* type_msg) {
    if (c.nargs != 1) return c.fail(count_msg, c.nargs);
    if (!is_bool(c, 1)) return c.fail("%s", type_msg);
    if (ScriptObjectState* s = edit_self(c)) s->*field = to_bool(c, 1);
    return 0;
}

#define ALAMO_BOOL_SETTER(fn, field, method, type_text)                                                         \
    int fn(Call& c) {                                                                                           \
        return set_bool(c, &ScriptObjectState::field,                                                           \
                        "GameObjectWrapper::" method " -- invalid number of parameters. Expected 1, got %d.", \
                        "GameObjectWrapper::" method " -- invalid type for parameter 1. Expected " type_text "."); \
    }
ALAMO_BOOL_SETTER(m_make_invulnerable, invulnerable, "Make_Invulnerable", "boolean")
ALAMO_BOOL_SETTER(m_set_cannot_be_killed, cannot_be_killed, "Set_Cannot_Be_Killed", "boolean")
ALAMO_BOOL_SETTER(m_prevent_all_fire, prevent_all_fire, "Prevent_All_Fire", "boolean")
ALAMO_BOOL_SETTER(m_prevent_opportunity_fire, prevent_opportunity_fire, "Prevent_Opportunity_Fire", "boolean")
ALAMO_BOOL_SETTER(m_suspend_locomotor, locomotor_suspended, "Suspend_Locomotor", "bool")
ALAMO_BOOL_SETTER(m_set_in_limbo, in_limbo, "Set_In_Limbo", "boolean")
ALAMO_BOOL_SETTER(m_set_check_contested_space, check_contested_space, "Set_Check_Contested_Space", "boolean")
ALAMO_BOOL_SETTER(m_disable_capture, capture_disabled, "Disable_Capture", "boolean")
#undef ALAMO_BOOL_SETTER

// Set_Garrison_Spawn: stores the flag like the other boolean setters, then tells the host (the stored
// flag's default does not match the original's enabled default, so the host is notified, not polled).
int m_set_garrison_spawn(Call& c) {
    const int r = set_bool(c, &ScriptObjectState::garrison_spawn,
                           "GameObjectWrapper::Set_Garrison_Spawn -- invalid number of parameters. Expected 1, got %d.",
                           "GameObjectWrapper::Set_Garrison_Spawn -- invalid type for parameter 1. Expected boolean.");
    if (r != 0) return r;  // wrong count / type: nothing stored, the host is not told
    if (const ObjectBridge* b = bridge(c); b && b->garrison_spawn_changed)
        b->garrison_spawn_changed(self_object(c), to_bool(c, 1));
    return r;
}

int m_set_selectable(Call& c) {
    if (c.nargs != 1)
        return c.fail("GameObjectWrapper::Set_Selectable -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    if (!is_bool(c, 1)) return c.fail("GameObjectWrapper::Set_Selectable -- invalid type for parameter 1. Expected boolean.");
    if (ScriptObjectState* s = edit_self(c)) s->selectable = to_bool(c, 1) ? 1 : 0;
    return 0;
}

int m_hide(Call& c) {
    if (c.nargs < 1) return c.fail("GameObjectWrapper::Hide -- Missing required boolean parameter.");
    if (!is_bool(c, 1)) return c.fail("GameObjectWrapper::Hide -- Expected bool parameter.");
    if (ScriptObjectState* s = edit_self(c)) s->hidden = to_bool(c, 1);
    return 0;
}

int m_prevent_ai_usage(Call& c) {
    if (!is_bool(c, 1)) return c.fail("GameObjectWrapper::Prevent_AI_Usage -- Expected bool parameter.");
    if (ScriptObjectState* s = edit_self(c)) s->prevent_ai_usage = to_bool(c, 1);
    return 0;
}

int highlight(Call& c, const char* method, bool small) {
    if (c.nargs < 1 || c.nargs > 2)
        return c.fail("GameObjectWrapper::%s -- invalid number of parameters. Expected 1 or 2.", method);
    if (!is_bool(c, 1)) return c.fail("GameObjectWrapper::%s -- invalid type for parameter 1. Expected boolean.", method);
    if (c.nargs == 2 && !c.is_number(2))
        return c.fail("GameObjectWrapper::%s -- invalid type for parameter 2. Expected number.", method);
    if (ScriptObjectState* s = edit_self(c)) {
        (small ? s->highlighted_small : s->highlighted) = to_bool(c, 1);
        if (c.nargs == 2) s->highlight_offset = static_cast<float>(c.number(2));
    }
    return 0;
}
int m_highlight(Call& c) { return highlight(c, "Highlight", false); }
int m_highlight_small(Call& c) { return highlight(c, "Highlight_Small", true); }

// Override_Max_Speed(number | false): a speed override, false (or true) clears it [inferred].
int m_override_max_speed(Call& c) {
    if (!c.is_number(1) && !is_bool(c, 1))
        return c.fail("GameObjectWrapper::Override_Max_Speed -- Expected a number or boolean parameter.");
    if (ScriptObjectState* s = edit_self(c)) s->max_speed_override = c.is_number(1) ? static_cast<float>(c.number(1)) : -1.0f;
    return 0;
}

int m_set_importance(Call& c) {
    if (c.nargs != 1)
        return c.fail("GameObjectWrapper::Set_Importance -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    if (!c.is_number(1)) return c.fail("GameObjectWrapper::Set_Importance -- invalid type for parameter 1. Expected number.");
    if (ScriptObjectState* s = edit_self(c)) s->importance = detail::to_int(c.number(1));
    return 0;
}

int m_set_targeting_priorities(Call& c) {
    if (c.nargs != 1)
        return c.fail("GameObjectWrapper::Set_Targeting_Priorities - invalid number of parameters. Expected 1, got %d.", c.nargs);
    if (!c.is_string(1))
        return c.fail("GameObjectWrapper::Set_Targeting_Priorities - invalid type for parameter 1. Expected string.");
#if ALAMO_LUA_HAVE_ABILITIES
    // OI lua: a combat unit of the battle gets the S8 per-unit override (TACTICAL_COMBAT 2.4, data pack +0x160: it wins
    // over the type's Targeting_Priority_Set). An unknown set name clears the override (the type's set again)
    // [inferred: the lookup result is stored as is]. The script state is not used for systems' objects.
    if (ObjectBridge* b = bridge(c); b && b->abilities && b->abilities->combat().has_unit(c.self->id)) {
        tactical::combat::CombatSystem& cs = b->abilities->combat();
        cs.set_priority_override(c.self->id, cs.priority_set_index(c.string(1)));
        return 0;
    }
#endif
    if (ScriptObjectState* s = edit_self(c)) s->targeting_priorities = std::string(c.string(1));
    return 0;
}

int m_lock_current_orders(Call& c) {
    if (ScriptObjectState* s = edit_self(c)) s->orders_locked = true;
    return 0;
}
int m_unlock_current_orders(Call& c) {
    if (ScriptObjectState* s = edit_self(c)) s->orders_locked = false;
    return 0;
}

// ---------------------------------------------------------------- animation (R3, wave 2a)

#if ALAMO_LUA_HAVE_ANIM
// The mode's AnimWorld when it has a row for the object.
anim::AnimWorld* anim_world(Call& c, ObjectId id) {
    ObjectBridge* b = bridge(c);
    return (b && b->anim && b->anim->contains(id)) ? b->anim : nullptr;
}
// Default object -> world transform [port] (object_bridge.hpp): yaw of the facing, Scale_Factor, position.
// Facing = the model's nose; Alamo models face model -Y (render::object_transform): X = left, Y = -facing.
anim::Mat34 default_object_to_world(Call& c, ObjectHandle h) {
    const objects::ObjectStore* st = store(c);
    const Vec3 p = st->position(h), f = st->facing(h);
    float cs = 0.0f, sn = -1.0f;  // zero facing: the model as authored (facing -Y)
    const float len = dmath::sqrt(f.x * f.x + f.y * f.y);
    if (len > 0.0f) {
        cs = f.x / len;
        sn = f.y / len;
    }
    float k = 1.0f;
    const objects::GameObjectTypes* ty = types(c);
    if (ty && st->type(h) < ty->size()) {
        const float sf = ty->get_float(st->type(h), "Scale_Factor");
        if (sf > 0.0f) k = sf;
    }
    anim::Mat34 m;
    m.m[0][0] = -sn * k;
    m.m[0][1] = -cs * k;
    m.m[0][2] = 0.0f;
    m.m[0][3] = p.x;
    m.m[1][0] = cs * k;
    m.m[1][1] = -sn * k;
    m.m[1][2] = 0.0f;
    m.m[1][3] = p.y;
    m.m[2][0] = 0.0f;
    m.m[2][1] = 0.0f;
    m.m[2][2] = k;
    m.m[2][3] = p.z;
    return m;
}
#endif

// Play_Animation(name, loop, [index]): recorded in the script state (presentation side) and, with an
// AnimWorld row, played: find_anim_type(name) (unknown name: nothing) + AnimWorld::play with blend 0.33 s,
// speed 1 (ANIM_PARTICLES §A.4 [H]); index absent -> 0 [M: the doc marks the default index M]. No RNG draw
// (the variant is explicit). Finished BlockingStatus.
int m_play_animation(Call& c) {
    if (c.nargs < 2 || c.nargs > 3)
        return c.fail("GameObjectWrapper::Play_Animation -- invalid number of parameters. Expected 2 or 3, got %d.", c.nargs);
    if (!c.is_string(1)) return c.fail("GameObjectWrapper::Play_Animation -- invalid type for parameter 1. Expected string.");
    if (!is_bool(c, 2)) return c.fail("GameObjectWrapper::Play_Animation -- invalid type for parameter 2. Expected boolean.");
    const int32_t index = (c.nargs == 3 && c.is_number(3)) ? detail::to_int(c.number(3)) : -1;
    if (ScriptObjectState* s = edit_self(c)) {
        s->animation = std::string(c.string(1));
        s->animation_loop = to_bool(c, 2);
        s->animation_index = index;
    }
#if ALAMO_LUA_HAVE_ANIM
    if (anim::AnimWorld* w = anim_world(c, c.self->id)) {
        anim::PlayRequest req;
        req.type = anim::find_anim_type(c.string(1));
        if (req.type != anim::kNoAnimType) {
            req.index = index > 0 ? static_cast<uint32_t>(index) : 0u;
            req.loop = to_bool(c, 2);
            req.blend_seconds = anim::kLuaPlayBlendSeconds;
            req.speed = 1.0f;
            const ObjectBridge* b = bridge(c);
            const uint64_t frame = (b && b->anim_frame) ? b->anim_frame() : static_cast<uint64_t>(c.vm->frame);
            w->play(c.self->id, req, nullptr, frame);
        }
    }
#endif
    return push_block(c, c.self->id, 0);
}

// Get_Bone_Position(bone) -> PositionWrapper: the host's bone_position hook, else AnimWorld::bone_world_position
// (bone looked up case-insensitively in the model's skeleton), else the object's position [port: unknown bone
// or no animation data].
int m_get_bone_position(Call& c) {
    if (c.nargs != 1)
        return c.fail("GameObjectWrapper::Get_Bone_Position -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    if (!c.is_string(1)) return c.fail("GameObjectWrapper::Get_Bone_Position -- invalid type for parameter 1. Expected string.");
    const ObjectHandle self = self_object(c);
    const ObjectBridge* b = bridge(c);
    Vec3 out{};
    if (b && b->bone_position && b->bone_position(self, c.string(1), out)) {
        push_position(c.L, out);
        return 1;
    }
#if ALAMO_LUA_HAVE_ANIM
    if (const anim::AnimWorld* w = anim_world(c, c.self->id)) {
        const anim::AnimModel* m = w->model(c.self->id);
        const int32_t bone = (m && m->has_skeleton) ? m->skeleton.find(c.string(1)) : -1;
        if (bone >= 0) {
            anim::Mat34 to_world;
            if (!(b && b->object_to_world && b->object_to_world(self, to_world))) to_world = default_object_to_world(c, self);
            anim::Float3 p;
            if (w->bone_world_position(c.self->id, bone, to_world, p)) {
                push_position(c.L, {p.x, p.y, p.z});
                return 1;
            }
        }
    }
#endif
    push_position(c.L, store(c)->position(self));
    return 1;
}

// Cancel_Ability(name, x): SetUnitAbilityMode(t, off) executed directly (§5): the S9 ability system when it
// has the object, else the event on the direct path. The second parameter is required by the original's
// count check; its meaning is not documented (ignored).
int m_cancel_ability(Call& c) {
    if (c.nargs != 2)
        return c.fail("GameObjectWrapper::Cancel_Ability -- invalid number of parameters. Expected 2, got %d.", c.nargs);
    if (!c.is_string(1)) return c.fail("GameObjectWrapper::Cancel_Ability -- invalid type for ability 1. Expected string.");
    const uint8_t id = ability_param(c, 1);
#if ALAMO_LUA_HAVE_ABILITIES
    if (ab::AbilitySystem* a = ability_system(c, c.self->id)) {
        const ObjectId self[] = {c.self->id};
        if (id != 0) a->set_unit_ability_mode(self, id, false, ability_frame(c));
        return 0;
    }
#endif
    if (id == 0 || !has_ability(c, self_object(c), id)) return 0;
    if (ScriptObjectState* s = edit_self(c)) s->abilities[id].active = false;
    issue(c, self_object(c), ev_ability_mode(c.self->id, id, false, objects::kNoObjectId, {}));
    return 0;
}

// Set_All_Abilities_Autofire(bool): every unit ability of the type, list order.
int m_set_all_abilities_autofire(Call& c) {
    if (c.nargs != 1)
        return c.fail("GameObjectWrapper::Set_All_Abilities_Autofire -- invalid number of parameters. Expected 1, got %d.",
                      c.nargs);
    if (!is_bool(c, 1)) return c.fail("GameObjectWrapper::Set_All_Abilities_Autofire -- invalid type for parameter 1. Expected bool.");
    const bool on = to_bool(c, 1);
    for (const uint8_t id : type_abilities(c, store(c)->type(self_object(c)))) set_autofire(c, id, on);
    return 0;
}

// GameObjectTypeWrapper.Is_Affected_By_Missile_Shield(): Projectile_Category is ROCKET (4) or MPTL_ROCKET (6)
// (ABILITIES_HARDPOINTS.md §4.3 / §5 [H]); the field's text, else its converted enum value.
int m_type_is_affected_by_missile_shield(Call& c) {
    const objects::GameObjectTypes* ty = types(c);
    const auto t = static_cast<TypeIndex>(c.self->value);
    if (!ty || t >= ty->size()) return push_bool(c, false);
    const std::string_view cat = ty->get_string(t, "Projectile_Category");
    if (!cat.empty()) return push_bool(c, iequal(cat, "ROCKET") || iequal(cat, "MPTL_ROCKET"));
    const int32_t v = ty->get_enum(t, "Projectile_Category");
    return push_bool(c, v == 4 || v == 6);
}

// ---------------------------------------------------------------- proximity events
//
// Event_Object_In_Range(func, range [, player]) registers `func` on the receiver; Service_Wrapper()
// (PGBase's PumpEvents / PGCommands' Process_Proximities call it every pump) queues one GetEvent
// event per object in range: func with GetEvent.Params() = {receiver, object}; PumpEvents then calls
// func(receiver, object) inside the script thread. Cancel_Event_Object_In_Range(func) removes it.
// Registrations are kept in the VM (registry array, registration order); candidates in ascending id.
// [inferred] (not in the RE docs): the event repeats on every service while the object stays in
// range (shipped handlers cancel themselves or filter); in range = 3D centre distance <= range;
// candidates = every live object except the receiver, of `player` when one is given; registering the
// same function twice on one object updates range / player instead of adding a second entry.
constexpr char kProxKey[] = "alamo.objects.prox";  // {n = count, [i] = {id, fn, range, player}}

int push_prox(lua_State* L) {
    lua_pushstring(L, kProxKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_type(L, -1) != LUA_TTABLE) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushstring(L, kProxKey);
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
    return lua_gettop(L);
}
int prox_count(lua_State* L, int t) {
    lua_pushstring(L, "n");
    lua_rawget(L, t);
    const int n = static_cast<int>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    return n;
}
void set_prox_count(lua_State* L, int t, int n) {
    lua_pushstring(L, "n");
    lua_pushnumber(L, n);
    lua_rawset(L, t);
}
double rec_field(lua_State* L, int rec, int i) {
    lua_rawgeti(L, rec, i);
    const double v = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return v;
}
// Index (1-based) of the registration (object id, function at fn_index), 0 if none.
int find_prox(lua_State* L, int t, ObjectId id, int fn_index) {
    const int n = prox_count(L, t);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, t, i);
        const int rec = lua_gettop(L);
        lua_rawgeti(L, rec, 2);
        const bool same = rec_field(L, rec, 1) == static_cast<double>(id) && lua_rawequal(L, -1, fn_index);
        lua_settop(L, rec - 1);
        if (same) return i;
    }
    return 0;
}
// Removes entry i, keeping the order of the rest.
void remove_prox(lua_State* L, int t, int i) {
    const int n = prox_count(L, t);
    for (int k = i; k < n; ++k) {
        lua_rawgeti(L, t, k + 1);
        lua_rawseti(L, t, k);
    }
    lua_pushnil(L);
    lua_rawseti(L, t, n);
    set_prox_count(L, t, n - 1);
}

int m_event_object_in_range(Call& c) {
    if (c.nargs < 2 || c.nargs > 3) return c.fail("GameObjectWrapper::Event_Object_In_Range -- Invalid number of params.");
    if (lua_type(c.L, c.idx(1)) != LUA_TFUNCTION)
        return c.fail("GameObjectWrapper::Event_Object_In_Range -- Expected function object as parameter 1.");
    if (!c.is_number(2)) return c.fail("GameObjectWrapper::Event_Object_In_Range -- Expected number as parameter 2.");
    int32_t player = -1;
    if (c.nargs == 3 && !lua_isnil(c.L, c.idx(3))) {
        const Ud* p = player_arg(c, 3);
        if (!p) return c.fail("GameObjectWrapper::Event_Object_In_Range -- Expected player as parameter 3.");  // [inferred]
        player = p->id;
    }
    lua_State* L = c.L;
    const int t = push_prox(L);
    int i = find_prox(L, t, c.self->id, c.idx(1));
    if (i == 0) {
        i = prox_count(L, t) + 1;
        set_prox_count(L, t, i);
    }
    lua_newtable(L);
    lua_pushnumber(L, c.self->id);
    lua_rawseti(L, -2, 1);
    lua_pushvalue(L, c.idx(1));
    lua_rawseti(L, -2, 2);
    lua_pushnumber(L, c.number(2));
    lua_rawseti(L, -2, 3);
    lua_pushnumber(L, player);
    lua_rawseti(L, -2, 4);
    lua_rawseti(L, t, i);
    lua_settop(L, t - 1);
    return 0;
}

int m_cancel_event_object_in_range(Call& c) {
    if (c.nargs != 1) return c.fail("GameObjectWrapper::Cancel_Event_Object_In_Range -- Invalid number of params.");
    if (lua_type(c.L, c.idx(1)) != LUA_TFUNCTION)  // the original reuses Event_Object_In_Range's message
        return c.fail("GameObjectWrapper::Event_Object_In_Range -- Expected function object as parameter 1.");
    lua_State* L = c.L;
    const int t = push_prox(L);
    if (const int i = find_prox(L, t, c.self->id, c.idx(1))) remove_prox(L, t, i);
    lua_settop(L, t - 1);
    return 0;
}

// Candidates of a proximity test around `center`, ascending id (deterministic).
void range_candidates(Call& c, Vec3 center, float range, std::vector<ObjectId>& out) {
    out.clear();
    const objects::ObjectStore* st = store(c);
    const ObjectBridge* b = bridge(c);
    if (b && b->objects_in_range) {
        b->objects_in_range(center, range, out);
    } else {
        for (const ObjectHandle h : st->all_objects())
            if (st->alive(h)) out.push_back(st->id(h));
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

int m_service_wrapper(Call& c) {
    lua_State* L = c.L;
    const objects::ObjectStore* st = store(c);
    const ObjectHandle self = self_object(c);
    const Vec3 center = st->position(self);
    const int t = push_prox(L);
    std::vector<ObjectId> ids;
    const int n = prox_count(L, t);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, t, i);
        const int rec = lua_gettop(L);
        if (rec_field(L, rec, 1) != static_cast<double>(c.self->id)) {
            lua_settop(L, rec - 1);
            continue;
        }
        const double range = rec_field(L, rec, 3);
        const auto player = static_cast<int32_t>(rec_field(L, rec, 4));
        range_candidates(c, center, static_cast<float>(range), ids);
        lua_rawgeti(L, rec, 2);  // fn at rec + 1
        for (const ObjectId id : ids) {
            if (id == c.self->id) continue;
            const ObjectHandle h = st->find(id);
            if (!h || !st->alive(h)) continue;
            if (player >= 0 && st->owner(h) != player) continue;
            if (distance(center, st->position(h)) > range) continue;
            lua_newtable(L);  // params {receiver, object}
            detail::push_object(L, *c.vm, self);
            lua_rawseti(L, -2, 1);
            detail::push_object(L, *c.vm, h);
            lua_rawseti(L, -2, 2);
            detail::queue_event(L, rec + 1, lua_gettop(L));
            lua_pop(L, 1);
        }
        lua_settop(L, rec - 1);
    }
    // Registrations of objects that are gone are dropped (order of the rest kept).
    for (int i = prox_count(L, t); i >= 1; --i) {
        lua_rawgeti(L, t, i);
        const auto id = static_cast<ObjectId>(rec_field(L, lua_gettop(L), 1));
        lua_pop(L, 1);
        const ObjectHandle h = st->find(id);
        if (!h || !st->alive(h)) remove_prox(L, t, i);
    }
    lua_settop(L, t - 1);
    return 0;
}

// ---------------------------------------------------------------- reinforcements

// Location of Reinforce_Unit / Spawn_From_Reinforcement_Pool: 1 ok, 0 `false` (no location), -1 bad.
int reinforce_location(Call& c, int i, Vec3& pos, ObjectHandle& loc) {
    if (is_bool(c, i) && !to_bool(c, i)) return 0;
    return position_arg(c, i, pos, &loc) == PosArg::Ok ? 1 : -1;
}

// Reinforce_Unit(type, location | false, player [, ignore_rules [, obey_zones]]) -> BlockingStatus whose
// Result() is the table of new units (PGSpawnUnits' Process_Reinforcements, SandCrawler.lua).
int g_reinforce_unit(Call& c) {
    if (c.nargs < 3 || c.nargs > 5) return c.fail("LuaReinforceCommandClass::Reinforce -- Invalid number of parameters.");
    const TypeArg t = type_arg(c, 1);
    if (!t.given || t.index == objects::kNone) return c.fail("LuaReinforceCommandClass::Reinforce -- Invalid parameter for type.");
    const Ud* p = player_arg(c, 3);
    if (!p) return c.fail("LuaReinforceCommandClass::Reinforce -- Invalid parameter for player.");
    Vec3 pos{};
    ObjectHandle loc{};
    const int where = reinforce_location(c, 2, pos, loc);
    if (where < 0) return c.fail("LuaReinforceCommandClass::Reinforce -- Invalid position parameter.");
    ObjectBridge* b = bridge(c);
    Ud u;
    u.cls = cls_blocking();
    u.reserved = kBlockReinforcement;
    if (!b || !store(c)) {  // nothing to wait on: a finished block with no units
        u.value = 0;
        detail::push_engine_userdata(c.L, u);
        return 1;
    }
    ReinforceRequest r;
    r.id = b->new_reinforcement();
    r.type = t.index;
    r.player = p->id;
    r.has_location = where == 1;
    r.position = pos;
    r.marker = loc ? store(c)->id(loc) : objects::kNoObjectId;
    r.ignore_rules = c.nargs >= 4 && to_bool(c, 4);
    r.obey_zones = c.nargs >= 5 && to_bool(c, 5);
    u.value = r.id;
    if (b->reinforce) {
        if (!b->reinforce(r)) b->finish_reinforcement(r.id, {});
    } else {
        std::vector<ObjectId> units;
        if (r.has_location) units.push_back(store(c)->id(spawn(c, r.type, r.player, pos, loc, true)));
        b->finish_reinforcement(r.id, std::move(units));
    }
    detail::push_engine_userdata(c.L, u);
    return 1;
}

// Spawn_From_Reinforcement_Pool(type, position, player) -> table of the spawned units.
int g_spawn_from_reinforcement_pool(Call& c) {
    if (c.nargs != 3)
        return c.fail("Spawn_From_Reinforcement_Pool -- Invalid number of parameters. Expected 3, got %d", c.nargs);
    const TypeArg t = type_arg(c, 1);
    if (!t.given || t.index == objects::kNone) return c.fail("Spawn_From_Reinforcement_Pool -- Invalid parameter for type.");
    const Ud* p = player_arg(c, 3);
    if (!p) return c.fail("Spawn_From_Reinforcement_Pool -- Invalid parameter for player.");
    Vec3 pos{};
    ObjectHandle loc{};
    if (position_arg(c, 2, pos, &loc) != PosArg::Ok) return c.fail("Spawn_From_Reinforcement_Pool -- Invalid position parameter.");
    lua_newtable(c.L);
    if (!store(c)) return 1;
    const ObjectBridge* b = bridge(c);
    std::vector<ObjectHandle> made;
    if (b && b->spawn_from_pool)
        made = b->spawn_from_pool(t.index, p->id, pos);
    else
        made.push_back(spawn(c, t.index, p->id, pos, loc, true));
    int n = 0;
    for (const ObjectHandle h : made) {
        if (!h || !store(c)->alive(h)) continue;
        detail::push_object(c.L, *c.vm, h);
        lua_rawseti(c.L, -2, ++n);
    }
    return 1;
}

// PlayerWrapper.Get_Space_Station(): the player's newest live object whose type is a <StarBase>
// element [inferred: no contract; the FreeStore script uses it for the tactical space station].
int m_get_space_station(Call& c) {
    const objects::ObjectStore* st = store(c);
    const objects::GameObjectTypes* ty = types(c);
    if (!st || !ty) return push_nil(c);
    for (const ObjectHandle h : st->objects_newest_first()) {
        if (!st->active(h) || st->owner(h) != c.self->id) continue;
        const TypeIndex t = st->type(h);
        if (t < ty->raw().type_count() && iequal(ty->raw().element_name(t), "StarBase")) {
            detail::push_object(c.L, *c.vm, h);
            return 1;
        }
    }
    return push_nil(c);
}

// ---------------------------------------------------------------- GameObjectTypeWrapper

const objects::GameObjectTypes* self_types(Call& c, TypeIndex& t) {
    t = static_cast<TypeIndex>(c.self->value);
    const objects::GameObjectTypes* ty = types(c);
    return (ty && t < ty->size()) ? ty : nullptr;
}
int m_type_combat_rating(Call& c) {  // AI_Combat_Power [inferred]
    TypeIndex t = 0;
    const auto* ty = self_types(c, t);
    return push_num(c, ty ? static_cast<double>(ty->columns().ai_combat_power[t]) : 0.0);
}
int m_type_tech_level(Call& c) {
    TypeIndex t = 0;
    const auto* ty = self_types(c, t);
    return push_num(c, ty ? ty->columns().tech_level[t] : 0);
}
int m_type_build_cost(Call& c) {
    TypeIndex t = 0;
    const auto* ty = self_types(c, t);
    return push_num(c, ty ? ty->columns().build_cost_credits[t] : 0);
}
int m_type_score_cost(Call& c) {
    TypeIndex t = 0;
    const auto* ty = self_types(c, t);
    return push_num(c, ty ? ty->columns().score_cost_credits[t] : 0);
}
int m_type_tactical_build_cost(Call& c) {  // Tactical_Build_Cost_Multiplayer [inferred]
    TypeIndex t = 0;
    const auto* ty = self_types(c, t);
    return push_num(c, ty ? ty->get_int(t, "Tactical_Build_Cost_Multiplayer") : 0);
}
int m_type_max_range(Call& c) {  // Targeting_Max_Attack_Distance [inferred]
    TypeIndex t = 0;
    const auto* ty = self_types(c, t);
    return push_num(c, ty ? static_cast<double>(ty->columns().targeting_max_attack_distance[t]) : 0.0);
}
int m_type_min_range(Call& c) {  // Targeting_Min_Attack_Distance [inferred]
    TypeIndex t = 0;
    const auto* ty = self_types(c, t);
    return push_num(c, ty ? static_cast<double>(ty->columns().targeting_min_attack_distance[t]) : 0.0);
}
int m_type_is_hero(Call& c) {  // Is_Named_Hero [inferred]
    TypeIndex t = 0;
    const auto* ty = self_types(c, t);
    return push_bool(c, ty && ty->get_bool(t, "Is_Named_Hero"));
}
int m_type_base_level(Call& c) {
    TypeIndex t = 0;
    const auto* ty = self_types(c, t);
    return push_num(c, ty ? ty->get_int(t, "Base_Level") : 0);
}

// ---------------------------------------------------------------- L1/S4 wave 1: containment, garrisons

// A garrison holder [inferred: OBJECT_RUNTIME.md "garrison" row]: an object with a GARRISON_VEHICLE /
// GARRISON_STRUCTURE / GARRISON_HOVER behaviour or the CanContainGarrison property (OBJECT_TYPES 9.1.2).
bool garrison_holder(Call& c, ObjectHandle h) {
    static const objects::BehaviorTypeId kHolders[] = {objects::behavior_type_id("GARRISON_VEHICLE"),
                                                       objects::behavior_type_id("GARRISON_STRUCTURE"),
                                                       objects::behavior_type_id("GARRISON_HOVER")};
    const objects::ObjectStore* st = store(c);
    if (!h || !st->alive(h)) return false;
    for (const objects::BehaviorTypeId b : kHolders)
        if (b != objects::kNoBehavior && st->has_behavior(h, b)) return true;
    const objects::GameObjectTypes* ty = types(c);
    if (!ty) return false;
    const auto m = ty->enums().get(objects::EnumId::GameObjectProperties).convert("CanContainGarrison");
    return m && *m != 0 && (ty->get_mask(st->type(h), "Property_Flags") & static_cast<uint64_t>(*m)) != 0;
}

// Named hero test (BATTLE_TRANSITIONS.md 1.x: Is_Named_Hero, recursive through fleets / transports): the
// object's type, a Company_Units member type of it [inferred: galactic teams carry no member objects],
// or anything it contains.
bool type_is_hero(Call& c, TypeIndex t) {
    const objects::GameObjectTypes* ty = types(c);
    if (!ty || t >= ty->raw().type_count()) return false;
    if (ty->get_bool(t, "Is_Named_Hero")) return true;
    for (const TypeIndex m : ty->get_type_refs(t, "Company_Units"))
        if (m != objects::kNone && m < ty->raw().type_count() && ty->get_bool(m, "Is_Named_Hero")) return true;
    return false;
}
void collect_heroes(Call& c, ObjectHandle h, std::vector<ObjectHandle>& out, uint32_t depth) {
    if (depth > 8) return;
    for (const ObjectHandle m : contents_of(c, h)) {
        if (type_is_hero(c, store(c)->type(m))) out.push_back(m);
        collect_heroes(c, m, out, depth + 1);
    }
}

int push_object_table(Call& c, const std::vector<ObjectHandle>& v) {
    lua_newtable(c.L);
    int n = 0;
    for (const ObjectHandle h : v) {
        detail::push_object(c.L, *c.vm, h);
        lua_rawseti(c.L, -2, ++n);
    }
    return 1;
}

int m_is_in_garrison(Call& c) { return push_bool(c, garrison_holder(c, container_of(c, self_object(c)))); }
int m_has_garrison(Call& c) {
    return push_bool(c, garrison_holder(c, self_object(c)) && !contents_of(c, self_object(c)).empty());
}
int m_get_garrisoned_units(Call& c) {
    if (!garrison_holder(c, self_object(c))) return push_object_table(c, {});
    return push_object_table(c, contents_of(c, self_object(c)));
}
// Can_Garrison(target) [inferred]: the target holds garrisons, its Garrison_Category shares a bit with this
// object's CategoryMask, and it is this object's owner's or empty. No capacity test (no garrison system).
int m_can_garrison(Call& c) {
    if (c.nargs != 1)
        return c.fail("GameObjectWrapper::Can_Garrison -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    const Ud* t = c.userdata(1, detail::cls_game_object());
    if (!t) return c.fail("GameObjectWrapper::Can_Garrison -- invalid type for parameter 1. Expected game object or AI target.");
    if (!detail::object_alive(*c.vm, *t)) return push_bool(c, false);
    const ObjectHandle target = detail::object_handle(*t), self = self_object(c);
    const objects::ObjectStore* st = store(c);
    const objects::GameObjectTypes* ty = types(c);
    if (target == self || !ty || !garrison_holder(c, target)) return push_bool(c, false);
    if ((ty->get_mask(st->type(target), "Garrison_Category") & ty->get_mask(st->type(self), "CategoryMask")) == 0)
        return push_bool(c, false);
    return push_bool(c, st->owner(target) == st->owner(self) || contents_of(c, target).empty());
}
// Eject_Garrison(): a Leave_Garrison order for every garrisoned unit (content order) [inferred].
int m_eject_garrison(Call& c) {
    if (!garrison_holder(c, self_object(c))) return 0;
    for (const ObjectHandle u : contents_of(c, self_object(c))) issue(c, u, ev_garrison(store(c)->id(u), objects::kNoObjectId));
    return 0;
}

int m_contains_hero(Call& c) {
    std::vector<ObjectHandle> heroes;
    collect_heroes(c, self_object(c), heroes, 0);
    return push_bool(c, !heroes.empty());
}
int m_get_contained_heroes(Call& c) {  // walk order: contents, depth first [inferred]
    std::vector<ObjectHandle> heroes;
    collect_heroes(c, self_object(c), heroes, 0);
    return push_object_table(c, heroes);
}

// Destroy_Contained_Objects(fraction): destroys round(n x fraction) of the n contained objects, content
// order, all of them at fraction >= 1 [inferred] (FleetEvents.lua passes 1.0).
int m_destroy_contained_objects(Call& c) {
    if (c.nargs != 1)
        return c.fail("GameObjectWrapper::Destroy_Contained_Objects -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    if (!c.is_number(1))
        return c.fail("GameObjectWrapper::Destroy_Contained_Objects -- invalid type for parameter 1. Expected number.");
    const std::vector<ObjectHandle> v = contents_of(c, self_object(c));
    const double f = c.number(1);
    size_t n = v.size();
    if (!(f >= 1.0)) n = f > 0.0 ? static_cast<size_t>(static_cast<double>(v.size()) * f + 0.5) : 0;
    for (size_t i = 0; i < n && i < v.size(); ++i)
        if (store(c)->alive(v[i])) destroy_object(c, v[i]);
    return 0;
}

// BlockForever(): a BlockingStatus that never finishes.
int g_block_forever(Call& c) {
    Ud u;
    u.cls = cls_blocking();
    u.reserved = kBlockForever;
    detail::push_engine_userdata(c.L, u);
    return 1;
}

constexpr BindingKind G = BindingKind::Global;
constexpr BindingKind M = BindingKind::Method;
constexpr const char* GO = "GameObjectWrapper";
constexpr const char* GT = "GameObjectTypeWrapper";

constexpr detail::ImplEntry kImpl[] = {
    // globals
    {G, "", "Find_Hint", g_find_hint},
    {G, "", "Find_All_Objects_With_Hint", g_find_all_with_hint},
    {G, "", "Find_All_Objects_Of_Type", g_find_all_objects_of_type},
    {G, "", "Find_First_Object", g_find_first_object},
    {G, "", "FindPlanet", g_find_planet},
    {G, "", "Find_Nearest", g_find_nearest},
    {G, "", "Spawn_Unit", g_spawn_unit},
    {G, "", "Create_Generic_Object", g_create_generic_object},
    {G, "", "Hide_Object", g_hide_object},
    {G, "", "Create_Position", g_create_position},
    {G, "", "Reinforce_Unit", g_reinforce_unit},
    {G, "", "Spawn_From_Reinforcement_Pool", g_spawn_from_reinforcement_pool},
    // L1/S4 wave 1: containment, garrisons, heroes
    {G, "", "BlockForever", g_block_forever},
    {M, GO, "Is_In_Garrison", m_is_in_garrison},
    {M, GO, "Has_Garrison", m_has_garrison},
    {M, GO, "Get_Garrisoned_Units", m_get_garrisoned_units},
    {M, GO, "Can_Garrison", m_can_garrison},
    {M, GO, "Eject_Garrison", m_eject_garrison},
    {M, GO, "Contains_Hero", m_contains_hero},
    {M, GO, "Get_Contained_Heroes", m_get_contained_heroes},
    {M, GO, "Destroy_Contained_Objects", m_destroy_contained_objects},
    {M, "PlayerWrapper", "Get_Space_Station", m_get_space_station},
    // proximity events, ability extras
    {M, GO, "Event_Object_In_Range", m_event_object_in_range},
    {M, GO, "Cancel_Event_Object_In_Range", m_cancel_event_object_in_range},
    {M, GO, "Service_Wrapper", m_service_wrapper},
    {M, GO, "Cancel_Ability", m_cancel_ability},
    {M, GO, "Set_All_Abilities_Autofire", m_set_all_abilities_autofire},
    // L1/S4 wave 2a: S9 abilities, R3 animation
    {M, GO, "Force_Ability_Recharge", m_force_ability_recharge},
    {M, GO, "Reset_Ability_Counter", m_reset_ability_counter},
    {M, GO, "Is_Under_Effects_Of_Ability", m_is_under_effects_of_ability},
    {M, GO, "Are_Engines_Online", m_are_engines_online},
    {M, GO, "Get_Bone_Position", m_get_bone_position},
    {M, GT, "Is_Affected_By_Missile_Shield", m_type_is_affected_by_missile_shield},
    // finder / position / blocking-status methods
    {M, "FindPlanetClass", "Get_All_Planets", m_get_all_planets},
    {M, "PositionWrapper", "Get_XYZ", m_position_get_xyz},
    {M, "PositionWrapper", "Is_Valid", m_true},
    {M, "BlockingStatus", "IsFinished", m_block_is_finished},
    {M, "BlockingStatus", "Result", m_block_result},
    // GameObjectWrapper: queries
    {M, GO, "Get_Position", m_get_position},
    {M, GO, "Get_Distance", m_get_distance},
    {M, GO, "Get_Name", m_get_name},
    {M, GO, "Get_Hull", m_get_hull},
    {M, GO, "Get_Health", m_get_hull},
    {M, GO, "Get_Shield", m_get_shield},
    {M, GO, "Is_Category", m_is_category},
    {M, GO, "Has_Property", m_has_property},
    {M, GO, "Get_Game_Scoring_Type", m_get_game_scoring_type},
    {M, GO, "Get_Parent_Object", m_get_parent_object},
    {M, GO, "Get_Planet_Location", m_get_planet_location},
    {M, GO, "Get_Hint", m_get_hint},
    {M, GO, "Is_Selectable", m_is_selectable},
    {M, GO, "Is_Transport", m_is_transport},
    {M, GO, "Can_Move", m_can_move},
    {M, GO, "Get_Contained_Object_Count", m_get_contained_object_count},
    {M, GO, "Contains_Object_Type", m_contains_object_type},
    {M, GO, "Has_Active_Orders", m_has_active_orders},
    {M, GO, "Get_Attack_Target", m_get_attack_target},
    {M, GO, "Has_Attack_Target", m_has_attack_target},
    // abilities
    {M, GO, "Has_Ability", m_has_ability},
    {M, GO, "Is_Ability_Ready", m_is_ability_ready},
    {M, GO, "Is_Ability_Active", m_is_ability_active},
    {M, GO, "Is_Ability_Autofire", m_is_ability_autofire},
    {M, GO, "Set_Single_Ability_Autofire", m_set_single_ability_autofire},
    {M, GO, "Activate_Ability", m_activate_ability},
    // orders
    {M, GO, "Move_To", m_move_to},
    {M, GO, "Attack_Move", m_attack_move},
    {M, GO, "Attack_Target", m_attack_target},
    {M, GO, "Guard_Target", m_guard_target},
    {M, GO, "Stop", m_stop},
    {M, GO, "Turn_To_Face", m_turn_to_face},
    {M, GO, "Hyperspace_Away", m_hyperspace_away},
    {M, GO, "Cancel_Hyperspace", m_cancel_hyperspace},
    {M, GO, "Garrison", m_garrison},
    {M, GO, "Leave_Garrison", m_leave_garrison},
    {M, GO, "Sell", m_sell},
    // direct state changes
    {M, GO, "Despawn", m_despawn},
    {M, GO, "Take_Damage", m_take_damage},
    {M, GO, "Teleport", m_teleport},
    {M, GO, "Teleport_And_Face", m_teleport_and_face},
    {M, GO, "Face_Immediate", m_face_immediate},
    {M, GO, "Change_Owner", m_change_owner},
    {M, GO, "Mark_Parent_Mode_Object_For_Death", m_mark_parent_mode_object_for_death},
    {M, GO, "Make_Invulnerable", m_make_invulnerable},
    {M, GO, "Set_Cannot_Be_Killed", m_set_cannot_be_killed},
    {M, GO, "Prevent_All_Fire", m_prevent_all_fire},
    {M, GO, "Prevent_Opportunity_Fire", m_prevent_opportunity_fire},
    {M, GO, "Prevent_AI_Usage", m_prevent_ai_usage},
    {M, GO, "Suspend_Locomotor", m_suspend_locomotor},
    {M, GO, "Set_In_Limbo", m_set_in_limbo},
    {M, GO, "Set_Garrison_Spawn", m_set_garrison_spawn},
    {M, GO, "Set_Check_Contested_Space", m_set_check_contested_space},
    {M, GO, "Disable_Capture", m_disable_capture},
    {M, GO, "Set_Selectable", m_set_selectable},
    {M, GO, "Hide", m_hide},
    {M, GO, "Highlight", m_highlight},
    {M, GO, "Highlight_Small", m_highlight_small},
    {M, GO, "Override_Max_Speed", m_override_max_speed},
    {M, GO, "Set_Importance", m_set_importance},
    {M, GO, "Set_Targeting_Priorities", m_set_targeting_priorities},
    {M, GO, "Lock_Current_Orders", m_lock_current_orders},
    {M, GO, "Unlock_Current_Orders", m_unlock_current_orders},
    {M, GO, "Play_Animation", m_play_animation},
    // GameObjectTypeWrapper
    {M, GT, "Get_Combat_Rating", m_type_combat_rating},
    {M, GT, "Get_Tech_Level", m_type_tech_level},
    {M, GT, "Get_Build_Cost", m_type_build_cost},
    {M, GT, "Get_Score_Cost_Credits", m_type_score_cost},
    {M, GT, "Get_Tactical_Build_Cost", m_type_tactical_build_cost},
    {M, GT, "Get_Max_Range", m_type_max_range},
    {M, GT, "Get_Min_Range", m_type_min_range},
    {M, GT, "Is_Hero", m_type_is_hero},
    {M, GT, "Get_Base_Level", m_type_base_level},
};

}  // namespace

std::span<const detail::ImplEntry> detail::impl_objects() { return kImpl; }

// OI lua: TaskForceClass.Set_Targeting_Priorities(set [, filter]) on the task force's units (unit-list order): each unit
// whose type name or CategoryMask matches `filter` (none: every unit) gets GameObjectWrapper.Set_Targeting_Priorities(set)
// [inferred: 0x600060's body is not traced; shipped scripts pass a category ("Infantry", "Fighter") or a type name
// ("TIE_Crawler"), later calls overriding earlier ones for the units they match].
void detail::task_force_set_targeting_priorities(Call& c, const std::vector<objects::ObjectHandle>& units) {
    const objects::ObjectStore* st = store(c);
    const objects::GameObjectTypes* ty = types(c);
    if (!st || !c.is_string(1)) return;
    const bool filtered = c.is_string(2);
    uint64_t mask = 0;
    if (filtered && ty)
        if (const auto m = ty->enums().get(objects::EnumId::GameObjectCategory).convert(c.string(2))) mask = static_cast<uint64_t>(*m);
    for (const ObjectHandle u : units) {
        if (!st->alive(u)) continue;
        if (filtered) {
            const bool by_name = iequal(object_type_name(c, u), c.string(2));
            const bool by_cat = mask != 0 && ty && (ty->get_mask(st->type(u), "CategoryMask") & mask) != 0;
            if (!by_name && !by_cat) continue;
        }
        detail::call_object_method(c, u, "Set_Targeting_Priorities", c.idx(1), 1);
    }
}

uint8_t unit_ability_id(std::string_view name) {
#if ALAMO_LUA_HAVE_ABILITIES
    return tactical::abilities::ability_type_from_name(name);  // S9's table: one name table for the engine
#else
    for (size_t i = 1; i < std::size(kAbilityNames); ++i)
        if (iequal(kAbilityNames[i], name)) return static_cast<uint8_t>(i);
    return 0;
#endif
}

std::string_view unit_ability_name(uint8_t id) { return id < std::size(kAbilityNames) ? kAbilityNames[id] : std::string_view(); }

void ObjectBridge::hash(StateHasher& h) const {
    h.u32(orders_issued);
    h.u32(static_cast<uint32_t>(states_.size()));
    for (const auto& [id, s] : states_) {
        h.i32(id);
        h.str(s.hint);
        for (const bool b : {s.hidden, s.prevent_ai_usage, s.prevent_all_fire, s.prevent_opportunity_fire, s.invulnerable,
                             s.cannot_be_killed, s.locomotor_suspended, s.highlighted, s.highlighted_small, s.in_limbo,
                             s.garrison_spawn, s.check_contested_space, s.capture_disabled, s.orders_locked, s.has_orders,
                             s.animation_loop})
            h.boolean(b);
        h.u8(static_cast<uint8_t>(s.selectable));
        h.f32(s.max_speed_override);
        h.f32(s.shield);
        h.f32(s.highlight_offset);
        h.i32(s.importance);
        h.i32(s.attack_target);
        h.str(s.targeting_priorities);
        h.str(s.animation);
        h.i32(s.animation_index);
        h.u32(static_cast<uint32_t>(s.abilities.size()));
        for (const auto& [a, st] : s.abilities) {
            h.u8(a);
            h.boolean(st.active);
            h.boolean(st.autofire);
        }
    }
    h.u32(next_reinforcement_);
    h.u32(static_cast<uint32_t>(reinforcements_.size()));
    for (const auto& [id, r] : reinforcements_) {
        h.u32(id);
        h.boolean(r.finished);
        h.u32(static_cast<uint32_t>(r.units.size()));
        for (const ObjectId u : r.units) h.i32(u);
    }
    // L1/S4 wave 2b: cinematic landing machines (only when there are some: no change to existing CRCs).
    if (tactical && tactical->transports && !tactical->transports->entries().empty()) tactical->transports->hash(h);
    // L1 final: the remaining family's per-object state (only when there is some: no change to existing CRCs).
    if (!remaining_states_.empty()) {
        h.u32(static_cast<uint32_t>(remaining_states_.size()));
        for (const auto& [id, s] : remaining_states_) {
            h.i32(id);
            h.boolean(s.dynamic_lod);
            h.boolean(s.build_pad_locked);
            h.boolean(s.diverted);
            h.f32(s.divert_goal.x);
            h.f32(s.divert_goal.y);
            h.f32(s.divert_goal.z);
            h.i32(s.pad_uc);
            h.u8(static_cast<uint8_t>(s.prefer_ground));
            h.f32(s.stickiness);
            h.u32(static_cast<uint32_t>(s.behaviors.size()));
            for (const auto& [b, on] : s.behaviors) {
                h.i32(b);
                h.boolean(on);
            }
        }
    }
}

// ---- N1 save / load ----

namespace {
constexpr uint32_t kBridgeSaveVersion = 1;
// ScriptObjectState's flags in save order (shared by write and read).
template <class S>
auto bridge_flags(S& s) {
    return std::array{&s.hidden,           &s.prevent_ai_usage,    &s.prevent_all_fire, &s.prevent_opportunity_fire,
                      &s.invulnerable,     &s.cannot_be_killed,    &s.locomotor_suspended, &s.highlighted,
                      &s.highlighted_small, &s.in_limbo,           &s.garrison_spawn,   &s.check_contested_space,
                      &s.capture_disabled, &s.orders_locked,       &s.has_orders,       &s.animation_loop};
}
}  // namespace

void ObjectBridge::write(ByteWriter& w) const {
    w.u32(kBridgeSaveVersion).u32(orders_issued);
    w.u32(static_cast<uint32_t>(states_.size()));
    for (const auto& [id, s] : states_) {
        w.i32(id).str(s.hint);
        for (const bool* b : bridge_flags(s)) w.boolean(*b);
        w.u8(static_cast<uint8_t>(s.selectable)).f32(s.max_speed_override).f32(s.shield).f32(s.highlight_offset);
        w.i32(s.importance).i32(s.attack_target).str(s.targeting_priorities).str(s.animation).i32(s.animation_index);
        w.u32(static_cast<uint32_t>(s.abilities.size()));
        for (const auto& [a, st] : s.abilities) w.u8(a).boolean(st.active).boolean(st.autofire);
    }
    w.u32(static_cast<uint32_t>(remaining_states_.size()));
    for (const auto& [id, s] : remaining_states_) {
        w.i32(id).boolean(s.dynamic_lod).boolean(s.build_pad_locked).u8(static_cast<uint8_t>(s.prefer_ground));
        w.f32(s.stickiness).u32(static_cast<uint32_t>(s.behaviors.size()));
        for (const auto& [b, on] : s.behaviors) w.i32(b).boolean(on);
        w.boolean(s.diverted).f32(s.divert_goal.x).f32(s.divert_goal.y).f32(s.divert_goal.z).i32(s.pad_uc);
    }
    w.u32(next_reinforcement_).u32(static_cast<uint32_t>(reinforcements_.size()));
    for (const auto& [id, rs] : reinforcements_) {
        w.u32(id).boolean(rs.finished).u32(static_cast<uint32_t>(rs.units.size()));
        for (const ObjectId u : rs.units) w.i32(u);
    }
}

bool ObjectBridge::read(ByteReader& r) {
    if (r.u32() != kBridgeSaveVersion || !r.ok()) return false;
    const uint32_t orders = r.u32();
    uint32_t n = r.u32();
    if (!r.ok() || n > r.remaining() / 40) return false;
    std::map<ObjectId, ScriptObjectState> states;
    for (uint32_t i = 0; i < n; ++i) {
        const ObjectId id = r.i32();
        if (!r.ok() || (!states.empty() && id <= states.rbegin()->first)) return false;
        ScriptObjectState& s = states[id];
        s.hint = r.str();
        for (bool* b : bridge_flags(s)) *b = r.boolean();
        s.selectable = static_cast<int8_t>(r.u8());
        s.max_speed_override = r.f32();
        s.shield = r.f32();
        s.highlight_offset = r.f32();
        s.importance = r.i32();
        s.attack_target = r.i32();
        s.targeting_priorities = r.str();
        s.animation = r.str();
        s.animation_index = r.i32();
        const uint32_t na = r.u32();
        if (!r.ok() || na > r.remaining() / 3) return false;
        for (uint32_t k = 0; k < na; ++k) {
            const uint8_t a = r.u8();
            AbilityState st;
            st.active = r.boolean();
            st.autofire = r.boolean();
            if (!r.ok() || (!s.abilities.empty() && a <= s.abilities.rbegin()->first)) return false;
            s.abilities.emplace(a, st);
        }
    }
    n = r.u32();
    if (!r.ok() || n > r.remaining() / 32) return false;
    std::map<ObjectId, RemainingObjectState> remaining_states;
    for (uint32_t i = 0; i < n; ++i) {
        const ObjectId id = r.i32();
        if (!r.ok() || (!remaining_states.empty() && id <= remaining_states.rbegin()->first)) return false;
        RemainingObjectState& s = remaining_states[id];
        s.dynamic_lod = r.boolean();
        s.build_pad_locked = r.boolean();
        s.prefer_ground = static_cast<int8_t>(r.u8());
        s.stickiness = r.f32();
        const uint32_t nb = r.u32();
        if (!r.ok() || nb > r.remaining() / 5) return false;
        for (uint32_t k = 0; k < nb; ++k) {
            const int32_t b = r.i32();
            const bool on = r.boolean();
            if (!r.ok() || (!s.behaviors.empty() && b <= s.behaviors.rbegin()->first)) return false;
            s.behaviors.emplace(b, on);
        }
        s.diverted = r.boolean();
        s.divert_goal.x = r.f32();
        s.divert_goal.y = r.f32();
        s.divert_goal.z = r.f32();
        s.pad_uc = r.i32();
    }
    const uint32_t next = r.u32();
    n = r.u32();
    if (!r.ok() || next < 1 || n > r.remaining() / 9) return false;
    std::map<uint32_t, ReinforceState> reinforcements;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t id = r.u32();
        ReinforceState rs;
        rs.finished = r.boolean();
        const uint32_t nu = r.u32();
        if (!r.ok() || id < 1 || id >= next || (!reinforcements.empty() && id <= reinforcements.rbegin()->first) ||
            nu > r.remaining() / 4)
            return false;
        rs.units.resize(nu);
        for (ObjectId& u : rs.units) u = r.i32();
        reinforcements.emplace(id, std::move(rs));
    }
    if (!r.ok()) return false;
    orders_issued = orders;
    states_ = std::move(states);
    remaining_states_ = std::move(remaining_states);
    reinforcements_ = std::move(reinforcements);
    next_reinforcement_ = next;
    return true;
}

void attach_object_bridge(LuaVm& vm, ObjectBridge* bridge) {
    lua_State* L = vm.state();
    lua_pushstring(L, kBridgeKey);
    if (bridge)
        lua_pushlightuserdata(L, bridge);
    else
        lua_pushnil(L);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

ObjectBridge* attached_object_bridge(LuaVm& vm) {
    lua_State* L = vm.state();
    lua_pushstring(L, kBridgeKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* b = static_cast<ObjectBridge*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return b;
}

}  // namespace alamo::lua
