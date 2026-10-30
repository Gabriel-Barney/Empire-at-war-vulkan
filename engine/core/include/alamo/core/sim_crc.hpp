// Per-tick simulation CRC harness (Q1).
//
// Every sim system registers a named state hasher with a CrcRegistry. After each tick the host
// calls capture(tick) and gets a CrcRecord: one 64-bit digest per system (registration order)
// plus a combined digest. A CrcLog is the sequence of records of a run; two logs are compared
// with first_divergence(), which names the first diverging tick and system. The same harness
// backs worker-count checks (0/1/4/N), cross-compiler checks (MSVC/clang/gcc golden files),
// replays, saves and parity runs.
//
// HASHER CONTRACT (what a system feeds its StateHasher):
//   * Only state that game logic can read on a later tick (positions, health, owners, timers,
//     queues, RNG state, published results). Never caches that are rebuilt from other state,
//     timings, counters of host frames, or anything a worker-count change may alter.
//   * Canonical order: entities by stable index/id ascending, containers in their deterministic
//     order. Never iterate pointer-keyed or seeded-hash containers (unordered_map with pointer
//     keys, std::hash of strings...) - sort keys first.
//   * Values, not memory: feed fields one by one (u32/f32/...) or spans of plain arithmetic
//     arrays. NEVER pointers/addresses, NEVER structs with padding (raw bytes() of a struct is
//     only allowed for types with std::has_unique_object_representations), never uninitialised
//     bytes, never size_t/long (width differs by platform) - cast to u32/u64.
//   * Floats are hashed by bit pattern (-0.0 != +0.0, NaN payloads count). The sim must produce
//     identical bits, so no tolerance is applied; canonicalise NaN yourself if it is not state.
//   * Variable-length data: feed the count first (u32) so [1,2][3] and [1][2,3] differ.
//   * Hashers run between ticks, read-only. capture() may run them in parallel (one per
//     system), so a hasher must not mutate shared state.
//
// StateHasher algorithm (fixed forever; changing it invalidates golden files): the fed bytes form
// one little-endian byte stream, consumed in 8-byte words w:
//     h = rotl64(h + w * P2, 31) * P1          (h0 = seed + P5)
// then the final partial word (zero-padded, may be empty) is absorbed the same way, h ^= length,
// and the murmur3 fmix64 finaliser is applied. P1/P2/P5 are the xxHash64 primes. It is a
// checksum, not a cryptographic hash.
#pragma once
#include <bit>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "alamo/core/parallel.hpp"

namespace alamo {

class StateHasher {
public:
    explicit StateHasher(uint64_t seed = 0) noexcept;

    // Raw canonical bytes (caller guarantees: no padding, no pointers, defined content).
    void bytes(const void* data, size_t len) noexcept;

    void u8(uint8_t v) noexcept { bytes(&v, 1); }
    void u16(uint16_t v) noexcept { put_le(v); }
    void u32(uint32_t v) noexcept { put_le(v); }
    void u64(uint64_t v) noexcept { put_le(v); }
    void i32(int32_t v) noexcept { put_le(static_cast<uint32_t>(v)); }
    void i64(int64_t v) noexcept { put_le(static_cast<uint64_t>(v)); }
    void boolean(bool v) noexcept { u8(v ? 1 : 0); }
    void f32(float v) noexcept;   // bit pattern
    void f64(double v) noexcept;  // bit pattern
    // Length (u32) then bytes.
    void str(std::string_view s) noexcept;

    // Any fixed-width integer, enum or IEEE float, little-endian, by value.
    template <class T>
    void value(T v) noexcept {
        static_assert(kHashable<T>, "StateHasher::value: integer, enum or float only");
        if constexpr (std::is_enum_v<T>) {
            value(static_cast<std::underlying_type_t<T>>(v));
        } else if constexpr (std::is_same_v<T, bool>) {
            boolean(v);
        } else if constexpr (std::is_same_v<T, float>) {
            f32(v);
        } else if constexpr (std::is_same_v<T, double>) {
            f64(v);
        } else {
            put_le(static_cast<std::make_unsigned_t<T>>(v));
        }
    }
    // Element-wise (count is NOT fed - call u32(size) first when the length can vary).
    template <class T>
    void span(std::span<const T> s) noexcept {
        static_assert(kHashable<T> && !std::is_same_v<T, bool>, "StateHasher::span: integer, enum or float only");
        // All supported targets are little-endian, so the array bytes ARE the canonical stream.
        static_assert(std::endian::native == std::endian::little, "big-endian host: add a byte swap");
        if (!s.empty()) bytes(s.data(), s.size_bytes());
    }
    template <class T>
    void span(const std::vector<T>& v) noexcept {
        span(std::span<const T>(v));
    }

