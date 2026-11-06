// Synthesises v1/v2/v3/encrypted-v3 archives in memory and checks the parser.
#include "alamo/meg/meg.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace alamo::meg;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __builtin_FILE(), __LINE__, #c); ++failures; } } while (0)

static void p32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }
static void p16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); }

struct F { std::string name, data; };

static std::vector<uint8_t> build(int ver, const std::vector<F>& files, bool encrypted = false) {
    // names table
    std::vector<uint8_t> names;
    for (auto& f : files) { p16(names, uint16_t(f.name.size())); names.insert(names.end(), f.name.begin(), f.name.end()); }
    size_t hdr = ver == 1 ? 8 : ver == 2 ? 20 : 24;
    size_t recsz = 20;
    uint32_t data_start = uint32_t(hdr + names.size() + files.size() * recsz);
    // records sorted by crc
    std::vector<size_t> order(files.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return name_crc(files[a].name) < name_crc(files[b].name); });
    std::vector<uint32_t> offs(files.size());
    uint32_t cur = data_start;
    for (size_t i = 0; i < files.size(); ++i) { offs[i] = cur; cur += uint32_t(files[i].data.size()); }
    std::vector<uint8_t> v;
    if (ver == 1) { p32(v, uint32_t(files.size())); p32(v, uint32_t(files.size())); }
    else {
        p32(v, encrypted ? 0x8FFFFFFFu : 0xFFFFFFFFu); p32(v, 0x3F7D70A4u); p32(v, data_start);
        p32(v, uint32_t(files.size())); p32(v, uint32_t(files.size()));
        if (ver == 3) p32(v, uint32_t(names.size()));
    }
    v.insert(v.end(), names.begin(), names.end());
    for (size_t r = 0; r < order.size(); ++r) {
        size_t i = order[r];
        if (ver == 3) p16(v, 0);
        p32(v, name_crc(files[i].name)); p32(v, uint32_t(r)); p32(v, uint32_t(files[i].data.size()));
        p32(v, offs[i]);
        if (ver == 3) p16(v, uint16_t(i)); else p32(v, uint32_t(i));
    }
    for (auto& f : files) v.insert(v.end(), f.data.begin(), f.data.end());
    return v;
}

int main() {
    CHECK(crc32("123456789", 9) == 0xCBF43926u);  // CRC-32/IEEE check value
    CHECK(glob_match("data\\xml\\*.XML", "DATA\\XML\\UNITS.XML"));
    CHECK(glob_match("*/scripts/*", "DATA\\SCRIPTS\\AI\\X.LUA"));
    CHECK(!glob_match("*.lua", "A.XML"));

    std::vector<F> files = {{"DATA\\XML\\A.XML", "<a/>"}, {"DATA\\SCRIPTS\\B.LUA", "-- b"}, {"DATA\\C.TXT", "ccc"}};
    for (int ver = 1; ver <= 3; ++ver) {
        std::string err;
        auto a = Archive::open(Source::from_memory(build(ver, files)), &err);
        CHECK(a != nullptr);
        if (!a) continue;
        CHECK(int(a->version()) == ver);
        CHECK(a->entries().size() == 3);
        for (auto& s : a->verify()) std::fprintf(stderr, "v%d: %s\n", ver, s.c_str());
        CHECK(a->verify().empty());
        const Entry* e = a->find("data/scripts/b.lua");
        CHECK(e != nullptr);
        std::vector<uint8_t> out;
        CHECK(e && a->read(*e, out) && std::string(out.begin(), out.end()) == "-- b");
        CHECK(a->find("DATA\\NOPE") == nullptr);
    }
    auto enc = Archive::open(Source::from_memory(build(3, files, true)));
    CHECK(enc && enc->encrypted() && enc->entries().empty());

    // Overlay: later mount shadows earlier.
    FileSystem fsys;
    fsys.mount(Archive::open(Source::from_memory(build(1, files))));
    fsys.mount(Archive::open(Source::from_memory(build(1, {{"DATA\\XML\\A.XML", "<patched/>"}}))));
    std::vector<uint8_t> out;
    CHECK(fsys.read("Data/XML/a.xml", out) && std::string(out.begin(), out.end()) == "<patched/>");
    CHECK(fsys.read("DATA\\C.TXT", out) && std::string(out.begin(), out.end()) == "ccc");

    std::printf(failures ? "selftest FAILED (%d)\n" : "selftest OK\n", failures);
    return failures ? 1 : 0;
}
