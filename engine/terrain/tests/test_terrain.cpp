// alamo_terrain tests (no framework; non-zero exit on failure).
//   * CPU (always): synthetic .ted write/parse round trip, mesh build on tiny grids, blend-texture
//     rules (incl. the lowest-index quad rule, full coverage, not rendered), texcoord/rotation and
//     blend-UV maths, normals, water mesh/shore alpha/constants, placements, fuzzing.
//   * Real data (ALAMO_FOC_DATA = FoC "corruption" folder; built with vfs/data/jobs): every map in
//     the VFS, the TERRAIN_WATER_POST.md 1.9 table, space maps, Endor/Dagobah/Hoth mesh stats and
//     determinism, Endor object placements resolved through the GameDatabase.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/core/log.hpp"
#include "alamo/terrain/terrain.hpp"
#include "alamo/test/support.hpp"

#if defined(ALAMO_TERRAIN_HAVE_DATA)
#include "alamo/data/game_database.hpp"
#include "alamo/jobs/job_system.hpp"
#include "alamo/meg/meg.h"
#include "alamo/vfs/vfs.hpp"
#endif

using namespace alamo::terrain;

using ::alamo::test::g_fail;

namespace {

bool approx(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

struct Lcg {
    uint64_t s;
    explicit Lcg(uint64_t seed) : s(seed) {}
    uint32_t next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(s >> 33);
    }
    uint32_t below(uint32_t n) { return n ? next() % n : 0; }
};

// ---------------------------------------------------------------------------- synthetic map
MapData make_map(uint32_t patches, uint64_t seed) {
    MapData m;
    Lcg rng(seed);
    m.info.present = true;
    m.info.format = 513;
    m.info.map_type = 1;
    m.info.counts = {6, 5, 3, 3, 3, 0};
    m.info.name_key = "TEXT_MAP_NAME_TEST";
    m.info.planet_name = "Testworld";
    m.info.text_0a = "control";
    const uint32_t n = patches * kPatchQuads + 1;
    m.info.world_width = m.info.world_height = static_cast<float>(n - 1) * kVertexSpacing;
    m.info.flag_12 = 1;
    m.start_positions = {{100.0f, 200.0f}, {300.5f, 400.25f}};
    m.preview_objects = {{7, 0x1234u, {0.25f, 0.75f}}};

    Environment e;
    e.name = "Sunrise_Clear";
    e.light_color[0][1] = 0.729f;
    e.intensity[0] = 0.646f;
    e.yaw[0] = 3.83972f;
    e.elevation[0] = 0.576133f;
    e.fog_enabled = true;
    e.fog_start = 0;
    e.fog_end = 3500;
    e.clear_color[2] = 0.85f;
    e.shadow_color[0] = 0.505f;
    e.skydome[0] = "DAY_STORM_SKY";
    e.skydome_scale[0] = 2.0f;
    e.skydome_angle_deg[0][1] = 128.0f;
    e.bloom_strength = 0.6f;
    e.bloom_cutoff = 0.8f;
    e.bloom_size = 1.0f;
    e.wind_direction_deg = 90.0f;
    e.wind_speed = 3.0f;
    e.cloud_scale = 1000.0f;
    e.cloud_speed = 30.0f;
    e.weather_effect = "RAIN";
    e.lightning_intensity = 0.25f;
    m.environments.environments.push_back(e);
    Environment e2;
    e2.name = "Noon";
    m.environments.environments.push_back(e2);
    m.environments.start_index = 1;
    m.environments.scene_fog_present = true;
    m.environments.scene_fog_enabled = true;
    m.environments.scene_fog_end = 3500;

    Terrain& t = m.terrain;
    t.present = true;
    t.width = t.height = n;
    t.header.width = t.header.height = n;
    t.header.vertex_count = n * n;
    t.header.cloud_texture = "W_Clouds00.tga";
    t.header.cloud_scale = 1000;
    t.header.water_height = 0.0f;
    t.header.water_type = WaterType::Water;
    t.header.water_type_raw = 1;
    t.header.reflection_fraction = 0.34f;
    t.header.water_color[1] = 0.88f;
    t.header.water_texture[0] = "W_WaterBump00.tga";
    t.header.water_texture[1] = "w_water_fallback_blue.tga";
    t.header.water_alpha = 0.9f;
    t.header.shore_fade_depth = 10.0f;
    t.header.shore_min_alpha = 0.001f;
    t.heights.resize(static_cast<size_t>(n) * n);
    t.material.resize(t.heights.size());
    t.brightness.assign(t.heights.size(), 0xFF);
    for (uint32_t y = 0; y < n; ++y)
        for (uint32_t x = 0; x < n; ++x) {
            const size_t i = static_cast<size_t>(y) * n + x;
            t.heights[i] = static_cast<int16_t>(static_cast<int32_t>(rng.below(4000)) - 1000);
            t.material[i] = static_cast<uint8_t>(rng.below(5));
        }
    for (uint32_t s = 0; s < kMaterialSlots; ++s) {
        TerrainMaterial& tm = t.materials[s];
        tm.present = true;
        if (s < 5) {
            tm.diffuse_texture = "W_Grnd0" + std::to_string(s) + ".tga";
            tm.normal_texture = s == 2 ? "" : "W_Grnd_B.tga";
            tm.tile_size = 100.0f + 10.0f * static_cast<float>(s);
            tm.rotation_z = 0.1f * static_cast<float>(s);
            tm.surface_type = s;
        }
    }
    t.materials[3].full_coverage = true;
    t.water_scroll[0] = {true, 112.0f, 0.0f, 0.05f, 0.0f};
    t.water_scroll[1] = {true, 150.0f, 0.2f, 0.05f, 1.0f};

    m.passability_present = true;
    m.passability.resize(static_cast<size_t>(n) * n);
    for (size_t i = 0; i < m.passability.size(); ++i) m.passability[i] = 1u << rng.below(9);

    for (uint32_t i = 0; i < 5; ++i) {
        ObjectPlacement o;
        o.list_id[0] = 1000 + i;
        o.list_id[1] = 2000 + i;
        o.object_id = 45000 + i;
        o.type_crc = alamo::name_crc(i % 2 ? "Skirmish_Build_Pad" : "Team_01_Power_Generator");
        o.owner = i;
        if (i == 2) o.health_fraction = 0.5f;
        o.position[0] = 100.0f * static_cast<float>(i + 1);
        o.position[1] = 50.0f * static_cast<float>(i + 1);
        o.position[2] = 12.5f;
        o.rotation_deg[2] = 30.0f * static_cast<float>(i);
        o.rotation_copy[2] = o.rotation_deg[2];
        o.has_secondary = i == 2;
        o.secondary[0] = 5.0f;
        o.attachment_slots = 3;
        if (i == 1) o.attachments = {"HP_A"};
        o.owner_raw = {mk_u32(0, i), mk_u32(3, 0xFFFFFFFFu)};
        m.objects.push_back(o);
    }
    m.playable.present = true;
    m.playable.has_box = true;
    m.playable.box_min[0] = 300.0f;
    m.playable.box_max[0] = 1000.0f;
    AiZone z;
    z.id = 77;
    z.name = "New Zone 1";
    z.stored_cells = 2;
    z.center[0] = 468.8f;
    z.links = {{78, 2}};
    z.cells = {{38, 59, 1}, {39, 59, 1}};
    m.zones.push_back(z);
    m.tactical_camera.present = true;
    m.tactical_camera.raw = {mk_f32(0x03, 1250.0f), mk_f32(0x08, 55.0f)};
    return m;
}

bool same_env(const Environment& a, const Environment& b) {
    bool ok = a.name == b.name && a.fog_enabled == b.fog_enabled && a.skydome[0] == b.skydome[0] &&
              a.skydome[1] == b.skydome[1] && a.weather_effect == b.weather_effect && a.cloud_texture == b.cloud_texture &&
              a.field_13 == b.field_13;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) ok = ok && a.light_color[i][j] == b.light_color[i][j];
        ok = ok && a.sun_specular[i] == b.sun_specular[i] && a.ambient[i] == b.ambient[i] && a.intensity[i] == b.intensity[i] &&
             a.yaw[i] == b.yaw[i] && a.elevation[i] == b.elevation[i] && a.fog_color[i] == b.fog_color[i] &&
             a.clear_color[i] == b.clear_color[i] && a.shadow_color[i] == b.shadow_color[i];
    }
    for (int i = 0; i < 2; ++i)
        ok = ok && a.skydome_scale[i] == b.skydome_scale[i] && a.skydome_angle_deg[i][0] == b.skydome_angle_deg[i][0] &&
             a.skydome_angle_deg[i][1] == b.skydome_angle_deg[i][1];
    return ok && a.fog_start == b.fog_start && a.fog_end == b.fog_end && a.bloom_strength == b.bloom_strength &&
           a.bloom_cutoff == b.bloom_cutoff && a.bloom_size == b.bloom_size && a.wind_direction_deg == b.wind_direction_deg &&
           a.wind_speed == b.wind_speed && a.lightning_intensity == b.lightning_intensity && a.cloud_scale == b.cloud_scale &&
           a.cloud_speed == b.cloud_speed;
}

bool same_material(const TerrainMaterial& a, const TerrainMaterial& b) {
    bool ok = a.diffuse_texture == b.diffuse_texture && a.normal_texture == b.normal_texture && a.tile_size == b.tile_size &&
              a.rotation_z == b.rotation_z && a.rotation_x == b.rotation_x && a.specular_power == b.specular_power &&
              a.surface_type == b.surface_type && a.flag_0d == b.flag_0d && a.full_coverage == b.full_coverage &&
              a.not_rendered == b.not_rendered;
    for (int i = 0; i < 3; ++i) ok = ok && a.diffuse_color[i] == b.diffuse_color[i] && a.specular_color[i] == b.specular_color[i];
    return ok;
}

// ---------------------------------------------------------------------------- CPU tests
void test_chunks() {
    std::vector<ChunkView> c;
    const uint8_t ok[] = {1, 0, 0, 0, 2, 0, 0, 0, 9, 9};
    CHECK(read_chunks(ok, c) && c.size() == 1 && c[0].payload.size() == 2 && !c[0].has_children);
    const uint8_t bad[] = {1, 0, 0, 0, 3, 0, 0, 0, 9, 9};
    CHECK(!read_chunks(bad, c));
    const uint8_t huge[] = {1, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF};
    CHECK(!read_chunks(huge, c));
    std::vector<MiniView> m;
    const uint8_t minis[] = {0x14, 3, 'a', 'b', 0, 0x05, 1, 7};
    CHECK(read_minis(minis, m) && m.size() == 2 && mini_string(m[0]) == "ab" && mini_u32_or(m[1], 0) == 7);
    const uint8_t badmini[] = {0x14, 9, 'a'};
    CHECK(!read_minis(badmini, m));
    const RawMini u = mk_utf16(8, "TEXT_X");
    CHECK(mini_utf16(MiniView{8, u.data}) == "TEXT_X");
    float nanv = std::nanf("");
    const RawMini fn = mk_f32(1, nanv);
    float out = 3.0f;
    CHECK(!mini_f32(MiniView{1, fn.data}, out) && out == 3.0f);
}

void test_round_trip() {
    MapData m = make_map(2, 1);
    if (m.objects.size() > 1) m.objects[1].hint = "falcongear1";  // q6 t28: 0x482 hint name
    const std::vector<uint8_t> bytes = write_ted(m);
    MapData r;
    std::string err;
    CHECK(parse_ted(bytes, r, &err));
    if (!err.empty()) std::printf("  parse error: %s\n", err.c_str());
    CHECK(r.info.format == 513 && r.info.map_type == 1 && r.info.counts == m.info.counts);
    CHECK(r.info.name_key == m.info.name_key && r.info.planet_name == "Testworld" && r.info.text_0a == "control");
    CHECK(r.info.world_width == m.info.world_width && r.info.flag_12 == 1);
    CHECK(r.start_positions == m.start_positions);
    CHECK(r.preview_objects == m.preview_objects);
    CHECK(r.environments.environments.size() == 2);
    if (r.environments.environments.size() == 2) {
        CHECK(same_env(r.environments.environments[0], m.environments.environments[0]));
        CHECK(same_env(r.environments.environments[1], m.environments.environments[1]));
    }
    CHECK(r.environments.start_index == 1 && r.environments.start() == &r.environments.environments[1]);
    CHECK(r.environments.scene_fog_present && r.environments.scene_fog_enabled && r.environments.scene_fog_end == 3500);
    const Terrain& a = m.terrain;
    const Terrain& b = r.terrain;
    CHECK(b.present && b.width == a.width && b.height == a.height && b.patches_x() == 2 && b.patches_y() == 2);
    CHECK(b.heights == a.heights && b.material == a.material && b.brightness == a.brightness);
    bool mats = true;
    for (uint32_t s = 0; s < kMaterialSlots; ++s) mats = mats && same_material(a.materials[s], b.materials[s]);
    CHECK(mats);
    CHECK(b.header.water_type == WaterType::Water && b.header.reflection_fraction == 0.34f && b.header.water_color[1] == 0.88f);
    CHECK(b.header.water_texture[0] == "W_WaterBump00.tga" && b.header.water_texture[2].empty());
    CHECK(b.header.water_alpha == 0.9f && b.header.shore_min_alpha == 0.001f && b.header.cloud_scale == 1000.0f);
    CHECK(b.water_scroll[0].present && b.water_scroll[0].tile_size == 112.0f && b.water_scroll[1].direction == 1.0f);
    CHECK(r.passability_present && r.passability == m.passability);
    CHECK(r.objects.size() == m.objects.size());
    for (size_t i = 0; i < std::min(r.objects.size(), m.objects.size()); ++i) {
        const ObjectPlacement &p = m.objects[i], &q = r.objects[i];
        CHECK(p.object_id == q.object_id && p.type_crc == q.type_crc && p.owner == q.owner && p.health_fraction == q.health_fraction);
        CHECK(p.list_id[0] == q.list_id[0] && p.list_id[1] == q.list_id[1]);
        CHECK(std::equal(p.position, p.position + 3, q.position) && std::equal(p.rotation_deg, p.rotation_deg + 3, q.rotation_deg));
        CHECK(p.has_secondary == q.has_secondary && p.attachments == q.attachments && p.attachment_slots == q.attachment_slots);
        CHECK(p.owner_raw == q.owner_raw);
        CHECK(p.hint == q.hint);
    }
    CHECK(r.objects.size() > 1 && r.objects[0].hint.empty() && r.objects[1].hint == "falcongear1");
    CHECK(r.playable.has_box && r.playable.box_min[0] == 300.0f && r.playable.box_max[0] == 1000.0f);
    CHECK(r.zones.size() == 1 && r.zones[0].name == "New Zone 1" && r.zones[0].id == 77 && r.zones[0].cells.size() == 2 &&
          r.zones[0].cells[0].x == 38 && r.zones[0].cells[0].y == 59 && r.zones[0].links.size() == 1);
    CHECK(r.tactical_camera.present && r.tactical_camera.f32_or(0x08, 0) == 55.0f);
    // Second round trip is byte-identical.
    CHECK(write_ted(r) == bytes);
    // Column-major passability on disk: cell (x=1, y=0) is file index H.
    {
        MapData pm = make_map(1, 3);
        pm.passability.assign(pm.passability.size(), 0);
        pm.passability[1] = 0xABCD;  // row-major (x=1, y=0)
        const auto pb = write_ted(pm);
        MapData pr;
        CHECK(parse_ted(pb, pr) && pr.passability_at(1, 0) == 0xABCD && pr.passability_at(0, 1) == 0);
    }
    // Space map: no terrain, outer/inner boxes.
    {
        MapData s;
        s.environments.environments.push_back(Environment{});
        s.playable.present = true;
        s.playable.has_outer_box = s.playable.has_box = true;
        s.playable.outer_min[0] = -6000;
        s.playable.box_min[0] = -5600;
        const auto sb = write_ted(s);
        MapData sr;
        CHECK(parse_ted(sb, sr) && sr.is_space() && sr.playable.has_outer_box && sr.playable.outer_min[0] == -6000.0f &&
              sr.playable.box_min[0] == -5600.0f);
    }
    // Clouds: environment overrides the header.
    const CloudParams cp = effective_clouds(r, &r.environments.environments[0]);
    CHECK(cp.scale == 1000.0f && approx(cp.velocity[0], 0.0f, 1e-4f) && approx(cp.velocity[1], 30.0f, 1e-4f));
    const auto wv = r.environments.environments[0].wind_vector();
    CHECK(approx(wv[1], 3.0f, 1e-5f));
}

