// alamo_core unit tests (no framework; returns non-zero on failure).
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "alamo/core/bytes.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/galaxy.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/core/parallel.hpp"
#include "alamo/core/test_support.hpp"
#include "alamo/core/text.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>

// Turns the empty directory `link` into a junction (IO_REPARSE_TAG_MOUNT_POINT) to `target` with
// FSCTL_SET_REPARSE_POINT. Junctions need no privilege (unlike symlinks).
static bool make_junction(const std::filesystem::path& link, const std::filesystem::path& target) {
    HANDLE h = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const std::wstring print = std::filesystem::absolute(target).native();
    const std::wstring subst = L"\\??\\" + print;
    // REPARSE_DATA_BUFFER (MountPointReparseBuffer): tag, length, reserved, 4 offsets/lengths, names.
    std::vector<unsigned char> buf(16 + (subst.size() + print.size() + 2) * sizeof(wchar_t), 0);
    auto put16 = [&](size_t off, size_t v) {
        buf[off] = static_cast<unsigned char>(v & 0xFF);
        buf[off + 1] = static_cast<unsigned char>((v >> 8) & 0xFF);
    };
    const DWORD tag = 0xA0000003u;  // IO_REPARSE_TAG_MOUNT_POINT
    std::memcpy(buf.data(), &tag, 4);
    put16(4, buf.size() - 8);                                      // ReparseDataLength
    put16(8, 0);                                                   // SubstituteNameOffset
    put16(10, subst.size() * sizeof(wchar_t));                     // SubstituteNameLength
    put16(12, (subst.size() + 1) * sizeof(wchar_t));               // PrintNameOffset
    put16(14, print.size() * sizeof(wchar_t));                     // PrintNameLength
    std::memcpy(buf.data() + 16, subst.c_str(), subst.size() * sizeof(wchar_t));
    std::memcpy(buf.data() + 16 + (subst.size() + 1) * sizeof(wchar_t), print.c_str(), print.size() * sizeof(wchar_t));
    DWORD ret = 0;
    const BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf.data(), static_cast<DWORD>(buf.size()), nullptr, 0,
                                    &ret, nullptr);
    CloseHandle(h);
    return ok != FALSE;
}

