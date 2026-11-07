#include "alamo/render/map_scene.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <unordered_map>

#ifdef ALAMO_RENDER_HAVE_ANIM
#include "alamo/anim/anim_state.hpp"  // R3: original loop / one-shot frame rules for background idles
#endif
#include "alamo/assets/asset_cache.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/core/log.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/data/values.hpp"

namespace alamo::render {

namespace {
using clock = std::chrono::steady_clock;
double ms_since(clock::time_point t) { return std::chrono::duration<double, std::milli>(clock::now() - t).count(); }

// Variant chain of a record (itself first, then Variant_Of_Existing_Type bases; at most 8 deep).
std::vector<const data::GameObjectRecord*> variant_chain(const data::GameDatabase& db, const data::GameObjectRecord* rec) {
    std::vector<const data::GameObjectRecord*> chain;
    for (int depth = 0; rec && depth < 8; ++depth) {
        if (std::find(chain.begin(), chain.end(), rec) != chain.end()) break;  // cycle
        chain.push_back(rec);
        const std::string_view base = rec->element.child_text("Variant_Of_Existing_Type");
        if (base.empty()) break;
        rec = db.find_game_object(base);
    }
    return chain;
}

// Model: the map mode's field first, searched along the whole variant chain (an explicit field on the
// variant wins over its base), then the other mode's field, then Galactic_Model_Name, likewise.
// Scale_Factor: the first record of the chain that writes it (an explicit 1.0 on a variant wins).
std::string model_of_record(const data::GameDatabase& db, const data::GameObjectRecord* rec, bool space, float& scale) {
    const std::vector<const data::GameObjectRecord*> chain = variant_chain(db, rec);
    scale = 1.0f;
    for (const data::GameObjectRecord* r : chain) {
        const std::string_view sf = r->element.child_text("Scale_Factor");
        if (sf.empty()) continue;
        scale = std::strtof(std::string(sf).c_str(), nullptr);
        if (!(scale > 0.0f) || !std::isfinite(scale)) scale = 1.0f;
        break;
    }
    static const char* const kLand[] = {"Land_Model_Name", "Space_Model_Name", "Galactic_Model_Name"};
    static const char* const kSpace[] = {"Space_Model_Name", "Land_Model_Name", "Galactic_Model_Name"};
    const char* const* order = space ? kSpace : kLand;
    for (int i = 0; i < 3; ++i)
        for (const data::GameObjectRecord* r : chain) {
            const std::string_view v = r->element.child_text(order[i]);
            if (!v.empty()) return std::string(v);
        }
    return {};
}}  // namespace

GameTypeResolver::GameTypeResolver(const data::GameDatabase& db) : db_(db) {
    for (const data::GameObjectRecord& r : db.game_objects())
        if (!r.duplicate) by_crc_.emplace(r.name_crc, &r);
}

bool GameTypeResolver::resolve(uint32_t crc, bool space, terrain::ResolvedType& out) const {
    const auto it = by_crc_.find(crc);
    if (it == by_crc_.end()) return false;
    out.type_name = std::string(it->second->name);
    out.scale_factor = 1.0f;
    out.model_name = model_of_record(db_, it->second, space, out.scale_factor);
    return true;
}

std::string GameTypeResolver::model_of(std::string_view type_name, bool space) const {
    terrain::ResolvedType t;
    return resolve(alamo::name_crc(type_name), space, t) ? t.model_name : std::string();
}

namespace {
const char* const kMpColorTags[kMpPaletteSize] = {"MP_Color_Blue",   "MP_Color_Red",    "MP_Color_Green",
                                                  "MP_Color_Orange", "MP_Color_Cyan",   "MP_Color_Purple",
                                                  "MP_Color_Yellow", "MP_Color_Gray",   "MP_Color_Eight"};
const uint8_t kMpColorBytes[kMpPaletteSize][3] = {{78, 150, 237},  {237, 78, 78},   {119, 237, 78},
                                                  {237, 149, 78},  {111, 217, 224}, {224, 111, 209},
                                                  {224, 215, 111}, {109, 75, 5},    {65, 129, 42}};

// "r, g, b[, a]" bytes -> (r,g,b)/255 with alpha 1. False unless 3+ numbers.
bool parse_rgb(std::string_view s, materials::float4& out) {
    float v[4] = {0, 0, 0, 0};
    int n = 0;
    std::string tmp(s);
    const char* p = tmp.c_str();
    while (*p && n < 4) {
        char* end = nullptr;
        const float f = std::strtof(p, &end);
        if (end == p) {
            ++p;
            continue;
        }
        v[n++] = f;
        p = end;
    }
    if (n < 3) return false;
    out = {v[0] / 255.0f, v[1] / 255.0f, v[2] / 255.0f, 1.0f};
    return true;
}

// Comma / whitespace separated names.
std::vector<std::string> split_names(std::string_view s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ',' || std::isspace(static_cast<unsigned char>(s[i])))) ++i;
        size_t j = i;
        while (j < s.size() && s[j] != ',' && !std::isspace(static_cast<unsigned char>(s[j]))) ++j;
        if (j > i) out.emplace_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

bool load_xml(const FileProvider& files, std::string_view path, data::XmlDocument& doc) {
    std::vector<uint8_t> bytes;
    if (!files.read(path, bytes)) return false;
    doc = data::XmlDocument::parse(bytes, std::string(path));
    return doc.ok();
}
}  // namespace