// Tiny terrain: one patch, all material 0 except chosen vertices.
Terrain tiny_terrain() {
    Terrain t;
    t.present = true;
    t.width = t.height = 32;
    t.header.width = t.header.height = 32;
    t.heights.assign(32 * 32, 0);
    t.material.assign(32 * 32, 0);
    t.brightness.assign(32 * 32, 0xFF);
    return t;
}

void test_mesh_blend_rules() {
    Terrain t = tiny_terrain();
    auto set = [&](uint32_t x, uint32_t y, uint8_t m) { t.material[y * 32 + x] = m; };
    // Material 5 at a single vertex (10,10); material 2 at vertex (20,20) and (21,20).
    set(10, 10, 5);
    set(20, 20, 2);
    set(21, 20, 2);
    TerrainMesh mesh;
    std::string err;
    CHECK(build_terrain_mesh(t, mesh, {}, alamo::serial_for, &err));
    CHECK(mesh.patches.size() == 1);
    const TerrainPatch& p = mesh.patches[0];
    CHECK(p.passes.size() == 3);
    if (p.passes.size() != 3) return;
    CHECK(p.passes[0].material == 0 && p.passes[1].material == 2 && p.passes[2].material == 5);
    // Material 0: every quad touches it (each quad has at least one 0 corner here) -> 961 quads.
    CHECK(p.passes[0].triangle_count() == 961 * 2);
    // Material 0 is the lowest index of every quad -> its texture is white everywhere.
    bool all_white = true;
    for (uint32_t y = 0; y < 32; ++y)
        for (uint32_t x = 0; x < 32; ++x) all_white = all_white && p.passes[0].texel(x, y) == 0xFFFF;
    CHECK(all_white);
    // Material 5: quads around (10,10) = 4 quads; only the vertex itself is white (5 is never lowest).
    const TerrainPass& p5 = p.passes[2];
    CHECK(p5.triangle_count() == 8);
    uint32_t white5 = 0;
    for (uint32_t y = 0; y < 32; ++y)
        for (uint32_t x = 0; x < 32; ++x) white5 += p5.texel(x, y) == 0xFFFF ? 1u : 0u;
    CHECK(white5 == 1 && p5.texel(10, 10) == 0xFFFF);
    // Material 2 with 0 in every touching quad: 6 quads, only its 2 vertices white.
    const TerrainPass& p2 = p.passes[1];
    CHECK(p2.triangle_count() == 12 && p2.texel(20, 20) == 0xFFFF && p2.texel(21, 20) == 0xFFFF && p2.texel(22, 20) == 0);
    // Texels are 0 or 0xFFFF only.
    bool binary = true;
    for (const TerrainPass& q : p.passes)
        for (uint32_t y = 0; y < 32; ++y)
            for (uint32_t x = 0; x < 32; ++x) binary = binary && (q.texel(x, y) == 0 || q.texel(x, y) == 0xFFFF);
    CHECK(binary);
    // Index pattern: first quad of pass 0 = (0,1,33),(0,33,32).
    CHECK(p.passes[0].indices.size() >= 6 && p.passes[0].indices[0] == 0 && p.passes[0].indices[1] == 1 &&
          p.passes[0].indices[2] == 33 && p.passes[0].indices[3] == 0 && p.passes[0].indices[4] == 33 &&
          p.passes[0].indices[5] == 32);
    CHECK(!p.has_holes && p.rendered_indices.empty());

    // Lowest-index quad rule: a quad with corners {3,3,4,7} -> material 3 is lowest -> all 4 corners
    // white in pass 3; pass 4 and 7 white only at their own vertices.
    Terrain u = tiny_terrain();
    for (auto& m : u.material) m = 9;  // background material 9 (higher than all)
    u.material[5 * 32 + 5] = 3;
    u.material[5 * 32 + 6] = 3;
    u.material[6 * 32 + 5] = 4;
    u.material[6 * 32 + 6] = 7;
    TerrainMesh um;
    CHECK(build_terrain_mesh(u, um));
    const auto& up = um.patches[0].passes;
    CHECK(up.size() == 4);
    if (up.size() == 4) {
        CHECK(up[0].material == 3 && up[1].material == 4 && up[2].material == 7 && up[3].material == 9);
        // Pass 3: quad (5,5) corners all white + quads (4,4),(4,5),(5,4) etc where 3 is lowest.
        CHECK(up[0].texel(5, 5) == 0xFFFF && up[0].texel(6, 5) == 0xFFFF && up[0].texel(5, 6) == 0xFFFF &&
              up[0].texel(6, 6) == 0xFFFF);
        // Quad (4,4) has corners (4,4)=9,(5,4)=9,(4,5)=9,(5,5)=3 -> lowest 3 -> its corners white too.
        CHECK(up[0].texel(4, 4) == 0xFFFF);
        // Pass 4: quad (5,5) lowest is 3, quad (4,5) corners (4,5)=9,(5,5)=3 -> lowest 3; quad (4,6)
        // corners (4,6)=9,(5,6)=4,(4,7)=9,(5,7)=9 -> lowest 4 -> (4,6),(4,7),(5,7) white.
        // Quad (5,6) corners 4,7,9,9 -> lowest 4 -> (6,6) white in pass 4 too; (7,7) touches no 4.
        CHECK(up[1].texel(5, 6) == 0xFFFF && up[1].texel(4, 7) == 0xFFFF && up[1].texel(6, 6) == 0xFFFF &&
              up[1].texel(7, 7) == 0);
        // Pass 7: quads around (6,6): (5,5) lowest 3, (6,5): corners (6,5)=3 -> 3; (5,6): corners
        // (5,6)=4 -> 4; (6,6): corners 7,9,9,9 -> lowest 7 -> (7,6),(6,7),(7,7) white.
        CHECK(up[2].texel(6, 6) == 0xFFFF && up[2].texel(7, 7) == 0xFFFF && up[2].texel(5, 6) == 0);
        // Pass 9 (background) is lowest nowhere near the island except quads made only of 9.
        CHECK(up[3].texel(5, 5) == 0 && up[3].texel(0, 0) == 0xFFFF);
    }

    // Full coverage: material 7 flagged -> every quad it touches is fully white.
    u.materials[7].full_coverage = true;
    TerrainMesh fm;
    CHECK(build_terrain_mesh(u, fm));
    if (fm.patches[0].passes.size() == 4) {
        const TerrainPass& f7 = fm.patches[0].passes[2];
        CHECK(f7.texel(5, 5) == 0xFFFF && f7.texel(5, 6) == 0xFFFF && f7.texel(6, 5) == 0xFFFF && f7.texel(7, 7) == 0xFFFF);
    }

    // Not rendered: material 4 skipped, patch has holes, rendered list present.
    u.materials[4].not_rendered = true;
    TerrainMesh nm;
    CHECK(build_terrain_mesh(u, nm));
    CHECK(nm.patches[0].passes.size() == 3 && nm.patches[0].has_holes && nm.patches[0].rendered_indices.size() == 961 * 6);

    // Out-of-range material indices clamp to 63.
    Terrain c = tiny_terrain();
    c.material[0] = 200;
    TerrainMesh cm;
    CHECK(build_terrain_mesh(c, cm) && cm.patches[0].passes.back().material == 63);

    // Inconsistent grids are rejected, not read out of bounds.
    Terrain bad = tiny_terrain();
    bad.heights.resize(10);
    CHECK(!build_terrain_mesh(bad, cm, {}, alamo::serial_for, &err));
    Terrain bad2 = tiny_terrain();
    bad2.width = 33;
    CHECK(!build_terrain_mesh(bad2, cm));
}

void test_mesh_geometry() {
    Terrain t = tiny_terrain();
    // 2x2 patches with a slope in x: h = 2x raw units.
    t.width = t.height = 63;
    t.heights.assign(63 * 63, 0);
    t.material.assign(63 * 63, 0);
    t.brightness.assign(63 * 63, 0x80);
    for (uint32_t y = 0; y < 63; ++y)
        for (uint32_t x = 0; x < 63; ++x) t.heights[y * 63 + x] = static_cast<int16_t>(x * 32);
    TerrainMesh m;
    MeshOptions smooth;
    smooth.quantize_normals = false;
    CHECK(build_terrain_mesh(t, m, smooth));
    CHECK(m.patches.size() == 4 && m.patches_x == 2);
    const TerrainPatch& p1 = m.patches[1];  // px = 1, py = 0
    CHECK(p1.px == 1 && p1.py == 0);
    const TerrainVertex& v = p1.vertex(0, 0);
    CHECK(v.position[0] == 620.0f && v.position[1] == 0.0f && v.texcoord[0] == 620.0f);
    CHECK(approx(v.position[2], static_cast<float>(31 * 32) * kHeightScale));
    CHECK(v.color[0] == 0xFF && v.color[3] == 0xFF);  // header 11 not set -> white
    CHECK(p1.bounds.min.x == 620.0f && p1.bounds.max.x == 1240.0f && p1.bounds.max.y == 620.0f);
    // Blend UV: patch min -> 1/64, max -> 63/64.
    CHECK(approx(620.0f * p1.blend_tex_scale[0] + p1.blend_tex_offset[0], 1.0f / 64.0f));
    CHECK(approx(1240.0f * p1.blend_tex_scale[0] + p1.blend_tex_offset[0], 63.0f / 64.0f));
    CHECK(approx(620.0f * p1.blend_tex_scale[1] + p1.blend_tex_offset[1], 63.0f / 64.0f));
    // Smooth normal of an interior vertex of a constant slope dz/dx.
    const float dzdx = 32.0f * kHeightScale / kVertexSpacing;
    const TerrainVertex& vi = p1.vertex(5, 5);
    const float len = std::sqrt(dzdx * dzdx + 1.0f);
    CHECK(approx(vi.normal[0], -dzdx / len, 2e-3f) && approx(vi.normal[1], 0.0f, 1e-5f) && approx(vi.normal[2], 1.0f / len, 2e-3f));
    // Vertex brightness used when header 11 is set.
    t.header.use_vertex_brightness = true;
    TerrainMesh mb;
    CHECK(build_terrain_mesh(t, mb));
    CHECK(mb.patches[0].vertex(3, 3).color[0] == 0x80 && mb.patches[0].vertex(3, 3).color[3] == 0xFF);
    // Quantised: flat terrain -> entry 255 (0,0,1); normals are table entries.
    Terrain f = tiny_terrain();
    TerrainMesh fm;
    CHECK(build_terrain_mesh(f, fm));
    CHECK(fm.patches[0].vertex(4, 4).normal[2] == 1.0f);
    const auto& table = normal_table();
    CHECK(table[255][2] == 1.0f && approx(table[0][0], 1.0f) && approx(table[0][2], 0.0f));
    // Halton index 1: u = 0.5, v = 1/3 -> (-sqrt(2/3), 0, sqrt(1/3)).
    CHECK(approx(table[1][0], -std::sqrt(2.0f / 3.0f), 1e-5f) && approx(table[1][2], std::sqrt(1.0f / 3.0f), 1e-5f));
    for (const auto& e : table) CHECK(approx(e[0] * e[0] + e[1] * e[1] + e[2] * e[2], 1.0f, 1e-5f) && e[2] >= 0.0f);
    CHECK(quantize_normal({0, 0, 1}) == 255);
    // Determinism: parallel-style split (reverse order chunks) == serial.
    TerrainMesh par;
    const alamo::ParallelFor reverse = [](uint32_t count, uint32_t, const alamo::RangeFn& fn) {
        for (uint32_t i = count; i-- > 0;) fn(i, i + 1);
    };
    t.header.use_vertex_brightness = false;
    CHECK(build_terrain_mesh(t, par, {}, reverse));
    TerrainMesh ser;
    CHECK(build_terrain_mesh(t, ser));
    bool same = par.patches.size() == ser.patches.size();
    for (size_t i = 0; same && i < par.patches.size(); ++i) {
        same = par.patches[i].vertices == ser.patches[i].vertices && par.patches[i].passes.size() == ser.patches[i].passes.size();
        for (size_t k = 0; same && k < par.patches[i].passes.size(); ++k)
            same = par.patches[i].passes[k].indices == ser.patches[i].passes[k].indices &&
                   par.patches[i].passes[k].blend_texture == ser.patches[i].passes[k].blend_texture;
    }
    CHECK(same);
}

void test_texcoords() {
    TerrainMaterial m;
    m.tile_size = 200.0f;
    const float p[3] = {400.0f, 100.0f, 50.0f};
    auto uv = material_uv(m, p);
    CHECK(approx(uv[0], 2.0f) && approx(uv[1], 0.5f));
    // Rotation about Z by 90 deg: U = (0, 1/t), V = (-1/t, 0).
    m.rotation_z = 1.5707963f;
    auto u = m.tex_u(), v = m.tex_v();
    CHECK(approx(u[0], 0.0f) && approx(u[1], 1.0f / 200.0f) && approx(v[0], -1.0f / 200.0f) && approx(v[1], 0.0f));
    uv = material_uv(m, p);
    CHECK(approx(uv[0], 0.5f) && approx(uv[1], -2.0f));
    // Rotation about X by 90 deg (Z rotation 0): V = (0, 0, 1/t) -> v = z / t; U unchanged.
    m.rotation_z = 0.0f;
    m.rotation_x = 1.5707963f;
    u = m.tex_u();
    v = m.tex_v();
    CHECK(approx(u[0], 1.0f / 200.0f) && approx(v[1], 0.0f) && approx(v[2], 1.0f / 200.0f));
    // X first, then Z: rx = 90, rz = 90 -> V = (0, 0, 1/t) unchanged by Z; U = (0, 1/t, 0).
    m.rotation_z = 1.5707963f;
    v = m.tex_v();
    u = m.tex_u();
    CHECK(approx(v[0], 0.0f) && approx(v[1], 0.0f) && approx(v[2], 1.0f / 200.0f) && approx(u[1], 1.0f / 200.0f));
    TerrainMaterial e;
    CHECK(e.effective_normal_texture() == "Generic_Flat_B.tga");
}

