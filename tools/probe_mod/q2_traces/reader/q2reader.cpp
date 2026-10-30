// q2reader: read-only reference-trace recorder for StarWarsG.exe (FoC Steam x64 1.121).
//
// Attaches to the running game with PROCESS_VM_READ (plus SYNCHRONIZE, and PROCESS_TERMINATE only with
// --close-game), polls the documented statics once per logic tick and writes a Q2TRACE text file
// (format: tools/probe_mod/q2_traces/README.md "Trace format"). It never writes to the game's memory,
// never injects code and never suspends the game's threads.
//
// Build: tools/probe_mod/q2_traces/build_reader.cmd (project-local MSVC, no dependencies).
#include <windows.h>
#include <timeapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "q2_layout.hpp"

namespace {

constexpr int kReaderVersion = 2;

// ---- checksums ------------------------------------------------------------------------------------
uint32_t g_crc_table[256];
void init_crc_table() {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crc_table[i] = c;
    }
}
// The original's CRC form (COMMANDS.md §7.1): reflected table, init 0, no final inversion.
uint32_t crc32_raw(const void* data, size_t len, uint32_t crc = 0) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < len; ++i) crc = g_crc_table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return crc;
}
// Standard zlib CRC-32 (used only for the engine-independent type-name key).
uint32_t crc32_zlib(const std::string& s) { return crc32_raw(s.data(), s.size(), 0xFFFFFFFFu) ^ 0xFFFFFFFFu; }

uint32_t bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

// Per-object CRC as engine/core parity_object_crc (OBJECT_RUNTIME.md §6): crc32_raw over the u32 xor of
// the raw position and facing bits, then the health bits when health != 0.
uint32_t object_crc(const float pos[3], const float fac[3], float health) {
    const uint32_t x = bits(pos[0]) ^ bits(pos[1]) ^ bits(pos[2]) ^ bits(fac[0]) ^ bits(fac[1]) ^ bits(fac[2]);
    unsigned char b[8];
    size_t len = 4;
    for (int i = 0; i < 4; ++i) b[i] = static_cast<unsigned char>((x >> (8 * i)) & 0xFFu);
    if (health != 0.0f) {
        const uint32_t hb = bits(health);
        for (int i = 0; i < 4; ++i) b[4 + i] = static_cast<unsigned char>((hb >> (8 * i)) & 0xFFu);
        len = 8;
    }
    return crc32_raw(b, len);
}
uint32_t rng_crc(uint32_t s) {
    unsigned char b[4];
    for (int i = 0; i < 4; ++i) b[i] = static_cast<unsigned char>((s >> (8 * i)) & 0xFFu);
    return crc32_raw(b, 4);
}

// StateHasher, bit-identical to engine/core/include/alamo/core/sim_crc.hpp (its documented algorithm).
class StateHasher {
public:
    void u32(uint32_t v) {
        unsigned char b[4];
        for (int i = 0; i < 4; ++i) b[i] = static_cast<unsigned char>((v >> (8 * i)) & 0xFFu);
        bytes(b, 4);
    }
    void u64(uint64_t v) {
        unsigned char b[8];
        for (int i = 0; i < 8; ++i) b[i] = static_cast<unsigned char>((v >> (8 * i)) & 0xFFu);
        bytes(b, 8);
    }
    void str(const std::string& s) {
        u32(static_cast<uint32_t>(s.size()));
        bytes(s.data(), s.size());
    }
    void bytes(const void* data, size_t len) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < len; ++i) {
            tail_[tail_len_++] = p[i];
            if (tail_len_ == 8) {
                h_ = round_word(h_, load_le(tail_));
                tail_len_ = 0;
            }
        }
        len_ += len;
    }
    uint64_t digest() const {
        unsigned char last[8] = {};
        for (uint32_t i = 0; i < tail_len_; ++i) last[i] = tail_[i];
        uint64_t h = round_word(h_, load_le(last));
        h ^= len_;
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdull;
        h ^= h >> 33;
        h *= 0xc4ceb9fe1a85ec53ull;
        h ^= h >> 33;
        return h;
    }

private:
    static constexpr uint64_t kP1 = 0x9E3779B185EBCA87ull;
    static constexpr uint64_t kP2 = 0xC2B2AE3D27D4EB4Full;
    static constexpr uint64_t kP5 = 0x27D4EB2F165667C5ull;
    static uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
    static uint64_t round_word(uint64_t h, uint64_t w) { return rotl(h + w * kP2, 31) * kP1; }
    static uint64_t load_le(const unsigned char* p) {
        uint64_t w = 0;
        for (int i = 7; i >= 0; --i) w = (w << 8) | p[i];
        return w;
    }
    uint64_t h_ = kP5;  // seed 0
    uint64_t len_ = 0;
    unsigned char tail_[8] = {};
    uint32_t tail_len_ = 0;
};

// ---- small helpers --------------------------------------------------------------------------------
std::string hex8(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "%08x", v);
    return b;
}
std::string hex16(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
    return b;
}
std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            o += ' ';
        } else {
            o += c;
        }
    }
    return o;
}
template <class T>
T get(const std::vector<unsigned char>& b, size_t off) {
    T v{};
    if (off + sizeof(T) <= b.size()) std::memcpy(&v, b.data() + off, sizeof(T));
    return v;
}
double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ---- options --------------------------------------------------------------------------------------
struct Options {
    std::string out, status, dump, queues, scenario = "galactic";
    double object_budget_ms = 12;  // an object read slower than this makes the next tick a light sample
    std::vector<std::pair<std::string, std::string>> meta;
    uint32_t days = 10, day_frames = 1350, ticks = 5400, key_every = 300;
    double wait_process = 180, wait_session = 1800, timeout = 3600, pause_stop = 60;
    bool close_game = false, allow_unknown_exe = false, selftest = false;
    DWORD pid = 0;
    uint64_t test_base = 0;  // test target only: treat this address as the module base
};

void usage() {
    std::fprintf(stderr,
                 "q2reader --out <trace.q2t> --status <status.json> --scenario galactic|skirmish\n"
                 "  [--days N=10] [--day-frames 1350] [--ticks M=5400] [--key-every 300]\n"
                 "  [--wait-process-sec 180] [--wait-session-sec 1800] [--timeout-sec 3600]\n"
                 "  [--pause-stop-sec 60] [--close-game] [--allow-unknown-exe] [--meta key=value]...\n"
                 "  [--dump playerlist_dump.txt] [--queues production.q2q] [--object-budget-ms 12]\n"
                 "  [--pid N --test-base 0xADDR]  (test target only)   |   --selftest\n");
}

bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string& dst) {
            if (i + 1 >= argc) return false;
            dst = argv[++i];
            return true;
        };
        std::string v;
        if (a == "--out") { if (!next(o.out)) return false; }
        else if (a == "--dump") { if (!next(o.dump)) return false; }
        else if (a == "--queues") { if (!next(o.queues)) return false; }
        else if (a == "--object-budget-ms") { if (!next(v)) return false; o.object_budget_ms = std::stod(v); }
        else if (a == "--status") { if (!next(o.status)) return false; }
        else if (a == "--scenario") { if (!next(o.scenario)) return false; }
        else if (a == "--days") { if (!next(v)) return false; o.days = static_cast<uint32_t>(std::stoul(v)); }
        else if (a == "--day-frames") { if (!next(v)) return false; o.day_frames = static_cast<uint32_t>(std::stoul(v)); }
        else if (a == "--ticks") { if (!next(v)) return false; o.ticks = static_cast<uint32_t>(std::stoul(v)); }
        else if (a == "--key-every") { if (!next(v)) return false; o.key_every = std::max(1u, static_cast<uint32_t>(std::stoul(v))); }
        else if (a == "--wait-process-sec") { if (!next(v)) return false; o.wait_process = std::stod(v); }
        else if (a == "--wait-session-sec") { if (!next(v)) return false; o.wait_session = std::stod(v); }
        else if (a == "--timeout-sec") { if (!next(v)) return false; o.timeout = std::stod(v); }
        else if (a == "--pause-stop-sec") { if (!next(v)) return false; o.pause_stop = std::stod(v); }
        else if (a == "--close-game") o.close_game = true;
        else if (a == "--allow-unknown-exe") o.allow_unknown_exe = true;
        else if (a == "--selftest") o.selftest = true;
        else if (a == "--pid") { if (!next(v)) return false; o.pid = static_cast<DWORD>(std::stoul(v)); }
        else if (a == "--test-base") { if (!next(v)) return false; o.test_base = std::stoull(v, nullptr, 16); }
        else if (a == "--meta") {
            if (!next(v)) return false;
            const auto eq = v.find('=');
            if (eq == std::string::npos) return false;
            std::string val = v.substr(eq + 1);
            std::replace(val.begin(), val.end(), '\n', ' ');
            o.meta.emplace_back(v.substr(0, eq), val);
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return false;
        }
    }
    if (o.selftest) return true;
    if (o.out.empty() || o.status.empty()) return false;
    return o.scenario == "galactic" || o.scenario == "skirmish";
}

// ---- process access ---------------------------------------------------------------------------------
struct Game {
    HANDLE h = nullptr;
    DWORD pid = 0;
    uint64_t base = 0;
    uint32_t timestamp = 0, size_of_image = 0;
    std::string path;

    bool rd(uint64_t addr, void* dst, size_t n) const {
        SIZE_T got = 0;
        if (addr == 0) return false;
        if (!ReadProcessMemory(h, reinterpret_cast<LPCVOID>(addr), dst, n, &got)) return false;
        return got == n;
    }
    template <class T>
    bool rdv(uint64_t addr, T& v) const { return rd(addr, &v, sizeof(T)); }
    bool block(uint64_t addr, size_t n, std::vector<unsigned char>& b) const {
        b.assign(n, 0);
        return rd(addr, b.data(), n);
    }
    bool alive() const { return WaitForSingleObject(h, 0) == WAIT_TIMEOUT; }
};

bool module_of(DWORD pid, uint64_t& base, std::string& path) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32 me{};
    me.dwSize = sizeof me;
    bool ok = Module32First(snap, &me) != FALSE;  // first entry = the exe
    if (ok) {
        base = reinterpret_cast<uint64_t>(me.modBaseAddr);
        path = me.szExePath;
    }
    CloseHandle(snap);
    return ok;
}

std::vector<DWORD> find_pids(const char* exe) {
    std::vector<DWORD> r;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return r;
    PROCESSENTRY32 pe{};
    pe.dwSize = sizeof pe;
    for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe))
        if (_stricmp(pe.szExeFile, exe) == 0) r.push_back(pe.th32ProcessID);
    CloseHandle(snap);
    return r;
}

bool read_pe_identity(Game& g) {
    uint32_t lfanew = 0;
    if (!g.rdv(g.base + 0x3C, lfanew)) return false;
    uint32_t sig = 0;
    if (!g.rdv(g.base + lfanew, sig) || sig != 0x00004550u) return false;
    if (!g.rdv(g.base + lfanew + 8, g.timestamp)) return false;
    return g.rdv(g.base + lfanew + 24 + 56, g.size_of_image);  // PE32+ optional header SizeOfImage
}

// ---- trace state ------------------------------------------------------------------------------------
struct TypeInfo {
    uint32_t index = 0, engine_crc = 0, name_crc = 0;
    std::string name;
};

struct ObjRec {
    int32_t id = 0, owner = 0, parent_mode = 0, container = q2::kNoObject, attach = q2::kNoObject;
    uint32_t type_index = 0, type_name_crc = 0, crc_cached = 0, crc_calc = 0;
    float pos[3] = {}, fac[3] = {}, health = 0;
    uint8_t flags = 0;
    uint64_t ptr = 0, container_ptr = 0, attach_ptr = 0, planet_pack = 0, production = 0;
    bool has_planet = false;
    std::string planet_line;
};

class Tracer {
public:
    Tracer(const Options& o, Game& g) : o_(o), g_(g) {}
    int run();

private:
    const Options& o_;
    Game& g_;
    FILE* f_ = nullptr;
    FILE* q_ = nullptr;  // production-queue side file (galactic, --queues)
    std::map<std::pair<int32_t, int>, std::string> last_queue_;
    uint64_t queue_lines_ = 0;
    std::unordered_map<uint64_t, TypeInfo> types_;  // tool-side cache keyed by type pointer (not sim)
    std::map<int32_t, std::string> last_obj_, last_planet_;
    std::map<uint32_t, std::string> last_player_;
    uint64_t player_at_ = 0;
    int player_kind_ = 0;
    std::string player_how_;
    uint64_t frames_ = 0, missed_ = 0, torn_ = 0, objects_max_ = 0, last_frame_ = 0, light_ = 0;
    double read_ms_sum_ = 0, read_ms_max_ = 0;
    bool layout_ok_ = false;
    uint32_t start_cycle_ = 0, cur_cycle_ = 0, start_mode_frame_ = 0, cur_mode_frame_ = 0;
    int32_t last_pause_ = 0;
    std::string state_ = "starting", reason_, error_;
    uint32_t seed_ = 0;
    double t0_ = 0, last_status_ = 0;

