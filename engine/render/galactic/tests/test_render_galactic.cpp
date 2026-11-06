// alamo_render_galactic_tests - R2 galactic sim -> render (no framework; non-zero exit on failure).
//   * primitives: procedural meshes valid, primitive model names round-trip, colour parsing (TradeRouteLines quirks)
//   * interpolation: original = latest, smooth blends fleets in flight only, new fleets keep their state
//   * read-only: the synthetic galaxy (GalacticOrders on) for 900 ticks with and without a snapshot + interpolation +
//     instance build after every tick -> identical per-tick CRC logs and GalacticSim state hash; fleets fly, planets
//     change hands
//   * thread determinism: snapshot digests per tick identical for 0/1/4/15 workers and serial building
//   * scene: planets / background / starbases / rings / routes / fleets / fleet lines, orbit slots, route classes,
//     fleets only in transit with one _ALT (stealth flag from the members), the planet-selection model,
//     ribbon and ring transforms
//   * GPU (skipped without a Vulkan 1.3 device): the synthetic galaxy drawn offscreen with 0 validation messages, the
//     CRC log of that run = the headless one, PNG logs/render_galactic_synthetic_test.png
//   * real data (ALAMO_FOC_DATA): FoC Sandbox_Heart_Of_The_Maw_Underworld, 600 ticks with / without snapshots ->
//     identical CRC logs; planet / route / fleet models resolved from the XML
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/log.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/jobs/crc_recorder.hpp"
#include "alamo/jobs/job_system.hpp"
#include "alamo/render/galactic/galactic_session.hpp"
#include "alamo/render/galactic/galactic_snapshot.hpp"
#include "alamo/render/galactic/galactic_view.hpp"
#include "alamo/render/galactic/planet_portrait.hpp"
#include "alamo/render/pipelines.hpp"
#include "alamo/assets/asset_cache.hpp"
#include "alamo/render/png.hpp"
#include "alamo/render/sim_view.hpp"
#include "alamo/rhi/rhi.hpp"
#include "alamo/vfs/vfs.hpp"

using namespace alamo;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __builtin_FILE(), __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

