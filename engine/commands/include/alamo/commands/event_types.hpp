// G0-3 stage 2: the original's command ("event") catalogue, from docs/re/COMMANDS.md §1-§3.
//
// Every order is one event of a type id 0..58 (6 bits on the wire). 54 ids have a class: 50 scheduled
// (synced, executed at a frame) and 4 non-scheduled (FrameInfo 0, Chat 16, PerformanceMetrics 18,
// Taunt 50: executed on receipt, never sim state). Ids 23, 24, 27, 41, 44 are dead (names only).
// Id 63 is the port's own PortExtension event (59..62 unused).
// (COMMANDS.md §1.1 says 56 classes / 52 scheduled; its payload table and the 54 factories give the
// 54 ids above - the two extra classes have no type id and cannot appear on the wire.)
//
// Each type's payload is a list of fields (the original's wire order). Field kinds
// (widths are the original's bit-packed network wire, which is not ported - CU7; they bound valid values):
//   Bits n     unsigned n-bit value (n <= 32)
//   Id         32-bit object id (kNoObject = 0x3FFFFF = none; OBJECT_RUNTIME.md id space)
//   Player     player id (PlayerList index)
//   Quant i    float quantized with table entry i (§3.2); stored as the integer code
//   List n     n-bit element count, then that many Id
//   CountedIds Id list whose count is the value of an earlier Bits field (SetUnitAbilityMode)
//   Str/WStr   narrow / wide string
//   Opaque     byte blob (SpecialAbility kinds)
// A field with cond >= 0 is present only when field `cond` == cond_value (ControlGroup's 'add',
// SpecialWeaponFire's position/target branch).
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace alamo::commands {

inline constexpr uint8_t kMaxOriginalEventType = 58;
inline constexpr uint8_t kMaxEventType = 63;  // 6-bit ids; 59..62 unused, 63 = port extension
inline constexpr uint32_t kNoObject = 0x3FFFFF;

using ObjectId = uint32_t;  // wire object id (S1 maps handles <-> ids)

enum class EventType : uint8_t {
    FrameInfo = 0,
    MoveToPosition = 1,
    MoveToObject = 2,
    MoveObjectToObject = 3,
    Look = 4,
    Select = 5,
    Attack = 6,
    Production = 7,
    FleetManagement = 8,
    Invade = 9,
    MoveThroughObjects = 10,
    Debug = 11,
    Reinforce = 12,
    SpecialAbility = 13,
    MoveToRay = 14,
    QuitGame = 15,
    Chat = 16,
    FrameSync = 17,
    PerformanceMetrics = 18,
    MoveToRayFacing = 19,
    Facing = 20,
    Escort = 21,
    Retreat = 22,
    SpecialWeaponFire = 25,
    CinematicAnimation = 26,
    BombingRun = 28,
    StopMovement = 29,
    ControlGroup = 30,
    SetupPhaseMove = 31,
    SetupPhaseTriggerEnd = 32,
    TacticalBuild = 33,
    TacticalSell = 34,
    Ally = 35,
    SaveGame = 36,
    SelectAll = 37,
    TacticalSuperWeapon = 38,
    GameOptions = 39,
    TacticalSpecialAbility = 40,
    DistributeMoney = 42,
    SetUnitAbilityMode = 43,
    RepairHardpoint = 45,
    GalacticSell = 46,
    MoveToPositionFacing = 47,
    PlaceBeacon = 48,
    Withdrawl = 49,
    Taunt = 50,
    ResumeGame = 51,
    Garrison = 52,
    SetMarkerID = 53,
    PlanetaryBombard = 54,
    MoveToGarrison = 55,
    SetGUIIndex = 56,
    TacticalSpecialAbilityWithDummyTarget = 57,
    SetAbilityAutofire = 58,
    // Port-only (never in an original stream or on an original peer's wire): scheduled, payload
    // u16 subtype + Opaque data. For engine test hooks / features the original has no event for.
    PortExtension = 63,
};

// FleetManagement (id 8) payload field "op" (Bits 4): the galactic fleet operations (AI_GALACTIC_2 §5.4, STRATEGIC_GAPS 1.7,
// executor 0x5af260; rules per op: galactic campaign.hpp). Shared by the sim (engine/galactic), the AI and the galactic HUD.
// The values are command payload bytes: changing one changes the command stream and its CRCs.
enum FleetOp : uint8_t {
    kFleetForm = 0, kFleetJoin = 1, kFleetDetach = 2, kFleetMerge = 3, kFleetLaunch = 4, kFleetLand = 5,
    kFleetMoveNew = 6, kFleetMoveNewFlag = 7, kFleetSlot = 8
};

// [port: Gabriel 2026-11-13] Fleet cohesion: PortExtension subtype with one data byte (0 loose, 1 tight) setting the
// issuing player's cohesion in the running tactical battle (sim::TacticalBattle::set_fleet_cohesion; GameSession's port
// hook applies it). Shared by the game host and the UI hotkey. Not in retail.
inline constexpr uint16_t kPortFleetCohesionSubtype = 0x4643;  // "FC"

enum class FieldKind : uint8_t { Bits, Id, Player, Quant, List, CountedIds, Str, WStr, Opaque };

struct FieldDef {
    std::string_view name;
    FieldKind kind;
    uint8_t arg = 0;         // Bits: width; Quant: table index; List: count width; CountedIds: count field index
    int8_t cond = -1;        // present only if field[cond] == cond_value
    uint8_t cond_value = 0;
};

struct EventDef {
    std::string_view name;  // "MoveToPosition" ...; empty for dead ids
    bool exists = false;
    bool scheduled = false;
    std::span<const FieldDef> fields;
};

// nullptr for type > kMaxEventType; dead ids return a def with exists == false.
const EventDef* event_def(uint8_t type) noexcept;
inline const EventDef* event_def(EventType t) noexcept { return event_def(static_cast<uint8_t>(t)); }
bool event_exists(uint8_t type) noexcept;
bool event_scheduled(uint8_t type) noexcept;  // false for non-scheduled and dead ids
std::string_view event_name(uint8_t type) noexcept;  // "Unknown" for dead/out of range
// Field index by name, or -1.
int field_index(EventType t, std::string_view name) noexcept;

// ---- float quantizers (COMMANDS.md §3.2) -----------------------------------------------------------
// Entry i: {min, max, bits}; effective step = (max - min) / (2^bits - 2). Encode clamps to [min, max]
// and stores trunc((v - min) / step + 0.5); decode gives code * step + min, snapped to exactly 0 when
// its magnitude is below step. Entry 0 is the raw 32-bit float pattern. Arithmetic is IEEE double,
// result rounded to float [port choice: the original's precision is not stated; single precision
// cannot hold the 24/25-bit codes exactly]. Codes are the stored truth (EventData), so the float
// view is identical on every machine either way.
inline constexpr uint8_t kQuantizerCount = 12;
struct Quantizer {
    float min = 0.0f, max = 0.0f;
    uint8_t bits = 32;
};
const Quantizer& quantizer(uint8_t index) noexcept;  // index >= kQuantizerCount -> entry 0
float quantizer_step(uint8_t index) noexcept;
uint32_t quantize(uint8_t index, float v) noexcept;
float dequantize(uint8_t index, uint32_t code) noexcept;
inline float requantize(uint8_t index, float v) noexcept { return dequantize(index, quantize(index, v)); }

}  // namespace alamo::commands