MpPalette default_mp_palette() {
    MpPalette p;
    for (uint32_t i = 0; i < kMpPaletteSize; ++i)
        p.color[i] = {kMpColorBytes[i][0] / 255.0f, kMpColorBytes[i][1] / 255.0f, kMpColorBytes[i][2] / 255.0f, 1.0f};
    return p;
}

materials::float4 owner_team_color(uint32_t owner, const MpPalette& palette) { return palette.color[owner % kMpPaletteSize]; }

MpPalette GameTypeResolver::palette() const {
    MpPalette p = default_mp_palette();
    for (uint32_t i = 0; i < kMpPaletteSize; ++i)
        if (const auto v = db_.constants().get(kMpColorTags[i])) parse_rgb(*v, p.color[i]);
    return p;
}

bool GameTypeResolver::load_render_xml(const FileProvider& files) {
    hardpoint_defs_.clear();
    hardpoints_loaded_ = false;
    data::XmlDocument list;
    if (load_xml(files, "Data\\XML\\HardPointDataFiles.xml", list)) {
        hardpoints_loaded_ = true;
        for (data::XmlNode f : list.root().children("File")) {
            data::XmlDocument doc;
            if (!load_xml(files, "Data\\XML\\" + std::string(f.text_trimmed()), doc)) {
                log::warnf("render", "hardpoint file %s missing", std::string(f.text_trimmed()).c_str());
                continue;
            }
            for (data::XmlNode hp : doc.root().children()) {
                const std::string_view name = hp.attribute("Name");
                if (name.empty()) continue;
                if (hardpoint_defs_.find(ascii_upper(name)) == hardpoint_defs_.end())  // first definition wins
                    hardpoint_defs_.emplace(ascii_upper(name), parse_hardpoint(hp));
            }
        }
    }
    factions_.clear();
    data::XmlDocument flist;
    if (load_xml(files, "Data\\XML\\FactionFiles.xml", flist)) {
        for (data::XmlNode f : flist.root().children("File")) {
            data::XmlDocument doc;
            if (!load_xml(files, "Data\\XML\\" + std::string(f.text_trimmed()), doc)) continue;
            for (data::XmlNode fa : doc.root().children("Faction")) {
                Faction x;
                x.name = std::string(fa.attribute("Name"));
                const std::string_view n = fa.child_text("Is_Neutral");
                x.neutral = data::parse_flag_char(n);
                parse_rgb(fa.child_text("No_Colorization_Color"), x.no_colorization);
                factions_.push_back(std::move(x));
            }
        }
    }
    return hardpoints_loaded_;
}

materials::float4 GameTypeResolver::owner_color(uint32_t owner, const TypeRenderInfo& t, const MpPalette& palette) const {
    if (!owner_neutral(owner)) return owner_team_color(owner, palette);
    return t.has_no_colorization ? t.no_colorization : factions_[owner].no_colorization;
}

TypeRenderInfo GameTypeResolver::render_info(uint32_t crc) const {
    TypeRenderInfo t;
    t.hardpoints_known = hardpoints_loaded_;
    const auto it = by_crc_.find(crc);
    if (it == by_crc_.end()) return t;
    const std::vector<const data::GameObjectRecord*> chain = variant_chain(db_, it->second);
    bool lod = false, hps = false, ncc = false, bg = false, loop = false, rate = false;
    for (const data::GameObjectRecord* r : chain) {  // first record of the chain that writes a field wins
        if (!loop) {
            const std::string_view v = r->element.child_text("Loop_Idle_Anim_00");
            size_t k = 0;
            while (k < v.size() && std::isspace(static_cast<unsigned char>(v[k]))) ++k;
            if (k < v.size()) {
                t.loop_idle = data::parse_flag_char(v.substr(k));
                loop = true;
            }
        }
        if (!rate) {
            const std::string_view v = r->element.child_text("Idle_Anim_00_Rate_Mod");
            if (!v.empty()) {
                const float x = std::strtof(std::string(v).c_str(), nullptr);
                if (std::isfinite(x) && x >= 0.0f) t.idle_rate = x;
                rate = true;
            }
        }
        if (!lod) {
            const std::string_view v = r->element.child_text("LOD_Bias");
            if (!v.empty()) {
                const float b = std::strtof(std::string(v).c_str(), nullptr);
                if (std::isfinite(b) && b >= 0.0f) t.lod_bias = b;
                lod = true;
            }
        }
        if (!bg) {
            const std::string_view v = r->element.child_text("In_Background");
            size_t k = 0;
            while (k < v.size() && std::isspace(static_cast<unsigned char>(v[k]))) ++k;
            if (k < v.size()) {
                t.in_background = data::parse_flag_char(v.substr(k));
                bg = true;
            }
        }
        if (!hps && r->element.child("HardPoints")) {
            for (data::XmlNode n : r->element.children("HardPoints"))  // repeated tags append
                for (std::string& h : split_names(n.text_trimmed())) t.hardpoints.push_back(std::move(h));
            hps = true;
        }
        if (!ncc) {
            const std::string_view v = r->element.child_text("No_Colorization_Color");
            if (!v.empty()) {
                materials::float4 c;
                if (parse_rgb(v, c) && (c.x != 0.0f || c.y != 0.0f || c.z != 0.0f)) {
                    t.has_no_colorization = true;
                    t.no_colorization = c;
                }
                ncc = true;
            }
        }
    }
    for (const std::string& h : t.hardpoints) {
        const HardpointDef* d = hardpoint(h);
        HardpointDef def;
        if (d) def = *d;
        else def.name = h;
        t.hardpoint_decal.push_back(def.damage_decal);
        if (!def.damage_decal.empty()) t.damage_decals.push_back(def.damage_decal);
        t.hardpoint_defs.push_back(std::move(def));
    }
    return t;
}

