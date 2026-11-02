#include "alamo/core/sim_crc.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <stdexcept>

#include "alamo/core/bytes.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/core/profile.hpp"

namespace alamo {
namespace {

constexpr uint64_t kP5 = 0x27D4EB2F165667C5ull;

inline uint64_t round_word(uint64_t h, uint64_t w) { return detail::hash_round(h, w); }
inline uint64_t load_le(const unsigned char* p) {  // little-endian host (static_assert in StateHasher::span)
    uint64_t w;
    std::memcpy(&w, p, 8);
    return w;
}
inline uint64_t fmix64(uint64_t h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return h;
}

std::string hex16(uint64_t v) {
    static const char* kDigits = "0123456789abcdef";
    std::string s(16, '0');
    for (int i = 15; i >= 0; --i) {
        s[static_cast<size_t>(i)] = kDigits[v & 0xF];
        v >>= 4;
    }
    return s;
}

std::vector<std::string_view> split_ws(std::string_view line) {
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        const size_t b = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
        if (i > b) out.push_back(line.substr(b, i - b));
    }
    return out;
}

bool parse_u64(std::string_view s, uint64_t& v, int base = 10) {
    if (s.empty()) return false;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v, base);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

bool fail(std::string* err, std::string msg) {
    if (err) *err = std::move(msg);
    return false;
}

}  // namespace

// ---- StateHasher ---------------------------------------------------------------------------------
StateHasher::StateHasher(uint64_t seed) noexcept : h_(seed + kP5) {}

void StateHasher::bytes(const void* data, size_t len) noexcept {
    if (len == 0) return;
    const auto* p = static_cast<const unsigned char*>(data);
    len_ += len;
    while (tail_len_ != 0 && len != 0) {  // fill the pending word
        acc_ |= static_cast<uint64_t>(*p++) << (8u * tail_len_);
        --len;
        if (++tail_len_ == 8) {
            h_ = round_word(h_, acc_);
            acc_ = 0;
            tail_len_ = 0;
        }
    }
    while (len >= 8) {
        h_ = round_word(h_, load_le(p));
        p += 8;
        len -= 8;
    }
    while (len != 0) {
        acc_ |= static_cast<uint64_t>(*p++) << (8u * tail_len_++);
        --len;
    }
}

void StateHasher::str(std::string_view s) noexcept {
    u32(static_cast<uint32_t>(s.size()));
    bytes(s.data(), s.size());
}

uint64_t StateHasher::digest() const noexcept {
    uint64_t h = round_word(h_, acc_);  // the partial word, zero-padded
    h ^= len_;
    return fmix64(h);
}

// ---- CrcRegistry -----------------------------------------------------------------------------------
CrcRegistry::SystemId CrcRegistry::add(std::string name, HashFn fn) {
    if (name.empty()) throw std::invalid_argument("CrcRegistry::add: empty system name");
    for (char c : name)
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            throw std::invalid_argument("CrcRegistry::add: whitespace in system name '" + name + "'");
    for (const auto& s : systems_)
        if (s.name == name) throw std::invalid_argument("CrcRegistry::add: duplicate system '" + name + "'");
    if (!fn) throw std::invalid_argument("CrcRegistry::add: empty hasher for '" + name + "'");
    // OPT3 t05: each system's hasher is a profiler zone (crc.<name>), so --profile splits the T13 CRC capture.
    const profile::ZoneId zone = profile::register_zone(("crc." + name).c_str());
    systems_.push_back(System{std::move(name), std::move(fn), zone});
    return static_cast<SystemId>(systems_.size() - 1);
}

void CrcRegistry::add_parity(std::string name, ParityFn fn) {
    if (name.empty()) throw std::invalid_argument("CrcRegistry::add_parity: empty name");
    for (char c : name)
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            throw std::invalid_argument("CrcRegistry::add_parity: whitespace in '" + name + "'");
    for (const auto& p : parity_)
        if (p.name == name) throw std::invalid_argument("CrcRegistry::add_parity: duplicate part '" + name + "'");
    if (!fn) throw std::invalid_argument("CrcRegistry::add_parity: empty function for '" + name + "'");
    parity_.push_back(Parity{std::move(name), std::move(fn)});
}

std::vector<std::string> CrcRegistry::parity_names() const {
    std::vector<std::string> out;
    for (const auto& p : parity_) out.push_back(p.name);
    return out;
}

std::vector<std::string> CrcRegistry::names() const {
    std::vector<std::string> out;
    out.reserve(systems_.size());
    for (const auto& s : systems_) out.push_back(s.name);
    return out;
}

uint64_t CrcRegistry::hash_system(SystemId id) const {
    const System& s = systems_.at(id);
    const profile::Scope zone(s.zone);
    StateHasher h;
    s.fn(h);
    return h.digest();
}

