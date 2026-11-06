// G0-3 command model: every order (player UI, network peers, replays; AI/Lua via the direct path) is
// one Command = header (issuer, source, arrival sequence, scheduled tick) + EventData (one of the
// original's event types, event_types.hpp, with its field values). Commands are queued by the
// scheduler (scheduler.hpp) and applied at their scheduled tick: a sim run is a pure function of
// (initial state, seed, drained command batches) - the lockstep model.
//
// Floats are stored as their quantizer codes (COMMANDS.md §3.2), so a value is canonical from the
// moment it is set: issuer, receivers and replays see bit-identical numbers (the original gets the
// same by re-quantizing in Execute). Object references are 32-bit wire ids (kNoObject = none).
//
// Two serializations:
//   * canonical v2 (this file; recordings): byte-aligned little-endian, no padding
//       command := u16 type | u8 player | u8 source | u32 sequence | u64 issue_tick | u64 scheduled_tick
//                  | u32 payload_len | payload                                         (28-byte header)
//       source byte: bits 0-6 CommandSource, bit 7 EventData::queued() [port: Gabriel, queueable actions]
//       payload := for each present field, in wire order:
//                  Bits/Id/Player/Quant -> u32 | List/CountedIds -> u32 count, u32 ids
//                  Str/Opaque -> u32 len, bytes | WStr -> u32 len, u16 units
//   * the original's bit-packed wire format (wire_codec.hpp; network / parity).
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/commands/event_types.hpp"

namespace alamo::commands {

inline constexpr uint16_t kWireVersion = 2;  // canonical format (v1 = stage-1 typed orders, retired)
inline constexpr size_t kCommandHeaderBytes = 28;
// [port] PlayerList cap (the original keeps a vector with no fixed cap). 32 since Q6 (was 16): Thrawn's Revenge campaigns
// have 22 players. The network wire format's player_bits (4) is separate and unchanged (multiplayer is out of scope).
inline constexpr uint8_t kMaxPlayerSlots = 32;
inline constexpr uint32_t kMaxPayloadBytes = 1u << 20;  // decode guard

// Who produced the order. Player/Network go through the scheduler; AI/Script use the direct path
// (CommandPipeline::execute_direct) like the original (COMMANDS.md §5).
enum class CommandSource : uint8_t { Player = 0, AI = 1, Script = 2, Network = 3 };
inline constexpr uint8_t kCommandSourceCount = 4;
inline constexpr uint8_t kCommandQueuedBit = 0x80;  // [port: Gabriel] canonical source byte: EventData::queued()

struct FieldValue {
    uint32_t u = 0;                // Bits / Id / Player / Quant code
    std::vector<uint32_t> ids;     // List / CountedIds
    std::string bytes;             // Str / Opaque
    std::u16string wide;           // WStr
    friend bool operator==(const FieldValue&, const FieldValue&) = default;
};

class EventData {
public:
    EventData() : EventData(EventType::StopMovement) {}
    explicit EventData(EventType t);  // all fields zero / empty; Id fields kNoObject

    EventType type() const noexcept { return type_; }
    uint8_t type_id() const noexcept { return static_cast<uint8_t>(type_); }
    const EventDef& def() const noexcept { return *event_def(type_); }
    size_t field_count() const noexcept { return values_.size(); }
    const FieldValue& value(size_t i) const { return values_.at(i); }
    FieldValue& value(size_t i) { return values_.at(i); }

    // By field name (throws std::out_of_range for an unknown name). Setters return *this.
    uint32_t u(std::string_view field) const { return values_[index(field)].u; }
    float f(std::string_view field) const;  // dequantized
    const std::vector<uint32_t>& ids(std::string_view field) const { return values_[index(field)].ids; }
    const std::string& str(std::string_view field) const { return values_[index(field)].bytes; }
    const std::u16string& wstr(std::string_view field) const { return values_[index(field)].wide; }
    EventData& set(std::string_view field, uint32_t v);
    EventData& set_f(std::string_view field, float v);  // quantizes
    EventData& set_ids(std::string_view field, std::vector<uint32_t> v);  // CountedIds also sets the count
    EventData& set_str(std::string_view field, std::string v);
    EventData& set_wstr(std::string_view field, std::u16string v);
    // Opaque / Str as raw bytes.
    EventData& set_bytes(std::string_view field, std::span<const uint8_t> v);
    std::span<const uint8_t> bytes_view(std::string_view field) const;