const HardpointDef* GameTypeResolver::hardpoint(std::string_view name) const {
    const auto it = hardpoint_defs_.find(ascii_upper(name));
    return it == hardpoint_defs_.end() ? nullptr : &it->second;
}

int32_t TypeRenderInfo::hardpoint_index(std::string_view name) const {
    for (size_t i = 0; i < hardpoints.size(); ++i) {
        if (ascii_iequals(hardpoints[i], name)) return static_cast<int32_t>(i);
    }
    return -1;
}

void MapScene::clear_loaded() {
    map_ = terrain::MapData{};
    mesh_ = terrain::TerrainMesh{};
    water_ = terrain::WaterSurface{};
    deco_ = WorldDecorations{};
    placements_ = terrain::PlacementResult{};
    fow_ = terrain::FowGrid{};
    track_fx_.clear();
    models_.clear();
    placement_model_.clear();
    type_info_.clear();
    placement_color_.clear();
    placement_vis_.clear();
    layout_cache_.clear();
    attach_models_.clear();
    placement_attach_.clear();
    placement_bg_.clear();
    placement_anim_.clear();
    files_ = nullptr;
    space_ = false;
    masks_dirty_ = false;
}

bool MapScene::load(const FileProvider& files, std::string_view map, const GameTypeResolver* resolver, const ParallelFor& parallel,
                    std::string* error) {
    // Load boundary: exceptions from the map parser / terrain builders (std::bad_alloc on a hostile map,
    // a throwing ParallelFor) never escape; the scene is left empty on any failure.
    std::string why;
    try {
        if (load_impl(files, map, resolver, parallel, &why)) return true;
    } catch (const std::exception& e) {
        why = "map " + std::string(map) + ": exception: " + e.what();
    } catch (...) {
        why = "map " + std::string(map) + ": unknown exception";
    }
    try {
        clear_loaded();
        log::warnf("render", "MapScene::load failed: %s", why.c_str());
        if (error) *error = std::move(why);
    } catch (...) {  // out of memory even for the message: still report failure
    }
    return false;
}

