// BM1 benchmark scenario manifest (bench_scenario.hpp).
#include "alamo/sim/bench_scenario.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "alamo/core/hash.hpp"

namespace alamo::sim::bench {

namespace {

void fail(std::string* err, std::string msg) {
    if (err) *err = std::move(msg);
}

bool read_bytes(const std::string& path, std::string& out) {
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !f.bad();
}

bool parse_u64(std::string_view s, uint64_t& out) {
    if (s.empty() || s.size() > 20) return false;
    uint64_t v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return false;
        const auto d = static_cast<uint64_t>(c - '0');
        if (v > (UINT64_MAX - d) / 10) return false;
        v = v * 10 + d;
    }
    out = v;
    return true;
}

// Make arguments: separated by one space; an argument that is empty or holds a space, a quote or a backslash is
// written in double quotes with \" and \\ escapes.
std::string quote_arg(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \"\\\t") == std::string::npos) return a;
    std::string q = "\"";
    for (const char c : a) {
        if (c == '"' || c == '\\') q += '\\';
        q += c;
    }
    return q + "\"";
}

bool split_args(std::string_view s, std::vector<std::string>& out) {
    out.clear();
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == ' ' || s[i] == '\t') {
            ++i;
            continue;
        }
        std::string a;
        if (s[i] == '"') {
            ++i;
            bool closed = false;
            while (i < s.size()) {
                const char c = s[i++];
                if (c == '\\' && i < s.size()) {
                    a += s[i++];
                } else if (c == '"') {
                    closed = true;
                    break;
                } else {
                    a += c;
                }
            }
            if (!closed) return false;
        } else {
            while (i < s.size() && s[i] != ' ' && s[i] != '\t') a += s[i++];
        }
        out.push_back(std::move(a));
    }
    return true;
}

std::string file_line(const char* key, const ScenarioFile& f) {
    if (!f.present()) return std::string(key) + " -\n";
    return std::string(key) + " " + f.path + " " + std::to_string(f.bytes) + " " + hex64(f.digest) + "\n";
}

bool parse_file(std::string_view v, ScenarioFile& f) {
    f = {};
    if (v == "-") return true;
    std::vector<std::string> parts;
    if (!split_args(v, parts) || parts.size() != 3) return false;
    f.path = parts[0];
    return !f.path.empty() && parse_u64(parts[1], f.bytes) && parse_hex64(parts[2], f.digest);
}

// Floats: shortest round-trip text (std::to_chars), read back with std::from_chars - the same bits in every build.
std::string float_text(float v) {
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    return std::string(buf, r.ptr);
}

bool parse_float(std::string_view s, float& out) {
    if (s.empty()) return false;
    float v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc() || r.ptr != s.data() + s.size() || !std::isfinite(v)) return false;
    out = v;
    return true;
}

std::vector<std::string_view> words(std::string_view s) {
    std::vector<std::string_view> w;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        const size_t b = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t') ++i;
        if (i > b) w.push_back(s.substr(b, i - b));
    }
    return w;
}

std::string camera_line(const CameraKey& k) {
    std::string t = "camera " + std::to_string(k.tick);
    for (const float v : {k.target[0], k.target[1], k.target[2], k.yaw_deg, k.pitch_deg, k.distance}) t += " " + float_text(v);
    if (k.fov_deg != 0.0f) t += " " + float_text(k.fov_deg);
    return t + "\n";
}

bool parse_camera(std::string_view v, CameraKey& k) {
    const std::vector<std::string_view> w = words(v);
    if (w.size() != 7 && w.size() != 8) return false;
    k = {};
    if (!parse_u64(w[0], k.tick)) return false;
    float* dst[7] = {&k.target[0], &k.target[1], &k.target[2], &k.yaw_deg, &k.pitch_deg, &k.distance, &k.fov_deg};
    for (size_t i = 1; i < w.size(); ++i)
        if (!parse_float(w[i], *dst[i - 1])) return false;
    return k.distance > 0.0f && k.pitch_deg >= -89.0f && k.pitch_deg <= 89.0f && k.fov_deg >= 0.0f && k.fov_deg < 180.0f;
}

