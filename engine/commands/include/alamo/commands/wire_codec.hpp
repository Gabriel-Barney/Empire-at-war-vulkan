// G0-3 stage 2: the original's bit-packed event wire format (COMMANDS.md §1.2, §3.1) - the network and
// parity format. Recordings keep the canonical byte format (command.hpp / recording.hpp).
//
// Bit stream: N-bit fields packed low bit first into consecutive bytes (bit i of the stream is bit
// (i % 8) of byte i / 8); the last byte is zero-padded.
//   event  := u6 type | P-bit player | [u26 execute frame, scheduled types only] | payload fields
//   fields := Bits n -> n bits | Id -> 32 | Player -> P | Quant i -> quantizer(i).bits (code)
//             List n -> n-bit count + count x 32 | CountedIds -> count x 32 (count sent earlier)
//             Str -> u16 length + 8 bits/char | WStr -> u16 length + 16 bits/unit
//             Opaque -> u16 byte count + 8 bits/byte                     [port: string/opaque layouts]
//   packet := u5 event count (<= 31) | events ...        scheduled and non-scheduled never share one
// P (player-id bits) is not recoverable from the decompile (COMMANDS.md §10): WireConfig::player_bits.
// The wire carries neither source nor sequence: a receiver stamps source Network and assigns arrival
// order (CommandScheduler::receive). The execute frame is 26 bits (frames < 2^26, ~25 days at 30 Hz).
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "alamo/commands/command.hpp"

namespace alamo::commands {

struct WireConfig {
    uint8_t player_bits = 4;          // P: unknown in the original; 4 covers 16 player slots
    size_t max_packet_bytes = 1024;   // packet split size [port: the original's limit is not documented]
    // [port: Gabriel] queueable unit actions: one extra bit (EventData::queued) after each scheduled event's
    // payload, port peers only. False = the original layout; packing a queued event then fails (InvalidPayload).
    bool port_queue_bit = false;
};

class BitWriter {
public:
    void put(uint32_t v, unsigned bits);  // low `bits` bits of v, low bit first (bits <= 32)
    const std::vector<uint8_t>& bytes() const noexcept { return buf_; }
    size_t bit_size() const noexcept { return bits_; }
    void patch(size_t bit_pos, uint32_t v, unsigned bits);  // overwrite earlier bits

private:
    std::vector<uint8_t> buf_;
    size_t bits_ = 0;
};

// Reads past the end return 0 (as the original) and set overrun().
class BitReader {
public:
    explicit BitReader(std::span<const uint8_t> d) noexcept : d_(d) {}
    uint32_t get(unsigned bits) noexcept;
    bool overrun() const noexcept { return overrun_; }
    size_t bit_pos() const noexcept { return pos_; }
    size_t bits_left() const noexcept { return d_.size() * 8 > pos_ ? d_.size() * 8 - pos_ : 0; }

private:
    std::span<const uint8_t> d_;
    size_t pos_ = 0;
    bool overrun_ = false;
};

enum class PackError : uint8_t { None = 0, InvalidPayload, PlayerOutOfRange, FrameOutOfRange, TooManyEvents };

// One event (header + payload). Scheduled events carry header.scheduled_tick as the execute frame.
PackError pack_event(BitWriter& w, const Command& c, const WireConfig& cfg);
// Reads one event; the header gets player, scheduled_tick (0 for non-scheduled), source Network,
// sequence 0 and issue_tick 0.
DecodeError unpack_event(BitReader& r, Command& out, const WireConfig& cfg);

// Splits events into packets (5-bit count, <= 31 events, <= max_packet_bytes unless a single event is
// larger). All events must be of one class (all scheduled or all non-scheduled): TooManyEvents is not
// used for that; mixing returns InvalidPayload.
PackError pack_packets(std::span<const Command> events, const WireConfig& cfg, std::vector<std::vector<uint8_t>>& out);
// Appends the packet's events to `out` (padding bits after the last event are ignored).
DecodeError unpack_packet(std::span<const uint8_t> packet, const WireConfig& cfg, std::vector<Command>& out);

}  // namespace alamo::commands