bool MapScene::load_impl(const FileProvider& files, std::string_view map, const GameTypeResolver* resolver, const ParallelFor& parallel,
                         std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    stats_ = {};
    resolver_ = resolver;
    auto t = clock::now();
    std::string err;
    if (!terrain::load_map(files, map, map_, &err)) return fail("map " + std::string(map) + ": " + err);
    stats_.parse_ms = ms_since(t);
    space_ = map_.is_space();
    mesh_ = {};
    water_ = {};
    deco_ = {};
    started_ = false;
    if (!space_) {
        t = clock::now();
        if (!terrain::build_terrain_mesh(map_.terrain, mesh_, {}, parallel, &err)) return fail("terrain mesh: " + err);
        stats_.mesh_ms = ms_since(t);
        stats_.mesh = mesh_.stats;
        t = clock::now();
        if (!terrain::build_water(map_.terrain, water_, {}, parallel, &err)) return fail("water: " + err);
        stats_.water_ms = ms_since(t);
        stats_.water_patches = static_cast<uint32_t>(water_.patches.size());
        stats_.water_triangles = water_.triangles;
        // Decorations (DECORATIONS.md 4, 5.1): map track ribbons (per-map budgets) and decal geometry.
        t = clock::now();
        stats_.tracks = static_cast<uint32_t>(map_.terrain.tracks.size());
        stats_.tracks_built = static_cast<uint32_t>(terrain::build_map_track_geometry(map_.terrain, deco_.tracks));
        for (const terrain::TrackGeometry& g : deco_.tracks) {
            stats_.water_tracks += (!g.mesh.empty() && g.type == terrain::DecorationType::WaterTrack) ? 1u : 0u;
            stats_.decoration_triangles += g.mesh.triangle_count();
        }
        stats_.decals = static_cast<uint32_t>(map_.terrain.decals.size());
        {   // per-map budget (out[i] <-> decals[i], skipped / oversized decals come back empty)
            std::vector<terrain::DecalGeometry> built;
            terrain::build_map_decal_geometry(map_.terrain, built);
            for (size_t i = 0; i < built.size() && i < map_.terrain.decals.size(); ++i) {
                if (built[i].mesh.empty()) continue;
                stats_.decoration_triangles += built[i].mesh.triangle_count();
                deco_.decals.push_back(std::move(built[i]));
                deco_.decal_records.push_back(map_.terrain.decals[i]);
            }
        }
        stats_.decals_built = static_cast<uint32_t>(deco_.decals.size());
        stats_.decoration_ms = ms_since(t);
    }
    // Fog-of-war grid over the playable box (fogged until the caller reveals cells).
    fow_ = terrain::FowGrid(terrain::make_fow_grid(map_));
    stats_.fow_width = fow_.width();
    stats_.fow_height = fow_.height();
    // DynamicTrackFX.xml (per-unit trails) and ShadowBlobMaterials.xml: textures loaded with the world.
    track_fx_.clear();
    deco_.blob_materials = terrain::builtin_blob_materials();
    {
        std::vector<uint8_t> xml;
        if (files.read("Data\\XML\\DynamicTrackFX.xml", xml)) terrain::parse_dynamic_track_fx(xml, track_fx_);
        xml.clear();
        if (files.read("Data\\XML\\ShadowBlobMaterials.xml", xml)) terrain::parse_shadow_blob_materials(xml, deco_.blob_materials);
        if (!space_)
            for (const terrain::DynamicTrackParams& p : track_fx_)
                if (!p.texture.empty() && std::find(deco_.extra_textures.begin(), deco_.extra_textures.end(), p.texture) == deco_.extra_textures.end())
                    deco_.extra_textures.push_back(p.texture);
    }
    t = clock::now();
    stats_.objects = static_cast<uint32_t>(map_.objects.size());
    if (resolver) placements_ = terrain::resolve_placements(map_, resolver->function());
    files_ = &files;
    // Diagnostics only: 0x4B0 mini 03 is the starting health fraction (not a scale; the renderer ignores it).
    for (const terrain::ResolvedPlacement& p : placements_.placements)
        if (p.initial_health_fraction != 1.0f) ++stats_.health_not_full;
    stats_.resolve_ms = ms_since(t);
    stats_.placements = static_cast<uint32_t>(placements_.placements.size());
    stats_.unknown_type = static_cast<uint32_t>(placements_.unknown_type.size());
    stats_.no_model = static_cast<uint32_t>(placements_.no_model.size());
    // Distinct models (first-appearance order).
    models_.clear();
    placement_model_.clear();
    std::unordered_map<std::string, uint32_t> index;
    for (const terrain::ResolvedPlacement& p : placements_.placements) {
        const std::string key = ascii_upper(p.model_name);
        auto [it, inserted] = index.try_emplace(key, static_cast<uint32_t>(models_.size()));
        if (inserted) models_.push_back(p.model_name);
        placement_model_.push_back(it->second);
    }
    stats_.distinct_models = static_cast<uint32_t>(models_.size());
    const terrain::Environment* env = map_.environments.start();
    // Space too: the main scene keeps the environment's fill lights (SPACE_AND_OPEN.md 0 item 2); the renderer
    // zeroes them for the background scene only (RecordOptions::space).
    if (env) lighting_ = Lighting::from_environment(*env, false);
    else if (space_) {
        lighting_ = Lighting::space_default();
        lighting_.space = false;
    } else {
        lighting_ = Lighting::land_default();
    }
    // Fog (SPACE_AND_OPEN.md B.1 [H]): the active environment's fog always wins; the 0x100/2 scene-fog block
    // is overwritten on every environment apply, so it is ignored here.
    log::infof("render", "map %s: fog from the start environment (%s, %.0f..%.0f)", std::string(map).c_str(),
               lighting_.fog_enabled ? "on" : "off", lighting_.fog_start, lighting_.fog_end);
    // Per-type render info + per-placement team colour (B.3: owner slot -> MP palette; neutral owners ->
    // No_Colorization_Color).
    type_info_.clear();
    placement_color_.clear();
    placement_vis_.assign(placements_.placements.size(), {});
    layout_cache_.clear();
    attach_models_.clear();
    placement_attach_.assign(placements_.placements.size(), {});
    std::unordered_map<std::string, uint32_t> attach_index;  // upper-cased model name -> attach_models_ index
    const MpPalette pal = resolver ? resolver->palette() : default_mp_palette();
    placement_bg_.clear();
    stats_.background_placements = 0;
    for (size_t pi = 0; pi < placements_.placements.size(); ++pi) {
        const terrain::ResolvedPlacement& p = placements_.placements[pi];
        auto [it, inserted] = type_info_.try_emplace(p.type_crc);
        if (inserted && resolver) it->second = resolver->render_info(p.type_crc);
        const TypeRenderInfo& ti = it->second;
        placement_vis_[pi].reset(ti.hardpoints.size());
        if (ti.hardpoints_known) {  // Model_To_Attach per hardpoint, HardPoints order
            std::vector<uint32_t>& pa = placement_attach_[pi];
            pa.assign(ti.hardpoint_defs.size(), UINT32_MAX);
            for (size_t h = 0; h < ti.hardpoint_defs.size(); ++h) {
                const std::string& m = ti.hardpoint_defs[h].model_to_attach;
                if (m.empty()) continue;
                auto [ai, ins] = attach_index.try_emplace(ascii_upper(m), static_cast<uint32_t>(attach_models_.size()));
                if (ins) attach_models_.push_back(m);
                pa[h] = ai->second;
            }
        }
        placement_color_.push_back(resolver ? resolver->owner_color(p.owner, ti, pal) : owner_team_color(p.owner, pal));
        const bool bg = space_ && ti.in_background;  // land has no background scene (A.3)
        placement_bg_.push_back(bg ? 1 : 0);
        stats_.background_placements += bg ? 1u : 0u;
    }
    masks_dirty_ = true;
    return true;
}