    void write_status(bool final_);
    bool type_of(uint64_t tp, TypeInfo& out);
    bool read_objects(uint64_t gom, std::vector<ObjRec>& objs, bool validate);
    bool vec_elems(uint64_t at, int kind, std::vector<uint64_t>& ptrs) const;
    bool looks_like_players(const std::vector<uint64_t>& ptrs) const;
    void find_players(bool late);
    bool search_players(uint64_t& at, int& kind, std::string& how, std::vector<uint64_t>& ptrs) const;
    void dump_playerlist(const char* when);
    void read_players(std::vector<std::string>& lines);
    void write_queues(uint32_t f, uint32_t mframe, const std::vector<ObjRec>& objs, bool key);
    int finish(int code);
};

void Tracer::write_status(bool final_) {
    if (o_.status.empty()) return;
    const std::string tmp = o_.status + ".tmp";
    FILE* s = std::fopen(tmp.c_str(), "wb");
    if (!s) return;
    std::fprintf(s,
                 "{\n  \"tool\": \"q2reader\",\n  \"version\": %d,\n  \"state\": \"%s\",\n  \"final\": %s,\n"
                 "  \"reason\": \"%s\",\n  \"error\": \"%s\",\n  \"scenario\": \"%s\",\n  \"pid\": %lu,\n"
                 "  \"seed\": \"0x%s\",\n  \"frames_sampled\": %llu,\n  \"frames_missed\": %llu,\n"
                 "  \"frames_torn\": %llu,\n  \"last_global_frame\": %llu,\n  \"mode_frame\": %u,\n"
                 "  \"cycles_done\": %u,\n  \"objects_max\": %llu,\n  \"galactic_pause\": %d,\n"
                 "  \"wall_seconds\": %.1f,\n  \"light_samples\": %llu,\n  \"object_read_ms_avg\": %.2f,\n"
                 "  \"object_read_ms_max\": %.2f,\n  \"player_vector\": \"%s\",\n  \"queue_lines\": %llu,\n"
                 "  \"trace\": \"%s\"\n}\n",
                 kReaderVersion, state_.c_str(), final_ ? "true" : "false", json_escape(reason_).c_str(),
                 json_escape(error_).c_str(), o_.scenario.c_str(), static_cast<unsigned long>(g_.pid),
                 hex8(seed_).c_str(), static_cast<unsigned long long>(frames_),
                 static_cast<unsigned long long>(missed_), static_cast<unsigned long long>(torn_),
                 static_cast<unsigned long long>(last_frame_), cur_mode_frame_ - start_mode_frame_,
                 cur_cycle_ - start_cycle_, static_cast<unsigned long long>(objects_max_), last_pause_,
                 t0_ > 0 ? now_s() - t0_ : 0.0, static_cast<unsigned long long>(light_),
                 frames_ > light_ ? read_ms_sum_ / static_cast<double>(frames_ - light_) : 0.0, read_ms_max_,
                 json_escape(player_how_).c_str(), static_cast<unsigned long long>(queue_lines_),
                 json_escape(o_.out).c_str());
    std::fclose(s);
    MoveFileExA(tmp.c_str(), o_.status.c_str(), MOVEFILE_REPLACE_EXISTING);
}

bool Tracer::type_of(uint64_t tp, TypeInfo& out) {
    auto it = types_.find(tp);
    if (it != types_.end()) {
        out = it->second;
        return true;
    }
    std::vector<unsigned char> b;
    if (!g_.block(tp + q2::kTypeIndex, q2::kTypeNameCrc + 4 - q2::kTypeIndex, b)) return false;
    TypeInfo t;
    t.index = get<uint32_t>(b, 0);
    t.engine_crc = get<uint32_t>(b, q2::kTypeNameCrc - q2::kTypeIndex);
    const size_t so = q2::kTypeName - q2::kTypeIndex;
    const auto size = get<uint64_t>(b, so + 0x10);
    const auto cap = get<uint64_t>(b, so + 0x18);
    if (size < 256) {
        if (cap > 15) {
            t.name.assign(static_cast<size_t>(size), '\0');
            if (size && !g_.rd(get<uint64_t>(b, so), t.name.data(), static_cast<size_t>(size))) t.name = "?";
        } else if (size <= 15) {
            t.name.assign(reinterpret_cast<const char*>(b.data() + so), static_cast<size_t>(size));
        }
    }
    for (char& c : t.name)
        if (static_cast<unsigned char>(c) <= 0x20 || static_cast<unsigned char>(c) >= 0x7F) c = '_';
    if (t.name.empty()) t.name = "?";
    t.name_crc = crc32_zlib(t.name);
    types_[tp] = t;
    out = t;
    return true;
}