void test_water() {
    CHECK(approx(shore_alpha(0.0f, -5.0f, 10.0f, 0.0f), 0.5f));
    CHECK(approx(shore_alpha(0.0f, -50.0f, 10.0f, 0.0f), 1.0f));
    CHECK(approx(shore_alpha(0.0f, 5.0f, 10.0f, 0.2f), 0.2f));
    CHECK(approx(shore_alpha(0.0f, -5.0f, 10.0f, 0.2f), 0.6f));
    Terrain t = tiny_terrain();
    t.width = t.height = 63;
    t.heights.assign(63 * 63, 400);  // 19.53 world units, above 0
    t.material.assign(63 * 63, 0);
    t.brightness.assign(63 * 63, 0xFF);
    t.header.water_type = WaterType::Water;
    t.header.water_height = 0.0f;
    t.header.shore_fade_depth = 10.0f;
    t.heights[10 * 63 + 40] = -102;  // ~-4.98 below the plane, in patch (1, 0)
    WaterSurface w;
    CHECK(build_water(t, w));
    CHECK(w.enabled && w.patches.size() == 1 && w.patches[0].px == 1 && w.patches[0].py == 0);
    CHECK(w.effect == "TerrainWater" && w.alpha_blend && w.targets.reflection && w.targets.refraction && w.targets.sky_cube);
    CHECK(w.targets.size == 512 && w.textures[2] == "w_generic_white.tga");
    if (!w.patches.empty()) {
        const WaterPatch& p = w.patches[0];
        CHECK(p.triangle_count() == 8);  // the 4 quads around the vertex
        const WaterVertex& v = p.vertex(40 - 31, 10);
        CHECK(v.position[2] == 0.0f && v.normal[2] == 1.0f && v.color1[3] == 0xFF);
        CHECK(v.color0[3] == static_cast<uint8_t>(std::lround(shore_alpha(0.0f, -102 * kHeightScale, 10.0f, 0.0f) * 255.0f)));
        CHECK(p.vertex(0, 0).color0[3] == 0);
    }
    // Types and options.
    t.header.water_type = WaterType::Lava;
    WaterOptions o;
    o.render_target_resolution = 4096;
    CHECK(build_water(t, w, o) && w.effect == "TerrainLava" && !w.alpha_blend && !w.targets.reflection && w.targets.size == 1024);
    t.header.water_type = WaterType::Ice;
    CHECK(build_water(t, w) && w.effect == "TerrainIce" && w.targets.reflection && !w.targets.refraction);
    t.header.water_type = WaterType::None;
    CHECK(build_water(t, w) && !w.enabled && w.patches.empty());
    // Constants.
    t.header.water_type = WaterType::Water;
    t.header.reflection_fraction = 0.25f;
    t.header.water_color[0] = 0.8f;
    t.water_scroll[0] = {true, 100.0f, 0.0f, 0.1f, 0.0f};
    CHECK(build_water(t, w));
    const WaterConstants c = water_constants(w, 10.0f);
    CHECK(approx(c.base_u[0], 0.01f) && approx(c.base_u[3], 1.0f) && approx(c.base_v[3], 0.0f) && std::signbit(c.base_v[0]));
    CHECK(approx(c.bump0_u[3], std::cos(0.785398f)) && approx(c.bump1_v[3], -std::sin(0.785398f)));
    CHECK(approx(c.reflect_c[0], 0.2f) && approx(c.refract_c[0], 0.6f) && c.reflect_c[3] == 1.0f);
    CHECK(approx(c.reflection_distortion[0], 0.01f));
}

void test_placements() {
    const MapData m = make_map(1, 5);
    const TypeResolver res = [](uint32_t crc, bool space, ResolvedType& out) {
        if (space) return false;
        if (crc == alamo::name_crc("Skirmish_Build_Pad")) {
            out = {"Skirmish_Build_Pad", "", 1.0f};
            return true;
        }
        if (crc == alamo::name_crc("TEAM_01_POWER_GENERATOR")) {
            out = {"Team_01_Power_Generator", "PowerGen.alo", 2.0f};
            return true;
        }
        return false;
    };
    const PlacementResult r = resolve_placements(m, res);
    CHECK(r.placements.size() == 3 && r.no_model.size() == 2 && r.unknown_type.empty());
    if (r.placements.size() == 3) {
        // object 2: yaw 60, Scale_Factor 2, mini 03 = 0.5 (health fraction, not applied to scale)
        const ResolvedPlacement& p = r.placements[1];
        CHECK(p.object_index == 2 && p.model_name == "PowerGen.alo" && p.scale == 2.0f);
        CHECK(p.initial_health_fraction == 0.5f && r.placements[0].initial_health_fraction == 1.0f);
        const auto o = p.transform.transform_point({1, 0, 0});
        CHECK(approx(o.x, 300.0f + 2.0f * 0.5f, 1e-3f) && approx(o.y, 150.0f + 2.0f * std::sqrt(3.0f) / 2.0f, 1e-3f) &&
              approx(o.z, 12.5f, 1e-4f));
    }
    PlacementOptions snap;
    snap.snap_to_terrain = true;
    const PlacementResult s = resolve_placements(m, res, snap);
    CHECK(!s.placements.empty() && approx(s.placements[0].position[2], m.terrain.sample_height(100.0f, 50.0f)));
    {  // Non-finite health fraction -> 1.0 with a warning.
        MapData bad = m;
        bad.objects[2].health_fraction = std::numeric_limits<float>::quiet_NaN();
        bad.objects[0].health_fraction = std::numeric_limits<float>::infinity();
        uint32_t warnings = 0;
        alamo::log::set_sink([&](alamo::log::Level lv, std::string_view, std::string_view) {
            warnings += lv == alamo::log::Level::Warn ? 1u : 0u;
        });
        const PlacementResult b = resolve_placements(bad, res);
        alamo::log::set_sink(nullptr);
        CHECK(b.placements.size() == 3 && warnings == 2);
        if (b.placements.size() == 3)
            CHECK(b.placements[0].initial_health_fraction == 1.0f && b.placements[1].initial_health_fraction == 1.0f &&
                  b.placements[1].scale == 2.0f);
    }
    const PlacementResult none = resolve_placements(m, TypeResolver{});
    CHECK(none.placements.empty() && none.unknown_type.size() == 5 && none.unknown_crcs.size() == 2);
    // sample_height on a vertex equals the vertex height and is linear on edges.
    const Terrain& t = m.terrain;
    CHECK(approx(t.sample_height(40.0f, 60.0f), t.height_at(2, 3)));
    CHECK(approx(t.sample_height(50.0f, 60.0f), 0.5f * (t.height_at(2, 3) + t.height_at(3, 3)), 1e-3f));
    CHECK(approx(t.sample_height(-100.0f, 1e9f), t.height_at(0, t.height - 1)));
}

void put32(std::vector<uint8_t>& b, uint32_t v) {
    const size_t at = b.size();
    b.resize(at + 4);
    std::memcpy(b.data() + at, &v, 4);
}

void test_hardening() {
    // Review regression: 48 bytes declaring a 1985^2 grid with no vertex data must fail without
    // allocating the grid.
    std::vector<uint8_t> b;
    put32(b, 1);
    put32(b, 0x80000000u | 40);  // world
    put32(b, 0x101);
    put32(b, 0x80000000u | 32);  // terrain
    put32(b, 0);
    put32(b, 24);                // header leaf
    auto mini32 = [&](uint8_t id, uint32_t v) {
        b.push_back(id);
        b.push_back(4);
        put32(b, v);
    };
    mini32(0x00, 1985);
    mini32(0x01, 1985);
    mini32(0x04, 1985u * 1985u);
    mini32(0x05, 64);
    CHECK(b.size() == 48);
    MapData r;
    std::string err;
    CHECK(!parse_ted(b, r, &err) && r.terrain.heights.empty() && r.passability.empty());
    std::printf("48-byte 1985^2 map rejected: %s\n", err.c_str());

    // Truncated vertex data and short passability are rejected.
    const MapData m = make_map(1, 11);
    CHECK(parse_ted(write_ted(m), r));
    {
        // Vertex payload cut to half via a crafted header: header 63x63, data for 32x32 only.
        MapData half = m;
        half.terrain.header.raw.clear();
        std::vector<uint8_t> hb = write_ted(half);
        // Patch the header width/height minis (u32 32) to 63 in place.
        for (size_t i = 0; i + 6 <= hb.size(); ++i) {
            if ((hb[i] == 0x00 || hb[i] == 0x01) && hb[i + 1] == 4) {
                uint32_t v = 0;
                std::memcpy(&v, hb.data() + i + 2, 4);
                if (v == 32) {
                    const uint32_t nv = 63;
                    std::memcpy(hb.data() + i + 2, &nv, 4);
                }
            }
        }
        MapData hr;
        CHECK(!parse_ted(hb, hr, &err));
    }

    // environments_only: a corrupted terrain chunk fails the full read but not the environment read.
    std::vector<uint8_t> cb = write_ted(m);
    std::vector<ChunkView> top, world;
    CHECK(read_chunks(cb, top));
    bool corrupted = false;
    for (const ChunkView& t : top) {
        if (t.id != 1 || !read_chunks(t.payload, world)) continue;
        for (const ChunkView& w : world)
            if (w.id == 0x101) {
                const size_t off = static_cast<size_t>(w.payload.data() - cb.data());
                std::memset(cb.data() + off, 0xEE, w.payload.size());
                corrupted = true;
            }
    }
    CHECK(corrupted);
    MapData full, env;
    CHECK(!parse_ted(cb, full, &err));
    ParseOptions eo;
    eo.environments_only = true;
    CHECK(parse_ted(cb, env, &err, eo));
    CHECK(env.environments.environments.size() == 2 && env.environments.start_index == 1 && !env.is_space() &&
          env.terrain.width == 0 && env.objects.empty() && env.start_positions.empty() && env.info.planet_name == "Testworld");
    // Short passability payload is rejected.
    // (the world chunk is the last top-level chunk write_ted emits: append a 16-byte 0x10A to it)
    MapData sp = m;
    sp.passability_present = false;
    std::vector<uint8_t> sb = write_ted(sp);
    std::vector<ChunkView> top2;
    CHECK(read_chunks(sb, top2) && !top2.empty() && top2.back().id == 1);
    const size_t world_hdr = static_cast<size_t>(top2.back().payload.data() - sb.data()) - 4;
    put32(sb, 0x10A);
    put32(sb, 16);
    for (int i = 0; i < 4; ++i) put32(sb, 1);
    uint32_t wsize = 0;
    std::memcpy(&wsize, sb.data() + world_hdr, 4);
    wsize += 24;
    std::memcpy(sb.data() + world_hdr, &wsize, 4);
    MapData spr;
    CHECK(!parse_ted(sb, spr, &err) && err.find("passability") != std::string::npos);
}

void fuzz_one(const std::vector<uint8_t>& bytes, uint32_t& parsed) {
    MapData r;
    std::string err;
    if (parse_ted(bytes, r, &err)) {
        ++parsed;
        if (r.terrain.present && r.terrain.width <= 125) {
            TerrainMesh m;
            build_terrain_mesh(r.terrain, m);
            WaterSurface w;
            build_water(r.terrain, w);
        }
        (void)write_ted(r);
    }
}

void test_fuzz() {
    const std::vector<uint8_t> base = write_ted(make_map(1, 9));
    Lcg rng(1234);
    uint32_t parsed = 0;
    const uint32_t iters = 4000;
    for (uint32_t it = 0; it < iters; ++it) {
        std::vector<uint8_t> b = base;
        const uint32_t mode = rng.below(5);
        if (mode == 0) {  // random byte flips
            const uint32_t n = 1 + rng.below(16);
            for (uint32_t k = 0; k < n; ++k) b[rng.below(static_cast<uint32_t>(b.size()))] = static_cast<uint8_t>(rng.next());
        } else if (mode == 1) {  // truncation
            b.resize(rng.below(static_cast<uint32_t>(b.size())));
        } else if (mode == 2) {  // random u32 overwrite (hits chunk sizes / counts)
            const uint32_t at = rng.below(static_cast<uint32_t>(b.size() - 4));
            const uint32_t v = rng.below(2) ? rng.next() : rng.below(4096) | (rng.below(2) ? 0x80000000u : 0u);
            std::memcpy(b.data() + at, &v, 4);
        } else if (mode == 3) {  // insert garbage
            const uint32_t at = rng.below(static_cast<uint32_t>(b.size()));
            const uint32_t n = 1 + rng.below(64);
            std::vector<uint8_t> g(n);
            for (auto& x : g) x = static_cast<uint8_t>(rng.next());
            b.insert(b.begin() + at, g.begin(), g.end());
        } else {  // flip size high bits of a plausible chunk header
            for (uint32_t k = 0; k < 4; ++k) {
                const uint32_t at = rng.below(static_cast<uint32_t>(b.size()));
                b[at] ^= static_cast<uint8_t>(1u << rng.below(8));
            }
        }
        fuzz_one(b, parsed);
    }
    // Fully random inputs.
    for (uint32_t it = 0; it < 2000; ++it) {
        std::vector<uint8_t> b(rng.below(512));
        for (auto& x : b) x = static_cast<uint8_t>(rng.next());
        fuzz_one(b, parsed);
    }
    // Malformed headers that must fail cleanly.
    MapData big = make_map(1, 2);
    big.terrain.width = big.terrain.height = 2101;  // > kMaxGridVerts
    big.passability_present = false;
    big.terrain.heights.clear();
    big.terrain.material.clear();
    big.terrain.brightness.clear();
    MapData r;
    CHECK(!parse_ted(write_ted(big), r));
    CHECK(!parse_ted({}, r));
    std::printf("fuzz: %u inputs, %u parsed without error, no crash\n", iters + 2000, parsed);
}

// ---------------------------------------------------------------------------- decorations
float mesh_area_xy(const DecorationMesh& m) {
    double a = 0;
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        const float* p0 = m.vertices[m.indices[i]].position;
        const float* p1 = m.vertices[m.indices[i + 1]].position;
        const float* p2 = m.vertices[m.indices[i + 2]].position;
        a += 0.5 * ((static_cast<double>(p1[0]) - p0[0]) * (static_cast<double>(p2[1]) - p0[1]) -
                    (static_cast<double>(p2[0]) - p0[0]) * (static_cast<double>(p1[1]) - p0[1]));
    }
    return static_cast<float>(a);
}

TerrainDecal sample_decal(float x, float y) {
    TerrainDecal d;
    d.permanent = true;
    d.render_mode = 3;
    d.position[0] = x;
    d.position[1] = y;
    d.width = d.height = 15.0f;
    d.slot_column = 0;
    d.slot_row = 1;
    return d;
}

TerrainTrack sample_track() {
    TerrainTrack t;
    t.flags = 4;
    t.sample_spacing = 50;
    t.points = {{100, 100, 0}, {300, 100, 0}, {500, 100, 0}};
    t.widths = {20, 20, 20};
    t.texture = "w_waves00.tga";
    t.profile = {0, 1, 0};
    t.profile_present = true;
    t.first_row_alpha = 1.0f;
    t.last_row_alpha = 1.0f;
    t.u_speed = 0.25f;
    t.v_speed = -0.5f;
    return t;
}