std::vector<MapScene::OwnerCount> MapScene::owner_histogram() const {
    std::vector<OwnerCount> h;
    for (const terrain::ResolvedPlacement& p : placements_.placements) {
        auto it = std::lower_bound(h.begin(), h.end(), p.owner, [](const OwnerCount& c, uint32_t o) { return c.owner < o; });
        if (it == h.end() || it->owner != p.owner) it = h.insert(it, OwnerCount{p.owner, 0, 0});
        ++it->objects;
        it->neutral += (resolver_ && resolver_->owner_neutral(p.owner)) ? 1u : 0u;
    }
    return h;
}

bool MapScene::set_hardpoint_destroyed(size_t placement, size_t hardpoint, bool destroyed) {
    if (placement >= placement_vis_.size()) return false;
    if (!placement_vis_[placement].set_destroyed(hardpoint, destroyed)) return false;  // mask updated in place
    masks_dirty_ = true;  // attachment instances follow their sub-object bit on the next update()
    return true;
}

bool MapScene::set_hardpoint_destroyed(size_t placement, std::string_view hardpoint, bool destroyed) {
    if (placement >= placement_vis_.size() || placement >= placements_.placements.size()) return false;
    const TypeRenderInfo* ti = type_info(placements_.placements[placement].type_crc);
    const int32_t h = ti ? ti->hardpoint_index(hardpoint) : -1;
    return h >= 0 && set_hardpoint_destroyed(placement, static_cast<size_t>(h), destroyed);
}

bool MapScene::start(Renderer& r, ModelStreamer& streamer, assets::AssetCache& cache, const WorldOptions& options,
                     std::string* error) {
    r.set_lighting(lighting_);
    if (space_) r.set_object_fade(kSpaceObjectFadeBegin, kSpaceObjectFadeEnd);
    else r.set_object_fade(kLandObjectFadeBegin, kLandObjectFadeEnd);
    terrain::CloudParams clouds = terrain::effective_clouds(map_, map_.environments.start());
    if (space_) clouds.texture.clear();  // no terrain: no cloud shadows
    const TextureSource tex = [&cache](std::string_view n, assets::TextureRole role) { return cache.texture(n, role); };
    WorldOptions wo = options;
    wo.space = space_;
    deco_.runtime_decals = runtime_decals_ && !space_;  // R5
    if (!world_.init(r, mesh_, space_ ? nullptr : &water_, clouds, tex, wo, error, &deco_)) return false;
    const terrain::FowGridDesc& fd = fow_.desc();
    if (fd.width > 0 && fd.height > 0) world_.set_fow_box(fd.x0, fd.y0, fd.x1, fd.y1);
    r.set_world(&world_);
    started_ = true;
    model_handle_.clear();
    attach_handle_.clear();
    if (objects_enabled_) {
        for (const std::string& m : models_) model_handle_.push_back(streamer.request(m));
        for (const std::string& m : attach_models_) attach_handle_.push_back(streamer.request(m));
    }
    stats_.attach_models = static_cast<uint32_t>(attach_handle_.size());
    // Background idle clips (D.7 4): space In_Background placements.
    placement_anim_.clear();
    placement_anim_.resize(placements_.placements.size());
    stats_.background_animated = 0;
    for (size_t i = 0; i < placements_.placements.size() && objects_enabled_; ++i) {
        if (i >= placement_bg_.size() || !placement_bg_[i]) continue;
        const TypeRenderInfo* ti = type_info(placements_.placements[i].type_crc);
        load_idle_anim(cache, models_[placement_model_[i]], ti ? *ti : TypeRenderInfo{}, placement_anim_[i]);
        stats_.background_animated += placement_anim_[i].clip ? 1u : 0u;
    }
    sky_handles_.clear();
    sky_instances_.clear();
    sky_done_ = false;
    sky_anim_[0] = sky_anim_[1] = IdleAnim{};
    for (int k = 0; k < 2; ++k) {
        const std::string& type = lighting_.skydome[k];
        if (type.empty() || !resolver_ || !sky_enabled_) continue;
        terrain::ResolvedType rt;
        const bool known = resolver_->resolve(alamo::name_crc(type), space_, rt);
        const std::string model = known ? rt.model_name : std::string();
        if (model.empty()) {
            log::warnf("render", "map: skydome object %s has no model", type.c_str());
            continue;
        }
        sky_handles_.push_back({streamer.request(model), k});
        sky_scale_[k] = rt.scale_factor;  // space: the type's Scale_Factor, not TEE 1B/1C (A.3)
        if (space_) {  // background scene objects play their idle clip (D.7 4); land domes stay static
            load_idle_anim(cache, model, resolver_->render_info(alamo::name_crc(type)), sky_anim_[k]);
            stats_.background_animated += sky_anim_[k].clip ? 1u : 0u;
        }
    }
    finished_seen_ = UINT32_MAX;
    ready_ = false;
    instances_.clear();
    return true;
}