bool Tracer::read_objects(uint64_t gom, std::vector<ObjRec>& objs, bool validate) {
    objs.clear();
    uint64_t head = 0, size = 0;
    if (!g_.rdv(gom + q2::kGomIdMapHead, head) || !g_.rdv(gom + q2::kGomIdMapSize, size)) return false;
    if (size > 1000000) {
        error_ = "id map size implausible: " + std::to_string(size);
        return false;
    }
    uint64_t node = 0;
    if (!g_.rdv(head + q2::kNodeNext, node)) return false;
    std::vector<unsigned char> nb, ob, pb;
    uint64_t matched = 0;
    for (uint64_t i = 0; i < size && node != head && node != 0; ++i) {
        if (!g_.block(node, q2::kNodeBlock, nb)) return false;
        const uint64_t next = get<uint64_t>(nb, q2::kNodeNext);
        const int32_t key = get<int32_t>(nb, q2::kNodeKey);
        const uint64_t obj = get<uint64_t>(nb, q2::kNodeValue);
        node = next;
        if (!g_.block(obj + q2::kObjReadFrom, q2::kObjReadTo - q2::kObjReadFrom, ob)) continue;
        auto at = [](uint32_t off) { return static_cast<size_t>(off - q2::kObjReadFrom); };
        ObjRec r;
        r.ptr = obj;
        r.id = get<int32_t>(ob, at(q2::kObjId));
        if (r.id == key) ++matched;
        r.parent_mode = get<int32_t>(ob, at(q2::kObjParentModeId));
        r.owner = get<int32_t>(ob, at(q2::kObjOwner));
        r.health = get<float>(ob, at(q2::kObjHealth));
        for (int k = 0; k < 3; ++k) {
            r.pos[k] = get<float>(ob, at(q2::kObjPos) + 4u * static_cast<size_t>(k));
            r.fac[k] = get<float>(ob, at(q2::kObjFacing) + 4u * static_cast<size_t>(k));
        }
        r.planet_pack = get<uint64_t>(ob, at(q2::kObjPlanetPack));
        r.production = get<uint64_t>(ob, at(q2::kObjProduction));
        r.container_ptr = get<uint64_t>(ob, at(q2::kObjContainer));
        r.attach_ptr = get<uint64_t>(ob, at(q2::kObjAttachParent));
        r.crc_cached = get<uint32_t>(ob, at(q2::kObjCrc));
        r.flags = get<uint8_t>(ob, at(q2::kObjFlags));
        TypeInfo t;
        if (type_of(get<uint64_t>(ob, at(q2::kObjType)), t)) {
            r.type_index = t.index;
            r.type_name_crc = t.name_crc;
        }
        r.crc_calc = object_crc(r.pos, r.fac, r.health);
        if (r.planet_pack && g_.block(r.planet_pack, q2::kPlanBlock, pb)) {
            r.has_planet = true;
            r.planet_line = "G " + std::to_string(r.id) + " " + hex8(get<uint32_t>(pb, q2::kPlanControl)) + " " +
                            std::to_string(get<int32_t>(pb, q2::kPlanBase)) + " " +
                            hex8(get<uint32_t>(pb, q2::kPlanBlackMkt)) + " " +
                            hex8(get<uint32_t>(pb, q2::kPlanPiracy)) + " " +
                            std::to_string(get<int32_t>(pb, q2::kPlanCorruption)) + " " +
                            std::to_string(get<uint8_t>(pb, q2::kPlanDestroyed)) + " " +
                            std::to_string(get<int32_t>(pb, q2::kPlanSiphoner));
        }
        objs.push_back(r);
    }
    if (validate && !objs.empty() && matched * 10 < objs.size() * 9) {
        error_ = "id map layout check failed: " + std::to_string(matched) + " of " + std::to_string(objs.size()) +
                 " objects have +0x50 == map key";
        return false;
    }
    std::unordered_map<uint64_t, int32_t> by_ptr;
    for (const auto& r : objs) by_ptr[r.ptr] = r.id;
    for (auto& r : objs) {
        auto res = [&](uint64_t p) {
            if (!p) return q2::kNoObject;
            auto it = by_ptr.find(p);
            if (it != by_ptr.end()) return it->second;
            int32_t id = q2::kNoObject;
            g_.rdv(p + q2::kObjId, id);
            return id;
        };
        r.container = res(r.container_ptr);
        r.attach = res(r.attach_ptr);
    }
    std::sort(objs.begin(), objs.end(), [](const ObjRec& a, const ObjRec& b) { return a.id < b.id; });
    return true;
}

// The player vector inside PlayerList (0x140a16fd0) is not documented. Candidate vector shapes at an
// address A: kind 0 = DynamicVector (data pointer, i32 count 8 bytes later, as GMM +0x40), kind 1 =
// std::vector (begin, end). A is PlayerList + off, or one pointer hop away (*(PlayerList + off1) + off2).
// The elements must look like players: readable, finite credits in [0, 1e8], Is_Human byte 0/1, player
// +0x48 (list index) == element index, and at least one human. Seen in the game (2026-10-30, read-only): a
// std::vector at PlayerList +0x00 in galactic and in skirmish. The v1 run found nothing because v1 tried only
// DynamicVector shapes and required a funded player at frame 0.
bool Tracer::vec_elems(uint64_t at, int kind, std::vector<uint64_t>& ptrs) const {
    ptrs.clear();
    uint64_t a = 0, b = 0;
    if (!g_.rdv(at, a) || !g_.rdv(at + 8, b) || a == 0) return false;
    uint64_t count = 0;
    if (kind == 0) {
        count = b & 0xFFFFFFFFull;
    } else {
        if (b < a || (b - a) % 8 != 0) return false;
        count = (b - a) / 8;
    }
    if (count < 2 || count > 32) return false;
    ptrs.resize(static_cast<size_t>(count));
    return g_.rd(a, ptrs.data(), ptrs.size() * 8);
}

bool Tracer::looks_like_players(const std::vector<uint64_t>& ptrs) const {
    std::vector<unsigned char> pb;
    int humans = 0;
    for (size_t i = 0; i < ptrs.size(); ++i) {
        if (!g_.block(ptrs[i], q2::kPlayerBlock, pb)) return false;
        const float cr = get<float>(pb, q2::kPlayerCredits);
        const uint8_t hu = get<uint8_t>(pb, q2::kPlayerIsHuman);
        if (!std::isfinite(cr) || cr < 0.0f || cr > 1.0e8f || hu > 1) return false;
        if (get<int32_t>(pb, q2::kPlayerIndex) != static_cast<int32_t>(i)) return false;
        humans += hu;
    }
    return humans >= 1;
}

// Search only, no side effects (also used to tell a real skirmish from the main menu's backdrop battle).
bool Tracer::search_players(uint64_t& at, int& kind, std::string& how, std::vector<uint64_t>& ptrs) const {
    std::vector<unsigned char> pl, inner;
    const uint64_t pl_addr = g_.base + q2::kRvaPlayerList;
    if (!g_.block(pl_addr, 0x200, pl)) return false;
    for (size_t off = 0; off + 16 <= pl.size(); off += 8)
        for (int k = 0; k < 2; ++k)
            if (vec_elems(pl_addr + off, k, ptrs) && looks_like_players(ptrs)) {
                char h[64];
                std::snprintf(h, sizeof h, "PlayerList+0x%zx", off);
                at = pl_addr + off;
                kind = k;
                how = h;
                return true;
            }
    for (size_t off1 = 0; off1 + 8 <= pl.size(); off1 += 8) {
        const uint64_t p = get<uint64_t>(pl, off1);
        if (p < 0x10000 || (p & 7) != 0 || !g_.block(p, 0x80, inner)) continue;
        for (size_t off2 = 0; off2 + 16 <= inner.size(); off2 += 8)
            for (int k = 0; k < 2; ++k)
                if (vec_elems(p + off2, k, ptrs) && looks_like_players(ptrs)) {
                    char h[80];
                    std::snprintf(h, sizeof h, "*(PlayerList+0x%zx)+0x%zx", off1, off2);
                    at = p + off2;
                    kind = k;
                    how = h;
                    return true;
                }
    }
    return false;
}