void test_decoration_records() {
    MapData m = make_map(1, 21);
    m.terrain.decals = {sample_decal(100, 100), sample_decal(200, 220)};
    m.terrain.decals[1].render_mode = 6;
    m.terrain.decals[1].fade_time = 4.0f;
    m.terrain.decals[1].rotation = 0.5f;
    m.terrain.tracks = {sample_track(), sample_track()};
    m.terrain.tracks[1].height_mode = 0;
    m.terrain.tracks[1].profile_present = false;
    m.terrain.tracks[1].profile = {1, 1, 1};
    m.terrain.tracks[1].clamp_v = true;
    const auto bytes = write_ted(m);
    MapData r;
    std::string err;
    CHECK(parse_ted(bytes, r, &err));
    CHECK(r.terrain.decals.size() == 2 && r.terrain.tracks.size() == 2);
    if (r.terrain.decals.size() == 2 && r.terrain.tracks.size() == 2) {
        const TerrainDecal& d = r.terrain.decals[1];
        CHECK(d.render_mode == 6 && d.fade_time == 4.0f && d.rotation == 0.5f && d.width == 15.0f && d.slot_row == 1 &&
              d.position[1] == 220.0f && d.permanent);
        const TerrainTrack& t = r.terrain.tracks[0];
        CHECK(t.points.size() == 3 && t.points[2][0] == 500.0f && t.widths.size() == 3 && t.texture == "w_waves00.tga");
        CHECK(t.profile == std::vector<float>({0, 1, 0}) && t.profile_present && t.flags == 4 && t.u_speed == 0.25f);
        const TerrainTrack& w = r.terrain.tracks[1];
        CHECK(w.on_water() && w.clamp_v && !w.clamp_u && !w.profile_present && w.profile.size() == 3);
    }
    CHECK(write_ted(r) == bytes);
    // Truncated / malformed decal and track chunks fail the read cleanly.
    {
        std::vector<uint8_t> b = bytes;
        std::vector<ChunkView> top, world, terr;
        CHECK(read_chunks(b, top));
        for (const ChunkView& tc : top) {
            if (tc.id != 1 || !read_chunks(tc.payload, world)) continue;
            for (const ChunkView& wc : world) {
                if (wc.id != 0x101 || !read_chunks(wc.payload, terr)) continue;
                for (const ChunkView& c : terr)
                    if (c.id == 9 && c.payload.size() > 40) {
                        // Corrupt the size of a mini inside the first track's 0x101 leaf.
                        const size_t off = static_cast<size_t>(c.payload.data() - b.data());
                        b[off + 16 + 1] = 0xFF;
                    }
            }
        }
        MapData br;
        CHECK(!parse_ted(b, br, &err));
    }
}

void test_decal_geometry() {
    Terrain t = tiny_terrain();
    DecalGeometry g;
    TerrainDecal d = sample_decal(100.0f, 100.0f);
    CHECK(build_decal_geometry(t, d, g));
    CHECK(approx(mesh_area_xy(g.mesh), 225.0f, 1e-2f));
    bool lifted = true, uv_in_slot = true;
    for (const LitVertex& v : g.mesh.vertices) {
        lifted = lifted && v.position[2] == kDecalLift;
        uv_in_slot = uv_in_slot && v.texcoord[0] >= -1e-5f && v.texcoord[0] <= 0.25f + 1e-5f && v.texcoord[1] >= 0.25f - 1e-5f &&
                     v.texcoord[1] <= 0.5f + 1e-5f;
        // MODULATE white decal at alpha 1: rgb 0, alpha 255.
        lifted = lifted && v.color[0] == 0 && v.color[2] == 0 && v.color[3] == 255;
    }
    CHECK(lifted && uv_in_slot);
    CHECK(g.mode == RenderMode::Modulate && g.mesh.bounds.min.x >= 92.5f - 1e-3f && g.mesh.bounds.max.x <= 107.5f + 1e-3f);
    // Corner (centre - X/2 - Y/2) with Y = -height maps to local (0, 0) = slot origin (0, 0.25).
    const auto f = DecorationFrame::make(100, 100, 15, 15, 0);
    const auto l = f.local(92.5f, 107.5f);
    CHECK(approx(l[0], 0.0f) && approx(l[1], 0.0f));
    // Rotated decal straddling grid lines: same area, all vertices within the rectangle.
    d.rotation = 0.7f;
    d.position[0] = 120.0f;  // on a grid line
    d.width = 33.0f;
    d.height = 21.0f;
    CHECK(build_decal_geometry(t, d, g));
    CHECK(approx(mesh_area_xy(g.mesh), 33.0f * 21.0f, 0.05f));
    // Sloped terrain: vertices follow the surface + 1.
    Terrain s = tiny_terrain();
    for (uint32_t y = 0; y < 32; ++y)
        for (uint32_t x = 0; x < 32; ++x) s.heights[y * 32 + x] = static_cast<int16_t>(x * 40 + ((x * 7 + y * 3) % 5) * 30);
    CHECK(build_decal_geometry(s, d, g));
    float worst = 0;
    for (const LitVertex& v : g.mesh.vertices)
        worst = std::max(worst, std::fabs(v.position[2] - (s.sample_height(v.position[0], v.position[1]) + kDecalLift)));
    CHECK(worst < 1e-3f);
    CHECK(approx(mesh_area_xy(g.mesh), 33.0f * 21.0f, 0.05f));
    // Fade.
    TerrainDecal fd = sample_decal(0, 0);
    fd.fade_time = 10.0f;
    CHECK(approx(decal_fade(fd, 5.0f).alpha, 0.5f) && !decal_fade(fd, 5.0f).dead && decal_fade(fd, 11.0f).dead);
    CHECK(decal_fade(sample_decal(0, 0), 1e6f).alpha == 1.0f && !decal_fade(sample_decal(0, 0), 1e6f).dead);
    const uint8_t tan[3] = {1, 2, 3};
    CHECK(decal_vertex_color(RenderMode::Additive, 0.5f, tan)[0] == 128 && decal_vertex_color(RenderMode::Modulate, 0.5f, tan)[1] == 128);
    CHECK(decal_vertex_color(RenderMode::DecalBumpAlpha, 1.0f, tan)[2] == 1 && decal_vertex_color(RenderMode::DecalBumpAlpha, 1.0f, tan)[0] == 3);
    set_decal_alpha(g, 0.0f);
    CHECK(g.mesh.vertices.empty() || g.mesh.vertices[0].color[3] == 0);
    // Off the map / degenerate.
    CHECK(!build_decal_geometry(t, sample_decal(-500, -500), g) && g.mesh.empty());
    TerrainDecal z = sample_decal(100, 100);
    z.width = 0;
    CHECK(!build_decal_geometry(t, z, g));
    // Render mode strings.
    CHECK(render_mode_from_string("decal_bump_alpha") == RenderMode::DecalBumpAlpha &&
          render_mode_from_string(" DIFFUSE_ALPHA ") == RenderMode::DiffuseAlpha && render_mode_from_string("bogus") == RenderMode::Opaque &&
          render_mode_from_u32(99) == RenderMode::Opaque);
}

// Atlas UVs against the rule captured from the original (g1f t05, DECORATIONS 4.3): with
// X^ = (cos t, sin t), Y^ = X^ turned +90 deg about +z, lx = (P - C).X^, ly = (P - C).Y^:
// u = (col + 0.5 + lx/w)/4 - 1, v = (row + 0.5 - ly/h)/4 - 1, compared mod 1 (WRAP addressing).
float wrap_diff(float a, float b) {
    const double d = static_cast<double>(a) - static_cast<double>(b);
    return static_cast<float>(std::fabs(d - std::round(d)));
}

void test_decal_atlas_uv() {
    const Terrain t = tiny_terrain();
    const float headings[] = {0.0f, 0.6f, 2.3f, -1.1f, 3.9f};
    const uint32_t slots[][2] = {{0, 3}, {1, 3}, {2, 1}, {3, 0}};
    float worst_vertex = 0.0f, worst_corner = 0.0f;
    size_t checked = 0;
    for (const float th : headings)
        for (const auto& sl : slots) {
            TerrainDecal d = sample_decal(150.0f, 170.0f);
            d.width = 25.0f;
            d.height = 30.0f;
            d.rotation = th;
            d.slot_column = sl[0];
            d.slot_row = sl[1];
            DecalGeometry g;
            CHECK(build_decal_geometry(t, d, g) && !g.mesh.vertices.empty());
            const float c = std::cos(th), s = std::sin(th);
            const float xh[2] = {c, s}, yh[2] = {-s, c};  // Y^ = X^ turned +90 deg about +z
            auto rule = [&](float px, float py, float& u, float& v) {
                const float dx = px - d.position[0], dy = py - d.position[1];
                const float lx = dx * xh[0] + dy * xh[1], ly = dx * yh[0] + dy * yh[1];
                u = (static_cast<float>(d.slot_column) + 0.5f + lx / d.width) / 4.0f - 1.0f;
                v = (static_cast<float>(d.slot_row) + 0.5f - ly / d.height) / 4.0f - 1.0f;
            };
            auto matrix = [&](float px, float py, float pz, float& u, float& v) {
                u = g.atlas_u[0] * px + g.atlas_u[1] * py + g.atlas_u[2] * pz + g.atlas_u[3];
                v = g.atlas_v[0] * px + g.atlas_v[1] * py + g.atlas_v[2] * pz + g.atlas_v[3];
            };
            for (const LitVertex& vx : g.mesh.vertices) {
                float u = 0, v = 0;
                rule(vx.position[0], vx.position[1], u, v);
                worst_vertex = std::max({worst_vertex, wrap_diff(vx.texcoord[0], u), wrap_diff(vx.texcoord[1], v)});
                ++checked;
            }
            // The four corners: (-X,+Y) -> slot (u min, v min), (+X,+Y) -> (u max, v min),
            // (+X,-Y) -> (u max, v max), (-X,-Y) -> (u min, v max).
            const float sx[4] = {-1, 1, 1, -1}, sy[4] = {1, 1, -1, -1};
            for (int k = 0; k < 4; ++k) {
                const float px = d.position[0] + 0.5f * (sx[k] * d.width * xh[0] + sy[k] * d.height * yh[0]);
                const float py = d.position[1] + 0.5f * (sx[k] * d.width * xh[1] + sy[k] * d.height * yh[1]);
                float u = 0, v = 0, ru = 0, rv = 0;
                matrix(px, py, kDecalLift, u, v);
                rule(px, py, ru, rv);
                const float eu = (static_cast<float>(d.slot_column) + (sx[k] > 0 ? 1.0f : 0.0f)) / 4.0f;
                const float ev = (static_cast<float>(d.slot_row) + (sy[k] > 0 ? 0.0f : 1.0f)) / 4.0f;
                worst_corner = std::max({worst_corner, wrap_diff(u, eu), wrap_diff(v, ev), wrap_diff(u, ru), wrap_diff(v, rv)});
            }
        }
    std::printf("decal atlas uv: %zu vertices, worst |uv - rule| mod 1 = %g, worst corner = %g\n", checked,
                static_cast<double>(worst_vertex), static_cast<double>(worst_corner));
    CHECK(checked > 0 && worst_vertex < 1e-4f && worst_corner < 1e-4f);
}