    uint64_t digest() const noexcept;  // does not change the state; more bytes may follow
    uint64_t length() const noexcept { return len_; }

private:
    template <class T>
    static constexpr bool kHashable =
        (std::is_integral_v<T> || std::is_enum_v<T> || std::is_same_v<T, float> || std::is_same_v<T, double>);

    template <class U>
    void put_le(U v) noexcept {
        unsigned char b[sizeof(U)];
        for (size_t i = 0; i < sizeof(U); ++i) b[i] = static_cast<unsigned char>((v >> (8 * i)) & 0xFFu);
        bytes(b, sizeof(U));
    }

    uint64_t h_;
    uint64_t len_ = 0;
    unsigned char tail_[8] = {};
    uint32_t tail_len_ = 0;
};

// ---- Parity CRC: the original's frame CRC (docs/re/COMMANDS.md §7.1, OBJECT_RUNTIME.md §5/§8) --------
// Calculate_Frame_CRC, per tick:
//   1. GOM = wrapping sum of per-object CRCs over the mode's primary store (dead objects skipped):
//      order-independent;  2. RND = crc32_raw(synced RNG state, 4 bytes LE);  3. PLR = player term
//      (not recovered yet: 0);  4. no-scoring CRC = GOM + RND + PLR (wrapping);
//   5. when (frame & 0x31) == 0 the scoring manager's CRC, seeded with that value, replaces it;
//   6. XOR 0xFFFFFFFF unless the cinematic lock / land condition holds; +1 when the FS+0x36 toggle is set;
//   7. with a mode: four CRC-32 byte steps fold in (mode byte, low byte of 0x140a284c4, 0, 0).
//   Only the low 16 bits are sent (parity_wire_crc).
// Parity parts registered with CrcRegistry::add_parity() are the step-4 terms (summed); capture()
// then applies steps 5-7 (parity_finish) with the registry's ParityFrameOptions. The ordered 64-bit
// per-system CRCs stay alongside: they pinpoint WHICH system diverged, parity reproduces retail's number.
//
// CRC-32 as the original computes it here: reflected 0xEDB88320 table, initial value 0, NO final
// inversion (unlike alamo::crc32). crc is the running value (continue = pass the previous result).
uint32_t crc32_raw(const void* data, size_t len, uint32_t crc = 0) noexcept;

struct ParityFrameOptions {
    // Step 5: TheGameScoringManager::Get_CRC(seed) - empty = not modelled yet (value unchanged).
    std::function<uint32_t(uint32_t seed)> scoring;
    bool scoring_always = false;  // sync-debug option: fold scoring every frame
    bool invert = true;           // false when the cinematic lock / land-mode condition holds
    bool add_one = false;         // FS +0x36 test toggle
    bool has_mode = false;        // step 7
    uint8_t mode_byte = 0;        // mode vtable +0x240
    uint8_t land_byte = 0;        // low byte of 0x140a284c4
};
uint32_t parity_finish(uint64_t frame, uint32_t no_scoring_crc, const ParityFrameOptions& opt);
inline uint16_t parity_wire_crc(uint32_t frame_crc) noexcept { return static_cast<uint16_t>(frame_crc & 0xFFFFu); }

class CrcSum {
public:
    void add(uint32_t crc) noexcept { sum_ += crc; }  // wrapping
    uint32_t value() const noexcept { return sum_; }

private:
    uint32_t sum_ = 0;
};
// Per-object CRC (retail 0x4ee890): crc32_raw over the u32 (pos.x ^ pos.y ^ pos.z ^ facing.x ^
// facing.y ^ facing.z) of the raw float bits, then the health float bits when health != 0 (LE bytes,
// one running CRC). The locomotor-data term is debug-only and omitted.
// PROVISIONAL: the raw CRC form (init 0, no inversion, as for the RND term) is assumed for objects too.
uint32_t parity_object_crc(const float pos[3], const float facing[3], float health) noexcept;
// RND term (COMMANDS.md §7.1 step 2): crc32_raw of the 4 state bytes, little-endian.
uint32_t parity_rng_crc(uint32_t rng_state) noexcept;

// One tick's CRCs: systems[i] belongs to the registry's system i; parity = retail-shaped frame CRC.
struct CrcRecord {
    uint64_t tick = 0;
    uint64_t combined = 0;
    uint32_t parity = 0;
    std::vector<uint64_t> systems;
    friend bool operator==(const CrcRecord&, const CrcRecord&) = default;
};

class CrcRegistry {
public:
    using HashFn = std::function<void(StateHasher&)>;
    using SystemId = uint32_t;

