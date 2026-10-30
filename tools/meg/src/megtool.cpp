// megtool - list / extract / verify Alamo MEG archives.
//   megtool list    <file.meg> [--crc] [--stream]
//   megtool extract <file.meg> <outdir> [--filter GLOB]... [--stream]
//   megtool verify  <file.meg>
//   megtool info    <file.meg>
// Output of `list` is byte-identical to `megtool.py list`.
#include "alamo/meg/meg.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace alamo::meg;

static int usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  megtool list    <file.meg> [--crc] [--stream]\n"
                 "  megtool extract <file.meg> <outdir> [--filter GLOB]... [--stream]\n"
                 "  megtool verify  <file.meg>\n"
                 "  megtool info    <file.meg>\n");
    return 2;
}

// Split an archive name into components, dropping empty / '.' / '..' ones so it can't escape outdir.
static std::vector<std::string> safe_parts(const std::string& name) {
    std::vector<std::string> parts;
    std::string part;
    auto flush = [&] {
        if (!part.empty() && part != "." && part != "..") parts.push_back(part);
        part.clear();
    };
    for (char c : name) {
        if (c == '\\' || c == '/') flush();
        else part.push_back(c);
    }
    flush();
    return parts;
}

// Windows: absolute, normalised path with the \\?\ (or \\?\UNC\) prefix so output paths longer
// than MAX_PATH work. Elsewhere: unchanged.
static fs::path long_path(const fs::path& p) {
    std::error_code ec;
    fs::path a = fs::absolute(p, ec);
    if (ec) return p;
    a = a.lexically_normal();
    a.make_preferred();
    std::wstring s = a.wstring();
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();  // "C:\out\" -> "C:\out"
    if (s.rfind(L"\\\\?\\", 0) == 0) return fs::path(s);
    if (s.rfind(L"\\\\", 0) == 0) return fs::path(L"\\\\?\\UNC\\" + s.substr(2));
    return fs::path(L"\\\\?\\" + s);
}

// Create `dir` (whose parent exists). An existing directory is fine.
static bool make_dir(const fs::path& dir, std::string& err) {
    std::error_code ec;
    fs::create_directory(dir, ec);
    if (!ec) return true;
    std::error_code ec2;
    if (fs::is_directory(dir, ec2)) return true;
    err = ec.message();
    return false;
}

static int extract(Archive& a, const std::string& outdir, const std::vector<std::string>& filters) {
    std::error_code ec;
    const fs::path root = long_path(outdir);
    fs::create_directories(root, ec);
    if (!fs::is_directory(root, ec)) fs::create_directories(fs::path(outdir), ec);  // STL without \\?\ support
    if (!fs::is_directory(root, ec)) {
        std::fprintf(stderr, "megtool: cannot create output directory %s\n", outdir.c_str());
        return 1;
    }
    size_t n = 0, failed = 0;
    std::vector<uint8_t> buf;
    for (auto& e : a.entries()) {
        if (!filters.empty()) {
            bool ok = false;
            for (auto& f : filters) ok = ok || glob_match(f, e.name);
            if (!ok) continue;
        }
        if (!a.read(e, buf)) {
            std::fprintf(stderr, "megtool: failed to read %s\n", e.name.c_str());
            ++failed;
            continue;
        }
        const auto parts = safe_parts(e.name);
        if (parts.empty()) {
            std::fprintf(stderr, "megtool: skipping entry with an empty name\n");
            ++failed;
            continue;
        }
        // Directories one component at a time below the (prefixed) root.
        fs::path p = root;
        std::string err;
        bool ok = true;
        for (size_t i = 0; i + 1 < parts.size() && ok; ++i) {
            p /= parts[i];
            ok = make_dir(p, err);
        }
        if (ok) {
            p /= parts.back();
            std::ofstream o(p, std::ios::binary);
            if (!o) {
                ok = false;
                err = "cannot open for writing";
            } else {
                o.write(reinterpret_cast<const char*>(buf.data()), std::streamsize(buf.size()));
                o.close();
                if (!o) {
                    ok = false;
                    err = "write failed";
                }
            }
        }
        if (!ok) {
            std::fprintf(stderr, "megtool: cannot extract %s: %s\n", e.name.c_str(), err.c_str());
            ++failed;
            continue;
        }
        ++n;
    }
    std::fprintf(stderr, "extracted %zu files", n);
    if (failed) std::fprintf(stderr, ", %zu failed", failed);
    std::fprintf(stderr, "\n");
    return failed ? 1 : 0;
}

static int run(int argc, char** argv) {
    if (argc < 3) return usage();
    std::string cmd = argv[1], file = argv[2];
    bool crc = false, stream = false;
    std::vector<std::string> filters;
    std::string outdir;
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--crc")) crc = true;
        else if (!std::strcmp(argv[i], "--stream")) stream = true;
        else if (!std::strcmp(argv[i], "--filter") && i + 1 < argc) filters.emplace_back(argv[++i]);
        else if (outdir.empty()) outdir = argv[i];
        else return usage();
    }
    std::string err;
    auto a = Archive::open(file, stream ? Source::Mode::Stream : Source::Mode::Auto, &err);
    if (!a) {
        std::fprintf(stderr, "megtool: %s\n", err.c_str());
        return 1;
    }
    if (a->encrypted()) {
        std::fprintf(stderr, "megtool: %s is an encrypted v3 archive (id 0x8FFFFFFF); not supported\n",
                     file.c_str());
        return 3;
    }

    if (cmd == "info") {
        std::printf("version  v%d\nentries  %zu\n", int(a->version()), a->entries().size());
        uint64_t total = 0;
        for (auto& e : a->entries()) total += e.size;
        std::printf("payload  %llu bytes\n", (unsigned long long)total);
        return 0;
    }
    if (cmd == "list") {
        for (auto& e : a->entries()) {
            if (crc) std::printf("%08X %10u %10u %s\n", e.crc, e.offset, e.size, e.name.c_str());
            else std::printf("%10u %10u %s\n", e.offset, e.size, e.name.c_str());
        }
        return 0;
    }
    if (cmd == "verify") {
        auto p = a->verify();
        for (auto& s : p) std::printf("%s\n", s.c_str());
        std::printf("%s: v%d, %zu entries, %zu problems\n", file.c_str(), int(a->version()),
                    a->entries().size(), p.size());
        return p.empty() ? 0 : 1;
    }
    if (cmd == "extract") {
        if (outdir.empty()) return usage();
        return extract(*a, outdir, filters);
    }
    return usage();
}

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {  // e.g. filesystem_error, bad_alloc: report, never abort
        std::fprintf(stderr, "megtool: %s\n", e.what());
        return 1;
    }
}