void Tracer::find_players(bool late) {
    std::vector<uint64_t> ptrs;
    uint64_t at = 0;
    int kind = 0;
    std::string how;
    if (!search_players(at, kind, how, ptrs)) return;
    player_at_ = at;
    player_kind_ = kind;
    player_how_ = how + (kind == 0 ? " dynamicvector" : " stdvector") + " players=" + std::to_string(ptrs.size());
    std::fprintf(f_, "%s player_vector %s\n", late ? "M" : "meta", player_how_.c_str());
}

// Raw dump of PlayerList and two pointer levels below it, for offline RE of the player vector.
void Tracer::dump_playerlist(const char* when) {
    if (o_.dump.empty()) return;
    FILE* d = std::fopen(o_.dump.c_str(), "ab");
    if (!d) return;
    std::fprintf(d, "== %s (module base 0x%llx, PlayerList RVA 0x%llx)\n", when, static_cast<unsigned long long>(g_.base),
                 static_cast<unsigned long long>(q2::kRvaPlayerList));
    int blocks = 0;
    std::vector<unsigned char> b;
    auto dump_block = [&](uint64_t addr, size_t n, const std::string& label) {
        if (!g_.block(addr, n, b)) return false;
        ++blocks;
        std::fprintf(d, "-- %s @0x%llx\n", label.c_str(), static_cast<unsigned long long>(addr));
        for (size_t i = 0; i < n; i += 16)
            std::fprintf(d, "  +%03zx %016llx %016llx  f32@+8:%g f32@+c:%g\n", i,
                         static_cast<unsigned long long>(get<uint64_t>(b, i)),
                         static_cast<unsigned long long>(get<uint64_t>(b, i + 8)), static_cast<double>(get<float>(b, i + 8)),
                         static_cast<double>(get<float>(b, i + 12)));
        return true;
    };
    std::vector<unsigned char> root;
    if (g_.block(g_.base + q2::kRvaPlayerList, 0x200, root)) {
        dump_block(g_.base + q2::kRvaPlayerList, 0x200, "PlayerList");
        for (size_t o1 = 0; o1 < 0x200 && blocks < 200; o1 += 8) {
            const uint64_t p = get<uint64_t>(root, o1);
            if (p < 0x10000 || (p & 7) != 0) continue;
            std::vector<unsigned char> lvl1;
            if (!g_.block(p, 0x100, lvl1)) continue;
            dump_block(p, 0x100, "PlayerList+" + std::to_string(o1) + " ->");
            for (size_t o2 = 0; o2 < 0x100 && blocks < 200; o2 += 8) {
                const uint64_t q = get<uint64_t>(lvl1, o2);
                if (q < 0x10000 || (q & 7) != 0) continue;
                dump_block(q, q2::kPlayerBlock, "PlayerList+" + std::to_string(o1) + " ->+" + std::to_string(o2) + " ->");
            }
        }
    }
    std::fprintf(d, "== end (%d blocks)\n", blocks);
    std::fclose(d);
}

void Tracer::read_players(std::vector<std::string>& lines) {
    lines.clear();
    if (player_at_ == 0) return;
    std::vector<uint64_t> ptrs;
    if (!vec_elems(player_at_, player_kind_, ptrs)) return;
    std::vector<unsigned char> pb;
    for (size_t i = 0; i < ptrs.size(); ++i) {
        if (!g_.block(ptrs[i], q2::kPlayerBlock, pb)) continue;
        lines.push_back("C " + std::to_string(i) + " " + hex8(get<uint32_t>(pb, q2::kPlayerCredits)) + " " +
                        hex8(get<uint32_t>(pb, q2::kPlayerCap)) + " " +
                        std::to_string(get<uint8_t>(pb, q2::kPlayerIsHuman)) + " " +
                        (get<uint64_t>(pb, q2::kPlayerAi) ? "1" : "0"));
    }
}

// Production queues of every planet (GALACTIC_ECONOMY.md §2.1), written to the side file only when a
// (planet, queue) line changes, or on keyframes. Type indices are the trace's T-line indices.
void Tracer::write_queues(uint32_t f, uint32_t mframe, const std::vector<ObjRec>& objs, bool key) {
    if (!q_) return;
    std::vector<unsigned char> pb, ub;
    for (const auto& r : objs) {
        if (!r.has_planet || !r.production) continue;
        if (!g_.block(r.production, q2::kProdBlock, pb)) continue;
        for (int qi = 0; qi < 2; ++qi) {
            const uint64_t data = get<uint64_t>(pb, q2::kProdQueueData[qi]);
            const int32_t n = get<int32_t>(pb, q2::kProdQueueCount[qi]);
            std::string line = std::to_string(r.id) + " " + std::to_string(qi) + " " + std::to_string(n);
            if (n > 0 && n <= 64 && data) {
                std::vector<uint64_t> ents(static_cast<size_t>(n));
                if (g_.rd(data, ents.data(), ents.size() * 8)) {
                    for (uint64_t e : ents) {
                        if (!g_.block(e, q2::kUcSize, ub)) {
                            line += " ?";
                            continue;
                        }
                        TypeInfo t;
                        const uint32_t ti = type_of(get<uint64_t>(ub, q2::kUcType), t) ? t.index : 0xFFFFFFFFu;
                        int32_t created = q2::kNoObject;
                        if (const uint64_t c = get<uint64_t>(ub, q2::kUcCreated)) g_.rdv(c + q2::kObjId, created);
                        line += " " + std::to_string(ti) + ":" + std::to_string(get<int32_t>(ub, q2::kUcPlayer)) + ":" +
                                std::to_string(get<uint32_t>(ub, q2::kUcStart)) + ":" +
                                std::to_string(get<uint32_t>(ub, q2::kUcFrames)) + ":" +
                                hex8(get<uint32_t>(ub, q2::kUcBuildTime)) + ":" + hex8(get<uint32_t>(ub, q2::kUcRemain)) +
                                ":" + hex8(get<uint32_t>(ub, q2::kUcCost)) + ":" +
                                std::to_string(get<int32_t>(ub, q2::kUcTag)) + ":" +
                                std::to_string(get<int32_t>(ub, q2::kUcPlanet)) + ":" + std::to_string(created);
                    }
                }
            }
            auto& last = last_queue_[{r.id, qi}];
            if (key || last != line) {
                std::fprintf(q_, "Q %u %u %s\n", f, mframe, line.c_str());
                last = line;
                ++queue_lines_;
            }
        }
    }
}