void test_blob_geometry() {
    Terrain t = tiny_terrain();
    ShadowBlob b;
    b.position[0] = 200.0f;
    b.position[1] = 200.0f;
    b.width = b.height = 40.0f;
    BlobGeometry g;
    CHECK(build_blob_geometry(t, b, RenderMode::Modulate, g));
    // AABB 180..220 -> vertices 9..11 on each axis = 3x3, 8 triangles.
    CHECK(g.vertices.size() == 9 && g.indices.size() == 24);
    if (g.vertices.size() == 9) {
        const PrelitVertex& c = g.vertices[4];
        CHECK(c.position[0] == 200.0f && approx(c.texcoord[0], 0.5f) && approx(c.texcoord[1], 0.5f) && c.position[2] == kBlobLift);
        CHECK(c.color[0] == 0 && c.color[3] == 255);  // white MODULATE -> 0
    }
    const float grey[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    CHECK(blob_vertex_color(RenderMode::Modulate, grey, 0.5f)[0] == 255);   // 0.5 + 0.5
    CHECK(blob_vertex_color(RenderMode::Additive, grey, 0.5f)[3] == 64);    // alpha^2
    const auto mats = builtin_blob_materials();
    CHECK(mats.size() == 2 && mats[1].mode == RenderMode::Additive && find_blob_material(mats, "nothing") == 0);
}

void test_track_geometry() {
    Terrain t = tiny_terrain();
    t.header.water_height = -3.0f;
    TerrainTrack k = sample_track();
    const auto s = sample_track_spline(k);
    CHECK(s.size() == 9);  // 400 units every 50 (0..350) + the end point
    if (s.size() == 9) {
        CHECK(approx(s[1].arc, 50.0f) && approx(s[1].position[0], 150.0f, 0.05f) && approx(s[8].arc, 400.0f, 1e-2f));
        CHECK(approx(s[3].tangent[0], 1.0f) && approx(s[3].position[1], 100.0f));
    }
    TrackGeometry g;
    CHECK(build_track_geometry(t, k, g));
    CHECK(g.rows == 9 && g.columns == 3 && g.mesh.vertices.size() == 27 && g.mesh.indices.size() == 8 * 2 * 6);
    CHECK(g.type == DecorationType::LandTrack && g.mode == RenderMode::DiffuseAlpha && g.texture == "w_waves00.tga");
    if (g.mesh.vertices.size() == 27) {
        const LitVertex& a = g.mesh.vertices[3 * 2 + 0];
        const LitVertex& c = g.mesh.vertices[3 * 2 + 1];
        CHECK(approx(a.position[1], 110.0f, 1e-3f) && approx(c.position[1], 100.0f, 1e-3f) && a.position[2] == 0.0f);
        CHECK(a.color[3] == 0 && c.color[3] == 255 && approx(c.texcoord[1], 0.5f) && approx(c.texcoord[0], 100.0f / 400.0f, 1e-4f));
        CHECK(approx(g.mesh.vertices[26].texcoord[0], 1.0f, 1e-4f));
    }
    CHECK(mesh_area_xy(g.mesh) > 0.0f);  // CCW seen from above
    CHECK(approx(mesh_area_xy(g.mesh), 400.0f * 20.0f, 1.0f));
    // Water mode, repeat length, first/last row multipliers.
    k.height_mode = 0;
    k.repeat_length = 100.0f;
    k.first_row_alpha = 0.0f;
    CHECK(build_track_geometry(t, k, g) && g.type == DecorationType::WaterTrack);
    CHECK(g.mesh.vertices[1].position[2] == -2.0f && g.mesh.vertices[1].color[3] == 0 && g.mesh.vertices[4].color[3] == 255);
    CHECK(approx(g.mesh.vertices.back().texcoord[0], 4.0f, 1e-3f));
    const auto off = track_uv_offset(k, 3.0f);
    CHECK(approx(off[0], 0.75f) && approx(off[1], 0.5f));
    // No ribbon flag / too few points -> nothing.
    TerrainTrack nf = sample_track();
    nf.flags = 3;
    CHECK(!build_track_geometry(t, nf, g));
    nf = sample_track();
    nf.points.resize(1);
    CHECK(!build_track_geometry(t, nf, g));
    // Curved track: samples are ~evenly spaced by arc length.
    TerrainTrack cv = sample_track();
    cv.points = {{100, 100, 0}, {300, 300, 0}, {500, 100, 0}, {600, 400, 0}};
    cv.sample_spacing = 25;
    const auto cs = sample_track_spline(cv);
    bool even = cs.size() > 10;
    for (size_t i = 1; i + 1 < cs.size(); ++i) {
        const float d = std::hypot(cs[i].position[0] - cs[i - 1].position[0], cs[i].position[1] - cs[i - 1].position[1]);
        even = even && d > 22.0f && d <= 25.5f;
    }
    CHECK(even);
    // Hostile spacing is capped.
    cv.sample_spacing = 1;
    const size_t capped = sample_track_spline(cv, 100).size();
    CHECK(capped >= 98 && capped <= 100);
}

// Review finding: a ~1.6 KB .ted track (2 points 1e6 apart, ribbon flag, a 0x105 profile of 256
// `09` minis) used to build 16384 x 256 = 4.2M vertices / 25M indices, and a file can repeat it.
void test_track_budget() {
    uint32_t warnings = 0;
    alamo::log::set_sink([&](alamo::log::Level lv, std::string_view, std::string_view) {
        warnings += lv == alamo::log::Level::Warn ? 1u : 0u;
    });
    TerrainTrack evil;
    evil.flags = 4;
    evil.sample_spacing = 1;
    evil.points = {{0, 0, 0}, {1.0e6f, 0, 0}};
    evil.widths = {20, 20};
    evil.texture = "w_waves00.tga";
    evil.profile.assign(256, 1.0f);
    evil.profile_present = true;
    MapData m = make_map(1, 21);
    m.terrain.tracks.assign(64, evil);
    const auto bytes = write_ted(m);
    MapData r;
    std::string err;
    CHECK(parse_ted(bytes, r, &err) && r.terrain.tracks.size() == 64);
    if (r.terrain.tracks.size() == 64) {
        CHECK(r.terrain.tracks[0].profile.size() == 256);
        TrackGeometry g;
        CHECK(build_track_geometry(r.terrain, r.terrain.tracks[0], g));
        CHECK(g.columns == kMaxTrackProfile && g.mesh.vertices.size() <= kMaxTrackVertices &&
              g.mesh.indices.size() <= kMaxTrackIndices && g.rows >= 2);
        CHECK(approx(g.length, 1.0e6f, 10.0f));  // clipped by coarser sampling, still spans the track
        CHECK(warnings >= 2);                   // profile clip + row budget
        std::vector<TrackGeometry> all;
        const size_t built = build_map_track_geometry(r.terrain, all);
        size_t verts = 0, idx = 0;
        for (const TrackGeometry& k : all) {
            verts += k.mesh.vertices.size();
            idx += k.mesh.indices.size();
        }
        CHECK(all.size() == 64 && built >= 1 && built < 64);
        CHECK(verts <= kMaxMapTrackVertices && idx <= kMaxMapTrackIndices);
        CHECK(all.back().mesh.vertices.empty() && all.back().rows == 0);
    }
    alamo::log::set_sink(nullptr);
}

// Review finding: decal geometry had no budget. A .ted with 10k decals of 1e6 x 1e6 (~320 KB)
// asked for the full grid per decal (tens of GB over the map).
void test_decal_budget() {
    uint32_t warnings = 0;
    alamo::log::set_sink([&](alamo::log::Level lv, std::string_view, std::string_view) {
        warnings += lv == alamo::log::Level::Warn ? 1u : 0u;
    });
    Terrain t = tiny_terrain();
    DecalGeometry g;
    // Oversized (by area, and a thin one by AABB cell count) -> rejected before clipping.
    TerrainDecal huge = sample_decal(300, 300);
    huge.width = huge.height = 1.0e6f;
    CHECK(!build_decal_geometry(t, huge, g) && g.mesh.empty() && g.mesh.vertices.empty());
    TerrainDecal thin = sample_decal(300, 300);
    thin.width = 1.0e6f;
    thin.height = 1.0f;
    CHECK(!build_decal_geometry(t, thin, g) && g.mesh.vertices.empty());
    TerrainDecal near_max = sample_decal(300, 300);
    near_max.width = 3.0e38f;
    near_max.height = 1.0f;  // finite determinant, AABB ~2e38 wide
    near_max.rotation = 0.7f;
    CHECK(!build_decal_geometry(t, near_max, g) && g.mesh.vertices.empty());
    CHECK(warnings >= 3);
    // Largest accepted: 600 x 600 on the 31 x 31-cell grid = 31 x 31 = 961 cells <= kMaxDecalCells.
    TerrainDecal big = sample_decal(310, 310);
    big.width = big.height = 600.0f;
    CHECK(build_decal_geometry(t, big, g));
    CHECK(g.mesh.vertices.size() <= kMaxDecalVertices && g.mesh.indices.size() <= kMaxDecalIndices);
    CHECK(approx(mesh_area_xy(g.mesh), 600.0f * 600.0f, 5.0f));

    // 10k hostile decals through the .ted reader -> nothing built, out[i] <-> decals[i].
    MapData m = make_map(1, 21);
    m.terrain.decals.assign(10000, huge);
    const auto bytes = write_ted(m);
    MapData r;
    std::string err;
    CHECK(parse_ted(bytes, r, &err) && r.terrain.decals.size() == 10000);
    std::printf("decal budget: 10000 x 1e6 decals = %zu bytes of .ted\n", bytes.size());
    warnings = 0;
    std::vector<DecalGeometry> all;
    CHECK(build_map_decal_geometry(r.terrain, all) == 0 && all.size() == 10000);
    bool all_empty = true;
    for (const DecalGeometry& d : all) all_empty = all_empty && d.mesh.vertices.empty() && d.mesh.indices.empty();
    CHECK(all_empty);
    CHECK(warnings >= 1 && warnings <= 16);  // capped per-decal logs + one summary

    // Per-map budget: 256 accepted 961-cell decals -> some built, the rest skipped (empty).
    Terrain bt = tiny_terrain();
    bt.decals.assign(256, big);
    bt.decals[1] = sample_decal(100, 100);  // small one early: always built
    const size_t built = build_map_decal_geometry(bt, all);
    size_t verts = 0, idx = 0;
    for (const DecalGeometry& d : all) {
        verts += d.mesh.vertices.size();
        idx += d.mesh.indices.size();
    }
    std::printf("decal budget: %zu of 256 large decals built (%zu vertices / %zu indices; caps %u / %u)\n", built, verts, idx,
                kMaxMapDecalVertices, kMaxMapDecalIndices);
    CHECK(all.size() == 256 && built >= 2 && built < 256 && !all[1].mesh.empty());
    CHECK(verts <= kMaxMapDecalVertices && idx <= kMaxMapDecalIndices);
    CHECK(all.back().mesh.vertices.empty() && all.back().mode == RenderMode::Modulate);
    // Same decals built one by one match the map builder for the built ones.
    DecalGeometry one;
    CHECK(build_decal_geometry(bt, bt.decals[0], one) && one.mesh.vertices.size() == all[0].mesh.vertices.size() &&
          one.mesh.indices == all[0].mesh.indices);
    alamo::log::set_sink(nullptr);
}

// Review findings: NaN -> size_t UB in the spline (finite points whose arc length overflows), and
// the arc table / ribbons allocated before any budget check.
void test_track_malformed() {
    uint32_t warnings = 0;
    alamo::log::set_sink([&](alamo::log::Level lv, std::string_view, std::string_view) {
        warnings += lv == alamo::log::Level::Warn ? 1u : 0u;
    });
    Terrain t = tiny_terrain();
    TrackGeometry g;
    // ~40 finite points alternating x = +-1e37: the arc length overflows to +inf.
    TerrainTrack ov = sample_track();
    ov.points.clear();
    ov.widths.clear();
    for (int i = 0; i < 40; ++i) {
        ov.points.push_back({(i & 1) ? 1.0e37f : -1.0e37f, 0.0f, 0.0f});
        ov.widths.push_back(20.0f);
    }
    CHECK(sample_track_spline(ov).empty());
    CHECK(!build_track_geometry(t, ov, g) && g.mesh.vertices.empty() && g.rows == 0);
    // Near FLT_MAX: the spline itself overflows (inf - inf = NaN chords).
    ov.points = {{-3.0e38f, 3.0e38f, 0}, {3.0e38f, -3.0e38f, 0}, {-3.0e38f, -3.0e38f, 0}};
    ov.widths = {20, 20, 20};
    CHECK(sample_track_spline(ov).empty() && !build_track_geometry(t, ov, g));
    // Non-finite points are dropped, still fine.
    ov = sample_track();
    ov.points.insert(ov.points.begin() + 1, {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f});
    CHECK(sample_track_spline(ov).size() == 9);
    CHECK(warnings >= 2);
    // max_samples == 2 (spacing = total / 0): just the end points, finite.
    {
        const auto s = sample_track_spline(sample_track(), 2);
        CHECK(s.size() == 2);
        if (s.size() == 2) CHECK(s[0].arc == 0.0f && approx(s[1].arc, 400.0f, 1e-2f) && std::isfinite(s[1].position[0]));
    }
    // Point cap, checked before the arc table is allocated (a 15 MB track = ~1.25M points).
    TerrainTrack many = sample_track();
    many.points.resize(static_cast<size_t>(kMaxTrackPoints) + 1);
    for (size_t i = 0; i < many.points.size(); ++i) many.points[i] = {static_cast<float>(i % 600), static_cast<float>(i / 600) * 20.0f, 0};
    many.widths.assign(many.points.size(), 10.0f);
    warnings = 0;
    CHECK(sample_track_spline(many).empty() && !build_track_geometry(t, many, g) && warnings >= 1);
    many.points.resize(kMaxTrackPoints);
    many.widths.resize(kMaxTrackPoints);
    CHECK(build_track_geometry(t, many, g) && g.mesh.vertices.size() <= kMaxTrackVertices);
    {
        TerrainTrack giant = sample_track();
        giant.points.assign(1250000, {100.0f, 100.0f, 0.0f});
        giant.points.back() = {500.0f, 100.0f, 0.0f};
        Terrain gt = tiny_terrain();
        gt.tracks.assign(3, giant);
        gt.tracks[1] = sample_track();
        std::vector<TrackGeometry> all;
        CHECK(build_map_track_geometry(gt, all) == 1 && all.size() == 3 && all[0].mesh.vertices.empty() && !all[1].mesh.empty());
    }
    // Budget checked before the ribbon is built: the over-budget track is never allocated, and the
    // running totals never pass the caps.
    {
        TerrainTrack wide = sample_track();
        wide.points = {{0, 0, 0}, {1.0e6f, 0, 0}};
        wide.widths = {20, 20};
        wide.sample_spacing = 1;
        wide.profile.assign(kMaxTrackProfile, 1.0f);
        Terrain wt = tiny_terrain();
        wt.tracks.assign(8, wide);
        std::vector<TrackGeometry> all;
        const size_t built = build_map_track_geometry(wt, all);
        size_t verts = 0;
        for (const TrackGeometry& k : all) verts += k.mesh.vertices.size();
        CHECK(built == kMaxMapTrackVertices / kMaxTrackVertices && verts <= kMaxMapTrackVertices);
        CHECK(all.back().mesh.vertices.capacity() == 0 && all.back().rows == 0 && all.back().columns == 0);
    }
    alamo::log::set_sink(nullptr);
}

void test_dynamic_track() {
    DynamicTrackParams p;
    p.width = 5.0f;
    p.segment_length = 10.0f;
    p.fade_begin_distance = 40.0f;
    p.fade_end_distance = 80.0f;
    p.fade_distance_per_second = 0.0f;
    p.mode = RenderMode::DiffuseAlpha;
    DynamicTrackGenerator g(p);
    for (int i = 0; i <= 20; ++i) g.update(3.0f * static_cast<float>(i), 50.0f, 0.1f);
    // Moved 60 units: points at 0, 10, ..., 60 (the head at 60 coincides with the last drop).
    const auto& pts = g.points();
    CHECK(pts.size() >= 7 && approx(g.head_distance(), 60.0f, 1e-3f));
    CHECK(approx(pts[1].position[0], 10.0f, 1e-3f) && approx(pts[1].distance, 10.0f, 1e-3f));
    CHECK(approx(g.point_alpha(pts.size() - 1), 1.0f));
    // 60 behind the head: t = (60 - 40) / 40 = 0.5 -> 1 - smoothstep = 0.5.
    CHECK(approx(g.point_alpha(0), 0.5f, 1e-4f));
    CHECK(approx(smoothstep01(0.25f), 0.15625f));
    Terrain t = tiny_terrain();
    DecorationMesh m;
    CHECK(g.build(t, m));
    CHECK(!m.empty() && approx(mesh_area_xy(m), 60.0f * 10.0f, 0.05f));
    bool ok = true;
    for (const LitVertex& v : m.vertices) ok = ok && v.position[2] == kDynamicTrackLift && (v.position[1] == 55.0f || v.position[1] == 45.0f);
    CHECK(ok);
    // Direction colour: +x -> R = 255, G = 127.
    CHECK(m.vertices[0].color[2] == 255 && m.vertices[0].color[1] == 127 && m.vertices[0].color[0] == 0x7F);
    // A right-angle turn keeps the mitred edge at width from both segments.
    for (int i = 1; i <= 10; ++i) g.update(60.0f, 50.0f + 3.0f * static_cast<float>(i), 0.1f);
    CHECK(g.build(t, m) && mesh_area_xy(m) > 0.0f);
    const DecorationMesh turned = m;
    // Fade with time: standing still for 10 s at 20 units/s removes everything but the head.
    DynamicTrackParams q = p;
    q.fade_distance_per_second = 20.0f;
    DynamicTrackGenerator h(q);
    for (int i = 0; i <= 20; ++i) h.update(3.0f * static_cast<float>(i), 50.0f, 0.0f);
    for (int i = 0; i < 100; ++i) h.update(60.0f, 50.0f, 0.1f);
    CHECK(h.points().size() == 1 && !h.build(t, m));
    // Teleport restarts.
    DynamicTrackGenerator j(p);
    j.update(0, 0, 0);
    j.update(1e9f, 0, 0);
    CHECK(j.points().size() == 1);
    // Determinism: identical input -> identical bytes.
    DecorationMesh m2;
    DynamicTrackGenerator g2(p);
    for (int i = 0; i <= 20; ++i) g2.update(3.0f * static_cast<float>(i), 50.0f, 0.1f);
    for (int i = 1; i <= 10; ++i) g2.update(60.0f, 50.0f + 3.0f * static_cast<float>(i), 0.1f);
    CHECK(g2.build(t, m2) && m2.vertices.size() == turned.vertices.size() && m2.indices == turned.indices &&
          std::memcmp(m2.vertices.data(), turned.vertices.data(), turned.vertices.size() * sizeof(LitVertex)) == 0);
    // XML params.
    const char* xml =
        "<?xml version=\"1.0\"?><DynamicTracks><DynamicTrack name=\"Track_Bump\"><width>5.0f</width>"
        "<texture_name>w_tank_track_01.tga</texture_name><opacity>.1</opacity><segment_length>7</segment_length>"
        "<Render_Mode>DECAL_BUMP_ALPHA</Render_Mode></DynamicTrack><DynamicTrack name=\"X\"/></DynamicTracks>";
    std::vector<DynamicTrackParams> all;
    std::string err;
    const auto* xb = reinterpret_cast<const uint8_t*>(xml);
    if (parse_dynamic_track_fx(std::span<const uint8_t>(xb, std::strlen(xml)), all, &err)) {
        CHECK(all.size() == 2);
        const DynamicTrackParams* tb = find_dynamic_track(all, "track_bump");
        CHECK(tb && tb->width == 5.0f && approx(tb->opacity, 0.1f) && tb->segment_length == 7.0f && tb->mode == RenderMode::DecalBumpAlpha &&
              tb->fade_begin_distance == 40.0f);
        CHECK(all[1].mode == RenderMode::Alpha && all[1].width == 20.0f);
    } else {
        std::printf("decoration XML: %s (skipped)\n", err.c_str());
    }
}

void test_fow() {
    // Synthetic 20 x 10 grid over (0,0)-(300,150) with 15-unit cells.
    const FowGridDesc d = make_fow_grid(0.0f, 0.0f, 300.0f, 150.0f, kLandFowCellSize);
    CHECK(d.width == 20 && d.height == 10 && d.cell_x == 15.0f && d.cell_y == 15.0f);
    FowGrid g(d);
    std::vector<uint8_t> vis(200, 255);
    for (uint32_t x = 0; x < 20; ++x) vis[5 * 20 + x] = 0;  // a fogged row
    CHECK(g.set_intensity(vis) && !g.set_intensity(std::vector<uint8_t>(3)));
    std::vector<uint8_t> proc;
    const auto pal = fow_palette(kLandFowVisible, kLandFowColor);
    g.build_texels(pal, true, &proc);
    CHECK(g.texels().size() == 800);
    // Border forced fogged.
    bool border = true;
    for (uint32_t x = 0; x < 20; ++x) border = border && proc[x] == 0 && proc[9 * 20 + x] == 0;
    for (uint32_t y = 0; y < 10; ++y) border = border && proc[y * 20] == 0 && proc[y * 20 + 19] == 0;
    CHECK(border);
    // Blur: row 5 fogged between clear rows: (4 * 255 * 1 + 4 * 255 * 1 ... ) -> (255*(4+4) + 8) >> 4 = 128.
    CHECK(proc[5 * 20 + 10] == 128);
    // Row 2 interior (clear, neighbours clear) stays 255; row 1 touches the border row.
    CHECK(proc[2 * 20 + 10] == 255 && proc[1 * 20 + 10] == (255 * 12 + 8) / 16);
    // Fogged texel = 129/255 ~ 0.51 grey, alpha 0; clear = 254.
    const uint8_t* fog = &g.texels()[0];
    CHECK(fog[0] == 129 && fog[1] == 129 && fog[2] == 129 && fog[3] == 0);
    CHECK(approx(static_cast<float>(fog[0]) / 255.0f, 0.51f, 0.01f));
    const uint8_t* clear = &g.texels()[4 * (2 * 20 + 10)];
    CHECK(clear[0] == 254 && clear[3] == 254);
    CHECK(pal[255] == 0xFEFEFEFEu && pal[0] == 0x00818181u);
    {  // Space plane edge fade: alpha ramps to 0 on the border, colour and interior untouched.
        FowGrid f(d);
        CHECK(f.set_intensity(std::vector<uint8_t>(200, 0)));
        f.build_texels(fow_palette(kSpaceFowVisible, kSpaceFowColor), false);
        const size_t mid = 4 * (5 * 20 + 10);  // 5 cells from the top / bottom edges
        const uint8_t a = f.texels()[mid + 3];
        f.fade_texel_edges(4);
        CHECK(f.texels()[3] == 0 && f.texels()[0] == 254 && f.texels()[mid + 3] == a);
        CHECK(f.texels()[4 * (1 * 20 + 10) + 3] == a / 4 && f.texels()[4 * (3 * 20 + 10) + 3] == a * 3 / 4);
    }
    // Plane vectors: box corners -> 0/1, V flipped.
    const auto U = g.tex_u(), V = g.tex_v();
    auto dot = [](const std::array<float, 4>& a, float x, float y) { return a[0] * x + a[1] * y + a[2] * 7.0f + a[3]; };
    CHECK(approx(dot(U, 0, 0), 0.0f) && approx(dot(U, 300, 0), 1.0f) && approx(dot(V, 0, 150), 0.0f) && approx(dot(V, 0, 0), 1.0f));
    const auto U2 = fow_tex_u(300, 1160, 5920, 5060), V2 = fow_tex_v(300, 1160, 5920, 5060);
    CHECK(approx(dot(U2, 300, 0), 0.0f) && approx(dot(U2, 5920, 0), 1.0f) && approx(dot(V2, 0, 5060), 0.0f) && approx(dot(V2, 0, 1160), 1.0f));
    // Cell lookup: row 0 is the max-y edge.
    uint32_t cx = 0, cy = 0;
    CHECK(d.cell_of(1.0f, 149.0f, cx, cy) && cx == 0 && cy == 0 && d.cell_of(299.0f, 1.0f, cx, cy) && cx == 19 && cy == 9);
    CHECK(!d.cell_of(-1.0f, 5.0f, cx, cy));
    // Endor-sized box -> 375 x 260; clamp at 512; degenerate.
    const FowGridDesc e = make_fow_grid(300, 1160, 5920, 5060, kLandFowCellSize);
    CHECK(e.width == 375 && e.height == 260);
    CHECK(make_fow_grid(0, 0, 100000, 10, 15).width == 512 && make_fow_grid(0, 0, 0, 10, 15).width == 0);
    // Intensity table.
    const auto& it = fow_intensity_table();
    CHECK(it[0] == 0 && it[1] == 16 && it[2] == 32 && it[15] == 239 && it[16] == 255 && it[238] == 255 && it[239] == 28 && it[255] == 255);
    CHECK(fow_state_to_intensity(20, 0, 2, 10) == 0 && fow_state_to_intensity(20, 0, 2, 3) == 223 && fow_state_to_intensity(100, 0, 0, 3) == 255);
    CHECK(pack_fow_color(130, 130, 130, 0) == 0x00828282u);
    // Space map grid uses the 100-unit cell and the 04/05 box.
    MapData s;
    s.playable.present = true;
    s.playable.raw = {mk_f32x3(0x04, {-6000.0f, -5000.0f, -100.0f}), mk_f32x3(0x05, {6000.0f, 5000.0f, 100.0f})};
    s.playable.has_outer_box = true;
    s.playable.outer_min[0] = -6000;
    s.playable.outer_min[1] = -5000;
    s.playable.outer_min[2] = -100;
    s.playable.outer_max[0] = 6000;
    s.playable.outer_max[1] = 5000;
    s.playable.outer_max[2] = 100;
    MapData sr;
    CHECK(parse_ted(write_ted(s), sr) && sr.is_space() && sr.space.present);
    CHECK(sr.space.playable_min[0] == -6000.0f && sr.space.camera_min[0] == -5600.0f && sr.space.camera_max[1] == 4600.0f);
    const FowGridDesc sd = make_fow_grid(sr);
    CHECK(sd.width == 120 && sd.height == 100);
}

}  // namespace

