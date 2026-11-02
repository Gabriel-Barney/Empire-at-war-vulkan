// Event payload tables: docs/re/COMMANDS.md §2 (wire order). Field names are the port's.
#include "alamo/commands/event_types.hpp"

#include <array>
#include <bit>
#include <cmath>

namespace alamo::commands {
namespace {

using K = FieldKind;

constexpr FieldDef B(std::string_view n, uint8_t w) { return {n, K::Bits, w}; }
constexpr FieldDef I(std::string_view n) { return {n, K::Id, 32}; }
constexpr FieldDef P(std::string_view n) { return {n, K::Player, 0}; }
constexpr FieldDef Q(std::string_view n, uint8_t q) { return {n, K::Quant, q}; }
constexpr FieldDef L(std::string_view n, uint8_t w) { return {n, K::List, w}; }

constexpr FieldDef kFrameInfo[] = {B("frame", 26), B("command_count", 22), B("crc", 16)};
constexpr FieldDef kMoveToPosition[] = {Q("x", 1), Q("y", 2), Q("z", 3), B("flags", 8), B("switch_locomotor", 1)};
constexpr FieldDef kMoveToObject[] = {I("target"), L("movers", 32)};
constexpr FieldDef kMoveObjectToObject[] = {I("object"), I("target")};
constexpr FieldDef kLook[] = {Q("a", 4), Q("b", 4)};
constexpr FieldDef kSelect[] = {L("ids", 7)};
constexpr FieldDef kAttack[] = {I("target"), B("ref2", 32), B("flag0", 1), B("flag1", 1), B("flag2", 1), L("attackers", 32)};
constexpr FieldDef kProduction[] = {B("type_index", 16), I("location"), B("b8", 8), B("cancel", 1), B("queue", 4), B("index", 4)};
constexpr FieldDef kFleetManagement[] = {I("target"), I("fleet"), I("unit"), B("op", 4), B("slot", 6), I("a"), I("b")};
constexpr FieldDef kInvade[] = {I("planet"), L("units", 7), B("flag", 1)};
constexpr FieldDef kMoveThroughObjects[] = {L("waypoints", 8), I("object")};
constexpr FieldDef kDebug[] = {{"text", K::Str}};
constexpr FieldDef kReinforce[] = {I("unit"), Q("x", 1), Q("y", 2), Q("z", 3)};
constexpr FieldDef kSpecialAbility[] = {B("kind", 3), {"data", K::Opaque}};
constexpr FieldDef kMoveToRay[] = {Q("ax", 1), Q("ay", 2), Q("az", 3), Q("bx", 1), Q("by", 2), Q("bz", 3), B("flags", 8)};
constexpr FieldDef kChat[] = {{"text", K::WStr}, B("allies_only", 1)};
constexpr FieldDef kFrameSync[] = {B("max_ahead", 8), B("multiplier", 8)};
constexpr FieldDef kPerformanceMetrics[] = {B("max_latency", 12), B("ticks_per_second", 7)};
constexpr FieldDef kMoveToRayFacing[] = {Q("ax", 1), Q("ay", 2), Q("az", 3), Q("bx", 1),   Q("by", 2),
                                         Q("bz", 3), Q("angle", 6), B("flags", 8)};
constexpr FieldDef kFacing[] = {Q("angle", 6), L("units", 32)};
constexpr FieldDef kEscort[] = {I("target"), B("option", 32)};
constexpr FieldDef kRetreat[] = {B("request", 1)};
constexpr FieldDef kSpecialWeaponFire[] = {I("weapon"), P("player"), B("at_position", 1),
                                           {"x", K::Quant, 1, 2, 1}, {"y", K::Quant, 2, 2, 1}, {"z", K::Quant, 3, 2, 1},
                                           {"target", K::Id, 32, 2, 0}};
constexpr FieldDef kPoint[] = {Q("x", 1), Q("y", 2), Q("z", 3)};
constexpr FieldDef kControlGroup[] = {B("group", 4), B("assign", 1), {"add", K::Bits, 1, 1, 1}};
constexpr FieldDef kSetupPhaseMove[] = {I("object"), Q("x", 1), Q("y", 2), Q("z", 3), Q("angle", 6)};
constexpr FieldDef kTacticalBuild[] = {I("pad"), B("type_index", 16), P("player")};
constexpr FieldDef kOneId[] = {I("object")};
constexpr FieldDef kAlly[] = {P("target")};
constexpr FieldDef kSaveGame[] = {B("slot", 7), {"name", K::Str}};
constexpr FieldDef kTacticalSuperWeapon[] = {I("weapon"), B("op", 2)};
constexpr FieldDef kGameOptions[] = {B("tactical_speed", 32), B("galactic_speed", 32)};
constexpr FieldDef kTacticalSpecialAbility[] = {L("users", 7), I("target"), I("id2"), B("ability", 7), I("id3")};
constexpr FieldDef kSetUnitAbilityMode[] = {B("count", 7), B("ability", 7), B("on", 1), B("play_response", 1), B("flag", 1),
                                            I("object"),   Q("x", 1),       Q("y", 2),  Q("z", 3),
                                            {"units", K::CountedIds, 0}};
constexpr FieldDef kRepairHardpoint[] = {I("object"), B("hardpoint", 6), P("player")};
constexpr FieldDef kMoveToPositionFacing[] = {Q("x", 1), Q("y", 2), Q("z", 3), Q("angle", 6), B("flags", 8), L("units", 32)};
constexpr FieldDef kPlaceBeacon[] = {B("beacon_type", 16), Q("x", 1), Q("y", 2), Q("z", 3),
                                     Q("a", 9), Q("b", 10), Q("c", 11), P("player")};
constexpr FieldDef kTaunt[] = {B("taunt", 32)};
constexpr FieldDef kOneBit[] = {B("flag", 1)};
constexpr FieldDef kGarrison[] = {I("unit"), I("garrison")};
constexpr FieldDef kSetMarkerID[] = {I("object"), B("marker", 32)};
constexpr FieldDef kMoveToGarrison[] = {Q("x", 1), Q("y", 2), Q("z", 3), B("flags", 8), I("a"), I("b"), I("c")};
constexpr FieldDef kSetGUIIndex[] = {I("object"), B("index", 12)};
constexpr FieldDef kTacticalSpecialAbilityDummy[] = {L("users", 7), I("target"), I("id2"), B("ability", 7), I("id3"),
                                                     Q("x", 1),     Q("y", 2),   Q("z", 3),   P("player")};
constexpr FieldDef kSetAbilityAutofire[] = {L("units", 7), B("ability", 7), B("on", 1)};
constexpr FieldDef kPortExtension[] = {B("subtype", 16), {"data", K::Opaque}};

constexpr EventDef S(std::string_view n, std::span<const FieldDef> f = {}) { return {n, true, true, f}; }
constexpr EventDef N(std::string_view n, std::span<const FieldDef> f) { return {n, true, false, f}; }
constexpr EventDef kDead{};

constexpr std::array<EventDef, kMaxEventType + 1> kDefs = {
    N("FrameInfo", kFrameInfo),                // 0
    S("MoveToPosition", kMoveToPosition),      // 1
    S("MoveToObject", kMoveToObject),          // 2
    S("MoveObjectToObject", kMoveObjectToObject),
    S("Look", kLook),
    S("Select", kSelect),                      // 5
    S("Attack", kAttack),
    S("Production", kProduction),
    S("FleetManagement", kFleetManagement),
    S("Invade", kInvade),
    S("MoveThroughObjects", kMoveThroughObjects),  // 10
    S("Debug", kDebug),
    S("Reinforce", kReinforce),
    S("SpecialAbility", kSpecialAbility),
    S("MoveToRay", kMoveToRay),
    S("QuitGame"),                             // 15
    N("Chat", kChat),
    S("FrameSync", kFrameSync),
    N("PerformanceMetrics", kPerformanceMetrics),
    S("MoveToRayFacing", kMoveToRayFacing),
    S("Facing", kFacing),                      // 20
    S("Escort", kEscort),
    S("Retreat", kRetreat),
    kDead,                                     // 23 EVENT_DEPLOYMENT
    kDead,                                     // 24 EVENT_SPECIAL_WEAPON_TARGET_MODE
    S("SpecialWeaponFire", kSpecialWeaponFire),  // 25
    S("CinematicAnimation"),
    kDead,                                     // 27 EVENT_KILL
    S("BombingRun", kPoint),
    S("StopMovement"),
    S("ControlGroup", kControlGroup),          // 30
    S("SetupPhaseMove", kSetupPhaseMove),
    S("SetupPhaseTriggerEnd"),
    S("TacticalBuild", kTacticalBuild),
    S("TacticalSell", kOneId),
    S("Ally", kAlly),                          // 35
    S("SaveGame", kSaveGame),
    S("SelectAll"),
    S("TacticalSuperWeapon", kTacticalSuperWeapon),
    S("GameOptions", kGameOptions),
    S("TacticalSpecialAbility", kTacticalSpecialAbility),  // 40
    kDead,                                     // 41 EVENT_ACTIVATE_BEHAVIOR
    S("DistributeMoney"),
    S("SetUnitAbilityMode", kSetUnitAbilityMode),
    kDead,                                     // 44 EVENT_SPAWN_OBJECT
    S("RepairHardpoint", kRepairHardpoint),    // 45
    S("GalacticSell", kOneId),
    S("MoveToPositionFacing", kMoveToPositionFacing),
    S("PlaceBeacon", kPlaceBeacon),
    S("Withdrawl"),
    N("Taunt", kTaunt),                        // 50
    S("ResumeGame", kOneBit),
    S("Garrison", kGarrison),
    S("SetMarkerID", kSetMarkerID),
    S("PlanetaryBombard", kPoint),
    S("MoveToGarrison", kMoveToGarrison),      // 55
    S("SetGUIIndex", kSetGUIIndex),
    S("TacticalSpecialAbilityWithDummyTarget", kTacticalSpecialAbilityDummy),
    S("SetAbilityAutofire", kSetAbilityAutofire),  // 58
    kDead, kDead, kDead, kDead,                // 59-62
    S("PortExtension", kPortExtension),      // 63 (port-only)
};

constexpr std::array<Quantizer, kQuantizerCount> kQuant = {{
    {0.0f, 0.0f, 32},                                                        // 0 raw
    {-10000.0f, 10000.0f, 18}, {-10000.0f, 10000.0f, 18}, {-10000.0f, 10000.0f, 18},  // 1-3 position
    {-1e6f, 1e6f, 25},                                                       // 4 Look
    {0.0f, 86400.0f, 24},                                                    // 5 durations
    {-360.0f, 360.0f, 13},                                                   // 6 facing
    {0.0f, 2.0f, 5},                                                         // 7
    {0.0f, 1e6f, 24},                                                        // 8 amounts
    {-360.0f, 360.0f, 13}, {-360.0f, 360.0f, 13}, {-360.0f, 360.0f, 13},     // 9-11 beacon angles
}};

}  // namespace

const EventDef* event_def(uint8_t type) noexcept { return type <= kMaxEventType ? &kDefs[type] : nullptr; }
bool event_exists(uint8_t type) noexcept { return type <= kMaxEventType && kDefs[type].exists; }
bool event_scheduled(uint8_t type) noexcept { return event_exists(type) && kDefs[type].scheduled; }
std::string_view event_name(uint8_t type) noexcept { return event_exists(type) ? kDefs[type].name : "Unknown"; }

int field_index(EventType t, std::string_view name) noexcept {
    const EventDef* d = event_def(t);
    if (!d) return -1;
    for (size_t i = 0; i < d->fields.size(); ++i)
        if (d->fields[i].name == name) return static_cast<int>(i);
    return -1;
}

const Quantizer& quantizer(uint8_t index) noexcept { return kQuant[index < kQuantizerCount ? index : 0]; }

namespace {
double step_d(const Quantizer& q) {
    return (static_cast<double>(q.max) - static_cast<double>(q.min)) / static_cast<double>((1ull << q.bits) - 2u);
}
}  // namespace

float quantizer_step(uint8_t index) noexcept {
    const Quantizer& q = quantizer(index);
    return q.bits >= 32 ? 0.0f : static_cast<float>(step_d(q));
}

uint32_t quantize(uint8_t index, float v) noexcept {
    const Quantizer& q = quantizer(index);
    if (q.bits >= 32) return std::bit_cast<uint32_t>(v);
    if (!(v >= q.min)) v = q.min;  // also NaN
    if (v > q.max) v = q.max;
    const double t = (static_cast<double>(v) - static_cast<double>(q.min)) / step_d(q) + 0.5;
    const uint32_t top = (1u << q.bits) - 1u;
    const auto c = static_cast<uint32_t>(t);  // t >= 0.5: truncation == floor
    return c > top ? top : c;
}

float dequantize(uint8_t index, uint32_t code) noexcept {
    const Quantizer& q = quantizer(index);
    if (q.bits >= 32) return std::bit_cast<float>(code);
    const double step = step_d(q);
    const double r = static_cast<double>(code) * step + static_cast<double>(q.min);
    return std::fabs(r) < step ? 0.0f : static_cast<float>(r);
}
}  // namespace alamo::commands