static std::string env_or_empty(const char* name) {
#ifdef _MSC_VER
    char* v = nullptr;
    size_t len = 0;
    std::string out;
    if (_dupenv_s(&v, &len, name) == 0 && v) {
        out = v;
        std::free(v);
    }
    return out;
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

static bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// Model source without a GPU: one dummy GpuModel per name / primitive (stable addresses).
struct FakeModels final : render::GalacticModelSource {
    std::map<std::string, render::GpuModel> by_name;
    std::map<uint64_t, render::GpuModel> prims;
    std::vector<std::string> asked;
    const render::GpuModel* model(const std::string& name) override {
        if (name.empty()) return nullptr;
        render::GalacticPrimitive k{};
        uint32_t c = 0;
        if (render::parse_primitive_model_name(name, k, c)) return primitive(k, c);
        asked.push_back(name);
        return &by_name[name];
    }
    const render::GpuModel* primitive(render::GalacticPrimitive kind, uint32_t rgba) override {
        return &prims[(uint64_t{static_cast<uint8_t>(kind)} << 32) | rgba];
    }
};

// ----------------------------------------------------------------------------------------- R2 answered items
struct MemFiles final : FileProvider {
    std::map<std::string, std::string> files;  // path as asked
    bool read(std::string_view path, std::vector<uint8_t>& out) const override {
        const auto it = files.find(std::string(path));
        if (it == files.end()) return false;
        out.assign(it->second.begin(), it->second.end());
        return true;
    }
    bool exists(std::string_view path) const override { return files.count(std::string(path)) != 0; }
    std::string origin(std::string_view path) const override { return exists(path) ? "memory" : ""; }
};

// STRATEGIC_GAPS §6.1: TradeRouteLines parser layout (ENEMY 0 / ALLY 1 / other 2 / GOOD 3 / EVIL 4 / CORRUPT 5, block
// field defaults), the galactic light record (ambient 0.05, sun 0.5 from the zoom-out angles, zoom lerp).
static void test_r2_records() {
    CHECK(render::route_settings_index("Enemy") == render::kRouteEnemy && render::route_settings_index("ALLY") == render::kRouteAlly &&
          render::route_settings_index("Neutral") == render::kRouteNeutral && render::route_settings_index("Pirates") == render::kRouteNeutral &&
          render::route_settings_index("good") == render::kRouteGood && render::route_settings_index("Evil") == render::kRouteEvil &&
          render::route_settings_index("Corrupt") == render::kRouteCorrupt);
    MemFiles mf;
    mf.files["Data\\XML\\TradeRouteLines.xml"] =
        "<TradeRouteLines><TradeRouteLine Name=\"Default\">"
        "<Settings_For_Faction Name=\"Ally\"><Color_Zoomed_Out>0, 255, 0, 255</Color_Zoomed_Out><Width>2.5</Width></Settings_For_Faction>"
        "<Settings_For_Faction Name=\"Smugglers\"><Render_Mode>alpha</Render_Mode></Settings_For_Faction>"
        "</TradeRouteLine></TradeRouteLines>";
    mf.files["Data\\XML\\GameConstants.xml"] =
        "<GameConstants><Galactic_Zoom_Out_Light_Angle>0 90 30</Galactic_Zoom_Out_Light_Angle>"
        "<Galactic_Zoom_In_Light_Angle>0, 180, 10</Galactic_Zoom_In_Light_Angle><Galactic_Zoom_Light_Level>0.8</Galactic_Zoom_Light_Level>"
        "<Corruption_Particle_Name> Light_Galactic_Corruption_Particles </Corruption_Particle_Name>"
        "<Particle_Brightness_Per_Corruption_Level> 0.25, 0.75 </Particle_Brightness_Per_Corruption_Level>"
        "</GameConstants>";
    const render::RouteLineStyle s = render::load_route_line_style(mf);
    const render::RouteLineSettings& ally = s.of(render::RouteClass::Ally);
    CHECK(near(ally.zoomed_out.y, 1.0f) && near(ally.width, 2.5f) && near(ally.zoomed_in.x, 0.5f) && ally.render_mode == "ADDITIVE" &&
          ally.texture == "W_galaxy_line_alpha.tga" && ally.spark == "w_galaxy_dot.tga");
    // "Smugglers" -> the neutral block (index 2) with the parser defaults: grey, width 0, its own render mode.
    const render::RouteLineSettings& neutral = s.settings[render::kRouteNeutral];
    CHECK(near(neutral.zoomed_out.x, 0.5f) && near(neutral.width, 0.0f) && neutral.render_mode == "ALPHA");
    CHECK(near(s.color(render::RouteClass::Enemy).x, 225 / 255.0f) && near(s.width(render::RouteClass::Corrupt), 3.0f));  // built-in
    const render::GalacticLightSettings ls = render::load_galactic_light_settings(mf);
    CHECK(near(ls.out_yaw_deg, 90.0f) && near(ls.out_elevation_deg, 30.0f) && near(ls.in_yaw_deg, 180.0f) && near(ls.zoom_light_level, 0.8f));
    const render::Lighting out = render::galactic_lighting(ls, 0.0f), in = render::galactic_lighting(ls, 1.0f);
    const assets::Float3 d = render::light_direction(90.0f * 3.14159265f / 180.0f, 30.0f * 3.14159265f / 180.0f);
    CHECK(near(out.sun.intensity, 0.5f) && near(in.sun.intensity, 0.8f) && near(out.ambient.x, 0.05f) && out.fill[0].intensity == 0.0f &&
          out.fill[1].intensity == 0.0f);
    CHECK(near(out.sun.direction.x, d.x) && near(out.sun.direction.y, d.y) && near(out.sun.direction.z, d.z));
    // R2 t03: halfway through the zoom (the camera's zoom blend) intensity and angles are halfway; the blend is clamped.
    {
        const render::Lighting mid = render::galactic_lighting(ls, 0.5f);
        const assets::Float3 dm = render::light_direction(135.0f * 3.14159265f / 180.0f, 20.0f * 3.14159265f / 180.0f);
        CHECK(near(mid.sun.intensity, 0.65f) && near(mid.sun.direction.x, dm.x) && near(mid.sun.direction.y, dm.y) &&
              near(mid.sun.direction.z, dm.z) && near(mid.ambient.x, 0.05f) && mid.fill[0].intensity == 0.0f);
        CHECK(near(render::galactic_lighting(ls, 7.0f).sun.intensity, 0.8f) && near(render::galactic_lighting(ls, -1.0f).sun.intensity, 0.5f));
        CHECK(near(render::galactic_lighting(ls, std::nanf("")).sun.intensity, 0.5f));
        // The FoC record: zoomed out FoC 160 / 58 -> sun direction (sin a cos e, -cos a cos e, sin e) [M for sin / cos].
        const render::GalacticLightSettings foc;
        const render::Lighting fo = render::galactic_lighting(foc, 0.0f);
        const float a = 160.0f * 3.14159265f / 180.0f, e = 58.0f * 3.14159265f / 180.0f;
        CHECK(near(fo.sun.direction.x, std::sin(a) * std::cos(e)) && near(fo.sun.direction.y, -std::cos(a) * std::cos(e)) &&
              near(fo.sun.direction.z, std::sin(e)) && near(fo.sun.intensity, 0.5f));
    }
    // R2 t01: faction colour table (Color / Display_Font_Color / Big_Fleet_Color; map vs text colour lookups).
    {
        MemFiles ff;
        ff.files["Data\\XML\\FactionFiles.xml"] = "<Factions><File>Factions.xml</File></Factions>";
        ff.files["Data\\XML\\Factions.xml"] =
            "<Factions>"
            "<Faction Name=\"Empire\"><Color> 54, 134, 242, 255 </Color><Display_Font_Color> 24, 133, 251, 255 </Display_Font_Color>"
            "<Big_Fleet_Color> 54, 134, 242, 150 </Big_Fleet_Color><No_Colorization_Color> 255, 255, 255, 255 </No_Colorization_Color></Faction>"
            "<Faction Name=\"Modded\"><Color> 10, 20, 30, 255 </Color></Faction>"
            "<Faction Name=\"Neutral\"><Is_Neutral>Yes</Is_Neutral><Color> 100, 100, 100, 255 </Color>"
            "<Display_Font_Color> 128, 128, 128, 255 </Display_Font_Color></Faction></Factions>";
        const std::vector<render::FactionColor> tab = render::load_faction_colors(ff);
        CHECK(tab.size() == 3 && tab[0].has_display_font && tab[0].has_big_fleet && near(tab[0].big_fleet.w, 150 / 255.0f) && !tab[1].has_display_font);
        materials::float4 c;
        CHECK(render::faction_map_color(tab, "EMPIRE", c) && near(c.x, 54 / 255.0f) && near(c.z, 242 / 255.0f));
        CHECK(render::faction_text_color(tab, "empire", c) && near(c.x, 24 / 255.0f) && near(c.z, 251 / 255.0f) && c.w == 1.0f);
        CHECK(render::faction_text_color(tab, "Modded", c) && near(c.x, 10 / 255.0f));  // no Display_Font_Color: <Color>
        CHECK(render::faction_text_color(tab, "Neutral", c) && near(c.x, 128 / 255.0f));
        CHECK(!render::faction_map_color(tab, "Hutts", c) && !render::faction_text_color(tab, "", c));
    }
    const render::GalacticCorruptionSettings cs = render::load_galactic_corruption_settings(mf);
    CHECK(cs.planet_particle == "Light_Galactic_Corruption_Particles" && cs.line_particle == "Galactic_Corruption_Line_Particles" &&
          near(cs.brightness[0], 0.25f) && near(cs.brightness[1], 0.75f));
    const render::Lighting def = render::galactic_lighting();
    CHECK(near(def.sun.intensity, 0.5f) && near(def.sun.color.x, 1.0f) && near(def.ambient.y, 0.05f));
}

// ----------------------------------------------------------------------------------------- primitives
static void test_primitives() {
    for (uint8_t k = 0; k < static_cast<uint8_t>(render::GalacticPrimitive::Count); ++k) {
        const auto kind = static_cast<render::GalacticPrimitive>(k);
        const uint32_t c = render::rgba8(10, 200, 30, 255);
        const auto m = render::galactic_primitive_model(kind, c);
        CHECK(m && m->submeshes.size() == 1 && m->meshes.size() == 1);
        if (!m || m->submeshes.empty()) continue;
        const assets::Submesh& s = m->submeshes[0];
        CHECK(s.vertex_count > 0 && s.index_count > 0 && s.index_count % 3 == 0 && s.vertices.size() == s.vertex_count * 64u);
        bool in_range = true;
        for (uint32_t i = 0; i < s.index_count; ++i) {
            uint16_t v = 0;
            std::memcpy(&v, s.indices.data() + i * 2u, 2);
            in_range = in_range && v < s.vertex_count;
        }
        CHECK(in_range);
        CHECK(m->bounds.max.x >= m->bounds.min.x && m->bounds.max.z > m->bounds.min.z && m->sphere.radius > 0.0f);
        render::GalacticPrimitive k2{};
        uint32_t c2 = 0;
        CHECK(render::parse_primitive_model_name(m->name, k2, c2) && k2 == kind && c2 == c);
    }
    render::GalacticPrimitive k{};
    uint32_t c = 0;
    CHECK(!render::parse_primitive_model_name("w_planet_fleet.alo", k, c));
    CHECK(render::parse_primitive_model_name("alamo_gal_ring_FF8000FF.alo", k, c) && k == render::GalacticPrimitive::Ring &&
          c == render::rgba8(255, 128, 0, 255));
    CHECK(render::rgba8_of({1.0f, 0.5f, 0.0f, 1.0f}) == render::rgba8(255, 128, 0, 255));
    const auto sphere = render::galactic_primitive_model(render::GalacticPrimitive::Sphere, 0xFFFFFFFFu);
    CHECK(near(sphere->bounds.max.x, render::kPlanetSphereRadius, 0.01f) && near(sphere->bounds.min.z, -render::kPlanetSphereRadius, 0.01f));
    materials::float4 col;
    CHECK(render::parse_color_bytes("128, 0 0, 255", col) && near(col.x, 128 / 255.0f) && col.y == 0.0f && col.z == 0.0f && col.w == 1.0f);
    CHECK(render::parse_color_bytes(" 54, 134, 242, 255 ", col) && near(col.z, 242 / 255.0f));
    CHECK(render::parse_color_bytes("0 235 18", col) && near(col.y, 235 / 255.0f) && col.w == 1.0f);
    CHECK(!render::parse_color_bytes("12", col));
    // Ribbon / ring transforms.
    const assets::Mat3x4 r = render::GalacticSceneBuilder::ribbon_transform({10, 20, 5}, {40, 60, 5}, 2.0f);
    auto apply = [&r](float x, float y, float z) {
        return Vec3{r.m[0][0] * x + r.m[0][1] * y + r.m[0][2] * z + r.m[0][3], r.m[1][0] * x + r.m[1][1] * y + r.m[1][2] * z + r.m[1][3],
                    r.m[2][0] * x + r.m[2][1] * y + r.m[2][2] * z + r.m[2][3]};
    };
    const Vec3 a = apply(0, 0, 0), b = apply(1, 0, 0), side = apply(0, 0.5f, 0);
    CHECK(near(a.x, 10) && near(a.y, 20) && near(b.x, 40) && near(b.y, 60) && near(b.z, 5));
    CHECK(near(std::hypot(side.x - a.x, side.y - a.y), 1.0f) && near(side.z, 5.0f));
    const assets::Mat3x4 ring = render::GalacticSceneBuilder::ring_transform({1, 2, 3}, 7.0f);
    CHECK(ring.m[0][0] == 7.0f && ring.m[1][1] == 7.0f && ring.m[0][3] == 1.0f && ring.m[2][3] == 3.0f);
    // Orbit slots: distinct positions, tangent facings.
    std::set<std::pair<int, int>> seen;
    for (uint32_t s = 0; s < 8; ++s) {
        const Vec3 o = render::GalacticSceneBuilder::orbit_slot_offset(s, 8.0f);
        seen.insert({static_cast<int>(std::lround(o.x * 10)), static_cast<int>(std::lround(o.y * 10))});
        const Vec3 f = render::GalacticSceneBuilder::orbit_slot_facing(s);
        CHECK(near(o.x * f.x + o.y * f.y, 0.0f, 1e-3f) && near(std::hypot(o.x, o.y), 8.0f * 1.9f, 1e-3f));
    }
    CHECK(seen.size() == 8);
}

// --------------------------------------------------------------------------------------- interpolation
static render::GalacticFleetSnap fleet(int32_t id, float x, bool moving) {
    render::GalacticFleetSnap f;
    f.id = id;
    f.position = {x, 0, 0};
    f.facing = {1, 0, 0};
    f.flags = static_cast<uint8_t>(render::kGalFleetDrawable | (moving ? render::kGalFleetMoving : 0));
    return f;
}

static void test_interp() {
    render::GalacticSnapshot a, b;
    a.tick = 10;
    a.mode_frame = 10;
    b.tick = 11;
    b.mode_frame = 11;
    a.fleets = {fleet(1, 0, true), fleet(2, 50, false), fleet(4, 0, true)};
    b.fleets = {fleet(1, 10, true), fleet(2, 60, false), fleet(3, 5, true), fleet(4, 4, false)};
    render::GalacticFrame f;
    render::interpolate_galactic(&a, b, 0.5f, render::InterpMode::Original, f);
    CHECK(f.fleets.size() == 4 && f.fleets[0].position.x == 10.0f && f.anim_frames == 11.0f);
    render::interpolate_galactic(&a, b, 0.5f, render::InterpMode::Smooth, f);
    CHECK(near(f.fleets[0].position.x, 5.0f) && near(f.anim_frames, 10.5f));
    CHECK(f.fleets[1].position.x == 60.0f);  // orbiting: drawn at its slot, not blended
    CHECK(f.fleets[2].position.x == 5.0f);   // new this tick
    CHECK(f.fleets[3].position.x == 4.0f);   // arrived this tick
    render::interpolate_galactic(nullptr, b, 0.25f, render::InterpMode::Smooth, f);
    CHECK(f.fleets[0].position.x == 10.0f);
    render::interpolate_galactic(&a, b, std::nanf(""), render::InterpMode::Smooth, f);
    CHECK(f.alpha == 1.0f && f.fleets[0].position.x == 10.0f);
    render::GalacticSnapshotBuffer buf;
    buf.publish(std::make_shared<render::GalacticSnapshot>(a));
    buf.publish(std::make_shared<render::GalacticSnapshot>(b));
    const auto p = buf.latest();
    CHECK(p.prev && p.curr && p.prev->tick == 10 && p.curr->tick == 11 && buf.published() == 2);
    CHECK(b.find_fleet(3) && b.find_fleet(3)->position.x == 5.0f && !b.find_fleet(7));
}

// ----------------------------------------------------------------------------------------- synthetic
struct RunSpec {
    uint64_t ticks = 900;
    int32_t workers = -1;
    bool snapshots = false;
    std::vector<uint64_t>* digests = nullptr;  // per tick (snapshots only)
    bool serial_snapshot = false;
};
struct RunResult {
    CrcLog log;
    uint64_t state = 0, orders = 0, boarded = 0;
    uint32_t max_moving = 0, owner_changes = 0, arrivals = 0;
    uint32_t slot_checked = 0, slot_mismatch = 0, slotted = 0;  // R2 t01: snapshot slot vs FleetState::slot
    size_t fleets = 0;
    render::GalacticViewStats last;
    std::shared_ptr<const render::GalacticSnapshot> snap;
};

static RunResult run(const render::GalacticWorld& w, const RunSpec& spec) {
    RunResult out;
    jobs::JobSystem js(jobs::JobSystemConfig{spec.workers});
    render::GalacticSessionSpec ss;
    ss.seed = 7;
    render::GalacticSession s(w.types, js, w.rules, w.setup, ss);
    CHECK(s.ok());
    if (!s.ok()) return out;
    render::GalacticTypeTable types;
    types.build(w.types);
    const render::GalacticPalette pal = render::make_galactic_palette(*s.sim(), w.faction_colors);
    render::GalacticSceneBuilder builder(types, w.route_style, render::galactic_view_options(*s.sim()));
    FakeModels models;
    render::GalacticSnapshotBuffer buf;
    render::GalacticFrame frame;
    std::vector<render::Instance> fg, bg;
    std::vector<int32_t> start;
    for (const auto& p : s.sim()->planets()) start.push_back(p.owner);
    std::vector<uint8_t> was_moving;
    for (uint64_t t = 0; t < spec.ticks && s.tick(); ++t) {
        uint32_t moving = 0;
        const auto& fl = s.sim()->fleets();
        was_moving.resize(fl.size(), 0);
        for (size_t i = 0; i < fl.size(); ++i) {
            moving += (fl[i].alive && fl[i].moving) ? 1u : 0u;
            if (was_moving[i] && !fl[i].moving) ++out.arrivals;
            was_moving[i] = fl[i].alive && fl[i].moving ? 1 : 0;
        }
        out.max_moving = std::max(out.max_moving, moving);
        if (!spec.snapshots) continue;
        auto snap = std::make_shared<render::GalacticSnapshot>();
        const bool got = render::capture_galactic_snapshot(s.host(), types, pal, *snap,
                                                           spec.serial_snapshot ? serial_for : jobs::make_parallel_for(js));
        CHECK(got);
        if (spec.digests) spec.digests->push_back(snap->digest());
        // R2 t01: the snapshot carries the sim's own orbit slot (FleetState::slot; -1 in transit), the one the HUD draws.
        for (const render::GalacticFleetSnap& f : snap->fleets) {
            const auto& sf = s.sim()->fleets()[f.index];
            const int8_t want = sf.moving ? static_cast<int8_t>(-1) : sf.slot;
            ++out.slot_checked;
            out.slot_mismatch += f.slot != want ? 1u : 0u;
            out.slotted += f.slot >= 0 ? 1u : 0u;
            if (f.slot >= 0 && f.slot < 3) {
                // unique among the owner's fleets at that planet
                for (const render::GalacticFleetSnap& o : snap->fleets)
                    if (&o != &f && o.owner == f.owner && o.planet == f.planet && o.planet != kInvalidPlanet && o.slot == f.slot)
                        ++out.slot_mismatch;
            }
        }
        buf.publish(snap);
        const auto p = buf.latest();
        for (float alpha : {0.0f, 0.5f}) {
            render::interpolate_galactic(p.prev.get(), *p.curr, alpha, alpha > 0 ? render::InterpMode::Smooth : render::InterpMode::Original, frame);
            builder.build(frame, models, fg, bg, &out.last);
        }
        out.snap = snap;
    }
    out.log = s.host().crc_log();
    out.state = s.state_hash();
    out.orders = s.orders();
    out.boarded = s.boarded();
    out.fleets = s.sim()->fleets().size();
    for (size_t i = 0; i < start.size(); ++i) out.owner_changes += s.sim()->planets()[i].owner != start[i] ? 1u : 0u;
    return out;
}

static bool same_log(const CrcLog& a, const CrcLog& b, const char* what) {
    const CrcDivergence d = first_divergence(a, b);
    std::printf("  %s: %zu vs %zu records: %s\n", what, a.records.size(), b.records.size(), d.message.c_str());
    return !d.diverged && a.records.size() == b.records.size() && !a.records.empty();
}

// Galactic fog: every planet row hidden from the local player -> wireframe globes, no rings / starbases / orbiting
// fleets / corruption smoke, routes neutral; an empty visibility span clears it again.
static void test_fog(const render::GalacticWorld& w, const render::GalacticSnapshot& last) {
    render::GalacticTypeTable types;
    types.build(w.types);
    render::GalacticSceneBuilder builder(types, w.route_style);
    render::GalacticSnapshot s = last;
    std::vector<uint8_t> visible(s.planets.size(), 0);
    visible.back() = 1;  // the background row stays as is
    render::apply_galactic_fog(s, visible);
    uint32_t fogged_planets = 0, orbiting = 0;
    for (const auto& p : s.planets) fogged_planets += ((p.flags & render::kGalFogged) && (p.flags & render::kGalPlanet)) ? 1u : 0u;
    for (const auto& f : s.fleets) orbiting += (!(f.flags & render::kGalFleetMoving) && f.planet < s.planets.size()) ? 1u : 0u;
    CHECK(fogged_planets == 12 && !(s.planets.back().flags & render::kGalFogged));
    for (auto& p : s.planets) p.flags |= render::kGalCorrupted;
    render::GalacticFrame frame;
    render::interpolate_galactic(nullptr, s, 1.0f, render::InterpMode::Original, frame);
    FakeModels models;
    std::vector<render::Instance> fg, bg;
    render::GalacticViewStats st;
    builder.build(frame, models, fg, bg, &st);
    std::printf("  fog: %u globes, %u fleets hidden of %u orbiting, %u rings, %u starbases, %u routes\n", st.fogged, st.fogged_fleets, orbiting,
                st.rings, st.starbases, st.routes);
    CHECK(st.fogged == 12 && st.planets == 12 && st.rings == 0 && st.starbases == 0 && st.fogged_fleets == orbiting);
    const render::GpuModel* globe = models.primitive(render::GalacticPrimitive::Globe, render::rgba8_of(builder.options().fog_globe_color));
    uint32_t globes = 0;
    for (const auto& in : fg) globes += in.model == globe ? 1u : 0u;
    CHECK(globes == 12);
    for (const auto& r : frame.routes) CHECK(render::classify_route(frame.planets, r, 0, {}) == render::RouteClass::Neutral);
    std::vector<render::GalacticCorruptionEffect> fx;
    render::galactic_corruption_effects(frame, render::GalacticCorruptionSettings{}, fx);
    CHECK(fx.empty());
    // The globe primitive has geometry; clearing the fog restores the planet models.
    const auto gm = render::galactic_primitive_model(render::GalacticPrimitive::Globe, 0xFFFF8020u);
    CHECK(gm && gm->submeshes.size() == 1 && gm->submeshes[0].index_count / 3 > 1000 &&
          near(gm->bounds.max.z, render::kPlanetSphereRadius, 0.2f));
    render::apply_galactic_fog(s, {});
    for (const auto& p : s.planets) CHECK(!(p.flags & render::kGalFogged));
}

// GRC 4 corruption particles (fraction -> scalars) and GRC 1 / 2.4 reveal -> fog / hidden routes.
static void test_corruption_fraction(const render::GalacticWorld& w) {
    // 0x4b7ed0 family: a + (b - a) x fraction.
    CHECK(render::galactic_corruption_lerp(0.3f, 1.0f, 0.0f) == 0.3f && render::galactic_corruption_lerp(0.3f, 1.0f, 1.0f) == 1.0f);
    CHECK(near(render::galactic_corruption_lerp(0.3f, 1.0f, 0.5f), 0.65f) && near(render::galactic_corruption_lerp(1.0f, 0.3f, 0.25f), 0.825f));
    // Hand-made frame: 0 gaining through a wrapper (pairs, fraction 0.5); 1 corrupted, no pairs (old behaviour); 2
    // ownership-change gain (transition, no particle object: nothing); 3 removal through a wrapper (fraction 0.25);
    // 4 particles but fogged.
    render::GalacticFrame fr;
    fr.planets.resize(5);
    for (size_t i = 0; i < fr.planets.size(); ++i) {
        fr.planets[i].row = static_cast<PlanetIndex>(i);
        fr.planets[i].flags = render::kGalPlanet;
        fr.planets[i].position = {static_cast<float>(10 * i), 0, 0};
    }
    auto& g = fr.planets[0];
    g.flags |= render::kGalCorruptionParticles | render::kGalCorruptionTransition;
    g.corruption_effective = 3;
    g.corruption_fraction = 0.5f;
    const float gp[6] = {0.3f, 1.0f, 1.0f, 2.0f, 1.0f, 0.5f};
    std::copy(std::begin(gp), std::end(gp), std::begin(g.corruption_pairs));
    fr.planets[1].flags |= render::kGalCorrupted;
    fr.planets[1].corruption_type = fr.planets[1].corruption_effective = 1;
    fr.planets[2].flags |= render::kGalCorruptionTransition;
    fr.planets[2].corruption_effective = 3;
    fr.planets[2].corruption_fraction = 0.4f;
    auto& r = fr.planets[3];
    r.flags |= render::kGalCorrupted | render::kGalCorruptionParticles | render::kGalCorruptionTransition;
    r.corruption_type = 3;
    r.corruption_fraction = 0.25f;
    const float rp[6] = {1.0f, 0.3f, 1.0f, 1.0f, 1.0f, 1.0f};
    std::copy(std::begin(rp), std::end(rp), std::begin(r.corruption_pairs));
    fr.planets[4].flags |= render::kGalCorrupted | render::kGalCorruptionParticles | render::kGalFogged;
    // Routes 1-3 (both corrupted: Corrupt) shown, then hidden.
    fr.routes = {{1, 3, 0}};
    std::vector<render::GalacticCorruptionEffect> fx;
    render::galactic_corruption_effects(fr, render::GalacticCorruptionSettings{}, fx);
    CHECK(fx.size() == 4);
    if (fx.size() == 4) {
        CHECK(fx[0].key == render::galactic_corruption_planet_key(0) && fx[0].scalars && fx[0].corruption_type == 3);
        CHECK(near(fx[0].brightness, 0.65f) && near(fx[0].scale, 1.5f) && near(fx[0].energy, 0.75f));
        CHECK(fx[1].key == render::galactic_corruption_planet_key(1) && !fx[1].scalars && fx[1].brightness == 1.0f);
        CHECK(fx[2].key == render::galactic_corruption_planet_key(3) && fx[2].scalars && near(fx[2].brightness, 0.825f) &&
              fx[2].scale == 1.0f && fx[2].corruption_type == -1);
        CHECK(fx[3].line && fx[3].key == render::galactic_corruption_route_key(0));
    }
    fr.routes[0].flags = render::kGalRouteHidden;
    render::galactic_corruption_effects(fr, render::GalacticCorruptionSettings{}, fx);
    CHECK(fx.size() == 3 && !fx.back().line);
    // Span fog: route ends' raw bytes hide routes independently of the planet fog.
    render::GalacticSnapshot hs;
    hs.planets = fr.planets;
    hs.routes = {{0, 1, 0}, {1, 2, 0}, {2, 9, 0}};
    const std::vector<uint8_t> vis = {1, 1, 1, 1, 1}, raw = {1, 1, 0, 1, 1};
    render::apply_galactic_fog(hs, vis, raw);
    CHECK(!(hs.routes[0].flags & render::kGalRouteHidden) && (hs.routes[1].flags & render::kGalRouteHidden) &&
          (hs.routes[2].flags & render::kGalRouteHidden) && !(hs.planets[2].flags & render::kGalFogged));
    render::apply_galactic_fog(hs, {});
    CHECK(!(hs.routes[1].flags & render::kGalRouteHidden));

    // Real sim (synthetic galaxy): a wrapper gain on an uncorrupted planet, snapshot fields = the sim accessors.
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    render::GalacticSession s(w.types, js, w.rules, w.setup, render::GalacticSessionSpec{});
    CHECK(s.ok());
    if (!s.ok()) return;
    auto* sim = const_cast<galactic::GalacticSim*>(s.sim());  // test only: drive the transition / reveal state
    PlanetIndex target = kInvalidPlanet;
    for (size_t i = 0; i < sim->planets().size() && target == kInvalidPlanet; ++i)
        if (sim->planets()[i].is_planet && sim->planets()[i].corruption == galactic::kCorruptionNone) target = static_cast<PlanetIndex>(i);
    CHECK(target != kInvalidPlanet);
    if (target == kInvalidPlanet) return;
    for (int t = 0; t < 3; ++t) s.tick();
    CHECK(sim->begin_corruption_gain(target, 3, 0, 10.0f));
    for (int t = 0; t < 40; ++t) s.tick();
    render::GalacticTypeTable types;
    types.build(w.types);
    const render::GalacticPalette pal = render::make_galactic_palette(*sim, w.faction_colors);
    render::GalacticSnapshot snap;
    CHECK(render::capture_galactic_snapshot(s.host(), types, pal, snap));
    const render::GalacticPlanetSnap& p = snap.planets[target];
    const float f = sim->corruption_transition_fraction(target);
    const galactic::GalacticSim::CorruptionParticlePairs pp = sim->corruption_particle_pairs(target);
    std::printf("  corruption: planet %u gaining, fraction %.4f, brightness %.3f -> %.3f\n", target, static_cast<double>(f),
                static_cast<double>(pp.brightness_start), static_cast<double>(pp.brightness_end));
    CHECK(f > 0.0f && f < 1.0f && p.corruption_fraction == f && p.corruption_effective == 3 && p.corruption_type == galactic::kCorruptionNone);
    CHECK((p.flags & render::kGalCorruptionTransition) && ((p.flags & render::kGalCorruptionParticles) != 0) == pp.present);
    CHECK(p.corruption_pairs[0] == pp.brightness_start && p.corruption_pairs[1] == pp.brightness_end && p.corruption_pairs[5] == pp.energy_end);
    // Reveal: the raw byte drives the globes (default rule) and the routes; auto-reveal only changes IsPlanetRevealed.
    if (sim->local_player() != galactic::kNoPlayer) {
        for (size_t i = 0; i < sim->planets().size(); ++i) sim->set_planet_revealed(static_cast<PlanetIndex>(i), i != target);
        sim->set_planet_auto_reveal(true);
        render::GalacticSnapshot a = snap, b = snap;
        render::apply_galactic_fog(*sim, sim->local_player(), a);
        render::apply_galactic_fog(*sim, sim->local_player(), b, render::GalacticFogRule::IsPlanetRevealed);
        uint32_t fogged = 0, hidden = 0, touching = 0;
        for (const auto& q : a.planets) fogged += (q.flags & render::kGalFogged) ? 1u : 0u;
        for (const auto& rt : a.routes) {
            hidden += (rt.flags & render::kGalRouteHidden) ? 1u : 0u;
            touching += (rt.a == target || rt.b == target) ? 1u : 0u;
        }
        CHECK(fogged == 1 && (a.planets[target].flags & render::kGalFogged) && hidden == touching && touching > 0);
        uint32_t fogged_b = 0, hidden_b = 0;
        for (const auto& q : b.planets) fogged_b += (q.flags & render::kGalFogged) ? 1u : 0u;
        for (const auto& rt : b.routes) hidden_b += (rt.flags & render::kGalRouteHidden) ? 1u : 0u;
        CHECK(fogged_b == 0 && hidden_b == touching);
        render::apply_galactic_fog(*sim, -1, a);  // no viewpoint: no fog
        for (const auto& q : a.planets) CHECK(!(q.flags & render::kGalFogged));
        std::printf("  reveal: local player %d, %u route(s) hidden by the unrevealed planet\n", sim->local_player(), hidden);
    } else {
        std::printf("  reveal: synthetic galaxy has no local (human) player - sim fog check skipped\n");
    }
}

static void test_synthetic(const render::GalacticWorld& w) {
    RunSpec off;
    const RunResult a = run(w, off);
    RunSpec on = off;
    on.snapshots = true;
    const RunResult b = run(w, on);
    std::printf("synthetic galaxy: 900 ticks, %" PRIu64 " orders, %" PRIu64 " ground units boarded, %zu fleets (max %u in flight, %u arrivals), "
                "%u planets changed hands, state %016" PRIx64 "\n",
                a.orders, a.boarded, a.fleets, a.max_moving, a.arrivals, a.owner_changes, a.state);
    CHECK(same_log(a.log, b.log, "snapshots off vs on"));
    CHECK(a.state == b.state && a.state != 0);
    CHECK(a.log.records.size() == 901);
    CHECK(a.orders > 10 && a.boarded >= 6 && a.max_moving >= 2 && a.arrivals >= 2 && a.owner_changes >= 1);
    std::printf("  fleet slots: %u fleet snapshots checked against FleetState::slot, %u mismatches, %u slotted\n", b.slot_checked, b.slot_mismatch, b.slotted);
    CHECK(b.slot_checked > 100 && b.slot_mismatch == 0 && b.slotted > 0);
    // Snapshot content of the last tick.
    CHECK(b.snap != nullptr);
    if (b.snap) {
        const render::GalacticSnapshot& s = *b.snap;
        CHECK(s.planets.size() == 13 && s.routes.size() == 17 && s.tick == 900 && s.mode_frame == 900);
        uint32_t planets = 0, bg = 0, owned = 0;
        for (const auto& p : s.planets) {
            planets += (p.flags & render::kGalPlanet) ? 1u : 0u;
            bg += (p.flags & render::kGalBackground) ? 1u : 0u;
            owned += p.owner >= 0 && p.owner != 3 ? 1u : 0u;
            CHECK((p.flags & render::kGalDrawable) != 0);
        }
        CHECK(planets == 12 && bg == 1 && owned >= 6);
        CHECK(s.planets[12].id == objects::kNoObjectId && (s.planets[12].flags & render::kGalBackground));
        CHECK(s.planets[0].starbase != objects::kNoObjectId || s.planets[0].owner != 0);
        for (size_t i = 1; i < s.fleets.size(); ++i) CHECK(s.fleets[i - 1].id < s.fleets[i].id);
        for (const auto& f : s.fleets) CHECK((f.flags & render::kGalFleetDrawable) && f.members > 0);
    }
    // Scene of the last frame.
    const render::GalacticViewStats& st = b.last;
    std::printf("  scene: %u planets, %u background, %u starbases, %u rings, %u routes, %u fleets, %u fleet lines\n", st.planets, st.background,
                st.starbases, st.rings, st.routes, st.fleets, st.fleet_lines);
    // FIXPASS_GALACTIC §3.1: no sparkles on Neutral-slot routes.
    std::printf("  route sparks %u of %u routes, corruption path lines %u\n", st.route_sparks, st.routes, st.corruption_paths);
    CHECK(st.planets == 12 && st.background == 1 && st.routes == 17 && st.route_sparks >= 1 && st.route_sparks < st.routes && st.rings >= 6 &&
          st.starbases >= 1 && st.no_model == 0);
    // GALAXY_MAP_PRESENTATION §2.1: only fleets in transit are drawn; orbiting ones are counted, not drawn.
    CHECK(b.snap && st.fleets + st.orbit_fleets + st.fogged_fleets == b.snap->fleets.size());
    if (b.snap) {
        uint32_t transit = 0;
        for (const auto& f : b.snap->fleets) transit += render::GalacticSceneBuilder::fleet_in_transit(f, b.snap->planets.size()) ? 1u : 0u;
        CHECK(st.fleets == transit && st.selection == 0);
    }
    if (b.snap) test_fog(w, *b.snap);
}

static void test_thread_determinism(const render::GalacticWorld& w) {
    RunSpec base;
    base.ticks = 400;
    base.snapshots = true;
    base.serial_snapshot = true;
    std::vector<uint64_t> ref;
    base.digests = &ref;
    base.workers = 0;
    const RunResult r0 = run(w, base);
    for (int32_t workers : {0, 1, 4, 15}) {
        RunSpec s = base;
        std::vector<uint64_t> d;
        s.digests = &d;
        s.workers = workers;
        s.serial_snapshot = false;
        const RunResult r = run(w, s);
        const bool same = d == ref && !ref.empty();
        std::printf("  snapshot digests workers %d: %s (last %016" PRIx64 ")\n", workers, same ? "identical" : "DIFFERENT",
                    d.empty() ? 0 : d.back());
        CHECK(same);
        CHECK(same_log(r0.log, r.log, "CRC log vs workers 0"));
    }
}

static void test_scene_rules(const render::GalacticWorld& w) {
    // Route classes on a hand-made frame: rows 0, 1 Empire (viewpoint 0), 2, 3 Rebel, 4 neutral player 3, 5 corrupted.
    std::vector<render::GalacticPlanetSnap> p(7);
    const int32_t owner[7] = {0, 0, 1, 1, 3, 2, 2};
    for (size_t i = 0; i < p.size(); ++i) {
        p[i].row = static_cast<PlanetIndex>(i);
        p[i].owner = owner[i];
    }
    p[5].flags = p[6].flags = render::kGalCorrupted;
    const std::vector<uint8_t> neutral = {0, 0, 0, 1};
    CHECK(render::classify_route(p, {0, 1}, 0, neutral) == render::RouteClass::Ally);
    CHECK(render::classify_route(p, {2, 3}, 0, neutral) == render::RouteClass::Enemy);
    CHECK(render::classify_route(p, {1, 2}, 0, neutral) == render::RouteClass::Neutral);
    CHECK(render::classify_route(p, {4, 4}, 0, neutral) == render::RouteClass::Neutral);
    // §3.1: the corruption STATE does not pick the slot (corrupted planets of player 2: Enemy without sides).
    CHECK(render::classify_route(p, {5, 6}, 0, neutral) == render::RouteClass::Enemy);
    CHECK(render::classify_route(p, {0, 99}, 0, neutral) == render::RouteClass::Neutral);
    // Faction sides: player 0 EMPIRE (Evil), 1 REBEL (Good), 2 UNDERWORLD (Corrupt); the neutral player stays Neutral.
    const std::vector<uint8_t> sides = {render::kRouteSideEvil, render::kRouteSideGood, render::kRouteSideOther, render::kRouteSideGood};
    CHECK(render::classify_route(p, {0, 1}, 0, neutral, sides) == render::RouteClass::Evil);
    CHECK(render::classify_route(p, {2, 3}, 0, neutral, sides) == render::RouteClass::Good);
    CHECK(render::classify_route(p, {4, 4}, 0, neutral, sides) == render::RouteClass::Neutral);
    CHECK(render::classify_route(p, {5, 6}, 0, neutral, sides) == render::RouteClass::Corrupt);
    CHECK(render::classify_route(p, {1, 2}, 0, neutral, sides) == render::RouteClass::Neutral);
    // Multiplayer flag: same-owner routes Ally / Enemy whatever the faction.
    CHECK(render::classify_route(p, {0, 1}, 0, neutral, sides, true) == render::RouteClass::Ally);
    CHECK(render::classify_route(p, {5, 6}, 0, neutral, sides, true) == render::RouteClass::Enemy);
    // Ownership change recolours at once (slot recomputed from the frame).
    {
        std::vector<render::GalacticPlanetSnap> q = p;
        q[1].owner = 1;
        CHECK(render::classify_route(q, {0, 1}, 0, neutral, sides) == render::RouteClass::Neutral);
        q[0].owner = 1;
        CHECK(render::classify_route(q, {0, 1}, 0, neutral, sides) == render::RouteClass::Good);
    }
    {
        const render::RouteLineStyle st;
        CHECK(render::RouteLineStyle::index(render::RouteClass::Evil) == render::kRouteEvil &&
              render::RouteLineStyle::index(render::RouteClass::Good) == render::kRouteGood);
        CHECK(st.color(render::RouteClass::Evil).z == 1.0f && st.color(render::RouteClass::Evil).x == 0.0f);
        // Zoom blend Out x (1 - f) + In x f; clamped; non-finite -> zoomed out.
        render::RouteLineStyle z;
        auto& e = z.settings[render::RouteLineStyle::index(render::RouteClass::Good)];
        e.zoomed_out = {1, 0, 0, 1};
        e.zoomed_in = {0, 0, 1, 0.5f};
        const materials::float4 h = z.color(render::RouteClass::Good, 0.25f);
        CHECK(near(h.x, 0.75f) && near(h.z, 0.25f) && near(h.w, 0.875f));
        CHECK(z.color(render::RouteClass::Good, 2.0f).z == 1.0f && z.color(render::RouteClass::Good, -1.0f).x == 1.0f);
        CHECK(z.color(render::RouteClass::Good, std::numeric_limits<float>::quiet_NaN()).x == 1.0f);
    }
    // Corruption path lines (§3.2) on a hand-made frame: 0, 1, 2 corrupted, 3 fading gain (0.25), 4 ownership-change gain
    // (no fading), 5 far away (corrupted, no route), 6 fogged corrupted.
    {
        render::GalacticFrame fr;
        fr.planets.resize(7);
        for (uint32_t i = 0; i < 7; ++i) {
            auto& q = fr.planets[i];
            q.row = i;
            q.flags = render::kGalPlanet | render::kGalDrawable;
            q.position = {10.0f * static_cast<float>(i), 0, 0};
            q.corruption_type = 1;
        }
        fr.planets[3].corruption_type = -1;
        fr.planets[3].flags |= render::kGalCorruptionTransition | render::kGalCorruptionFading;
        fr.planets[3].corruption_fraction = 0.25f;
        fr.planets[4].corruption_type = -1;
        fr.planets[4].flags |= render::kGalCorruptionTransition;
        fr.planets[4].corruption_fraction = 0.5f;
        fr.planets[5].position = {1000, 0, 0};
        fr.planets[6].flags |= render::kGalFogged;
        const render::CorruptionPathSettings cs;
        std::vector<render::CorruptionPathLine> lines;
        render::galactic_corruption_paths(fr, cs, lines);
        // Pairs among {0, 1, 2, 3} (all within 100): (0,1) (0,2) (1,2) idle, owned by the lower row at factor 1; (0,3) (1,3)
        // (2,3) owned by 3 only, factor 0.25 -> (int)(36 x 0.25) = 9.
        CHECK(lines.size() == 6);
        uint32_t idle = 0, fading = 0;
        for (const auto& l : lines) {
            if (l.owner == 3) {
                ++fading;
                CHECK(near(l.factor, 0.25f) && l.rgba[0] == 9 && l.rgba[1] == 9 && l.rgba[2] == 0 && l.rgba[3] == 255);
            } else {
                ++idle;
                CHECK(l.owner < l.other && l.other < 3 && l.factor == 1.0f && l.rgba[0] == 36 && l.rgba[1] == 36 && l.rgba[2] == 0);
            }
            CHECK(l.owner != 4 && l.other != 4 && l.owner != 5 && l.other != 5 && l.owner != 6 && l.other != 6);
        }
        CHECK(idle == 3 && fading == 3);
        // Fading removal of planet 1 (corrupted, transition + fading): factor 1 - 0.25; 3 keeps its own lines; a route
        // makes 5 a neighbour of 0 despite the distance.
        fr.planets[1].flags |= render::kGalCorruptionTransition | render::kGalCorruptionFading;
        fr.planets[1].corruption_fraction = 0.25f;
        fr.routes = {{0, 5}};
        render::galactic_corruption_paths(fr, cs, lines);
        uint32_t own1 = 0, both13 = 0, to5 = 0;
        for (const auto& l : lines) {
            if (l.owner == 1) {
                ++own1;
                CHECK(near(l.factor, 0.75f) && l.rgba[0] == 27);
            }
            both13 += ((l.owner == 1 && l.other == 3) || (l.owner == 3 && l.other == 1)) ? 1u : 0u;
            to5 += (l.owner == 0 && l.other == 5) ? 1u : 0u;
        }
        CHECK(own1 == 3 && both13 == 2 && to5 == 1);
        // Factor 0 (gain just begun): black, dropped.
        fr.planets[3].corruption_fraction = 0.0f;
        render::galactic_corruption_paths(fr, cs, lines);
        for (const auto& l : lines) CHECK(l.owner != 3);
        // Snapshot override of the fading flag: only rows in transition take it.
        render::GalacticSnapshot sn;
        sn.planets = fr.planets;
        const std::vector<uint8_t> fade = {1, 0, 1, 0, 1};
        render::apply_galactic_corruption_fading(sn, fade);
        CHECK(!(sn.planets[0].flags & render::kGalCorruptionFading) && !(sn.planets[1].flags & render::kGalCorruptionFading) &&
              !(sn.planets[3].flags & render::kGalCorruptionFading) && (sn.planets[4].flags & render::kGalCorruptionFading));
    }
    // Corruption effects: planets 5, 6 (corrupted planets) and the corrupt route 5-6 at its midpoint, along the route.
    {
        render::GalacticFrame fr;
        fr.planets = p;
        for (auto& q : fr.planets) q.flags |= render::kGalPlanet;
        fr.planets[5].position = {0, 0, 0};
        fr.planets[6].position = {0, 20, 0};
        fr.routes = {{0, 1}, {5, 6}};
        const render::GalacticCorruptionSettings cs;
        std::vector<render::GalacticCorruptionEffect> fx;
        render::galactic_corruption_effects(fr, cs, fx);
        CHECK(fx.size() == 3);
        if (fx.size() == 3) {
            CHECK(fx[0].key == render::galactic_corruption_planet_key(5) && !fx[0].line && *fx[0].type == cs.planet_particle);
            CHECK(fx[1].key == render::galactic_corruption_planet_key(6));
            CHECK(fx[2].key == render::galactic_corruption_route_key(1) && fx[2].line && *fx[2].type == cs.line_particle);
            CHECK(near(fx[2].transform.m[1][3], 10.0f) && near(fx[2].transform.m[1][0], 1.0f));  // +X along +Y
        }
    }
    // Fleet / planet models of the synthetic types.
    render::GalacticTypeTable types;
    types.build(w.types);
    CHECK(types.fleet_type() != objects::kNone);
    render::GalacticSceneBuilder b(types);
    render::GalacticFleetSnap f;
    f.model_type = types.fleet_type();
    render::GalacticPrimitive k{};
    uint32_t c = 0;
    CHECK(render::parse_primitive_model_name(b.fleet_model(f), k, c) && k == render::GalacticPrimitive::Fleet);
    const objects::TypeIndex kuat = types.find("SYN_KUAT");
    CHECK(kuat != objects::kNone && near(b.planet_radius(kuat), 1.8f * render::kPlanetSphereRadius));
    // Orbiting fleet drawn at its slot; a fleet in flight at its sim position.
    std::vector<render::GalacticPlanetSnap> pl(1);
    pl[0].type = kuat;
    pl[0].position = {100, 50, 10};
    f.planet = 0;
    f.orbit_slot = 2;
    f.position = {100, 50, 10};
    const Vec3 at = b.fleet_draw_position(f, pl);
    const Vec3 off = render::GalacticSceneBuilder::orbit_slot_offset(2, b.planet_radius(kuat));
    CHECK(near(at.x, 100 + off.x) && near(at.y, 50 + off.y) && near(at.z, 10.5f));
    f.flags = render::kGalFleetMoving;
    f.position = {3, 4, 5};
    const Vec3 mv = b.fleet_draw_position(f, pl);
    CHECK(mv.x == 3.0f && mv.y == 4.0f && mv.z == 5.0f);
}

// GALAXY_MAP_PRESENTATION §2.1 / §3 on a hand-made frame: fleets drawn only in transit, one _ALT each (stealth -> 1),
// along the sim facing; the selection model at the selected planet, facing the camera, tinted with the owner colour
// (fogged: the unrevealed colour), none without a selection. Snapshot: kGalFleetStealthed from the members' stealth bytes.
static void test_fleets_and_selection(const render::GalacticWorld& w) {
    render::GalacticTypeTable types;
    types.build(w.types);
    const objects::TypeIndex kuat = types.find("SYN_KUAT");
    CHECK(kuat != objects::kNone && types.fleet_type() != objects::kNone);
    if (kuat == objects::kNone || types.fleet_type() == objects::kNone) return;
    render::GalacticFrame frame;
    frame.planets.resize(2);
    for (uint32_t i = 0; i < 2; ++i) {
        auto& p = frame.planets[i];
        p.row = i;
        p.id = static_cast<objects::ObjectId>(100 + i);
        p.type = kuat;
        p.owner = 0;
        p.position = {200.0f * static_cast<float>(i), 50, 0};
        p.team_color = {0.25f, 0.5f, 1.0f, 1};
        p.flags = render::kGalPlanet | render::kGalDrawable;
    }
    frame.planets[1].flags |= render::kGalFogged;
    auto fleet = [&](objects::ObjectId id, bool moving, bool stealth) {
        render::GalacticFleetSnap f;
        f.id = id;
        f.model_type = types.fleet_type();
        f.flags = render::kGalFleetDrawable;
        if (moving) f.flags |= render::kGalFleetMoving;
        if (stealth) f.flags |= render::kGalFleetStealthed;
        f.planet = moving ? kInvalidPlanet : 0;
        f.position = {10, 20, 0};
        f.facing = {0, 1, 0};
        return f;
    };
    frame.fleets = {fleet(1, false, false), fleet(2, true, false), fleet(3, true, true), fleet(4, false, true)};
    frame.fleets[3].planet = 1;  // orbiting the fogged planet
    render::GalacticSceneBuilder b(types);
    b.options().routes = b.options().rings = b.options().starbases = b.options().fleet_lines = false;
    b.options().route_sparks = false;
    FakeModels models;
    std::vector<render::Instance> fg, bg;
    render::GalacticViewStats st;
    b.build(frame, models, fg, bg, &st);
    CHECK(st.fleets == 2 && st.orbit_fleets == 1 && st.fogged_fleets == 1 && st.stealth_fleets == 1 && st.selection == 0);
    // Planets first (row order: the model, then the fog globe), then the fleets in id order.
    CHECK(fg.size() == 4);
    if (fg.size() == 4) {
        const render::Instance& f2 = fg[2];
        const render::Instance& f3 = fg[3];
        CHECK(f2.alt == 0 && f3.alt == 1 && f2.team_color_override);
        CHECK(near(f2.transform.m[0][3], 10.0f) && near(f2.transform.m[1][3], 20.0f));
        const float sc = types[types.fleet_type()].scale;
        CHECK(near(f2.transform.m[1][1], sc) && near(f2.transform.m[0][1], 0.0f));  // t32: the darts' tips (+Y) along the hop (+Y)
    }
    // Selection: planet 100, camera south of it, 45 degrees up.
    render::GalacticSelection sel;
    sel.planet = 100;
    sel.eye = {0, -100, 100};
    sel.target = {0, 0, 0};
    sel.up = {0, 0, 1};
    b.set_selection(sel);
    b.build(frame, models, fg, bg, &st);
    const std::string sm = b.options().selection_model;
    CHECK(st.selection == 1 && sm == "w_planet_select_neutral.alo" && models.by_name.count(sm) == 1);
    auto find_sel = [&]() -> const render::Instance* {
        for (const auto& in : fg)
            if (in.model == &models.by_name[sm]) return &in;
        return nullptr;
    };
    const render::Instance* si = find_sel();
    CHECK(si != nullptr);
    if (si) {
        const float r = std::sqrt(0.5f);
        CHECK(near(si->transform.m[0][0], 1.0f) && near(si->transform.m[1][0], 0.0f) && near(si->transform.m[2][0], 0.0f));  // X right
        CHECK(near(si->transform.m[1][2], -r) && near(si->transform.m[2][2], r));  // Z toward the camera
        CHECK(near(si->transform.m[1][1], r) && near(si->transform.m[2][1], r));   // Y screen up
        CHECK(near(si->transform.m[0][3], 0.0f) && near(si->transform.m[1][3], 50.0f));
        CHECK(near(si->light_scale.x, 0.25f) && near(si->light_scale.y, 0.5f) && near(si->light_scale.z, 1.0f) && si->light_scale.w == 1.0f);
        CHECK(!si->team_color_override);
    }
    // QA D6: rings, selection and fleet lines are gone from f = 0.25 on (planets and fleets stay), half at f = 0.125. t39:
    // the route stays to f = 1 (FIXPASS_GALACTIC §3.1) in Color_Zoomed_In; only its sparkle goes.
    {
        CHECK(render::GalacticSceneBuilder::overlay_fade(0.0f) == 1.0f && near(render::GalacticSceneBuilder::overlay_fade(0.125f), 0.5f) &&
              render::GalacticSceneBuilder::overlay_fade(0.25f) == 0.0f && render::GalacticSceneBuilder::overlay_fade(1.0f) == 0.0f);
        render::GalacticFrame fr = frame;
        fr.routes.push_back({0, 1, 0});
        fr.fleets[1].to = 0;
        render::GalacticSceneBuilder o(types);
        o.set_selection(sel);
        std::vector<render::Instance> ofg, obg;
        render::GalacticViewStats os;
        o.build(fr, models, ofg, obg, &os);
        CHECK(os.routes == 1 && os.rings == 1 && os.fleet_lines == 1 && os.selection == 1);
        o.set_route_zoom(0.125f);
        o.build(fr, models, ofg, obg, &os);
        CHECK(os.routes == 1 && os.rings == 1 && os.fleet_lines == 1 && os.selection == 1);
        for (const render::Instance& in : ofg)
            if (in.model == &models.by_name[sm]) CHECK(near(in.light_scale.z, 0.5f) && near(in.light_scale.w, 0.5f));
        o.set_route_zoom(0.25f);
        o.build(fr, models, ofg, obg, &os);
        CHECK(os.routes == 1 && os.route_sparks == 0 && os.rings == 0 && os.fleet_lines == 0 && os.selection == 0 && os.planets == 2 &&
              os.fleets == 2);
        o.set_route_zoom(1.0f);
        o.build(fr, models, ofg, obg, &os);
        CHECK(os.routes == 1 && os.route_sparks == 0);
        // Additive, tinted per instance with Color_Zoomed_In (Neutral 150).
        bool tinted = false;
        for (const render::Instance& in : ofg) tinted = tinted || (near(in.light_scale.x, 150.0f / 255.0f) && near(in.light_scale.w, 1.0f));
        CHECK(tinted);
        // t49: a route runs centre to centre (the depth test hides the part inside the planets), at every zoom.
        {
            const Vec3 pa = fr.planets[0].position, pb = fr.planets[1].position;
            const assets::Mat3x4 rt = render::GalacticSceneBuilder::ribbon_transform(pa, pb, o.style().width(render::RouteClass::Neutral));
            bool centred = false;
            for (const render::Instance& in : ofg)
                centred = centred || (near(in.transform.m[0][3], pa.x) && near(in.transform.m[1][3], pa.y) && near(in.transform.m[0][0], rt.m[0][0]) && near(in.transform.m[1][0], rt.m[1][0]));
            CHECK(centred);
        }
        // t49: corruption paths have no zoom hide rule either (FIXPASS_GALACTIC §3.2): full tint at f = 1, as at f = 0.
        {
            render::GalacticFrame cf = fr;
            for (auto& q : cf.planets) {
                q.corruption_type = 0;
                q.flags &= ~render::kGalFogged;
            }
            cf.fleets.clear();
            o.build(cf, models, ofg, obg, &os);
            CHECK(os.corruption_paths == 1);
            o.set_route_zoom(0.0f);
            o.build(cf, models, ofg, obg, &os);
            CHECK(os.corruption_paths == 1);
        }
        // t34: a fogged planet whose model has an _ALT2 mesh draws that alternate; without one, the procedural globe.
        render::GalacticFrame ff = frame;
        ff.routes.clear();
        ff.fleets.clear();
        render::GalacticSceneBuilder gb(types);
        FakeModels gm;
        std::vector<render::Instance> gfg, gbg;
        render::GalacticViewStats gs;
        gb.build(ff, gm, gfg, gbg, &gs);
        const render::GpuModel* globe = gm.primitive(render::GalacticPrimitive::Globe, render::rgba8_of(gb.options().fog_globe_color));
        CHECK(gs.fogged == 1 && gfg.size() >= 2 && gfg[1].model == globe && gfg[1].alt == 0);
        const std::string fn = gb.planet_model(ff.planets[1]);
        render::GpuModel* fm = const_cast<render::GpuModel*>(gm.model(fn));  // a primitive name in the synthetic world
        fm->max_alt = 2;
        gb.build(ff, gm, gfg, gbg, &gs);
        CHECK(gs.fogged == 1 && gfg.size() >= 2 && gfg[1].model == fm && gfg[1].alt == 2);
    }
    sel.planet = 101;  // fogged -> the unrevealed colour
    b.set_selection(sel);
    b.build(frame, models, fg, bg, &st);
    si = find_sel();
    CHECK(st.selection == 1 && si && near(si->light_scale.x, b.options().selection_unrevealed_color.x));
    sel.planet = 7777;  // not a planet of the frame
    b.set_selection(sel);
    b.build(frame, models, fg, bg, &st);
    CHECK(st.selection == 0 && st.no_model == 0 && !find_sel());
    sel.planet = objects::kNoObjectId;
    b.set_selection(sel);
    b.build(frame, models, fg, bg, &st);
    CHECK(st.selection == 0 && !find_sel());
    // Snapshot: a live fleet with ships; every non-transport member stealthed -> kGalFleetStealthed, one cleared -> not.
    jobs::JobSystem js(jobs::JobSystemConfig{1});
    render::GalacticSessionSpec ss;
    ss.seed = 7;
    render::GalacticSession s(w.types, js, w.rules, w.setup, ss);
    CHECK(s.ok());
    if (!s.ok()) return;
    // The session hands renderers a const sim; the test toggles the stealth byte as an ability activation would.
    auto& gs = const_cast<galactic::GalacticSim&>(*s.sim());
    const render::GalacticPalette pal = render::make_galactic_palette(gs, w.faction_colors);
    uint32_t pick = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < gs.fleets().size() && pick == 0xFFFFFFFFu; ++i) {
        const auto& f = gs.fleets()[i];
        if (!f.alive) continue;
        for (objects::ObjectHandle h : f.contents)
            if (gs.store().alive(h) && !gs.is_company_transport(h)) pick = i;
    }
    CHECK(pick != 0xFFFFFFFFu);
    if (pick == 0xFFFFFFFFu) return;
    const objects::ObjectId fid = gs.store().id(gs.fleets()[pick].object);
    render::GalacticSnapshot snap;
    render::build_galactic_snapshot(gs, 0, types, pal, snap);
    CHECK(snap.find_fleet(fid) && !(snap.find_fleet(fid)->flags & render::kGalFleetStealthed));
    std::vector<objects::ObjectHandle> ships;
    for (objects::ObjectHandle h : gs.fleets()[pick].contents)
        if (gs.store().alive(h) && !gs.is_company_transport(h)) ships.push_back(h);
    for (objects::ObjectHandle h : ships) gs.set_galactic_stealth(h, true);
    render::build_galactic_snapshot(gs, 0, types, pal, snap);
    CHECK(snap.find_fleet(fid) && (snap.find_fleet(fid)->flags & render::kGalFleetStealthed));
    gs.set_galactic_stealth(ships.back(), false);
    render::build_galactic_snapshot(gs, 0, types, pal, snap);
    CHECK(snap.find_fleet(fid) && !(snap.find_fleet(fid)->flags & render::kGalFleetStealthed));
    std::printf("fleets / selection: fleet %u with %zu ships, stealth flag follows the members' bytes\n", static_cast<unsigned>(fid),
                ships.size());
}