bool parse_impl(std::string_view text, Scenario& out, std::string* err, bool recipe);

}  // namespace

const char* host_name(Host h) { return h == Host::Game ? "game" : "headless"; }

double camera_time(uint64_t ticks_run, float alpha) {
    if (ticks_run == 0) return 0.0;
    const double a = std::clamp(static_cast<double>(alpha), 0.0, 1.0);
    return static_cast<double>(ticks_run - 1) + a;
}

CameraKey camera_pose(const std::vector<CameraKey>& keys, double u) {
    if (keys.empty()) return {};
    if (u <= static_cast<double>(keys.front().tick)) return keys.front();
    if (u >= static_cast<double>(keys.back().tick)) return keys.back();
    size_t i = 1;
    while (i + 1 < keys.size() && static_cast<double>(keys[i].tick) <= u) ++i;
    const CameraKey& a = keys[i - 1];
    const CameraKey& b = keys[i];
    const double t = (u - static_cast<double>(a.tick)) / static_cast<double>(b.tick - a.tick);
    const auto lerp = [t](float x, float y) {
        const double d = static_cast<double>(y) - static_cast<double>(x);
        const double m = d * t;  // two separate operations (and -ffp-contract=off / fp:precise): no FMA
        return static_cast<float>(static_cast<double>(x) + m);
    };
    CameraKey p;
    p.tick = a.tick;
    for (int j = 0; j < 3; ++j) p.target[j] = lerp(a.target[j], b.target[j]);
    p.yaw_deg = lerp(a.yaw_deg, b.yaw_deg);
    p.pitch_deg = lerp(a.pitch_deg, b.pitch_deg);
    p.distance = lerp(a.distance, b.distance);
    p.fov_deg = a.fov_deg > 0.0f && b.fov_deg > 0.0f ? lerp(a.fov_deg, b.fov_deg) : a.fov_deg;
    return p;
}