int Tracer::finish(int code) {
    if (f_ && g_.alive()) dump_playerlist("end of trace");
    if (f_) {
        std::vector<std::pair<uint32_t, const TypeInfo*>> ts;
        for (const auto& kv : types_) ts.emplace_back(kv.second.index, &kv.second);
        std::sort(ts.begin(), ts.end(), [](const auto& a, const auto& b) {
            return a.first != b.first ? a.first < b.first : a.second->name < b.second->name;
        });
        for (const auto& t : ts)
            std::fprintf(f_, "T %u %s %s %s\n", t.first, hex8(t.second->name_crc).c_str(),
                         hex8(t.second->engine_crc).c_str(), t.second->name.c_str());
        std::fprintf(f_, "E %s\nend\n", reason_.c_str());
        std::fclose(f_);
        f_ = nullptr;
    }
    if (q_) {
        std::fprintf(q_, "E %s\nend\n", reason_.c_str());
        std::fclose(q_);
        q_ = nullptr;
    }
    state_ = "done";
    write_status(true);
    if (o_.close_game && g_.alive()) {
        EnumWindows(
            [](HWND w, LPARAM lp) -> BOOL {
                DWORD p = 0;
                GetWindowThreadProcessId(w, &p);
                if (p == static_cast<DWORD>(lp)) PostMessageA(w, WM_CLOSE, 0, 0);
                return TRUE;
            },
            static_cast<LPARAM>(g_.pid));
        if (WaitForSingleObject(g_.h, 20000) != WAIT_OBJECT_0) TerminateProcess(g_.h, 0);
    }
    return code;
}

