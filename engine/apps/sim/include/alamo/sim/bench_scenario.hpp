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
// Format 2 (BM2, 2026-11-06) adds the rendered run's keys; a manifest without them is still written as format 1, byte for
// byte as before, and format 1 manifests read unchanged. The keys follow final_crc, in this order:
//   host <headless | game>                whose session the save is: an alamo_headless --save-session harness (default) or
//                                         an alamo_game save (game.campaign / game.skirmish). Each program runs only its own.
//   mode <realtime | fixed>               alamo_game's benchmark mode (default fixed)
//   preset <name>                         alamo_game's benchmark preset (window size, video options; default bm-high)
//   frames_per_tick <K>                   fixed-frames mode: counted frames per tick (default 2)
//   camera_anchor <world | start>         camera key targets in world units, or relative to the drawn view's own camera
//                                         target at the first gameplay frame of the run (default world)
//   camera <tick> <tx> <ty> <tz> <yaw> <pitch> <distance> [<fov>]
//                                         one key of the scripted camera (any number, ticks strictly increasing): at
//                                         `tick` ticks after the measurement started the camera looks at target t from
//                                         `distance` world units, yaw degrees about +Z (0 = camera on +X; not wrapped, so
//                                         keys can turn several times), pitch degrees above the XY plane (-89 .. 89),
//                                         horizontal field of view `fov` degrees (0 or left out = the view's own). Between
//                                         keys every value is linear; before the first and after the last key it holds.
// A "recipe" is a manifest with only name / description / format-2 keys (no pins yet): alamo_game --bench-make S reads it
// as the run's settings and camera, then writes the full scenario over it (parse_recipe).
//
// Determinism: nothing here is read by the sim; the benchmark drivers (alamo_headless --bench, alamo_game --benchmark) read it.
// The camera is presentation only: camera_pose() is a function of the tick count and the render alpha, never of wall time.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace alamo::sim::bench {

inline constexpr uint32_t kFormat = 2;    // the newest format this build reads and writes
inline constexpr uint32_t kFormatV1 = 1;  // BM1's: still read, and written when no format-2 key is set
inline constexpr const char* kExtension = ".abench";

enum class Host : uint8_t { Headless, Game };
const char* host_name(Host h);  // "headless" / "game"

enum class CameraAnchor : uint8_t { World, Start };

// One scripted camera key (format 2 "camera" line); also the pose camera_pose() returns.
struct CameraKey {
    uint64_t tick = 0;
    float target[3] = {0, 0, 0};
    float yaw_deg = 0, pitch_deg = 30, distance = 1000;
    float fov_deg = 0;  // 0 = the view's own
    friend bool operator==(const CameraKey&, const CameraKey&) = default;
};

// The camera time of a frame: ticks run since the measurement started, minus one, plus the frame's render alpha (the
// frame interpolates between the previous tick and the last one); 0 before the first tick. Never wall-clock time.
double camera_time(uint64_t ticks_run, float alpha);
// The pose at camera time u (keys non-empty, ticks increasing): linear between the two keys around u, held outside them.
// The fov is linear only when both keys give one, else the earlier key's. Same inputs, same bits (no FMA, double math).
CameraKey camera_pose(const std::vector<CameraKey>& keys, double u);

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
    // Format 2 (alamo_game's rendered runs).
    Host host = Host::Headless;
    std::string mode;              // "" (= fixed) / "realtime" / "fixed"
    std::string preset;            // "" = alamo_game's default preset
    uint32_t frames_per_tick = 0;  // 0 = alamo_game's default
    CameraAnchor camera_anchor = CameraAnchor::World;
    std::vector<CameraKey> camera;

    std::string dir;  // folder of the manifest (set by load(); not written)

    // Any format-2 key set (then to_text() writes format 2).
    bool has_v2() const noexcept {
        return host != Host::Headless || !mode.empty() || !preset.empty() || frames_per_tick != 0 || camera_anchor != CameraAnchor::World ||
               !camera.empty();
    }
    // Copies the format-2 run settings and camera of `o` (a remake keeps them).
    void copy_run_settings(const Scenario& o) {
        host = o.host;
        mode = o.mode;
        preset = o.preset;
        frames_per_tick = o.frames_per_tick;
        camera_anchor = o.camera_anchor;
        camera = o.camera;
    }

    // `f` resolved against dir ("" when f is absent).
    std::string resolve(const ScenarioFile& f) const;
    // Same content files (save / orders / fingerprint pins) as `o`: then a regenerate keeps the version.
    bool same_content(const Scenario& o) const noexcept {
        return save == o.save && orders == o.orders && fingerprint == o.fingerprint && fingerprint_every == o.fingerprint_every &&
               start_tick == o.start_tick && ticks == o.ticks && host == o.host && mode == o.mode && preset == o.preset &&
               frames_per_tick == o.frames_per_tick && camera_anchor == o.camera_anchor && camera == o.camera;
    }

    std::string to_text() const;
    static bool parse(std::string_view text, Scenario& out, std::string* err = nullptr);
    // As parse, but save / fingerprint / ticks may be missing (a recipe, or a full manifest).
    static bool parse_recipe(std::string_view text, Scenario& out, std::string* err = nullptr);
    bool save_manifest(const std::string& path, std::string* err = nullptr) const;
    static bool load(const std::string& path, Scenario& out, std::string* err = nullptr);
    static bool load_recipe(const std::string& path, Scenario& out, std::string* err = nullptr);
};

// Reads `path` and fills bytes / digest (path itself is left as is). False + *err when the file cannot be read.
bool pin_file(const std::string& path, ScenarioFile& f, std::string* err = nullptr);
// Checks every file the manifest names against its pin. False + *err naming the first file that is missing or differs.
bool verify_files(const Scenario& s, std::string* err = nullptr);

std::string hex64(uint64_t v);
bool parse_hex64(std::string_view s, uint64_t& out);

}  // namespace alamo::sim::bench