std::string hex64(uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

bool parse_hex64(std::string_view s, uint64_t& out) {
    if (s.empty() || s.size() > 16) return false;
    uint64_t v = 0;
    for (const char c : s) {
        uint64_t d = 0;
        if (c >= '0' && c <= '9') d = static_cast<uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<uint64_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = static_cast<uint64_t>(c - 'A' + 10);
        else return false;
        v = (v << 4) | d;
    }
    out = v;
    return true;
}

std::string Scenario::resolve(const ScenarioFile& f) const {
    if (!f.present()) return {};
    return (std::filesystem::path(dir.empty() ? std::string(".") : dir) / f.path).string();
}

std::string Scenario::to_text() const {
    const bool v2 = has_v2();
    std::string t = "ALAMO_BENCH " + std::to_string(v2 ? kFormat : kFormatV1) + "\n";
    t += "name " + name + "\n";
    t += "version " + std::to_string(version) + "\n";
    if (!description.empty()) t += "description " + description + "\n";
    if (!make_args.empty()) {
        t += "make";
        for (const std::string& a : make_args) t += " " + quote_arg(a);
        t += "\n";
    }
    t += file_line("save", save);
    t += file_line("orders", orders);
    t += file_line("fingerprint", fingerprint);
    t += "fingerprint_every " + std::to_string(fingerprint_every) + "\n";
    t += "start_tick " + std::to_string(start_tick) + "\n";
    t += "ticks " + std::to_string(ticks) + "\n";
    t += "workers " + (workers < 0 ? std::string("auto") : std::to_string(workers)) + "\n";
    t += "final_crc " + hex64(final_crc) + "\n";
    if (!v2) return t;
    t += std::string("host ") + host_name(host) + "\n";
    if (!mode.empty()) t += "mode " + mode + "\n";
    if (!preset.empty()) t += "preset " + preset + "\n";
    if (frames_per_tick) t += "frames_per_tick " + std::to_string(frames_per_tick) + "\n";
    if (camera_anchor != CameraAnchor::World) t += "camera_anchor start\n";
    for (const CameraKey& k : camera) t += camera_line(k);
    return t;
}

bool Scenario::parse(std::string_view text, Scenario& out, std::string* err) { return parse_impl(text, out, err, false); }
bool Scenario::parse_recipe(std::string_view text, Scenario& out, std::string* err) { return parse_impl(text, out, err, true); }

namespace {

bool parse_impl(std::string_view text, Scenario& out, std::string* err, bool recipe) {
    out = {};
    bool have_format = false, have_name = false, have_save = false, have_fp = false, have_ticks = false;
    size_t line_no = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t e = std::min(text.find('\n', pos), text.size());
        std::string_view line = text.substr(pos, e - pos);
        pos = e + 1;
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line.front() == '#') {
            if (e == text.size()) break;
            continue;
        }
        const size_t sp = line.find(' ');
        const std::string_view key = line.substr(0, sp);
        const std::string_view val = sp == std::string_view::npos ? std::string_view() : line.substr(sp + 1);
        auto bad = [&](const char* what) {
            fail(err, "scenario line " + std::to_string(line_no) + ": " + what);
            return false;
        };
        uint64_t n = 0;
        if (!have_format) {
            if (key != "ALAMO_BENCH" || !parse_u64(val, n)) return bad("expected 'ALAMO_BENCH <format>' first");
            if (n != kFormat && n != kFormatV1) return bad("unsupported scenario format (this build reads formats 1 and 2)");
            out.format = static_cast<uint32_t>(n);
            have_format = true;
        } else if (key == "name") {
            if (val.empty() || val.find_first_of(" \t") != std::string_view::npos) return bad("name: one word");
            out.name = std::string(val);
            have_name = true;
        } else if (key == "version") {
            if (!parse_u64(val, n) || n == 0 || n > UINT32_MAX) return bad("version: a positive integer");
            out.version = static_cast<uint32_t>(n);
        } else if (key == "description") {
            out.description = std::string(val);
        } else if (key == "make") {
            if (!split_args(val, out.make_args)) return bad("make: unclosed quote");
        } else if (key == "save") {
            if (!parse_file(val, out.save) || !out.save.present()) return bad("save: <file> <bytes> <digest>");
            have_save = true;
        } else if (key == "orders") {
            if (!parse_file(val, out.orders)) return bad("orders: <file> <bytes> <digest> or '-'");
        } else if (key == "fingerprint") {
            if (!parse_file(val, out.fingerprint) || !out.fingerprint.present()) return bad("fingerprint: <file> <bytes> <digest>");
            have_fp = true;
        } else if (key == "fingerprint_every") {
            if (!parse_u64(val, n) || n == 0 || n > UINT32_MAX) return bad("fingerprint_every: a positive integer");
            out.fingerprint_every = static_cast<uint32_t>(n);
        } else if (key == "start_tick") {
            if (!parse_u64(val, out.start_tick)) return bad("start_tick: an integer");
        } else if (key == "ticks") {
            if (!parse_u64(val, out.ticks) || out.ticks == 0) return bad("ticks: a positive integer");
            have_ticks = true;
        } else if (key == "workers") {
            if (val == "auto") out.workers = -1;
            else if (parse_u64(val, n) && n <= 1024) out.workers = static_cast<int32_t>(n);
            else return bad("workers: an integer or 'auto'");
        } else if (key == "final_crc") {
            if (!parse_hex64(val, out.final_crc)) return bad("final_crc: 16 hex digits");
        } else if (key == "host" || key == "mode" || key == "preset" || key == "frames_per_tick" || key == "camera_anchor" || key == "camera") {
            if (out.format < kFormat) return bad("a format-2 key in a format-1 manifest (write 'ALAMO_BENCH 2')");
            if (key == "host") {
                if (val == "headless") out.host = Host::Headless;
                else if (val == "game") out.host = Host::Game;
                else return bad("host: headless or game");
            } else if (key == "mode") {
                if (val != "realtime" && val != "fixed") return bad("mode: realtime or fixed");
                out.mode = std::string(val);
            } else if (key == "preset") {
                if (val.empty() || val.find_first_of(" \t") != std::string_view::npos) return bad("preset: one word");
                out.preset = std::string(val);
            } else if (key == "frames_per_tick") {
                if (!parse_u64(val, n) || n == 0 || n > 64) return bad("frames_per_tick: 1..64");
                out.frames_per_tick = static_cast<uint32_t>(n);
            } else if (key == "camera_anchor") {
                if (val == "world") out.camera_anchor = CameraAnchor::World;
                else if (val == "start") out.camera_anchor = CameraAnchor::Start;
                else return bad("camera_anchor: world or start");
            } else {
                CameraKey k;
                if (!parse_camera(val, k)) return bad("camera: <tick> <tx> <ty> <tz> <yaw> <pitch -89..89> <distance > 0> [<fov 0..180>]");
                if (!out.camera.empty() && k.tick <= out.camera.back().tick) return bad("camera: key ticks must increase");
                out.camera.push_back(k);
            }
        } else {
            return bad("unknown key");
        }
        if (e == text.size()) break;
    }
    if (!have_format || !have_name || (!recipe && (!have_save || !have_fp || !have_ticks))) {
        fail(err, recipe ? "scenario: missing ALAMO_BENCH / name" : "scenario: missing one of ALAMO_BENCH / name / save / fingerprint / ticks");
        return false;
    }
    return true;
}