CrcRecord CrcRegistry::capture(uint64_t tick, const ParallelFor& pf) const {
    CrcRecord r;
    r.tick = tick;
    r.systems.assign(systems_.size(), 0);
    const auto n = static_cast<uint32_t>(systems_.size());
    const RangeFn body = [&](uint32_t b, uint32_t e) {
        for (uint32_t i = b; i < e; ++i) r.systems[i] = hash_system(i);
    };
    if (pf) pf(n, 1, body);
    else serial_for(n, 1, body);
    StateHasher c;
    for (uint32_t i = 0; i < n; ++i) {
        c.str(systems_[i].name);
        c.u64(r.systems[i]);
    }
    r.combined = c.digest();
    CrcSum sum;
    for (const auto& p : parity_) sum.add(p.fn());
    r.parity = parity_finish(tick, sum.value(), parity_opt_);
    return r;
}

namespace {
constexpr std::array<uint32_t, 256> make_crc_table() {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        t[i] = c;
    }
    return t;
}
constexpr auto kCrcTable = make_crc_table();
}  // namespace

uint32_t crc32_raw(const void* data, size_t len, uint32_t crc) noexcept {
    const auto* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < len; ++i) crc = kCrcTable[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return crc;
}

uint32_t parity_finish(uint64_t frame, uint32_t no_scoring_crc, const ParityFrameOptions& opt) {
    uint32_t v = no_scoring_crc;
    if (opt.scoring && (opt.scoring_always || (frame & 0x31u) == 0)) v = opt.scoring(v);
    if (opt.invert) v ^= 0xFFFFFFFFu;
    if (opt.add_one) v += 1u;
    if (opt.has_mode) {
        const unsigned char tail[4] = {opt.mode_byte, opt.land_byte, 0, 0};
        v = crc32_raw(tail, 4, v);
    }
    return v;
}

uint32_t parity_object_crc(const float pos[3], const float facing[3], float health) noexcept {
    const uint32_t x = std::bit_cast<uint32_t>(pos[0]) ^ std::bit_cast<uint32_t>(pos[1]) ^
                       std::bit_cast<uint32_t>(pos[2]) ^ std::bit_cast<uint32_t>(facing[0]) ^
                       std::bit_cast<uint32_t>(facing[1]) ^ std::bit_cast<uint32_t>(facing[2]);
    unsigned char b[8];
    size_t len = 4;
    for (int i = 0; i < 4; ++i) b[i] = static_cast<unsigned char>((x >> (8 * i)) & 0xFFu);
    if (health != 0.0f) {
        const uint32_t hb = std::bit_cast<uint32_t>(health);
        for (int i = 0; i < 4; ++i) b[4 + i] = static_cast<unsigned char>((hb >> (8 * i)) & 0xFFu);
        len = 8;
    }
    return crc32_raw(b, len);
}

uint32_t parity_rng_crc(uint32_t rng_state) noexcept {
    unsigned char b[4];
    for (int i = 0; i < 4; ++i) b[i] = static_cast<unsigned char>((rng_state >> (8 * i)) & 0xFFu);
    return crc32_raw(b, 4);
}

uint64_t combine_crcs(std::span<const std::string> names, std::span<const uint64_t> crcs) {
    StateHasher c;
    for (size_t i = 0; i < names.size() && i < crcs.size(); ++i) {
        c.str(names[i]);
        c.u64(crcs[i]);
    }
    return c.digest();
}

// ---- CrcLog ----------------------------------------------------------------------------------------
std::string CrcLog::to_text() const {
    std::string s = "ALAMO_CRC 1\nsystems " + std::to_string(systems.size());
    for (const auto& n : systems) s += " " + n;
    s += "\nparity " + std::to_string(parity_parts.size());
    for (const auto& n : parity_parts) s += " " + n;
    s += "\n";
    for (const auto& c : comments) s += "# " + c + "\n";
    for (const auto& r : records) {
        s += std::to_string(r.tick) + " " + hex16(r.combined) + " " + hex16(r.parity).substr(8);
        for (uint64_t v : r.systems) s += " " + hex16(v);
        s += "\n";
    }
    return s;
}