// remove_all_long safety on Windows: junctions are not followed, read-only entries go, a locked file
// is a bounded failure, and inputs Win32 would widen ("x.", "C:x", roots) are refused.
static void test_remove_all_long_safety(const std::filesystem::path& tmp) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path tree = tmp / "tree";
    const fs::path outside = tmp / "outside";  // the junction target: not under `tree`
    fs::create_directories(tree / "ro_dir", ec);
    fs::create_directories(outside / "inner", ec);
    { std::ofstream(outside / "keep.txt") << "keep"; }
    { std::ofstream(outside / "inner" / "keep2.txt") << "keep"; }
    { std::ofstream(tree / "ro_dir" / "ro.txt") << "ro"; }
    SetFileAttributesW((tree / "ro_dir" / "ro.txt").c_str(), FILE_ATTRIBUTE_READONLY);
    SetFileAttributesW((tree / "ro_dir").c_str(), FILE_ATTRIBUTE_READONLY);
    fs::create_directory(tree / "junction", ec);
    const bool junction = make_junction(tree / "junction", outside);
    if (junction) {
        CHECK(fs::exists(tree / "junction" / "keep.txt", ec));  // the junction resolves
    } else {
        std::printf("  (junction test skipped: FSCTL_SET_REPARSE_POINT failed, error %lu)\n", GetLastError());
    }
    const std::uintmax_t removed = alamo::remove_all_long(tree, ec);
    CHECK(!ec && removed == 4);  // tree, ro_dir, ro.txt, junction (the link itself)
    CHECK(!fs::exists(tree, ec));
    CHECK(fs::exists(outside / "keep.txt", ec) && fs::exists(outside / "inner" / "keep2.txt", ec));
    // A junction passed as the top-level path is removed as a link too.
    fs::create_directory(tmp / "top_junction", ec);
    if (make_junction(tmp / "top_junction", outside)) {
        CHECK(alamo::remove_all_long(tmp / "top_junction", ec) == 1 && !ec);
        CHECK(!fs::exists(tmp / "top_junction", ec) && fs::exists(outside / "keep.txt", ec));
    }

    // Locked file (no FILE_SHARE_DELETE): a bounded failure, not a hang; the tree stays consistent.
    const fs::path locked_dir = tmp / "locked";
    fs::create_directories(locked_dir, ec);
    HANDLE h = CreateFileW((locked_dir / "busy.bin").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(alamo::remove_all_long(locked_dir, ec) == static_cast<std::uintmax_t>(-1) && ec);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 5000);
    std::printf("  locked file: remove_all_long failed after %lld ms (%s)\n", static_cast<long long>(ms),
                ec.message().c_str());
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    CHECK(alamo::remove_all_long(locked_dir, ec) == 2 && !ec);  // released: now it goes

    // Refused inputs (each would otherwise widen to something else; all targets here are harmless).
    fs::create_directories(tmp / "victim", ec);
    CHECK(alamo::remove_all_long(tmp / "victim.", ec) == static_cast<std::uintmax_t>(-1) && ec);
    CHECK(alamo::remove_all_long(tmp / "victim ", ec) == static_cast<std::uintmax_t>(-1) && ec);
    CHECK(alamo::remove_all_long(tmp / "victim" / ".", ec) == static_cast<std::uintmax_t>(-1) && ec);
    CHECK(alamo::remove_all_long(tmp / "victim" / "x" / "..", ec) == static_cast<std::uintmax_t>(-1) && ec);
    CHECK(fs::exists(tmp / "victim", ec));
    {   // drive-relative "X:name": resolved against the drive's current directory
        const fs::path cwd = fs::current_path(ec);
        const std::wstring name = L"alamo_core_driverel_" + std::to_wstring(GetCurrentProcessId());
        fs::create_directory(cwd / name, ec);
        CHECK(alamo::remove_all_long(fs::path(cwd.root_name().native() + name), ec) == static_cast<std::uintmax_t>(-1) && ec);
        CHECK(fs::exists(cwd / name, ec));
        fs::remove(cwd / name, ec);
    }
    {   // a volume root (on an unused drive letter, so a broken guard could not delete anything)
        const DWORD drives = GetLogicalDrives();
        for (int i = 25; i >= 3; --i)
            if (!(drives & (1u << i))) {
                const std::wstring root = std::wstring(1, static_cast<wchar_t>(L'A' + i)) + L":\\";
                CHECK(alamo::remove_all_long(fs::path(root), ec) == static_cast<std::uintmax_t>(-1) && ec);
                CHECK(alamo::remove_all_long(fs::path(L"\\\\?\\" + root), ec) == static_cast<std::uintmax_t>(-1) && ec);
                break;
            }
    }
    CHECK(alamo::remove_all_long(fs::path(L"\\\\.\\pipe\\x"), ec) == static_cast<std::uintmax_t>(-1) && ec);
    CHECK(alamo::remove_all_long(tmp / "victim", ec) == 1 && !ec);
    CHECK(alamo::remove_all_long(outside, ec) == 4 && !ec);  // outside, inner, keep.txt, keep2.txt
}

