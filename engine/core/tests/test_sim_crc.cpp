// Q1 harness unit tests: StateHasher, CrcRegistry, CrcLog + compare, ByteWriter/Reader, SimRng.
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __builtin_FILE(), __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

template <class F>
static bool throws(F&& f) {
    try {
        f();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

static void test_hasher() {
    // The digest is a function of the byte stream only, not of how it was fed.
    std::vector<uint8_t> data(1000);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i * 7 + 3);
    alamo::StateHasher a;
    a.bytes(data.data(), data.size());
    for (size_t step : {1u, 3u, 7u, 8u, 13u, 64u}) {
        alamo::StateHasher b;
        for (size_t i = 0; i < data.size(); i += step) b.bytes(data.data() + i, std::min(step, data.size() - i));
        CHECK(a.digest() == b.digest());
    }
    // Typed feeds are little-endian byte streams.
    alamo::StateHasher t1, t2;
    t1.u32(0x04030201u);
    t1.f32(1.0f);
    t1.u16(0xBEEF);
    const uint8_t raw[] = {1, 2, 3, 4, 0x00, 0x00, 0x80, 0x3F, 0xEF, 0xBE};
    t2.bytes(raw, sizeof raw);
    CHECK(t1.digest() == t2.digest());
    // OPT3 t05: the inline typed feeds (pending-word fast path) equal the byte stream fed one byte at a time, for every
    // alignment of every width, mixed with bulk bytes() calls.
    {
        alamo::StateHasher typed, bytewise;
        std::vector<uint8_t> blob(40);
        for (size_t i = 0; i < blob.size(); ++i) blob[i] = static_cast<uint8_t>(0xA5u ^ (i * 29u));
        uint64_t x = 0x9E3779B97F4A7C15ull;
        auto feed_bytewise = [&](uint64_t v, uint32_t n) {
            for (uint32_t i = 0; i < n; ++i) {
                const auto b = static_cast<uint8_t>((v >> (8 * i)) & 0xFFu);
                bytewise.bytes(&b, 1);
            }
        };
        bool same = true;
        for (uint32_t op = 0; op < 4000; ++op) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            switch (x % 6) {
            case 0: typed.u8(static_cast<uint8_t>(x >> 8)); feed_bytewise(static_cast<uint8_t>(x >> 8), 1); break;
            case 1: typed.u16(static_cast<uint16_t>(x >> 8)); feed_bytewise(static_cast<uint16_t>(x >> 8), 2); break;
            case 2: typed.u32(static_cast<uint32_t>(x >> 8)); feed_bytewise(static_cast<uint32_t>(x >> 8), 4); break;
            case 3: typed.u64(x * 3u); feed_bytewise(x * 3u, 8); break;
            case 4: typed.f32(static_cast<float>(x % 1000u) * 0.5f); feed_bytewise(std::bit_cast<uint32_t>(static_cast<float>(x % 1000u) * 0.5f), 4); break;
            default: {
                const size_t n = static_cast<size_t>((x >> 20) % blob.size());
                typed.bytes(blob.data(), n);
                for (size_t i = 0; i < n; ++i) bytewise.bytes(&blob[i], 1);
            }
            }
            if (typed.digest() != bytewise.digest() || typed.length() != bytewise.length()) same = false;
        }
        CHECK(same);
    }
    // Spans of arrays equal element-wise feeds.
    const std::vector<float> fv = {1.5f, -0.0f, 3.25f};
    alamo::StateHasher s1, s2;
    s1.span(fv);
    for (float f : fv) s2.f32(f);
    CHECK(s1.digest() == s2.digest());
    // Sensitivity: -0 vs +0, length, seed, digest() non-destructive.
    alamo::StateHasher z1, z2;
    z1.f32(0.0f);
    z2.f32(-0.0f);
    CHECK(z1.digest() != z2.digest());
    alamo::StateHasher e1, e2;
    e2.u8(0);
    CHECK(e1.digest() != e2.digest());
    CHECK(alamo::StateHasher(1).digest() != alamo::StateHasher(2).digest());
    const uint64_t d = a.digest();
    CHECK(a.digest() == d);
    // Algorithm pinned (golden): changing it invalidates every golden CRC file.
    alamo::StateHasher g;
    g.str("alamo");
    g.u64(0x0123456789ABCDEFull);
    g.f64(3.5);
    std::printf("  StateHasher pin digest %016llx\n", static_cast<unsigned long long>(g.digest()));
    CHECK(g.digest() == 0x5e4582e2c39f7cb0ull);  // pinned (MSVC; clang/gcc must match)
}

