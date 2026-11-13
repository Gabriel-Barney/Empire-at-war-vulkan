// alamo_render - Phase 3 renderer test app.
//   alamo_render --clear [--frames N]
//       C1: Win32 window + Vulkan 1.3 device (validation on when toolchain\vvl\bin exists), cleared
//       to a cycling colour every frame; resize and minimise handled. Esc quits.
//   alamo_render --game <corruption dir> --model <NAME.ALO> [--anim <NAME.ALA>] [MODPATH=..] [FALLBACKPATH=..]
//       C3b: the model loaded through AssetCache on job-system workers, uploaded in the background
//       and drawn LIT through its materials effects (hot-set / FF pipelines created on the job
//       system, disk pipeline cache). --unlit draws the C2b unlit path instead. Left-drag orbits,
//       wheel zooms. --anim plays an animation on the 30 Hz tick. --show-damage shows hardpoint
//       damage decals and STEALTH/buildup/charged meshes (hidden by default; maps use each type's
//       hardpoint Damage_Decal names, a lone model the *_BLAST* name rule). --team-color R,G,B (0..1).
//       --sun-yaw / --sun-elev (degrees, space-environment convention).
//   alamo_render --game <dir> --fleet N [--seed S]
//       C5: N instances of capital ships + fighters from the install (render/fleet.hpp), per-instance
//       team colour; prints CPU build / record / submit and GPU time per frame.
//   alamo_render --game <dir> --map <NAME> [--fow] [--dynamic-track-demo] (e.g. _MP_LAND_ENDOR, _MP_SPACE_KUAT)
//       Land: terrain + decorations (map tracks, decals; --dynamic-track-demo drives a unit trail + shadow blob around
//       the start target), --fow a fog of war revealed in a disc around it. Space: background scene, Space_Mode camera.
//       A land map: terrain (bucket 0), water + reflection / refraction / sky cube (bucket 7), every object
//       placement's model with its owner's team colour, lighting / fog / bloom / sky from the start
//       environment, the tactical start camera (left-drag orbits, wheel zooms).
//   alamo_render --game <dir> --sweep [--cold]
//       No window: every ALO in the install -> effects -> distinct PsoKeys -> pipelines; prints
//       counts and creation times (--cold deletes the pipeline cache first).
// Common: --frames N, --width W --height H, --mailbox / --immediate, --workers N,
//         --record-threads T (0/1 = single-threaded recording; default = worker count),
//         --min-chunk D (minimum draws per secondary command buffer, default 128),
//         --cache-dir DIR (default %LOCALAPPDATA%\alamo\render), --hidden (never show the window),
//         --no-batch (maps: unbatched reference terrain / water draws), --no-instancing (one call per mesh item), --compat-effect-remap (opt-in
//         materials::compat_missing_effect_remap(): FissureMeshGloss -> MeshGloss). Exit code is non-zero if a
//         validation error/warning occurred or loading failed.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "alamo/assets/asset_cache.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/core/log.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/jobs/job_system.hpp"
#include "alamo/render/camera.hpp"
#include "alamo/render/convert.hpp"
#include "alamo/render/fleet.hpp"
#include "alamo/render/hardpoints.hpp"
#include "alamo/render/map_scene.hpp"
#include "alamo/render/pipelines.hpp"
#include "alamo/render/renderer.hpp"
#include "alamo/render/objects.hpp"
#include "alamo/render/ted.hpp"
#include "alamo/rhi/rhi.hpp"
#include "alamo/vfs/vfs.hpp"
#include "app_common.hpp"  // engine/render/app (CU4 t03)

using namespace alamo;
using Clock = std::chrono::steady_clock;