// ---------------------------------------------------------------------------- real data
#if defined(ALAMO_TERRAIN_HAVE_DATA)
namespace {

struct DocRow {
    const char* name;
    uint32_t grid, patches;
    int32_t hmin, hmax;
    uint32_t mats;
    float avg;
    uint32_t max;
    WaterType water;
    float water_h;
    uint32_t envs, objects;
};
constexpr WaterType N = WaterType::None, Wa = WaterType::Water, La = WaterType::Lava, Ic = WaterType::Ice;
const DocRow kDoc[] = {
    {"_LAND_PLANET_DATHOMIR_01", 249, 8, -1026, 6966, 23, 9.5f, 17, N, 0, 1, 2134},
    {"_LAND_PLANET_GEONOSIS_02", 187, 6, -3071, 6540, 26, 13.0f, 21, Wa, -80, 4, 206},
    {"_MP_LAND_ALDERAAN", 187, 6, -1217, 7409, 31, 11.9f, 18, Wa, -2, 2, 550},
    {"_MP_LAND_BESPIN", 280, 9, 0, 5389, 4, 1.0f, 3, N, 0, 1, 796},
    {"_MP_LAND_BOTHAWUI", 187, 6, -1023, 5580, 44, 15.0f, 24, Wa, 0, 2, 1555},
    {"_MP_LAND_CORELLIA", 311, 10, -1297, 12200, 27, 8.2f, 14, Wa, -7.5f, 4, 2333},
    {"_MP_LAND_DAGOBAH", 249, 8, -1535, 2003, 16, 5.9f, 11, Wa, 0, 2, 1388},
    {"_MP_LAND_DANTOOINE", 311, 10, -2998, 3372, 28, 9.8f, 14, Wa, -115, 5, 1103},
    {"_MP_LAND_DATHOMIR", 249, 8, -1023, 6966, 16, 6.3f, 11, N, 0, 1, 746},
    {"_MP_LAND_ENDOR", 311, 10, -437, 4096, 25, 8.4f, 17, Wa, 0, 4, 1514},
    {"_MP_LAND_FELUCIA", 249, 8, -204, 9085, 13, 6.8f, 13, Wa, 0, 1, 1885},
    {"_MP_LAND_GEONOSIS", 311, 10, -5078, 4096, 30, 13.0f, 22, N, 0, 3, 842},
    {"_MP_LAND_HONOGHR", 249, 8, -426, 8288, 24, 10.2f, 14, Wa, -4, 1, 1295},
    {"_MP_LAND_HOTH", 218, 7, -545, 5898, 28, 10.7f, 19, N, 0, 3, 359},
    {"_MP_LAND_HYPORI", 373, 12, -5119, 5421, 27, 8.1f, 14, N, 0, 1, 1060},
    {"_MP_LAND_KAMINO", 249, 8, 0, 5340, 22, 2.1f, 6, Wa, 7, 1, 821},
    {"_MP_LAND_KESSEL", 280, 9, -4844, 3582, 21, 8.8f, 13, N, 0, 2, 408},
    {"_MP_LAND_MANDALORE", 311, 10, -1317, 7570, 26, 6.9f, 15, Wa, 0, 3, 3982},
    {"_MP_LAND_NABOO", 249, 8, -4065, 5428, 19, 9.0f, 14, N, 0, 2, 1697},
    {"_MP_LAND_POLUS", 187, 6, -1334, 5845, 30, 9.3f, 20, Ic, -0.01f, 3, 183},
    {"_MP_LAND_RYLOTH", 218, 7, -3829, 9363, 25, 10.6f, 17, N, 0, 1, 1051},
    {"_MP_LAND_SALEUCAMI", 218, 7, 4064, 8691, 22, 6.6f, 16, Wa, 208, 2, 1386},
    {"_MP_LAND_SHOLA", 187, 6, -2250, 5119, 23, 12.7f, 17, La, -16, 2, 251},
    {"_MP_LAND_TATOOINE", 187, 6, -2636, 3637, 28, 12.5f, 18, N, 0, 2, 1290},
    {"_MP_LAND_UTAPAU", 311, 10, 0, 12384, 6, 2.1f, 4, Wa, 0, 1, 38},
    {"_MP_LAND_YAVIN", 218, 7, -1003, 3071, 17, 7.0f, 11, N, 0, 2, 1826},
};

const char* water_name(WaterType t) {
    switch (t) {
    case WaterType::Water: return "water";
    case WaterType::Lava: return "lava";
    case WaterType::Ice: return "ice";
    default: return "none";
    }
}

// Layers per patch = distinct rendered material indices over each patch's 32x32 vertices.
void layer_stats(const Terrain& t, float& avg, uint32_t& mx, uint32_t& used) {
    std::set<uint32_t> all;
    for (uint8_t m : t.material) all.insert(m);
    used = static_cast<uint32_t>(all.size());
    uint64_t sum = 0;
    mx = 0;
    const uint32_t np = t.patches_x() * t.patches_y();
    for (uint32_t py = 0; py < t.patches_y(); ++py)
        for (uint32_t px = 0; px < t.patches_x(); ++px) {
            bool seen[kMaterialSlots] = {};
            uint32_t n = 0;
            for (uint32_t r = 0; r < kPatchVerts; ++r)
                for (uint32_t c = 0; c < kPatchVerts; ++c) {
                    uint8_t m = t.material_at(px * kPatchQuads + c, py * kPatchQuads + r);
                    if (m >= kMaterialSlots) m = kMaterialSlots - 1;
                    if (!seen[m] && !t.materials[m].not_rendered) {
                        seen[m] = true;
                        ++n;
                    }
                }
            sum += n;
            mx = std::max(mx, n);
        }
    avg = np ? static_cast<float>(sum) / static_cast<float>(np) : 0.0f;
}

std::string model_of(const alamo::data::GameDatabase& db, const alamo::data::GameObjectRecord* rec, bool space,
                     float& scale) {
    for (int depth = 0; rec && depth < 8; ++depth) {
        const auto& el = rec->element;
        const char* order_land[] = {"Land_Model_Name", "Space_Model_Name", "Galactic_Model_Name"};
        const char* order_space[] = {"Space_Model_Name", "Land_Model_Name", "Galactic_Model_Name"};
        if (scale == 1.0f) {
            const std::string_view sf = el.child_text("Scale_Factor");
            if (!sf.empty()) scale = std::strtof(std::string(sf).c_str(), nullptr);
            if (!(scale > 0.0f) || !std::isfinite(scale)) scale = 1.0f;
        }
        const char* const* order = space ? order_space : order_land;
        for (int i = 0; i < 3; ++i) {
            const std::string_view v = el.child_text(order[i]);
            if (!v.empty()) return std::string(v);
        }
        const std::string_view base = el.child_text("Variant_Of_Existing_Type");
        if (base.empty()) break;
        rec = db.find_game_object(base);
    }
    return {};
}

// DECORATIONS.md 6: decals / tracks over every map the FoC VFS sees, UM11 decal geometry, the
// decoration XML, space maps and FOW grids.
template <class V>
void test_real_decorations(V& vfs, const std::vector<std::string>& maps, const alamo::meg::Archive* maps_meg) {
    std::string err;
    uint32_t decal_maps = 0, decals = 0, track_maps = 0, tracks = 0, foc_maps = 0, foc_tracks = 0, geo_ok = 0;
    uint64_t geo_tris = 0;
    uint32_t hmode[3] = {0, 0, 0}, flags4 = 0, flags5 = 0, bad_point_count = 0, max_tris = 0;
    int64_t doc_adjust = 0, doc_adjust_mode1 = 0;  // patch copy vs maps.meg copy of the same map
    size_t max_track_verts = 0, max_track_idx = 0, max_map_verts = 0, max_map_idx = 0, max_profile = 0, max_points = 0;
    uint32_t max_rows = 0;
    std::map<std::string, uint32_t> textures;
    for (const std::string& path : maps) {
        MapData m;
        std::vector<uint8_t> bytes;
        if (!vfs.read(path, bytes) || !parse_ted(bytes, m, &err)) continue;
        const Terrain& t = m.terrain;
        if (!t.decals.empty()) {
            ++decal_maps;
            decals += static_cast<uint32_t>(t.decals.size());
        }
        if (t.tracks.empty()) continue;
        ++track_maps;
        tracks += static_cast<uint32_t>(t.tracks.size());
        const std::string origin = vfs.origin(path);
        const bool foc = origin.find("corruption") != std::string::npos;
        foc_maps += foc ? 1u : 0u;
        foc_tracks += foc ? static_cast<uint32_t>(t.tracks.size()) : 0u;
        if (origin.find("Patch") != std::string::npos && maps_meg) {
            const alamo::meg::Entry* e = maps_meg->find(path);
            std::vector<uint8_t> mb;
            MapData mm;
            if (e && maps_meg->read(*e, mb) && parse_ted(mb, mm)) {
                doc_adjust += static_cast<int64_t>(mm.terrain.tracks.size()) - static_cast<int64_t>(t.tracks.size());
                for (const TerrainTrack& k : mm.terrain.tracks) doc_adjust_mode1 += k.height_mode == 1 ? 1 : 0;
                for (const TerrainTrack& k : t.tracks) doc_adjust_mode1 -= k.height_mode == 1 ? 1 : 0;
            }
            std::printf("  %s: %zu tracks (%s), %zu in maps.meg\n", path.c_str(), t.tracks.size(), origin.c_str(),
                        mm.terrain.tracks.size());
        }
        size_t map_verts = 0, map_idx = 0;
        for (const TerrainTrack& k : t.tracks) {
            max_profile = std::max(max_profile, k.profile.size());
            max_points = std::max(max_points, k.points.size());
            if (k.height_mode < 3) ++hmode[k.height_mode];
            flags4 += k.flags == 4 ? 1u : 0u;
            flags5 += k.flags == 5 ? 1u : 0u;
            bad_point_count += k.stored_point_count != k.points.size() || k.widths.size() != k.points.size() ? 1u : 0u;
            ++textures[k.texture];
            TrackGeometry g;
            if (build_track_geometry(t, k, g)) {
                ++geo_ok;
                geo_tris += g.mesh.triangle_count();
                max_tris = std::max(max_tris, g.mesh.triangle_count());
                max_rows = std::max(max_rows, g.rows);
                max_track_verts = std::max(max_track_verts, g.mesh.vertices.size());
                max_track_idx = std::max(max_track_idx, g.mesh.indices.size());
                map_verts += g.mesh.vertices.size();
                map_idx += g.mesh.indices.size();
            }
        }
        max_map_verts = std::max(max_map_verts, map_verts);
        max_map_idx = std::max(max_map_idx, map_idx);
        // The map-level builder (per-map budget) builds every vanilla ribbon unclipped.
        std::vector<TrackGeometry> all;
        CHECK(build_map_track_geometry(t, all) == t.tracks.size());
    }
    std::printf("track budget: largest track %zu vertices / %zu indices (%u rows, profile %zu, %zu points); largest map %zu "
                "vertices / %zu indices (caps %u/%u per track, %u/%u per map, profile %u, %u points)\n",
                max_track_verts, max_track_idx, max_rows, max_profile, max_points, max_map_verts, max_map_idx, kMaxTrackVertices,
                kMaxTrackIndices, kMaxMapTrackVertices, kMaxMapTrackIndices, kMaxTrackProfile, kMaxTrackPoints);
    CHECK(max_track_verts <= kMaxTrackVertices && max_map_verts <= kMaxMapTrackVertices && max_points <= kMaxTrackPoints);
    std::printf("decorations: %u maps with decals (%u decals); %u maps with tracks (%u tracks; %u FoC maps with %u; height mode "
                "0/1/2 = %u/%u/%u, flags 4/5 = %u/%u, %u point/width-count mismatches); %u ribbons built (%llu triangles, "
                "largest %u), %zu textures; maps.meg-copy adjustment %lld\n",
                decal_maps, decals, track_maps, tracks, foc_maps, foc_tracks, hmode[0], hmode[1], hmode[2], flags4, flags5,
                bad_point_count, geo_ok, static_cast<unsigned long long>(geo_tris), max_tris, textures.size(),
                static_cast<long long>(doc_adjust));
    CHECK(decals == 65 && decal_maps == 1);
    // VFS view (what the game loads; it also mounts the EaW GameData maps): 24 maps, 509 tracks.
    // DECORATIONS.md 6 (491 = 363/107/21) counts _MP_LAND_UTAPAU from maps.meg (no tracks)
    // instead of its Patch2 copy (18 height-mode-1 tracks): both are reproduced.
    CHECK(tracks == 509 && track_maps == 24 && foc_maps == 23 && geo_ok == tracks);
    CHECK(hmode[0] == 107 && hmode[1] == 381 && hmode[2] == 21);
    if (maps_meg) {
        CHECK(static_cast<int64_t>(tracks) + doc_adjust == 491 && static_cast<int64_t>(hmode[1]) + doc_adjust_mode1 == 363);
    }

    // UM11 decals -> geometry.
    for (const std::string& path : maps) {
        if (path.find("UM11") == std::string::npos) continue;
        MapData m;
        if (!load_map(vfs, path, m, &err)) continue;
        uint32_t built = 0, tris = 0, mod = 0, m6 = 0;
        size_t dv = 0, di = 0, dmax_v = 0, dmax_i = 0;
        double dmax_cells = 0.0, dmax_side = 0.0;
        for (const TerrainDecal& d : m.terrain.decals) {
            mod += d.render_mode == 3 && d.width == 15.0f && d.height == 15.0f && d.slot_column == 0 && d.slot_row == 1 ? 1u : 0u;
            m6 += d.render_mode == 6 ? 1u : 0u;
            if (d.render_mode == 6)
                std::printf("  mode-6 decal %gx%g rot %g\n", static_cast<double>(d.width), static_cast<double>(d.height),
                            static_cast<double>(d.rotation));
            const DecorationFrame f = DecorationFrame::make(d.position[0], d.position[1], d.width, d.height, d.rotation);
            const double cx = std::ceil(f.max_xy[0] / kVertexSpacing) - std::floor(f.min_xy[0] / kVertexSpacing);
            const double cy = std::ceil(f.max_xy[1] / kVertexSpacing) - std::floor(f.min_xy[1] / kVertexSpacing);
            dmax_cells = std::max(dmax_cells, cx * cy);
            dmax_side = std::max(dmax_side, static_cast<double>(std::max(d.width, d.height)));
            DecalGeometry g;
            if (build_decal_geometry(m.terrain, d, g)) {
                ++built;
                tris += g.mesh.triangle_count();
                dv += g.mesh.vertices.size();
                di += g.mesh.indices.size();
                dmax_v = std::max(dmax_v, g.mesh.vertices.size());
                dmax_i = std::max(dmax_i, g.mesh.indices.size());
            }
        }
        std::printf("%s: %zu decals (%u MODULATE 15x15 slot (0,1), %u mode 6), %u built, %u triangles; %zu tracks\n", path.c_str(),
                    m.terrain.decals.size(), mod, m6, built, tris, m.terrain.tracks.size());
        std::printf("decal budget: largest decal %.0f cells (side %g), %zu vertices / %zu indices; map %zu vertices / %zu indices "
                    "(caps %u cells, %u/%u per decal, %u/%u per map)\n",
                    dmax_cells, dmax_side, dmax_v, dmax_i, dv, di, kMaxDecalCells, kMaxDecalVertices, kMaxDecalIndices,
                    kMaxMapDecalVertices, kMaxMapDecalIndices);
        CHECK(mod == 64 && m6 == 1 && built == 65);
        CHECK(dmax_cells <= kMaxDecalCells && dv <= kMaxMapDecalVertices && di <= kMaxMapDecalIndices);
        std::vector<DecalGeometry> all;
        CHECK(build_map_decal_geometry(m.terrain, all) == 65 && all.size() == 65);
        // FOW grid of a land map.
        const FowGridDesc fd = make_fow_grid(m);
        std::printf("  FOW grid %ux%u (cell %.2f x %.2f)\n", fd.width, fd.height, static_cast<double>(fd.cell_x),
                    static_cast<double>(fd.cell_y));
        CHECK(fd.width > 0 && fd.width <= kMaxFowCells && fd.height > 0);
    }
    {
        MapData endor;
        CHECK(load_map(vfs, "_MP_LAND_ENDOR", endor));
        const FowGridDesc fd = make_fow_grid(endor);
        CHECK(fd.width == 375 && fd.height == 260);
    }

    // XML.
    {
        std::vector<uint8_t> bytes;
        std::vector<DynamicTrackParams> fx;
        if (vfs.read("Data\\XML\\DynamicTrackFX.xml", bytes) && parse_dynamic_track_fx(bytes, fx, &err)) {
            const DynamicTrackParams* tb = find_dynamic_track(fx, "Track_Bump");
            std::printf("DynamicTrackFX.xml: %zu tracks\n", fx.size());
            CHECK(fx.size() == 11 && tb && tb->width == 5.0f && tb->mode == RenderMode::DecalBumpAlpha && tb->fade_end_distance == 280.0f);
        } else {
            std::printf("DynamicTrackFX.xml: not read (%s)\n", err.c_str());
        }
        std::vector<ShadowBlobMaterial> mats = builtin_blob_materials();
        if (vfs.read("Data\\XML\\ShadowBlobMaterials.xml", bytes) && parse_shadow_blob_materials(bytes, mats, &err)) {
            const uint32_t sel = find_blob_material(mats, "Selection_Rebel_Land");
            std::printf("ShadowBlobMaterials.xml: %zu materials (incl. 2 built-in)\n", mats.size());
            CHECK(mats.size() > 20 && sel > 1 && mats[sel].shader == "BlobStencilMasked.fx" && mats[sel].texture2 == "i_selection_rebel.tga");
            CHECK(mats[find_blob_material(mats, "Terrain_Editor_Brush")].mode == RenderMode::Alpha);
        }
    }

    // Space maps: description + objects.
    std::printf("\n| space map | objects | playable box | camera box | FOW grid |\n|---|---|---|---|---|\n");
    uint32_t space_n = 0;
    for (const std::string& path : maps) {
        if (path.find("SPACE") == std::string::npos) continue;
        MapData m;
        if (!load_map(vfs, path, m, &err)) continue;
        if (!m.is_space()) continue;
        ++space_n;
        const SpaceMapDesc& s = m.space;
        CHECK(s.present && s.playable_max[0] > s.playable_min[0] && s.camera_max[0] > s.camera_min[0]);
        const FowGridDesc fd = make_fow_grid(m);
        if (space_n <= 6)
            std::printf("| %s | %zu | %.0f..%.0f x %.0f..%.0f | %.0f..%.0f | %ux%u |\n", path.substr(14).c_str(), m.objects.size(),
                        static_cast<double>(s.playable_min[0]), static_cast<double>(s.playable_max[0]),
                        static_cast<double>(s.playable_min[1]), static_cast<double>(s.playable_max[1]),
                        static_cast<double>(s.camera_min[0]), static_cast<double>(s.camera_max[0]), fd.width, fd.height);
        CHECK(fd.width > 0 && fd.height > 0);
    }
    std::printf("%u space maps decoded\n", space_n);
    CHECK(space_n >= 24);
}

void test_real_data() {
    const char* root = std::getenv("ALAMO_FOC_DATA");
    if (!root || !*root) {
        std::printf("real data: ALAMO_FOC_DATA not set, skipped\n");
        return;
    }
    alamo::vfs::VfsConfig cfg;
    cfg.foc_root = root;
    std::string err;
    auto vfs = alamo::vfs::Vfs::create(cfg, &err);
    CHECK(vfs != nullptr);
    if (!vfs) {
        std::printf("real data: vfs failed: %s\n", err.c_str());
        return;
    }
    alamo::jobs::JobSystem js;
    const alamo::ParallelFor pf = alamo::jobs::make_parallel_for(js);

    // 1. Every map in the VFS parses and (land) builds.
    const auto maps = vfs->list("Data\\Art\\Maps\\*.ted");
    uint32_t ok = 0, land = 0, space = 0, legacy = 0, hints = 0, hint_maps = 0;
    std::map<uint32_t, uint32_t> unknown_ids;
    const auto t_all = std::chrono::steady_clock::now();
    for (const std::string& path : maps) {
        std::vector<uint8_t> bytes;
        if (!vfs->read(path, bytes)) continue;
        MapData m;
        if (!parse_ted(bytes, m, &err)) {
            std::printf("FAIL parse %s: %s\n", path.c_str(), err.c_str());
            ++g_fail;
            continue;
        }
        ++ok;
        {  // q6 t28: 0x482 hint names (TERRAIN_WATER_POST 1.6)
            uint32_t n = 0;
            for (const ObjectPlacement& o : m.objects) n += o.hint.empty() ? 0u : 1u;
            hints += n;
            hint_maps += n ? 1u : 0u;
        }
        {
            ParseOptions eo;
            eo.environments_only = true;
            MapData e;
            CHECK(parse_ted(bytes, e, &err, eo));
            CHECK(e.environments.environments.size() == m.environments.environments.size() && e.is_space() == m.is_space() &&
                  e.environments.start_index == m.environments.start_index);
        }
        if (m.environments.legacy_format) ++legacy;
        for (uint32_t id : m.unknown_world_chunks) ++unknown_ids[id];
        if (m.is_space()) {
            ++space;
            continue;
        }
        ++land;
        if ((m.terrain.header.width - 1) % 31 != 0 || m.terrain.header.width != m.terrain.header.height ||
            m.terrain.header.vertex_count != m.terrain.width * m.terrain.height) {
            std::printf("  note: %s grid %ux%u (vertex count %u)\n", path.c_str(), m.terrain.header.width,
                        m.terrain.header.height, m.terrain.header.vertex_count);
        }
        // Re-encode stability: parse(write(parse(x))) == write(parse(x)).
        const auto w1 = write_ted(m);
        MapData m2;
        CHECK(parse_ted(w1, m2));
        CHECK(write_ted(m2) == w1);
    }
    const double all_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_all).count();
    std::printf("all maps: %zu listed, %u parsed (%u land, %u space, %u legacy env), parse+rewrite %.2f s\n", maps.size(), ok,
                land, space, legacy, all_s);
    for (const auto& [id, n] : unknown_ids) std::printf("  unhandled world chunk 0x%X in %u maps\n", id, n);
    CHECK(ok == maps.size() && ok > 0);
    std::printf("map hints (0x482): %u in %u maps\n", hints, hint_maps);
    CHECK(hints > 2000 && hint_maps > 50);  // TERRAIN_WATER_POST 1.6 counts maps.meg alone (2741 in 155 maps)
    {  // UM01 (Underworld act I mission 1): 154 hints in maps.meg (the patched copy the VFS serves: 152); Find_Hint("UMP_SMALL_GEAR", "falcongear1") needs this one.
        MapData um;
        CHECK(load_map(*vfs, "UM01_A_Crimelord_Unleashed", um));
        uint32_t n = 0, gear = 0;
        for (const ObjectPlacement& o : um.objects) {
            n += o.hint.empty() ? 0u : 1u;
            gear += o.hint == "falcongear1" && o.type_crc == alamo::name_crc("UMP_SMALL_GEAR") ? 1u : 0u;
        }
        std::printf("UM01_A_Crimelord_Unleashed: %u hints, falcongear1 on UMP_SMALL_GEAR x%u\n", n, gear);
        CHECK(n > 150 && gear == 1);
    }