static void test_registry_and_log() {
    alamo::CrcRegistry reg;
    uint32_t a = 1;
    float b = 2.0f;
    reg.add("sys.a", [&](alamo::StateHasher& h) { h.u32(a); });
    reg.add("sys.b", [&](alamo::StateHasher& h) { h.f32(b); });
    CHECK(throws([&] { reg.add("sys.a", [](alamo::StateHasher&) {}); }));
    CHECK(throws([&] { reg.add("bad name", [](alamo::StateHasher&) {}); }));
    CHECK(throws([&] { reg.add("", [](alamo::StateHasher&) {}); }));
    CHECK(reg.size() == 2 && reg.name(1) == "sys.b");

    // Parallel capture == serial capture.
    const alamo::ParallelFor threaded = [](uint32_t count, uint32_t, const alamo::RangeFn& fn) {
        std::vector<std::thread> ts;
        for (uint32_t i = 0; i < count; ++i) ts.emplace_back([&fn, i] { fn(i, i + 1); });
        for (auto& t : ts) t.join();
    };
    alamo::CrcLog log, log2;
    log.systems = log2.systems = reg.names();
    for (uint64_t tick = 0; tick < 5; ++tick) {
        a += 1;
        if (tick == 3) b = 2.5f;
        const auto r1 = reg.capture(tick);
        const auto r2 = reg.capture(tick, threaded);
        CHECK(r1 == r2);
        CHECK(r1.combined == alamo::combine_crcs(log.systems, r1.systems));
        log.records.push_back(r1);
        log2.records.push_back(r2);
    }
    log.comments.push_back("seed 7");
    CHECK(!alamo::first_divergence(log, log2).diverged);

    // Text round trip (comments kept, CRLF tolerated).
    const std::string text = log.to_text();
    alamo::CrcLog back;
    std::string err;
    CHECK(alamo::CrcLog::parse(text, back, &err));
    CHECK(back.systems == log.systems && back.records == log.records && back.comments == log.comments);
    std::string crlf;
    for (char c : text) {
        if (c == '\n') crlf += '\r';
        crlf += c;
    }
    CHECK(alamo::CrcLog::parse(crlf, back, &err) && back.records == log.records);
    CHECK(!alamo::CrcLog::parse("ALAMO_CRC 1\nsystems 1 x\nparity 0\n0 00 00\n", back, &err));  // column count
    CHECK(!alamo::CrcLog::parse("ALAMO_CRC 1\nsystems 1 x\nparity 0\n5 1 0 1\n4 1 0 1\n", back, &err));  // tick order

    // Divergence: first tick + system.
    alamo::CrcLog bad = log;
    bad.records[2].systems[1] ^= 1;
    bad.records[2].combined ^= 1;
    bad.records[4].systems[0] ^= 1;
    auto dv = alamo::first_divergence(log, bad);
    CHECK(dv.diverged && dv.tick == 2 && dv.system == "sys.b");
    alamo::CrcLog shorter = log;
    shorter.records.pop_back();
    dv = alamo::first_divergence(log, shorter);
    CHECK(dv.diverged && dv.tick == 4 && dv.system.empty());
    alamo::CrcLog renamed = log;
    renamed.systems[0] = "sys.z";
    CHECK(alamo::first_divergence(log, renamed).diverged);
}

static void test_bytes() {
    alamo::ByteWriter w;
    w.u8(2).u32(0xDEADBEEFu).f64(-1.5).f32(0.25f).u16(7).u64(1ull << 40).i32(-3);
    CHECK(w.bytes().size() == 1 + 4 + 8 + 4 + 2 + 8 + 4);
    CHECK(w.bytes()[1] == 0xEF && w.bytes()[4] == 0xDE);  // little-endian
    alamo::ByteReader r(w.bytes());
    CHECK(r.u8() == 2 && r.u32() == 0xDEADBEEFu && r.f64() == -1.5 && r.f32() == 0.25f && r.u16() == 7 &&
          r.u64() == (1ull << 40) && r.i32() == -3 && r.ok() && r.done());
    CHECK(r.u8() == 0 && !r.ok());
}