namespace {

struct Options {
    bool frame_graph = false;      // --clear --frame-graph: an empty scene through Renderer::render (sync smoke test)
    float camera_distance = -1.0f; // --camera-distance D: map start camera distance override
    bool no_batch = false;         // --no-batch: map terrain / water drawn one post_translated call per draw (reference)
    bool no_instancing = false;    // --no-instancing: one draw call per mesh item (RecordOptions::instancing off)
    bool hidden = false;           // --hidden: the window is never shown (swapchain on a hidden window; smoke tests)
    bool clear = false, unlit = false, show_damage = false, sweep = false, cold = false;
    std::string game, model, anim, map;
    std::vector<std::string> game_args;
    long frames = -1;
    uint32_t width = 1280, height = 720;
    rhi::PresentMode mode = rhi::PresentMode::Fifo;
    int32_t workers = -1;
    bool exercise_window = false;  // --exercise-window: scripted resize + minimise/restore (automation)
    std::string cache_dir;
    bool team = false;
    float team_rgb[3] = {1, 1, 1};
    float sun_yaw = -1000.0f, sun_elev = -1000.0f;  // degrees; default: the environment's sun
    std::string effect_info;  // --effect-info A,B,...: print how materials resolved these effects
    std::string env;          // --env <map>: lighting from the map's start environment (TEE record)
    bool land = false;        // --land: built-in land environment (Endor Sunrise_Clear) instead of Kuat space
    bool soft_shadows = false, no_post = false, no_shadows = false, no_sky = false;
    bool ao = false;                             // [port] --ao: screen-space ambient occlusion
    float ao_strength = -1.0f, ao_radius = -1.0f;  // --ao-strength S / --ao-radius R (< 0: RecordOptions default)
    bool taa = false;                            // [port] --taa: temporal anti-aliasing (RecordOptions::taa)
    float taa_sharpen = -1.0f;                   // --taa-sharpen S (0..1; < 0: RecordOptions default)
    int fsr = -1;                                // [port] --fsr Q: FSR upscaling, quality mode Q 0..4 (RecordOptions::upscaler)
    bool smaa = false;                           // [port] --smaa: SMAA 1x (RecordOptions::smaa)
    uint32_t msaa = 1;                           // [port] --msaa N: MSAA samples (RecordOptions::msaa; 1, 2, 4, 8)
    float time_scale = 1.0f;  // scene clock = wall time x scale (UV scrolling, FF expressions)
    uint32_t fleet = 0, seed = 1;
    int32_t record_threads = -1;  // -1: worker count
    uint32_t min_chunk = 128;
    bool fow = false;               // --fow: fog of war with a synthetic visibility grid (disc around the camera target)
    bool dynamic_track_demo = false;  // --dynamic-track-demo: a DynamicTrackGenerator driven around the camera target + a blob
    std::string type;                 // --type NAME: a GameObjectType's space model + its hardpoints (Model_To_Attach turrets)
    std::string destroy_hp;           // --destroy-hardpoints all|A,B,..: destroyed hardpoints (--type model / every map placement)
    bool compat_effect_remap = false; // --compat-effect-remap: missing effects -> shipped stand-ins (FissureMeshGloss ->
                                      // MeshGloss; materials::compat_missing_effect_remap(), not original behaviour)
    uint32_t aniso = 0;               // --aniso N: [port] Renderer::set_anisotropy (0/1 off, 2..16; default off)
};

// EffectCache options of this run (--compat-effect-remap adds the opt-in missing-effect remap).
materials::LoadOptions effect_options(const Options& opt) {
    materials::LoadOptions o = render::EffectCache::default_options();
    if (opt.compat_effect_remap) o.missing_effect_remap = materials::compat_missing_effect_remap();
    return o;
}

// --destroy-hardpoints: "all" or comma-separated hardpoint names -> indices into ti.hardpoints. Unknown names are
// reported once (report = true).
std::vector<size_t> destroy_list(const std::string& spec, const render::TypeRenderInfo& ti, bool report) {
    std::vector<size_t> out;
    if (spec == "all") {
        for (size_t i = 0; i < ti.hardpoints.size(); ++i) out.push_back(i);
        return out;
    }
    size_t s = 0;
    while (s < spec.size()) {
        const size_t e = std::min(spec.find(',', s), spec.size());
        const std::string name = spec.substr(s, e - s);
        s = e + 1;
        if (name.empty()) continue;
        const int32_t h = ti.hardpoint_index(name);
        if (h >= 0) out.push_back(static_cast<size_t>(h));
        else if (report) std::fprintf(stderr, "--destroy-hardpoints: %s is no hardpoint of this type\n", name.c_str());
    }
    return out;
}

int usage() {
    std::fprintf(stderr,
                 "usage: alamo_render --clear [--frames N]\n"
                 "       alamo_render --game DIR --model NAME.ALO [--anim NAME.ALA] [--unlit] [--show-damage]\n"
                 "                    [--team-color R,G,B] [--sun-yaw DEG] [--sun-elev DEG] [MODPATH=..] [FALLBACKPATH=..]\n"
                 "       alamo_render --game DIR --type GAMEOBJECTTYPE [--destroy-hardpoints all|NAME,..] (e.g. Star_Destroyer)\n"
                 "       alamo_render --game DIR --fleet N [--seed S]\n"
                 "       alamo_render --game DIR --map NAME (e.g. _MP_LAND_ENDOR)\n"
                 "       alamo_render --game DIR --sweep [--cold]\n"
                 "       common: [--frames N] [--width W] [--height H] [--mailbox|--immediate] [--workers N] [--cache-dir DIR]\n"
                 "               [--record-threads T] [--min-chunk D] [--hidden] [--no-batch] [--camera-distance D]\n"
                 "               [--compat-effect-remap] (opt-in: FissureMeshGloss -> MeshGloss instead of the green default)\n"
                 "       maps:   [--fow] [--dynamic-track-demo] [--destroy-hardpoints all|NAME,..] (land or space .ted)\n"
                 "       [--ao] [--ao-strength S] [--ao-radius R] [port] screen-space ambient occlusion (land maps)\n"
                 "       [--taa] [--taa-sharpen S] [port] temporal anti-aliasing (S 0..1, 0 = no sharpen)\n"
                 "       [--fsr Q] [port] FSR upscaling, Q = 0 native AA, 1 quality, 2 balanced, 3 performance, 4 ultra performance\n"
                 "       [--smaa] [port] SMAA 1x after the post chain (and after TAA)\n"
                 "       [--msaa N] [port] multisampled scene, N = 2, 4 or 8 (clamped to the device)\n"
                 "       [--aniso N] anisotropic filtering of world textures (0/1 off, 2, 4, 8, 16; default off)\n");
    return 2;
}

struct DepthTarget {
    rhi::Image image;
    bool create(rhi::Allocator& a, uint32_t w, uint32_t h) {
        if (image) a.destroy(image);
        if (!w || !h) return true;
        rhi::ImageDesc d;
        d.format = rhi::Format::D32F;
        d.width = w;
        d.height = h;
        d.mips = 1;
        d.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        d.name = "render.depth";
        return a.create_image(d, image);
    }
};

// Pipeline / shader (track B) caches of --cache-dir: app_common.hpp.
std::filesystem::path cache_dir_of(const Options& o) { return render_app::cache_dir_of(o.cache_dir); }
std::filesystem::path shader_cache_of(const Options& o) { return render_app::shader_cache_of(o.cache_dir); }

// Scene lighting: --env map (start environment), else the built-in Kuat space (or Endor land) record.
render::Lighting scene_lighting(const Options& o, const FileProvider* files, bool& ok) {
    ok = true;
    render::Lighting l = o.land ? render::Lighting::land_default() : render::Lighting::space_default();
    if (!o.env.empty() && files) {
        render::MapEnvironments me;
        std::string err;
        if (render::load_map_environments(*files, o.env, me, &err) && me.start()) {
            l = render::Lighting::from_environment(*me.start(), me.space);
            std::printf("environment %s: %zu records, start %u \"%s\"%s\n", render::map_path(o.env).c_str(),
                        me.environments.size(), me.start_index, me.start()->name.c_str(), me.space ? " (space: fills off)" : "");
        } else {
            std::fprintf(stderr, "--env %s: %s\n", o.env.c_str(), err.c_str());
            ok = false;
        }
    }
    if (o.sun_yaw > -999.0f && o.sun_elev > -999.0f) l.set_sun_angles(o.sun_yaw * 0.017453293f, o.sun_elev * 0.017453293f);
    const auto& s = l.sun;
    std::printf("lighting \"%s\": sun (%.3f,%.3f,%.3f) x %.3f dir (%.3f,%.3f,%.3f) spec (%.3f,%.3f,%.3f); fills x %.2f / %.2f; "
                "ambient (%.3f,%.3f,%.3f); bloom %.2f/%.2f/%.2f\n",
                l.environment.c_str(), s.color.x, s.color.y, s.color.z, s.intensity, s.direction.x, s.direction.y, s.direction.z,
                s.specular.x, s.specular.y, s.specular.z, l.fill[0].intensity, l.fill[1].intensity, l.ambient.x, l.ambient.y,
                l.ambient.z, l.bloom_strength, l.bloom_cutoff, l.bloom_size);
    return l;
}


struct Averages {
    double build = 0, record = 0, submit = 0, frame = 0, gpu = 0, cull = 0, sort = 0, fill = 0;
    double world = 0, post = 0, gpu_world = 0, gpu_scene = 0, gpu_post = 0;  // render() sections (CPU record / GPU)
    double gpu_ao = 0;  // [port] "ao" scope (inside "scene")
    long n = 0, gpu_n = 0;
};
// ---------------------------------------------------------------------------------------- sweep

int run_sweep(const Options& opt, vfs::Vfs& files, bool strict) {
    const std::filesystem::path dir = cache_dir_of(opt);
    if (opt.cold) {
        std::error_code ec;
        // Long-path safe listing (MinGW's directory_iterator lists the CWD for long "\\?\" paths) and a
        // UTF-8 name compare (path::string() throws on names outside the ANSI code page).
        for (const std::filesystem::path& p : alamo::list_directory(dir, ec))
            if (alamo::path_to_utf8(p.filename()).rfind("alamo_pso_", 0) == 0) std::filesystem::remove(alamo::long_path(p), ec);
        if (!opt.cache_dir.empty()) alamo::remove_all_long(shader_cache_of(opt), ec);  // cold track B too
    }
    rhi::Context ctx;
    rhi::ContextDesc cd;
    cd.app_name = "alamo_render_sweep";
    cd.validation = strict ? rhi::Validation::Required : rhi::Validation::IfAvailable;
    if (!ctx.init(cd)) {
        std::fprintf(stderr, "no suitable Vulkan 1.3 device\n");
        return 1;
    }
    int rc = 0;
    {
        rhi::Allocator alloc;
        rhi::Uploader up;
        jobs::JobSystemConfig jc;
        jc.workers = opt.workers;
        jobs::JobSystem js(jc);
        render::EffectCache effects(&files, effect_options(opt));
        render::Renderer renderer;
        render::RendererDesc rd;
        rd.color_format = VK_FORMAT_B8G8R8A8_UNORM;  // the swapchain format of this PC (alamo_render prints it)
        rd.depth_format = VK_FORMAT_D32_SFLOAT;
        rd.effects = &effects;
        rd.js = &js;
        rd.pipeline_cache_dir = dir;
        rd.shader_cache_dir = shader_cache_of(opt);
        const auto ti0 = Clock::now();
        if (!alloc.init(ctx) || !up.init(ctx, alloc) || !renderer.init(ctx, alloc, up, rd)) return 1;
        render::PipelineManager& pm = renderer.pipelines();
        pm.wait_idle();  // pre-creation of the keys reloaded from disk
        const double preload_ms = std::chrono::duration<double, std::milli>(Clock::now() - ti0).count();
        const render::PipelineManager::Stats s0 = pm.stats();
        std::printf("pipeline cache %s: %u keys reloaded (%u ready) in %.0f ms, VkPipelineCache %s%s; preload %u hits / %u misses\n",
                    alamo::path_to_utf8(dir).c_str(), s0.reloaded, s0.ready, preload_ms, s0.cache_loaded ? "loaded" : "empty",
                    s0.invalidated ? ", invalidated" : "", s0.cache_hits, s0.cache_misses);

        assets::AssetCache cache(files, effects.model_load_options());  // effect _ALAMO_VERTEX_TYPE overrides (D.2)
        const std::vector<std::string> models = files.list("Data\\Art\\Models\\*.ALO");
        struct Result {
            std::vector<materials::PsoKey> keys;
            uint32_t submeshes = 0, lit = 0, no_effect = 0, untranslated = 0, mismatch = 0, not_drawn = 0, damage = 0;
            uint32_t bump_gloss = 0;  // submeshes on MeshBumpColorize / MeshGloss*
            uint64_t bump_gloss_tris = 0;
            bool loaded = false;
            struct XPass {  // track-B pass to translate (keys need the translated program)
                std::shared_ptr<const materials::MaterialEffect> eff;
                const materials::ResolvedPass* pass;
                materials::VertexLayout layout;
            };
            std::vector<XPass> xpasses;
            std::vector<std::string> xeffects;  // effect of every translated submesh
            std::vector<std::string> prev;      // ... of those, the ones C3b could not draw (no FF technique)
            std::vector<std::string> skipped;   // Shadow / Heat bucket effects (not drawn yet)
        };
        // C3b settings (FF fallback on) only to name the submeshes that were undrawable before track B.
        materials::LoadOptions c3b = effect_options(opt);
        c3b.ff_fallback_for_untranslated = true;
        render::EffectCache c3b_effects(&files, c3b);
        std::vector<Result> res(models.size());
        const uint8_t rp = render::render_pass_id(rd.color_format, rd.depth_format);
        const auto tl0 = Clock::now();
        js.parallel_for(static_cast<uint32_t>(models.size()), 4, [&](uint32_t b, uint32_t e) {
            for (uint32_t i = b; i < e; ++i) {
                Result& r = res[i];
                auto m = cache.model(models[i]);
                if (!m) continue;
                r.loaded = true;
                const render::MeshSelection sel = render::select_meshes(*m, 1.0f, 0, false);
                r.damage = sel.damage_hidden;
                for (uint32_t si : render::drawable_submeshes(*m, sel, true)) {
                    const assets::Submesh& sm = m->submeshes[si];
                    ++r.submeshes;
                    const materials::VertexLayout layout = render::vertex_layout_for(
                        sm.vertex_type_overridden ? sm.effective_vertex_format : sm.vertex_format, sm.format);
                    auto eff = sm.material.effect.empty() ? nullptr : effects.get(sm.material.effect);
                    if (!eff) {
                        ++r.no_effect;
                        continue;
                    }
                    const std::string ek = render::effect_key(sm.material.effect);
                    if (ek == "MESHBUMPCOLORIZE" || ek.rfind("MESHGLOSS", 0) == 0) {
                        ++r.bump_gloss;
                        r.bump_gloss_tris += sm.index_count / 3;
                    }
                    const render::PhaseBucket bucket = render::phase_bucket(eff->phase);
                    bool ok = true, mism = false;
                    for (const auto& p : eff->passes) {
                        if (p.restore_only) continue;
                        const bool xl = p.program_status == materials::ProgramStatus::Translated;
                        if (!xl && (p.program_status != materials::ProgramStatus::Ok || !p.program.vs)) ok = false;
                        const auto el = p.pso[0].vertex_layout;
                        if (!p.fixed_function && !xl && el != materials::VertexLayout::Unknown &&
                            materials::vertex_layout_skinned(el) != materials::vertex_layout_skinned(layout))
                            mism = true;
                    }
                    if (!ok) {
                        ++r.untranslated;
                        continue;
                    }
                    if (mism) {
                        ++r.mismatch;
                        continue;
                    }
                    // Shadow and Heat buckets are drawn by the frame graph (stencil volumes / heat pass).
                    if (bucket == render::PhaseBucket::Heat) r.skipped.push_back(ek);
                    if (bucket == render::PhaseBucket::Shadow) ++r.not_drawn;  // counted as shadow volumes below
                    ++r.lit;
                    bool any_x = false;
                    for (const auto& p : eff->passes) {
                        if (p.restore_only) continue;
                        if (p.program_status == materials::ProgramStatus::Translated) {
                            r.xpasses.push_back({eff, &p, layout});
                            any_x = true;
                            continue;
                        }
                        for (int v = 0; v < 2; ++v) {
                            if (v == 1 && !p.fade_dynamic && !p.fade_expr) continue;
                            materials::PsoKey k = p.pso[v];  // same rule as Renderer::draw_key_variant
                            k.render_pass = rp;
                            k.vertex_layout = layout;
                            if (p.fixed_function) {
                                const materials::Program prog = materials::ff_program(materials::ff_inputs_for_layout(layout));
                                if (prog.vs) k.vs_id = prog.vs->id;
                                if (prog.ps) k.ps_id = prog.ps->id;
                            }
                            r.keys.push_back(k);
                        }
                    }
                    if (any_x) {
                        r.xeffects.push_back(ek);
                        bool before = false;  // with the FF fallback, still Translated = no FF technique
                        if (auto old = c3b_effects.get(sm.material.effect))
                            for (const auto& op : old->passes)
                                before = before || (!op.restore_only && op.program_status == materials::ProgramStatus::Translated);
                        if (before) r.prev.push_back(ek);
                    }
                }
            }
        });
        const double load_ms = std::chrono::duration<double, std::milli>(Clock::now() - tl0).count();
        std::set<materials::PsoKey> keys;
        Result tot;
        uint32_t loaded = 0;
        std::vector<std::pair<uint64_t, size_t>> ranking;
        for (size_t i = 0; i < res.size(); ++i) {
            const Result& r = res[i];
            loaded += r.loaded ? 1u : 0u;
            keys.insert(r.keys.begin(), r.keys.end());
            tot.submeshes += r.submeshes;
            tot.lit += r.lit;
            tot.no_effect += r.no_effect;
            tot.untranslated += r.untranslated;
            tot.mismatch += r.mismatch;
            tot.not_drawn += r.not_drawn;
            tot.damage += r.damage;
            const std::string& n = models[i];
            const size_t slash = n.find_last_of('\\');
            const std::string base = n.substr(slash + 1);
            if (base.size() > 3 && base[2] == '_' && base[1] == 'V' && r.bump_gloss >= 3) ranking.push_back({r.bump_gloss_tris, i});
        }
        std::printf("sweep: %zu ALOs listed, %u loaded in %.0f ms; %u drawable submeshes: %u lit, %u no effect, %u untranslated "
                    "(no FF technique), %u layout-family mismatch, %u stencil shadow volumes; %u damage meshes hidden\n",
                    models.size(), loaded, load_ms, tot.submeshes, tot.lit, tot.no_effect, tot.untranslated, tot.mismatch,
                    tot.not_drawn, tot.damage);
        const render::EffectCache::Stats es = effects.stats();
        std::printf("effects: %zu loaded, %zu failed (%zu green default, %zu remapped), %zu via FF fallback, %zu translated + %zu "
                    "untranslatable passes\n",
                    es.loaded, es.failed, es.defaulted, es.remapped, es.ff_fallback, es.translated_passes, es.untranslated_passes);
        // Track B: every distinct translated program (runtime DXC + shader cache), then its pipelines.
        std::map<std::string, int> xfx;
        std::vector<std::pair<size_t, size_t>> xrank;
        size_t xsub = 0;
        for (size_t i = 0; i < res.size(); ++i) {
            for (const std::string& n : res[i].xeffects) ++xfx[n];
            xsub += res[i].xeffects.size();
            if (!res[i].xeffects.empty()) xrank.push_back({res[i].xeffects.size(), i});
        }
        std::printf("track B: %zu submeshes draw translated passes (runtime DXC %s):", xsub, renderer.dxc_status().c_str());
        for (const auto& [n, k] : xfx) std::printf(" %s x%d", n.c_str(), k);
        std::printf("\n");
        std::sort(xrank.rbegin(), xrank.rend());
        std::map<std::string, int> prevfx;
        size_t nprev = 0;
        for (const Result& r : res)
            for (const std::string& n : r.prev) {
                ++prevfx[n];
                ++nprev;
            }
        std::printf("track B: %zu of them were undrawable in C3b (best technique untranslated, no FF technique):", nprev);
        for (const auto& [n, k] : prevfx) std::printf(" %s x%d", n.c_str(), k);
        std::printf("\n");
        std::printf("Heat bucket users (drawn into the distortion target):");
        for (size_t i = 0; i < res.size(); ++i)
            if (!res[i].skipped.empty()) std::printf(" %s (%s x%zu)", models[i].c_str() + 16, res[i].skipped[0].c_str(), res[i].skipped.size());
        std::printf("\n");
        std::printf("models with the most translated submeshes:");
        for (size_t i = 0; i < xrank.size() && i < 6; ++i) std::printf(" %s (%zu)", models[xrank[i].second].c_str() + 16, xrank[i].first);
        std::printf("\n");
        const auto tx0 = Clock::now();
        for (const Result& r : res)
            for (const auto& x : r.xpasses) (void)renderer.translated_program(x.eff, *x.pass);
        renderer.wait_translations();
        const double xms = std::chrono::duration<double, std::milli>(Clock::now() - tx0).count();
        const render::Renderer::TranslationStats xs = renderer.translation_stats();
        std::printf("track B: %u programs (VS+PS pairs): %u ready, %u failed, %u from the shader cache; %.0f ms wall, %.0f ms "
                    "summed job time\n",
                    xs.programs, xs.ready, xs.failed, xs.cache_hits, xms, xs.translate_ms);
        if (xs.failed) rc = 1;
        for (const Result& r : res)
            for (const auto& x : r.xpasses) {
                const materials::TranslatedProgram* tp = renderer.translated_program(x.eff, *x.pass);
                if (!tp) continue;
                for (int v = 0; v < 2; ++v) {
                    if (v == 1 && !x.pass->fade_dynamic && !x.pass->fade_expr) continue;
                    materials::PsoKey k = tp->key(x.pass->pso[v]);  // same rule as Renderer::translated_key
                    k.render_pass = rp;
                    k.vertex_layout = x.layout;
                    keys.insert(k);
                }
            }
        std::sort(ranking.rbegin(), ranking.rend());
        std::printf("ships with the most MeshBumpColorize/MeshGloss* triangles:");
        for (size_t i = 0; i < ranking.size() && i < 6; ++i)
            std::printf(" %s (%llu)", models[ranking[i].second].c_str() + 16, static_cast<unsigned long long>(ranking[i].first));
        std::printf("\n");

        const auto tp0 = Clock::now();
        for (const materials::PsoKey& k : keys) (void)pm.get(k);
        pm.wait_idle();
        const double wall_ms = std::chrono::duration<double, std::milli>(Clock::now() - tp0).count();
        const render::PipelineManager::Stats s1 = pm.stats();
        std::printf("pipelines: %zu distinct PsoKeys in the install; manager %u keys (%u ready, %u failed); creation of the "
                    "keys not preloaded: %.1f ms wall on %u workers; summed vkCreateGraphicsPipelines %.1f ms (max %.2f ms); "
                    "VkPipelineCache (rhi stats): %u hits, %u misses, %u without feedback\n",
                    keys.size(), s1.keys, s1.ready, s1.failed, wall_ms, js.worker_count(), s1.create_ms, s1.max_create_ms,
                    s1.cache_hits, s1.cache_misses, s1.feedback_unknown);
        if (s1.failed) rc = 1;
        ctx.wait_idle();
        renderer.shutdown();  // saves keys + VkPipelineCache
        up.shutdown();
        alloc.shutdown();
    }
    std::printf("validation: %s, %u errors, %u warnings\n", ctx.validation_enabled() ? "on" : "off", ctx.validation_errors(),
                ctx.validation_warnings());
    if (ctx.validation_errors() || ctx.validation_warnings()) rc = 1;
    ctx.shutdown();
    return rc;
}

}  // namespace