bool CrcLog::parse(std::string_view text, CrcLog& out, std::string* err) {
    out = CrcLog{};
    bool have_magic = false, have_systems = false, have_parity = false;
    size_t line_no = 0;
    while (!text.empty()) {
        const size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view() : text.substr(nl + 1);
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const std::string where = "line " + std::to_string(line_no) + ": ";
        if (!line.empty() && line.front() == '#') {
            line.remove_prefix(1);
            if (!line.empty() && line.front() == ' ') line.remove_prefix(1);
            out.comments.emplace_back(line);
            continue;
        }
        const auto tok = split_ws(line);
        if (tok.empty()) continue;
        if (!have_magic) {
            if (tok.size() != 2 || tok[0] != "ALAMO_CRC" || tok[1] != "1")
                return fail(err, where + "expected 'ALAMO_CRC 1'");
            have_magic = true;
            continue;
        }
        if (!have_systems) {
            uint64_t n = 0;
            if (tok[0] != "systems" || tok.size() < 2 || !parse_u64(tok[1], n) || tok.size() != n + 2)
                return fail(err, where + "expected 'systems <count> <names...>'");
            for (size_t i = 2; i < tok.size(); ++i) out.systems.emplace_back(tok[i]);
            have_systems = true;
            continue;
        }
        if (!have_parity) {
            uint64_t n = 0;
            if (tok[0] != "parity" || tok.size() < 2 || !parse_u64(tok[1], n) || tok.size() != n + 2)
                return fail(err, where + "expected 'parity <count> <names...>'");
            for (size_t i = 2; i < tok.size(); ++i) out.parity_parts.emplace_back(tok[i]);
            have_parity = true;
            continue;
        }
        if (tok.size() != out.systems.size() + 3) return fail(err, where + "wrong number of CRC columns");
        CrcRecord r;
        uint64_t parity = 0;
        if (!parse_u64(tok[0], r.tick) || !parse_u64(tok[1], r.combined, 16) || !parse_u64(tok[2], parity, 16) ||
            parity > 0xFFFFFFFFull)
            return fail(err, where + "bad tick, combined or parity CRC");
        r.parity = static_cast<uint32_t>(parity);
        r.systems.resize(out.systems.size());
        for (size_t i = 0; i < out.systems.size(); ++i)
            if (!parse_u64(tok[i + 3], r.systems[i], 16)) return fail(err, where + "bad CRC");
        if (!out.records.empty() && r.tick <= out.records.back().tick)
            return fail(err, where + "ticks must increase");
        out.records.push_back(std::move(r));
    }
    if (!have_parity) return fail(err, "missing header");
    return true;
}

bool CrcLog::save(const std::string& path, std::string* err) const { return write_text_file(path, to_text(), err); }

bool CrcLog::load(const std::string& path, CrcLog& out, std::string* err) {
    std::string text;
    if (!read_text_file(path, text, err)) return false;
    return parse(text, out, err);
}

CrcDivergence first_divergence(const CrcLog& expected, const CrcLog& actual) {
    CrcDivergence d;
    if (expected.systems != actual.systems || expected.parity_parts != actual.parity_parts) {
        d.diverged = true;
        d.message = "system lists differ";
        for (size_t i = 0; i < std::max(expected.systems.size(), actual.systems.size()); ++i) {
            const std::string a = i < expected.systems.size() ? expected.systems[i] : "<none>";
            const std::string b = i < actual.systems.size() ? actual.systems[i] : "<none>";
            if (a != b) {
                d.message += ": #" + std::to_string(i) + " expected '" + a + "', got '" + b + "'";
                break;
            }
        }
        return d;
    }
    const size_t n = std::min(expected.records.size(), actual.records.size());
    for (size_t i = 0; i < n; ++i) {
        const CrcRecord& e = expected.records[i];
        const CrcRecord& a = actual.records[i];
        if (e.tick != a.tick) {
            d.diverged = true;
            d.tick = std::min(e.tick, a.tick);
            d.message = "record " + std::to_string(i) + ": expected tick " + std::to_string(e.tick) + ", got tick " +
                        std::to_string(a.tick);
            return d;
        }
        if (e == a) continue;
        d.diverged = true;
        d.tick = e.tick;
        for (size_t s = 0; s < e.systems.size(); ++s) {
            if (e.systems[s] != a.systems[s]) {
                d.system = expected.systems[s];
                d.message = "tick " + std::to_string(e.tick) + ": system '" + d.system + "' expected " +
                            hex16(e.systems[s]) + ", got " + hex16(a.systems[s]);
                return d;
            }
        }
        if (e.parity != a.parity) {
            d.system = "parity";
            d.message = "tick " + std::to_string(e.tick) + ": parity CRC differs (" + hex16(e.parity).substr(8) +
                        " vs " + hex16(a.parity).substr(8) + "), ordered systems equal";
            return d;
        }
        d.message = "tick " + std::to_string(e.tick) + ": combined CRC differs (" + hex16(e.combined) + " vs " +
                    hex16(a.combined) + ")";
        return d;
    }
    if (expected.records.size() != actual.records.size()) {
        d.diverged = true;
        const auto& longer = expected.records.size() > n ? expected.records : actual.records;
        d.tick = longer[n].tick;
        d.message = "expected " + std::to_string(expected.records.size()) + " records, got " +
                    std::to_string(actual.records.size()) + " (first unmatched tick " + std::to_string(d.tick) + ")";
        return d;
    }
    d.message = "identical (" + std::to_string(n) + " ticks)";
    return d;
}

}  // namespace alamo