void MapScene::update(Renderer& r, ModelStreamer& streamer) {
    (void)r;
    world_.make_resident();
    // Instances of every ready model (rebuilt when the number of finished models changes).
    uint32_t finished = 0, ready = 0, failed = 0;
    for (uint32_t h : model_handle_) {
        const ModelStreamer::State s = streamer.state(h);
        ready += s == ModelStreamer::State::Ready ? 1u : 0u;
        failed += s == ModelStreamer::State::Failed ? 1u : 0u;
    }
    finished = ready + failed;
    stats_.models_ready = ready;
    stats_.models_failed = failed;
    uint32_t attach_finished = 0;
    for (uint32_t h : attach_handle_) {
        const ModelStreamer::State s = streamer.state(h);
        attach_finished += (s == ModelStreamer::State::Ready || s == ModelStreamer::State::Failed) ? 1u : 0u;
    }
    finished += attach_finished;
    if (finished != finished_seen_ || masks_dirty_) {
        finished_seen_ = finished;
        masks_dirty_ = false;
        instances_.clear();
        bg_placements_.clear();
        placement_gpu_.resize(placements_.placements.size(), nullptr);
        stats_.attachments = 0;
        for (size_t i = 0; i < placements_.placements.size() && objects_enabled_; ++i) {
            const uint32_t h = model_handle_[placement_model_[i]];
            if (streamer.state(h) != ModelStreamer::State::Ready) continue;
            const terrain::ResolvedPlacement& p = placements_.placements[i];
            Instance in;
            in.model = streamer.model(h);
            in.transform = p.transform;
            in.team_color_override = true;
            in.team_color = placement_color_[i];
            const TypeRenderInfo* ti = type_info(p.type_crc);
            if (ti) in.lod_bias = ti->lod_bias;
            // Hidden sub-objects (B.9 / B.10): ALO flag + the type's Damage_Decals + STEALTH/buildup/charged +
            // Damage_Particles subtrees, then the destroyed hardpoints' effects; without the hardpoint XML the
            // model's default (name rule). Bound once per placement when its model is ready.
            HardpointVisibility& vis = placement_vis_[i];
            if (!vis.ready() && in.model && in.model->asset) {
                const assets::ModelAsset& ma = *in.model->asset;
                const uint64_t key = (uint64_t{p.type_crc} << 32) | placement_model_[i];
                HardpointLayoutPtr& cached = layout_cache_[key];
                if (!cached) {
                    if (ti && ti->hardpoints_known)
                        cached = std::make_shared<const HardpointLayout>(build_hardpoint_layout(ma, ti->hardpoint_defs, show_damage_));
                    else
                        cached = std::make_shared<const HardpointLayout>(fallback_hardpoint_layout(
                            ma, show_damage_ ? hidden_mesh_mask(ma, nullptr) : in.model->mesh_hidden, ti ? ti->hardpoints.size() : 0));
                }
                vis.init(cached);
            }
            placement_gpu_[i] = in.model;
            in.mesh_hidden = vis.mask();
            if (i < placement_anim_.size()) in.bone_world = bind_idle_anim(placement_anim_[i], in.model);
            std::vector<Instance>& dst = (i < placement_bg_.size() && placement_bg_[i]) ? bg_placements_ : instances_;
            dst.push_back(in);
            // Model_To_Attach at Attachment_Bone (B.10), HardPoints order, after the owner: owner transform x the
            // owner's bone (bind pose: map placements are not animated). Skipped while not ready or hidden.
            const HardpointLayout* lay = vis.layout();
            const size_t n_attach = i < placement_attach_.size() ? placement_attach_[i].size() : 0;
            for (size_t hp = 0; hp < n_attach && lay && in.model && in.model->asset; ++hp) {
                const uint32_t am = placement_attach_[i][hp];
                if (am == UINT32_MAX || am >= attach_handle_.size() || !vis.attachment_visible(static_cast<uint32_t>(hp))) continue;
                const uint32_t ah = attach_handle_[am];
                if (streamer.state(ah) != ModelStreamer::State::Ready) continue;
                Instance at = in;
                at.model = streamer.model(ah);
                at.mesh_hidden = {};
                at.bone_world = {};
                at.transform = attachment_transform(*in.model->asset, in.transform, {}, lay->attach_bone[hp]);
                dst.push_back(at);
                ++stats_.attachments;
            }
        }
        stats_.instances = static_cast<uint32_t>(instances_.size());
        background_ = sky_instances_;
        background_.insert(background_.end(), bg_placements_.begin(), bg_placements_.end());
    }
    if (!sky_done_) {
        bool all = true;
        std::vector<Instance> sky;
        for (const auto& [h, k] : sky_handles_) {
            const ModelStreamer::State s = streamer.state(h);
            if (s == ModelStreamer::State::Failed) continue;
            GpuModel* m = streamer.model(h);
            if (s != ModelStreamer::State::Ready || !m || !m->resident()) {
                all = false;
                continue;
            }
            Instance si;
            si.model = m;
            if (space_) {  // at the origin, orientation from the TEE angles, the type's Scale_Factor
                Lighting l = lighting_;
                l.skydome_scale[k] = sky_scale_[k];
                si.transform = skydome_transform(l, k);
                si.bone_world = bind_idle_anim(sky_anim_[k], m);
            } else {
                si.transform = skydome_transform(lighting_, k);
            }
            sky.push_back(si);
        }
        if (all) {
            sky_instances_ = sky;
            if (!space_) world_.set_sky(sky_instances_);
            sky_done_ = true;
            background_ = sky_instances_;
            background_.insert(background_.end(), bg_placements_.begin(), bg_placements_.end());
        }
    }
    ready_ = world_.resident() && sky_done_ && finished == model_handle_.size() + attach_handle_.size();
}