#ifdef ALAMO_RENDER_HAVE_SIM
int run_sim_main(int argc, char** argv);  // app/sim_main.cpp: --sim / --sim-headless-check (R1)
#endif
#ifdef ALAMO_RENDER_HAVE_GALACTIC  // R2 hook (owner: engine/render/galactic)
int run_galactic_main(int argc, char** argv);  // galactic/app/galactic_main.cpp: --galactic / --galactic-headless-check
#endif
#ifdef ALAMO_RENDER_HAVE_PARTICLES  // R4 hook (owner: engine/render/particles)
int run_particles_main(int argc, char** argv);  // particles/app/particles_main.cpp: --particles
#endif

int main(int argc, char** argv) {
#ifdef ALAMO_RENDER_HAVE_PARTICLES  // R4 hook
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--particles") == 0) return run_particles_main(argc, argv);
#endif
#ifdef ALAMO_RENDER_HAVE_SIM
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--sim") == 0 || std::strcmp(argv[i], "--sim-headless-check") == 0) return run_sim_main(argc, argv);
#endif
#ifdef ALAMO_RENDER_HAVE_GALACTIC  // R2 hook
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--galactic") == 0 || std::strcmp(argv[i], "--galactic-headless-check") == 0)
            return run_galactic_main(argc, argv);
#endif
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--clear") opt.clear = true;
        else if (a == "--frame-graph") opt.frame_graph = true;
        else if (a == "--camera-distance") opt.camera_distance = std::strtof(next().c_str(), nullptr);
        else if (a == "--no-batch") opt.no_batch = true;
        else if (a == "--no-instancing") opt.no_instancing = true;
        else if (a == "--hidden") opt.hidden = true;
        else if (a == "--game") opt.game = next();
        else if (a == "--model") opt.model = next();
        else if (a == "--map") opt.map = next();
        else if (a == "--anim") opt.anim = next();
        else if (a == "--frames") opt.frames = std::atol(next().c_str());
        else if (a == "--width") opt.width = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--height") opt.height = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--mailbox") opt.mode = rhi::PresentMode::Mailbox;
        else if (a == "--immediate") opt.mode = rhi::PresentMode::Immediate;
        else if (a == "--exercise-window") opt.exercise_window = true;
        else if (a == "--workers") opt.workers = static_cast<int32_t>(std::strtol(next().c_str(), nullptr, 10));
        else if (a == "--unlit") opt.unlit = true;
        else if (a == "--show-damage") opt.show_damage = true;
        else if (a == "--sweep") opt.sweep = true;
        else if (a == "--cold") opt.cold = true;
        else if (a == "--cache-dir") opt.cache_dir = next();
        else if (a == "--fleet") opt.fleet = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--seed") opt.seed = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--record-threads") opt.record_threads = static_cast<int32_t>(std::strtol(next().c_str(), nullptr, 10));
        else if (a == "--min-chunk") opt.min_chunk = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--sun-yaw") opt.sun_yaw = std::strtof(next().c_str(), nullptr);
        else if (a == "--sun-elev") opt.sun_elev = std::strtof(next().c_str(), nullptr);
        else if (a == "--time-scale") opt.time_scale = std::strtof(next().c_str(), nullptr);
        else if (a == "--env") opt.env = next();
        else if (a == "--effect-info") opt.effect_info = next();
        else if (a == "--land") opt.land = true;
        else if (a == "--soft-shadows") opt.soft_shadows = true;
        else if (a == "--ao") opt.ao = true;
        else if (a == "--taa") opt.taa = true;
        else if (a == "--smaa") opt.smaa = true;
        else if (a == "--msaa") opt.msaa = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--taa-sharpen") opt.taa_sharpen = std::strtof(next().c_str(), nullptr);
        else if (a == "--fsr") opt.fsr = std::clamp(std::atoi(next().c_str()), 0, 4);
        else if (a == "--ao-strength") opt.ao_strength = std::strtof(next().c_str(), nullptr);
        else if (a == "--ao-radius") opt.ao_radius = std::strtof(next().c_str(), nullptr);
        else if (a == "--no-post") opt.no_post = true;
        else if (a == "--no-shadows") opt.no_shadows = true;
        else if (a == "--no-sky") opt.no_sky = true;
        else if (a == "--fow") opt.fow = true;
        else if (a == "--dynamic-track-demo") opt.dynamic_track_demo = true;
        else if (a == "--type") opt.type = next();
        else if (a == "--destroy-hardpoints") opt.destroy_hp = next();
        else if (a == "--compat-effect-remap") opt.compat_effect_remap = true;
        else if (a == "--aniso") opt.aniso = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--team-color") {
            const std::string v = next();
            opt.team = std::sscanf(v.c_str(), "%f,%f,%f", &opt.team_rgb[0], &opt.team_rgb[1], &opt.team_rgb[2]) == 3;
            if (!opt.team) return usage();
        } else if (a.find('=') != std::string::npos) opt.game_args.push_back(a);
        else return usage();
    }
    if (!opt.clear && (opt.game.empty() || (opt.model.empty() && opt.type.empty() && opt.map.empty() && !opt.sweep && !opt.fleet &&
                                            opt.effect_info.empty())))
        return usage();

    const bool strict = render::setup_validation_env(ALAMO_RENDER_SOURCE_DIR);

    // ---- data (before the window, so a bad path fails fast)
    std::unique_ptr<vfs::Vfs> files;
    if (!opt.clear) {
        std::string err;
        files = vfs::mount_game(opt.game, opt.game_args, &err);  // mounted exactly as alamo_headless does
        if (!files) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        std::printf("VFS: %zu MEGs mounted, EaW root '%s'\n", files->mounts().size(), files->eaw_root().c_str());
    }
    if (opt.sweep) return run_sweep(opt, *files, strict);
    if (!opt.effect_info.empty()) {  // diagnostics: how materials resolved some effects
        render::EffectCache effects(files.get(), effect_options(opt));
        size_t s = 0;
        while (s < opt.effect_info.size()) {
            const size_t e = std::min(opt.effect_info.find(',', s), opt.effect_info.size());
            const std::string name = opt.effect_info.substr(s, e - s);
            s = e + 1;
            std::string err;
            auto fx = effects.get(name, &err);
            if (!fx) {
                std::printf("%s: %s\n", name.c_str(), err.c_str());
                continue;
            }
            std::printf("%s: phase %s, technique %s, layout %s, %zu passes\n", fx->name.c_str(),
                        materials::render_phase_name(fx->phase), fx->technique_name.c_str(), materials::vertex_layout_name(fx->layout),
                        fx->passes.size());
            for (const auto& p : fx->passes) {
                const materials::PsoKey& k = p.pso[0];
                static const char* const kStatus[] = {"hot-set", "needs translation", "unknown permutation", "translated"};
                std::printf("  pass %s: %s%s%s, vs %s ps %s, blend %u (%u,%u) cw %x cull %u z %u/%u op %u stencil %u (f %u/%u/%u cmp %u, b %u/%u/%u cmp %u) ref %u rmask %x wmask %x, alpha %u/%u, fog %u\n",
                            p.name.c_str(), kStatus[static_cast<int>(p.program_status) & 3], p.fixed_function ? " FF" : "",
                            p.restore_only ? " restore-only" : "", p.program.vs ? p.program.vs->name : "-", p.program.ps ? p.program.ps->name : "-",
                            k.blend_enable, static_cast<unsigned>(k.src_color), static_cast<unsigned>(k.dst_color), k.color_write_mask,
                            static_cast<unsigned>(k.cull), k.depth_test, k.depth_write, static_cast<unsigned>(k.depth_compare), k.stencil_enable,
                            static_cast<unsigned>(k.front.fail_op), static_cast<unsigned>(k.front.depth_fail_op),
                            static_cast<unsigned>(k.front.pass_op), static_cast<unsigned>(k.front.compare),
                            static_cast<unsigned>(k.back.fail_op), static_cast<unsigned>(k.back.depth_fail_op),
                            static_cast<unsigned>(k.back.pass_op), static_cast<unsigned>(k.back.compare), p.dynamic.stencil_ref,
                            p.dynamic.stencil_read_mask, p.dynamic.stencil_write_mask, p.push.alpha_func, p.push.alpha_ref, p.push.flags);
                for (const auto& sb : p.samplers)
                    std::printf("    s%u: texture %s semantic %s\n", sb.slot, sb.texture_param.c_str(), sb.texture_semantic.c_str());
            }
            for (const auto& pb : fx->params)
                if (pb.numeric || pb.texture) std::printf("    param %s : %s%s\n", pb.name.c_str(), pb.semantic.c_str(), pb.engine ? " (engine)" : "");
        }
        return 0;
    }

    rhi::Window win;
    if (!win.create({"alamo_render (Esc quits)", opt.width, opt.height, true, !opt.hidden})) return 1;

    rhi::Context ctx;
    rhi::ContextDesc cd;
    cd.app_name = "alamo_render";
    cd.presentation = true;
    cd.upscaler_features = opt.fsr >= 0;  // [port] FSR's ML upscaler features
    cd.validation = strict ? rhi::Validation::Required : rhi::Validation::IfAvailable;
    if (!ctx.init(cd)) {
        std::fprintf(stderr, "no suitable Vulkan 1.3 device%s\n", strict ? " (or the validation layer failed to load)" : "");
        return 1;
    }
    std::printf("device %s, validation %s%s\n", ctx.caps().name.c_str(), ctx.validation_enabled() ? "on" : "off",
                strict ? " (required)" : "");

    int exit_code = 0;
    {
        std::unique_ptr<jobs::JobSystem> js;
        if (!opt.clear) {
            jobs::JobSystemConfig jc;
            jc.workers = opt.workers;
            js = std::make_unique<jobs::JobSystem>(jc);
        }
        rhi::Allocator alloc;
        rhi::Uploader up;
        rhi::Swapchain sc;
        rhi::FrameRing ring;
        rhi::SwapchainDesc sd;
        sd.hinstance = win.native_instance();
        sd.hwnd = win.native_handle();
        sd.width = win.width();
        sd.height = win.height();
        sd.mode = opt.mode;
        rhi::FrameRingDesc fd;
        fd.frames_in_flight = 2;
        fd.uniform_bytes = (opt.fleet || !opt.map.empty()) ? (128ull << 20) : (8ull << 20);
        // One pool per job-system slot: Renderer records secondaries on frame.secondary(js->current_slot()),
        // so the ring must have >= js->thread_count() recording threads (clamped up here, checked below).
        fd.recording_threads = render::ring_recording_threads(1, js ? js->thread_count() : 1);
        if (js && fd.recording_threads < js->thread_count()) {
            std::fprintf(stderr, "frame ring: %u recording threads < job system thread_count %u\n", fd.recording_threads,
                         js->thread_count());
            ctx.shutdown();
            return 1;
        }
        if (!alloc.init(ctx) || !up.init(ctx, alloc) || !sc.init(ctx, sd) || !ring.init(ctx, alloc, fd)) {
            std::fprintf(stderr, "rhi init failed\n");
            ctx.shutdown();
            return 1;
        }
        std::printf("swapchain %ux%u, %u images, format %d, present mode %d\n", sc.extent().width, sc.extent().height,
                    sc.image_count(), static_cast<int>(sc.format()), static_cast<int>(sc.present_mode()));
        rhi::GpuTimer gpu;
        const bool have_gpu_timer = gpu.init(ctx, fd.frames_in_flight);

        DepthTarget depth;
        render::Renderer renderer;
        renderer.set_anisotropy(opt.aniso);  // --aniso (kept across the re-inits below)
        std::unique_ptr<render::EffectCache> effects;
        std::unique_ptr<assets::AssetCache> cache;
        std::unique_ptr<render::ModelStreamer> streamer;
        std::vector<uint32_t> handles;
        std::vector<std::pair<uint32_t, int>> sky_handles;  // (streamer handle, skydome index)
        std::vector<render::Instance> sky_instances;
        std::vector<render::FleetInstance> fleet;
        std::shared_ptr<const assets::AnimationClip> clip;
        std::atomic<bool> anim_done{false};
        jobs::JobCounter anim_counter;
        render::RecordOptions ropt;
        std::unique_ptr<data::GameDatabase> db;
        std::unique_ptr<render::GameTypeResolver> resolver;
        std::unique_ptr<render::MapScene> scene;
        // --type: the type's hardpoints, their Model_To_Attach handles (UINT32_MAX: none) and the instance state.
        render::TypeRenderInfo type_info;
        std::vector<uint32_t> attach_handles;
        render::HardpointVisibility type_vis;
        bool ok = true;
        if (opt.clear && opt.frame_graph) {  // renderer without data: post passes skipped, targets + copy exercised
            render::RendererDesc rd;
            rd.color_format = sc.format();
            rd.depth_format = VK_FORMAT_D32_SFLOAT;
            rd.translated = false;
            ok = renderer.init(ctx, alloc, up, rd);
            renderer.set_frame_ring(&ring);
            if (!ok) std::fprintf(stderr, "renderer init failed\n");
        }
        if (!opt.clear) {
            effects = std::make_unique<render::EffectCache>(files.get(), effect_options(opt));
            render::RendererDesc rd;
            rd.color_format = sc.format();
            rd.depth_format = VK_FORMAT_D32_SFLOAT;
            rd.effects = effects.get();
            rd.js = js.get();
            rd.pipeline_cache_dir = cache_dir_of(opt);
            rd.shader_cache_dir = shader_cache_of(opt);
            rd.hide_damage_decals = !opt.show_damage;
            rd.team_color_override = opt.team;
            rd.team_color = {opt.team_rgb[0], opt.team_rgb[1], opt.team_rgb[2], 1.0f};
            ropt.threads = opt.record_threads < 0 ? js->worker_count() : static_cast<uint32_t>(opt.record_threads);
            ropt.min_draws_per_chunk = opt.min_chunk;
            ropt.instancing = !opt.no_instancing;
            ropt.post = !opt.no_post;
            ropt.shadows = !opt.no_shadows;
            ropt.soft_shadows = opt.soft_shadows;
            ropt.ambient_occlusion = opt.ao;
            if (opt.ao_strength >= 0.0f) ropt.ao_strength = opt.ao_strength;
            if (opt.ao_radius > 0.0f) ropt.ao_radius = opt.ao_radius;
            ropt.taa = opt.taa;
            ropt.smaa = opt.smaa;
            ropt.msaa = opt.msaa;
            if (opt.taa_sharpen >= 0.0f) ropt.taa_sharpen = opt.taa_sharpen;
            if (opt.fsr >= 0) {  // [port] FSR (falls back to TAA where it cannot run)
                ropt.upscaler = render::Upscaler::Fsr;
                ropt.upscale_quality = static_cast<render::UpscaleQuality>(opt.fsr);
                ropt.taa = true;
            }
            ropt.gpu_timer = &gpu;  // render() sections (world / scene / post)
            ok = depth.create(alloc, sc.extent().width, sc.extent().height) && renderer.init(ctx, alloc, up, rd);
            renderer.set_frame_ring(&ring);  // bindless slots released / retired against the frame timeline
            if (ok) {
                bool env_ok = true;
                renderer.set_lighting(scene_lighting(opt, files.get(), env_ok));
                if (!env_ok) exit_code = 1;
                std::printf("track B: runtime DXC %s\n", renderer.dxc_status().c_str());
                const auto ps = renderer.pipelines().stats();
                std::printf("pipeline cache %s: %u keys reloaded, VkPipelineCache %s%s\n",
                            alamo::path_to_utf8(rd.pipeline_cache_dir).c_str(), ps.reloaded, ps.cache_loaded ? "loaded" : "empty",
                            ps.invalidated ? " (invalidated)" : "");
                cache = std::make_unique<assets::AssetCache>(*files, effects->model_load_options());  // D.2 vertex-type overrides
                streamer = std::make_unique<render::ModelStreamer>(renderer, *cache, *js);
                if (!opt.type.empty() && opt.map.empty()) {  // a GameObjectType: space model + hardpoints (B.10)
                    db = std::make_unique<data::GameDatabase>();
                    if (!db->load(*files)) std::fprintf(stderr, "game database: load reported errors (continuing)\n");
                    resolver = std::make_unique<render::GameTypeResolver>(*db);
                    if (!resolver->load_render_xml(*files)) std::fprintf(stderr, "hardpoint XML not found: no hardpoints\n");
                    terrain::ResolvedType rt;
                    const uint32_t crc = alamo::name_crc(opt.type);
                    if (!resolver->resolve(crc, true, rt) || rt.model_name.empty()) {
                        std::fprintf(stderr, "--type %s: unknown type or no model\n", opt.type.c_str());
                        ok = false;
                        exit_code = 1;
                    } else {
                        opt.model = rt.model_name;
                        type_info = resolver->render_info(crc);
                        std::printf("type %s: model %s, %zu hardpoints:", opt.type.c_str(), rt.model_name.c_str(), type_info.hardpoints.size());
                        for (const render::HardpointDef& d : type_info.hardpoint_defs) {
                            attach_handles.push_back(d.model_to_attach.empty() ? UINT32_MAX : streamer->request(d.model_to_attach));
                            std::printf(" %s%s%s", d.name.c_str(), d.model_to_attach.empty() ? "" : "+", d.model_to_attach.c_str());
                        }
                        std::printf("\n");
                    }
                }
                // Background objects of the environment (TEE 19/1A object types -> models via the XML).
                if (!opt.map.empty()) {
                    // Land map: game database (object types), terrain + water on the job system, placements.
                    const auto tm = Clock::now();
                    db = std::make_unique<data::GameDatabase>();
                    if (!db->load(*files)) std::fprintf(stderr, "game database: load reported errors (continuing)\n");
                    resolver = std::make_unique<render::GameTypeResolver>(*db);
                    if (!resolver->load_render_xml(*files))
                        std::fprintf(stderr, "hardpoint XML not found: damage decals use the *_BLAST* name rule\n");
                    const double db_ms = std::chrono::duration<double, std::milli>(Clock::now() - tm).count();
                    scene = std::make_unique<render::MapScene>();
                    scene->set_show_damage(opt.show_damage);
                    scene->set_sky_enabled(!opt.no_sky);
                    std::string err;
                    render::WorldOptions wo;
                    wo.batch_draws = !opt.no_batch;
                    if (!scene->load(*files, opt.map, resolver.get(), jobs::make_parallel_for(*js), &err) ||
                        !scene->start(renderer, *streamer, *cache, wo, &err)) {
                        std::fprintf(stderr, "--map %s: %s\n", opt.map.c_str(), err.c_str());
                        ok = false;
                        exit_code = 1;
                    } else {
                        const render::MapSceneStats& ms = scene->stats();
                        const terrain::MapData& md = scene->map();
                        std::printf("map %s (%s): grid %ux%u, %u patches, %u passes (max %u), %llu terrain triangles, water %s at %.1f "
                                    "(%u patches, %llu triangles); game database %.0f ms, parse %.1f ms, mesh %.1f ms, water %.1f ms "
                                    "(%u threads), resolve %.1f ms\n",
                                    opt.map.c_str(), md.info.planet_name.c_str(), md.terrain.width, md.terrain.height, ms.mesh.patches,
                                    ms.mesh.passes, ms.mesh.max_passes, static_cast<unsigned long long>(ms.mesh.triangles),
                                    scene->water().enabled ? scene->water().effect.c_str() : "none", scene->water().height, ms.water_patches,
                                    static_cast<unsigned long long>(ms.water_triangles), db_ms, ms.parse_ms, ms.mesh_ms, ms.water_ms,
                                    js->thread_count(), ms.resolve_ms);
                        std::printf("map objects: %u placements -> %u with a model (%u distinct models), %u type without model, "
                                    "%u unknown type\n",
                                    ms.objects, ms.placements, ms.distinct_models, ms.no_model, ms.unknown_type);
                        std::printf("map %s: decorations: %u tracks (%u built, %u on water), %u decals (%u built), %llu triangles, "
                                    "%.1f ms; FOW grid %ux%u; %u background placements; %zu DynamicTrackFX entries\n",
                                    scene->space() ? "space" : "land", ms.tracks, ms.tracks_built, ms.water_tracks, ms.decals,
                                    ms.decals_built, static_cast<unsigned long long>(ms.decoration_triangles), ms.decoration_ms,
                                    ms.fow_width, ms.fow_height, ms.background_placements, scene->dynamic_track_params().size());
                        scene->apply(ropt);  // space flag (background scene in world space, fills off there)
                        if (!opt.destroy_hp.empty()) {  // every placement whose type has the listed hardpoints (B.10)
                            uint32_t n_hp = 0, n_obj = 0;
                            const auto& pl = scene->placements().placements;
                            for (size_t pi = 0; pi < pl.size(); ++pi) {
                                const render::TypeRenderInfo* ti = scene->type_info(pl[pi].type_crc);
                                if (!ti) continue;
                                const std::vector<size_t> hl = destroy_list(opt.destroy_hp, *ti, false);
                                for (size_t h : hl) n_hp += scene->set_hardpoint_destroyed(pi, h, true) ? 1u : 0u;
                                n_obj += hl.empty() ? 0u : 1u;
                            }
                            std::printf("--destroy-hardpoints %s: %u hardpoints destroyed on %u placements\n", opt.destroy_hp.c_str(), n_hp,
                                        n_obj);
                        }
                        const render::Lighting& l = renderer.lighting();
                        std::printf("map environment \"%s\": fog %s %.0f..%.0f, bloom %.2f/%.2f/%.2f, shadow colour (%.2f,%.2f,%.2f), sky %s / %s\n",
                                    l.environment.c_str(), l.fog_enabled ? "on" : "off", l.fog_start, l.fog_end, l.bloom_strength,
                                    l.bloom_cutoff, l.bloom_size, l.shadow_color.x, l.shadow_color.y, l.shadow_color.z,
                                    l.skydome[0].c_str(), l.skydome[1].c_str());
                    }
                }
                for (int k = 0; k < 2 && !opt.no_sky && opt.map.empty(); ++k) {
                    const std::string& type = renderer.lighting().skydome[k];
                    if (type.empty()) continue;
                    const std::string model = render::find_object_model(*files, type);
                    std::printf("sky object %d: %s -> %s\n", k, type.c_str(), model.empty() ? "(not found)" : model.c_str());
                    if (!model.empty()) sky_handles.push_back({streamer->request(model), k});
                }
                if (opt.fleet) {
                    const auto tf = Clock::now();
                    const render::FleetModels fm = render::pick_fleet_models(*files, *cache, *js, opt.seed);
                    std::printf("fleet: %u XML ship models (%u capital, %u corvette, %u fighter loadable) -> %zu capital + %zu "
                                "corvette + %zu fighter types, %u with a translated pass (%u candidates%s) (%.0f ms):\n",
                                fm.candidates, fm.available[0], fm.available[1], fm.available[2], fm.capitals.size(),
                                fm.corvettes.size(), fm.fighters.size(), fm.translated_picked, fm.translated_candidates,
                                fm.translated_forced ? ", coverage pick" : "",
                                std::chrono::duration<double, std::milli>(Clock::now() - tf).count());
                    const std::vector<std::string>* fm_models[3] = {&fm.capitals, &fm.corvettes, &fm.fighters};
                    const std::vector<std::string>* fm_names[3] = {&fm.capital_names, &fm.corvette_names, &fm.fighter_names};
                    for (int c = 0; c < 3; ++c) {
                        std::printf("  %s:", render::fleet_class_name(static_cast<render::FleetClass>(c)));
                        for (size_t k = 0; k < fm_models[c]->size(); ++k)
                            std::printf(" %s=%s", (*fm_names[c])[k].c_str(), (*fm_models[c])[k].c_str() + 16);
                        std::printf("\n");
                    }
                    for (const auto& n : fm.capitals) handles.push_back(streamer->request(n));
                    for (const auto& n : fm.corvettes) handles.push_back(streamer->request(n));
                    for (const auto& n : fm.fighters) handles.push_back(streamer->request(n));
                    fleet = render::make_fleet(opt.fleet, opt.seed, static_cast<uint32_t>(fm.capitals.size()),
                                               static_cast<uint32_t>(fm.corvettes.size()), static_cast<uint32_t>(fm.fighters.size()));
                    if (handles.empty()) ok = false;
                } else if (opt.map.empty()) {
                    handles.push_back(streamer->request(opt.model));
                }
                if (!opt.anim.empty())
                    js->spawn(anim_counter, [&] {
                        clip = cache->animation(opt.anim);
                        anim_done = true;
                    }, jobs::Priority::Background);
                std::printf("loading on %u workers (%s), record threads %u, min %u draws per chunk\n", js->worker_count(),
                            opt.unlit ? "unlit" : "lit", ropt.threads, ropt.min_draws_per_chunk);
            } else {
                std::fprintf(stderr, "renderer init failed\n");
            }
        }

        render::OrbitCamera cam;
        std::vector<terrain::DynamicTrackGenerator> demo_tracks;  // --dynamic-track-demo
        render::Float3 demo_center{0, 0, 0};
        bool framed = false, reported = false, anim_reported = false, all_lit_reported = false;
        std::vector<render::Mat3x4> bone_world;
        std::vector<render::Instance> instances;
        assets::Pose pose;
        const auto t0 = Clock::now();
        auto t_ready = t0;
        long frames = 0, frames_since_ready = 0;
        float frame_t = 0.0f;
        bool need_recreate = false;
        render::DrawStats last_stats;
        render::DrawTiming last_timing;
        Averages avg;
        uint64_t last_tick = UINT64_MAX;
        int last_mx = 0, last_my = 0;
        long iterations = 0, minimized_iterations = 0, recreates = 0;
        const render::DrawMode mode = opt.unlit ? render::DrawMode::Unlit : render::DrawMode::Lit;
        while (ok && win.pump()) {
            ++iterations;
#ifdef _WIN32
            if (opt.exercise_window) {
                // Scripted: shrink at frame 20, minimise at frame 40 (restored after 15 idle loops),
                // enlarge at frame 60. Exercises recreate + depth/pipeline rebuild + the minimised path.
                HWND hw = static_cast<HWND>(win.native_handle());
                static int stage = 0;
                if (stage == 0 && frames >= 20) { SetWindowPos(hw, nullptr, 0, 0, 640, 360, SWP_NOMOVE | SWP_NOZORDER); stage = 1; }
                else if (stage == 1 && frames >= 40) { ShowWindow(hw, SW_MINIMIZE); stage = 2; }
                else if (stage == 2 && minimized_iterations >= 15) { ShowWindow(hw, SW_RESTORE); stage = 3; }
                else if (stage == 3 && frames >= 60) { SetWindowPos(hw, nullptr, 0, 0, 1024, 768, SWP_NOMOVE | SWP_NOZORDER); stage = 4; }
            }
#endif
            if (win.key_down(0x1B)) break;  // VK_ESCAPE
            if (opt.frames >= 0 && frames >= opt.frames) break;
            if (win.take_resized()) need_recreate = true;
            if (need_recreate) {
                ring.wait_idle();
                sc.recreate(win.width(), win.height());
                ++recreates;
                if (sc.valid() && opt.clear && opt.frame_graph && !renderer.set_color_format(sc.format())) break;
                if (sc.valid() && !opt.clear) {
                    ok = depth.create(alloc, sc.extent().width, sc.extent().height) && renderer.set_color_format(sc.format());
                    if (!ok) break;
                }
                need_recreate = false;
            }
            if (!sc.valid() || win.minimized()) {  // minimised: nothing to present
                ++minimized_iterations;
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                continue;
            }

            const auto tf0 = Clock::now();
            rhi::Frame* f = ring.begin_frame();
            if (!f) break;
            double gpu_ms = -1.0, gpu_world = 0, gpu_scene = 0, gpu_post = 0, gpu_ao = 0;
            uint32_t image = 0;
            const rhi::SwapchainStatus as = sc.acquire(f->acquire_semaphore(), image);
            if (as == rhi::SwapchainStatus::OutOfDate || as == rhi::SwapchainStatus::Minimized ||
                as == rhi::SwapchainStatus::Error) {
                ring.end_frame(*f);
                need_recreate = true;
                if (as == rhi::SwapchainStatus::Error) break;
                continue;
            }
            const rhi::DeviceFns& fn = ctx.vkd();
            VkCommandBuffer cmd = f->cmd();
            const rhi::AcquireResult acq = up.record_acquires(cmd);
            uint32_t gpu_scope = 0;
            if (have_gpu_timer) {
                gpu.begin_frame(cmd, f->slot());  // collects this slot's previous frame
                for (const rhi::GpuTimerResult& r : gpu.results()) {
                    if (r.name == "frame") gpu_ms = r.ms;
                    if (r.name == "world") gpu_world = r.ms;
                    if (r.name == "scene") gpu_scene = r.ms;
                    if (r.name == "post") gpu_post = r.ms;
                    if (r.name == "ao") gpu_ao = r.ms;
                }
                gpu_scope = gpu.begin_scope(cmd, "frame");
            }
            const float last_t = frame_t;
            const float t = std::chrono::duration<float>(Clock::now() - t0).count();
            frame_t = t;
            if (!opt.clear) renderer.set_time(t * opt.time_scale);

            render::PassTarget pt;
            pt.color = sc.image(image);
            pt.color_view = sc.view(image);
            pt.extent = sc.extent();
            pt.color_after = rhi::ImageState::Present;
            bool all_ready = false;
            if (opt.clear) {
                pt.clear[0] = 0.5f + 0.5f * std::sin(t * 1.3f);
                pt.clear[1] = 0.5f + 0.5f * std::sin(t * 1.7f + 2.0f);
                pt.clear[2] = 0.5f + 0.5f * std::sin(t * 2.3f + 4.0f);
            } else {
                pt.depth = depth.image.handle;
                pt.depth_view = depth.image.view;
                const bool env_clear = !opt.env.empty() || !opt.map.empty();  // the environment's clear colour (TEE mini 15)
                pt.clear[0] = env_clear ? renderer.lighting().clear_color.x : 0.02f;
                pt.clear[1] = env_clear ? renderer.lighting().clear_color.y : 0.025f;
                pt.clear[2] = env_clear ? renderer.lighting().clear_color.z : 0.05f;
                if (scene) {  // land: TEE 15; space: black (SPACE_AND_OPEN.md 0 item 3)
                    const render::Float3 c = scene->clear_color();
                    pt.clear[0] = c.x;
                    pt.clear[1] = c.y;
                    pt.clear[2] = c.z;
                }
                streamer->update();
                all_ready = true;
                bool all_finished = true;
                for (uint32_t h : handles) {
                    const auto st = streamer->state(h);
                    all_ready = all_ready && st == render::ModelStreamer::State::Ready;
                    all_finished = all_finished && (st == render::ModelStreamer::State::Ready ||
                                                    st == render::ModelStreamer::State::Failed);
                    if (st == render::ModelStreamer::State::Failed && !opt.fleet) {
                        std::fprintf(stderr, "model %s failed: %s\n", opt.model.c_str(), streamer->error(h).c_str());
                        exit_code = 1;
                        ok = false;
                    }
                }
                if (scene) {
                    scene->update(renderer, *streamer);
                    // Background idle animations (planet spin) advance on the 30 Hz tick (SPACE_AND_OPEN.md D.7 4).
                    scene->set_sim_tick(static_cast<uint64_t>(std::floor(static_cast<double>(std::max(0.0f, t)) * 30.0)));
                    scene->world().update(std::max(0.0f, t - last_t) * opt.time_scale);
                    instances.assign(scene->instances().begin(), scene->instances().end());
                    // --no-sky drops only the skydomes (set_sky_enabled); space background placements stay.
                    sky_instances.assign(scene->sky().begin(), scene->sky().end());
                    all_ready = scene->ready();
                    if (!framed) {
                        cam = scene->tactical_camera(static_cast<float>(pt.extent.width) / static_cast<float>(pt.extent.height),
                                                     opt.camera_distance);
                        framed = true;
                        if (opt.fow) {  // synthetic visibility: a revealed disc around the start camera target
                            scene->reveal_circle(cam.target.x, cam.target.y, scene->space() ? 1500.0f : 300.0f);
                            if (!scene->apply_fog_of_war()) std::fprintf(stderr, "--fow: no fog-of-war grid for this map\n");
                        }
                        if (opt.dynamic_track_demo && !scene->space() && !scene->dynamic_track_params().empty()) {
                            const auto& fxs = scene->dynamic_track_params();
                            const terrain::DynamicTrackParams* pick = &fxs.front();
                            for (const auto& p : fxs)
                                if (p.mode == terrain::RenderMode::DiffuseAlpha) {
                                    pick = &p;
                                    break;
                                }
                            demo_tracks.emplace_back(*pick);
                            demo_center = cam.target;
                            std::printf("dynamic track demo: %s (%s)\n", pick->name.c_str(), pick->texture.c_str());
                        }
                    }
                    if (!demo_tracks.empty()) {  // head on a 150-unit circle at 60 units/s, blob under the head
                        const float dt = std::max(0.0f, t - last_t);
                        const float a = 0.4f * t;
                        const float hx = demo_center.x + 150.0f * std::cos(a), hy = demo_center.y + 150.0f * std::sin(a);
                        demo_tracks[0].update(hx, hy, dt);
                        scene->set_dynamic_tracks(demo_tracks);
                        terrain::ShadowBlob b;
                        b.position[0] = hx;
                        b.position[1] = hy;
                        b.width = b.height = 60.0f;
                        scene->set_shadow_blobs(std::span<const terrain::ShadowBlob>(&b, 1));
                    }
                } else if (opt.fleet) {
                    all_ready = all_finished;  // failed fleet types are simply not instanced
                    if (all_finished && instances.empty()) {
                        for (const render::FleetInstance& fi : fleet) {
                            render::GpuModel* m = streamer->model(handles[fi.model]);
                            if (!m || streamer->state(handles[fi.model]) != render::ModelStreamer::State::Ready) continue;
                            render::Instance in;
                            in.model = m;
                            in.transform = fi.transform;
                            in.team_color_override = true;
                            in.team_color = fi.team;
                            instances.push_back(in);
                        }
                        cam.frame({0, 0, 0}, render::fleet_radius(opt.fleet),
                                  static_cast<float>(pt.extent.width) / static_cast<float>(pt.extent.height));
                        cam.pitch = 0.6f;
                        framed = true;
                    }
                } else {
                    render::GpuModel* gm = streamer->model(handles[0]);
                    if (gm && gm->asset && !framed) {
                        const assets::Sphere s = gm->drawn_sphere();
                        cam.frame(s.center, s.radius, static_cast<float>(pt.extent.width) / static_cast<float>(pt.extent.height));
                        framed = true;
                    }
                    if (gm) {
                        if (instances.empty()) instances.resize(1);
                        instances[0].model = gm;
                        instances[0].bone_world = bone_world;
                    }
                    if (gm && gm->asset && !opt.type.empty()) {  // --type: hardpoint state + Model_To_Attach (B.10)
                        if (!type_vis.ready()) {
                            type_vis.reset(type_info.hardpoints.size());
                            type_vis.init(std::make_shared<const render::HardpointLayout>(
                                render::build_hardpoint_layout(*gm->asset, type_info.hardpoint_defs, opt.show_damage)));
                            const std::vector<size_t> hl = destroy_list(opt.destroy_hp, type_info, true);
                            for (size_t h : hl) type_vis.set_destroyed(h, true);
                            if (!opt.destroy_hp.empty()) std::printf("--destroy-hardpoints %s: %zu destroyed\n", opt.destroy_hp.c_str(), hl.size());
                        }
                        instances.resize(1);
                        instances[0].mesh_hidden = type_vis.mask();
                        const render::HardpointLayout* lay = type_vis.layout();
                        for (uint32_t h = 0; lay && h < lay->hardpoints && h < attach_handles.size(); ++h) {
                            const uint32_t ah = attach_handles[h];
                            if (ah == UINT32_MAX || streamer->state(ah) != render::ModelStreamer::State::Ready || !type_vis.attachment_visible(h))
                                continue;
                            render::Instance a;
                            a.model = streamer->model(ah);
                            a.transform = render::attachment_transform(*gm->asset, instances[0].transform, bone_world, lay->attach_bone[h]);
                            instances.push_back(a);  // follows the owner's bone each frame (posed with --anim)
                        }
                    }
                }
                if (all_ready && !reported) {
                    reported = true;
                    t_ready = Clock::now();
                    if (scene) {
                        const render::MapSceneStats& ms = scene->stats();
                        std::printf("map ready after %.0f ms: %u instances of %u models (%u ready, %u failed), world %u textures\n",
                                    std::chrono::duration<double, std::milli>(t_ready - t0).count(), ms.instances, ms.distinct_models,
                                    ms.models_ready, ms.models_failed, scene->world().stats().textures);
                    } else if (opt.fleet) {
                        std::printf("fleet ready after %.0f ms: %zu instances of %zu model types\n",
                                    std::chrono::duration<double, std::milli>(t_ready - t0).count(), instances.size(),
                                    handles.size());
                    } else {
                        render::GpuModel* gm = streamer->model(handles[0]);
                        const assets::ModelAsset& ma = *gm->asset;
                        uint32_t fallbacks = 0;
                        for (const auto& tx : gm->textures) fallbacks += tx.fallback ? 1u : 0u;
                        std::printf("model ready after %.0f ms (job %.0f ms): %zu bones, %zu meshes, %zu submeshes, max LOD %d, "
                                    "%zu uploaded, %u damage meshes hidden, %zu textures (%u fallback), %.1f MB uploaded\n",
                                    std::chrono::duration<double, std::milli>(t_ready - t0).count(), streamer->cpu_ms(handles[0]),
                                    ma.skeleton.size(), ma.meshes.size(), ma.submeshes.size(), gm->max_lod, gm->submeshes.size(),
                                    gm->damage_meshes_hidden, gm->textures.size(), fallbacks,
                                    static_cast<double>(gm->upload_bytes) / (1024.0 * 1024.0));
                        static const char* const kLit[] = {"lit", "no effect", "untranslated", "layout mismatch", "not drawn"};
                        for (const auto& g : gm->submeshes) {
                            const auto& sm = ma.submeshes[g.submesh];
                            const std::string bucket = g.effect ? std::string(", ") + render::phase_bucket_name(g.bucket) : std::string();
                            std::printf("  submesh %2u %-24s %-21s %-22s %5u tris, LOD %d, %s, %zu pass%s%s%s\n", g.submesh,
                                        ma.meshes[sm.mesh].name.c_str(), sm.vertex_format.c_str(), sm.material.effect.c_str(),
                                        g.index_count / 3, g.lod, kLit[static_cast<int>(g.lit)], g.passes.size(),
                                        g.passes.size() == 1 ? "" : "es", bucket.c_str(), g.skinned ? ", skinned" : "");
                        }
                    }
                }
                // Mouse orbit / zoom (rhi Window mouse API).
                const int mx = win.mouse_x(), my = win.mouse_y();
                if (win.mouse_down(rhi::MouseButton::Left))
                    cam.orbit(-0.008f * static_cast<float>(mx - last_mx), 0.008f * static_cast<float>(my - last_my));
                last_mx = mx;
                last_my = my;
                const float wheel = win.take_wheel();
                if (wheel != 0) cam.zoom(wheel);
                // Animation on the 30 Hz tick.
                render::GpuModel* gm0 = (opt.fleet || scene) ? nullptr : streamer->model(handles[0]);
                if (clip && gm0 && gm0->asset && gm0->asset->skeleton.size()) {
                    const uint64_t tick = static_cast<uint64_t>(std::floor(static_cast<double>(t) * 30.0));
                    if (tick != last_tick) {
                        last_tick = tick;
                        const assets::Skeleton& sk = gm0->asset->skeleton;
                        assets::sample(sk, *clip, static_cast<float>(static_cast<double>(tick) / 30.0), true, pose);
                        bone_world.resize(sk.size());
                        assets::compute_world(sk, pose.local, bone_world);
                        if (!instances.empty()) instances[0].bone_world = bone_world;
                    }
                    if (!anim_reported) {
                        anim_reported = true;
                        std::printf("animation %s: %u frames at %.1f fps, %zu tracks\n", clip->name.c_str(), clip->frame_count,
                                    clip->fps, clip->tracks.size());
                    }
                } else if (anim_done && !clip && !anim_reported) {
                    anim_reported = true;
                    std::fprintf(stderr, "animation %s failed to load\n", opt.anim.c_str());
                    exit_code = 1;
                }
            }

            if (opt.clear && opt.frame_graph && renderer.world() == nullptr && sc.valid()) {
                // Empty scene through the frame graph: renderer targets, StretchRect into the swapchain image.
                render::OrbitCamera ec;
                const render::ViewMatrices vm = render::make_view(ec, pt.extent.width, pt.extent.height);
                render::RecordOptions eo;
                if (!renderer.render(*f, cmd, pt, vm, {}, render::DrawMode::Lit, eo, &last_stats, &last_timing)) {
                    exit_code = 1;
                    ok = false;
                }
            } else if (opt.clear || instances.empty()) {
                render::begin_pass(fn, cmd, pt);
                render::end_pass(fn, cmd, pt);
            } else {
                const render::ViewMatrices vm = scene ? render::make_view(cam, pt.extent.width, pt.extent.height, render::MapScene::kNear,
                                                                          render::MapScene::kFar)
                                                      : render::make_view(cam, pt.extent.width, pt.extent.height);
                if (!scene && sky_instances.size() < sky_handles.size()) {
                    sky_instances.clear();
                    bool all = true;
                    for (const auto& [h, k] : sky_handles) {
                        render::GpuModel* m = streamer->model(h);
                        if (!m || streamer->state(h) != render::ModelStreamer::State::Ready) {
                            all = all && streamer->state(h) == render::ModelStreamer::State::Failed;
                            continue;
                        }
                        render::Instance si;
                        si.model = m;
                        si.transform = render::skydome_transform(renderer.lighting(), k);
                        sky_instances.push_back(si);
                    }
                    if (!all && sky_instances.size() < sky_handles.size()) sky_instances.clear();  // wait for all
                    if (all && sky_instances.empty()) sky_handles.clear();
                }
                if (!renderer.render(*f, cmd, pt, vm, instances, mode, ropt, &last_stats, &last_timing, sky_instances)) {
                    exit_code = 1;
                    ok = false;
                }
                const bool world_done = !scene || (scene->world().stats().terrain_pending == 0 && scene->world().stats().water_pending == 0 &&
                                                   scene->world().stats().decorations.pending == 0 && last_stats.translation_pending == 0);
                if (reported && !all_lit_reported && last_stats.pending_pipeline == 0 && world_done) {
                    all_lit_reported = true;
                    std::printf("all pipelines ready after %.0f ms: %u draws (%u hot-set, %u FF, %u unlit), %u materials, "
                                "%u instances (%u culled), %u secondaries\n",
                                std::chrono::duration<double, std::milli>(Clock::now() - t0).count(), last_stats.draws,
                                last_stats.lit_draws, last_stats.ff_draws, last_stats.unlit_draws, last_stats.materials,
                                last_stats.instances, last_stats.culled, last_timing.chunks);
                }
            }
            if (have_gpu_timer) gpu.end_scope(cmd, gpu_scope);

            const auto ts0 = Clock::now();
            VkSemaphoreSubmitInfo waits[2];
            uint32_t nw = 0;
            // The first swapchain write is either the colour pass (begin_pass) or the frame graph's transfer
            // (layout transition + StretchRect copy): the acquire must block both.
            waits[nw++] = rhi::semaphore_info(f->acquire_semaphore(), 0,
                                              VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
            if (acq.wait_value) waits[nw++] = rhi::semaphore_info(up.timeline(), acq.wait_value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
            const VkSemaphoreSubmitInfo signal =
                rhi::semaphore_info(sc.render_finished(image), 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);  // after the present transition
            rhi::FrameSubmit fs;
            fs.waits = {waits, nw};
            fs.signals = {&signal, 1};
            if (!ring.end_frame(*f, fs)) break;
            const rhi::SwapchainStatus ps = sc.present(image);
            const auto ts1 = Clock::now();
            if (ps == rhi::SwapchainStatus::OutOfDate || ps == rhi::SwapchainStatus::Suboptimal ||
                as == rhi::SwapchainStatus::Suboptimal)
                need_recreate = true;
            else if (ps == rhi::SwapchainStatus::Error)
                break;
            ++frames;
            if (reported) ++frames_since_ready;
            // Steady-state averages: everything resident and every pipeline compiled, skip 10 warm-up frames.
            if (all_ready && all_lit_reported && frames_since_ready > 10) {
                avg.build += last_timing.build_ms;
                avg.cull += last_timing.cull_ms;
                avg.sort += last_timing.sort_ms;
                avg.fill += last_timing.fill_ms;
                avg.record += last_timing.record_ms;
                avg.submit += std::chrono::duration<double, std::milli>(ts1 - ts0).count();
                avg.frame += std::chrono::duration<double, std::milli>(ts1 - tf0).count();
                ++avg.n;
                avg.world += last_timing.world_ms;
                avg.post += last_timing.post_ms;
                if (gpu_ms >= 0) {
                    avg.gpu += gpu_ms;
                    avg.gpu_world += gpu_world;
                    avg.gpu_scene += gpu_scene;
                    avg.gpu_post += gpu_post;
                    avg.gpu_ao += gpu_ao;
                    ++avg.gpu_n;
                }
            }
            if (frames % 60 == 0) {
                char title[256];
                std::snprintf(title, sizeof title, "alamo_render %s - %u draws, %llu tris - %.1f fps (Esc quits)",
                              opt.clear ? "--clear" : (opt.fleet ? "--fleet" : opt.model.c_str()), last_stats.draws,
                              static_cast<unsigned long long>(last_stats.triangles),
                              static_cast<double>(frames) / std::max(1e-3, static_cast<double>(t)));
                win.set_title(title);
            }
        }
        const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
        if (opt.clear && opt.frames >= 0 && frames < opt.frames) {  // smoke tests: every requested frame presented
            std::fprintf(stderr, "only %ld of %ld frames were presented\n", frames, opt.frames);
            exit_code = 1;
        }
        std::printf("%ld frames in %.2f s; last frame %u draws (%u hot-set, %u FF, %u unlit, %u pending), %llu triangles, "
                    "%u materials, %u instances (%u culled), %u secondaries\n",
                    frames, secs, last_stats.draws, last_stats.lit_draws, last_stats.ff_draws, last_stats.unlit_draws,
                    last_stats.pending_pipeline, static_cast<unsigned long long>(last_stats.triangles), last_stats.materials,
                    last_stats.instances, last_stats.culled, last_timing.chunks);
        std::printf("last frame: %u mesh draw calls for %u draws (%u instanced calls)\n", last_stats.draw_calls, last_stats.draws,
                    last_stats.instanced_calls);
        std::printf("last frame: %u translated, %u shadow volumes (darken %s%s), %u heat draws (composite %s), bloom %s, %u sky draws, "
                    "%u post passes pending\n",
                    last_stats.translated_draws, last_stats.shadow_volumes, last_stats.darkened ? "yes" : "no",
                    last_stats.soft_shadows ? ", soft" : "", last_stats.heat_draws, last_stats.heat ? "yes" : "no",
                    last_stats.bloom ? "yes" : "no", last_stats.sky_draws, last_stats.post_pending);
        if (scene) {
            const render::WorldStats& w = scene->world().stats();
            const render::WorldBindStats& wb = w.binds;
            std::printf("map world draws (%s): %u draws, %u pipeline binds, %u set binds, %u vertex/index binds, %u push, %u dynamic-state "
                        "groups, %u set allocs, %u set writes, %u uniform allocs\n",
                        opt.no_batch ? "unbatched" : "batched", wb.draws, wb.pipeline_binds, wb.set_binds, wb.buffer_binds,
                        wb.push_constants, wb.dynamic_state, wb.set_allocs, wb.set_writes, wb.uniform_allocs);
            std::printf("map last frame: %u instances, %u visible patches of %u (%u terrain draws, %u pending, %llu triangles), water %u "
                        "draws (%u pending), reflection %s (%u draws), refraction %s (%u draws), sky cube %s\n",
                        last_stats.instances, w.visible_patches, w.patches, w.terrain_draws, w.terrain_pending,
                        static_cast<unsigned long long>(w.terrain_triangles), w.water_draws, w.water_pending, w.reflection ? "yes" : "no",
                        w.reflection_draws, w.refraction ? "yes" : "no", w.refraction_draws, w.sky_cube_rendered ? "re-rendered" : "kept");
            const render::DecorationStats& d = w.decorations;
            std::printf("map decorations last frame: %u land tracks, %u water tracks, %u dynamic, %u decals, %u blobs -> %u draws (%u FF, "
                        "%u translated, %u FOW patches, %u water-track, %u space FOW), %u pipeline binds, %llu tris, %u pending; fog of war %s\n",
                        d.land_tracks, d.water_tracks, d.dynamic_tracks, d.decals, d.blobs, d.draws, d.ff_draws, d.translated_draws,
                        d.fow_draws, d.water_track_draws, d.space_fow_draws, d.pipeline_binds, static_cast<unsigned long long>(d.triangles),
                        d.pending, d.fow ? "on" : "off");
        }
        if (avg.n) {
            const double n = static_cast<double>(avg.n);
            std::printf("timing over %ld steady frames: frame %.3f ms (build %.3f [cull/LOD/keys %.3f, sort %.3f, records %.3f], "
                        "record %.3f, submit+present %.3f), GPU %.3f ms\n",
                        avg.n, avg.frame / n, avg.build / n, avg.cull / n, avg.sort / n, avg.fill / n, avg.record / n, avg.submit / n,
                        avg.gpu_n ? avg.gpu / static_cast<double>(avg.gpu_n) : -1.0);
            const double gn = avg.gpu_n ? static_cast<double>(avg.gpu_n) : 1.0;
            std::printf("sections: world prepass CPU %.3f / GPU %.3f ms, scene GPU %.3f ms, post chain CPU %.3f / GPU %.3f ms\n",
                        avg.world / n, avg.gpu_world / gn, avg.gpu_scene / gn, avg.post / n, avg.gpu_post / gn);
            if (opt.ao) std::printf("ambient occlusion GPU %.3f ms (part of scene)\n", avg.gpu_ao / gn);
        }
        std::printf("window: %ld loop iterations, %ld while minimised, %ld swapchain recreates, final %ux%u\n", iterations,
                    minimized_iterations, recreates, sc.extent().width, sc.extent().height);
        if (!opt.clear) {
            const auto ps = renderer.pipelines().stats();
            std::printf("pipelines: %u keys (%u ready, %u failed, %u pending), %.1f ms summed creation; VkPipelineCache %u hits, "
                        "%u misses, %u without feedback; bindless %u live, %u pending, high water %u\n",
                        ps.keys, ps.ready, ps.failed, ps.pending, ps.create_ms, ps.cache_hits, ps.cache_misses, ps.feedback_unknown,
                        renderer.textures_resident(), renderer.texture_slots_pending(), renderer.texture_slot_high_water());
            if (!reported) {
                std::fprintf(stderr, "model was not resident before exit\n");
                if (opt.frames >= 0) exit_code = 1;
            }
        }

        ring.wait_idle();
        ctx.wait_idle();
        gpu.shutdown();
        if (js) js->wait(anim_counter);
        if (scene) scene->stop(renderer);
        if (streamer) streamer->destroy_all();
        streamer.reset();
        renderer.shutdown();  // saves the pipeline cache
        cache.reset();
        effects.reset();
        if (depth.image) alloc.destroy(depth.image);
        ring.shutdown();
        sc.shutdown();
        up.shutdown();
        alloc.shutdown();
        js.reset();
    }
    const uint32_t errors = ctx.validation_errors(), warnings = ctx.validation_warnings();
    std::printf("validation: %s, %u errors, %u warnings\n", ctx.validation_enabled() ? "on" : "off", errors, warnings);
    ctx.shutdown();
    win.destroy();
    if (errors || warnings) exit_code = 1;
    return exit_code;
}