// Golden sequences: computed independently from the MAIN_LOOP.md §4.1 spec (Python model, float32
// rounding emulated), so they pin the algorithm, not just this implementation.
static void test_rng() {
    alamo::SimRng a(12345);
    const uint32_t r15[] = {0x76fc, 0x1147, 0x3141, 0x532c, 0x668a, 0x531d, 0x79bc, 0x6dfa};
    for (uint32_t e : r15) CHECK(a.next15() == e);
    CHECK(a.state() == 0xe1b7eb81u && a.draws() == 8);

    alamo::SimRng d6(1);
    const int32_t dice[] = {2, 4, 4, 2, 6, 4, 2, 1, 5, 4, 1, 1};
    for (int32_t e : dice) CHECK(d6.range_int(1, 6) == e);
    alamo::SimRng wide(2026);
    const int32_t wides[] = {-976915, -967805, -992096, -976395};  // range capped at 0x7FFE
    for (int32_t e : wides) CHECK(wide.range_int(-1000000, 1000000) == e);
    alamo::SimRng sw(1);
    CHECK(sw.range_int(6, 1) == 2);  // swapped bounds
    const alamo::SimRng before_eq = sw;
    CHECK(sw.range_int(9, 9) == 9 && sw == before_eq);  // equal bounds: no draw

    alamo::SimRng pc(7);
    const int pct[] = {1, 0, 1, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 0, 0, 0};
    for (int e : pct) CHECK(static_cast<int>(pc.percent(30)) == e);
    CHECK(pc.state() == 0xdc78978cu);
    const alamo::SimRng before_pc = pc;
    CHECK(!pc.percent(0) && pc.percent(100) && pc == before_pc);  // no draw for 0 / 100

    alamo::SimRng fl(99);
    const uint32_t fbits[] = {0x3f306360u, 0x3f7ec3feu, 0xbf7bf7f8u, 0xbeeb45d6u};
    for (uint32_t e : fbits) CHECK(std::bit_cast<uint32_t>(fl.range_float(-1.0f, 1.0f)) == e);

    // GameRandom: Lua Free_Random no-arg form draws int(0, 0xFFFF) from the SYNCED state (SP and MP).
    alamo::GameRandom g(5, 77);
    const int32_t fr[] = {14309, 4739, 30255, 2541};
    for (int32_t e : fr) CHECK(g.lua_free_random(false) == e);
    const uint32_t free_before = g.free().state();
    const uint32_t synced_before = g.synced().state();
    g.lua_free_random(false, 1, 10);  // SP ranged form: free state
    CHECK(g.free().state() != free_before && g.synced().state() == synced_before);
    g.lua_free_random(true, 1, 10);  // MP: synced
    CHECK(g.synced().state() != synced_before);
    // Session seeding: free stream untouched; CRC covers the synced stream only.
    alamo::GameRandom g2 = g;
    g2.free().next15();
    alamo::StateHasher h1, h2;
    g.hash(h1);
    g2.hash(h2);
    CHECK(h1.digest() == h2.digest());
    g2.seed_session(42);
    CHECK(g2.synced().state() == 42 && g2.synced().draws() == 0);

    // Inline copies (count as draws) and the preview save/restore guard.
    alamo::SimRng in(12345);
    const bool coin = in.inline_coin();
    CHECK(coin == ((0x76fcu & 1u) != 0) && in.draws() == 1);
    CHECK(in.inline_unit_float() == static_cast<float>(0x1147) / 32767.0f);
    {
        const alamo::SimRng before_preview = in;
        {
            alamo::SimRngRestore guard(in);
            in.range_int(0, 100);
            in.percent(50);
            CHECK(!(in == before_preview));
        }
        CHECK(in == before_preview);
    }

    // Save/load round trip.
    alamo::ByteWriter w;
    g.write(w);
    alamo::GameRandom loaded;
    alamo::ByteReader r(w.bytes());
    CHECK(loaded.read(r) && loaded == g && r.done());
    alamo::ByteReader shortr(std::span<const uint8_t>(w.bytes().data(), 6));
    CHECK(!loaded.read(shortr) && loaded == g);
}