int Tracer::run() {
    t0_ = now_s();
    const uint64_t fs = g_.base + q2::kRvaFrameSync, gmm = g_.base + q2::kRvaGmm;
    const bool galactic = o_.scenario == "galactic";
    std::vector<unsigned char> gb, fb;
    // 1. Wait for the session (UI navigation and loading happen here).
    state_ = "waiting_session";
    bool load_seen = false;
    uint32_t load_frame = 0, load_rng = 0;
    // Skirmish: the main menu's backdrop is itself a running space mode (game type 3, no human player; seen
    // 2026-10-30), so a tactical session counts only once a player vector with a human exists.
    bool have_players = false;
    uint64_t ps_at = 0;
    int ps_kind = 0;
    double last_ps = 0.0;
    for (;;) {
        if (!g_.alive()) { reason_ = "game_exited"; error_ = "game exited before the session started"; return finish(12); }
        if (now_s() - t0_ > o_.wait_session) { reason_ = "no_session"; return finish(5); }
        if (g_.block(gmm, q2::kGmmBlock, gb) && g_.block(fs, q2::kFsBlock, fb)) {
            const int32_t stack = get<int32_t>(gb, q2::kGmmStackCount);
            const int32_t mtype = get<int32_t>(gb, q2::kGmmModeType);
            const uint64_t mode = get<uint64_t>(gb, q2::kGmmCurMode);
            const uint8_t stopped = get<uint8_t>(fb, q2::kFsSimStopped);
            if (stack >= 1 && !load_seen) {
                load_seen = true;
                load_frame = get<uint32_t>(fb, q2::kFsFrame);
                g_.rdv(g_.base + q2::kRvaRngSync, load_rng);
            }
            if (stack < 1) {
                load_seen = false;
                have_players = false;
            }
            if (!galactic && stack >= 1) {
                std::vector<uint64_t> ptrs;
                if (have_players) have_players = vec_elems(ps_at, ps_kind, ptrs) && looks_like_players(ptrs);
                if (!have_players && now_s() - last_ps > 0.25) {
                    last_ps = now_s();
                    std::string how;
                    have_players = search_players(ps_at, ps_kind, how, ptrs);
                }
            }
            const bool want = galactic ? (mtype == 0) : ((mtype == 1 || mtype == 2) && have_players);
            if (stack == 1 && mode != 0 && want && stopped == 0) break;
        }
        if (now_s() - last_status_ > 2.0) { write_status(false); last_status_ = now_s(); }
        Sleep(2);
    }
    // 2. Header.
    f_ = std::fopen(o_.out.c_str(), "wb");
    if (!f_) { error_ = "cannot open " + o_.out; reason_ = "io_error"; return finish(1); }
    seed_ = get<uint32_t>(gb, q2::kGmmSeed);
    const uint64_t mode = get<uint64_t>(gb, q2::kGmmCurMode);
    const int32_t mtype0 = get<int32_t>(gb, q2::kGmmModeType);
    std::fprintf(f_, "Q2TRACE 1\nmeta reader %d\nmeta scenario %s\nmeta exe_timestamp 0x%s\nmeta exe_size_of_image 0x%s\n",
                 kReaderVersion, o_.scenario.c_str(), hex8(g_.timestamp).c_str(), hex8(g_.size_of_image).c_str());
    for (const auto& m : o_.meta) std::fprintf(f_, "meta %s %s\n", m.first.c_str(), m.second.c_str());
    std::fprintf(f_, "meta game_type %d\nmeta mode_type %d\n", get<int32_t>(gb, q2::kGmmGameType), mtype0);
    if (galactic) std::fprintf(f_, "meta target_cycles %u\nmeta day_frames %u\n", o_.days, o_.day_frames);
    else std::fprintf(f_, "meta target_ticks %u\n", o_.ticks);
    std::fprintf(f_, "meta key_every %u\n", o_.key_every);
    std::fprintf(f_, "seed %s %s %u\n", hex8(seed_).c_str(), hex8(get<uint32_t>(gb, q2::kGmmAltSeed)).c_str(),
                 get<uint8_t>(gb, q2::kGmmUseAlt));
    if (load_seen) std::fprintf(f_, "load %u %s\n", load_frame, hex8(load_rng).c_str());
    find_players(false);
    dump_playerlist("session start");
    std::fprintf(f_, "end_header\n");
    if (galactic && !o_.queues.empty()) {
        q_ = std::fopen(o_.queues.c_str(), "wb");
        if (q_)
            std::fprintf(q_, "Q2QUEUES 1\n# Q <global_frame> <mode_frame> <planet_id> <queue 0=ground 1=space> <n> "
                             "[<type_index>:<player>:<start_frame>:<frames>:<T_f32>:<remain_f32>:<cost_f32>:<tag>:"
                             "<planet>:<created_id>]...  (GALACTIC_ECONOMY.md 2.1; type indices = trace T lines)\n");
    }
    // 3. Per-tick loop.
    state_ = "tracing";
    uint32_t last_f = get<uint32_t>(fb, q2::kFsFrame);
    bool first = true, light_next = false, key_pending = false;
    double pause_since = 0;
    uint64_t sample_no = 0;
    std::vector<ObjRec> objs;
    std::vector<std::string> players;
    uint32_t mode_frame0 = 0;
    for (;;) {
        if (!g_.alive()) { reason_ = "game_exited"; return finish(12); }
        if (now_s() - t0_ > o_.timeout) { reason_ = "timeout"; return finish(13); }
        if (now_s() - last_status_ > 2.0) { write_status(false); last_status_ = now_s(); }
        uint32_t f = 0;
        if (!g_.rdv(fs + q2::kFsFrame, f)) { Sleep(1); continue; }
        if (!first && f == last_f) { Sleep(1); continue; }
        // Wait for the tick that advanced the frame to finish (in-tick byte cleared).
        uint32_t flags = 0;
        const double spin0 = now_s();
        for (;;) {
            uint8_t in_tick = 0;
            g_.rdv(fs + q2::kFsInTick, in_tick);
            if (in_tick == 0) break;
            if (now_s() - spin0 > 0.03) { flags |= 2u; break; }
            Sleep(0);
        }
        if (!g_.block(gmm, q2::kGmmBlock, gb)) { Sleep(1); continue; }
        const int32_t stack = get<int32_t>(gb, q2::kGmmStackCount);
        const int32_t mtype = get<int32_t>(gb, q2::kGmmModeType);
        const uint64_t cur_mode = get<uint64_t>(gb, q2::kGmmCurMode);
        const int32_t pause = get<int32_t>(gb, q2::kGmmPause);
        if (cur_mode != mode || stack != 1 || mtype != mtype0) {
            reason_ = "mode_changed";
            return finish(10);
        }
        uint32_t mframe = 0, rng = 0, last_cycle = 0, next_cycle = 0, cycle = 0;
        uint64_t gom = 0;
        g_.rdv(cur_mode + q2::kModeFrame, mframe);
        g_.rdv(cur_mode + q2::kModePrimary, gom);
        g_.rdv(g_.base + q2::kRvaRngSync, rng);
        if (galactic) {
            g_.rdv(cur_mode + q2::kGalLastCycle, last_cycle);
            g_.rdv(cur_mode + q2::kGalNextCycle, next_cycle);
            g_.rdv(cur_mode + q2::kGalCycleCount, cycle);
        }
        // A light sample (F + C lines only) follows an object read that took longer than the budget, so a
        // slow machine loses object detail on some ticks instead of losing whole ticks (RNG, frames).
        const bool light = light_next && !first;
        light_next = false;
        if (!light) {
            const double r0 = now_s();
            if (!read_objects(gom, objs, !layout_ok_)) {
                if (!error_.empty() && !layout_ok_) { reason_ = "layout_error"; return finish(4); }
                Sleep(1);
                continue;
            }
            if (!objs.empty()) layout_ok_ = true;
            const double ms = (now_s() - r0) * 1000.0;
            read_ms_sum_ += ms;
            read_ms_max_ = std::max(read_ms_max_, ms);
            light_next = ms > o_.object_budget_ms;
        } else {
            ++light_;
            flags |= 4u;
        }
        read_players(players);
        if (player_at_ == 0 && sample_no % 150 == 149) find_players(true);
        uint32_t f2 = 0;
        g_.rdv(fs + q2::kFsFrame, f2);
        if (f2 != f) { flags |= 1u; ++torn_; }
        unsigned char ring[q2::kFsRingEntry] = {};
        g_.rd(fs + q2::kFsRing + q2::kFsRingEntry * (f & 0xFFu), ring, sizeof ring);
        if (first) {
            start_cycle_ = cycle;
            start_mode_frame_ = mframe;
            mode_frame0 = mframe;
            std::fprintf(f_, "start %u %d %u %s\n", f, mtype, mframe, hex8(rng).c_str());
        } else if (f > last_f + 1) {
            missed_ += f - last_f - 1;
        }
        // F line.
        std::fprintf(f_, "F %u %d %u %s %d %d %u", f, mtype, mframe, hex8(rng).c_str(), pause, stack, flags);
        if (galactic) std::fprintf(f_, " %u %u %u", last_cycle, next_cycle, cycle);
        std::fputc('\n', f_);
        bool key = false;
        if (!light) {
        // Sums and reference digests.
        uint32_t n_live = 0, sum_cached = 0, sum_calc = 0;
        StateHasher ho;
        for (const auto& r : objs)
            if (!(r.flags & q2::kFlagDead)) ++n_live;
        ho.u32(n_live);
        for (const auto& r : objs) {
            if (r.flags & q2::kFlagDead) continue;
            sum_cached += r.crc_cached;
            sum_calc += r.crc_calc;
            ho.u32(static_cast<uint32_t>(r.id));
            ho.u32(static_cast<uint32_t>(r.owner));
            ho.u32(r.type_name_crc);
            for (int k = 0; k < 3; ++k) ho.u32(bits(r.pos[k]));
            for (int k = 0; k < 3; ++k) ho.u32(bits(r.fac[k]));
            ho.u32(bits(r.health));
        }
        StateHasher hr;
        hr.u32(rng);
        std::string ringhex;
        for (unsigned char c : ring) {
            char b[4];
            std::snprintf(b, sizeof b, "%02x", c);
            ringhex += b;
        }
        std::fprintf(f_, "S %u %u %s %s %s %s %s %s\n", f, n_live, hex8(sum_cached).c_str(), hex8(sum_calc).c_str(),
                     hex8(rng_crc(rng)).c_str(), hex16(hr.digest()).c_str(), hex16(ho.digest()).c_str(),
                     ringhex.c_str());
        // Objects: keyframe or delta.
        key = first || key_pending || ((mframe - mode_frame0) % o_.key_every) == 0;
        key_pending = false;
        if (key) std::fprintf(f_, "K %u\n", f);
        std::map<int32_t, bool> seen;
        for (const auto& r : objs) {
            seen[r.id] = true;
            char line[400];
            std::snprintf(line, sizeof line, "O %d %d %u %s %s %s %s %s %s %s %u %d %d %d %s %s", r.id, r.owner,
                          r.type_index, hex8(bits(r.pos[0])).c_str(), hex8(bits(r.pos[1])).c_str(),
                          hex8(bits(r.pos[2])).c_str(), hex8(bits(r.fac[0])).c_str(), hex8(bits(r.fac[1])).c_str(),
                          hex8(bits(r.fac[2])).c_str(), hex8(bits(r.health)).c_str(), r.flags, r.parent_mode,
                          r.container, r.attach, hex8(r.crc_cached).c_str(), hex8(r.crc_calc).c_str());
            auto& last = last_obj_[r.id];
            if (key || last != line) {
                std::fprintf(f_, "%s\n", line);
                last = line;
            }
            if (r.has_planet) {
                auto& lp = last_planet_[r.id];
                if (key || lp != r.planet_line) {
                    std::fprintf(f_, "%s\n", r.planet_line.c_str());
                    lp = r.planet_line;
                }
            }
        }
        for (auto it = last_obj_.begin(); it != last_obj_.end();) {
            if (!seen.count(it->first)) {
                std::fprintf(f_, "X %d\n", it->first);
                last_planet_.erase(it->first);
                it = last_obj_.erase(it);
            } else {
                ++it;
            }
        }
        objects_max_ = std::max<uint64_t>(objects_max_, objs.size());
        write_queues(f, mframe, objs, key);
        } else if (((mframe - mode_frame0) % o_.key_every) == 0) {
            key_pending = true;  // the keyframe moves to the next full sample
        }
        for (uint32_t i = 0; i < players.size(); ++i) {
            auto& lp = last_player_[i];
            if (key || lp != players[i]) {
                std::fprintf(f_, "%s\n", players[i].c_str());
                lp = players[i];
            }
        }
        if (pause != last_pause_ || first) std::fprintf(f_, "P %u %d\n", f, pause);
        ++frames_;
        ++sample_no;
        last_frame_ = f;
        last_f = f;
        cur_cycle_ = cycle;
        cur_mode_frame_ = mframe;
        first = false;
        // Stop conditions.
        if (galactic) {
            if (cycle - start_cycle_ >= o_.days) { reason_ = "cycles_reached"; return finish(0); }
            if (mframe - start_mode_frame_ >= (o_.days + 1) * o_.day_frames) { reason_ = "frames_reached"; return finish(0); }
            if (pause > 0) {
                if (pause_since == 0) pause_since = now_s();
                if (now_s() - pause_since > o_.pause_stop) { reason_ = "paused_too_long"; return finish(11); }
            } else {
                pause_since = 0;
            }
        } else if (mframe - start_mode_frame_ >= o_.ticks) {
            reason_ = "ticks_reached";
            return finish(0);
        }
        last_pause_ = pause;
        if (sample_no % 64 == 0) {
            std::fflush(f_);
            if (q_) std::fflush(q_);
        }
    }
}

