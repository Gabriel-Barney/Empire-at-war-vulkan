// alamo_game_skirmish_maps_tests (p4ui t06 + p4uim t03, docs/re/UI.md section 9): the skirmish map catalog and map preview on a fake FoC install
// (loose official + custom maps, a MEG, a mod folder), in its own temp folder; and, with ALAMO_FOC_DATA, on the shipped data.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/game/game_data.hpp"
#include "alamo/meg/meg.h"
#include "alamo/terrain/ted.hpp"
#include "alamo/vfs/vfs.hpp"

namespace fs = std::filesystem;
using alamo::game::SkirmishMapCatalog;
using alamo::game::SkirmishMapEntry;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

namespace {

using Bytes = std::vector<uint8_t>;
using Files = std::vector<std::pair<std::string, Bytes>>;

void write_file(const fs::path& p, const Bytes& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void put_u16(Bytes& s, uint32_t v) {
    s.push_back(static_cast<uint8_t>(v & 0xFFu));
    s.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}
void put_u32(Bytes& s, uint32_t v) {
    for (int i = 0; i < 4; ++i) s.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
}

// Retail-style MEG V1 (as the vfs tests): filename table, records sorted by CRC, contiguous data.
void write_meg(const fs::path& p, const Files& files) {
    const auto n = static_cast<uint32_t>(files.size());
    Bytes head;
    put_u32(head, n);
    put_u32(head, n);
    for (const auto& f : files) {
        put_u16(head, static_cast<uint32_t>(f.first.size()));
        head.insert(head.end(), f.first.begin(), f.first.end());
    }
    struct Rec {
        uint32_t crc, name_index;
    };
    std::vector<Rec> recs;
    for (uint32_t i = 0; i < n; ++i) recs.push_back({alamo::meg::name_crc(files[i].first), i});
    std::sort(recs.begin(), recs.end(), [](const Rec& a, const Rec& b) { return a.crc < b.crc; });
    uint32_t offset = static_cast<uint32_t>(head.size()) + 20u * n;
    Bytes table, data;
    for (uint32_t i = 0; i < n; ++i) {
        const Bytes& payload = files[recs[i].name_index].second;
        put_u32(table, recs[i].crc);
        put_u32(table, i);
        put_u32(table, static_cast<uint32_t>(payload.size()));
        put_u32(table, offset);
        put_u32(table, recs[i].name_index);
        offset += static_cast<uint32_t>(payload.size());
        data.insert(data.end(), payload.begin(), payload.end());
    }
    head.insert(head.end(), table.begin(), table.end());
    head.insert(head.end(), data.begin(), data.end());
    write_file(p, head);
}

Bytes ted(uint32_t kind, uint32_t players, uint8_t custom, const char* key = "") {
    alamo::terrain::MapData md;
    md.info.present = true;
    md.info.format = 513;
    md.info.map_type = kind;
    md.info.counts[0] = players;
    md.info.flag_0b = custom;
    md.info.name_key = key;
    return alamo::terrain::write_ted(md);
}
const Bytes kGarbage = {'n', 'o', 't', ' ', 'a', ' ', 't', 'e', 'd'};

std::vector<std::string> paths(const std::vector<SkirmishMapEntry>& v) {
    std::vector<std::string> o;
    for (const auto& e : v) o.push_back(e.path);
    return o;
}
std::vector<std::string> maps(const std::vector<SkirmishMapEntry>& v) {
    std::vector<std::string> o;
    for (const auto& e : v) o.push_back(e.map);
    return o;
}
const SkirmishMapEntry* by_path(const std::vector<SkirmishMapEntry>& v, const std::string& p) {
    for (const auto& e : v)
        if (e.path == p) return &e;
    return nullptr;
}

// p4uim t03: read_skirmish_map_preview on synthetic maps: the embedded top chunk 0x13, the start positions as 0..1 fractions
// (land from the lower left corner, space centred; y flipped to screen down), mini 12 gating, a missing file.
void test_preview() {
    const fs::path T = fs::temp_directory_path() / "alamo_game_skirmish_preview_test";
    fs::remove_all(T);
    const fs::path MAPS = fs::path("Data") / "Art" / "Maps";
    const auto map = [](uint32_t kind, uint8_t flag12, std::vector<alamo::terrain::Vec2f> starts) {
        alamo::terrain::MapData md;
        md.info.present = true;
        md.info.format = 513;
        md.info.map_type = kind;
        md.info.counts[0] = 2;
        md.info.world_width = 1000.0f;
        md.info.world_height = 2000.0f;
        md.info.flag_12 = flag12;
        md.start_positions = std::move(starts);
        return alamo::terrain::write_ted(md);
    };
    write_file(T / "StarWarsG.exe", {'M', 'Z'});
    write_file(T / MAPS / "LANDP.TED", map(1, 1, {{250.0f, 500.0f}, {1000.0f, 0.0f}, {2000.0f, -5.0f}}));
    write_file(T / MAPS / "SPACEP.TED", map(2, 1, {{0.0f, 0.0f}, {-500.0f, 1000.0f}}));
    write_file(T / MAPS / "NOFLAG.TED", map(1, 0, {{250.0f, 500.0f}}));
    Bytes embedded = map(1, 1, {{500.0f, 1000.0f}});  // a TGA-looking payload appended as top chunk 0x13
    const Bytes tga = {0, 0, 2, 9, 9, 9};
    put_u32(embedded, 0x13);
    put_u32(embedded, static_cast<uint32_t>(tga.size()));
    embedded.insert(embedded.end(), tga.begin(), tga.end());
    write_file(T / MAPS / "EMBED.TED", embedded);
    alamo::vfs::VfsConfig cfg;
    cfg.foc_root = alamo::path_to_utf8(T);
    std::string err;
    auto v = alamo::vfs::Vfs::create(cfg, &err);
    CHECK(v != nullptr);
    if (!v) return;
    using alamo::game::read_skirmish_map_preview;
    const auto land = read_skirmish_map_preview(*v, "LANDP", false);
    CHECK(land.found && land.embedded_image.empty() && land.starts.size() == 3);
    CHECK(land.starts[0] == std::make_pair(0.25f, 0.75f) && land.starts[1] == std::make_pair(1.0f, 1.0f) && land.starts[2] == std::make_pair(1.0f, 1.0f));  // clamped
    const auto space = read_skirmish_map_preview(*v, "SPACEP", true);
    CHECK(space.starts.size() == 2 && space.starts[0] == std::make_pair(0.5f, 0.5f) && space.starts[1] == std::make_pair(0.0f, 0.0f));
    CHECK(read_skirmish_map_preview(*v, "NOFLAG", false).starts.empty());  // no start positions without mini 12
    const auto emb = read_skirmish_map_preview(*v, "embed", false);
    CHECK(emb.found && emb.embedded_image == tga && emb.starts.size() == 1);
    const auto missing = read_skirmish_map_preview(*v, "NOPE", false);
    CHECK(!missing.found && missing.starts.empty() && missing.embedded_image.empty());
    fs::remove_all(T);
}

void test_fake_install() {
    const fs::path T = fs::temp_directory_path() / "alamo_game_skirmish_maps_test";
    fs::remove_all(T);
    const fs::path FOC = T / "corruption", MOD = T / "Mod", MAPS = fs::path("Data") / "Art" / "Maps", CUSTOM = fs::path("Data") / "CustomMaps";
    write_file(FOC / "StarWarsG.exe", {'M', 'Z'});
    // Loose official maps.
    write_file(FOC / MAPS / "_MP_LAND_ALPHA.ted", ted(1, 4, 0, "TEXT_MAP_NAME_ALPHA"));
    write_file(FOC / MAPS / "_MP_SPACE_BETA.TED", ted(2, 2, 0, "TEXT_MAP_NAME_BETA"));
    write_file(FOC / MAPS / "CAMPAIGN_ONE.TED", ted(1, 1, 0));
    write_file(FOC / MAPS / "BROKEN.TED", kGarbage);
    write_file(FOC / MAPS / "_MP_LAND_FLAGGED.TED", ted(1, 3, 1));  // an official-list map whose header says custom
    write_file(FOC / MAPS / "NOTAMAP.TED", ted(7, 5, 0));           // kind 7: kept official, never a skirmish row
    write_file(FOC / MAPS / "Sub" / "DEEP.TED", ted(1, 2, 0));      // not directly in Data\Art\Maps: not scanned
    // Loose custom folder.
    write_file(FOC / CUSTOM / "MyMap.TED", ted(1, 2, 1, "TEXT_MAP_NAME_MINE"));
    write_file(FOC / CUSTOM / "NotCustom.TED", ted(1, 2, 0));  // custom scan keeps only flag 1
    write_file(FOC / CUSTOM / "Bad.TED", kGarbage);            // unreadable custom entry: dropped
    // MEG: an official map, a copy of ALPHA the loose file wins over, and a custom-folder map.
    write_meg(FOC / "Data" / "Patch.meg", {{"DATA\\ART\\MAPS\\_MP_SPACE_MEG.TED", ted(2, 3, 0, "TEXT_MAP_NAME_MEG")},
                                           {"DATA\\ART\\MAPS\\_MP_LAND_ALPHA.TED", ted(1, 9, 0)},
                                           {"DATA\\CUSTOMMAPS\\MEGCUSTOM.TED", ted(2, 2, 1)}});
    // Mod folder: its Data\Art\Maps goes to the custom scan; its Data\CustomMaps is never scanned.
    write_file(MOD / MAPS / "ModMap.ted", ted(1, 2, 1, "TEXT_MAP_NAME_MODMAP"));
    write_file(MOD / MAPS / "ModPlain.ted", ted(1, 2, 0));        // flag 0: dropped from the custom scan, not official
    write_file(MOD / MAPS / "_MP_LAND_ALPHA.ted", ted(1, 6, 1));  // overrides an official map: its header says custom
    write_file(MOD / MAPS / "_MP_SPACE_BETA.ted", ted(2, 3, 0));  // overrides an official map, still official
    write_file(MOD / CUSTOM / "Ignored.TED", ted(1, 2, 1));

    alamo::vfs::VfsConfig cfg;
    cfg.foc_root = alamo::path_to_utf8(FOC);
    cfg.mod_paths = {alamo::path_to_utf8(MOD)};
    cfg.eaw_root = std::string();
    std::string err;
    auto v = alamo::vfs::Vfs::create(cfg, &err);
    CHECK(v != nullptr);
    if (!v) {
        std::printf("vfs create failed: %s\n", err.c_str());
        return;
    }
    const SkirmishMapCatalog cat = alamo::game::build_skirmish_map_catalog(*v);
    const auto P = [](const char* n) { return std::string("DATA\\ART\\MAPS\\") + n + ".TED"; };

    // Official list: every Data\Art\Maps\*.TED of the base + MEG layers (not the mod-only maps, not subfolders), in path order,
    // readable or not.
    CHECK(paths(cat.official) == (std::vector<std::string>{P("BROKEN"), P("CAMPAIGN_ONE"), P("NOTAMAP"), P("_MP_LAND_ALPHA"), P("_MP_LAND_FLAGGED"),
                                                          P("_MP_SPACE_BETA"), P("_MP_SPACE_MEG")}));
    CHECK(std::is_sorted(cat.official.begin(), cat.official.end(), [](const auto& a, const auto& b) { return a.path < b.path; }));
    const SkirmishMapEntry* broken = by_path(cat.official, P("BROKEN"));
    CHECK(broken && !broken->readable && broken->kind == 0 && broken->map == "BROKEN");
    // Headers come through the normal lookup: a mod copy of an official name decides its header; loose beats the MEG.
    const SkirmishMapEntry* alpha = by_path(cat.official, P("_MP_LAND_ALPHA"));
    CHECK(alpha && alpha->readable && alpha->kind == 1 && alpha->players == 6 && alpha->custom && alpha->map == "_MP_LAND_ALPHA" &&
          alpha->name_key.empty());  // the mod copy has no key
    const SkirmishMapEntry* beta = by_path(cat.official, P("_MP_SPACE_BETA"));
    CHECK(beta && beta->kind == 2 && beta->players == 3 && !beta->custom && beta->name_key.empty());
    const SkirmishMapEntry* meg = by_path(cat.official, P("_MP_SPACE_MEG"));
    CHECK(meg && meg->kind == 2 && meg->players == 3 && !meg->custom && meg->name_key == "TEXT_MAP_NAME_MEG");
    const SkirmishMapEntry* camp = by_path(cat.official, P("CAMPAIGN_ONE"));
    CHECK(camp && camp->kind == 1 && camp->players == 1);

    // Custom scan: base + MEG Data\CustomMaps plus the mod layer's Data\Art\Maps; flag-1 entries only; path order.
    CHECK(paths(cat.custom_scan) == (std::vector<std::string>{P("MODMAP"), P("_MP_LAND_ALPHA"), "DATA\\CUSTOMMAPS\\MEGCUSTOM.TED",
                                                              "DATA\\CUSTOMMAPS\\MYMAP.TED"}));
    CHECK(std::is_sorted(cat.custom_scan.begin(), cat.custom_scan.end(), [](const auto& a, const auto& b) { return a.path < b.path; }));
    for (const auto& e : cat.custom_scan) CHECK(e.readable && e.custom);
    const SkirmishMapEntry* mine = by_path(cat.custom_scan, "DATA\\CUSTOMMAPS\\MYMAP.TED");
    CHECK(mine && mine->map == "DATA\\CUSTOMMAPS\\MYMAP.TED" && mine->kind == 1 && mine->players == 2 && mine->name_key == "TEXT_MAP_NAME_MINE");
    const SkirmishMapEntry* modmap = by_path(cat.custom_scan, P("MODMAP"));
    CHECK(modmap && modmap->map == "MODMAP");
    CHECK(by_path(cat.custom_scan, "DATA\\CUSTOMMAPS\\NOTCUSTOM.TED") == nullptr && by_path(cat.custom_scan, "DATA\\CUSTOMMAPS\\BAD.TED") == nullptr &&
          by_path(cat.custom_scan, P("MODPLAIN")) == nullptr && by_path(cat.custom_scan, "DATA\\CUSTOMMAPS\\IGNORED.TED") == nullptr);

    // Rows. Official tab: kind 1 / 2, players >= 2, custom flag 0 (the overridden ALPHA moved to the Custom tab; CAMPAIGN_ONE,
    // BROKEN and the kind 7 map never pass).
    const auto official_rows = alamo::game::skirmish_map_rows(cat, false);
    CHECK(maps(official_rows) == (std::vector<std::string>{"_MP_SPACE_BETA", "_MP_SPACE_MEG"}));
    // Custom tab: official-list entries flagged custom and the kept custom-scan entries; ALPHA is in both lists, listed once.
    const auto custom_rows = alamo::game::skirmish_map_rows(cat, true);
    CHECK(maps(custom_rows) == (std::vector<std::string>{"_MP_LAND_ALPHA", "_MP_LAND_FLAGGED", "MODMAP", "DATA\\CUSTOMMAPS\\MEGCUSTOM.TED",
                                                         "DATA\\CUSTOMMAPS\\MYMAP.TED"}));
    // Every row's id reaches its file again through terrain::map_path.
    for (const auto* rows : {&official_rows, &custom_rows})
        for (const auto& e : *rows) CHECK(alamo::terrain::map_path(e.map) == e.path && v->exists(e.path));
    // Lookup by id (stem, file name, path; case-insensitive).
    CHECK(alamo::game::find_skirmish_map(cat, "_mp_space_beta") == beta && alamo::game::find_skirmish_map(cat, "_MP_SPACE_BETA.ted") == beta &&
          alamo::game::find_skirmish_map(cat, "data/customMaps/mymap.ted") == mine && alamo::game::find_skirmish_map(cat, "Nope") == nullptr &&
          alamo::game::find_skirmish_map(cat, "") == nullptr);
    CHECK(alamo::game::find_skirmish_map(cat, "_MP_LAND_ALPHA") && alamo::game::find_skirmish_map(cat, "_MP_LAND_ALPHA")->players == 6);

    // Deterministic: a second build gives the same lists, field by field.
    const SkirmishMapCatalog again = alamo::game::build_skirmish_map_catalog(*v);
    CHECK(paths(again.official) == paths(cat.official) && paths(again.custom_scan) == paths(cat.custom_scan));
    for (size_t i = 0; i < cat.official.size() && i < again.official.size(); ++i)
        CHECK(again.official[i].players == cat.official[i].players && again.official[i].kind == cat.official[i].kind &&
              again.official[i].custom == cat.official[i].custom && again.official[i].name_key == cat.official[i].name_key);

    v.reset();
    std::error_code ec;
    fs::remove_all(T, ec);
}

// q6 t02: skirmish_space_maps / skirmish_land_maps leave out maps whose file is empty through the normal lookup (Thrawn's
// Revenge ships a 0-byte _MP_SPACE_KUAT.ted over the FoC one); KUAT still goes first when it reads.
void test_map_lists_skip_empty() {
    const fs::path T = fs::temp_directory_path() / "alamo_game_skirmish_lists_test";
    fs::remove_all(T);
    const fs::path FOC = T / "corruption", MOD = T / "Mod", MAPS = fs::path("Data") / "Art" / "Maps";
    write_file(FOC / "StarWarsG.exe", {'M', 'Z'});
    for (const char* n : {"_MP_SPACE_ALPHA", "_MP_SPACE_KUAT", "_MP_SPACE_ZETA"}) write_file(FOC / MAPS / (std::string(n) + ".ted"), ted(2, 2, 0));
    for (const char* n : {"_MP_LAND_ALPHA", "_MP_LAND_BETA"}) write_file(FOC / MAPS / (std::string(n) + ".ted"), ted(1, 2, 0));
    const auto lists = [&](bool with_mod) {
        alamo::game::GameData d;
        alamo::vfs::VfsConfig cfg;
        cfg.foc_root = alamo::path_to_utf8(FOC);
        if (with_mod) cfg.mod_paths = {alamo::path_to_utf8(MOD)};
        cfg.eaw_root = std::string();
        std::string err;
        auto v = alamo::vfs::Vfs::create(cfg, &err);
        CHECK(v != nullptr);
        d.vfs = v.get();
        d.files = std::move(v);
        return std::make_pair(alamo::game::skirmish_space_maps(d), alamo::game::skirmish_land_maps(d));
    };
    const auto plain = lists(false);
    CHECK(plain.first == (std::vector<std::string>{"_MP_SPACE_KUAT", "_MP_SPACE_ALPHA", "_MP_SPACE_ZETA"}));
    CHECK(plain.second == (std::vector<std::string>{"_MP_LAND_ALPHA", "_MP_LAND_BETA"}));
    write_file(MOD / MAPS / "_MP_SPACE_KUAT.ted", {});  // empty mod copies hide the maps
    write_file(MOD / MAPS / "_MP_LAND_BETA.ted", {});
    write_file(MOD / MAPS / "_MP_SPACE_MOD.ted", ted(2, 2, 0));
    const auto modded = lists(true);
    CHECK(modded.first == (std::vector<std::string>{"_MP_SPACE_ALPHA", "_MP_SPACE_MOD", "_MP_SPACE_ZETA"}));
    CHECK(modded.second == (std::vector<std::string>{"_MP_LAND_ALPHA"}));
    std::error_code ec;
    fs::remove_all(T, ec);
}

// The shipped data (ALAMO_FOC_DATA): UI.md 9.4 "Vanilla result after the patches: 24 land and 24 space maps, all official, no custom".
void test_real_data() {
    const char* root = std::getenv("ALAMO_FOC_DATA");
    if (!root || !*root) {
        std::printf("skirmish maps: ALAMO_FOC_DATA not set, real-data part skipped\n");
        return;
    }
    alamo::vfs::VfsConfig cfg;
    cfg.foc_root = root;
    std::string err;
    auto v = alamo::vfs::Vfs::create(cfg, &err);
    CHECK(v != nullptr);
    if (!v) return;
    const SkirmishMapCatalog cat = alamo::game::build_skirmish_map_catalog(*v);
    const auto rows = alamo::game::skirmish_map_rows(cat, false);
    size_t land = 0, space = 0;
    for (const auto& e : rows) {
        (e.kind == 1 ? land : space) += 1;
        CHECK(e.players >= 2 && !e.custom && e.readable);
        CHECK((e.kind == 1) == (e.map.find("_MP_LAND_") == 0) && (e.kind == 2) == (e.map.find("_MP_SPACE_") == 0));
    }
    std::printf("skirmish maps: official %zu entries, %zu custom-scan, rows: %zu land, %zu space; custom rows %zu\n", cat.official.size(),
                cat.custom_scan.size(), land, space, alamo::game::skirmish_map_rows(cat, true).size());
    CHECK(land == 24 && space == 24);
    CHECK(alamo::game::skirmish_map_rows(cat, true).empty());
    const SkirmishMapEntry* kuat = alamo::game::find_skirmish_map(cat, "_MP_SPACE_KUAT");
    CHECK(kuat && kuat->kind == 2 && !kuat->name_key.empty());
}

}  // namespace

int main() {
    test_preview();
    test_fake_install();
    test_map_lists_skip_empty();
    test_real_data();
    std::printf(g_fail ? "alamo_game_skirmish_maps_tests: %d failure(s)\n" : "alamo_game_skirmish_maps_tests: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
