// alamo_commands tests (no framework; non-zero exit on failure).
// Semantics under test: docs/re/COMMANDS.md (event catalogue §2, wire §1.2/§3, scheduling §4).
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "alamo/commands/command.hpp"
#include "alamo/commands/event_types.hpp"
#include "alamo/commands/events.hpp"
#include "alamo/commands/pipeline.hpp"
#include "alamo/commands/recording.hpp"
#include "alamo/commands/scheduler.hpp"
#include "alamo/commands/wire_codec.hpp"
#include "alamo/core/bytes.hpp"
#include "alamo/core/hash.hpp"

using namespace alamo;
using namespace alamo::commands;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

namespace {

struct Rng {  // splitmix64
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t next() {
        uint64_t z = (s += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    uint32_t below(uint32_t n) { return n ? static_cast<uint32_t>(next() % n) : 0; }
    uint32_t bits(unsigned b) { return b >= 32 ? static_cast<uint32_t>(next()) : static_cast<uint32_t>(next() & ((1ull << b) - 1)); }
};

std::vector<uint8_t> scheduled_types() {
    std::vector<uint8_t> t;
    for (uint8_t i = 0; i <= kMaxEventType; ++i)
        if (event_scheduled(i)) t.push_back(i);
    return t;
}

// Random valid event of `type` (Player fields < 16 so they fit P = 4).
EventData random_event(Rng& r, uint8_t type) {
    EventData e(static_cast<EventType>(type));
    const EventDef& d = e.def();
    for (size_t i = 0; i < d.fields.size(); ++i) {
        const FieldDef& f = d.fields[i];
        FieldValue& v = e.value(i);
        switch (f.kind) {
            case FieldKind::Bits: v.u = r.bits(f.arg); break;
            case FieldKind::Id: v.u = r.below(4) == 0 ? kNoObject : static_cast<uint32_t>(r.next()); break;
            case FieldKind::Player: v.u = r.below(16); break;
            case FieldKind::Quant: v.u = quantize(f.arg, static_cast<float>(r.below(2000000)) * 0.01f - 10000.0f); break;
            case FieldKind::List: {
                v.ids.resize(std::min<uint32_t>(r.below(6), f.arg >= 32 ? 0xFFFFFFFFu : (1u << f.arg) - 1));  // arg = count bits (32: no shift)
                for (auto& id : v.ids) id = static_cast<uint32_t>(r.next());
                break;
            }
            case FieldKind::CountedIds: {
                v.ids.resize(e.value(f.arg).u);
                for (auto& id : v.ids) id = static_cast<uint32_t>(r.next());
                break;
            }
            case FieldKind::Str:
            case FieldKind::Opaque:
                v.bytes.resize(r.below(6));
                for (auto& c : v.bytes) c = static_cast<char>(r.below(256));
                break;
            case FieldKind::WStr:
                v.wide.resize(r.below(6));
                for (auto& c : v.wide) c = static_cast<char16_t>(r.below(65536));
                break;
        }
    }
    // Keep CountedIds consistent when the count field was randomised after the list was sized.
    for (size_t i = 0; i < d.fields.size(); ++i)
        if (d.fields[i].kind == FieldKind::CountedIds) e.value(i).ids.resize(e.value(d.fields[i].arg).u);
    for (size_t i = 0; i < d.fields.size(); ++i)  // absent conditional fields keep their defaults
        if (!e.present(i)) {
            e.value(i) = FieldValue{};
            if (d.fields[i].kind == FieldKind::Id) e.value(i).u = kNoObject;
        }
    return e;
}

EventData random_any(Rng& r) {
    static const auto types = scheduled_types();
    return random_event(r, types[r.below(static_cast<uint32_t>(types.size()))]);
}

std::vector<uint8_t> from_hex(const char* hex) {
    std::string h;
    for (const char* q = hex; *q; ++q)
        if (*q != ' ') h += *q;
    std::vector<uint8_t> out;
    auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (size_t i = 0; i + 1 < h.size(); i += 2) out.push_back(static_cast<uint8_t>(nib(h[i]) * 16 + nib(h[i + 1])));
    return out;
}

// ---- catalogue + quantizers ------------------------------------------------------------------------
void test_catalogue() {
    uint32_t exists = 0, sched = 0, original = 0;
    for (uint8_t i = 0; i <= kMaxEventType; ++i) {
        exists += event_exists(i) ? 1 : 0;
        sched += event_scheduled(i) ? 1 : 0;
        original += (event_exists(i) && i <= kMaxOriginalEventType) ? 1 : 0;
    }
    CHECK(original == 54);  // ids 0..58 minus 23, 24, 27, 41, 44
    CHECK(exists == 55);    // + PortExtension 63
    CHECK(sched == 51);     // 50 original scheduled + PortExtension
    for (int dead : {23, 24, 27, 41, 44, 59, 62}) CHECK(!event_exists(static_cast<uint8_t>(dead)));
    for (int n : {0, 16, 18, 50}) CHECK(event_exists(static_cast<uint8_t>(n)) && !event_scheduled(static_cast<uint8_t>(n)));
    CHECK(event_name(29) == "StopMovement" && event_name(23) == "Unknown");
    CHECK(field_index(EventType::ControlGroup, "add") == 2);
    bool threw = false;
    try {
        EventData bad(static_cast<EventType>(27));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

void test_quantizers() {
    // Bits = smallest b with 2^b - 1 >= ceil((max - min) / step) + 1 (COMMANDS.md §3.2 nominal steps).
    const double steps[kQuantizerCount] = {0, 0.1, 0.1, 0.1, 0.1, 0.01, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1};
    for (uint8_t i = 1; i < kQuantizerCount; ++i) {
        const Quantizer& q = quantizer(i);
        const double need = std::ceil((static_cast<double>(q.max) - q.min) / steps[i] - 1e-6) + 1;
        unsigned b = 1;
        while (static_cast<double>((1ull << b) - 1) < need) ++b;
        CHECK(b == q.bits);
    }
    CHECK(quantizer(1).bits == 18 && quantizer(4).bits == 25 && quantizer(6).bits == 13 && quantizer(7).bits == 5);
    CHECK(quantize(1, 0.0f) == 131071 && dequantize(1, 131071) == 0.0f);
    CHECK(quantize(1, 10000.0f) == 262142 && quantize(1, 1e9f) == 262142);
    CHECK(quantize(1, -10000.0f) == 0 && quantize(1, -1e9f) == 0 && dequantize(1, 0) == -10000.0f);
    CHECK(quantize(1, std::numeric_limits<float>::quiet_NaN()) == 0);
    CHECK(dequantize(1, 131072) == 0.0f);  // |r| < step snaps to 0
    CHECK(std::fabs(requantize(6, 90.0f) - 90.0f) < quantizer_step(6));
    CHECK(quantize(0, -0.0f) == 0x80000000u && std::signbit(dequantize(0, 0x80000000u)));
    Rng r(3);
    for (int k = 0; k < 5000; ++k) {
        // Tables whose codes are exact in float (positions, angles); 4/5/8 exceed float precision.
        static const uint8_t kExact[] = {1, 2, 3, 6, 7, 9, 10, 11};
        const uint8_t i = kExact[r.below(8)];
        const Quantizer& q = quantizer(i);
        const float v = q.min + (q.max - q.min) * static_cast<float>(r.below(1u << 20)) / static_cast<float>(1u << 20);
        const float a = requantize(i, v);
        CHECK(requantize(i, a) == a);                    // canonical after one pass
        CHECK(std::fabs(a - v) <= quantizer_step(i));     // within one step
    }
    // Setting a float stores the code: issuer and receivers see the same bits.
    const EventData m = ev::move_to_position(123.456f, -0.01f, 5000.0f);
    CHECK(m.f("x") == requantize(1, 123.456f) && m.f("y") == 0.0f);
}

// ---- canonical serialization ----------------------------------------------------------------------
void test_round_trip() {
    Rng r(1);
    for (int i = 0; i < 3000; ++i) {
        Command c;
        c.header.player = static_cast<uint8_t>(r.below(kMaxPlayerSlots));
        c.header.source = static_cast<CommandSource>(r.below(4));
        c.header.sequence = static_cast<uint32_t>(r.next());
        c.header.issue_tick = r.next();
        c.header.scheduled_tick = r.next();
        uint8_t type = 0;
        do type = static_cast<uint8_t>(r.below(kMaxEventType + 1));
        while (!event_exists(type));
        c.payload = random_event(r, type);  // includes non-scheduled types
        CHECK(c.payload.valid());
        const auto bytes = encode_command(c);
        size_t pos = 0;
        Command d;
        CHECK(decode_command(bytes, pos, d) == DecodeError::None);
        CHECK(pos == bytes.size());
        CHECK(d == c);
    }
}

void test_golden_canonical() {
    Command c;
    c.header = CommandHeader{2, CommandSource::AI, 7, 100, 101};
    c.payload = ev::move_to_position(0.0f, 10000.0f, -10000.0f, 0x08);
    c.payload.set("switch_locomotor", 1);
    const auto move = from_hex(
        "0100 02 01 07000000 6400000000000000 6500000000000000 14000000"
        "ffff0100 feff0300 00000000 08000000 01000000");
    CHECK(encode_command(c) == move);

    CommandBatch batch{101, {c}};
    std::vector<uint8_t> bb;
    encode_batch(batch, bb);
    std::vector<uint8_t> want = from_hex("6500000000000000 01000000");
    want.insert(want.end(), move.begin(), move.end());
    CHECK(bb == want);

    CommandRecording rec;
    rec.seed = 0x55;
    rec.end_tick = 102;
    rec.batches.push_back(batch);
    std::vector<uint8_t> want_file = from_hex(
        "41434d44 0200 0000 01000000 08 00 00 00 01000000 02000000"
        "5500000000000000 0000000000000000 6600000000000000 00000000 01000000");
    want_file.insert(want_file.end(), want.begin(), want.end());
    CHECK(rec.to_bytes() == want_file);
}

void test_decode_errors() {
    Command c;
    c.header = CommandHeader{1, CommandSource::Player, 3, 4, 5};
    c.payload = ev::attack(9, {1, 2});
    const auto good = encode_command(c);
    Command d;
    size_t pos = 0;
    for (size_t n = 0; n < good.size(); ++n) {
        std::vector<uint8_t> t(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n));
        pos = 0;
        CHECK(decode_command(t, pos, d) == DecodeError::Truncated && pos == 0);
    }
    auto bad = good;
    bad[0] = 23;  // dead id
    pos = 0;
    CHECK(decode_command(bad, pos, d) == DecodeError::UnknownType);
    bad = good;
    bad[kCommandHeaderBytes + 8] = 2;  // Attack flag0 (1 bit) = 2
    pos = 0;
    CHECK(decode_command(bad, pos, d) == DecodeError::BadField);
    bad = good;
    bad.push_back(0);
    bad[24] = static_cast<uint8_t>(bad[24] + 1);
    pos = 0;
    CHECK(decode_command(bad, pos, d) == DecodeError::TrailingBytes);
    EventData sel = ev::select(std::vector<ObjectId>(128, 1));  // list7: max 127
    CHECK(!sel.valid());
    EventData sum(EventType::SetUnitAbilityMode);
    sum.set("count", 2);  // count without ids
    CHECK(!sum.valid());
    sum.set_ids("units", {4, 5});
    CHECK(sum.valid() && sum.u("count") == 2);
}

// [port: Gabriel] queueable unit actions: EventData::queued() in the canonical source byte (bit 7) and,
// opt-in, one bit after a scheduled event on the port wire. Unqueued encodings are unchanged.
void test_queued_flag() {
    Command c;
    c.header = CommandHeader{2, CommandSource::AI, 7, 100, 101};
    c.payload = ev::move_to_position(0.0f, 10000.0f, -10000.0f, 0x08);
    c.payload.set("switch_locomotor", 1);
    const auto plain = encode_command(c);
    CHECK(plain[3] == 0x01);  // source AI, no queue bit (test_golden_canonical bytes)
    c.payload.set_queued(true);
    const auto q = encode_command(c);
    CHECK(q.size() == plain.size() && q[3] == (0x01 | kCommandQueuedBit));
    for (size_t i = 0; i < q.size(); ++i)
        if (i != 3) CHECK(q[i] == plain[i]);
    size_t pos = 0;
    Command d;
    CHECK(decode_command(q, pos, d) == DecodeError::None && d == c && d.payload.queued() && d.header.source == CommandSource::AI);
    pos = 0;
    CHECK(decode_command(plain, pos, d) == DecodeError::None && !d.payload.queued());
    auto bad = q;
    bad[3] = static_cast<uint8_t>(kCommandQueuedBit | 0x05);  // source 5 is still out of range
    pos = 0;
    CHECK(decode_command(bad, pos, d) == DecodeError::BadField);
    // Round trip through a recording (replays keep the flag).
    CommandRecording rec;
    rec.end_tick = 102;
    rec.batches.push_back(CommandBatch{101, {c}});
    CommandRecording back;
    std::string err;
    CHECK(CommandRecording::from_bytes(rec.to_bytes(), back, &err) && back.batches.size() == 1 &&
          back.batches[0].commands.size() == 1 && back.batches[0].commands[0].payload.queued());
    // Bit-packed wire: the original layout cannot carry it; the port wire adds one bit.
    WireConfig orig, port;
    port.port_queue_bit = true;
    BitWriter w0, w1, w2;
    CHECK(pack_event(w0, c, orig) == PackError::InvalidPayload);
    CHECK(pack_event(w1, c, port) == PackError::None);
    Command u = c;
    u.payload.set_queued(false);
    CHECK(pack_event(w2, u, port) == PackError::None && w2.bit_size() == w1.bit_size());
    BitReader r1(w1.bytes()), r2(w2.bytes());
    Command b1, b2;
    CHECK(unpack_event(r1, b1, port) == DecodeError::None && b1.payload == c.payload && b1.payload.queued());
    CHECK(unpack_event(r2, b2, port) == DecodeError::None && b2.payload == u.payload && !b2.payload.queued());
    // Equality includes the flag.
    EventData a = ev::stop(), b = ev::stop();
    b.set_queued(true);
    CHECK(!(a == b));
}

// ---- bit-packed wire --------------------------------------------------------------------------------
void test_wire_golden() {
    WireConfig w3;
    w3.player_bits = 3;
    // Packet [count 1] StopMovement(29), player 1, frame 7:
    //   bits: count 1 (5) | type 29 (6) | player 1 (3) | frame 7 (26) = 40 bits.
    Command stop;
    stop.header.player = 1;
    stop.header.scheduled_tick = 7;
    stop.payload = ev::stop();
    std::vector<std::vector<uint8_t>> pk;
    CHECK(pack_packets(std::span<const Command>(&stop, 1), w3, pk) == PackError::None);
    CHECK(pk.size() == 1 && pk[0] == from_hex("a1cb010000"));

    // MoveToPosition(1), player 0, frame 0, x=0 (code 0x1FFFF), y=10000 (0x3FFFE), z=-10000 (0),
    // flags 0x08, cancel 1: 6+3+26+3*18+8+1 = 98 bits, low bit first.
    Command mv;
    mv.payload = ev::move_to_position(0.0f, 10000.0f, -10000.0f, 0x08);
    mv.payload.set("switch_locomotor", 1);
    BitWriter bw;
    CHECK(pack_event(bw, mv, w3) == PackError::None);
    CHECK(bw.bit_size() == 98);
    CHECK(bw.bytes() == from_hex("01000000f8ffcfff7f00001002"));
    BitReader br(bw.bytes());
    Command back;
    CHECK(unpack_event(br, back, w3) == DecodeError::None);
    CHECK(back.payload == mv.payload && back.header.source == CommandSource::Network);

    // Non-scheduled: no frame field. FrameInfo: 6 + 3 + 26 + 22 + 16 = 73 bits.
    Command fi;
    fi.payload = EventData(EventType::FrameInfo);
    fi.payload.set("frame", 5).set("command_count", 9).set("crc", 0xBEEF);
    BitWriter fw;
    CHECK(pack_event(fw, fi, w3) == PackError::None && fw.bit_size() == 73);
    // Conditional fields: ControlGroup recall = 5 payload bits, assign = 6.
    Command cg;
    cg.payload = ev::control_group(3, false);
    BitWriter g1, g2;
    pack_event(g1, cg, w3);
    cg.payload = ev::control_group(3, true, true);
    pack_event(g2, cg, w3);
    CHECK(g1.bit_size() == 35 + 5 && g2.bit_size() == 35 + 6);
    // SpecialWeaponFire: position branch (3 x 18 bits) vs target branch (32 bits).
    Command sw;
    sw.payload = EventData(EventType::SpecialWeaponFire);
    sw.payload.set("weapon", 4).set("player", 2).set("at_position", 1).set_f("x", 1.0f);
    BitWriter s1, s2;
    CHECK(pack_event(s1, sw, w3) == PackError::None);
    sw.payload.set("at_position", 0).set("target", 77);
    CHECK(!sw.payload.valid());  // x still set while the position branch is absent
    CHECK(pack_event(s2, sw, w3) == PackError::InvalidPayload);
    sw.payload.set("x", 0);
    CHECK(pack_event(s2, sw, w3) == PackError::None);
    CHECK(s1.bit_size() == 35 + 32 + 3 + 1 + 54 && s2.bit_size() == 35 + 32 + 3 + 1 + 32);
    BitReader sr(s2.bytes());
    Command sb;
    CHECK(unpack_event(sr, sb, w3) == DecodeError::None && sb.payload == sw.payload);    // Range checks.
    Command big = stop;
    big.header.player = 8;  // needs 4 bits
    BitWriter x;
    CHECK(pack_event(x, big, w3) == PackError::PlayerOutOfRange);
    big = stop;
    big.header.scheduled_tick = 1ull << 26;
    CHECK(pack_event(x, big, w3) == PackError::FrameOutOfRange);
}

void test_wire_packets() {
    const WireConfig cfg;  // P = 4
    Rng r(11);
    std::vector<Command> evs;
    for (int i = 0; i < 200; ++i) {
        Command c;
        c.header.player = static_cast<uint8_t>(r.below(16));
        c.header.scheduled_tick = r.below(1u << 26);
        c.header.source = CommandSource::Network;
        c.payload = random_any(r);
        evs.push_back(std::move(c));
    }
    std::vector<std::vector<uint8_t>> pk;
    CHECK(pack_packets(evs, cfg, pk) == PackError::None);
    std::vector<Command> back;
    for (const auto& p : pk) {
        CHECK(p.size() <= cfg.max_packet_bytes);
        CHECK((p[0] & 0x1F) >= 1);
        CHECK(unpack_packet(p, cfg, back) == DecodeError::None);
    }
    CHECK(back == evs);  // source Network, sequence/issue 0 on both sides

    // 40 small events -> 31 + 9 (5-bit count).
    std::vector<Command> stops(40);
    for (auto& s : stops) s.payload = ev::stop();
    pk.clear();
    CHECK(pack_packets(stops, cfg, pk) == PackError::None);
    CHECK(pk.size() == 2 && (pk[0][0] & 0x1F) == 31 && (pk[1][0] & 0x1F) == 9);
    // Byte limit: 13-byte moves with a 16-byte limit -> one per packet.
    WireConfig small;
    small.max_packet_bytes = 16;
    std::vector<Command> moves(3);
    for (auto& m : moves) m.payload = ev::move_to_position(1, 2, 3);
    pk.clear();
    CHECK(pack_packets(moves, small, pk) == PackError::None && pk.size() == 3);
    // Scheduled and non-scheduled never share a packet.
    std::vector<Command> mixed(2);
    mixed[1].payload = EventData(EventType::Taunt);
    pk.clear();
    CHECK(pack_packets(mixed, cfg, pk) == PackError::InvalidPayload);
    // Decode errors.
    pk.clear();
    pack_packets(moves, cfg, pk);
    std::vector<uint8_t> cut(pk[0].begin(), pk[0].end() - 3);
    back.clear();
    CHECK(unpack_packet(cut, cfg, back) == DecodeError::Truncated && back.empty());
    BitWriter dead;
    dead.put(1, 5);
    dead.put(23, 6);
    CHECK(unpack_packet(dead.bytes(), cfg, back) == DecodeError::UnknownType);
}

// ---- scheduling -------------------------------------------------------------------------------------
std::vector<uint8_t> run_threaded(uint32_t run_seed, uint32_t players, uint32_t ticks, SchedulerConfig cfg) {
    CommandScheduler s(cfg);
    std::vector<uint8_t> out;
    for (uint32_t t = 0; t < ticks; ++t) {
        std::vector<std::thread> th;
        for (uint32_t p = 0; p < players; ++p) {
            th.emplace_back([&s, p, t, run_seed] {
                Rng r(1000u * p + t);
                Rng jitter(run_seed * 77u + p * 13u + t);
                const uint32_t n = r.below(4);
                for (uint32_t k = 0; k < n; ++k) {
                    for (uint32_t y = jitter.below(3); y > 0; --y) std::this_thread::yield();
                    s.submit(static_cast<uint8_t>(p), CommandSource::Player, random_any(r));
                }
            });
        }
        if (run_seed & 1u) std::reverse(th.begin(), th.end());
        for (auto& x : th) x.join();
        const CommandBatch b = s.drain(t);
        for (size_t i = 1; i < b.commands.size(); ++i)
            CHECK(drain_order_less(b.commands[i - 1].header, b.commands[i].header));
        encode_batch(b, out);
    }
    return out;
}

void test_drain_order() {
    for (const SchedulerConfig& cfg : {SchedulerConfig::single_player(), SchedulerConfig::internet()}) {
        const auto ref = run_threaded(0, 8, 40, cfg);
        for (uint32_t seed = 1; seed < 5; ++seed) CHECK(run_threaded(seed, 8, 40, cfg) == ref);
    }
    // Network arrival: players' streams interleaved in any order; each player's own order kept.
    Rng r(9);
    std::vector<std::vector<Command>> per_player(8);
    for (uint8_t p = 0; p < 8; ++p)
        for (uint32_t q = 0; q < 6; ++q) {
            Command c;
            c.header.player = p;
            c.header.scheduled_tick = 1 + q % 3;
            c.payload = random_any(r);
            per_player[p].push_back(c);
        }
    std::vector<uint8_t> first;
    for (int round = 0; round < 5; ++round) {
        CommandScheduler s;
        std::vector<size_t> next(8, 0);
        size_t left = 48;
        while (left) {
            const uint32_t p = r.below(8);
            if (next[p] == per_player[p].size()) continue;
            CHECK(s.receive(per_player[p][next[p]++]).ok());
            --left;
        }
        std::vector<uint8_t> bytes;
        for (uint64_t t = 0; t < 4; ++t) encode_batch(s.drain(t), bytes);
        CHECK(s.pending() == 0);
        if (round == 0)
            first = bytes;
        else
            CHECK(bytes == first);
    }
}

void test_schedule_formula() {
    CHECK(lockstep_execute_frame(10, 1, 2) == 14);  // LAN: exactly +4
    for (uint64_t cur = 100; cur < 140; ++cur) {
        const uint64_t f = lockstep_execute_frame(cur, 5, 10);  // internet: +11..+15 on multiples of 5
        CHECK(f % 5 == 0 && f - cur >= 11 && f - cur <= 15);
        const uint64_t g = lockstep_execute_frame(cur, 2, 4);  // +5..+6 on even frames
        CHECK(g % 2 == 0 && g - cur >= 5 && g - cur <= 6);
    }
    CHECK(lockstep_execute_frame(10, 1, 0) == 12);
    CommandScheduler lan(SchedulerConfig::lan());
    for (uint64_t t = 0; t <= 10; ++t) lan.drain(t);
    CHECK(lan.submit(0, CommandSource::Player, ev::stop()).scheduled_tick == 14);
    lan.set_max_ahead(6);
    CHECK(lan.submit(0, CommandSource::Player, ev::stop()).scheduled_tick == 18);
    // Lockstep re-alignment of received frames: rounded up to a multiple of T.
    CommandScheduler inet(SchedulerConfig::internet());
    Command c;
    c.header.player = 1;
    c.header.scheduled_tick = 23;
    c.payload = ev::stop();
    const auto rr = inet.receive(c);
    CHECK(rr.ok() && rr.scheduled_tick == 25);
}

void test_scheduler_rules() {
    SchedulerConfig cfg;
    cfg.delay = 3;
    cfg.max_players = 4;
    cfg.first_tick = 10;
    CommandScheduler s(cfg);
    auto a = s.submit(1, CommandSource::Player, ev::stop());
    auto b = s.submit(1, CommandSource::Player, ev::stop());
    auto c = s.submit(0, CommandSource::Player, ev::select_all());
    CHECK(a.ok() && a.scheduled_tick == 13 && a.sequence == 0 && b.sequence == 1 && c.sequence == 0);
    CHECK(s.submit(4, CommandSource::Player, ev::stop()).status == RejectReason::InvalidPlayer);
    CHECK(s.submit(0, CommandSource::Player, EventData(EventType::Chat)).status == RejectReason::NotScheduled);
    CHECK(s.submit(0, CommandSource::Player, ev::select(std::vector<ObjectId>(200, 1))).status == RejectReason::Malformed);

    Command n;
    n.header = CommandHeader{2, CommandSource::Network, 5, 10, 12};
    n.payload = ev::move_to_object(42);
    CHECK(s.insert(n).ok());
    CHECK(s.insert(n).status == RejectReason::Duplicate);
    CHECK(s.submit(2, CommandSource::Player, ev::stop()).sequence == 6);

    bool threw = false;
    try {
        s.drain(11);
    } catch (const std::logic_error&) {
        threw = true;
    }
    CHECK(threw);
    for (uint64_t t = 10; t <= 12; ++t) CHECK(s.drain(t).commands.size() == (t == 12 ? 1u : 0u));
    CHECK(s.now() == 12 && s.next_tick() == 13);
    CHECK(s.insert(n).status == RejectReason::Duplicate);
    Command late = n;
    late.header.sequence = 9;
    late.header.scheduled_tick = 12;
    CHECK(s.insert(late).status == RejectReason::Late);
    Command hole = n;
    hole.header.sequence = 2;
    hole.header.scheduled_tick = 14;
    CHECK(s.insert(hole).ok());
    const auto b13 = s.drain(13);
    CHECK(b13.commands.size() == 4);
    if (b13.commands.size() == 4) {
        CHECK(b13.commands[0].header.player == 0 && b13.commands[1].header.player == 1);
        CHECK(b13.commands[1].header.sequence == 0 && b13.commands[2].header.sequence == 1);
        CHECK(b13.commands[3].header.player == 2 && b13.commands[3].header.sequence == 6);
    }
    const auto b14 = s.drain(14);
    CHECK(b14.commands.size() == 1 && b14.commands[0].header.sequence == 2);

    SchedulerConfig bc;
    bc.max_players = 2;
    bc.late = LatePolicy::Bump;
    CommandScheduler bump(bc);
    bump.drain(0);
    bump.drain(1);
    Command l;
    l.header = CommandHeader{0, CommandSource::Network, 0, 0, 1};
    l.payload = ev::stop();
    const auto br = bump.insert(l);
    CHECK(br.ok() && br.scheduled_tick == 2);
    CHECK(bump.drain(2).commands.size() == 1);

    threw = false;
    try {
        SchedulerConfig z;
        z.delay = 0;
        CommandScheduler zz(z);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

// ---- events -----------------------------------------------------------------------------------------
void test_event_bus() {
    EventBus bus;
    std::vector<std::string> log;
    bus.subscribe([&](const Event& e) {
        if (const auto* s = std::get_if<SimEvent>(&e)) {
            log.push_back("A" + std::to_string(s->value));
            if (s->value == 1) bus.post(SimEvent{0, 1, 0, 0, 99});
        }
    });
    const auto idb = bus.subscribe([&](const Event& e) {
        if (const auto* s = std::get_if<SimEvent>(&e)) log.push_back("B" + std::to_string(s->value));
    });
    bus.post(SimEvent{0, 1, 0, 0, 1});
    bus.open_lanes(4);
    std::vector<std::thread> th;
    for (int i = 3; i >= 0; --i)
        th.emplace_back([&bus, i] {
            for (int k = 0; k < 2; ++k) bus.lane(static_cast<size_t>(i)).post(SimEvent{0, 2, 0, 0, 10 * (i + 1) + k});
        });
    for (auto& x : th) x.join();
    bus.merge_lanes();
    CHECK(bus.pending().size() == 9);
    bus.deliver();
    const std::vector<std::string> want = {"A1",  "B1",  "A10", "B10", "A11", "B11", "A20", "B20", "A21",
                                           "B21", "A30", "B30", "A31", "B31", "A40", "B40", "A41", "B41"};
    CHECK(log == want);
    log.clear();
    bus.unsubscribe(idb);
    bus.deliver();
    CHECK(log == std::vector<std::string>{"A99"});
    CHECK(reject_reason_name(RejectReason::NotScheduled) == "not scheduled");
}

// ---- pipeline, direct path, record + replay -----------------------------------------------------------
struct ToySim {
    uint64_t state = 0x1234;
    std::vector<uint32_t> selection[4];
    std::vector<CommandResult> results;
    std::vector<uint64_t> batch_crcs;

    // Original-style execution: ids resolved now; a missing target is a silent no-op (diagnostic only).
    RejectReason apply(uint64_t tick, const Command& c) {
        const EventData& e = c.payload;
        const uint8_t p = c.header.player;
        if (e.type() == EventType::Select) {
            selection[p & 3] = e.ids("ids");
        } else if (e.type() == EventType::Attack && e.u("target") % 7 == 0) {
            return RejectReason::InvalidTarget;  // "target dead": no state change
        } else if (e.type() == EventType::StopMovement) {
            for (uint32_t id : selection[p & 3]) state = fnv1a64(&id, sizeof id, state);  // acts on Sel
        }
        const auto b = encode_command(c);
        state = fnv1a64(b.data(), b.size(), state ^ tick);
        return RejectReason::None;
    }
    void hook(CommandPipeline& p) {
        p.set_handler([this](uint64_t t, const Command& c) { return apply(t, c); });
        p.set_crc_hook([this](const void* d, size_t n) { batch_crcs.push_back(fnv1a64(d, n)); });
        p.events().subscribe([this](const Event& e) {
            if (const auto* r = std::get_if<CommandResult>(&e)) results.push_back(*r);
        });
    }
};

void test_pipeline_replay() {
    SchedulerConfig cfg = SchedulerConfig::internet();
    cfg.max_players = 4;
    cfg.first_tick = 5;
    const uint32_t ticks = 300;
    CommandRecording rec;
    rec.seed = 777;
    rec.meta.emplace_back("map", "test");
    ToySim live;
    size_t direct = 0;
    {
        CommandPipeline p(cfg);
        live.hook(p);
        p.record_to(&rec);
        Rng r(4242);
        for (uint64_t t = cfg.first_tick; t < cfg.first_tick + ticks; ++t) {
            p.begin_tick(t);
            // AI direct path inside the tick: applied now, never queued or recorded.
            if (t % 10 == 0) {
                CHECK(p.execute_direct(t, 3, CommandSource::AI, ev::production(5, 100)) == RejectReason::None);
                ++direct;
            }
            std::vector<std::thread> th;
            for (uint8_t pl = 0; pl < 3; ++pl) {
                const uint64_t s = r.next();
                th.emplace_back([&p, pl, s] {
                    Rng lr(s);
                    for (uint32_t k = lr.below(3); k > 0; --k) {
                        EventData e = lr.below(3) == 0 ? ev::select({lr.bits(8), lr.bits(8)})
                                      : lr.below(2)    ? ev::stop()
                                                       : random_any(lr);
                        p.submit(pl, CommandSource::Player, std::move(e));
                    }
                });
            }
            for (auto& x : th) x.join();
            p.events().deliver();
        }
        CHECK(p.scheduler().pending() > 0);
    }
    CHECK(live.batch_crcs.size() == ticks);
    CHECK(rec.end_tick == cfg.first_tick + ticks);
    CHECK(rec.command_count() + direct == live.results.size());
    for (const auto& b : rec.batches) CHECK(b.tick % 5 == 0);  // internet: turn-aligned frames
    size_t rejected = 0;
    for (const auto& r : live.results) rejected += r.accepted() ? 0 : 1;
    CHECK(rejected > 0);
    std::printf("pipeline: %zu commands in %zu batches, %zu rejected, %zu direct\n", rec.command_count(),
                rec.batches.size(), rejected, direct);

    const std::string path = "alamo_commands_test.acmd";
    std::string err;
    CHECK(rec.save(path, &err));
    CommandRecording loaded;
    CHECK(CommandRecording::load(path, loaded, &err));
    std::remove(path.c_str());
    CHECK(loaded == rec && loaded.to_bytes() == rec.to_bytes());

    ToySim replay;
    CommandPipeline p2(loaded.config);
    replay.hook(p2);
    p2.replay_from(&loaded);
    CHECK(p2.submit(0, CommandSource::Player, ev::stop()).status == RejectReason::NotAllowed);
    CommandRecording rerecord;
    p2.record_to(&rerecord);
    for (uint64_t t = loaded.config.first_tick; t < loaded.end_tick; ++t) {
        p2.begin_tick(t);
        if (t % 10 == 0) p2.execute_direct(t, 3, CommandSource::AI, ev::production(5, 100));  // AI re-runs
        p2.events().deliver();
    }
    CHECK(p2.replay_failures() == 0);
    CHECK(replay.state == live.state);
    CHECK(replay.batch_crcs == live.batch_crcs);
    CHECK(replay.results == live.results);
    CHECK(rerecord.batches == rec.batches);

    // FrameSync retunes MaxAhead at its synced frame.
    CommandPipeline fs(SchedulerConfig::lan());
    EventData sync(EventType::FrameSync);
    sync.set("max_ahead", 6);
    CHECK(fs.submit(0, CommandSource::Player, sync).scheduled_tick == 4);
    for (uint64_t t = 0; t <= 4; ++t) fs.begin_tick(t);
    CHECK(fs.scheduler().config().max_ahead == 6);
    CHECK(fs.submit(0, CommandSource::Player, ev::stop()).scheduled_tick == 4 + 1 + 6 + 1);
    // Non-scheduled events cannot take the direct path either; no handler -> Unhandled.
    CHECK(fs.execute_direct(5, 0, CommandSource::Script, EventData(EventType::Chat)) == RejectReason::NotScheduled);
    CHECK(fs.execute_direct(5, 0, CommandSource::Script, ev::stop()) == RejectReason::Unhandled);
}

// ---- N1: pipeline save / load, replay from a save ------------------------------------------------------
// Orders of tick t, the same for every pipeline: local players (lockstep stamping), a network peer
// (receive keeps the execute frame), a FrameSync that retunes MaxAhead before the save.
void orders_for_tick(CommandPipeline& p, uint64_t t) {
    Rng r(t * 7919u + 13u);
    for (uint8_t pl = 0; pl < 3; ++pl)
        for (uint32_t k = r.below(3); k > 0; --k) {
            EventData e = r.below(3) == 0 ? ev::select({r.bits(8), r.bits(8)}) : r.below(2) ? ev::stop() : random_any(r);
            p.submit(pl, CommandSource::Player, std::move(e));
        }
    if (t % 9 == 0) {
        Command c;
        c.header.player = 3;
        c.header.source = CommandSource::Network;
        c.header.issue_tick = t;
        c.header.scheduled_tick = t + 12;
        c.payload = random_any(r);
        p.receive(std::move(c));
    }
    if (t == 40) {
        EventData sync(EventType::FrameSync);
        sync.set("max_ahead", 6);
        p.submit(0, CommandSource::Player, sync);
    }
}

void sim_event_for_tick(CommandPipeline& p, uint64_t t) {
    if (t % 4 == 0) p.events().post(SimEvent{t, 0x51u, t * 3u, 7u, -static_cast<int64_t>(t)});
}

std::vector<uint8_t> pipeline_bytes(const CommandPipeline& p) {
    ByteWriter w;
    p.write(w);
    return w.bytes();
}

template <class T>
std::vector<T> tail_of(const std::vector<T>& v, size_t from) {
    return std::vector<T>(v.begin() + static_cast<std::ptrdiff_t>(from), v.end());
}

void copy_sim_state(ToySim& to, const ToySim& from) {
    to.state = from.state;
    for (int k = 0; k < 4; ++k) to.selection[k] = from.selection[k];
}

void test_pipeline_save() {
    SchedulerConfig cfg = SchedulerConfig::internet();  // T=5, A=10: orders wait 11..15 ticks in the queue
    cfg.max_players = 4;
    cfg.first_tick = 1;
    const uint64_t save_tick = 120, end_tick = 300;

    // Live run, recorded in full; saved at the end of tick save_tick (before that tick's events are delivered).
    ToySim live;
    std::vector<SimEvent> live_sim;
    CommandPipeline p(cfg);
    live.hook(p);
    p.events().subscribe([&live_sim](const Event& e) {
        if (const auto* s = std::get_if<SimEvent>(&e)) live_sim.push_back(*s);
    });
    CommandRecording full;
    full.seed = 5;
    p.record_to(&full);
    std::vector<uint8_t> saved;
    size_t sched_bytes = 0, queued_at_save = 0, crcs_at_save = 0, results_at_save = 0, sims_at_save = 0;
    ToySim at_save;
    SchedulerConfig cfg_at_save;
    for (uint64_t t = cfg.first_tick; t < end_tick; ++t) {
        p.begin_tick(t);
        orders_for_tick(p, t);
        sim_event_for_tick(p, t);
        if (t == save_tick) {
            saved = pipeline_bytes(p);
            ByteWriter ws;
            p.scheduler().write(ws);
            sched_bytes = ws.size();
            queued_at_save = p.scheduler().pending();
            copy_sim_state(at_save, live);
            crcs_at_save = live.batch_crcs.size();
            results_at_save = live.results.size();
            sims_at_save = live_sim.size();
            cfg_at_save = p.scheduler().config();
            CHECK(queued_at_save > 0 && !p.events().pending().empty());
        }
        p.events().deliver();
    }
    CHECK(full.end_tick == end_tick && full.command_count() > 100);
    std::printf("pipeline save: %zu bytes at tick %llu (%zu queued, digest %016llx)\n", saved.size(),
                static_cast<unsigned long long>(save_tick), queued_at_save,
                static_cast<unsigned long long>(fnv1a64(saved.data(), saved.size())));

    // Load into a pipeline built with another config (read restores it), continue with the same orders:
    // identical drained batches, batch CRCs, results and sim events; save -> load -> save byte-identical.
    const SavePoint sp{save_tick, 0x5EEDF00Dull};
    CommandRecording cont;
    {
        ToySim branch;
        copy_sim_state(branch, at_save);
        std::vector<SimEvent> sims;
        CommandPipeline q(SchedulerConfig::lan());
        branch.hook(q);
        q.events().subscribe([&sims](const Event& e) {
            if (const auto* s = std::get_if<SimEvent>(&e)) sims.push_back(*s);
        });
        ByteReader r(saved);
        CHECK(q.read(r) && r.done());
        CHECK(pipeline_bytes(q) == saved);
        CHECK(q.scheduler().next_tick() == save_tick + 1 && q.scheduler().pending() == queued_at_save);
        const SchedulerConfig& qc = q.scheduler().config();
        CHECK(qc.mode == ScheduleMode::Lockstep && qc.turn == 5 && qc.max_players == 4 && qc.first_tick == 1 &&
              qc.max_ahead == cfg_at_save.max_ahead && qc.delay == cfg_at_save.delay && qc.late == cfg_at_save.late);
        q.record_to(&cont, sp);
        q.events().deliver();  // the save tick's events, as the live run delivered them
        for (uint64_t t = save_tick + 1; t < end_tick; ++t) {
            q.begin_tick(t);
            orders_for_tick(q, t);
            sim_event_for_tick(q, t);
            q.events().deliver();
        }
        CHECK(branch.state == live.state);
        CHECK(branch.batch_crcs == tail_of(live.batch_crcs, crcs_at_save));
        CHECK(branch.results == tail_of(live.results, results_at_save));
        CHECK(sims == tail_of(live_sim, sims_at_save) && !sims.empty());
        CHECK(pipeline_bytes(q) == pipeline_bytes(p));
    }
    // The continuing stream: starts after the save, names it, holds exactly the live run's later batches.
    CHECK(cont.from_save == sp && cont.config.first_tick == save_tick + 1 && cont.end_tick == end_tick);
    {
        std::vector<CommandBatch> later;
        for (const auto& b : full.batches)
            if (b.tick > save_tick) later.push_back(b);
        CHECK(cont.batches == later);
    }
    // Round trip of the stream file; the flag is set only with a save point.
    CommandRecording cont2;
    {
        const auto bytes = cont.to_bytes();
        std::string err;
        CHECK(CommandRecording::from_bytes(bytes, cont2, &err) && cont2 == cont && cont2.to_bytes() == bytes);
        CHECK(bytes[6] == 1 && bytes[7] == 0 && full.to_bytes()[6] == 0);
        std::vector<uint8_t> bad = bytes;
        bad[6] = 3;  // unknown flag bit
        CommandRecording tmp;
        CHECK(!CommandRecording::from_bytes(bad, tmp, &err));
        // Truncated anywhere in the header (incl. the save point) or the stream.
        bool refused = true;
        for (size_t n = 0; n < bytes.size(); n += (n < 128 ? 1 : 97))
            refused = refused && !CommandRecording::from_bytes(std::span<const uint8_t>(bytes).first(n), tmp);
        CHECK(refused);
        const std::string path = "alamo_commands_save_test.acmd";
        CHECK(cont.save(path, &err) && CommandRecording::load(path, tmp, &err) && tmp == cont);
        std::remove(path.c_str());
        CHECK(save_digest(std::span<const uint8_t>(bytes).first(7)) == 0);
        const uint8_t tail8[10] = {0xEE, 0xFF, 1, 2, 3, 4, 5, 6, 7, 8};
        CHECK(save_digest(tail8) == 0x0807060504030201ull);
    }

    // Orders queued at the save and drained before the end: the replay finds them already in the loaded queue.
    size_t preloaded = 0;
    for (const auto& b : full.batches)
        for (const Command& c : b.commands) preloaded += (b.tick > save_tick && c.header.issue_tick <= save_tick) ? 1u : 0u;
    CHECK(preloaded > 0 && preloaded <= queued_at_save);

    // Replay from the save: the continuing stream, and the full stream of the run (earlier batches skipped).
    for (const CommandRecording* stream : {&cont2, &full}) {
        ToySim rs;
        copy_sim_state(rs, at_save);
        CommandPipeline rp;
        rs.hook(rp);
        ByteReader r(saved);
        CHECK(rp.read(r));
        std::string err;
        CHECK(rp.replay_from_save(stream, sp, &err) && rp.replaying());
        CHECK(rp.submit(0, CommandSource::Player, ev::stop()).status == RejectReason::NotAllowed);
        rp.events().deliver();
        for (uint64_t t = save_tick + 1; t < end_tick; ++t) {
            rp.begin_tick(t);
            rp.events().deliver();
        }
        CHECK(rp.replay_failures() == 0 && rp.replay_already_queued() == preloaded);
        CHECK(rs.state == live.state && rs.batch_crcs == tail_of(live.batch_crcs, crcs_at_save));
        CHECK(rs.results == tail_of(live.results, results_at_save));
    }

    // Streams that do not continue the loaded state are refused, nothing attached.
    {
        CommandPipeline rp;
        ByteReader r(saved);
        CHECK(rp.read(r));
        std::string err;
        CHECK(!rp.replay_from_save(&cont2, SavePoint{save_tick, 1}, &err) && !rp.replaying() && !err.empty());
        CHECK(!rp.replay_from_save(&cont2, SavePoint{save_tick + 1, sp.digest}, &err));
        CommandRecording shifted = cont2;
        shifted.config.first_tick += 1;
        CHECK(!rp.replay_from_save(&shifted, sp, &err));
        CommandRecording other_turn = cont2;
        other_turn.config.turn = 1;
        CHECK(!rp.replay_from_save(&other_turn, sp, &err));
        CommandRecording late_full = full;
        late_full.config.first_tick = save_tick + 2;
        CHECK(!rp.replay_from_save(&late_full, sp, &err));
        CHECK(!rp.replay_from_save(nullptr, sp, &err) && !rp.replaying());
        // The loaded drained-sequence state still refuses re-inserting an order applied before the save.
        const CommandBatch* old = nullptr;
        for (const auto& b : full.batches)
            if (b.tick <= save_tick) old = &b;
        CHECK(old && rp.scheduler().insert(old->commands.front()).status == RejectReason::Duplicate);
    }

    // Truncated / invalid data: read() false and the pipeline unchanged.
    {
        CommandPipeline x(SchedulerConfig::lan());
        for (uint64_t t = 0; t < 30; ++t) {
            x.begin_tick(t);
            orders_for_tick(x, t);
        }
        x.events().post(SimEvent{1, 2, 3, 4, 5});
        const auto before = pipeline_bytes(x);
        bool refused = true;
        for (size_t n = 0; n < saved.size(); ++n) {
            ByteReader r(std::span<const uint8_t>(saved).first(n));
            refused = refused && !x.read(r);
        }
        CHECK(refused && pipeline_bytes(x) == before);
        auto refuses = [&](size_t off, uint8_t v) {
            std::vector<uint8_t> bad = saved;
            bad[off] = v;
            ByteReader r(bad);
            return !x.read(r) && pipeline_bytes(x) == before;
        };
        // pipeline version | scheduler version | delay | max_players (0, 17) | late | mode
        CHECK(refuses(0, 9) && refuses(4, 9) && refuses(8, 0) && refuses(12, 0) && refuses(12, 17));
        CHECK(refuses(13, 2) && refuses(14, 2));
        // A bad event kind after a valid scheduler part: the scheduler is not loaded either.
        const size_t first_kind = 4 + sched_bytes + 8;
        CHECK((saved[first_kind] == 0 || saved[first_kind] == 1) && refuses(first_kind, 7));
    }
}

// Existing .acmd streams (headless / sim goldens) still load and re-encode byte-identically.
void test_old_streams() {
#ifdef ALAMO_COMMANDS_APPS_DIR
    const char* files[] = {"/headless/testdata/replay_stream.acmd", "/sim/testdata/synthetic.acmd",
                           "/sim/testdata/_LAND_PLANET_BYSS_KIDNAPPING.acmd", "/sim/testdata/_SPACE_PLANET_CORELLIA_PIRACY.acmd"};
    size_t checked = 0;
    for (const char* f : files) {
        const std::string path = std::string(ALAMO_COMMANDS_APPS_DIR) + f;
        std::FILE* fp = std::fopen(path.c_str(), "rb");
        if (!fp) {
            std::printf("old streams: %s missing, skipped\n", path.c_str());
            continue;
        }
        std::vector<uint8_t> bytes;
        uint8_t buf[4096];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, fp)) > 0;) bytes.insert(bytes.end(), buf, buf + n);
        std::fclose(fp);
        CommandRecording rec;
        std::string err;
        CHECK(CommandRecording::from_bytes(bytes, rec, &err));
        CHECK(!rec.from_save && rec.to_bytes() == bytes);
        ++checked;
    }
    std::printf("old streams: %zu .acmd file(s) load and re-encode identically\n", checked);
#endif
}

}  // namespace

int main() {
    test_catalogue();
    test_quantizers();
    test_round_trip();
    test_golden_canonical();
    test_decode_errors();
    test_queued_flag();
    test_wire_golden();
    test_wire_packets();
    test_drain_order();
    test_schedule_formula();
    test_scheduler_rules();
    test_event_bus();
    test_pipeline_replay();
    test_pipeline_save();
    test_old_streams();
    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("alamo_commands_tests: all passed\n");
    return 0;
}