void MapScene::load_idle_anim(assets::AssetCache& cache, std::string_view model, const TypeRenderInfo& ti, IdleAnim& out) {
    out = IdleAnim{};
    // "<model stem>_IDLE_00" (w_planet_kuat_high.alo -> W_PLANET_KUAT_HIGH_IDLE_00.ALA); checked for existence
    // first so a model without an idle clip does not count as a failed load.
    size_t b = model.find_last_of("\\/");
    std::string_view stem = b == std::string_view::npos ? model : model.substr(b + 1);
    const size_t dot = stem.find_last_of('.');
    if (dot != std::string_view::npos) stem = stem.substr(0, dot);
    if (stem.empty()) return;
    const std::string clip = std::string(stem) + "_IDLE_00";
    if (files_ && !files_->exists(assets::animation_path(clip))) return;
    out.clip = cache.animation(clip);
    out.rate = ti.idle_rate;
    out.loop = ti.loop_idle;
    if (out.clip && out.clip->frame_count == 0) out.clip.reset();
}

// ---- R3 (ANIM_PARTICLES.md A.5 / A.9): original frame rules for the background idle clips ----------------
namespace {

// Frame position of a clip started at frame 0 after `seconds` of play: loops wrap at count - 1 (the last stored
// frame is never reached), one-shots hold count - 2. Closed form of the per-tick advance (visual only).
float idle_frame_position(double seconds, float fps, uint32_t count, bool loop) {
    if (count < 2 || !(seconds > 0.0) || !(fps > 0.0f)) return 0.0f;
    const double pos = seconds * static_cast<double>(fps);
    if (loop) return static_cast<float>(std::fmod(pos, static_cast<double>(count - 1)));
    return static_cast<float>(std::min(pos, static_cast<double>(count - 2)));
}

#ifdef ALAMO_RENDER_HAVE_ANIM
struct R3IdleClip {  // alamo_anim clip bound to the instance's skeleton
    anim::Skeleton skeleton;
    anim::ClipRef clip;
};
#endif

}  // namespace

void MapScene::pose_idle_anim(IdleAnim& a) const {
    if (!a.clip || !a.asset || a.world.size() != a.asset->skeleton.size()) return;
    const double seconds = idle_anim_seconds(sim_tick_, a.rate);
    const float frame = idle_frame_position(seconds, a.clip->fps, a.clip->frame_count, a.loop);
#ifdef ALAMO_RENDER_HAVE_ANIM
    if (a.r3) {
        const auto& r3 = *static_cast<const R3IdleClip*>(a.r3.get());
        std::vector<anim::Mat34> world(r3.skeleton.size());
        anim::clip_world_pose(r3.skeleton, r3.clip, frame, world);
        static_assert(sizeof(anim::Mat34) == sizeof(Mat3x4), "pose layouts differ");
        std::memcpy(static_cast<void*>(a.world.data()), world.data(), world.size() * sizeof(Mat3x4));
        return;
    }
#endif
    // Without engine/anim: same frame rule, sampled by alamo_assets (non-looping, so the last interval of a
    // loop lerps toward the stored last frame instead of frame 0 - equal for clips whose last frame repeats 0).
    assets::Pose pose;
    assets::sample(a.asset->skeleton, *a.clip, a.clip->fps > 0.0f ? frame / a.clip->fps : 0.0f, false, pose);
    assets::compute_world(a.asset->skeleton, pose.local, a.world);
}

std::span<const Mat3x4> MapScene::bind_idle_anim(IdleAnim& a, const GpuModel* m) {
    if (!a.clip || !m || !m->asset || m->asset->skeleton.size() == 0) return {};
    [[maybe_unused]] const bool rebind = a.asset != m->asset.get() || !a.r3;
    a.asset = m->asset.get();
    if (a.world.size() != a.asset->skeleton.size()) a.world.resize(a.asset->skeleton.size());
#ifdef ALAMO_RENDER_HAVE_ANIM
    if (rebind) a.r3.reset();
    if (rebind && files_) {
        std::vector<uint8_t> bytes;
        auto r3 = std::make_shared<R3IdleClip>();
        auto clip = std::make_shared<anim::Clip>();
        if (files_->read(a.clip->name, bytes) && anim::parse_clip(bytes, a.clip->name, *clip)) {
            const assets::Skeleton& s = a.asset->skeleton;
            std::vector<anim::Mat34> bind(s.size());
            std::memcpy(static_cast<void*>(bind.data()), static_cast<const void*>(s.bind_local.data()), s.size() * sizeof(Mat3x4));
            r3->skeleton = anim::skeleton_from_arrays(s.names, s.parents, s.billboard_mode, bind);
            anim::AnimModel binder;  // binding rule: model bones == ALA bones + 1
            binder.skeleton = r3->skeleton;
            binder.has_skeleton = true;
            binder.clips[0].push_back(anim::ClipRef{clip, false, {}});
            anim::bind_clips(binder);
            r3->clip = binder.clips[0][0];
            a.r3 = std::move(r3);
        }
    }
#endif
    pose_idle_anim(a);
    return a.world;
}

void MapScene::set_sim_tick(uint64_t tick) {
    if (tick == sim_tick_) return;
    sim_tick_ = tick;
    for (IdleAnim& a : placement_anim_) pose_idle_anim(a);  // in place: the instances' bone_world spans stay valid
    for (IdleAnim& a : sky_anim_) pose_idle_anim(a);
}

bool MapScene::apply_fog_of_war() {
    if (!started_ || fow_.width() == 0 || fow_.height() == 0) return false;
    const std::array<uint32_t, 256> pal = space_ ? terrain::fow_palette(terrain::kSpaceFowVisible, terrain::kSpaceFowColor)
                                                 : terrain::fow_palette(terrain::kLandFowVisible, terrain::kLandFowColor);
    fow_.build_texels(pal);
    return world_.set_fog_of_war(fow_);
}