    // 2. The doc table (TERRAIN_WATER_POST 1.9).
    std::printf("\n| map | grid | patches | raw h min..max | mats | layers avg/max | water | env | objects | vs doc |\n");
    std::printf("|---|---|---|---|---|---|---|---|---|---|\n");
    // The doc extracted maps from corruption\Data\maps.meg; the VFS also mounts the patch MEGs,
    // which replace some maps. A row that differs through the VFS is re-checked on maps.meg.
    std::string meg_err;
    auto maps_meg = alamo::meg::Archive::open(std::string(root) + "\\Data\\maps.meg", alamo::meg::Source::Mode::Auto, &meg_err);
    test_real_decorations(*vfs, maps, maps_meg ? &*maps_meg : nullptr);
    auto row = [&](const DocRow& d, const MapData& m, const char* src) {
        const Terrain& t = m.terrain;
        int32_t hmin = 0, hmax = 0;
        if (!t.heights.empty()) {
            const auto mm = std::minmax_element(t.heights.begin(), t.heights.end());
            hmin = *mm.first;
            hmax = *mm.second;
        }
        float avg = 0;
        uint32_t mx = 0, used = 0;
        layer_stats(t, avg, mx, used);
        const bool match = t.width == d.grid && t.patches_x() == d.patches && t.patches_y() == d.patches && hmin == d.hmin &&
                           hmax == d.hmax && used == d.mats && std::fabs(avg - d.avg) < 0.051f && mx == d.max &&
                           t.header.water_type == d.water && (d.water == N || approx(t.header.water_height, d.water_h, 1e-3f)) &&
                           m.environments.environments.size() == d.envs && m.objects.size() == d.objects;
        std::printf("| %s%s | %u | %ux%u | %d..%d | %u | %.1f / %u | %s %g | %zu | %zu | %s |\n", d.name, src, t.width,
                    t.patches_x(), t.patches_y(), hmin, hmax, used, static_cast<double>(avg), mx,
                    water_name(t.header.water_type), static_cast<double>(t.header.water_height),
                    m.environments.environments.size(), m.objects.size(), match ? "match" : "DIFF");
        return match;
    };
    for (const DocRow& d : kDoc) {
        MapData m;
        if (!load_map(*vfs, d.name, m, &err)) {
            std::printf("FAIL load %s: %s\n", d.name, err.c_str());
            ++g_fail;
            continue;
        }
        bool match = row(d, m, "");
        if (!match && maps_meg) {
            std::printf("|   (VFS copy from %s) | | | | | | | | | |\n", vfs->origin(map_path(d.name)).c_str());
            const alamo::meg::Entry* e = maps_meg->find(map_path(d.name));
            std::vector<uint8_t> bytes;
            MapData mm;
            if (e && maps_meg->read(*e, bytes) && parse_ted(bytes, mm, &err)) match = row(d, mm, " (maps.meg)");
        }
        CHECK(match);
    }

