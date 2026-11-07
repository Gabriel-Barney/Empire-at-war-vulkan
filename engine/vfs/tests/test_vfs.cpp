// alamo_vfs unit tests (no framework; returns non-zero on failure). Builds fake FoC/EaW/mod/
// fallback installs with small V1 MEGs in a temp dir and checks the verified resolution order.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/meg/meg.h"
#include "alamo/vfs/vfs.hpp"

namespace fs = std::filesystem;
using alamo::vfs::Vfs;
using alamo::vfs::VfsConfig;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

namespace {

using Files = std::vector<std::pair<std::string, std::string>>;  // name, content

void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

void put_u16(std::string& s, uint32_t v) {
    s.push_back(static_cast<char>(v & 0xFFu));
    s.push_back(static_cast<char>((v >> 8) & 0xFFu));
}
void put_u32(std::string& s, uint32_t v) {
    for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFFu));
}

// Retail-style MEG V1: filename table (insertion order), records sorted by CRC, contiguous data.
void write_meg(const fs::path& p, const Files& files) {
    const uint32_t n = static_cast<uint32_t>(files.size());
    std::string head;
    put_u32(head, n);
    put_u32(head, n);
    for (const auto& f : files) {
        put_u16(head, static_cast<uint32_t>(f.first.size()));
        head += f.first;
    }
    struct Rec { uint32_t crc, name_index; };
    std::vector<Rec> recs;
    for (uint32_t i = 0; i < n; ++i) recs.push_back({alamo::meg::name_crc(files[i].first), i});
    std::sort(recs.begin(), recs.end(), [](const Rec& a, const Rec& b) { return a.crc < b.crc; });
    uint32_t offset = static_cast<uint32_t>(head.size()) + 20u * n;
    std::string table, data;
    for (uint32_t i = 0; i < n; ++i) {
        const std::string& payload = files[recs[i].name_index].second;
        put_u32(table, recs[i].crc);
        put_u32(table, i);
        put_u32(table, static_cast<uint32_t>(payload.size()));
        put_u32(table, offset);
        put_u32(table, recs[i].name_index);
        offset += static_cast<uint32_t>(payload.size());
        data += payload;
    }
    write_file(p, head + table + data);
}

std::string read_str(const Vfs& v, std::string_view path) {
    std::vector<uint8_t> buf;
    if (!v.read(path, buf)) return "<missing>";
    return std::string(buf.begin(), buf.end());
}

std::string megafiles(const std::vector<std::string>& entries) {
    std::string s = "<?xml version=\"1.0\"?>\n<Mega_Files>\n  <!-- <File>Data\\Bogus.meg</File> -->\n";
    for (const auto& e : entries) s += "\t<File> " + e + " </File>\n";
    return s + "</Mega_Files>\n";
}

}  // namespace

