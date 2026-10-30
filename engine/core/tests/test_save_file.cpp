// N1 save container tests: ByteWriter/Reader save helpers, SaveFile encode / decode / corruption checks, the LZ save
// codec (round trips, malformed streams, a cross-compiler output digest).
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/core/compress.hpp"
#include "alamo/core/save_file.hpp"
#include "alamo/core/sim_crc.hpp"

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

static void test_byte_helpers() {
    alamo::ByteWriter w;
    w.str("plot").boolean(true).boolean(false).i64(-5).u8(7);
    alamo::ByteReader r(w.bytes());
    CHECK(r.str() == "plot");
    CHECK(r.boolean() && !r.boolean());
    CHECK(r.i64() == -5);
    CHECK(r.remaining() == 1);
    const auto tail = r.raw(1);
    CHECK(tail.size() == 1 && tail[0] == 7);
    CHECK(r.ok() && r.done());

    // A bool byte other than 0/1 and a string length past the end are rejected.
    const std::vector<uint8_t> bad_bool{2};
    alamo::ByteReader rb(bad_bool);
    rb.boolean();
    CHECK(!rb.ok());
    alamo::ByteWriter ws;
    ws.u32(100).u8('x');
    alamo::ByteReader rs(ws.bytes());
    CHECK(rs.str().empty() && !rs.ok());
}

