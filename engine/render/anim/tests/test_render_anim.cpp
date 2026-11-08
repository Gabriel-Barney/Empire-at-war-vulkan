// alamo_render_anim tests (R3 render side). Needs ALAMO_FOC_DATA (real models and clips); skipped without it.
//   * skeleton consistency: alamo_anim's skeleton (formats ALO parse) == alamo_assets' (the renderer's inverse
//     bind palettes) in bone count, parents and bind-local bits, for every land model with clips that is used;
//   * SimAnimDriver over real types: 48 objects (half of them moving), 240 ticks: digests and every pose
//     identical for serial and 4 / 15 job workers; poses change over time; move and idle plays happen;
//   * GPU (alamo_render built with the R1 sim view): alamo_render --sim-headless-check with unit animation on
//     for a land and a space map: per-tick CRC rendering on == off, 0 validation messages, PNGs in logs/.
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "alamo/assets/model.hpp"
#include "alamo/render/anim/anim_pose_cache.hpp"
#include "alamo/render/anim/sim_anim_driver.hpp"
#include "alamo/test/support.hpp"
#ifdef ALAMO_RENDER_ANIM_HAVE_JOBS
#include "alamo/jobs/job_system.hpp"
#endif
#ifdef ALAMO_RENDER_ANIM_HAVE_VFS
#include "alamo/vfs/vfs.hpp"
#endif