int main() {
    // CRC-32/IEEE check value.
    CHECK(alamo::crc32("123456789", 9) == 0xCBF43926u);
    CHECK(alamo::name_crc("data/xml/planets.xml") == alamo::name_crc("DATA\\XML\\PLANETS.XML"));
    CHECK(alamo::iequals("Data/Xml", "DATA\\XML"));
    CHECK(!alamo::iequals("abc", "abd"));
    CHECK(alamo::normalize_path(".\\Data//XML\\x.xml") == "DATA\\XML\\X.XML");

    // DirectoryProvider: case-insensitive.
    namespace fs = std::filesystem;
    // Unique per run, so concurrent ctest runs (e.g. MSVC + clang at once) don't share the folder.
    const fs::path tmp = fs::temp_directory_path() /
                         ("alamo_core_test_" + std::to_string(std::random_device{}()) + "_" +
                          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code tec;
    alamo::remove_all_long(tmp, tec);
    fs::create_directories(tmp / "Data" / "XML");
    { std::ofstream(tmp / "Data" / "XML" / "PLANETS.XML") << "<Planets/>"; }
    alamo::DirectoryProvider dp(alamo::path_to_utf8(tmp));
    std::vector<uint8_t> buf;
    CHECK(dp.exists("data/xml/planets.xml"));
    CHECK(dp.read(".\\Data\\XML\\Planets.xml", buf) && buf.size() == 10);
    CHECK(!dp.read("Data\\XML\\missing.xml", buf) && buf.empty());
    alamo::remove_all_long(tmp, tec);
    CHECK(!tec);

    // long_path: forms, then a write / rename / read round trip in a folder deeper than 300 characters.
    CHECK(alamo::long_path(fs::path()).empty());
    CHECK(alamo::long_path(fs::path(L"C:/a/./b/../c.txt")).native() == L"\\\\?\\C:\\a\\c.txt");
    CHECK(alamo::long_path(fs::path(L"\\\\srv\\share\\x/y")).native() == L"\\\\?\\UNC\\srv\\share\\x\\y");
    CHECK(alamo::long_path(fs::path(L"\\\\?\\C:\\x")).native() == L"\\\\?\\C:\\x");
    CHECK(alamo::long_path(fs::path(L"\\\\.\\pipe\\p")).native() == L"\\\\.\\pipe\\p");
    CHECK(alamo::long_path(fs::path(L"rel")).native().rfind(L"\\\\?\\", 0) == 0);
    // Generic forms of "\\?\C:\x\y" after a UTF-8 round trip (MSVC "//?/", libstdc++ "/?/").
    CHECK(alamo::long_path(fs::path(L"//?/C:/x/y")).native() == L"\\\\?\\C:\\x\\y");
    CHECK(alamo::long_path(fs::path(L"/?/C:/x/y")).native() == L"\\\\?\\C:\\x\\y");
    CHECK(alamo::long_path(alamo::path_from_utf8(alamo::path_to_utf8(fs::path(L"\\\\?\\C:\\x\\y")))).native() ==
          L"\\\\?\\C:\\x\\y");
    {
        fs::path deep = tmp / "long";
        while (deep.native().size() <= 320) deep /= "nested_directory_level_0123456789";
        std::error_code ec;
        CHECK(fs::create_directories(alamo::long_path(deep), ec) && !ec);
        const fs::path tmp_file = deep / "file.tmp";
        const fs::path file = deep / "file.bin";
        { std::ofstream(alamo::long_path(tmp_file), std::ios::binary) << "long path payload"; }
        fs::rename(alamo::long_path(tmp_file), alamo::long_path(file), ec);
        CHECK(!ec);
        CHECK(fs::exists(alamo::long_path(file), ec));
        CHECK(alamo::read_host_file(alamo::path_to_utf8(file), buf) && buf.size() == 17);
        CHECK(alamo::read_host_file(file, buf) && buf.size() == 17);                     // plain long path
        CHECK(alamo::read_host_file(alamo::long_path(file), buf) && buf.size() == 17);   // "\\?\" path
        CHECK(alamo::read_host_file(alamo::path_to_utf8(alamo::long_path(file)), buf) && buf.size() == 17);
        CHECK(!alamo::read_host_file(deep / "missing.bin", buf) && buf.empty());
        fs::create_directories(alamo::long_path(deep / "sub"), ec);
        const std::vector<fs::path> entries = alamo::list_directory(deep, ec);
        CHECK(!ec && entries.size() == 2);  // file.bin + sub (not the current directory, see list_directory)
        for (const fs::path& e : entries) CHECK(e == file || e == deep / "sub");
        alamo::list_directory(deep / "missing", ec);
        CHECK(ec);
        const std::uintmax_t removed = alamo::remove_all_long(tmp, ec);
        CHECK(!ec && removed >= 8);  // tmp, long, the nested levels, file.bin, sub
        CHECK(!fs::exists(alamo::long_path(tmp), ec));
        CHECK(alamo::remove_all_long(tmp, ec) == 0 && !ec);  // missing: 0, no error
    }
    fs::create_directories(tmp, tec);
    test_remove_all_long_safety(tmp);
    CHECK(alamo::remove_all_long(tmp, tec) == 1 && !tec);

    // Galaxy tables.
    alamo::GalaxyMap g;
    const auto a = g.planets.add("Coruscant");
    const auto b = g.planets.add("Corellia");
    CHECK(g.planets.find("CORUSCANT") == a);
    CHECK(g.planets.find("Nope") == alamo::kInvalidPlanet);
    const auto r = g.routes.add("Coruscant_Corellia");
    g.routes.point_a[r] = a;
    g.routes.point_b[r] = b;
    CHECK(g.validate().empty());
    g.routes.point_b[r] = a;
    CHECK(!g.validate().empty());

    // serial_for covers the range.
    uint32_t sum = 0;
    alamo::serial_for(10, 1, [&](uint32_t s, uint32_t e) { for (uint32_t i = s; i < e; ++i) sum += i; });
    CHECK(sum == 45);

    // text.hpp: locale-free ASCII helpers (no '/' folding, unlike iequals).
    CHECK(alamo::ascii_upper('a') == 'A' && alamo::ascii_upper('Z') == 'Z' && alamo::ascii_upper('1') == '1');
    CHECK(alamo::ascii_lower("MiXed_1") == "mixed_1" && alamo::ascii_upper("MiXed_1") == "MIXED_1");
    CHECK(alamo::ascii_iequals("Data", "DATA") && !alamo::ascii_iequals("a/b", "a\\b") && !alamo::ascii_iequals("ab", "abc"));
    CHECK(alamo::ascii_trim(" \t x y\r\n") == "x y" && alamo::ascii_trim("  ").empty() && alamo::ascii_trim("").empty());

    // bytes.hpp: read_count bounds, write_vec3 / read_vec3 round trip, read_file_bytes.
    {
        struct V3 {
            float x, y, z;
        };
        alamo::ByteWriter w;
        w.u32(3);
        alamo::write_vec3(w, V3{1.f, 2.f, 3.f});
        alamo::ByteReader rd(w.bytes());
        uint32_t n = 0;
        CHECK(alamo::read_count(rd, n, 4) && n == 3);  // 3 * 4 <= 12 bytes left
        const V3 v = alamo::read_vec3<V3>(rd);
        CHECK(v.x == 1.f && v.y == 2.f && v.z == 3.f && rd.ok() && rd.done());
        alamo::ByteReader rd2(w.bytes());
        CHECK(!alamo::read_count(rd2, n, 5));  // 3 * 5 > 12
        alamo::ByteReader rd3(std::span<const uint8_t>{});
        CHECK(!alamo::read_count(rd3, n, 0));  // short read clears ok()

        const fs::path f = fs::temp_directory_path() / ("alamo_core_bytes_" + std::to_string(std::random_device{}()) + ".bin");
        alamo::testing::write_file(f, std::string_view("a\0b", 3));
        std::vector<uint8_t> got;
        CHECK(alamo::read_file_bytes(f.string(), got) && got.size() == 3 && got[1] == 0);
        std::error_code ec;
        fs::remove(f, ec);
        std::string err;
        CHECK(!alamo::read_file_bytes(f.string(), got, &err) && !err.empty());
    }

    // test_support.hpp MemoryProvider: normalised keys, text and byte overloads.
    {
        alamo::testing::MemoryProvider mp;
        mp.add("Data/XML/A.xml", "<a/>");
        mp.add("b.bin", std::vector<uint8_t>{1, 2});
        CHECK(mp.exists("data\\xml\\a.XML") && mp.read("b.bin", buf) && buf.size() == 2);
        CHECK(!mp.read("c", buf) && buf.empty() && mp.origin("c").empty());
    }

    std::printf(g_fail ? "alamo_core_tests: %d failure(s)\n" : "alamo_core_tests: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