int main() {
    const fs::path T = fs::temp_directory_path() / "alamo_vfs_test";
    fs::remove_all(T);
    const fs::path FOC = T / "corruption";
    const fs::path GD = T / "GameData";
    const fs::path MODA = FOC / "Mods" / "ModA";
    const fs::path MODB = FOC / "Mods" / "Mod B";
    const fs::path MEGMOD = FOC / "Mods" / "MegMod";
    const fs::path FB1 = T / "Fall Back 1";
    const fs::path FB2 = T / "FB2";
    const fs::path X = fs::path("Data") / "XML";

    write_file(GD / "sweaw.exe", "MZ");
    write_file(FOC / "StarWarsG.exe", "MZ");

    // --- precedence pairs (loose) ---
    write_file(MODA / X / "P_ModA_ModB.xml", "A");
    write_file(MODB / X / "P_ModA_ModB.xml", "B");
    write_file(MODB / X / "P_ModB_Base.xml", "B");
    write_file(FOC / X / "P_ModB_Base.xml", "BASE");
    write_file(FOC / X / "P_Base_Meg.xml", "BASE");
    write_file(MODA / X / "P_Mod_Meg.xml", "A");
    write_file(FB1 / X / "P_Meg_FB.xml", "FB1");
    write_file(FB1 / X / "P_EawMeg_FB.xml", "FB1");
    write_file(GD / X / "P_EawMeg_FB.xml", "GD");
    write_file(FOC / X / "P_Base_FB.xml", "BASE");
    write_file(FB1 / X / "P_Base_FB.xml", "FB1");
    write_file(FB1 / X / "P_FB1_FB2.xml", "FB1");
    write_file(FB2 / "data" / "xml" / "p_fb1_fb2.XML", "FB2");  // lower-case Data dir
    write_file(FB2 / "data" / "xml" / "FB2Only.xml", "FB2");
    write_file(FOC / "Data" / "xml" / "MixedCase.XML", "MIXED");
    // Probe E.
    write_file(GD / "Data" / "Scripts" / "Library" / "ProbeE1.lua", "E1-GAMEDATA");
    write_file(FB1 / "Data" / "Scripts" / "Library" / "ProbeE2.lua", "E2-FB");
    write_file(GD / "Data" / "Scripts" / "Library" / "ProbeE2.lua", "E2-GAMEDATA");
    // list() fixtures.
    const fs::path AI = fs::path("Data") / "Scripts" / "AI";
    write_file(MODA / AI / "A1.lua", "MOD-A1");
    write_file(FOC / AI / "a1.lua", "BASE-A1");
    write_file(FOC / AI / "Base.lua", "BASE");
    write_file(FOC / AI / "SpaceMode" / "Sub.lua", "SUB");
    write_file(FB1 / AI / "Fb.lua", "FB");

    // --- MEGs: priority (highest first) FoC 64Patch, patch2, Patch, Second, Config, EaW same ---
    const char* const kTiers[10] = {"FOC64", "FOCP2", "FOCP", "FOCSECOND", "FOCCONFIG",
                                    "EAW64", "EAWP2", "EAWP", "EAWSECOND", "EAWCONFIG"};
    Files megs[10];
    for (int k = 0; k < 10; ++k) {
        for (int j = k; j < 10; ++j) megs[j].push_back({"DATA\\XML\\ORDER" + std::to_string(k) + ".XML", kTiers[j]});
    }
    megs[4].push_back({"DATA\\XML\\P_BASE_MEG.XML", "MEG"});
    megs[0].push_back({"DATA\\XML\\P_MOD_MEG.XML", "MEG"});
    megs[4].push_back({"DATA\\XML\\P_MEG_FB.XML", "MEG"});
    megs[9].push_back({"DATA\\XML\\P_EAWMEG_FB.XML", "EAWMEG"});
    megs[4].push_back({"data/xml/lower_stored.xml", "LOWER"});  // Probe B: stored lower-case + '/'
    megs[4].push_back({"DATA\\SCRIPTS\\AI\\MEG1.LUA", "MEG1"});
    megs[4].push_back({"DATA\\SCRIPTS\\AI\\BASE.LUA", "MEGBASE"});
    megs[3].push_back({"DATA\\XML\\S2_SECOND.XML", "FOCSECOND"});
    megs[2].push_back({"DATA\\XML\\S2_PATCH.XML", "FOCPATCH"});
    megs[2].push_back({"DATA\\XML\\S2_BASEPATCHONLY.XML", "FOCPATCH"});
    write_meg(FOC / "Data" / "64Patch.meg", megs[0]);
    write_meg(FOC / "Data" / "patch2.meg", megs[1]);
    write_meg(FOC / "Data" / "Patch.meg", megs[2]);
    write_meg(FOC / "Data" / "Second.meg", megs[3]);
    write_meg(FOC / "Data" / "Config.meg", megs[4]);
    write_meg(GD / "Data" / "64Patch.meg", megs[5]);
    write_meg(GD / "Data" / "patch2.meg", megs[6]);
    write_meg(GD / "Data" / "Patch.meg", megs[7]);
    write_meg(GD / "Data" / "Second.meg", megs[8]);
    write_meg(GD / "Data" / "Config.meg", megs[9]);
    write_file(FOC / "Data" / "MegaFiles.xml", megafiles({"Data\\Config.meg", "Data\\Missing.meg", "Data/second.MEG", ".\\data\\CONFIG.MEG"}));
    write_file(GD / "Data" / "MegaFiles.xml", megafiles({"Data\\Config.meg", "Data\\Second.meg"}));

    // Mod with its own MegaFiles.xml + Patch.meg.
    write_file(MEGMOD / "Data" / "MegaFiles.xml", megafiles({"Data\\ModOnly.meg", "Data\\Config.meg"}));
    write_meg(MEGMOD / "Data" / "ModOnly.meg", {{"DATA\\XML\\S2_MODONLY.XML", "MODONLY"}});
    write_meg(MEGMOD / "Data" / "Patch.meg", {{"DATA\\XML\\S2_PATCH.XML", "MODPATCH"}});

    // Mod that only ships its own Data\Config.meg (Q14: replaces FoC's in the MegaFiles.xml slot).
    const fs::path CFGMOD = FOC / "Mods" / "CfgMod";
    write_meg(CFGMOD / "Data" / "Config.meg", {{"DATA\\XML\\ORDER4.XML", "CFGMOD"}, {"DATA\\XML\\S2B.XML", "CFGMOD"}});

    const auto u8 = [](const fs::path& p) { return alamo::path_to_utf8(p); };

    // ================= Scenario 1: two mods, two fallback dirs, auto-detected EaW =================
    VfsConfig cfg;
    cfg.foc_root = u8(FOC);
    cfg.mod_search = {{alamo::vfs::ModSearchEntry::Kind::Path, u8(MODA)}, {alamo::vfs::ModSearchEntry::Kind::Path, u8(MODB)}};
    cfg.fallback_paths = {u8(FB1), u8(FB2)};
    std::string err;
    auto v = Vfs::create(cfg, &err);
    CHECK(v != nullptr);
    if (!v) {
        std::printf("create failed: %s\n", err.c_str());
        return 1;
    }
    CHECK(alamo::iequals(v->eaw_root(), u8(GD)) || fs::equivalent(alamo::path_from_utf8(v->eaw_root()), GD));

    // Mount order.
    const char* const kReasons[10] = {"EaW MegaFiles.xml", "EaW MegaFiles.xml", "EaW Patch", "EaW patch2", "EaW 64Patch",
                                      "FoC MegaFiles.xml", "FoC MegaFiles.xml", "FoC Patch", "FoC patch2", "FoC 64Patch"};
    CHECK(v->mounts().size() == 10);
    for (size_t i = 0; i < v->mounts().size() && i < 10; ++i) CHECK(v->mounts()[i].reason == kReasons[i]);
    CHECK(!v->warnings().empty());  // Data\Missing.meg
    bool missing_warned = false;
    for (const auto& w : v->warnings()) missing_warned |= w.find("Missing.meg") != std::string::npos;
    CHECK(missing_warned);

    // Precedence pairs.
    CHECK(read_str(*v, "Data\\XML\\P_ModA_ModB.xml") == "A");
    CHECK(read_str(*v, "Data\\XML\\P_ModB_Base.xml") == "B");
    CHECK(read_str(*v, "Data\\XML\\P_Base_Meg.xml") == "BASE");
    CHECK(read_str(*v, "Data\\XML\\P_Mod_Meg.xml") == "A");
    CHECK(read_str(*v, "Data\\XML\\P_Meg_FB.xml") == "MEG");
    CHECK(read_str(*v, "Data\\XML\\P_EawMeg_FB.xml") == "EAWMEG");
    CHECK(read_str(*v, "Data\\XML\\P_Base_FB.xml") == "BASE");
    CHECK(read_str(*v, "Data\\XML\\P_FB1_FB2.xml") == "FB1");
    CHECK(read_str(*v, "Data\\XML\\FB2Only.xml") == "FB2");
    // Probe E.
    CHECK(read_str(*v, "Data\\Scripts\\Library\\ProbeE1.lua") == "E1-GAMEDATA");
    CHECK(read_str(*v, "Data\\Scripts\\Library\\ProbeE2.lua") == "E2-FB");
    // MEG priority chain: FoC 64Patch > patch2 > Patch > Second (later listed) > Config > EaW ...
    for (int k = 0; k < 10; ++k) {
        const std::string p = "Data\\XML\\Order" + std::to_string(k) + ".xml";
        if (read_str(*v, p) != kTiers[k]) {
            std::printf("  order %d -> %s (expected %s)\n", k, read_str(*v, p).c_str(), kTiers[k]);
            ++g_fail;
        }
        const auto r = v->resolve(p);
        CHECK(r && r->layer == Vfs::Resolution::Meg && r->layer_index == 9 - k);
    }
    CHECK(read_str(*v, "Data\\XML\\S2_Second.xml") == "FOCSECOND");
    CHECK(read_str(*v, "Data\\XML\\S2_Patch.xml") == "FOCPATCH");

    // Resolution metadata.
    {
        auto r = v->resolve("Data\\XML\\P_ModB_Base.xml");
        CHECK(r && r->layer == Vfs::Resolution::Mod && r->layer_index == 1);
        r = v->resolve("Data\\XML\\P_Base_FB.xml");
        CHECK(r && r->layer == Vfs::Resolution::Base && r->layer_index == 0);
        r = v->resolve("Data\\XML\\FB2Only.xml");
        CHECK(r && r->layer == Vfs::Resolution::Fallback && r->layer_index == 1);
        r = v->resolve("Data\\Scripts\\Library\\ProbeE1.lua");
        CHECK(r && r->layer == Vfs::Resolution::Fallback && r->layer_index == 2);  // GameData is last
        r = v->resolve("Data\\XML\\P_Meg_FB.xml");
        CHECK(r && r->location.find("Config.meg#") != std::string::npos);
        CHECK(v->origin("Data\\XML\\P_Meg_FB.xml") == r->location);
    }

    // Case / separator / prefix insensitivity; not found.
    CHECK(read_str(*v, "data/xml/p_moda_modb.XML") == "A");
    CHECK(read_str(*v, ".\\Data\\XML\\P_ModA_ModB.xml") == "A");
    CHECK(read_str(*v, "./data//xml/order0.xml") == "FOC64");
    CHECK(read_str(*v, "DATA\\XML\\LOWER_STORED.XML") == "LOWER");
    CHECK(read_str(*v, "Data/Xml/Lower_Stored.xml") == "LOWER");
    CHECK(read_str(*v, "Data\\XML\\mixedcase.xml") == "MIXED");
    CHECK(v->exists("DATA/XML/MIXEDCASE.XML"));
    CHECK(!v->exists("Data\\XML\\Nope.xml"));
    CHECK(!v->resolve("Data\\XML\\Nope.xml"));
    CHECK(v->origin("Data\\XML\\Nope.xml").empty());
    {
        std::vector<uint8_t> buf{1, 2, 3};
        CHECK(!v->read("Data\\XML\\Nope.xml", buf) && buf.empty());
    }

    // list(): union, dedupe, sort, '*' does not cross '\\', '**' does.
    {
        const auto l = v->list("Data\\Scripts\\AI\\*.lua");
        const std::vector<std::string> want = {"DATA\\SCRIPTS\\AI\\A1.LUA", "DATA\\SCRIPTS\\AI\\BASE.LUA",
                                               "DATA\\SCRIPTS\\AI\\FB.LUA", "DATA\\SCRIPTS\\AI\\MEG1.LUA"};
        CHECK(l == want);
        if (l != want)
            for (const auto& s : l) std::printf("  list: %s\n", s.c_str());
        const auto all = v->list("data/scripts/ai/**.LUA");
        CHECK(all.size() == 5 && std::is_sorted(all.begin(), all.end()));
        CHECK(std::find(all.begin(), all.end(), "DATA\\SCRIPTS\\AI\\SPACEMODE\\SUB.LUA") != all.end());
        CHECK(read_str(*v, l[0]) == "MOD-A1");
        CHECK(read_str(*v, l[1]) == "BASE");  // loose base beats MEGBASE
        CHECK(v->list("Data\\XML\\Order?.xml").size() == 10);
        CHECK(alamo::vfs::path_glob_match("Data/*/x.?", "DATA\\XML\\X.Y"));
        CHECK(!alamo::vfs::path_glob_match("Data\\*.y", "DATA\\XML\\X.Y"));
    }
    CHECK(v->describe().find("FoC 64Patch") != std::string::npos);

    // Concurrent reads.
    {
        std::atomic<int> bad{0};
        std::vector<std::thread> ts;
        for (int t = 0; t < 8; ++t) {
            ts.emplace_back([&, t] {
                for (int i = 0; i < 400; ++i) {
                    const int k = (i + t) % 10;
                    if (read_str(*v, "Data\\XML\\Order" + std::to_string(k) + ".xml") != kTiers[k]) ++bad;
                    if (read_str(*v, "Data\\XML\\P_ModA_ModB.xml") != "A") ++bad;
                    if (read_str(*v, "Data\\Scripts\\Library\\ProbeE2.lua") != "E2-FB") ++bad;
                    if (v->exists("Data\\XML\\Nope.xml")) ++bad;
                    if ((i % 50) == 0 && v->list("Data\\Scripts\\AI\\*.lua").size() != 4) ++bad;
                }
            });
        }
        for (auto& th : ts) th.join();
        CHECK(bad.load() == 0);
    }

    // ================= Scenario 2: mod MegaFiles.xml + Patch.meg replace FoC's =================
    {
        VfsConfig c2;
        c2.foc_root = u8(FOC);
        c2.mod_search = {{alamo::vfs::ModSearchEntry::Kind::Path, u8(MEGMOD)}};
        auto v2 = Vfs::create(c2, &err);
        CHECK(v2 != nullptr);
        if (v2) {
            CHECK(read_str(*v2, "Data\\XML\\S2_ModOnly.xml") == "MODONLY");
            CHECK(read_str(*v2, "Data\\XML\\S2_Second.xml") == "<missing>");        // FoC list replaced
            CHECK(read_str(*v2, "Data\\XML\\S2_Patch.xml") == "MODPATCH");          // mod Patch.meg
            CHECK(read_str(*v2, "Data\\XML\\S2_BasePatchOnly.xml") == "<missing>");  // base Patch.meg not mounted
            CHECK(read_str(*v2, "Data\\XML\\Order4.xml") == "FOCCONFIG");           // base Config.meg via mod list
            CHECK(read_str(*v2, "Data\\XML\\Order0.xml") == "FOC64");               // base 64Patch still mounts
            CHECK(v2->mounts().size() == 10);
            // Mod list order: ModOnly then Config -> Config mounted later.
            CHECK(v2->mounts()[5].path.find("ModOnly.meg") != std::string::npos);
            CHECK(v2->mounts()[7].path.find("MegMod") != std::string::npos);  // FoC Patch slot from the mod
        }
    }

    // ================= Scenario 2b: mod Data\Config.meg replaces the base one =================
    {
        VfsConfig c2;
        c2.foc_root = u8(FOC);
        c2.mod_search = {{alamo::vfs::ModSearchEntry::Kind::Path, u8(CFGMOD)}};
        auto v2 = Vfs::create(c2, &err);
        CHECK(v2 != nullptr);
        if (v2) {
            CHECK(read_str(*v2, "Data\\XML\\S2B.xml") == "CFGMOD");
            CHECK(read_str(*v2, "Data\\XML\\Order4.xml") == "CFGMOD");        // mod Config.meg in the Config slot
            CHECK(read_str(*v2, "Data\\XML\\Order3.xml") == "FOCSECOND");     // later-listed MEG still wins
            CHECK(read_str(*v2, "Data\\XML\\P_Meg_FB.xml") == "<missing>");   // only in the base Config.meg
            CHECK(v2->mounts().size() == 10);  // base Config.meg not mounted; duplicate list entry skipped
            int cfg_mounts = 0;
            for (const auto& m : v2->mounts()) {
                if (m.path.find("Config.meg") == std::string::npos || m.reason != "FoC MegaFiles.xml") continue;
                ++cfg_mounts;
                CHECK(m.path.find("CfgMod") != std::string::npos);
            }
            CHECK(cfg_mounts == 1);
            CHECK(v2->mounts()[5].path.find("CfgMod") != std::string::npos);  // first FoC list entry
        }
    }

    // ================= Scenario 3: no EaW (eaw_root = "") and no MEGs =================
    {
        VfsConfig c3;
        c3.foc_root = u8(FOC);
        c3.fallback_paths = {u8(FB1)};
        c3.eaw_root = std::string();
        auto v3 = Vfs::create(c3, &err);
        CHECK(v3 != nullptr);
        if (v3) {
            CHECK(v3->eaw_root().empty());
            CHECK(v3->mounts().size() == 5);
            CHECK(read_str(*v3, "Data\\Scripts\\Library\\ProbeE1.lua") == "<missing>");
            CHECK(read_str(*v3, "Data\\XML\\P_EawMeg_FB.xml") == "FB1");
            CHECK(read_str(*v3, "Data\\XML\\Order5.xml") == "<missing>");
        }
        c3.mount_megs = false;
        c3.eaw_root = u8(GD);  // explicit override
        auto v4 = Vfs::create(c3, &err);
        CHECK(v4 && v4->mounts().empty());
        if (v4) {
            CHECK(read_str(*v4, "Data\\XML\\P_Meg_FB.xml") == "FB1");
            CHECK(read_str(*v4, "Data\\Scripts\\Library\\ProbeE1.lua") == "E1-GAMEDATA");
        }
        VfsConfig bad;
        bad.foc_root = u8(T / "does_not_exist");
        err.clear();
        CHECK(Vfs::create(bad, &err) == nullptr && !err.empty());
    }

    // ================= Command line =================
    {
        VfsConfig c;
        c.foc_root = u8(FOC);
        const std::vector<std::string> args = {"StarWarsG.exe",
                                               "MODPATH=Mods\\ModA",
                                               "modpath=\"Mods\\Mod B\"",
                                               "\"FallbackPath=" + u8(FB1) + "\"",
                                               "FALLBACKPATH=..\\FB2\\",
                                               "SteamMod=1234567",
                                               "NOARTPROCESS",
                                               "MODPATH="};
        CHECK(!alamo::vfs::parse_command_line(args, c));  // empty MODPATH= reported
        CHECK(c.mod_search.size() == 3 && c.fallback_paths.size() == 2);
        CHECK(c.mod_search[2].kind == alamo::vfs::ModSearchEntry::Kind::SteamWorkshop && c.mod_search[2].value == "1234567");
        CHECK(alamo::vfs::parse_command_line(std::vector<std::string>{"MODPATH=mods\\moda"}, c));
        CHECK(c.mod_search.size() == 4);
        auto vc = Vfs::create(c, &err);
        CHECK(vc != nullptr);
        if (vc) {
            CHECK(read_str(*vc, "Data\\XML\\P_ModA_ModB.xml") == "A");
            CHECK(read_str(*vc, "Data\\XML\\P_ModB_Base.xml") == "B");  // "Mods\Mod B" with a space
            CHECK(read_str(*vc, "Data\\XML\\FB2Only.xml") == "FB2");    // "..\FB2\" relative
            CHECK(read_str(*vc, "Data\\XML\\P_FB1_FB2.xml") == "FB1");
            const auto r = vc->resolve("Data\\XML\\P_ModA_ModB.xml");
            CHECK(r && r->layer == Vfs::Resolution::Mod && r->layer_index == 0);
            bool warned = false;  // the case-mismatched mods\moda must still index (Linux)
            for (const auto& w : vc->warnings()) warned |= w.find("moda") != std::string::npos;
            CHECK(!warned);
            bool steam_warned = false;  // no resolver: STEAMMOD skipped with a warning
            for (const auto& w : vc->warnings()) steam_warned |= w.find("STEAMMOD=1234567") != std::string::npos;
            CHECK(steam_warned && vc->mod_dirs().size() == 3);
        }
    }

    // ================= Command line: MODPATH= / STEAMMOD= interleaved (Q14) =================
    {
        const auto resolver = [&](std::string_view id) -> std::optional<std::string> {
            if (id == "111") return u8(MODA);
            if (id == "222") return u8(MODB);
            return std::nullopt;  // not subscribed
        };
        // STEAMMOD(ModA) before MODPATH(Mod B): ModA wins.
        VfsConfig c;
        c.foc_root = u8(FOC);
        c.steam_resolver = resolver;
        CHECK(alamo::vfs::parse_command_line(
            std::vector<std::string>{"STEAMMOD=999", "STEAMMOD=111", "MODPATH=Mods\\Mod B"}, c));
        CHECK(c.mod_search.size() == 3);
        auto va = Vfs::create(c, &err);
        CHECK(va != nullptr);
        if (va) {
            CHECK(va->mod_dirs().size() == 2);  // 999 unresolved: nothing mounted for it
            CHECK(read_str(*va, "Data\\XML\\P_ModA_ModB.xml") == "A");
            const auto r = va->resolve("Data\\XML\\P_ModB_Base.xml");
            CHECK(r && r->layer == Vfs::Resolution::Mod && r->layer_index == 1);
            CHECK(va->describe().find("STEAMMOD=111") != std::string::npos);
        }
        // MODPATH(Mod B) before STEAMMOD(ModA): Mod B wins; the later entry (ModA again) is searched last.
        VfsConfig d;
        d.foc_root = u8(FOC);
        d.steam_resolver = resolver;
        CHECK(alamo::vfs::parse_command_line(std::vector<std::string>{"MODPATH=Mods\\Mod B", "STEAMMOD=111"}, d));
        d.mod_search.push_back({alamo::vfs::ModSearchEntry::Kind::Path, u8(MODA)});
        auto vb = Vfs::create(d, &err);
        CHECK(vb != nullptr);
        if (vb) {
            CHECK(vb->mod_dirs().size() == 3);
            CHECK(read_str(*vb, "Data\\XML\\P_ModA_ModB.xml") == "B");
            const auto r = vb->resolve("Data\\XML\\P_Mod_Meg.xml");  // only in ModA (index 1 = STEAMMOD 111)
            CHECK(r && r->layer == Vfs::Resolution::Mod && r->layer_index == 1);
        }
    }

    // MegaFiles.xml extractor.
    {
        const std::string x = "<Mega_Files><!--<File>a</File>--><Files>no</Files><file>Data\\A.meg</file>"
                              "<File/><FILE attr=\"1\">\r\n Data\\B.meg\t</FILE></Mega_Files>";
        const auto e = alamo::vfs::parse_megafiles_xml(
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(x.data()), x.size()));
        CHECK(e.size() == 2 && e[0] == "Data\\A.meg" && e[1] == "Data\\B.meg");
    }

    // ================= Roots deeper than MAX_PATH (260) =================
    // FoC, GameData and a mod under a > 300-character root: loose files, MEGs (both MegaFiles.xml
    // lists) and the GameData auto-detection must all work without the process long-path opt-in.
    {
        const fs::path TL = fs::temp_directory_path() / "alamo_vfs_test_long";
        std::error_code lec;
        alamo::remove_all_long(TL, lec);
        fs::path deep = TL;
        while (deep.native().size() <= 300) deep /= "a_rather_long_directory_name_0123456789";
        const fs::path LFOC = deep / "corruption";
        const fs::path LGD = deep / "GameData";
        const fs::path LMOD = LFOC / "Mods" / "Deep Mod";
        const auto write_long = [](const fs::path& p, const std::string& content) {
            std::error_code e;
            fs::create_directories(alamo::long_path(p.parent_path()), e);
            std::ofstream f(alamo::long_path(p), std::ios::binary);
            f.write(content.data(), static_cast<std::streamsize>(content.size()));
        };
        const auto meg_bytes = [&](const Files& files) {
            const fs::path scratch = T / "scratch.meg";
            write_meg(scratch, files);
            std::vector<uint8_t> b;
            alamo::read_host_file(scratch, b);
            return std::string(b.begin(), b.end());
        };
        write_long(LGD / "sweaw.exe", "MZ");
        write_long(LGD / X / "DeepGD.xml", "DEEP-GD");
        write_long(LGD / "Data" / "Config.meg", meg_bytes({{"DATA\\XML\\DEEPEAWMEG.XML", "DEEP-EAWMEG"}}));
        write_long(LGD / "Data" / "MegaFiles.xml", megafiles({"Data\\Config.meg"}));
        write_long(LFOC / X / "Deep.xml", "DEEP-BASE");
        write_long(LFOC / "Data" / "xml" / "Lower Case Dir.XML", "DEEP-CI");
        write_long(LFOC / "Data" / "Config.meg", meg_bytes({{"DATA\\XML\\DEEPMEG.XML", "DEEP-MEG"}}));
        write_long(LFOC / "Data" / "MegaFiles.xml", megafiles({"Data\\Config.meg"}));
        write_long(LMOD / X / "Deep.xml", "DEEP-MOD");
        CHECK(u8(LMOD / X / "Deep.xml").size() > 300);

        VfsConfig cl;
        cl.foc_root = u8(LFOC);
        cl.mod_search = {{alamo::vfs::ModSearchEntry::Kind::Path, u8(LMOD)}};
        auto vl = Vfs::create(cl, &err);
        CHECK(vl != nullptr);
        if (!vl) std::printf("  deep create failed: %s\n", err.c_str());
        if (vl) {
            CHECK(!vl->eaw_root().empty());
            CHECK(vl->mounts().size() == 2);
            CHECK(read_str(*vl, "Data\\XML\\Deep.xml") == "DEEP-MOD");
            CHECK(read_str(*vl, "Data\\XML\\Lower Case Dir.xml") == "DEEP-CI");
            CHECK(read_str(*vl, "Data\\XML\\DeepMeg.xml") == "DEEP-MEG");
            CHECK(read_str(*vl, "Data\\XML\\DeepEawMeg.xml") == "DEEP-EAWMEG");
            CHECK(read_str(*vl, "Data\\XML\\DeepGD.xml") == "DEEP-GD");
            CHECK(vl->list("Data\\XML\\Deep*.xml").size() == 4);
            const auto rm = vl->resolve("Data\\XML\\DeepMeg.xml");
            CHECK(rm && rm->layer == Vfs::Resolution::Meg && rm->location.find("Config.meg#") != std::string::npos);
            const auto rl = vl->resolve("Data\\XML\\Deep.xml");
            CHECK(rl && rl->layer == Vfs::Resolution::Mod);
            // Reported paths stay plain (no "\\?\" prefix).
            CHECK(rm && rm->location.find('?') == std::string::npos);
            CHECK(rl && rl->location.find('?') == std::string::npos);
            for (const auto& m : vl->mounts()) CHECK(m.path.find('?') == std::string::npos);
            for (const auto& w : vl->warnings()) std::printf("  deep warning: %s\n", w.c_str());
            CHECK(vl->warnings().empty());
        }
        vl.reset();
        CHECK(alamo::remove_all_long(TL, lec) > 0 && !lec);
    }

    // list(glob, layers): the layer-restricted listing, on its own fixture folder (unique to this block).
    {
        const fs::path TX = fs::temp_directory_path() / "alamo_vfs_test_list_layers";
        fs::remove_all(TX);
        const fs::path FX = TX / "corruption", MX = TX / "ModX", BX = TX / "Fallback";
        const fs::path MAPS = fs::path("Data") / "Art" / "Maps";
        write_file(FX / "StarWarsG.exe", "MZ");
        write_file(FX / MAPS / "Loose_Only.ted", "L");
        write_file(FX / MAPS / "Both.ted", "L");
        write_file(FX / MAPS / "Mod_Over_Base.ted", "L");
        write_meg(FX / "Data" / "Patch.meg", {{"DATA\\ART\\MAPS\\BOTH.TED", "M"},
                                              {"DATA\\ART\\MAPS\\MEG_ONLY.TED", "M"},
                                              {"DATA\\XML\\NOT_A_MAP.XML", "M"}});
        write_file(MX / MAPS / "Mod_Only.TED", "D");
        write_file(MX / MAPS / "mod_over_base.ted", "D");
        write_file(BX / MAPS / "Fb_Only.ted", "F");
        VfsConfig cx;
        cx.foc_root = u8(FX);
        cx.mod_search = {{alamo::vfs::ModSearchEntry::Kind::Path, u8(MX)}};
        cx.fallback_paths = {u8(BX)};
        cx.eaw_root = std::string();  // no EaW
        std::string errx;
        auto vx = Vfs::create(cx, &errx);
        CHECK(vx != nullptr);
        if (vx) {
            using L = std::vector<std::string>;
            const auto p = [](const char* n) { return std::string("DATA\\ART\\MAPS\\") + n + ".TED"; };
            const std::string g = "Data\\Art\\Maps\\*.TED";
            CHECK(vx->mounts().size() == 1);
            CHECK(vx->list(g, Vfs::kLayerMod) == (L{p("MOD_ONLY"), p("MOD_OVER_BASE")}));
            CHECK(vx->list(g, Vfs::kLayerBase) == (L{p("BOTH"), p("LOOSE_ONLY"), p("MOD_OVER_BASE")}));
            CHECK(vx->list(g, Vfs::kLayerMeg) == (L{p("BOTH"), p("MEG_ONLY")}));
            CHECK(vx->list(g, Vfs::kLayerFallback) == (L{p("FB_ONLY")}));
            // Base + MEG: BOTH is loose and in the MEG, listed once.
            CHECK(vx->list(g, Vfs::kLayerBase | Vfs::kLayerMeg) == (L{p("BOTH"), p("LOOSE_ONLY"), p("MEG_ONLY"), p("MOD_OVER_BASE")}));
            // All layers = the plain list(); mask 0 and unknown bits select nothing.
            const L everything = {p("BOTH"), p("FB_ONLY"), p("LOOSE_ONLY"), p("MEG_ONLY"), p("MOD_ONLY"), p("MOD_OVER_BASE")};
            CHECK(vx->list(g) == everything);
            CHECK(vx->list(g, Vfs::kLayerAll) == everything);
            CHECK(vx->list(g, 0).empty());
            CHECK(vx->list(g, 0x100u).empty());
            // The glob still applies per layer (the XML entry in the MEG does not match the map glob).
            CHECK(vx->list("Data\\XML\\*.XML", Vfs::kLayerMeg) == (L{"DATA\\XML\\NOT_A_MAP.XML"}));
            CHECK(vx->list("Data\\XML\\*.XML", Vfs::kLayerBase | Vfs::kLayerMod).empty());
            // A name listed from a layer is not an assertion about what a read returns (the mod copy wins).
            CHECK(read_str(*vx, p("MOD_OVER_BASE")) == "D");
        } else {
            std::printf("layers create failed: %s\n", errx.c_str());
        }
        vx.reset();
        std::error_code lx;
        fs::remove_all(TX, lx);
    }

    if (std::getenv("ALAMO_VFS_DESCRIBE")) std::printf("%s", v->describe().c_str());
    v.reset();
    std::error_code ec;
    fs::remove_all(T, ec);
    std::printf(g_fail ? "alamo_vfs_tests: %d failure(s)\n" : "alamo_vfs_tests: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