    // 3. Space maps.
    std::printf("\n| space map | envs | objects | env0 sun int | bloom str/cut/size | skydomes |\n|---|---|---|---|---|---|\n");
    uint32_t space_ok = 0;
    for (const std::string& path : maps) {
        if (path.find("_MP_SPACE_") == std::string::npos) continue;
        MapData m;
        if (!load_map(*vfs, path, m, &err)) continue;
        ++space_ok;
        const Environment* e = m.environments.start();
        CHECK(m.is_space() && e != nullptr && m.playable.has_box);
        if (!e) continue;
        std::printf("| %s | %zu | %zu | %.3f | %.2f / %.2f / %.2f | %s %s |\n", path.substr(14).c_str(),
                    m.environments.environments.size(), m.objects.size(), static_cast<double>(e->intensity[0]),
                    static_cast<double>(e->bloom_strength), static_cast<double>(e->bloom_cutoff),
                    static_cast<double>(e->bloom_size), e->skydome[0].c_str(), e->skydome[1].c_str());
    }
    CHECK(space_ok == 24);
    {
        MapData k;
        CHECK(load_map(*vfs, "_MP_SPACE_KUAT", k));
        const Environment& e = k.environments.environments.at(0);
        CHECK(approx(e.light_color[0][0], 0.918f, 1e-3f) && approx(e.intensity[0], 0.646f, 1e-3f));
        CHECK(e.skydome[0] == "STARS_LOW" && e.skydome[1] == "STAR_BACKDROP_GREEN_ORANGE_2");
        CHECK(approx(e.skydome_angle_deg[1][1], 210.0f, 0.01f));
    }

    // 4. Endor environment + header fields (5.4, 1.3).
    MapData endor;
    CHECK(load_map(*vfs, "_MP_LAND_ENDOR", endor));
    {
        const Environment& e = endor.environments.environments.at(0);
        CHECK(e.name == "Sunrise_Clear" && approx(e.light_color[0][1], 0.729f, 1e-3f) && approx(e.intensity[0], 0.646f, 1e-3f));
        CHECK(approx(e.yaw[0] * 57.29578f, 220.0f, 0.05f) && approx(e.elevation[0] * 57.29578f, 33.0f, 0.05f));
        CHECK(e.fog_enabled && e.fog_start == 0.0f && e.fog_end == 3500.0f && e.bloom_size == 1.0f);
        CHECK(e.skydome[0] == "DAY_STORM_SKY" && e.cloud_scale == 1000.0f);
        const TerrainHeader& h = endor.terrain.header;
        CHECK(h.water_type == WaterType::Water && h.water_height == 0.0f && h.water_texture[0] == "W_WaterBump00.tga");
        CHECK(endor.terrain.water_scroll[0].tile_size == 112.0f && endor.terrain.water_scroll[1].tile_size == 150.0f);
        CHECK(endor.playable.has_box && endor.playable.box_min[0] == 300.0f && endor.playable.box_max[1] == 5060.0f);
        CHECK(endor.start_positions.size() == 2 && endor.info.planet_name == "Endor" && endor.info.world_width == 6220.0f);
        CHECK(endor.tactical_camera.present && approx(endor.tactical_camera.f32_or(0x03, 0), 1250.0f, 0.01f));
        std::printf("\nEndor: %zu zones, %zu preview objects, info name key %s\n", endor.zones.size(), endor.preview_objects.size(),
                    endor.info.name_key.c_str());
        // Passability order check: mean slope of Clear vs Steep cells (1.5).
        double slope[2] = {0, 0};
        uint64_t cnt[2] = {0, 0};
        const Terrain& t = endor.terrain;
        for (uint32_t y = 1; y + 1 < t.height; ++y)
            for (uint32_t x = 1; x + 1 < t.width; ++x) {
                const uint32_t p = endor.passability_at(x, y);
                const int k = p == kPassClear ? 0 : (p == kPassSteep ? 1 : -1);
                if (k < 0) continue;
                const auto n = terrain_normal(t, x, y);
                slope[k] += std::sqrt(n[0] * n[0] + n[1] * n[1]) / n[2];
                ++cnt[k];
            }
        const double clear = cnt[0] ? slope[0] / static_cast<double>(cnt[0]) : 0, steep = cnt[1] ? slope[1] / static_cast<double>(cnt[1]) : 0;
        std::printf("Endor passability: Clear mean slope %.3f (%llu cells), Steep %.3f (%llu cells)\n", clear,
                    static_cast<unsigned long long>(cnt[0]), steep, static_cast<unsigned long long>(cnt[1]));
        CHECK(steep > clear * 1.5);
    }

    // 5. Mesh builds.
    std::printf("\n| map | patches | passes (max) | triangles | water patches / tris | memory MB | build ms (%u threads) | serial ms |\n|---|---|---|---|---|---|---|---|\n",
                js.thread_count());
    for (const char* name : {"_MP_LAND_ENDOR", "_MP_LAND_DAGOBAH", "_MP_LAND_HOTH"}) {
        MapData m;
        CHECK(load_map(*vfs, name, m));
        TerrainMesh mesh, serial;
        WaterSurface water;
        auto t0 = std::chrono::steady_clock::now();
        CHECK(build_terrain_mesh(m.terrain, mesh, {}, pf, &err));
        CHECK(build_water(m.terrain, water, {}, pf));
        const double par_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        t0 = std::chrono::steady_clock::now();
        CHECK(build_terrain_mesh(m.terrain, serial));
        const double ser_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        bool same = mesh.patches.size() == serial.patches.size();
        for (size_t i = 0; same && i < mesh.patches.size(); ++i) {
            same = mesh.patches[i].vertices == serial.patches[i].vertices &&
                   mesh.patches[i].passes.size() == serial.patches[i].passes.size();
            for (size_t k = 0; same && k < mesh.patches[i].passes.size(); ++k)
                same = mesh.patches[i].passes[k].indices == serial.patches[i].passes[k].indices &&
                       mesh.patches[i].passes[k].blend_texture == serial.patches[i].passes[k].blend_texture;
        }
        CHECK(same);
        const auto& s = mesh.stats;
        std::printf("| %s | %u | %u (%u) | %llu | %zu / %llu | %.2f (+%.2f water) | %.1f | %.1f |\n", name, s.patches, s.passes,
                    s.max_passes, static_cast<unsigned long long>(s.triangles), water.patches.size(),
                    static_cast<unsigned long long>(water.triangles), static_cast<double>(s.total_bytes()) / 1048576.0,
                    static_cast<double>(water.bytes) / 1048576.0, par_ms, ser_ms);
        // Blend-texture invariant on real data: every quad is fully covered by its lowest material.
        uint32_t bad = 0;
        for (const TerrainPatch& p : mesh.patches) {
            const uint32_t x0 = p.px * kPatchQuads, y0 = p.py * kPatchQuads;
            for (uint32_t r = 0; r < kPatchQuads; ++r)
                for (uint32_t c = 0; c < kPatchQuads; ++c) {
                    uint8_t lo = 255;
                    for (uint32_t k = 0; k < 4; ++k) lo = std::min(lo, m.terrain.material_at(x0 + c + (k & 1), y0 + r + (k >> 1)));
                    for (const TerrainPass& q : p.passes)
                        if (q.material == lo)
                            for (uint32_t k = 0; k < 4; ++k) bad += q.texel(c + (k & 1), r + (k >> 1)) == 0xFFFF ? 0u : 1u;
                }
        }
        CHECK(bad == 0);
    }

    // 6. Endor placements through the game database.
    alamo::data::GameDatabase db;
    std::vector<std::string> diag;
    const auto t_db = std::chrono::steady_clock::now();
    const bool db_ok = db.load(*vfs, {}, &diag);
    std::printf("\ngame database: %s, %zu game objects (%.1f s)\n", db_ok ? "loaded" : "FAILED", db.game_objects().size(),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t_db).count());
    CHECK(db_ok);
    std::unordered_map<uint32_t, const alamo::data::GameObjectRecord*> by_crc;
    for (const auto& r : db.game_objects())
        if (!r.duplicate) by_crc.emplace(r.name_crc, &r);
    const TypeResolver resolver = [&](uint32_t crc, bool sp, ResolvedType& out) {
        const auto it = by_crc.find(crc);
        if (it == by_crc.end()) return false;
        out.type_name = std::string(it->second->name);
        out.scale_factor = 1.0f;
        out.model_name = model_of(db, it->second, sp, out.scale_factor);
        return true;
    };
    for (const char* name : {"_MP_LAND_ENDOR", "_MP_LAND_DAGOBAH", "_MP_LAND_HOTH"}) {
        MapData m;
        CHECK(load_map(*vfs, name, m));
        const PlacementResult pr = resolve_placements(m, resolver);
        std::set<std::string> models;
        for (const auto& p : pr.placements) models.insert(p.model_name);
        std::printf("%s: %zu objects -> %zu with a model (%zu distinct .alo), %zu type without model, %zu unknown CRC (%zu distinct)\n",
                    name, m.objects.size(), pr.placements.size(), models.size(), pr.no_model.size(), pr.unknown_type.size(),
                    pr.unknown_crcs.size());
        if (std::string(name) == "_MP_LAND_ENDOR") {
            std::map<std::string, uint32_t> nm;
            for (uint32_t i : pr.no_model) {
                ResolvedType t;
                resolver(m.objects[i].type_crc, false, t);
                ++nm[t.type_name];
            }
            for (const auto& [n, c] : nm) std::printf("  no model: %s x%u\n", n.c_str(), c);
            for (const auto& [crc, c] : pr.unknown_crcs) std::printf("  unknown CRC 0x%08X x%u\n", crc, c);
            // Stored z vs terrain surface.
            double sum = 0, mxd = 0;
            uint32_t within = 0;
            for (const auto& o : m.objects) {
                const double d = std::fabs(static_cast<double>(o.position[2]) - m.terrain.sample_height(o.position[0], o.position[1]));
                sum += d;
                mxd = std::max(mxd, d);
                within += d < 0.5 ? 1u : 0u;
            }
            std::printf("  stored z vs terrain: mean |dz| %.2f, max %.1f, %u / %zu within 0.5 units\n",
                        sum / static_cast<double>(std::max<size_t>(1, m.objects.size())), mxd, within, m.objects.size());
            CHECK(pr.unknown_type.empty());
            CHECK(pr.placements.size() > m.objects.size() / 2);
            // Mini 03 is the starting health fraction: the 4 mineral processor pads store 0.08 and
            // must resolve at their type's normal Scale_Factor, carrying the fraction for the sim.
            uint32_t pads = 0, pads_scaled = 0, pads_ok = 0;
            ResolvedType pad_type;
            const bool pad_known = resolver(alamo::name_crc("Skirmish_Mineral_Processor_Pad"), false, pad_type);
            CHECK(pad_known);
            for (const auto& p : pr.placements) {
                if (p.type_crc != alamo::name_crc("Skirmish_Mineral_Processor_Pad")) continue;
                ++pads;
                pads_scaled += p.scale == pad_type.scale_factor ? 1u : 0u;
                pads_ok += p.scale == pad_type.scale_factor && approx(p.initial_health_fraction, 0.08f, 1e-3f) ? 1u : 0u;
            }
            std::printf("  Skirmish_Mineral_Processor_Pad: %u placed, %u at Scale_Factor %g, %u of them with health fraction ~0.08\n",
                        pads, pads_scaled, static_cast<double>(pad_type.scale_factor), pads_ok);
            CHECK(pads == 6 && pads_scaled == pads && pads_ok == 4);
            // Every model name should exist as a file.
            uint32_t missing_files = 0;
            for (const std::string& mn : models) {
                std::string p = "Data\\Art\\Models\\" + mn;
                if (!vfs->exists(p)) {
                    ++missing_files;
                    std::printf("  model file missing: %s\n", mn.c_str());
                }
            }
            std::printf("  %zu distinct models, %u missing as files\n", models.size(), missing_files);
        }
    }
}

}  // namespace
#endif

int main() {
    test_chunks();
    test_round_trip();
    test_mesh_blend_rules();
    test_mesh_geometry();
    test_texcoords();
    test_water();
    test_placements();
    test_hardening();
    test_fuzz();
    test_decoration_records();
    test_decal_geometry();
    test_decal_atlas_uv();
    test_blob_geometry();
    test_track_geometry();
    test_track_budget();
    test_decal_budget();
    test_track_malformed();
    test_dynamic_track();
    test_fow();
#if defined(ALAMO_TERRAIN_HAVE_DATA)
    test_real_data();
#else
    std::printf("real data: built without vfs/data/jobs, skipped\n");
#endif
    if (g_fail) {
        std::printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    std::printf("all terrain tests passed\n");
    return 0;
}
