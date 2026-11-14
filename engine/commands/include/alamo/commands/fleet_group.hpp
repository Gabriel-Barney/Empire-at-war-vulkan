// [port] Fleet groups (docs/DECISIONS.md "Fleet groups: exception to the current focus", Gabriel 2026-11-13): persistent,
// player-made unit groups in tactical battles. Not retail: every fleet group order is one PortExtension event (id 63) of
// subtype kFleetGroupSubtype, so the original event catalogue is unchanged.
//
// Payload (`data` bytes): op (u8), then per op
//   Create       nothing          the issuer's selection becomes a new group (its units leave their old groups)
//   Dissolve     nothing          every group with a unit in the selection is removed
//   RemoveUnits  nothing          the selected units leave their groups
//   SetLock      locked (u8 0/1)  every group with a unit in the selection is locked / unlocked
//   Rename       group (u16 LE), name length (u8), name bytes (UTF-8, at most kFleetGroupNameMax)
// The sim applies them at the command phase from its synced selection (sim/fleet_groups.hpp).
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "alamo/commands/command.hpp"

namespace alamo::commands {

inline constexpr uint16_t kFleetGroupSubtype = 0x4647;  // "FG"
inline constexpr size_t kFleetGroupNameMax = 32;         // bytes

enum class FleetGroupOp : uint8_t { Create = 0, Dissolve = 1, RemoveUnits = 2, SetLock = 3, Rename = 4 };

struct FleetGroupCommand {
    FleetGroupOp op = FleetGroupOp::Create;
    bool locked = false;  // SetLock
    uint16_t group = 0;   // Rename
    std::string name;     // Rename (cut to kFleetGroupNameMax bytes when encoded)
};

// The PortExtension event of `c`.
EventData fleet_group_event(const FleetGroupCommand& c);
// The command in a kFleetGroupSubtype payload; nullopt for an unknown op, a short / long payload or an over-long name.
std::optional<FleetGroupCommand> parse_fleet_group(std::span<const uint8_t> data);

}  // namespace alamo::commands