    // Names: non-empty, no whitespace, unique (throws std::invalid_argument). Registration order
    // is the record order and part of the combined CRC, so register systems in a fixed order.
    SystemId add(std::string name, HashFn fn);
    // Order-independent parity part (objects store sum, RNG term, players term ...): fn returns its
    // uint32 contribution; capture() sums all parts (wrapping) into CrcRecord::parity. Same name rules.
    using ParityFn = std::function<uint32_t()>;
    void add_parity(std::string name, ParityFn fn);
    std::vector<std::string> parity_names() const;
    // Steps 5-7 applied to the summed parts (default: invert only).
    void set_parity_options(ParityFrameOptions opt) { parity_opt_ = std::move(opt); }

    size_t size() const noexcept { return systems_.size(); }
    const std::string& name(SystemId id) const { return systems_.at(id).name; }
    std::vector<std::string> names() const;

    // Digest of one system (fresh StateHasher, seed 0).
    uint64_t hash_system(SystemId id) const;
    // Runs every hasher (in parallel through `pf` if given; outputs are per index, so the
    // result is identical) and combines them: combined = StateHasher over (str(name), u64(crc))
    // for each system in order.
    CrcRecord capture(uint64_t tick, const ParallelFor& pf = serial_for) const;

private:
    struct System {
        std::string name;
        HashFn fn;
    };
    std::vector<System> systems_;
    struct Parity {
        std::string name;
        ParityFn fn;
    };
    std::vector<Parity> parity_;
    ParityFrameOptions parity_opt_;
};

uint64_t combine_crcs(std::span<const std::string> names, std::span<const uint64_t> crcs);

// A run's CRC sequence. Text file format (LF line ends, written byte-identically by every build):
//     ALAMO_CRC 1
//     systems <count> <name0> <name1> ...
//     parity <count> <part0> <part1> ...
//     # free comment lines (meta: seed, stream, workers ...) - ignored by compare
//     <tick> <combined:16 hex> <parity:8 hex> <crc0:16 hex> <crc1:16 hex> ...
// Lines are one record per captured tick, ticks strictly increasing.
struct CrcLog {
    std::vector<std::string> systems;
    std::vector<std::string> parity_parts;
    std::vector<std::string> comments;  // written as "# ..." after the systems line
    std::vector<CrcRecord> records;

    std::string to_text() const;
    static bool parse(std::string_view text, CrcLog& out, std::string* err = nullptr);
    bool save(const std::string& path, std::string* err = nullptr) const;
    static bool load(const std::string& path, CrcLog& out, std::string* err = nullptr);
};

struct CrcDivergence {
    bool diverged = false;
    uint64_t tick = 0;      // first diverging tick (when diverged)
    std::string system;     // first system (registration order) whose CRC differs, or "parity";
                            // empty if the system lists or the record counts differ
    std::string message;    // one-line human-readable summary
};
// Compares records in order (comments ignored). Missing records count as divergence at the first
// tick present in only one log.
CrcDivergence first_divergence(const CrcLog& expected, const CrcLog& actual);

}  // namespace alamo
