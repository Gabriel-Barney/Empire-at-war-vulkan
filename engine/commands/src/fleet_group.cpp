#include "alamo/commands/fleet_group.hpp"

#include <vector>

namespace alamo::commands {

EventData fleet_group_event(const FleetGroupCommand& c) {
    std::vector<uint8_t> b;
    b.push_back(static_cast<uint8_t>(c.op));
    switch (c.op) {
        case FleetGroupOp::SetLock: b.push_back(c.locked ? 1u : 0u); break;
        case FleetGroupOp::Rename: {
            b.push_back(static_cast<uint8_t>(c.group & 0xFFu));
            b.push_back(static_cast<uint8_t>(c.group >> 8));
            const size_t n = c.name.size() < kFleetGroupNameMax ? c.name.size() : kFleetGroupNameMax;
            b.push_back(static_cast<uint8_t>(n));
            for (size_t i = 0; i < n; ++i) b.push_back(static_cast<uint8_t>(c.name[i]));
            break;
        }
        case FleetGroupOp::Create:
        case FleetGroupOp::Dissolve:
        case FleetGroupOp::RemoveUnits: break;
    }
    return ev::port_extension(kFleetGroupSubtype, std::span<const uint8_t>(b));
}

std::optional<FleetGroupCommand> parse_fleet_group(std::span<const uint8_t> data) {
    if (data.empty()) return std::nullopt;
    FleetGroupCommand c;
    switch (data[0]) {
        case static_cast<uint8_t>(FleetGroupOp::Create):
        case static_cast<uint8_t>(FleetGroupOp::Dissolve):
        case static_cast<uint8_t>(FleetGroupOp::RemoveUnits):
            if (data.size() != 1) return std::nullopt;
            c.op = static_cast<FleetGroupOp>(data[0]);
            return c;
        case static_cast<uint8_t>(FleetGroupOp::SetLock):
            if (data.size() != 2 || data[1] > 1) return std::nullopt;
            c.op = FleetGroupOp::SetLock;
            c.locked = data[1] != 0;
            return c;
        case static_cast<uint8_t>(FleetGroupOp::Rename): {
            if (data.size() < 4) return std::nullopt;
            const size_t n = data[3];
            if (n > kFleetGroupNameMax || data.size() != 4 + n) return std::nullopt;
            c.op = FleetGroupOp::Rename;
            c.group = static_cast<uint16_t>(data[1] | (data[2] << 8));
            c.name.assign(reinterpret_cast<const char*>(data.data() + 4), n);
            return c;
        }
        default: return std::nullopt;
    }
}

}  // namespace alamo::commands