static void test_save_file() {
    alamo::SaveFile f;
    f.tick = 12345;
    f.description = "Kuat, day 12";
    alamo::ByteWriter a;
    a.u32(1).f32(2.5f);
    f.add("galactic.state", 3, a);
    f.add("story", 1, std::vector<uint8_t>{});
    bool threw = false;
    try {
        f.add("story", 1, std::vector<uint8_t>{});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    const std::vector<uint8_t> bytes = f.encode();
    alamo::SaveFile back;
    std::string err;
    CHECK(alamo::SaveFile::decode(bytes, back, &err));
    CHECK(back.tick == 12345 && back.description == "Kuat, day 12");
    CHECK(back.chunks().size() == 2);
    const alamo::SaveChunk* g = back.find("galactic.state");
    CHECK(g && g->version == 3 && g->data == a.bytes());
    CHECK(back.find("story") && back.find("story")->data.empty());
    CHECK(!back.find("ai.state"));

    // Every single-byte corruption and every truncation is caught; `out` stays unchanged.
    for (size_t i = 0; i < bytes.size(); ++i) {
        std::vector<uint8_t> c = bytes;
        c[i] ^= 0x40;
        alamo::SaveFile o;
        o.tick = 99;
        CHECK(!alamo::SaveFile::decode(c, o, &err));
        CHECK(o.tick == 99);
    }
    for (size_t n = 0; n < bytes.size(); ++n) {
        alamo::SaveFile o;
        CHECK(!alamo::SaveFile::decode(std::span<const uint8_t>(bytes.data(), n), o));
    }
    // Re-encoding is byte-identical.
    CHECK(back.encode() == bytes);
}

// ---------------------------------------------------------------- LZ codec

static uint64_t digest(const std::vector<uint8_t>& b) {
    alamo::StateHasher h;
    h.bytes(b.data(), b.size());
    return h.digest();
}

struct XorShift {
    uint64_t s;
    uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
};

// Save-like data: `copies` variants of one `unit`-byte record (a quarter of it noise, the rest small structured
// values) with a few differing bytes each, separated by fresh noise - repeats far apart, like a save's Lua VM images.
static std::vector<uint8_t> save_like(size_t unit, int copies, uint64_t seed) {
    XorShift rng{seed};
    std::vector<uint8_t> base(unit);
    for (size_t i = 0; i < unit; ++i)
        base[i] = (i % 4 == 0) ? static_cast<uint8_t>(rng.next()) : static_cast<uint8_t>((i / 16) % 7);
    std::vector<uint8_t> out;
    for (int c = 0; c < copies; ++c) {
        std::vector<uint8_t> v = base;
        for (int k = 0; k < 40; ++k) v[rng.next() % unit] = static_cast<uint8_t>(rng.next());
        out.insert(out.end(), v.begin(), v.end());
        for (int k = 0; k < 300; ++k) out.push_back(static_cast<uint8_t>(rng.next()));
    }
    return out;
}

static bool round_trip(const std::vector<uint8_t>& in, size_t* packed = nullptr) {
    const std::vector<uint8_t> c = alamo::lz_compress(in);
    if (packed) *packed = c.size();
    std::vector<uint8_t> back;
    if (!alamo::lz_decompress(c, in.size(), back) || back != in) return false;
    // The same input packs to the same bytes; a wrong expected size is refused.
    if (alamo::lz_compress(in) != c) return false;
    if (alamo::lz_decompress(c, in.size() + 1, back)) return false;
    return in.empty() || !alamo::lz_decompress(c, in.size() - 1, back);
}

static void test_lz_codec() {
    XorShift rng{0x9E3779B97F4A7C15ull};
    CHECK(round_trip({}));
    CHECK(round_trip({42}));
    CHECK(round_trip({1, 2, 3, 1, 2, 3, 1, 2, 3, 1, 2, 3}));
    std::vector<uint8_t> noise(70000);
    for (uint8_t& b : noise) b = static_cast<uint8_t>(rng.next());
    CHECK(round_trip(noise));
    // Runs longer than the longest match (1 MiB + 2), overlapping copies at distance 1.
    const std::vector<uint8_t> zeros(3u << 20, 0);
    size_t packed = 0;
    CHECK(round_trip(zeros, &packed));
    CHECK(packed < 1000);
    std::vector<uint8_t> text;
    for (int i = 0; i < 4000; ++i) {
        const std::string line = "line " + std::to_string(i % 97) + " of the plan " + std::to_string(i % 13) + "\n";
        text.insert(text.end(), line.begin(), line.end());
    }
    CHECK(round_trip(text, &packed));
    CHECK(packed * 8 < text.size());
    // Repeats 100 KB apart (beyond a 64 KB window) must still be found.
    const std::vector<uint8_t> structured = save_like(100000, 30, 7);
    CHECK(round_trip(structured, &packed));
    std::printf("lz: save-like %zu -> %zu bytes (%.1fx)\n", structured.size(), packed,
                static_cast<double>(structured.size()) / static_cast<double>(packed));
    CHECK(packed * 10 < structured.size());

    // Cross-compiler determinism: the MSVC and clang builds must write exactly these bytes.
    const std::vector<uint8_t> golden_in = save_like(20000, 12, 99);
    const std::vector<uint8_t> golden = alamo::lz_compress(golden_in);
    std::printf("lz: golden input %zu -> %zu bytes, digest %016llx\n", golden_in.size(), golden.size(),
                static_cast<unsigned long long>(digest(golden)));
    CHECK(golden.size() == 14394 && digest(golden) == 0x9cc7ec0f7906227aull);

    // Malformed streams: every truncation and trailing data are refused; damaged bytes never crash the decoder.
    const std::vector<uint8_t> raw = save_like(3000, 3, 5);
    const std::vector<uint8_t> small = alamo::lz_compress(raw);
    std::vector<uint8_t> back;
    for (size_t n = 0; n < small.size(); ++n)
        CHECK(!alamo::lz_decompress(std::span<const uint8_t>(small.data(), n), raw.size(), back));
    std::vector<uint8_t> longer = small;
    longer.push_back(0);
    CHECK(!alamo::lz_decompress(longer, raw.size(), back));
    int accepted = 0;
    for (size_t i = 0; i < small.size(); ++i)
        for (const uint8_t flip : {uint8_t{0x01}, uint8_t{0x10}, uint8_t{0x80}}) {
            std::vector<uint8_t> c = small;
            c[i] = static_cast<uint8_t>(c[i] ^ flip);
            if (alamo::lz_decompress(c, raw.size(), back)) ++accepted;  // possible: the save file's digest refuses it
        }
    std::printf("lz: %d of %zu damaged streams still decode\n", accepted, small.size() * 3);
    for (int k = 0; k < 2000; ++k) {  // random garbage
        std::vector<uint8_t> g(1 + rng.next() % 200);
        for (uint8_t& b : g) b = static_cast<uint8_t>(rng.next());
        alamo::lz_decompress(g, static_cast<size_t>(rng.next() % 5000), back);
    }
}

// Bigger than the 16 MiB window: the match finder's ring of chain links wraps.
static void test_lz_window() {
    const std::vector<uint8_t> big = save_like(1u << 20, 17, 3);
    const auto t0 = std::chrono::steady_clock::now();
    size_t packed = 0;
    CHECK(round_trip(big, &packed));
    std::printf("lz: %zu -> %zu bytes (%.1fx), 2 packs + 3 unpacks in %.0f ms\n", big.size(), packed,
                static_cast<double>(big.size()) / static_cast<double>(packed),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    CHECK(packed * 10 < big.size());
}

// Format 2 container: packed when that is smaller, stored otherwise; format 1 files are refused.
static void test_save_file_packing() {
    alamo::SaveFile f;
    f.tick = 7;
    f.description = "packed";
    f.add("ai", 2, save_like(50000, 20, 11));
    f.add("objects", 1, std::vector<uint8_t>{1, 2, 3});
    const std::vector<uint8_t> bytes = f.encode();
    const size_t encoding_at = 4 + 4 + 8 + 4 + f.description.size();
    CHECK(bytes.size() > encoding_at && bytes[encoding_at] == 1);  // LZ
    CHECK(bytes.size() * 10 < f.find("ai")->data.size());
    alamo::SaveFile back;
    std::string err;
    CHECK(alamo::SaveFile::decode(bytes, back, &err));
    CHECK(back.tick == 7 && back.chunks().size() == 2 && back.find("ai")->data == f.find("ai")->data &&
          back.find("ai")->version == 2);
    CHECK(back.encode() == bytes);
    for (size_t i = 0; i < bytes.size(); i += 7) {
        std::vector<uint8_t> c = bytes;
        c[i] ^= 0x04;
        alamo::SaveFile o;
        CHECK(!alamo::SaveFile::decode(c, o));
    }
    for (size_t n = 0; n < bytes.size(); n += 5) CHECK(!alamo::SaveFile::decode(std::span<const uint8_t>(bytes.data(), n), back));

    // A tiny save is stored as it is (packing would not shrink it).
    alamo::SaveFile t;
    t.add("rng", 1, std::vector<uint8_t>{9, 8, 7});
    const std::vector<uint8_t> tb = t.encode();
    CHECK(tb[4 + 4 + 8 + 4] == 0);
    CHECK(alamo::SaveFile::decode(tb, back, &err) && back.find("rng")->data == std::vector<uint8_t>({9, 8, 7}));

    // Format 1 (the body unpacked after the description), with a valid digest: refused by its version.
    alamo::ByteWriter v1;
    v1.u32(alamo::kSaveMagic).u32(1).u64(5).str("old").u32(1).str("rng").u32(1).u32(1).u8(3);
    std::vector<uint8_t> old = v1.bytes();
    alamo::ByteWriter d;
    d.u64(digest(old));
    old.insert(old.end(), d.bytes().begin(), d.bytes().end());
    CHECK(!alamo::SaveFile::decode(old, back, &err) && err == "unsupported save format version 1");
}

int main() {
    test_byte_helpers();
    test_save_file();
    test_lz_codec();
    test_lz_window();
    test_save_file_packing();
    if (g_fail) {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("alamo_core_save_tests: OK\n");
    return 0;
}