namespace {

using ::alamo::test::g_fail;

using namespace alamo;

#ifdef ALAMO_RENDER_ANIM_HAVE_VFS
struct RunResult {
    std::vector<uint64_t> digests;
    std::vector<std::vector<uint8_t>> poses;  // per tick, all objects' pose bytes concatenated
    uint32_t idle = 0, move = 0, posed = 0;
    bool cache_ok = true;
};

RunResult run_driver(const FileProvider& files, const objects::GameObjectTypes& types, const std::vector<objects::TypeIndex>& picked,
                     const ParallelFor& pf) {
    RunResult out;
    render::SimAnimDriver d(files, types, objects::ModeKind::Land);
    std::vector<render::AnimDriverObject> objs;
    for (size_t i = 0; i < 48; ++i) objs.push_back({static_cast<objects::ObjectId>(10 + i * 3), picked[i % picked.size()], {}});
    for (uint64_t tick = 0; tick <= 240; ++tick) {
        for (size_t i = 0; i < objs.size(); ++i)
            if (i % 2 == 0 && (tick / 60) % 2 == 0) objs[i].position.x += 1.5f;  // moves 60 ticks, stops 60 ticks
        d.update(tick, objs, pf);
        out.digests.push_back(d.digest());
        std::vector<uint8_t> bytes;
        for (const auto& o : objs) {
            const anim::AnimModel* m = d.world().model(o.id);
            if (!m) continue;
            const auto p = d.pose(o.id, m->skeleton.size());
            const auto* b = reinterpret_cast<const uint8_t*>(p.data());
            bytes.insert(bytes.end(), b, b + p.size_bytes());
        }
        out.poses.push_back(std::move(bytes));
        if (tick % 60 == 30) {  // AnimPoseCache over the same world: equal poses, pure getter (repeat calls identical)
            render::AnimPoseCache c;
            c.update(&d.world(), pf);
            for (const auto& o : objs) {
                const anim::AnimModel* m = d.world().model(o.id);
                if (!m) continue;
                const auto p1 = c.pose(o.id, m->skeleton.size()), p2 = c.pose(o.id, m->skeleton.size());
                const auto pd = d.pose(o.id, m->skeleton.size());
                out.cache_ok = out.cache_ok && p1.data() == p2.data() && p1.size() == pd.size() &&
                               (p1.empty() || std::memcmp(p1.data(), pd.data(), p1.size_bytes()) == 0);
            }
            out.cache_ok = out.cache_ok && c.stats().posed == d.stats().posed;
        }
    }
    out.idle = d.stats().idle_plays;
    out.move = d.stats().move_plays;
    out.posed = d.stats().posed;
    return out;
}

int run_app(const std::string& args) {
#if defined(ALAMO_RENDER_APP) && defined(ALAMO_RENDER_APP_HAS_SIM)
    std::string cmd = std::string("\"") + ALAMO_RENDER_APP + "\" " + args;
#ifdef _WIN32
    cmd = "\"" + cmd + "\"";  // cmd.exe strips the outer quotes
#endif
    std::printf("running: %s\n", cmd.c_str());
    std::fflush(stdout);
    return std::system(cmd.c_str());
#else
    (void)args;
    return -1;
#endif
}

void test_real_data() {
    const char* root = std::getenv("ALAMO_FOC_DATA");
    if (!root || !*root) {
        std::printf("real data: ALAMO_FOC_DATA not set, skipped\n");
        return;
    }
    vfs::VfsConfig cfg;
    cfg.foc_root = root;
    std::string err;
    auto files = vfs::Vfs::create(cfg, &err);
    if (!files) {
        std::printf("FAIL VFS: %s\n", err.c_str());
        ++g_fail;
        return;
    }
    objects::ObjectTypeDatabase db;
    objects::GameObjectTypes types;
    if (!db.load(*files) || !types.build(db, files.get())) {
        std::printf("FAIL object types\n");
        ++g_fail;
        return;
    }
    // Land types with a bound idle clip and a move clip (infantry / vehicles), first 12 distinct models in type order.
    std::vector<objects::TypeIndex> picked;
    std::vector<std::string> picked_models;
    anim::ClipCache cache;
    for (objects::TypeIndex t = 0; t < types.size() && picked.size() < 12; ++t) {
        const anim::TypeAnimParams p = anim::type_anim_params(types, t, objects::ModeKind::Land);
        if (p.model.empty()) continue;
        bool dup = false;
        for (const auto& s : picked_models) dup = dup || s == p.model;
        if (dup) continue;
        anim::AnimModel m;
        if (!anim::load_anim_model(*files, p.model, p.anim_override, m, &cache)) continue;
        const anim::ClipRef* idle = m.clip(anim::kAnimIdle, 0);
        if (!idle || !idle->bound || m.variant_count(anim::kAnimMove) == 0) continue;
        // Skeleton consistency with the renderer's asset.
        assets::ModelAsset asset;
        if (!assets::load_model(*files, p.model, asset)) continue;
        const assets::Skeleton& s = asset.skeleton;
        bool same = s.size() == m.skeleton.size();
        for (size_t b = 0; same && b < s.size(); ++b)
            same = s.parents[b] == m.skeleton.parents[b] && std::memcmp(&s.bind_local[b], &m.skeleton.bind_local[b], sizeof(assets::Mat3x4)) == 0 &&
                   s.billboard_mode[b] == m.skeleton.billboard_mode[b];
        if (!same) std::printf("  skeleton mismatch anim vs assets: %s\n", p.model.c_str());
        CHECK(same);
        picked.push_back(t);
        picked_models.push_back(p.model);
    }
    std::printf("real data: %zu animated land types picked:", picked.size());
    for (const auto& s : picked_models) std::printf(" %s", s.c_str());
    std::printf("\n");
    CHECK(picked.size() >= 4);
    if (picked.empty()) return;

    const RunResult ref = run_driver(*files, types, picked, serial_for);
    size_t changes = 0;
    for (size_t i = 1; i < ref.poses.size(); ++i) changes += ref.poses[i] != ref.poses[i - 1] ? 1u : 0u;
    std::printf("driver serial: %u posed objects, %u idle plays, %u move plays, pose changed on %zu of %zu ticks, final digest %016" PRIx64 "\n", ref.posed,
                ref.idle, ref.move, changes, ref.poses.size() - 1, ref.digests.back());
    CHECK(ref.posed > 0 && ref.move > 0 && ref.idle > 0 && changes > 200);
    std::printf("AnimPoseCache over the driver's world: %s\n", ref.cache_ok ? "same poses, pure getter" : "DIFFERS");
    CHECK(ref.cache_ok);
#ifdef ALAMO_RENDER_ANIM_HAVE_JOBS
    for (int32_t workers : {4, 15}) {
        jobs::JobSystem js(jobs::JobSystemConfig{workers});
        const RunResult r = run_driver(*files, types, picked, jobs::make_parallel_for(js));
        const bool same = r.digests == ref.digests && r.poses == ref.poses;
        std::printf("driver %2d workers: digests + poses %s\n", workers, same ? "identical" : "DIFFER");
        CHECK(same);
    }
#endif

    // GPU: the render app with unit animation on (CRC on == off, 0 validation messages, PNG).
#if defined(ALAMO_RENDER_APP) && defined(ALAMO_RENDER_APP_HAS_SIM)
    const std::string logs = ALAMO_RENDER_ANIM_LOG_DIR;
    int rc = run_app("--sim-headless-check --map _LAND_PLANET_BYSS_KIDNAPPING --ticks 90 --png \"" + logs + "/render_anim_byss_land.png\"");
    std::printf("alamo_render land check exit %d\n", rc);
    CHECK(rc == 0);
    rc = run_app("--sim-headless-check --map _SPACE_PLANET_CORELLIA_PIRACY --ticks 90 --png \"" + logs + "/render_anim_corellia_space.png\"");
    std::printf("alamo_render space check exit %d\n", rc);
    CHECK(rc == 0);
    // Close-up of an animated unit, animated vs bind pose at the same tick: the images must differ (the pose
    // reaches the GPU skinning path).
    const std::string anim_png = logs + "/render_anim_byss_closeup.png", bind_png = logs + "/render_anim_byss_closeup_bindpose.png";
    rc = run_app("--sim-headless-check --map _LAND_PLANET_BYSS_KIDNAPPING --ticks 75 --anim-focus 90 --png \"" + anim_png + "\"");
    CHECK(rc == 0);
    rc = run_app("--sim-headless-check --map _LAND_PLANET_BYSS_KIDNAPPING --ticks 75 --anim-focus 90 --no-anim --png \"" + bind_png + "\"");
    CHECK(rc == 0);
    std::vector<uint8_t> a, b;
    CHECK(read_host_file(anim_png, a) && read_host_file(bind_png, b) && !a.empty() && !b.empty());
    std::printf("close-up animated vs bind pose PNGs: %s\n", a != b ? "differ" : "IDENTICAL");
    CHECK(a != b);
    // Sim-owned animation (the default above) with the opt-in "anim" CRC system + particles, and the fallback driver for
    // hosts without a SimAnimation: per-tick CRC rendering on == off in both, 0 validation messages.
    rc = run_app("--sim-headless-check --map _LAND_PLANET_BYSS_KIDNAPPING --ticks 90 --anim-crc --expect-particles --anim-focus 160 --png \"" + logs +
                 "/render_sim_anim_simowned_byss.png\"");
    std::printf("alamo_render land check (sim-owned anim, \"anim\" CRC) exit %d\n", rc);
    CHECK(rc == 0);
    rc = run_app("--sim-headless-check --map _LAND_PLANET_BYSS_KIDNAPPING --ticks 60 --no-sim-anim --expect-particles");
    std::printf("alamo_render land check (fallback driver) exit %d\n", rc);
    CHECK(rc == 0);
#else
    std::printf("GPU: alamo_render without the R1 sim view, skipped\n");
#endif
}
#endif

}  // namespace

int main() {
#ifdef ALAMO_RENDER_ANIM_HAVE_VFS
    test_real_data();
#else
    std::printf("engine/vfs not configured, skipped\n");
#endif
    if (g_fail) {
        std::printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    std::printf("all render anim tests passed\n");
    return 0;
}
