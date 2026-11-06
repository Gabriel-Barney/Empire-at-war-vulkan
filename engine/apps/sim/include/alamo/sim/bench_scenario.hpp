// BM1 benchmark scenario file (docs/PHASE4.md "BM - repeatable benchmark"): one fixed piece of game time that every
// benchmark run replays exactly, so two runs measure the same work and the per-tick CRCs prove that they did.
//
// A scenario is a small text manifest (*.abench) next to three files it names:
//   save         a N1 save (SimHost::save, alamo/core/save_file.hpp) the run starts from;
//   orders       the recorded order stream that continues that save (commands/recording.hpp, from_save set; may hold no
//                commands: AI actions are not in the stream, the AI reruns identically from the save);
//   fingerprint  the expected CRC log of the run from the save tick on (alamo/core/sim_crc.hpp; a full per-tick log, or
//                a checkpoint log when the scenario was made with fingerprint_every > 1).
// The manifest pins each file by size and FNV-1a 64 digest, so a sidecar file changed without regenerating the
// scenario is caught before any timing is taken.
//
// Manifest text (LF, one "key value" per line, '#' comments; written byte-identically by every build):
//   ALAMO_BENCH 1                         format line (kFormat); a reader refuses other formats
//   name <identifier>                     no whitespace
//   version <N>                           scenario version: bumped by a regenerate whose files differ; results of two
//                                         versions are never compared
//   description <free text>
//   make <arg> <arg> ...                  the alamo_headless arguments that regenerate it (--bench-regen re-runs them)
//   save <file> <bytes> <digest:16 hex>
//   orders <file> <bytes> <digest>        "orders -" = no stream
//   fingerprint <file> <bytes> <digest>
//   fingerprint_every <N>                 1 = every tick stored; N > 1 = checkpoint log (ALAMO_CRC 2)
//   start_tick <T>                        the save's tick (the fingerprint's first record)
//   ticks <K>                             ticks a run plays after the load
//   workers <W | auto>                    default job-system workers (auto = machine default)
//   final_crc <16 hex>                    the combined CRC after the last tick (quick check / display)
// File names are relative to the manifest's folder.
//
// Determinism: nothing here is read by the sim; the benchmark driver (alamo_headless --bench, later alamo_game) reads it.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace alamo::sim::bench {

inline constexpr uint32_t kFormat = 1;
inline constexpr const char* kExtension = ".abench";

struct ScenarioFile {
    std::string path;     // relative to the manifest folder; empty = none
    uint64_t bytes = 0;
    uint64_t digest = 0;  // fnv1a64 of the file bytes
    bool present() const noexcept { return !path.empty(); }
    friend bool operator==(const ScenarioFile&, const ScenarioFile&) = default;
};

struct Scenario {
    uint32_t format = kFormat;
    std::string name;
    uint32_t version = 1;
    std::string description;
    std::vector<std::string> make_args;
    ScenarioFile save, orders, fingerprint;
    uint32_t fingerprint_every = 1;
    uint64_t start_tick = 0;
    uint64_t ticks = 0;
    int32_t workers = -1;  // < 0: machine default ("auto")
    uint64_t final_crc = 0;

    std::string dir;  // folder of the manifest (set by load(); not written)

    // `f` resolved against dir ("" when f is absent).
    std::string resolve(const ScenarioFile& f) const;
    // Same content files (save / orders / fingerprint pins) as `o`: then a regenerate keeps the version.
    bool same_content(const Scenario& o) const noexcept {
        return save == o.save && orders == o.orders && fingerprint == o.fingerprint && fingerprint_every == o.fingerprint_every &&
               start_tick == o.start_tick && ticks == o.ticks;
    }

    std::string to_text() const;
    static bool parse(std::string_view text, Scenario& out, std::string* err = nullptr);
    bool save_manifest(const std::string& path, std::string* err = nullptr) const;
    static bool load(const std::string& path, Scenario& out, std::string* err = nullptr);
};

// Reads `path` and fills bytes / digest (path itself is left as is). False + *err when the file cannot be read.
bool pin_file(const std::string& path, ScenarioFile& f, std::string* err = nullptr);
// Checks every file the manifest names against its pin. False + *err naming the first file that is missing or differs.
bool verify_files(const Scenario& s, std::string* err = nullptr);

std::string hex64(uint64_t v);
bool parse_hex64(std::string_view s, uint64_t& out);

}  // namespace alamo::sim::bench