static void test_parity() {
    // Order-independent: same per-object CRCs in any order give the same sum; dead objects skipped
    // by the caller simply do not contribute.
    const float pos[3][3] = {{1.0f, 2.0f, 3.0f}, {-5.5f, 0.0f, 7.25f}, {100.0f, 200.0f, -0.0f}};
    const float fac[3] = {0.0f, 1.0f, 0.0f};
    alamo::CrcSum s1, s2;
    for (int i = 0; i < 3; ++i) s1.add(alamo::parity_object_crc(pos[i], fac, 10.0f * static_cast<float>(i)));
    for (int i = 2; i >= 0; --i) s2.add(alamo::parity_object_crc(pos[i], fac, 10.0f * static_cast<float>(i)));
    CHECK(s1.value() == s2.value());
    // Health 0 is not hashed (CRC of the 4 xor bytes only).
    const uint32_t x = std::bit_cast<uint32_t>(1.0f) ^ std::bit_cast<uint32_t>(2.0f) ^ std::bit_cast<uint32_t>(3.0f) ^
                       std::bit_cast<uint32_t>(1.0f);
    const uint8_t xb[4] = {static_cast<uint8_t>(x), static_cast<uint8_t>(x >> 8), static_cast<uint8_t>(x >> 16),
                           static_cast<uint8_t>(x >> 24)};
    CHECK(alamo::parity_object_crc(pos[0], fac, 0.0f) == alamo::crc32_raw(xb, 4));
    // crc32_raw = the standard table CRC without the init/final inversion.
    CHECK((alamo::crc32_raw("123456789", 9, 0xFFFFFFFFu) ^ 0xFFFFFFFFu) == 0xCBF43926u);
    CHECK(alamo::crc32_raw(xb, 0) == 0u);
    // Frame finish (COMMANDS.md §7.1 steps 5-7).
    alamo::ParityFrameOptions po;
    CHECK(alamo::parity_finish(5, 0x12345678u, po) == ~0x12345678u);  // invert only
    po.scoring = [](uint32_t seed) { return seed * 3u + 1u; };
    CHECK(alamo::parity_finish(64, 10u, po) == ~31u);   // 64 & 0x31 == 0 -> scoring folded
    CHECK(alamo::parity_finish(65, 10u, po) == ~10u);   // 65 & 0x31 != 0 -> not
    po.invert = false;
    po.add_one = true;
    CHECK(alamo::parity_finish(65, 10u, po) == 11u);
    po.has_mode = true;
    po.mode_byte = 2;
    const unsigned char tail[4] = {2, 0, 0, 0};
    CHECK(alamo::parity_finish(65, 10u, po) == alamo::crc32_raw(tail, 4, 11u));
    CHECK(alamo::parity_wire_crc(0xABCD1234u) == 0x1234u);
    CHECK(alamo::parity_object_crc(pos[0], fac, 5.0f) != alamo::parity_object_crc(pos[0], fac, 0.0f));

    // Registry parity parts: summed into the record, carried through the log, compared.
    alamo::CrcRegistry reg;
    uint32_t objects_sum = 0;
    alamo::SimRng rng(9);
    reg.add("objects.ordered", [&](alamo::StateHasher& h) { h.u32(objects_sum); });
    reg.add_parity("objects", [&] { return objects_sum; });
    reg.add_parity("rng", [&] { return alamo::parity_rng_crc(rng.state()); });
    CHECK(throws([&] { reg.add_parity("rng", [] { return 0u; }); }));
    objects_sum = s1.value();
    const auto rec = reg.capture(3);
    CHECK(rec.parity == ~static_cast<uint32_t>(s1.value() + alamo::parity_rng_crc(9)));
    alamo::CrcLog log;
    log.systems = reg.names();
    log.parity_parts = reg.parity_names();
    log.records.push_back(rec);
    alamo::CrcLog back;
    CHECK(alamo::CrcLog::parse(log.to_text(), back) && back.records == log.records && back.parity_parts.size() == 2);
    alamo::CrcLog other = log;
    other.records[0].parity ^= 4;
    const auto d = alamo::first_divergence(log, other);
    CHECK(d.diverged && d.system == "parity" && d.tick == 3);
}

int main() {
    test_hasher();
    test_registry_and_log();
    test_bytes();
    test_rng();
    test_parity();
    std::printf(g_fail ? "alamo_core_sim_tests: %d FAILED\n" : "alamo_core_sim_tests: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