void MapScene::reveal_circle(float x, float y, float radius) {
    std::vector<uint8_t>& cells = fow_.intensity();
    const terrain::FowGridDesc& d = fow_.desc();
    cells.assign(static_cast<size_t>(d.width) * d.height, 0);
    const float edge = 2.0f * std::max(d.cell_x, d.cell_y);
    for (uint32_t cy = 0; cy < d.height; ++cy)
        for (uint32_t cx = 0; cx < d.width; ++cx) {
            const std::array<float, 2> c = d.cell_center(cx, cy);
            const float dist = std::sqrt((c[0] - x) * (c[0] - x) + (c[1] - y) * (c[1] - y));
            const float t = std::clamp((radius - dist) / (edge > 0.0f ? edge : 1.0f), 0.0f, 1.0f);
            cells[static_cast<size_t>(cy) * d.width + cx] = static_cast<uint8_t>(std::lround(t * 255.0f));
        }
}

void MapScene::set_dynamic_tracks(std::span<const terrain::DynamicTrackGenerator> tracks) {
    std::vector<DynamicTrackMesh> out;
    for (const terrain::DynamicTrackGenerator& g : tracks) {
        DynamicTrackMesh m;
        if (!g.build(map_.terrain, m.mesh)) continue;
        m.texture = g.params().texture;
        m.mode = g.params().mode;
        out.push_back(std::move(m));
    }
    world_.set_dynamic_tracks(std::move(out));
}

void MapScene::set_shadow_blobs(std::span<const terrain::ShadowBlob> blobs) {
    std::vector<BlobMesh> out;
    for (const terrain::ShadowBlob& b : blobs) {
        BlobMesh m;
        m.material = b.material < deco_.blob_materials.size() ? b.material : 0u;
        const terrain::RenderMode mode = deco_.blob_materials.empty() ? terrain::RenderMode::Modulate : deco_.blob_materials[m.material].mode;
        if (!terrain::build_blob_geometry(map_.terrain, b, mode, m.geometry)) continue;
        out.push_back(std::move(m));
    }
    world_.set_shadow_blobs(std::move(out));
}

void MapScene::stop(Renderer& r) {
    started_ = false;
    if (r.world() == &world_) r.set_world(nullptr);
    r.set_object_fade(-1.0f, -1.0f);
    world_.shutdown();
}

OrbitCamera MapScene::tactical_camera(float aspect, float distance_override) const {
    OrbitCamera cam;
    if (space_) {
        // Space_Mode constants overwrite the 0x103 pitch / yaw / distance / FOV after load (A.1, A.2); the look-at
        // (03) is kept. Yaw 0 -> the land convention (camera south of the target, looking north) [choice].
        const terrain::SpaceMapDesc& sd = map_.space;
        cam.target = {sd.camera_look_at[0], sd.camera_look_at[1], sd.camera_look_at[2]};
        cam.yaw = -1.5707963f;
        cam.pitch = kSpacePitch * 0.017453293f;
        cam.distance = distance_override > 0.0f ? distance_override : kSpaceDistance;
        cam.fov_x = tactical_fov_x(kSpaceFov, aspect);
        cam.radius = cam.distance;
        return cam;
    }
    float cx = 0.5f * static_cast<float>(map_.terrain.width > 0 ? map_.terrain.width - 1 : 0) * terrain::kVertexSpacing;
    float cy = 0.5f * static_cast<float>(map_.terrain.height > 0 ? map_.terrain.height - 1 : 0) * terrain::kVertexSpacing;
    if (map_.playable.has_box) {
        cx = 0.5f * (map_.playable.box_min[0] + map_.playable.box_max[0]);
        cy = 0.5f * (map_.playable.box_min[1] + map_.playable.box_max[1]);
    }
    cam.target = {cx, cy, map_.terrain.sample_height(cx, cy)};
    cam.yaw = -1.5707963f;  // camera south of the target, looking north (+Y)
    // TacticalCameras.xml Land_Mode defaults (RENDER.md 3.4): pitch 50, distance 460, FOV 55. The map's 0x10C
    // block stores 12 = 50, 14 = 460, 15 = 55, read here as pitch / distance / FOV [inferred]; 03 (1250..2300)
    // is not the start distance.
    const terrain::CameraBlock& tc = map_.tactical_camera;
    float pitch = tc.present ? tc.f32_or(0x12, kLandPitch) : kLandPitch;
    if (!(pitch >= 10.0f && pitch <= 89.0f)) pitch = kLandPitch;
    float dist = tc.present ? tc.f32_or(0x14, kLandDistance) : kLandDistance;
    if (!(dist >= 100.0f && dist <= 2000.0f)) dist = kLandDistance;
    if (distance_override > 0.0f) dist = distance_override;
    float fov = tc.present ? tc.f32_or(0x15, kLandFov) : kLandFov;
    if (!(fov >= 20.0f && fov <= 90.0f)) fov = kLandFov;
    cam.pitch = pitch * 0.017453293f;
    cam.distance = dist;
    cam.fov_x = tactical_fov_x(fov, aspect);
    cam.radius = dist;
    return cam;
}
}  // namespace alamo::render