// ------------------------------------------------------------------------------------------- GPU
struct Gpu {
    rhi::Context ctx;
    rhi::Allocator alloc;
    rhi::Uploader up;
    rhi::FrameRing ring;
    render::Renderer renderer;
};

struct Offscreen {
    rhi::Image color;
    rhi::Buffer readback;
    uint32_t w = 0, h = 0;
    bool create(Gpu& g, uint32_t width, uint32_t height) {
        w = width;
        h = height;
        rhi::ImageDesc cd;
        cd.format = rhi::Format::RGBA8;
        cd.width = w;
        cd.height = h;
        cd.mips = 1;
        cd.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        return g.alloc.create_image(cd, color) &&
               g.alloc.create_buffer(static_cast<VkDeviceSize>(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::MemoryUsage::Readback, readback);
    }
    void destroy(Gpu& g) {
        g.alloc.destroy(color);
        g.alloc.destroy(readback);
    }
};

static void test_gpu(const render::GalacticWorld& w, const std::string& src_dir) {
    const bool strict = render::setup_validation_env(src_dir);
    {
        rhi::Loader probe;
        if (!probe.open()) {
            std::printf("SKIP gpu: no Vulkan loader\n");
            return;
        }
    }
    Gpu g;
    rhi::ContextDesc cd;
    cd.app_name = "alamo_render_galactic_tests";
    cd.validation = strict ? rhi::Validation::Required : rhi::Validation::IfAvailable;
    if (!g.ctx.init(cd)) {
        CHECK(!strict);
        std::printf("SKIP gpu: no suitable Vulkan 1.3 device\n");
        return;
    }
    std::printf("gpu: %s, validation %s%s\n", g.ctx.caps().name.c_str(), g.ctx.validation_enabled() ? "on" : "off", strict ? " (required)" : "");
    rhi::FrameRingDesc fd;
    fd.frames_in_flight = 2;
    fd.uniform_bytes = 16ull << 20;
    fd.recording_threads = jobs::JobSystem::default_worker_count() + 1;
    CHECK(g.alloc.init(g.ctx) && g.up.init(g.ctx, g.alloc) && g.ring.init(g.ctx, g.alloc, fd));
    render::RendererDesc rd;
    rd.color_format = VK_FORMAT_R8G8B8A8_UNORM;
    rd.depth_format = VK_FORMAT_D32_SFLOAT;
    rd.translated = false;
    const bool rinit = g.renderer.init(g.ctx, g.alloc, g.up, rd);
    CHECK(rinit);
    g.renderer.set_frame_ring(&g.ring);
    g.renderer.set_lighting(render::galactic_lighting());
    Offscreen o;
    CHECK(o.create(g, 640, 360));
    if (rinit) {
        RunSpec off;
        off.ticks = 120;
        const RunResult ref = run(w, off);
        jobs::JobSystem js(jobs::JobSystemConfig{-1});
        render::GalacticSessionSpec ss;
        ss.seed = 7;
        render::GalacticSession s(w.types, js, w.rules, w.setup, ss);
        render::GalacticTypeTable types;
        types.build(w.types);
        const render::GalacticPalette pal = render::make_galactic_palette(*s.sim(), w.faction_colors);
        render::GalacticSceneBuilder builder(types, w.route_style, render::galactic_view_options(*s.sim()));
        render::GalacticModels models(g.renderer, nullptr, nullptr, nullptr);
        render::GalacticSnapshotBuffer buf;
        render::GalacticFrame frame;
        std::vector<render::Instance> fg, bg;
        render::CameraParams cp;
        cp.target = {0, 0, 0};
        cp.yaw = -1.5707963f;  // looking north (+Y)
        cp.pitch = 0.5235988f; // 30 degrees down (GMC_InitialPitchAngleDegrees)
        cp.fov_x = render::kGalacticFovX;
        cp.distance = 0.62f * 460.0f / std::tan(0.5f * cp.fov_x);
        cp.znear = render::kGalacticNear;
        cp.zfar = 10000.0f;
        const render::ViewMatrices vm = render::camera_view(cp, o.w, o.h);
        render::DrawStats st;
        render::RecordOptions ro;
        ro.post = false;
        ro.shadows = false;
        ro.space = true;
        ro.galactic = true;
        uint32_t frames = 0;
        bool all_drawn = true;
        auto frame_once = [&](float alpha, render::InterpMode mode) {
            rhi::Frame* f = g.ring.begin_frame();
            if (!f) return false;
            VkCommandBuffer cmd = f->cmd();
            const rhi::DeviceFns& fn = g.ctx.vkd();
            const rhi::AcquireResult acq = g.up.record_acquires(cmd);
            models.update();
            const auto p = buf.latest();
            render::interpolate_galactic(p.prev.get(), *p.curr, alpha, mode, frame);
            builder.build(frame, models, fg, bg);
            render::PassTarget pt;
            pt.color = o.color.handle;
            pt.color_view = o.color.view;
            pt.extent = {o.w, o.h};
            pt.color_after = rhi::ImageState::TransferSrc;
            const bool drawn = g.renderer.render(*f, cmd, pt, vm, fg, render::DrawMode::Lit, ro, &st, nullptr, bg);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {o.w, o.h, 1};
            fn.vkCmdCopyImageToBuffer(cmd, o.color.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, o.readback.handle, 1, &copy);
            const VkBufferMemoryBarrier2 hb = rhi::buffer_barrier(o.readback.handle, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                                                  VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
            rhi::cmd_barrier(fn, cmd, {}, std::span<const VkBufferMemoryBarrier2>(&hb, 1));
            VkSemaphoreSubmitInfo wait = rhi::semaphore_info(g.up.timeline(), acq.wait_value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
            rhi::FrameSubmit fs;
            if (acq.wait_value) fs.waits = {&wait, 1};
            if (!g.ring.end_frame(*f, fs)) return false;
            g.ring.wait_idle();
            g.alloc.invalidate(o.readback.alloc);
            return drawn;
        };
        for (uint64_t t = 0; t < off.ticks && s.tick(); ++t) {
            auto snap = std::make_shared<render::GalacticSnapshot>();
            render::capture_galactic_snapshot(s.host(), types, pal, *snap, jobs::make_parallel_for(js));
            buf.publish(std::move(snap));
            all_drawn = frame_once(0.0f, render::InterpMode::Original) && all_drawn;
            all_drawn = frame_once(0.5f, render::InterpMode::Smooth) && all_drawn;
            frames += 2;
        }
        for (int i = 0; i < 4; ++i) all_drawn = frame_once(1.0f, render::InterpMode::Original) && all_drawn;  // primitives resident
        CHECK(all_drawn && frames == 240);
        // Host smoke waits on all_finished(): requested counts every model kind (line models included). Line models are
        // keyed by texture only (tint per instance), so many path-line tints share one model.
        const uint32_t prims_before = models.requested();
        for (uint32_t k = 0; k < 40; ++k) (void)models.line("w_galaxy_line.tga", render::rgba8(static_cast<uint8_t>(k), 36, 0, 255));
        (void)models.line("W_GALAXY_LINE.TGA", 0xFFFFFFFFu);
        const uint32_t line_models = models.requested() - prims_before;
        for (int i = 0; i < 2; ++i) all_drawn = frame_once(1.0f, render::InterpMode::Original) && all_drawn;
        std::printf("gpu: models %u requested, %u ready, %u failed\n", models.requested(), models.ready(), models.failed());
        CHECK(line_models <= 1 && models.all_finished() && models.ready() + models.failed() == models.requested());
        CHECK(same_log(ref.log, s.host().crc_log(), "rendering off vs on (120 ticks, 240 frames)"));
        CHECK(ref.state == s.state_hash());
        const auto* px = static_cast<const uint8_t*>(o.readback.mapped());
        uint32_t lit = 0;
        for (size_t i = 0; i < static_cast<size_t>(o.w) * o.h; ++i)
            if (px[i * 4 + 0] > 40 || px[i * 4 + 1] > 40 || px[i * 4 + 2] > 40) ++lit;
        std::printf("gpu: synthetic galaxy frame: %zu + %zu instances, %u draws, %u sky draws, %u lit pixels\n", fg.size(), bg.size(), st.draws,
                    st.sky_draws, lit);
        CHECK(st.draws >= 40 && st.sky_draws >= 1 && lit > 2000);
        const std::vector<uint8_t> rgba(px, px + static_cast<size_t>(o.w) * o.h * 4);
        const std::string png = src_dir + "/logs/render_galactic_synthetic_test.png";
        CHECK(render::write_png_rgba(png, o.w, o.h, rgba));
        std::printf("wrote %s\n", png.c_str());
        g.ring.wait_idle();
        models.destroy();
    }
    g.ring.wait_idle();
    o.destroy(g);
    g.renderer.shutdown();
    g.ring.shutdown();
    g.up.shutdown();
    g.alloc.shutdown();
    const uint32_t ve = g.ctx.validation_errors(), vw = g.ctx.validation_warnings();
    std::printf("gpu: validation %u errors, %u warnings\n", ve, vw);
    CHECK(ve == 0 && vw == 0);
    g.ctx.shutdown();
}

// ------------------------------------------------------------------------------------- real data
static void test_real_data() {
    const std::string root = env_or_empty("ALAMO_FOC_DATA");
    if (root.empty()) {
        std::printf("real data: ALAMO_FOC_DATA not set, skipped\n");
        return;
    }
    vfs::VfsConfig cfg;
    cfg.foc_root = root;
    std::string err;
    auto files = vfs::Vfs::create(cfg, &err);
    CHECK(files != nullptr);
    if (!files) return;
    auto w = render::load_galactic_world(std::move(files), render::kDefaultGalacticCampaign, &err);
    CHECK(w != nullptr);
    if (!w) {
        std::printf("real data: %s\n", err.c_str());
        return;
    }
    RunSpec off;
    off.ticks = 600;
    off.workers = 4;
    const RunResult a = run(*w, off);
    RunSpec on = off;
    on.snapshots = true;
    const RunResult b = run(*w, on);
    std::printf("%s: %zu planet rows, %zu routes, %" PRIu64 " orders, %zu fleets (max %u in flight), %u planets changed hands\n", w->name.c_str(),
                w->setup.map.planets.size(), w->setup.map.routes.size(), a.orders, a.fleets, a.max_moving, a.owner_changes);
    CHECK(same_log(a.log, b.log, "snapshots off vs on (600 ticks)"));
    CHECK(a.state == b.state);
    CHECK(b.snap != nullptr);
    if (!b.snap) return;
    render::GalacticTypeTable types;
    types.build(w->types);
    render::GalacticSceneBuilder builder(types);
    uint32_t bg = 0, drawable = 0;
    bool core = false;
    for (const auto& p : b.snap->planets) {
        drawable += (p.flags & render::kGalDrawable) ? 1u : 0u;
        if (p.flags & render::kGalBackground) {
            ++bg;
            core = core || builder.planet_model(p) == "w_galaxy00.alo";
        }
    }
    std::printf("  snapshot: %zu planets (%u drawable, %u background), %zu routes, %zu fleets; scene %u planets, %u starbases, %u routes, "
                "%u fleets\n",
                b.snap->planets.size(), drawable, bg, b.snap->routes.size(), b.snap->fleets.size(), b.last.planets, b.last.starbases,
                b.last.routes, b.last.fleets);
    CHECK(drawable + 2 >= b.snap->planets.size() && core && bg >= 1);
    CHECK(!b.snap->fleets.empty() && b.last.fleets + b.last.orbit_fleets + b.last.fogged_fleets == b.snap->fleets.size() &&
          b.last.routes + 2 >= b.snap->routes.size() &&
          b.last.routes >= 8);
    bool fleet_model = false;
    for (const auto& f : b.snap->fleets) fleet_model = fleet_model || builder.fleet_model(f) == "w_planet_fleet.alo";
    CHECK(fleet_model);
}

// Planet portraits (planet_portrait.hpp) on FoC data: Kuat's and Kashyyyk's galactic models rendered into the 512 x 512 RGBA target
// through the frame ring, read back, served as an RGBA8 texture by "planet_portrait:Kuat"; opaque pixels in the
// centre, transparent corners, cached (same pointer), unknown types fail; 0 validation messages.
static void test_planet_portrait(const std::string& src_dir) {
    const std::string root = env_or_empty("ALAMO_FOC_DATA");
    if (root.empty()) {
        std::printf("planet portrait: ALAMO_FOC_DATA not set, skipped\n");
        return;
    }
    const bool strict = render::setup_validation_env(src_dir);
    {
        rhi::Loader probe;
        if (!probe.open()) {
            std::printf("SKIP planet portrait: no Vulkan loader\n");
            return;
        }
    }
    vfs::VfsConfig cfg;
    cfg.foc_root = root;
    std::string err;
    auto files = vfs::Vfs::create(cfg, &err);
    CHECK(files != nullptr);
    if (!files) return;
    auto w = render::load_galactic_world(std::move(files), render::kDefaultGalacticCampaign, &err);
    CHECK(w != nullptr);
    if (!w) return;
    Gpu g;
    rhi::ContextDesc cd;
    cd.app_name = "alamo_render_galactic_tests_portrait";
    cd.validation = strict ? rhi::Validation::Required : rhi::Validation::IfAvailable;
    if (!g.ctx.init(cd)) {
        CHECK(!strict);
        std::printf("SKIP planet portrait: no suitable Vulkan 1.3 device\n");
        return;
    }
    jobs::JobSystem js(jobs::JobSystemConfig{-1});
    rhi::FrameRingDesc fd;
    fd.frames_in_flight = 2;
    fd.uniform_bytes = 64ull << 20;
    fd.recording_threads = jobs::JobSystem::default_worker_count() + 1;
    CHECK(g.alloc.init(g.ctx) && g.up.init(g.ctx, g.alloc) && g.ring.init(g.ctx, g.alloc, fd));
    auto effects = std::make_unique<render::EffectCache>(w->files.get());
    render::RendererDesc rd;
    rd.color_format = VK_FORMAT_B8G8R8A8_UNORM;  // a swapchain-like BGRA target: the readback swaps to RGBA
    rd.depth_format = VK_FORMAT_D32_SFLOAT;
    rd.effects = effects.get();
    rd.js = &js;
    const bool rinit = g.renderer.init(g.ctx, g.alloc, g.up, rd);
    CHECK(rinit);
    if (rinit) {
        g.renderer.set_frame_ring(&g.ring);
        g.renderer.set_lighting(render::galactic_lighting());
        auto cache = std::make_unique<assets::AssetCache>(*w->files, effects->model_load_options());
        auto streamer = std::make_unique<render::ModelStreamer>(g.renderer, *cache, js);
        render::GalacticTypeTable types;
        types.build(w->types);
        render::GalacticModels models(g.renderer, streamer.get(), cache.get(), w->files.get());
        render::PlanetPortraits portraits(g.renderer, g.alloc, g.ring, models, types);
        portraits.set_lighting(render::galactic_lighting(render::load_galactic_light_settings(*w->files), 1.0f));
        const std::string name = render::planet_portrait_name("Kuat");
        CHECK(render::is_planet_portrait_name(name) && render::planet_portrait_type(name) == "Kuat" &&
              !render::is_planet_portrait_name("planet_portrait:") && !render::is_planet_portrait_name("load_overlay.tga"));
        CHECK(portraits.texture("planet_portrait:No_Such_Planet_Type") == nullptr && portraits.failed("planet_portrait:NO_SUCH_PLANET_TYPE"));
        // Kuat (ring of stations) and Kashyyyk (the battle load screen's planet in the smoke run; its model carries an
        // atmosphere glow) are checked and written; the others are logged.
        struct Want {
            const char* type;
            const char* png;  // nullptr: logged only
        };
        const Want wants[] = {{"Kuat", "kuat"}, {"Kashyyyk", "kashyyyk"}, {"Coruscant", nullptr}, {"Hoth", nullptr}, {"Alderaan", nullptr}};
        std::shared_ptr<const assets::TextureAsset> texs[std::size(wants)];
        uint32_t frames = 0;
        const auto all_done = [&] {
            for (size_t i = 0; i < std::size(wants); ++i) {
                if (!texs[i]) texs[i] = portraits.texture(render::planet_portrait_name(wants[i].type));
                if (!texs[i] && !portraits.failed(render::planet_portrait_name(wants[i].type))) return false;
            }
            return true;
        };
        for (; frames < 3000; ++frames) {
            rhi::Frame* f = g.ring.begin_frame();
            if (!f) break;
            VkCommandBuffer cmd = f->cmd();
            const rhi::AcquireResult acq = g.up.record_acquires(cmd);
            models.update();
            const bool done = all_done();
            portraits.record(*f, cmd);
            VkSemaphoreSubmitInfo wait = rhi::semaphore_info(g.up.timeline(), acq.wait_value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
            rhi::FrameSubmit fs;
            if (acq.wait_value) fs.waits = {&wait, 1};
            if (!g.ring.end_frame(*f, fs) || done) break;
        }
        g.ring.wait_idle();
        const uint32_t n = render::kPlanetPortraitSize;
        for (size_t i = 0; i < std::size(wants); ++i) {
            const std::string pname = render::planet_portrait_name(wants[i].type);
            const auto& tex = texs[i];
            if (wants[i].png) CHECK(tex != nullptr && !portraits.failed(pname));
            if (!tex) {
                std::printf("planet portrait: %s %s\n", pname.c_str(), portraits.failed(pname) ? "failed" : "not ready");
                continue;
            }
            CHECK(tex->width == n && tex->height == n && tex->format == assets::TextureFormat::RGBA8 && tex->levels.size() == 1 &&
                  tex->data.size() == size_t{n} * n * 4);
            CHECK(portraits.texture(pname) == tex);  // cached
            uint32_t centre_alpha = 0, centre_opaque = 0, centre_lit = 0, corner_alpha = 0, any_alpha = 0, dark_alpha = 0;
            for (uint32_t y = n / 2 - 32; y < n / 2 + 32; ++y)
                for (uint32_t x = n / 2 - 32; x < n / 2 + 32; ++x) {
                    const uint8_t* p = &tex->data[(size_t{y} * n + x) * 4];
                    centre_alpha += p[3] > 0 ? 1u : 0u;
                    centre_opaque += p[3] == 255 ? 1u : 0u;
                    centre_lit += (p[0] > 10 || p[1] > 10 || p[2] > 10) ? 1u : 0u;
                }
            // the four 8 x 8 corners; the highest alpha on the 1-pixel frame border (an additive glow quad that wrote
            // its texture alpha showed as an opaque black square reaching the border)
            uint32_t border_alpha = 0;
            for (uint32_t y = 0; y < 8; ++y)
                for (uint32_t x = 0; x < 8; ++x)
                    for (const uint32_t yy : {y, n - 1 - y})
                        for (const uint32_t xx : {x, n - 1 - x}) corner_alpha += tex->data[(size_t{yy} * n + xx) * 4 + 3] > 0 ? 1u : 0u;
            for (uint32_t t = 0; t < n; ++t)
                for (const size_t at : {size_t{t}, size_t{n - 1} * n + t, size_t{t} * n, size_t{t} * n + n - 1})
                    border_alpha = std::max<uint32_t>(border_alpha, tex->data[at * 4 + 3]);
            // alpha but (almost) no colour: drawn as a dark veil over the dialog (premultiplied brightness < 8 of 255
            // at alpha > 64: the opaque night side is excluded by alpha < 255)
            for (size_t k = 0; k < tex->data.size(); k += 4) {
                const uint8_t* p = &tex->data[k];
                any_alpha += p[3] > 0 ? 1u : 0u;
                dark_alpha += (p[3] > 64 && p[3] < 255 && std::max({p[0], p[1], p[2]}) * p[3] < 8 * 255) ? 1u : 0u;
            }
            const uint8_t* c = &tex->data[(size_t{n / 2} * n + n / 2) * 4];
            std::printf("planet portrait: %s after %u frames, centre 64x64: %u with alpha, %u opaque, %u lit; corner alpha %u, border max "
                        "alpha %u; %u px with alpha, %u dark veil; centre rgba %u %u %u %u\n",
                        pname.c_str(), frames, centre_alpha, centre_opaque, centre_lit, corner_alpha, border_alpha, any_alpha, dark_alpha,
                        c[0], c[1], c[2], c[3]);
            if (wants[i].png) {
                CHECK(centre_alpha == 64u * 64u && centre_opaque > 3000 && centre_lit > 3000 && corner_alpha == 0 && border_alpha < 32 &&
                      dark_alpha == 0);
                const std::string png = src_dir + "/logs/render_planet_portrait_" + wants[i].png + ".png";
                CHECK(render::write_png_rgba(png, n, n, tex->data));
                std::printf("wrote %s\n", png.c_str());
            }
        }
        portraits.destroy();
        models.destroy();
        streamer.reset();
    }
    g.ring.wait_idle();
    g.renderer.shutdown();
    g.ring.shutdown();
    g.up.shutdown();
    g.alloc.shutdown();
    const uint32_t verrs = g.ctx.validation_errors(), vwarns = g.ctx.validation_warnings();
    std::printf("planet portrait: validation %u errors, %u warnings\n", verrs, vwarns);
    CHECK(verrs == 0 && vwarns == 0);
    g.ctx.shutdown();
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    log::set_min_level(log::Level::Error);
    const std::string src_dir = ALAMO_RENDER_SOURCE_DIR;
    test_primitives();
    test_r2_records();
    test_interp();
    std::string err;
    const fs::path dir = fs::temp_directory_path() / "alamo_render_galactic_tests";
    std::error_code ec;
    remove_all_long(dir, ec);
    const auto world = render::make_synthetic_galactic_world(path_to_utf8(dir), &err);
    CHECK(world != nullptr);
    if (world) {
        test_scene_rules(*world);
        test_corruption_fraction(*world);
        test_fleets_and_selection(*world);
        test_synthetic(*world);
        test_thread_determinism(*world);
        test_gpu(*world, src_dir);
    } else {
        std::printf("synthetic galaxy: %s\n", err.c_str());
    }
    test_real_data();
    test_planet_portrait(src_dir);
    std::printf(g_fail == 0 ? "alamo_render_galactic_tests: all passed\n" : "alamo_render_galactic_tests: %d failure(s)\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