    // Field present in this instance (conditional fields)?
    bool present(size_t i) const;
    // Every present field fits its wire width (Bits/List counts/CountedIds count, Quant code range)
    // and every absent conditional field holds its default (0 / empty / kNoObject).
    // Player fields are checked against WireConfig at pack time.
    bool valid() const;

    // [port: Gabriel] Queueable unit actions (docs/PHASE4.md): true = the order runs after the units' current
    // orders instead of replacing them (shift+click). Not a payload field of the original: the canonical
    // format carries it as bit 7 of the header's source byte (kCommandQueuedBit; unqueued commands encode
    // byte-identically to before), the bit-packed wire format only with WireConfig::port_queue_bit.
    bool queued() const noexcept { return queued_; }
    EventData& set_queued(bool q) noexcept {
        queued_ = q;
        return *this;
    }

    friend bool operator==(const EventData&, const EventData&) = default;

private:
    size_t index(std::string_view field) const;
    EventType type_;
    std::vector<FieldValue> values_;
    bool queued_ = false;  // [port: Gabriel]
};

using CommandPayload = EventData;

struct CommandHeader {
    uint8_t player = 0;                          // issuing player id (PlayerList index)
    CommandSource source = CommandSource::Player;
    uint32_t sequence = 0;                       // per-player arrival order (the original has none on the wire)
    uint64_t issue_tick = 0;                     // tick the issuer was in when submitting
    uint64_t scheduled_tick = 0;                 // execute frame: drained + applied at the start of this tick
    friend bool operator==(const CommandHeader&, const CommandHeader&) = default;
};

struct Command {
    CommandHeader header;
    EventData payload;
    friend bool operator==(const Command&, const Command&) = default;
};

enum class DecodeError : uint8_t { None = 0, Truncated, UnknownType, BadField, TrailingBytes, TooLarge };
std::string_view decode_error_name(DecodeError e) noexcept;

// Canonical payload / command (appended to `out`).
void encode_payload(const EventData& p, std::vector<uint8_t>& out);
DecodeError decode_payload(uint16_t type, std::span<const uint8_t> bytes, EventData& out);
void encode_command(const Command& c, std::vector<uint8_t>& out);
std::vector<uint8_t> encode_command(const Command& c);
DecodeError decode_command(std::span<const uint8_t> in, size_t& pos, Command& out);

// Canonical drain order: scheduled tick, then player id (PlayerList index), then arrival sequence.
// Matches the original executor (COMMANDS.md §4.5).
inline bool drain_order_less(const CommandHeader& a, const CommandHeader& b) noexcept {
    if (a.scheduled_tick != b.scheduled_tick) return a.scheduled_tick < b.scheduled_tick;
    if (a.player != b.player) return a.player < b.player;
    return a.sequence < b.sequence;
}

// ---- builders for common orders (fields as COMMANDS.md §2; "Sel" = issuer's selection) ----------
namespace ev {
EventData move_to_position(float x, float y, float z, uint8_t flags = 0);  // moves Sel
EventData move_to_object(ObjectId target, std::vector<ObjectId> movers = {});  // empty = Sel
EventData attack(ObjectId target, std::vector<ObjectId> attackers);
EventData escort(ObjectId target, uint32_t option = 0);  // guard
EventData stop();                                         // stops Sel
EventData select(std::vector<ObjectId> ids);              // max 127
EventData select_all();
EventData control_group(uint8_t group, bool assign, bool add = false);
EventData production(uint16_t type_index, ObjectId location, bool cancel = false, uint8_t queue = 0, uint8_t index = 0);
EventData tactical_build(ObjectId pad, uint16_t type_index, uint8_t player);
EventData ability(std::vector<ObjectId> users, uint8_t ability_type, ObjectId target);
EventData move_object_to_object(ObjectId object, ObjectId target);  // galactic fleet hop
EventData retreat(bool request);
// Port-only event (id 63): engine test hooks / features without an original event.
EventData port_extension(uint16_t subtype, std::span<const uint8_t> data);
}  // namespace ev

}  // namespace alamo::commands