bool load_impl(const std::string& path, Scenario& out, std::string* err, bool recipe) {
    std::string text;
    if (!read_bytes(path, text)) {
        fail(err, "cannot read " + path);
        return false;
    }
    if (!parse_impl(text, out, err, recipe)) {
        if (err) *err = path + ": " + *err;
        return false;
    }
    out.dir = std::filesystem::path(path).parent_path().string();
    return true;
}

}  // namespace

bool Scenario::save_manifest(const std::string& path, std::string* err) const {
    std::ofstream f(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    const std::string t = to_text();
    if (!f || !f.write(t.data(), static_cast<std::streamsize>(t.size()))) {
        fail(err, "cannot write " + path);
        return false;
    }
    return true;
}

bool Scenario::load(const std::string& path, Scenario& out, std::string* err) { return load_impl(path, out, err, false); }
bool Scenario::load_recipe(const std::string& path, Scenario& out, std::string* err) { return load_impl(path, out, err, true); }

bool pin_file(const std::string& path, ScenarioFile& f, std::string* err) {
    std::string bytes;
    if (!read_bytes(path, bytes)) {
        fail(err, "cannot read " + path);
        return false;
    }
    f.bytes = bytes.size();
    f.digest = fnv1a64(bytes.data(), bytes.size());
    return true;
}

bool verify_files(const Scenario& s, std::string* err) {
    for (const ScenarioFile* f : {&s.save, &s.orders, &s.fingerprint}) {
        if (!f->present()) continue;
        ScenarioFile now = *f;
        const std::string p = s.resolve(*f);
        if (!pin_file(p, now, err)) return false;
        if (now.bytes != f->bytes || now.digest != f->digest) {
            fail(err, f->path + " differs from the manifest (" + std::to_string(now.bytes) + " bytes, digest " + hex64(now.digest) +
                          "; manifest: " + std::to_string(f->bytes) + " bytes, " + hex64(f->digest) + ")");
            return false;
        }
    }
    return true;
}

}  // namespace alamo::sim::bench
