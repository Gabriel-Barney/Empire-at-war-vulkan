// BM1 benchmark scenario manifest (bench_scenario.hpp).
#include "alamo/sim/bench_scenario.hpp"

#include <algorithm>
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

}  // namespace

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
    std::string t = "ALAMO_BENCH " + std::to_string(format) + "\n";
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
    return t;
}

bool Scenario::parse(std::string_view text, Scenario& out, std::string* err) {
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
            if (n != kFormat) return bad("unsupported scenario format (this build reads format 1)");
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
        } else {
            return bad("unknown key");
        }
        if (e == text.size()) break;
    }
    if (!have_format || !have_name || !have_save || !have_fp || !have_ticks) {
        fail(err, "scenario: missing one of ALAMO_BENCH / name / save / fingerprint / ticks");
        return false;
    }
    return true;
}

bool Scenario::save_manifest(const std::string& path, std::string* err) const {
    std::ofstream f(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    const std::string t = to_text();
    if (!f || !f.write(t.data(), static_cast<std::streamsize>(t.size()))) {
        fail(err, "cannot write " + path);
        return false;
    }
    return true;
}

bool Scenario::load(const std::string& path, Scenario& out, std::string* err) {
    std::string text;
    if (!read_bytes(path, text)) {
        fail(err, "cannot read " + path);
        return false;
    }
    if (!parse(text, out, err)) {
        if (err) *err = path + ": " + *err;
        return false;
    }
    out.dir = std::filesystem::path(path).parent_path().string();
    return true;
}

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