int selftest() {
    // Fixed vectors shared with q2trace.py selftest (both must print the same numbers).
    StateHasher h;
    std::printf("hash_empty %s\n", hex16(h.digest()).c_str());
    h.u32(0x12345678u);
    std::printf("hash_u32 %s\n", hex16(h.digest()).c_str());
    StateHasher h2;
    for (uint32_t i = 0; i < 37; ++i) h2.u32(i * 2654435761u);
    h2.str("REBEL_X-WING_SQUADRON");
    std::printf("hash_mix %s\n", hex16(h2.digest()).c_str());
    const float pos[3] = {100.5f, -20.25f, 3.0f}, fac[3] = {0.0f, 1.0f, 0.0f};
    std::printf("object_crc %s %s\n", hex8(object_crc(pos, fac, 250.0f)).c_str(), hex8(object_crc(pos, fac, 0.0f)).c_str());
    std::printf("rng_crc %s\n", hex8(rng_crc(0xDEADBEEFu)).c_str());
    std::printf("name_crc %s\n", hex8(crc32_zlib("STAR_DESTROYER")).c_str());
    uint32_t s = 1;
    for (int i = 0; i < 1000; ++i) s = s * q2::kLcgMul + q2::kLcgAdd;
    std::printf("lcg_1000 %s\n", hex8(s).c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    init_crc_table();
    Options o;
    if (!parse_args(argc, argv, o)) {
        usage();
        return 1;
    }
    if (o.selftest) return selftest();
    timeBeginPeriod(1);
    // Our own process only: run the poller above normal so a busy machine delays it less (fewer missed
    // ticks). Not REALTIME/HIGH: the game must never be starved by its observer.
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    Game g;
    Tracer* tracer = nullptr;
    int code = 0;
    const double t0 = now_s();
    // Find the process.
    for (;;) {
        std::vector<DWORD> pids;
        if (o.pid) pids.push_back(o.pid);
        else pids = find_pids("StarWarsG.exe");
        for (DWORD pid : pids) {
            const DWORD access = PROCESS_VM_READ | PROCESS_QUERY_INFORMATION | SYNCHRONIZE |
                                 (o.close_game ? PROCESS_TERMINATE : 0);
            HANDLE h = OpenProcess(access, FALSE, pid);
            if (!h) continue;
            Game c;
            c.h = h;
            c.pid = pid;
            if (o.test_base) {
                c.base = o.test_base;
                c.path = "test-target";
            } else if (!module_of(pid, c.base, c.path) || !read_pe_identity(c)) {
                CloseHandle(h);
                continue;
            }
            if (!o.test_base && !o.allow_unknown_exe &&
                (c.timestamp != q2::kFocTimeDateStamp || c.size_of_image != q2::kFocSizeOfImage)) {
                // EaW's StarWarsG.exe or another build: not the FoC 1.121 exe the layout is for.
                CloseHandle(h);
                if (pids.size() == 1 && o.pid) {
                    std::fprintf(stderr, "pid %lu is not FoC StarWarsG.exe 1.121\n", static_cast<unsigned long>(pid));
                    return 3;
                }
                continue;
            }
            g = c;
            break;
        }
        if (g.h) break;
        if (now_s() - t0 > o.wait_process) {
            FILE* s = std::fopen(o.status.c_str(), "wb");
            if (s) {
                std::fprintf(s, "{\n  \"tool\": \"q2reader\",\n  \"state\": \"done\",\n  \"final\": true,\n"
                                "  \"reason\": \"no_process\",\n  \"error\": \"no FoC StarWarsG.exe 1.121 process found\"\n}\n");
                std::fclose(s);
            }
            std::fprintf(stderr, "no FoC StarWarsG.exe (1.121) process found\n");
            return 2;
        }
        Sleep(500);
    }
    tracer = new Tracer(o, g);
    code = tracer->run();
    delete tracer;
    CloseHandle(g.h);
    timeEndPeriod(1);
    return code;
}
