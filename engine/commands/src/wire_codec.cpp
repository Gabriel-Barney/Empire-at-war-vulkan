#include "alamo/commands/wire_codec.hpp"

namespace alamo::commands {

void BitWriter::put(uint32_t v, unsigned bits) {
    for (unsigned i = 0; i < bits; ++i, ++bits_) {
        if ((bits_ & 7u) == 0) buf_.push_back(0);
        if ((v >> i) & 1u) buf_.back() = static_cast<uint8_t>(buf_.back() | (1u << (bits_ & 7u)));
    }
}

void BitWriter::patch(size_t bit_pos, uint32_t v, unsigned bits) {
    for (unsigned i = 0; i < bits; ++i) {
        const size_t p = bit_pos + i;
        uint8_t& b = buf_.at(p >> 3);
        const auto m = static_cast<uint8_t>(1u << (p & 7u));
        b = static_cast<uint8_t>(((v >> i) & 1u) ? (b | m) : (b & ~m));
    }
}

uint32_t BitReader::get(unsigned bits) noexcept {
    uint32_t v = 0;
    for (unsigned i = 0; i < bits; ++i, ++pos_) {
        if ((pos_ >> 3) >= d_.size()) {
            overrun_ = true;
            continue;  // past the end reads 0
        }
        if ((d_[pos_ >> 3] >> (pos_ & 7u)) & 1u) v |= 1u << i;
    }
    return v;
}

namespace {

unsigned field_bits(const FieldDef& f, const WireConfig& cfg) {
    switch (f.kind) {
        case FieldKind::Bits: return f.arg;
        case FieldKind::Id: return 32;
        case FieldKind::Player: return cfg.player_bits;
        case FieldKind::Quant: return quantizer(f.arg).bits;
        default: return 0;
    }
}

}  // namespace

PackError pack_event(BitWriter& w, const Command& c, const WireConfig& cfg) {
    const EventData& e = c.payload;
    if (!e.valid()) return PackError::InvalidPayload;
    if (e.queued() && !(cfg.port_queue_bit && e.def().scheduled)) return PackError::InvalidPayload;  // [port: Gabriel]
    const unsigned pb = cfg.player_bits;
    auto player_ok = [pb](uint32_t p) { return pb >= 32 || p < (1u << pb); };
    if (!player_ok(c.header.player)) return PackError::PlayerOutOfRange;
    const EventDef& d = e.def();
    for (size_t i = 0; i < d.fields.size(); ++i)
        if (d.fields[i].kind == FieldKind::Player && e.present(i) && !player_ok(e.value(i).u))
            return PackError::PlayerOutOfRange;
    if (d.scheduled && c.header.scheduled_tick >= (1ull << kFrameBits)) return PackError::FrameOutOfRange;

    w.put(e.type_id(), kEventTypeBits);
    w.put(c.header.player, pb);
    if (d.scheduled) w.put(static_cast<uint32_t>(c.header.scheduled_tick), kFrameBits);
    for (size_t i = 0; i < d.fields.size(); ++i) {
        if (!e.present(i)) continue;
        const FieldDef& f = d.fields[i];
        const FieldValue& v = e.value(i);
        switch (f.kind) {
            case FieldKind::Bits:
            case FieldKind::Id:
            case FieldKind::Player:
            case FieldKind::Quant: w.put(v.u, field_bits(f, cfg)); break;
            case FieldKind::List:
                w.put(static_cast<uint32_t>(v.ids.size()), f.arg);
                for (uint32_t id : v.ids) w.put(id, 32);
                break;
            case FieldKind::CountedIds:
                for (uint32_t id : v.ids) w.put(id, 32);
                break;
            case FieldKind::Str:
            case FieldKind::Opaque:
                w.put(static_cast<uint32_t>(v.bytes.size()), 16);
                for (char ch : v.bytes) w.put(static_cast<uint8_t>(ch), 8);
                break;
            case FieldKind::WStr:
                w.put(static_cast<uint32_t>(v.wide.size()), 16);
                for (char16_t ch : v.wide) w.put(static_cast<uint16_t>(ch), 16);
                break;
        }
    }
    if (cfg.port_queue_bit && d.scheduled) w.put(e.queued() ? 1u : 0u, 1);  // [port: Gabriel] queued flag
    return PackError::None;
}

DecodeError unpack_event(BitReader& r, Command& out, const WireConfig& cfg) {
    const auto type = static_cast<uint8_t>(r.get(kEventTypeBits));
    if (r.overrun()) return DecodeError::Truncated;
    if (!event_exists(type)) return DecodeError::UnknownType;
    Command c;
    c.payload = EventData(static_cast<EventType>(type));
    c.header.source = CommandSource::Network;
    const uint32_t player = r.get(cfg.player_bits);
    if (player >= kMaxPlayerSlots) return DecodeError::BadField;
    c.header.player = static_cast<uint8_t>(player);
    EventData& e = c.payload;
    const EventDef& d = e.def();
    if (d.scheduled) c.header.scheduled_tick = r.get(kFrameBits);
    for (size_t i = 0; i < d.fields.size() && !r.overrun(); ++i) {
        if (!e.present(i)) continue;
        const FieldDef& f = d.fields[i];
        FieldValue& v = e.value(i);
        switch (f.kind) {
            case FieldKind::Bits:
            case FieldKind::Id:
            case FieldKind::Player:
            case FieldKind::Quant: v.u = r.get(field_bits(f, cfg)); break;
            case FieldKind::List:
            case FieldKind::CountedIds: {
                const uint32_t n = f.kind == FieldKind::List ? r.get(f.arg) : e.value(f.arg).u;
                if (r.bits_left() / 32 < n) return DecodeError::Truncated;
                v.ids.resize(n);
                for (auto& id : v.ids) id = r.get(32);
                break;
            }
            case FieldKind::Str:
            case FieldKind::Opaque: {
                const uint32_t n = r.get(16);
                if (r.bits_left() / 8 < n) return DecodeError::Truncated;
                v.bytes.resize(n);
                for (auto& ch : v.bytes) ch = static_cast<char>(r.get(8));
                break;
            }
            case FieldKind::WStr: {
                const uint32_t n = r.get(16);
                if (r.bits_left() / 16 < n) return DecodeError::Truncated;
                v.wide.resize(n);
                for (auto& ch : v.wide) ch = static_cast<char16_t>(r.get(16));
                break;
            }
        }
    }
    if (cfg.port_queue_bit && d.scheduled && !r.overrun()) e.set_queued(r.get(1) != 0);  // [port: Gabriel]
    if (r.overrun()) return DecodeError::Truncated;
    if (!e.valid()) return DecodeError::BadField;
    out = std::move(c);
    return DecodeError::None;
}

PackError pack_packets(std::span<const Command> events, const WireConfig& cfg, std::vector<std::vector<uint8_t>>& out) {
    if (events.empty()) return PackError::None;
    const bool sched = events[0].payload.def().scheduled;
    for (const Command& c : events)
        if (c.payload.def().scheduled != sched) return PackError::InvalidPayload;
    constexpr uint32_t kMaxCount = (1u << kPacketCountBits) - 1u;
    size_t i = 0;
    while (i < events.size()) {
        BitWriter w;
        w.put(0, kPacketCountBits);
        uint32_t n = 0;
        while (i < events.size() && n < kMaxCount) {
            BitWriter one;  // measure the event first: does it still fit?
            const PackError e = pack_event(one, events[i], cfg);
            if (e != PackError::None) return e;
            const size_t total_bits = w.bit_size() + one.bit_size();
            if (n > 0 && (total_bits + 7) / 8 > cfg.max_packet_bytes) break;
            pack_event(w, events[i], cfg);
            ++n;
            ++i;
        }
        w.patch(0, n, kPacketCountBits);
        out.push_back(w.bytes());
    }
    return PackError::None;
}

DecodeError unpack_packet(std::span<const uint8_t> packet, const WireConfig& cfg, std::vector<Command>& out) {
    BitReader r(packet);
    const uint32_t n = r.get(kPacketCountBits);
    if (r.overrun()) return DecodeError::Truncated;
    std::vector<Command> got;
    for (uint32_t k = 0; k < n; ++k) {
        Command c;
        const DecodeError e = unpack_event(r, c, cfg);
        if (e != DecodeError::None) return e;
        got.push_back(std::move(c));
    }
    if (r.bits_left() >= 8) return DecodeError::TrailingBytes;
    for (auto& c : got) out.push_back(std::move(c));
    return DecodeError::None;
}

}  // namespace alamo::commands
