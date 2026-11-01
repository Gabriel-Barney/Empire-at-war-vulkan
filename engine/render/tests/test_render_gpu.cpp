// test_render: tests split from test_render.cpp (PAF t08).
#include "test_render_common.hpp"

namespace test_render_t {

int run_gpu(const std::string& src_dir) {
    const bool strict = render::setup_validation_env(src_dir);
    {
        rhi::Loader probe;
        if (!probe.open()) {
            std::printf("SKIP gpu: no Vulkan loader\n");
            return 0;
        }
    }
    Gpu g;
    rhi::ContextDesc cd;
    cd.app_name = "alamo_render_tests";
    cd.validation = strict ? rhi::Validation::Required : rhi::Validation::IfAvailable;
    if (!g.ctx.init(cd)) {
        if (strict) {
            rhi::ContextDesc any = cd;
            any.validation = rhi::Validation::Off;
            rhi::Context c2;
            if (c2.init(any)) {
                std::printf("FAIL gpu: the validation layer is present but could not be enabled\n");
                c2.shutdown();
                return 1;
            }
        }
        std::printf("SKIP gpu: no suitable Vulkan 1.3 device\n");
        return 0;
    }
    std::printf("gpu: %s, validation %s%s, transfer queue %s\n", g.ctx.caps().name.c_str(),
                g.ctx.validation_enabled() ? "on" : "off", strict ? " (required)" : "", g.ctx.has_transfer_queue() ? "yes" : "no");
    CHECK(!strict || g.ctx.validation_enabled());
    rhi::FrameRingDesc fd;
    fd.frames_in_flight = 2;
    fd.uniform_bytes = 64ull << 20;
    fd.recording_threads = jobs::JobSystem::default_worker_count() + 1;  // one pool per job-system slot
    CHECK(g.alloc.init(g.ctx) && g.up.init(g.ctx, g.alloc) && g.ring.init(g.ctx, g.alloc, fd));
    render::RendererDesc rd;
    rd.color_format = VK_FORMAT_R8G8B8A8_UNORM;
    rd.depth_format = VK_FORMAT_D32_SFLOAT;
    const bool rinit = g.renderer.init(g.ctx, g.alloc, g.up, rd);
    CHECK(rinit);

    if (rinit) {
        // ---- synthetic model, 128x128, camera on -Y at distance 5 seeing x,z in [-1, 1] at y = 0.
        render::GpuModel gm;
        std::string err;
        CHECK(g.renderer.upload_model(synthetic_model(), synthetic_texture, gm, nullptr, &err));
        // NEAR QUAD BACK DETAIL_LOD0/1 SKIN + the ALO-hidden FLASH (uploaded masked, B.10); LOD picked per draw.
        CHECK(gm.submeshes.size() == 7);
        for (const auto& s : gm.submeshes)
            CHECK(gm.mesh_hidden[s.mesh] == (gm.asset->meshes[s.mesh].hidden ? 1 : 0));
        CHECK(gm.selection.lod == 1);
        CHECK(gm.textures.size() == 3);
        for (const auto& t : gm.buffer_tickets) CHECK(g.up.wait(t));
        for (const auto& t : gm.textures) CHECK(g.up.wait(t.ticket));
        Offscreen o;
        CHECK(o.create(g, 128, 128));
        render::OrbitCamera cam;
        cam.yaw = -1.5707963f;
        cam.pitch = 0.0f;
        cam.distance = 5.0f;
        cam.radius = 2.0f;
        cam.fov_x = 2.0f * std::atan(0.2f);
        const render::ViewMatrices vm = render::make_view(cam, 128, 128);
        render::Instance inst;
        inst.model = &gm;
        render::DrawStats st;
        CHECK(render_frame(g, g.renderer, o, vm, {&inst, 1}, &gm, &st));
        CHECK(gm.resident());
        CHECK(st.draws == 5 && st.skinned_draws == 1);
        auto expect = [&](const char* what, uint32_t x, uint32_t y, int r, int gg, int b) {
            const bool okp = rgb_close(o.px(x, y), r, gg, b);
            if (!okp) print_px(what, o.px(x, y));
            CHECK(okp);
        };
        expect("quad top-left (red, also checks the Y flip)", 48, 48, 255, 0, 0);
        expect("quad top-right (green)", 80, 48, 0, 255, 0);
        expect("quad bottom-left (blue)", 48, 80, 0, 0, 255);
        expect("quad bottom-right (white)", 80, 80, 255, 255, 255);
        expect("near quad on bone 1 wins the depth test", 64, 64, 255, 0, 255);
        expect("back-facing quad culled", 112, 16, 0, 0, 64);
        expect("hidden mesh not drawn", 16, 16, 0, 0, 64);
        expect("LOD0 mesh not drawn", 16, 112, 0, 0, 64);
        expect("skinned quad in bind pose", 16, 64, 255, 255, 0);
        // LOD1 quad: missing texture -> magenta/black checker fallback x green vertex colour.
        const uint8_t* lp = o.px(112, 112);
        CHECK(lp[0] < 8 && lp[2] < 8 && lp[1] < 8);  // magenta x green = black, black x green = black
        CHECK(gm.textures.size() == 3 && gm.textures[2].fallback && !gm.textures[1].fallback);

        // ---- posed frame: bone 1 moved up by 0.6 (static NEAR follows its attach bone, SKIN its palette),
        // and the instance moved +0.2 in X (instance x bone world, instance x palette).
        std::vector<Mat3x4> bw = gm.asset->skeleton.bind_world;
        bw[1].m[2][3] += 0.6f;
        inst.bone_world = bw;
        inst.transform.m[0][3] = 0.2f;
        CHECK(render_frame(g, g.renderer, o, vm, {&inst, 1}, &gm));
        // NEAR at y=-1 (distance 4): screen z = 0.6 * 5/4 = 0.75 -> row 16; x = 0.2 * 5/4 = 0.25 -> col 80.
        expect("posed near quad", 80, 16, 255, 0, 255);
        expect("quad red texel after the instance shift", 60, 48, 255, 0, 0);
        // SKIN: x in [-0.7,-0.4] -> cols 19..38, z in [0.45,0.75] -> rows 16..35 (at y=0, distance 5).
        expect("posed skinned quad", 28, 26, 255, 255, 0);
        expect("skinned quad left its bind position", 16, 64, 0, 0, 64);

        // ---- [port] SSAO (RecordOptions::ambient_occlusion): off frames stay byte-identical around an AO frame; AO
        // only darkens (never brightens), leaves the empty background alone, and darkens the back quad where the near
        // copy (0.5 units in front) is within the radius; a second target size recreates the half-res targets. Camera at
        // distance 15 so the 1.2-unit radius stays under the 1/4-screen disc clamp.
        {
            // Two copies, the second 0.5 units nearer and 1 unit to the right: its edge stands over the first one.
            render::Instance ai[2];
            ai[0].model = ai[1].model = &gm;
            ai[1].transform.m[0][3] = 1.0f;
            ai[1].transform.m[1][3] = -0.5f;
            render::OrbitCamera far_cam = cam;
            far_cam.distance = 15.0f;
            const render::ViewMatrices fvm = render::make_view(far_cam, 128, 128);
            CHECK(render_frame(g, g.renderer, o, fvm, ai, &gm));
            const std::vector<uint8_t> off(o.px(0, 0), o.px(0, 0) + 128 * 128 * 4);
            render::RecordOptions ao;
            ao.ambient_occlusion = true;
            ao.ao_strength = 1.0f;
            ao.ao_radius = 1.2f;
            CHECK(render_frame(g, g.renderer, o, fvm, ai, &gm, nullptr, render::DrawMode::Lit, ao));
            const std::vector<uint8_t> on(o.px(0, 0), o.px(0, 0) + 128 * 128 * 4);
            int brighter = 0, darker = 0, bg_changed = 0;
            for (size_t i = 0; i < on.size(); i += 4) {
                const bool bg = off[i] == 0 && off[i + 1] == 0 && off[i + 2] == 64;
                for (size_t c = 0; c < 3; ++c) {
                    if (on[i + c] > off[i + c] + 1) ++brighter;
                    if (bg && on[i + c] != off[i + c]) ++bg_changed;
                }
                if (on[i] + on[i + 1] + on[i + 2] + 24 < off[i] + off[i + 1] + off[i + 2]) ++darker;
            }
            std::printf("  ssao: %d pixels darker, %d brighter channels, %d background channels changed\n", darker, brighter,
                        bg_changed);
            render::write_png_rgba(std::string(ALAMO_RENDER_SOURCE_DIR) + "/logs/render_ssao_off.png", 128, 128, off);
            render::write_png_rgba(std::string(ALAMO_RENDER_SOURCE_DIR) + "/logs/render_ssao_on.png", 128, 128, on);
            CHECK(brighter == 0 && bg_changed == 0 && darker > 20);
            CHECK(render_frame(g, g.renderer, o, fvm, ai, &gm));
            CHECK(std::memcmp(off.data(), o.px(0, 0), off.size()) == 0);
            Offscreen o2;
            CHECK(o2.create(g, 96, 72));
            CHECK(render_frame(g, g.renderer, o2, render::make_view(far_cam, 96, 72), ai, &gm, nullptr, render::DrawMode::Lit, ao));
            g.ring.wait_idle();
            o2.destroy(g);
        }

        // ---- [port] anisotropy changed at runtime: the engine samplers are rewritten by the next render() (an idle
        // wait, no validation error for the bound set); screen-aligned quads look the same; back to off.
        CHECK(g.renderer.anisotropy() == 0);
        const uint32_t dev_cap = g.ctx.caps().sampler_anisotropy ? static_cast<uint32_t>(g.ctx.caps().max_anisotropy) : 1u;
        g.renderer.set_anisotropy(16);
        CHECK(g.renderer.anisotropy() == 0);  // latched by the next draw()/render()
        CHECK(render_frame(g, g.renderer, o, vm, {&inst, 1}, &gm));
        CHECK(g.renderer.anisotropy() == render::anisotropy_level(16, dev_cap));
        expect("posed near quad (16x anisotropy)", 80, 16, 255, 0, 255);
        expect("quad red texel (16x anisotropy)", 60, 48, 255, 0, 0);
        g.renderer.set_anisotropy(0);
        CHECK(render_frame(g, g.renderer, o, vm, {&inst, 1}, &gm));
        CHECK(g.renderer.anisotropy() == 0);
        expect("quad red texel (anisotropy off again)", 60, 48, 255, 0, 0);

        g.ring.wait_idle();
        o.destroy(g);
        g.renderer.destroy_model(gm);
    }

    // ---- lit hot-set path on synthetic data: injected effects (MeshBumpColorize program, Tree program with
    // alpha test), pipelines created on the job system, disk cache round trip.
    if (rinit) {
        const std::filesystem::path cache_dir = std::filesystem::temp_directory_path() / "alamo_render_test_pso";
        std::error_code ec;
        alamo::remove_all_long(cache_dir, ec);
        auto make_effect = [](const char* name, const char* program, materials::CompareOp alpha, uint32_t ref) {
            materials::MaterialEffect e;
            e.name = name;
            e.phase = materials::RenderPhase::Opaque;
            e.layout = materials::VertexLayout::NU2U3U3;
            materials::ResolvedPass p;
            p.name = "p0";
            p.program = materials::find_program(program).program;
            p.program_status = p.program.vs ? materials::ProgramStatus::Ok : materials::ProgramStatus::NeedsTranslation;
            const materials::DeviceState st = materials::engine_device_defaults();
            p.pso[0] = materials::resolve_pso(st);
            p.pso[0].vs_id = p.program.vs ? p.program.vs->id : 0;
            p.pso[0].ps_id = p.program.ps ? p.program.ps->id : 0;
            p.pso[0].vertex_layout = e.layout;
            p.pso[0].alpha_test = alpha;
            p.pso[1] = p.pso[0];
            p.dynamic = materials::resolve_dynamic(st);
            p.push = materials::resolve_push(st);
            p.push.alpha_func = static_cast<uint32_t>(alpha);
            p.push.alpha_ref = ref;
            p.push.flags = 0;
            materials::SamplerBinding s0, s1;
            s0.slot = 0;
            s0.texture_param = "BaseTexture";
            s1.slot = 1;
            s1.texture_param = "NormalTexture";
            p.samplers = {s0, s1};
            e.passes.push_back(p);
            return e;
        };
        auto lit_model = [] {
            auto m = std::make_shared<assets::ModelAsset>();
            m->name = "SYNTHETIC_LIT";
            m->skeleton.names = {"ROOT"};
            m->skeleton.parents = {-1};
            m->skeleton.visible = {1};
            m->skeleton.billboard_mode = {0};
            m->skeleton.bind_local = {Mat3x4{}};
            m->skeleton.finalize();
            const uint8_t white[4] = {255, 255, 255, 255};
            add_quad(*m, "LIT", 0, -0.9f, -0.1f, -0.4f, 0.4f, 0.0f, white, "grey.dds");
            add_quad(*m, "ALPHATEST", 0, 0.1f, 0.9f, -0.4f, 0.4f, 0.0f, white, "halfalpha.dds");
            m->submeshes[0].material.effect = "TestBump.fx";
            m->submeshes[1].material.effect = "TestTree.fx";
            assets::MaterialParam np;
            np.name = "NormalTexture";
            np.type = assets::ParamType::Texture;
            np.texture = "flatnormal.dds";
            m->submeshes[0].material.params.push_back(np);
            m->bounds = {{-1, -1, -1}, {1, 1, 1}};
            m->sphere = {{0, 0, 0}, 1.8f};
            return m;
        };
        auto lit_textures = [](std::string_view name, assets::TextureRole) -> std::shared_ptr<const assets::TextureAsset> {
            auto t = std::make_shared<assets::TextureAsset>();
            t->name = std::string(name);
            t->format = assets::TextureFormat::RGBA8;
            t->mip_levels = 1;
            t->height = 1;
            if (name == "grey.dds") {
                t->width = 1;
                t->data = {128, 128, 128, 0};  // alpha 0: no team colour
            } else if (name == "flatnormal.dds") {
                t->width = 1;
                t->data = {128, 128, 255, 0};  // +Z tangent-space normal, no specular mask
            } else if (name == "halfalpha.dds") {
                t->width = 2;
                t->data = {200, 200, 200, 0, 200, 200, 200, 255};  // left texel discarded by ALPHAFUNC GREATER 128
            } else {
                return nullptr;
            }
            t->levels.push_back({0, 0, t->width, t->height, 0, t->data.size()});
            return t;
        };
        struct LitRun {
            render::PipelineManager::Stats ps;
            double first_ready_ms = 0;
        };
        auto run_lit = [&](LitRun& out) {
            jobs::JobSystemConfig jc;
            jc.workers = 2;
            jobs::JobSystem js(jc);
            render::EffectCache effects(nullptr);
            effects.add("TestBump.fx", make_effect("TESTBUMP", "MeshBumpColorize", materials::CompareOp::Always, 0));
            effects.add("TestTree.fx", make_effect("TESTTREE", "Tree", materials::CompareOp::Greater, 128));
            render::Renderer r;
            render::RendererDesc rd;
            rd.color_format = VK_FORMAT_R8G8B8A8_UNORM;
            rd.effects = &effects;
            rd.js = &js;
            rd.pipeline_cache_dir = cache_dir;
            const auto t0 = std::chrono::steady_clock::now();
            CHECK(r.init(g.ctx, g.alloc, g.up, rd));
            render::GpuModel gm;
            CHECK(r.upload_model(lit_model(), lit_textures, gm, &js));
            CHECK(gm.submeshes.size() == 2 && gm.submeshes[0].lit == render::LitStatus::Lit &&
                  gm.submeshes[1].lit == render::LitStatus::Lit);
            for (const auto& t : gm.buffer_tickets) CHECK(g.up.wait(t));
            for (const auto& t : gm.textures) CHECK(g.up.wait(t.ticket));
            Offscreen o;
            CHECK(o.create(g, 128, 128));
            render::OrbitCamera cam;
            cam.yaw = -1.5707963f;
            cam.pitch = 0.0f;
            cam.distance = 5.0f;
            cam.radius = 2.0f;
            cam.fov_x = 2.0f * std::atan(0.2f);
            const render::ViewMatrices vm = render::make_view(cam, 128, 128);
            render::Instance inst;
            inst.model = &gm;
            render::DrawStats st;
            // First frame: pipelines are requested and drawn unlit meanwhile (never a stall).
            CHECK(render_frame(g, r, o, vm, {&inst, 1}, &gm, &st));
            const bool first_pending = st.pending_pipeline > 0;
            r.pipelines().wait_idle();
            out.first_ready_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            auto sun = [&](float y) {
                render::Lighting l = render::Lighting::space_default();
                l.sun.direction = {0, y, 0};  // y = -1: towards the camera side (lights the quad's front)
                l.sun.intensity = 0.6f;
                r.set_lighting(l);
                CHECK(render_frame(g, r, o, vm, {&inst, 1}, &gm, &st));
                CHECK(st.pending_pipeline == 0 && st.lit_draws == 2 && st.unlit_draws == 0);
                return std::array<int, 3>{o.px(32, 64)[0], o.px(80, 64)[0], o.px(112, 64)[0]};
            };
            const auto front = sun(-1.0f);
            const auto back = sun(1.0f);
            std::printf("lit: sun towards the face %d, away %d (grey 128 base); alpha test: discarded texel %d, kept texel %d; "
                        "first frame %s\n",
                        front[0], back[0], front[1], front[2], first_pending ? "drawn unlit while pipelines compiled" : "lit");
            CHECK(front[0] > back[0] + 40);       // N.L term follows DIR_LIGHT_VEC_0
            CHECK(back[0] > 5);                   // SH fill (ambient) still lights the back-lit face
            CHECK(rgb_close(o.px(80, 64), 0, 0, 64));  // alpha 0 < 128 -> discard -> clear colour
            CHECK(front[2] > 64);                 // alpha 255 kept (sun towards the face)
            out.ps = r.pipelines().stats();
            g.ring.wait_idle();
            o.destroy(g);
            r.destroy_model(gm);
            r.shutdown();  // saves the key list + VkPipelineCache
        };
        LitRun cold, warm;
        run_lit(cold);
        run_lit(warm);
        std::printf("pso cache: cold %u keys created (%.2f ms summed, ready after %.1f ms); warm %u keys reloaded, %u ready "
                    "(%.2f ms summed, ready after %.1f ms)\n",
                    cold.ps.keys, cold.ps.create_ms, cold.first_ready_ms, warm.ps.reloaded, warm.ps.ready, warm.ps.create_ms,
                    warm.first_ready_ms);
        CHECK(cold.ps.reloaded == 0 && !cold.ps.cache_loaded && cold.ps.ready == 2 && cold.ps.failed == 0);
        CHECK(warm.ps.reloaded == 2 && warm.ps.cache_loaded && warm.ps.keys == 2 && warm.ps.ready == 2);
        // rhi::PipelineCache::stats() (creation feedback): every warm pipeline is a VkPipelineCache hit.
        std::printf("pso cache: rhi stats cold %u hits / %u misses / %u unknown, warm %u / %u / %u\n", cold.ps.cache_hits,
                    cold.ps.cache_misses, cold.ps.feedback_unknown, warm.ps.cache_hits, warm.ps.cache_misses, warm.ps.feedback_unknown);
        CHECK(warm.ps.cache_hits == warm.ps.ready && warm.ps.cache_misses == 0 && warm.ps.ready == 2);
        {   // destroy_model() with frames in flight: vertex / index buffers (like images) wait for the frames that
            // used them (validation flags a buffer destroyed while a pending command buffer reads it).
            jobs::JobSystemConfig jc;
            jc.workers = 2;
            jobs::JobSystem js(jc);
            render::EffectCache effects(nullptr);
            effects.add("TestBump.fx", make_effect("TESTBUMP", "MeshBumpColorize", materials::CompareOp::Always, 0));
            effects.add("TestTree.fx", make_effect("TESTTREE", "Tree", materials::CompareOp::Greater, 128));
            render::Renderer r;
            render::RendererDesc frd;
            frd.color_format = VK_FORMAT_R8G8B8A8_UNORM;
            frd.effects = &effects;
            frd.js = &js;
            CHECK(r.init(g.ctx, g.alloc, g.up, frd));
            r.set_frame_ring(&g.ring);
            render::GpuModel gm;
            CHECK(r.upload_model(lit_model(), lit_textures, gm, &js));
            for (const auto& t : gm.buffer_tickets) CHECK(g.up.wait(t));
            for (const auto& t : gm.textures) CHECK(g.up.wait(t.ticket));
            r.pipelines().wait_idle();
            Offscreen o;
            CHECK(o.create(g, 64, 64));
            render::OrbitCamera cam;
            cam.distance = 5.0f;
            cam.radius = 2.0f;
            const render::ViewMatrices vm = render::make_view(cam, 64, 64);
            render::Instance inst;
            inst.model = &gm;
            // One frame submitted without waiting for it (the ring keeps it in flight).
            auto submit = [&](std::span<const render::Instance> in, render::GpuModel* resident) {
                rhi::Frame* f = g.ring.begin_frame();
                if (!f) return false;
                VkCommandBuffer cmd = f->cmd();
                const rhi::AcquireResult acq = g.up.record_acquires(cmd);
                if (resident) r.make_resident(*resident);
                render::PassTarget pt;
                pt.color = o.color.handle;
                pt.color_view = o.color.view;
                pt.depth = o.depth.handle;
                pt.depth_view = o.depth.view;
                pt.extent = {o.w, o.h};
                std::memcpy(pt.clear, kClear, sizeof kClear);
                pt.color_after = rhi::ImageState::TransferSrc;
                const bool drawn = r.render(*f, cmd, pt, vm, in, render::DrawMode::Lit, {});
                VkSemaphoreSubmitInfo wait = rhi::semaphore_info(g.up.timeline(), acq.wait_value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
                rhi::FrameSubmit fs;
                if (acq.wait_value) fs.waits = {&wait, 1};
                return g.ring.end_frame(*f, fs) && drawn;
            };
            CHECK(submit({&inst, 1}, &gm));
            CHECK(submit({&inst, 1}, &gm));
            r.destroy_model(gm);  // both frames may still be reading the buffers
            const size_t pending = r.buffers_pending();
            CHECK(submit({}, nullptr));  // collect_released() after begin_frame: only retired buffers go
            CHECK(submit({}, nullptr));
            g.ring.wait_idle();
            r.collect_released();
            std::printf("destroy_model in flight: %zu buffers deferred, %zu left after the frames retired\n", pending,
                        r.buffers_pending());
            CHECK(r.buffers_pending() == 0);
            o.destroy(g);
            r.shutdown();
        }
        // Invalidation: a different programs fingerprint in the .keys header discards both files.
        for (const std::filesystem::path& kp : alamo::list_directory(cache_dir, ec)) {
            if (kp.extension() != ".keys") continue;
            std::fstream kf(alamo::long_path(kp), std::ios::in | std::ios::out | std::ios::binary);
            kf.seekp(8 + 4 + 12 + VK_UUID_SIZE);  // magic, version, vendor/device/driver, uuid -> fingerprint
            const char x = 0x5A;
            kf.write(&x, 1);
        }
        {
            render::PipelineManager pm;
            render::PipelineManagerDesc pd;
            pd.dir = cache_dir;
            CHECK(pm.init(g.ctx, pd));
            CHECK(pm.stats().invalidated && pm.stats().reloaded == 0 && !pm.stats().cache_loaded);
            pm.shutdown(false);
        }
        {   // Pipeline cache in a directory path over 300 characters (alamo::long_path): saved, then reloaded.
            std::filesystem::path long_dir = std::filesystem::temp_directory_path() / "alamo_render_long_pso";
            std::error_code lec;
            alamo::remove_all_long(long_dir, lec);
            const std::filesystem::path root = long_dir;
            while (long_dir.native().size() < 320) long_dir /= std::string(40, 'd');
            auto once = [&](render::PipelineManager::Stats& out) {
                jobs::JobSystem js;
                render::EffectCache effects(nullptr);
                effects.add("TestBump.fx", make_effect("TESTBUMP", "MeshBumpColorize", materials::CompareOp::Always, 0));
                effects.add("TestTree.fx", make_effect("TESTTREE", "Tree", materials::CompareOp::Greater, 128));
                render::Renderer r;
                render::RendererDesc ld;
                ld.color_format = VK_FORMAT_R8G8B8A8_UNORM;
                ld.effects = &effects;
                ld.js = &js;
                ld.pipeline_cache_dir = long_dir;
                CHECK(r.init(g.ctx, g.alloc, g.up, ld));
                render::GpuModel gm;
                CHECK(r.upload_model(lit_model(), lit_textures, gm, &js));
                for (const auto& t : gm.buffer_tickets) CHECK(g.up.wait(t));
                for (const auto& t : gm.textures) CHECK(g.up.wait(t.ticket));
                r.prepare_pipelines(gm);
                r.pipelines().wait_idle();
                out = r.pipelines().stats();
                r.destroy_model(gm);
                r.shutdown();  // saves keys + VkPipelineCache into the long directory
            };
            render::PipelineManager::Stats first, second;
            once(first);
            size_t vkcache_files = 0;  // the .vkcache (rhi::PipelineCache, read_host_file) lands in the long dir too
            for (const std::filesystem::path& cp : alamo::list_directory(long_dir, lec)) vkcache_files += cp.extension() == ".vkcache" ? 1u : 0u;
            CHECK(vkcache_files == 1);
            once(second);
            std::printf("pso cache in a %zu-character dir: first %u ready / %u reloaded, second %u reloaded (cache %s)\n",
                        long_dir.native().size(), first.ready, first.reloaded, second.reloaded, second.cache_loaded ? "loaded" : "missing");
            CHECK(first.reloaded == 0 && first.ready == 2 && second.reloaded == 2 && second.cache_loaded);
            CHECK(alamo::remove_all_long(root, lec) > 0 && !lec);
        }
        // ---- (d) scene fog and (e) background objects, through render()'s frame graph.
        {
            jobs::JobSystem js;
            render::EffectCache effects(nullptr);
            effects.add("TestBump.fx", make_effect("TESTBUMP", "MeshBumpColorize", materials::CompareOp::Always, 0));
            render::Renderer r;
            render::RendererDesc frd;
            frd.color_format = VK_FORMAT_R8G8B8A8_UNORM;
            frd.effects = &effects;
            frd.js = &js;
            CHECK(r.init(g.ctx, g.alloc, g.up, frd));
            render::GpuModel lit, unl;
            CHECK(r.upload_model(lit_model(), lit_textures, lit, &js));
            auto plain = std::make_shared<assets::ModelAsset>();  // unlit quads: main (magenta) and sky (green)
            plain->name = "PLAIN";
            plain->skeleton.names = {"ROOT"};
            plain->skeleton.parents = {-1};
            plain->skeleton.visible = {1};
            plain->skeleton.billboard_mode = {0};
            plain->skeleton.bind_local = {Mat3x4{}};
            plain->skeleton.finalize();
            const uint8_t magenta[4] = {255, 0, 255, 255}, green[4] = {0, 255, 0, 255};
            add_quad(*plain, "MAIN", 0, -1.0f, 1.0f, -1.0f, 1.0f, 0.0f, magenta, nullptr);
            plain->bounds = {{-1, -1, -1}, {1, 1, 1}};
            plain->sphere = {{0, 0, 0}, 1.5f};
            auto skym = std::make_shared<assets::ModelAsset>(*plain);
            skym->name = "SKY";
            skym->submeshes.clear();
            skym->meshes.clear();
            add_quad(*skym, "SKY", 0, -100.0f, 100.0f, -100.0f, 100.0f, 20.0f, green, nullptr);
            skym->bounds = {{-100, 19, -100}, {100, 21, 100}};
            render::GpuModel gplain, gsky;
            CHECK(r.upload_model(plain, lit_textures, gplain, &js));
            CHECK(r.upload_model(skym, lit_textures, gsky, &js));
            for (render::GpuModel* m : {&lit, &gplain, &gsky}) {
                for (const auto& t : m->buffer_tickets) CHECK(g.up.wait(t));
                for (const auto& t : m->textures) CHECK(g.up.wait(t.ticket));
            }
            r.prepare_pipelines(lit);
            r.pipelines().wait_idle();
            Offscreen o;
            CHECK(o.create(g, 128, 128));
            render::OrbitCamera cam;
            cam.yaw = -1.5707963f;
            cam.pitch = 0.0f;
            cam.distance = 5.0f;
            cam.radius = 2.0f;
            cam.fov_x = 2.0f * std::atan(0.2f);
            const render::ViewMatrices vm = render::make_view(cam, 128, 128);
            render::Instance li;
            li.model = &lit;
            CHECK(render_frame(g, r, o, vm, {&li, 1}, &lit));  // acquires
            r.make_resident(gplain);
            r.make_resident(gsky);
            render::Lighting fl = render::Lighting::land_default();
            fl.sun.direction = {0, -1, 0};
            fl.fog_enabled = false;
            r.set_lighting(fl);
            render::DrawStats st;
            CHECK(render_frame(g, r, o, vm, {&li, 1}, nullptr, &st));
            const uint8_t* p0 = o.px(32, 64);
            const int c0[3] = {p0[0], p0[1], p0[2]};
            fl.fog_enabled = true;  // linear fog 0..10, red
            fl.fog_start = 0.0f;
            fl.fog_end = 10.0f;
            fl.fog_color = {1, 0, 0};
            r.set_lighting(fl);
            CHECK(render_frame(g, r, o, vm, {&li, 1}, nullptr, &st));
            const uint8_t* p1 = o.px(32, 64);
            // Expected: lerp(FogColor, colour, f) with vertex fog f = saturate(FOG_VALS.x |clip.xyz| + FOG_VALS.y)
            // evaluated at the quad's vertices and interpolated over the triangle under the pixel.
            auto vfog = [&](float x, float z) {
                const render::Float4 cl = vm.view_proj * render::Float4{x, 0.0f, z, 1.0f};
                return std::clamp(-0.1f * std::sqrt(cl.x * cl.x + cl.y * cl.y + cl.z * cl.z) + 1.0f, 0.0f, 1.0f);
            };
            const float wx = 32.5f / 64.0f - 1.0f, wz = 1.0f - 64.5f / 64.0f;
            // LIT quad x -0.9..-0.1, z -0.4..0.4; triangle (v0 (-0.9,-0.4), v1 (-0.1,-0.4), v2 (-0.1,0.4)) holds the pixel.
            const float ax = -0.9f, az = -0.4f, bx = -0.1f, bz = -0.4f, cx = -0.1f, cz = 0.4f;
            const float den = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz);
            const float la = ((bz - cz) * (wx - cx) + (cx - bx) * (wz - cz)) / den;
            const float lb = ((cz - az) * (wx - cx) + (ax - cx) * (wz - cz)) / den;
            const float f = la * vfog(ax, az) + lb * vfog(bx, bz) + (1.0f - la - lb) * vfog(cx, cz);
            const float expect[3] = {255.0f * (1 - f) + c0[0] * f, c0[1] * f, c0[2] * f};
            std::printf("fog: off (%d,%d,%d) -> on (%d,%d,%d), expected (%.1f,%.1f,%.1f) for fog factor %.3f\n", c0[0], c0[1], c0[2],
                        p1[0], p1[1], p1[2], expect[0], expect[1], expect[2], f);
            for (int ch = 0; ch < 3; ++ch) CHECK(std::fabs(p1[ch] - expect[ch]) <= 4.0f);
            CHECK(f > 0.2f && f < 0.8f);
            {   // Degenerate range (start == end): a step at that distance in the scene fog colour (never black).
                float x = 0, y = 0;
                render::Lighting dl = fl;
                dl.fog_start = dl.fog_end = 100.0f;
                CHECK(render::scene_fog_vals(dl, x, y));
                const float before = std::clamp(99.0f * x + y, 0.0f, 1.0f), after = std::clamp(100.0f * x + y, 0.0f, 1.0f);
                materials::FrameCB dcb{};
                render::apply_scene_fog(dl, dcb);
                dl.fog_enabled = false;
                float ox = 0, oy = 0;
                CHECK(!render::scene_fog_vals(dl, ox, oy) && ox == -0.0002f && oy == 3.0f);
                fl.fog_start = fl.fog_end = 1.0f;  // the quad is ~5 units away: fully fogged
                r.set_lighting(fl);
                CHECK(render_frame(g, r, o, vm, {&li, 1}, nullptr, &st));
                const uint8_t* q = o.px(32, 64);
                const int far_px[3] = {q[0], q[1], q[2]};
                fl.fog_start = fl.fog_end = 100.0f;  // the quad is nearer than the step: unfogged
                r.set_lighting(fl);
                CHECK(render_frame(g, r, o, vm, {&li, 1}, nullptr, &st));
                q = o.px(32, 64);
                const int near_px[3] = {q[0], q[1], q[2]};
                std::printf("fog step (start == end): factor %.3f before / %.3f at the step, FogColor (%.0f,%.0f,%.0f); quad beyond "
                            "the step (%d,%d,%d), before it (%d,%d,%d)\n",
                            before, after, dcb.fog_color.x * 255.0f, dcb.fog_color.y * 255.0f, dcb.fog_color.z * 255.0f, far_px[0],
                            far_px[1], far_px[2], near_px[0], near_px[1], near_px[2]);
                CHECK(before == 1.0f && after == 0.0f && dcb.fog_color.x == 1.0f && dcb.fog_color.y == 0.0f);
                CHECK(std::abs(far_px[0] - 255) <= 2 && far_px[1] <= 2 && far_px[2] <= 2);
                for (int ch = 0; ch < 3; ++ch) CHECK(std::abs(near_px[ch] - c0[ch]) <= 2);
                fl.fog_start = 0.0f;
                fl.fog_end = 10.0f;
            }
            {   // A pass that sets FogEnable = FALSE keeps its colour under scene fog (RENDER.md 2.4).
                materials::MaterialEffect nf = make_effect("TESTBUMP", "MeshBumpColorize", materials::CompareOp::Always, 0);
                materials::StateSlot& fs = nf.passes[0].state.rs[static_cast<size_t>(materials::RS::FogEnable)];
                fs.value = 0;
                fs.origin = materials::StateOrigin::Constant;
                effects.add("TestBump.fx", std::move(nf));
                render::GpuModel nfm;
                CHECK(r.upload_model(lit_model(), lit_textures, nfm, &js));
                for (const auto& t : nfm.buffer_tickets) CHECK(g.up.wait(t));
                for (const auto& t : nfm.textures) CHECK(g.up.wait(t.ticket));
                r.prepare_pipelines(nfm);
                r.pipelines().wait_idle();
                render::Instance ni;
                ni.model = &nfm;
                r.set_lighting(fl);  // fog on, 0..10, red
                CHECK(render_frame(g, r, o, vm, {&ni, 1}, &nfm, &st));
                CHECK(render_frame(g, r, o, vm, {&ni, 1}, nullptr, &st));
                const uint8_t* q = o.px(32, 64);
                std::printf("fog: pass with FogEnable FALSE under scene fog (%d,%d,%d), unfogged (%d,%d,%d)\n", q[0], q[1], q[2], c0[0],
                            c0[1], c0[2]);
                CHECK(!nfm.submeshes.empty() && nfm.submeshes[0].passes[0].fog_off && !lit.submeshes[0].passes[0].fog_off);
                for (int ch = 0; ch < 3; ++ch) CHECK(std::abs(q[ch] - c0[ch]) <= 2);
                g.ring.wait_idle();
                r.destroy_model(nfm);
            }
            // (e) sky: a background quad NEARER than the main quad (20 vs 60 units) still ends up behind it.
            fl.fog_enabled = false;
            r.set_lighting(fl);
            render::OrbitCamera sc = cam;
            sc.distance = 60.0f;
            sc.radius = 30.0f;
            sc.fov_x = 2.0f * std::atan(0.05f);
            const render::ViewMatrices svm = render::make_view(sc, 128, 128);
            render::Instance mi, si;
            mi.model = &gplain;
            si.model = &gsky;
            CHECK(render_frame(g, r, o, svm, {&mi, 1}, nullptr, &st, render::DrawMode::Lit, {}, nullptr, {&si, 1}));
            std::printf("sky: %u sky draws; centre (%d,%d,%d) corner (%d,%d,%d)\n", st.sky_draws, o.px(64, 64)[0], o.px(64, 64)[1],
                        o.px(64, 64)[2], o.px(4, 4)[0], o.px(4, 4)[1], o.px(4, 4)[2]);
            CHECK(st.sky_draws == 1 && rgb_close(o.px(64, 64), 255, 0, 255, 2) && rgb_close(o.px(4, 4), 0, 255, 0, 2));
            g.ring.wait_idle();
            o.destroy(g);
            for (render::GpuModel* m : {&lit, &gplain, &gsky}) r.destroy_model(*m);
            r.shutdown();
        }
        // ---- Unlit pass: the material's alpha test (first pass ALPHAFUNC / ALPHAREF) and scene fog.
        {
            jobs::JobSystem js;
            render::EffectCache effects(nullptr);
            effects.add("TestBump.fx", make_effect("TESTBUMP", "MeshBumpColorize", materials::CompareOp::Always, 0));
            effects.add("TestTree.fx", make_effect("TESTTREE", "Tree", materials::CompareOp::Greater, 128));
            render::Renderer r;
            render::RendererDesc urd;
            urd.color_format = VK_FORMAT_R8G8B8A8_UNORM;
            urd.effects = &effects;
            urd.js = &js;
            CHECK(r.init(g.ctx, g.alloc, g.up, urd));
            render::GpuModel gm;
            CHECK(r.upload_model(lit_model(), lit_textures, gm, &js));
            for (const auto& t : gm.buffer_tickets) CHECK(g.up.wait(t));
            for (const auto& t : gm.textures) CHECK(g.up.wait(t.ticket));
            CHECK(gm.submeshes.size() == 2 &&
                  gm.submeshes[1].unlit_alpha_func == static_cast<uint32_t>(materials::CompareOp::Greater) &&
                  gm.submeshes[1].unlit_alpha_ref == 128 &&
                  gm.submeshes[0].unlit_alpha_func == static_cast<uint32_t>(materials::CompareOp::Always));
            Offscreen o;
            CHECK(o.create(g, 128, 128));
            render::OrbitCamera cam;
            cam.yaw = -1.5707963f;
            cam.pitch = 0.0f;
            cam.distance = 5.0f;
            cam.radius = 2.0f;
            cam.fov_x = 2.0f * std::atan(0.2f);
            const render::ViewMatrices vm = render::make_view(cam, 128, 128);
            render::Instance inst;
            inst.model = &gm;
            render::Lighting fl = render::Lighting::space_default();
            fl.fog_enabled = false;
            r.set_lighting(fl);
            render::DrawStats st;
            CHECK(render_frame(g, r, o, vm, {&inst, 1}, &gm, &st, render::DrawMode::Unlit));  // acquires
            CHECK(render_frame(g, r, o, vm, {&inst, 1}, nullptr, &st, render::DrawMode::Unlit));
            CHECK(st.unlit_draws == 2);
            const bool discarded = rgb_close(o.px(80, 64), 0, 0, 64);  // alpha 0 texel, GREATER 128 -> clear colour
            const uint8_t* k0 = o.px(112, 64);                          // alpha 255 texel kept
            const int c0[3] = {k0[0], k0[1], k0[2]};
            CHECK(discarded && !rgb_close(k0, 0, 0, 64));
            fl.fog_enabled = true;  // linear fog 0..10, red
            fl.fog_start = 0.0f;
            fl.fog_end = 10.0f;
            fl.fog_color = {1, 0, 0};
            r.set_lighting(fl);
            CHECK(render_frame(g, r, o, vm, {&inst, 1}, nullptr, &st, render::DrawMode::Unlit));
            const uint8_t* k1 = o.px(112, 64);
            // Vertex fog interpolated over the ALPHATEST quad's triangle (0.1,-0.4) (0.9,-0.4) (0.9,0.4) under the pixel.
            auto vfog = [&](float x, float z) {
                const render::Float4 cl = vm.view_proj * render::Float4{x, 0.0f, z, 1.0f};
                return std::clamp(-0.1f * std::sqrt(cl.x * cl.x + cl.y * cl.y + cl.z * cl.z) + 1.0f, 0.0f, 1.0f);
            };
            const float wx = 112.5f / 64.0f - 1.0f, wz = 1.0f - 64.5f / 64.0f;
            const float ax = 0.1f, az = -0.4f, bx = 0.9f, bz = -0.4f, cx = 0.9f, cz = 0.4f;
            const float den = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz);
            const float la = ((bz - cz) * (wx - cx) + (cx - bx) * (wz - cz)) / den;
            const float lb = ((cz - az) * (wx - cx) + (ax - cx) * (wz - cz)) / den;
            const float f = la * vfog(ax, az) + lb * vfog(bx, bz) + (1.0f - la - lb) * vfog(cx, cz);
            const float expect[3] = {255.0f * (1 - f) + c0[0] * f, c0[1] * f, c0[2] * f};
            std::printf("unlit: alpha test %s the alpha-0 texel; fog off (%d,%d,%d) -> on (%d,%d,%d), expected (%.1f,%.1f,%.1f) "
                        "for fog factor %.3f; discarded texel still clear %s\n",
                        discarded ? "discards" : "KEEPS", c0[0], c0[1], c0[2], k1[0], k1[1], k1[2], expect[0], expect[1],
                        expect[2], f, rgb_close(o.px(80, 64), 0, 0, 64) ? "yes" : "no");
            for (int ch = 0; ch < 3; ++ch) CHECK(std::fabs(k1[ch] - expect[ch]) <= 5.0f);
            CHECK(f > 0.2f && f < 0.8f && rgb_close(o.px(80, 64), 0, 0, 64));
            g.ring.wait_idle();
            o.destroy(g);
            r.destroy_model(gm);
            r.shutdown();
        }
        // ---- C5: multithreaded recording must equal single-threaded recording (draw list and pixels).
        {
            jobs::JobSystem js;
            render::EffectCache effects(nullptr);
            effects.add("TestBump.fx", make_effect("TESTBUMP", "MeshBumpColorize", materials::CompareOp::Always, 0));
            effects.add("TestTree.fx", make_effect("TESTTREE", "Tree", materials::CompareOp::Greater, 128));
            render::Renderer r;
            render::RendererDesc mrd;
            mrd.color_format = VK_FORMAT_R8G8B8A8_UNORM;
            mrd.effects = &effects;
            mrd.js = &js;
            CHECK(r.init(g.ctx, g.alloc, g.up, mrd));
            render::GpuModel lit, unl;
            CHECK(r.upload_model(lit_model(), lit_textures, lit, &js));
            CHECK(r.upload_model(synthetic_model(), synthetic_texture, unl, &js));
            for (render::GpuModel* m : {&lit, &unl}) {
                for (const auto& t : m->buffer_tickets) CHECK(g.up.wait(t));
                for (const auto& t : m->textures) CHECK(g.up.wait(t.ticket));
            }
            r.prepare_pipelines(lit);
            r.pipelines().wait_idle();
            Offscreen o;
            CHECK(o.create(g, 256, 256));
            std::vector<render::Instance> inst(200);
            static const materials::float4 kTeam[3] = {{1, 0.3f, 0.2f, 1}, {0.2f, 0.5f, 1, 1}, {0.3f, 1, 0.3f, 1}};
            for (uint32_t i = 0; i < 200; ++i) {
                render::Instance& in = inst[i];
                in.model = (i % 2) ? &unl : &lit;
                in.transform.m[0][3] = (static_cast<float>(i % 20) - 9.5f) * 2.2f;
                in.transform.m[1][3] = static_cast<float>(i % 7) * 0.4f;
                in.transform.m[2][3] = (static_cast<float>(i / 20) - 4.5f) * 2.2f;
                in.team_color_override = true;
                in.team_color = kTeam[i % 3];
            }
            render::OrbitCamera cam;
            cam.frame({0, 0, 0}, 26.0f, 1.0f);
            cam.yaw = -1.5707963f;
            cam.pitch = 0.15f;
            const render::ViewMatrices vm = render::make_view(cam, 256, 256);
            CHECK(render_frame(g, r, o, vm, inst, &lit));  // records the upload acquires
            CHECK(r.make_resident(unl) && lit.resident());
            const uint32_t counts[3] = {1, 4, std::max<uint32_t>(js.worker_count(), 2)};
            std::vector<uint8_t> ref;
            std::vector<render::DrawListEntry> ref_list;
            for (int k = 0; k < 3; ++k) {
                render::RecordOptions ro;
                ro.threads = counts[k];
                ro.min_draws_per_chunk = 8;
                render::DrawStats st;
                render::DrawTiming tm;
                CHECK(render_frame(g, r, o, vm, inst, nullptr, &st, render::DrawMode::Lit, ro, &tm));
                const uint8_t* px = static_cast<const uint8_t*>(o.readback.mapped());
                std::vector<uint8_t> img(px, px + static_cast<size_t>(256) * 256 * 4);
                std::printf("mt: %u record threads: %u draws (%u hot-set, %u unlit), %u materials, %u secondaries, build %.2f ms, "
                            "record %.2f ms\n",
                            counts[k], st.draws, st.lit_draws, st.unlit_draws, st.materials, tm.chunks, tm.build_ms, tm.record_ms);
                CHECK(st.draws > 400 && st.pending_pipeline == 0 && st.lit_draws > 0);
                if (k == 0) {
                    ref = img;
                    ref_list = r.last_draw_list();
                    CHECK(tm.chunks == 0);
                } else {
                    CHECK(tm.chunks > 1);
                    CHECK(r.last_draw_list() == ref_list);
                    size_t diff = 0;
                    for (size_t i = 0; i < img.size(); ++i) diff += img[i] != ref[i];
                    if (diff) std::printf("  %zu bytes differ from single-threaded output\n", diff);
                    CHECK(diff == 0);
                }
            }
            // ---- Q3: instanced runs draw the same pixels as one call per item; render() allocates nothing per frame
            // in steady state (build scratch, memo, draw lists and chunk lists kept across frames).
            {
                render::RecordOptions ro;
                ro.instancing = false;
                render::DrawStats off, on;
                CHECK(render_frame(g, r, o, vm, inst, nullptr, &off, render::DrawMode::Lit, ro));
                const uint8_t* px = static_cast<const uint8_t*>(o.readback.mapped());
                size_t diff = 0;
                for (size_t i = 0; i < ref.size(); ++i) diff += px[i] != ref[i];
                CHECK(r.last_draw_list() == ref_list);
                CHECK(render_frame(g, r, o, vm, inst, nullptr, &on, render::DrawMode::Lit, {}));
                std::printf("mt: instancing: %u draws -> %u calls (%u instanced); off: %u calls, %zu bytes differ\n", on.draws,
                            on.draw_calls, on.instanced_calls, off.draw_calls, diff);
                CHECK(diff == 0 && off.draw_calls == off.draws && on.draws == off.draws);
                CHECK(on.instanced_calls > 0 && on.draw_calls < on.draws);
                // 1 thread; 4 threads with a parallel build but inline recording; 4 threads with secondaries (Q3: the
                // FrameRing preallocates its command buffers, rhi::FrameRing::command_buffer_growth() counts late ones);
                // the same with the post chain (heat / bloom PostSpecs are views, their register images cached).
                // All strict: 0 per frame (GCC's deque churn aside, GCC is retired).
                for (int cfg = 0; cfg < 4; ++cfg) {
                    render::RecordOptions ra;
                    ra.threads = cfg == 0 ? 1u : 4u;
                    ra.min_draws_per_chunk = cfg >= 2 ? 8u : 1u << 30;
                    ra.post = cfg == 3;
                    uint64_t worst = 0;
                    uint64_t growth0 = 0;
                    for (int k = 0; k < 24; ++k) {
                        g_measure_allocs = k >= 20;
                        if (k == 20) growth0 = g.ring.command_buffer_growth();
                        CHECK(render_frame(g, r, o, vm, inst, nullptr, nullptr, render::DrawMode::Lit, ra));
                        if (k >= 20) worst = std::max(worst, g_render_allocs);
                    }
                    g_measure_allocs = false;
                    const uint64_t growth = g.ring.command_buffer_growth() - growth0;
                    std::printf("mt: heap allocations in render(), steady state, %u threads%s: %llu per frame, %llu command buffers grown",
                                ra.threads, cfg == 3 ? " + secondaries + post" : cfg == 2 ? " + secondaries" : (cfg == 1 ? " (inline recording)" : ""),
                                static_cast<unsigned long long>(worst), static_cast<unsigned long long>(growth));
                    for (uint64_t k = 0; k < std::min<uint64_t>(worst, 8); ++k) std::printf("%s%zu B", k ? ", " : " (", g_alloc_sizes[k]);
                    std::printf("%s\n", worst ? ")" : "");
                    if (cfg == 0 || !kStdDequeNodeChurn) CHECK(worst == 0);
                    CHECK(growth == 0);
                }
            }
            uint64_t covered = 0;
            for (size_t i = 0; i < ref.size(); i += 4) covered += !rgb_close(&ref[i], 0, 0, 64, 1);
            std::printf("mt: coverage %.1f%%, outputs identical for 1 / 4 / %u record threads\n",
                        100.0 * static_cast<double>(covered) / (256.0 * 256.0), counts[2]);
            CHECK(covered > 256 * 256 / 30);  // the scene really draws
            g.ring.wait_idle();
            o.destroy(g);
            r.destroy_model(lit);
            r.destroy_model(unl);
            r.shutdown();
        }
        alamo::remove_all_long(cache_dir, ec);
    }

    // ---- real data (ALAMO_FOC_DATA): lit + unlit captures
    const char* root = std::getenv("ALAMO_FOC_DATA");
    if (rinit && root && *root) {
        vfs::VfsConfig cfg;
        cfg.foc_root = root;
        std::string err;
        auto files = vfs::Vfs::create(cfg, &err);
        CHECK(files != nullptr);
        if (files) {
            render::EffectCache effects(files.get());
            assets::AssetCache cache(*files, effects.model_load_options());  // effect vertex-type overrides (D.2)
            jobs::JobSystem js;
            render::Renderer rr;
            render::RendererDesc rrd;
            rrd.color_format = VK_FORMAT_R8G8B8A8_UNORM;
            rrd.effects = &effects;
            rrd.js = &js;
            rrd.pipeline_cache_dir = std::filesystem::temp_directory_path() / "alamo_render_test_real_pso";
            CHECK(rr.init(g.ctx, g.alloc, g.up, rrd));
            rr.set_frame_ring(&g.ring);  // bindless slots released / retired against the frame timeline
            {   // every capture below is lit by the map's own Kuat start environment (TEE record via the VFS)
                render::MapEnvironments kuat_env;
                std::string kerr;
                CHECK(render::load_map_environments(*files, "_MP_SPACE_KUAT", kuat_env, &kerr) && kuat_env.start());
                if (kuat_env.start()) rr.set_lighting(render::Lighting::from_environment(*kuat_env.start(), kuat_env.space));
                std::printf("track B: runtime DXC %s\n", rr.dxc_status().c_str());
            }
            Offscreen o;
            CHECK(o.create(g, 512, 512));
            std::error_code ec;
            std::filesystem::create_directories(alamo::path_from_utf8(src_dir) / "logs", ec);
            struct Capture {
                double coverage = 0, lit = 0;
                std::vector<uint8_t> rgba;
                render::DrawStats stats;
            };
            // Render `gm` framed at 512x512 with an optional pose; returns pixels + coverage.
            auto capture = [&](render::GpuModel& gm, std::span<const Mat3x4> pose, float zoom, Capture& c,
                               render::DrawMode mode, float pitch = 0.35f, float yaw = 0.6f,
                               std::span<const render::Instance> extra = {}, std::span<const render::Instance> sky = {},
                               const render::RecordOptions& ro = {}) {
                render::OrbitCamera cam;
                cam.pitch = pitch;
                cam.yaw = yaw;
                const assets::Sphere sph = gm.drawn_sphere();
                cam.frame(sph.center, sph.radius, 1.0f);
                cam.distance *= zoom;
                std::vector<render::Instance> all(1);
                all[0].model = &gm;
                all[0].bone_world = pose;
                all.insert(all.end(), extra.begin(), extra.end());
                const render::ViewMatrices vm = render::make_view(cam, 512, 512);
                CHECK(render_settled(g, rr, o, vm, all, &gm, c.stats, mode, ro, nullptr, sky));
                CHECK(c.stats.post_pending == 0);  // deterministic PNGs: every post pass ran
                uint64_t covered = 0, lit = 0;
                c.rgba.assign(static_cast<size_t>(512) * 512 * 4, 0);
                for (uint32_t y = 0; y < 512; ++y)
                    for (uint32_t x = 0; x < 512; ++x) {
                        const uint8_t* p = o.px(x, y);
                        uint8_t* q = &c.rgba[(static_cast<size_t>(y) * 512 + x) * 4];
                        q[0] = p[0];
                        q[1] = p[1];
                        q[2] = p[2];
                        q[3] = 255;
                        if (!rgb_close(p, 0, 0, 64, 1)) {
                            ++covered;
                            if (p[0] + p[1] + p[2] > 60) ++lit;
                        }
                    }
                c.coverage = static_cast<double>(covered) / (512.0 * 512.0);
                c.lit = covered ? static_cast<double>(lit) / static_cast<double>(covered) : 0.0;
                return vm;
            };
            // Background load on the job system; the streamer is polled once per (empty) frame like the app does.
            auto load = [&](render::ModelStreamer& streamer, const char* name) -> render::GpuModel* {
                const auto t0 = std::chrono::steady_clock::now();
                const uint32_t h = streamer.request(name);
                render::OrbitCamera cam;
                int frames = 0;
                for (; frames < 5000; ++frames) {
                    streamer.update();
                    const auto s = streamer.state(h);
                    if (s == render::ModelStreamer::State::Ready || s == render::ModelStreamer::State::Failed) break;
                    render::GpuModel* part = streamer.model(h);
                    render::Instance inst;
                    inst.model = part;
                    render_frame(g, rr, o, render::make_view(cam, 512, 512),
                                 part ? std::span<const render::Instance>(&inst, 1) : std::span<const render::Instance>(), nullptr);
                }
                const double ready_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                if (streamer.state(h) != render::ModelStreamer::State::Ready) {
                    std::printf("%s: load failed: %s\n", name, streamer.error(h).c_str());
                    return nullptr;
                }
                render::GpuModel* gm = streamer.model(h);
                // Lit captures need every program and pipeline (tests may block; the app never does).
                rr.wait_translations();
                rr.prepare_pipelines(*gm);
                rr.pipelines().wait_idle();
                const double psos_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                const assets::ModelAsset& ma = *gm->asset;
                uint32_t fallbacks = 0, lit = 0;
                for (const auto& t : gm->textures) fallbacks += t.fallback ? 1u : 0u;
                std::map<std::string, int> fx;
                for (const auto& s : gm->submeshes) {
                    lit += s.lit == render::LitStatus::Lit ? 1u : 0u;
                    ++fx[s.effect_name.empty() ? std::string("<none>") : s.effect_name];
                }
                std::printf("%s: resident in %.0f ms, pipelines ready at %.0f ms (job %.0f ms, %d frames polled): %zu bones, "
                            "%zu submeshes, LOD %d, %zu drawn (%u lit), %u damage meshes hidden, %zu textures (%u fallback), "
                            "%.1f MB; effects:",
                            name, ready_ms, psos_ms, streamer.cpu_ms(h), frames, ma.skeleton.size(), ma.submeshes.size(),
                            gm->selection.lod, gm->submeshes.size(), lit, gm->damage_meshes_hidden, gm->textures.size(), fallbacks,
                            static_cast<double>(gm->upload_bytes) / (1024.0 * 1024.0));
                for (const auto& [k, n] : fx) std::printf(" %s x%d", k.c_str(), n);
                std::printf("\n");
                return gm;
            };
            auto report = [&](const char* label, const Capture& c) {
                std::printf("%s: %u draws (%u hot-set, %u FF, %u unlit, %u skinned, %u blended), %u materials, %llu triangles, "
                            "coverage %.1f%%, %.1f%% of covered pixels brighter than (20,20,20)\n",
                            label, c.stats.draws, c.stats.lit_draws, c.stats.ff_draws, c.stats.unlit_draws, c.stats.skinned_draws,
                            c.stats.transparent_draws, c.stats.materials, static_cast<unsigned long long>(c.stats.triangles),
                            c.coverage * 100.0, c.lit * 100.0);
            };
            auto save = [&](const char* file, const Capture& c) {
                const std::string png = src_dir + "/logs/" + file;
                CHECK(render::write_png_rgba(png, 512, 512, c.rgba));
                std::printf("  wrote %s\n", png.c_str());
            };

            // -- Shadow volumes (SPACE_AND_OPEN.md B.5): z-fail two-sided, colour write 0, Z LESS without write, cull
            // none, stencil ALWAYS ref 1 masks 0x3F; D3D9 CW faces (Vulkan back, pso.hpp) z-fail INCR (wrap), CCW faces
            // (Vulkan front) z-fail DECR (wrap), pass / fail KEEP; extrusion 200.
            for (const char* svname : {"MeshShadowVolume.fx", "RSkinShadowVolume.fx"}) {
                auto sv = effects.get(svname);
                CHECK(sv != nullptr);
                if (!sv) continue;
                const materials::ResolvedPass* dp = nullptr;
                for (const auto& p : sv->passes)
                    if (!p.restore_only && !dp) dp = &p;
                CHECK(dp != nullptr);
                if (!dp) continue;
                const materials::PsoKey& k = dp->pso[0];
                using SO = materials::StencilOp;
                const bool ok = k.color_write_mask == 0 && k.depth_test && !k.depth_write && k.depth_compare == materials::CompareOp::Less &&
                                k.cull == materials::CullMode::None && k.stencil_enable && k.back.depth_fail_op == SO::IncrementAndWrap &&
                                k.front.depth_fail_op == SO::DecrementAndWrap && k.back.pass_op == SO::Keep && k.front.pass_op == SO::Keep &&
                                k.back.fail_op == SO::Keep && k.front.fail_op == SO::Keep && k.back.compare == materials::CompareOp::Always &&
                                k.front.compare == materials::CompareOp::Always && dp->dynamic.stencil_ref == 1 &&
                                dp->dynamic.stencil_read_mask == 0x3F && dp->dynamic.stencil_write_mask == 0x3F;
                std::printf("shadow volume %s: technique %s, pass %s: colour mask %x, z %u/%u cmp %u, cull %u, stencil back zfail %u front zfail %u, "
                            "ref %u masks %x/%x, extrusion %.0f -> %s\n",
                            svname, sv->technique_name.c_str(), dp->name.c_str(), k.color_write_mask, k.depth_test, k.depth_write,
                            static_cast<unsigned>(k.depth_compare), static_cast<unsigned>(k.cull), static_cast<unsigned>(k.back.depth_fail_op),
                            static_cast<unsigned>(k.front.depth_fail_op), dp->dynamic.stencil_ref, dp->dynamic.stencil_read_mask,
                            dp->dynamic.stencil_write_mask, render::kShadowExtrusionDistance, ok ? "matches B.5" : "MISMATCH");
                CHECK(ok);
                CHECK(sv->technique_name == "t0_zfail");
            }
            CHECK(render::kShadowExtrusionDistance == 200.0f);

            render::ModelStreamer streamer(rr, cache, js);
            // -- Thrawn's Star Destroyer: lit and unlit.
            render::GpuModel* gm = load(streamer, "EV_STARDESTOYER_THRAWN.ALO");
            CHECK(gm != nullptr);
            {  // Scripted runs' blocking path: finish_pending makes a fresh request Ready without a polled frame.
                render::ModelStreamer blocking(rr, cache, js);
                const uint32_t h = blocking.request("EV_STARDESTOYER_THRAWN.ALO");
                blocking.finish_pending();
                const bool ready = blocking.state(h) == render::ModelStreamer::State::Ready;
                std::printf("model streamer finish_pending: %s\n", ready ? "Ready without a polled frame" : blocking.error(h).c_str());
                CHECK(ready);
                blocking.destroy_all();  // never drawn: nothing in flight reads it
            }
            if (gm) {
                const auto ps = rr.pipelines().stats();
                std::printf("thrawn: %u pipelines (%u failed, %.1f ms summed creation, %u reloaded from the cache dir)\n",
                            ps.keys, ps.failed, ps.create_ms, ps.reloaded);
                CHECK(gm->damage_meshes_hidden == 8);
                Capture lit, unlit;
                // The bounding sphere of a wedge is loose; get closer for the capture.
                // Lit by the Kuat sun (yaw 217 deg): look from the sun side (camera yaw 2.2 rad).
                // Shadow-volume / Heat submeshes have no unlit stand-in in any mode: none may appear in the last draw
                // list as an unlit (pass -1) draw (submesh 9 SHADOW / MeshShadowVolume used to draw white in Unlit).
                auto shadow_heat_standins = [&](const render::GpuModel& m, uint32_t& subs) {
                    auto no_standin = [](const render::GpuSubmesh& s) {
                        return s.effect && (s.bucket == render::PhaseBucket::Shadow || s.bucket == render::PhaseBucket::Heat);
                    };
                    subs = 0;
                    for (const render::GpuSubmesh& s : m.submeshes) subs += no_standin(s) ? 1u : 0u;
                    uint32_t n = 0;
                    for (const render::DrawListEntry& e : rr.last_draw_list()) {
                        if (e.pass >= 0 || e.instance != 0) continue;
                        for (const render::GpuSubmesh& s : m.submeshes)
                            if (s.submesh == e.submesh && no_standin(s)) ++n;
                    }
                    return n;
                };
                uint32_t sh_subs = 0;
                const render::ViewMatrices vm = capture(*gm, {}, 0.75f, lit, render::DrawMode::Lit, 0.35f, 2.2f);
                report("thrawn lit", lit);
                CHECK(lit.stats.pending_pipeline == 0 && lit.stats.lit_draws + lit.stats.ff_draws > 0);
                CHECK(lit.coverage > 0.10 && lit.coverage < 0.95 && lit.lit > 0.3);
                CHECK(shadow_heat_standins(*gm, sh_subs) == 0);
                save("render_thrawn_sd_lit.png", lit);
                capture(*gm, {}, 0.75f, unlit, render::DrawMode::Unlit);
                report("thrawn unlit", unlit);
                CHECK(unlit.coverage > 0.10 && unlit.lit > 0.5);
                const uint32_t unlit_standins = shadow_heat_standins(*gm, sh_subs);
                std::printf("thrawn unlit: %u shadow/heat submeshes, %u drawn by an unlit stand-in\n", sh_subs, unlit_standins);
                CHECK(sh_subs > 0 && unlit_standins == 0);
                CHECK(unlit.stats.draws == 3 && unlit.stats.triangles == 4017);
                save("render_thrawn_sd.png", unlit);
                render::Instance inst;
                inst.model = gm;
                const auto f0 = std::chrono::steady_clock::now();
                for (int i = 0; i < 60; ++i) render_frame(g, rr, o, vm, {&inst, 1}, gm);
                const double fms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - f0).count() / 60.0;
                std::printf("thrawn: %.3f ms per offscreen 512x512 lit frame (record + submit + wait + readback)\n", fms);
            }
            // -- A ship drawn mostly with MeshBumpColorize / MeshGloss (picked from alamo_render --sweep).
            render::GpuModel* s2 = load(streamer, kSecondShip);
            CHECK(s2 != nullptr);
            if (s2) {
                Capture c;
                capture(*s2, {}, 1.0f, c, render::DrawMode::Lit, 0.75f);  // long and flat: look from higher up
                report(kSecondShip, c);
                CHECK(c.stats.pending_pipeline == 0 && c.stats.lit_draws > 0 && c.coverage > 0.06 && c.lit > 0.3);
                save(kSecondShipPng, c);
            }
            // -- Royal Guard: rigid-skinned (RSkin*), bind pose vs an idle-animation frame (tick 30 = 1 s).
            render::GpuModel* rg = load(streamer, "EI_ROYALGAURD");
            std::shared_ptr<const assets::AnimationClip> clip = cache.animation("EI_ROYALGAURD_IDLE_00");
            CHECK(rg != nullptr && clip != nullptr);
            if (rg && clip) {
                Capture bind, posed;
                capture(*rg, {}, 1.0f, bind, render::DrawMode::Lit);
                report("royalguard bind", bind);
                const assets::Skeleton& sk = rg->asset->skeleton;
                assets::Pose pose;
                assets::sample(sk, *clip, 30.0f / 30.0f, true, pose);
                std::vector<Mat3x4> world(sk.size());
                assets::compute_world(sk, pose.local, world);
                capture(*rg, world, 1.0f, posed, render::DrawMode::Lit);
                report("royalguard posed", posed);
                uint64_t differ = 0;
                for (size_t i = 0; i < bind.rgba.size(); i += 4)
                    differ += (std::abs(bind.rgba[i] - posed.rgba[i]) + std::abs(bind.rgba[i + 1] - posed.rgba[i + 1]) +
                               std::abs(bind.rgba[i + 2] - posed.rgba[i + 2])) > 30;
                std::printf("royalguard: %.2f%% of pixels changed between bind pose and idle frame 30\n",
                            100.0 * static_cast<double>(differ) / (512.0 * 512.0));
                CHECK(bind.stats.skinned_draws > 0 && bind.stats.skinned_draws == bind.stats.draws);
                CHECK(bind.coverage > 0.05 && posed.coverage > 0.05 && posed.lit > 0.3);
                CHECK(differ > 100);
                save("render_royalguard_idle.png", posed);
            }
            // -- Opt-in missing-effect remap (alamo_render --compat-effect-remap): W_TEST_FISSURE.ALO "Line01" uses
            // FissureMeshGloss.fx (no FXO shipped): green D.3 default by default, MeshGloss with the remap. Each run has
            // its own EffectCache / AssetCache (model_load_options resolves through the same remap) / Renderer.
            for (const bool remap : {false, true}) {
                materials::LoadOptions lo = render::EffectCache::default_options();
                if (remap) lo.missing_effect_remap = materials::compat_missing_effect_remap();
                render::EffectCache fx(files.get(), lo);
                assets::AssetCache fcache(*files, fx.model_load_options());
                std::shared_ptr<const assets::ModelAsset> ma = fcache.model("W_TEST_FISSURE.ALO");
                if (!ma) {
                    std::printf("W_TEST_FISSURE.ALO: not in this install (EaW models.meg), remap test skipped\n");
                    break;
                }
                render::Renderer r2;
                render::RendererDesc d2 = rrd;
                d2.effects = &fx;
                d2.pipeline_cache_dir = std::filesystem::temp_directory_path() / "alamo_render_test_fissure_pso";
                CHECK(r2.init(g.ctx, g.alloc, g.up, d2));
                r2.set_frame_ring(&g.ring);
                r2.set_lighting(rr.lighting());
                render::GpuModel fm;
                std::string uerr;
                CHECK(r2.upload_model(ma, [](std::string_view, assets::TextureRole) { return std::shared_ptr<const assets::TextureAsset>(); },
                                      fm, nullptr, &uerr));
                r2.prepare_pipelines(fm);
                r2.wait_translations();
                r2.pipelines().wait_idle();
                const render::GpuSubmesh* line = nullptr;
                for (const render::GpuSubmesh& s : fm.submeshes)
                    if (s.mesh < ma->meshes.size() && render::effect_key(ma->meshes[s.mesh].name) == "LINE01") line = &s;
                CHECK(line != nullptr && line->effect != nullptr);
                if (line && line->effect) {
                    render::OrbitCamera cam;
                    const assets::Sphere sph = fm.drawn_sphere();
                    cam.frame(sph.center, sph.radius, 1.0f);
                    render::Instance fi;
                    fi.model = &fm;
                    render::DrawStats st;
                    CHECK(render_settled(g, r2, o, render::make_view(cam, 512, 512), {&fi, 1}, &fm, st));
                    bool drawn_lit = false;
                    for (const render::DrawListEntry& e : r2.last_draw_list())
                        drawn_lit = drawn_lit || (e.instance == 0 && e.submesh == line->submesh && e.pass >= 0);
                    const render::EffectCache::Stats es = fx.stats();
                    const assets::Submesh& as = ma->submeshes[line->submesh];
                    std::printf("W_TEST_FISSURE Line01 (%s, remap %s): effect %s (%s), bucket %s, layout %s%s, %s; %u draws; "
                                "EffectCache %zu defaulted / %zu remapped\n",
                                as.material.effect.c_str(), remap ? "on" : "off", line->effect->name.c_str(),
                                line->effect->is_default_effect() ? "green default" : "shipped", render::phase_bucket_name(line->bucket),
                                as.effective_vertex_format.c_str(), as.vertex_type_overridden ? " (overridden)" : "",
                                drawn_lit ? "drawn lit" : "not drawn lit", st.draws, es.defaulted, es.remapped);
                    CHECK(drawn_lit && st.post_pending == 0);
                    if (remap) {
                        CHECK(!line->effect->is_default_effect() && line->effect->name == "MESHGLOSS" &&
                              !line->effect->remapped_from.empty() && es.remapped >= 1);
                        // model_load_options went through the remap: Line01 is packed for MeshGloss's _ALAMO_VERTEX_TYPE (if any).
                        const materials::VertexLayout want = line->effect->vertex_type_override();
                        if (want != materials::VertexLayout::Unknown) CHECK(line->layout == want);
                    } else {
                        CHECK(line->effect->is_default_effect() && es.remapped == 0 && es.defaulted >= 1);
                    }
                }
                g.ring.wait_idle();
                r2.destroy_model(fm);
                r2.shutdown();
            }
            // -- C5 fleet: 200 ships from the install, single-threaded vs multithreaded recording.
            {
                const render::FleetModels fm = render::pick_fleet_models(*files, cache, js, 1, 6, 10, 0);  // no corvettes
                std::printf("fleet: %zu capital + %zu fighter types from %u candidates\n", fm.capitals.size(), fm.fighters.size(),
                            fm.candidates);
                std::vector<render::GpuModel*> types;
                for (const auto& n : fm.capitals) types.push_back(load(streamer, n.c_str()));
                for (const auto& n : fm.fighters) types.push_back(load(streamer, n.c_str()));
                const auto fleet = render::make_fleet(200, 1, static_cast<uint32_t>(fm.capitals.size()),
                                                      static_cast<uint32_t>(fm.fighters.size()));
                std::vector<render::Instance> inst;
                // The only translated passes in this fleet are MeshShield on the TIE Phantom's STEALTH mesh, which
                // is hidden by default (B.9); show every mesh so the identity check below covers translated draws.
                std::map<const render::GpuModel*, std::vector<uint8_t>> show_all;
                for (const render::GpuModel* t : types)
                    if (t && t->asset) show_all[t] = render::hidden_mesh_mask(*t->asset, nullptr);  // only the ALO-hidden stay hidden
                for (const auto& fi : fleet) {
                    if (fi.model >= types.size() || !types[fi.model]) continue;
                    render::Instance in;
                    in.model = types[fi.model];
                    in.mesh_hidden = show_all[in.model];
                    in.transform = fi.transform;
                    in.team_color_override = true;
                    in.team_color = fi.team;
                    inst.push_back(in);
                }
                CHECK(inst.size() >= 150 && !fm.capitals.empty() && !fm.fighters.empty());
                render::OrbitCamera cam;
                cam.frame({0, 0, 0}, render::fleet_radius(200), 1.0f);
                cam.pitch = 0.6f;
                cam.distance *= 0.7f;
                const render::ViewMatrices vm = render::make_view(cam, 512, 512);
                std::vector<uint8_t> ref;
                std::vector<render::DrawListEntry> ref_list;
                for (uint32_t t : {1u, std::max<uint32_t>(js.worker_count(), 2)}) {
                    render::RecordOptions ro;
                    ro.threads = t;
                    ro.min_draws_per_chunk = 32;
                    render::DrawStats st;
                    render::DrawTiming tm;
                    CHECK(render_settled(g, rr, o, vm, inst, nullptr, st, render::DrawMode::Lit, ro, &tm));
                    CHECK(st.post_pending == 0);
                    const uint8_t* px = static_cast<const uint8_t*>(o.readback.mapped());
                    std::vector<uint8_t> img(px, px + static_cast<size_t>(512) * 512 * 4);
                    std::printf("fleet 200: %u record threads: %u instances (%u culled), %u draws (%u hot-set, %u FF, %u translated, %u unlit), "
                                "%u materials, %llu triangles, %u secondaries, build %.2f ms, record %.2f ms\n",
                                t, st.instances, st.culled, st.draws, st.lit_draws, st.ff_draws, st.translated_draws, st.unlit_draws, st.materials,
                                static_cast<unsigned long long>(st.triangles), tm.chunks, tm.build_ms, tm.record_ms);
                    CHECK(st.pending_pipeline == 0 && st.translation_pending == 0 && st.draws > 200);
                    // (d) identity below covers translated draws (pick_fleet_models guarantees a type with a translated pass).
                    CHECK(st.translated_draws > 0);
                    if (t == 1) {
                        ref = img;
                        ref_list = rr.last_draw_list();
                    } else {
                        CHECK(tm.chunks > 1);
                        CHECK(rr.last_draw_list() == ref_list);
                        size_t diff = 0;
                        for (size_t i = 0; i < img.size(); ++i) diff += img[i] != ref[i];
                        std::printf("fleet 200: %zu bytes differ between 1 and %u record threads\n", diff, t);
                        CHECK(diff == 0);
                    }
                }
                // Q3: (a) one call per item gives the same pixels as the instanced runs; (b) no heap allocation in
                // render() per frame in steady state (post chain included: PostSpecs are views, images cached);
                // (c) the per-(material, pass) translated register images patched per draw equal the full fill.
                {
                    render::RecordOptions ro;
                    ro.threads = std::max<uint32_t>(js.worker_count(), 2);
                    ro.min_draws_per_chunk = 32;
                    ro.instancing = false;
                    render::DrawStats st;
                    CHECK(render_settled(g, rr, o, vm, inst, nullptr, st, render::DrawMode::Lit, ro));
                    const uint8_t* px = static_cast<const uint8_t*>(o.readback.mapped());
                    size_t diff = 0;
                    for (size_t i = 0; i < ref.size(); ++i) diff += px[i] != ref[i];
                    CHECK(rr.last_draw_list() == ref_list);
                    render::DrawStats si;
                    ro.instancing = true;
                    CHECK(render_settled(g, rr, o, vm, inst, nullptr, si, render::DrawMode::Lit, ro));
                    std::printf("fleet 200: instancing: %u draws -> %u calls (%u instanced, %u of them translated; %u translated draws); "
                                "off: %u calls, %zu bytes differ\n",
                                si.draws, si.draw_calls, si.instanced_calls, si.translated_instanced_calls, si.translated_draws, st.draw_calls,
                                diff);
                    CHECK(diff == 0 && st.draw_calls == st.draws && si.draw_calls < si.draws);
                    // as the mt test: 1 thread, parallel build inline, + secondaries, + secondaries and the post chain
                    for (int acfg = 0; acfg < 4; ++acfg) {
                        render::RecordOptions ra = ro;
                        ra.threads = acfg == 0 ? 1u : ro.threads;
                        ra.min_draws_per_chunk = acfg >= 2 ? 32u : 1u << 30;
                        ra.post = acfg == 3;
                        uint64_t worst = 0;
                        for (int k = 0; k < 24; ++k) {
                            g_measure_allocs = k >= 20;
                            CHECK(render_frame(g, rr, o, vm, inst, nullptr, nullptr, render::DrawMode::Lit, ra));
                            if (k >= 20) worst = std::max(worst, g_render_allocs);
                        }
                        g_measure_allocs = false;
                        std::printf("fleet 200: heap allocations in render(), steady state, %u threads%s: %llu per frame", ra.threads,
                                    acfg == 3 ? " + secondaries + post" : acfg == 2 ? " + secondaries" : (acfg == 1 ? " (inline recording)" : ""),
                                    static_cast<unsigned long long>(worst));
                        for (uint64_t k = 0; k < std::min<uint64_t>(worst, 8); ++k) std::printf("%s%zu B", k ? ", " : " (", g_alloc_sizes[k]);
                        std::printf("%s\n", worst ? ")" : "");
                        if (acfg == 0 || !kStdDequeNodeChurn) CHECK(worst == 0);
                    }
                    size_t checked = 0, mismatched = 0;
                    for (const render::Instance& in : inst)
                        for (const render::GpuSubmesh& gs : in.model->submeshes)
                            for (const render::GpuPass& gp : gs.passes) {
                                if (!gp.translated) continue;
                                materials::VSConstants va, vb;
                                materials::PSConstants pa, pb;
                                materials::IntBoolConstants ia, ib, ja, jb;
                                const bool fa = rr.translated_constants(in, gs, gp, vm, va, pa, &ia, &ja);
                                const bool fb = rr.translated_constants_cached(in, gs, gp, vm, vb, pb, &ib, &jb);
                                ++checked;
                                if (!fa || !fb || std::memcmp(&va, &vb, sizeof va) || std::memcmp(&pa, &pb, sizeof pa) ||
                                    std::memcmp(&ia, &ib, sizeof ia) || std::memcmp(&ja, &jb, sizeof ja))
                                    ++mismatched;
                            }
                    std::printf("fleet 200: translated register images, cached + per-draw patch vs full fill: %zu of %zu differ\n",
                                mismatched, checked);
                    CHECK(checked > 0 && mismatched == 0);
                }
                Capture c;
                c.rgba = ref;
                uint64_t covered = 0;
                for (size_t i = 0; i < ref.size(); i += 4) {
                    c.rgba[i + 3] = 255;
                    covered += !rgb_close(&ref[i], 0, 0, 64, 1);
                }
                std::printf("fleet 200: coverage %.1f%%\n", 100.0 * static_cast<double>(covered) / (512.0 * 512.0));
                CHECK(covered > 512 * 512 / 50);
                save("render_fleet_200.png", c);
            }
            // -- C4b follow-ups with the real FF effects: (a) MeshAlpha fading by LIGHT_SCALE.w on the GPU,
            // (b) vertex colour reaching an FF effect through COLOR0, (c) an RSkin mesh on an FF-fallback
            // effect follows its palette (ff_vs_n_skin) instead of drawing in bind pose.
            {
                auto white_tex = [](std::string_view name, assets::TextureRole) -> std::shared_ptr<const assets::TextureAsset> {
                    if (name != "white.dds") return nullptr;
                    auto t = std::make_shared<assets::TextureAsset>();
                    t->name = "white.dds";
                    t->format = assets::TextureFormat::RGBA8;
                    t->width = t->height = 1;
                    t->mip_levels = 1;
                    t->data = {255, 255, 255, 255};
                    t->levels.push_back({0, 0, 1, 1, 0, 4});
                    return t;
                };
                auto skel = [](assets::ModelAsset& m) {
                    m.skeleton.names = {"ROOT", "ARM"};
                    m.skeleton.parents = {-1, 0};
                    m.skeleton.visible = {1, 1};
                    m.skeleton.billboard_mode = {0, 0};
                    Mat3x4 arm;
                    arm.m[1][3] = -1.0f;
                    m.skeleton.bind_local = {Mat3x4{}, arm};
                    m.skeleton.finalize();
                    m.bounds = {{-1, -1, -1}, {1, 1, 1}};
                    m.sphere = {{0, 0, 0}, 1.8f};
                };
                const uint8_t white[4] = {255, 255, 255, 255}, red[4] = {255, 0, 0, 255};
                auto fa = std::make_shared<assets::ModelAsset>();  // (a) one MeshAlpha quad, instanced twice
                fa->name = "FF_FADE";
                skel(*fa);
                add_quad(*fa, "FADE", 0, -0.2f, 0.2f, -0.2f, 0.2f, 0.0f, white, "white.dds");
                fa->submeshes[0].material.effect = "MeshAlpha.fx";
                fa->submeshes[0].vertex_format = "alD3dVertNU2";  // no vertex colour: diffuse/alpha from the material (x LIGHT_SCALE)
                auto fb = std::make_shared<assets::ModelAsset>();  // (b) red vs white vertex colour, (c) skinned FF
                fb->name = "FF_COLOR_SKIN";
                skel(*fb);
                add_quad(*fb, "RED", 0, 0.3f, 0.7f, 0.3f, 0.7f, 0.0f, red, "white.dds");
                add_quad(*fb, "WHITE", 0, 0.3f, 0.7f, -0.7f, -0.3f, 0.0f, white, "white.dds");
                add_quad(*fb, "SKIN", 1, -0.15f, 0.15f, -0.15f, 0.15f, 0.0f, white, "white.dds", false, -1, false, true);
                for (auto& sm : fb->submeshes) sm.material.effect = "PrimAlpha.fx";
                fb->submeshes[2].material.effect = "MeshAlpha.fx";
                render::GpuModel ga, gb;
                CHECK(rr.upload_model(fa, white_tex, ga, &js));
                CHECK(rr.upload_model(fb, white_tex, gb, &js));
                for (render::GpuModel* m : {&ga, &gb}) {
                    for (const auto& t : m->buffer_tickets) CHECK(g.up.wait(t));
                    for (const auto& t : m->textures) CHECK(g.up.wait(t.ticket));
                    for (const auto& s : m->submeshes) CHECK(s.lit == render::LitStatus::Lit && !s.passes.empty() && s.passes[0].ff);
                }
                CHECK(!gb.submeshes.empty() && gb.submeshes[0].layout == materials::VertexLayout::NU2C);
                if (gb.submeshes.size() == 3) {
                    const materials::PsoKey sk = rr.draw_key_variant(gb.submeshes[2], gb.submeshes[2].passes[0], 0);
                    const materials::PsoKey ck = rr.draw_key_variant(gb.submeshes[0], gb.submeshes[0].passes[0], 0);
                    const materials::ShaderBlob* svs = render::shader_blob_by_id(sk.vs_id);
                    const materials::ShaderBlob* cvs = render::shader_blob_by_id(ck.vs_id);
                    std::printf("ff: RSkin mesh on MeshAlpha draws with %s, NU2C mesh on PrimAlpha with %s\n", svs ? svs->name : "?",
                                cvs ? cvs->name : "?");
                    CHECK(svs && std::string(svs->name) == "ff_vs_n_skin");
                    CHECK(cvs && std::string(cvs->name).find("ff_vs_nc") == 0);
                }
                std::vector<render::Instance> inst(3);
                inst[0].model = &ga;
                inst[0].transform.m[0][3] = -0.5f;
                inst[0].transform.m[2][3] = 0.5f;
                inst[1] = inst[0];
                inst[1].transform.m[2][3] = -0.5f;
                inst[1].light_scale = {1, 1, 1, 0.5f};  // fading
                inst[2].model = &gb;
                render::OrbitCamera cam;
                cam.yaw = -1.5707963f;
                cam.pitch = 0.0f;
                cam.distance = 5.0f;
                cam.radius = 2.0f;
                cam.fov_x = 2.0f * std::atan(0.2f);
                const render::ViewMatrices vm = render::make_view(cam, 512, 512);
                const render::Lighting saved_light = rr.lighting();
                render::Lighting front_light = render::Lighting::space_default();
                front_light.sun.direction = {0.0f, -1.0f, 0.3f};  // towards the camera side: the quads are lit
                rr.set_lighting(front_light);
                render::DrawStats st;
                for (int k = 0; k < 2; ++k) {  // frame 1 records the upload acquires; then everything resident
                    CHECK(render_frame(g, rr, o, vm, inst, &ga, &st));
                    rr.make_resident(gb);
                    rr.prepare_pipelines(ga);
                    rr.prepare_pipelines(gb);
                    rr.pipelines().wait_idle();
                }
                CHECK(render_frame(g, rr, o, vm, inst, nullptr, &st));
                CHECK(st.pending_pipeline == 0 && st.unlit_draws == 0 && st.ff_draws == 5);
                auto px = [&](uint32_t x, uint32_t y) {
                    const uint8_t* p = o.px(x, y);
                    return std::array<int, 3>{p[0], p[1], p[2]};
                };
                const auto opaque = px(128, 128), fading = px(128, 384), redq = px(384, 128), whiteq = px(384, 384);
                const auto skin_bind = px(256, 256);
                const int clear[3] = {0, 0, 64};
                std::printf("ff: MeshAlpha w=1 (%d,%d,%d), w=0.5 (%d,%d,%d); PrimAlpha red-vertex (%d,%d,%d) white-vertex (%d,%d,%d)\n",
                            opaque[0], opaque[1], opaque[2], fading[0], fading[1], fading[2], redq[0], redq[1], redq[2], whiteq[0],
                            whiteq[1], whiteq[2]);
                bool half = true;
                for (size_t ch = 0; ch < 3; ++ch) half = half && std::abs(fading[ch] - (opaque[ch] + clear[ch]) / 2) <= 12;
                CHECK(half && std::abs(opaque[0] - clear[0]) > 40);  // (a) alpha = LIGHT_SCALE.w x material alpha, blended
                CHECK(redq[0] > 100 && redq[1] < redq[0] / 3 && whiteq[1] > 100);  // (b) COLOR0 reaches the FF stages
                CHECK(!rgb_close(o.px(256, 256), 0, 0, 64, 4));                    // (c) bind pose: quad at the centre
                std::vector<Mat3x4> bw = gb.asset->skeleton.bind_world;
                bw[1].m[2][3] += 0.6f;  // pose: bone 1 up by 0.6 -> quad centre at z = 0.6 -> row 102
                inst[2].bone_world = bw;
                CHECK(render_frame(g, rr, o, vm, inst, nullptr, &st));
                const bool moved = rgb_close(o.px(256, 256), 0, 0, 64, 4) && !rgb_close(o.px(256, 102), 0, 0, 64, 4);
                std::printf("ff: skinned MeshAlpha quad bind (%d,%d,%d) -> posed centre clear %s, at z=0.6 drawn %s\n", skin_bind[0],
                            skin_bind[1], skin_bind[2], rgb_close(o.px(256, 256), 0, 0, 64, 4) ? "yes" : "no",
                            rgb_close(o.px(256, 102), 0, 0, 64, 4) ? "no" : "yes");
                CHECK(moved);  // (c) posed, not bind pose
                rr.set_lighting(saved_light);
                g.ring.wait_idle();
                rr.destroy_model(ga);
                rr.destroy_model(gb);
            }            // -- Track B on the GPU vs shaderxlat::Interpreter (same constants), plus the D3D9 half-pixel
            // convention: a quad edge at a quarter pixel must start covering where D3D9 would.
            {
                const char* kXlatEffect = "BatchMeshGloss.fx";
                auto fxp = effects.get(kXlatEffect);
                CHECK(fxp != nullptr);
                const materials::ResolvedPass* xpass = nullptr;
                if (fxp)
                    for (const auto& p : fxp->passes)
                        if (!p.restore_only && p.program_status == materials::ProgramStatus::Translated && !xpass) xpass = &p;
                CHECK(xpass != nullptr && rr.dxc_available());
                if (fxp && xpass && rr.dxc_available()) {
                    const uint8_t texel[4] = {180, 120, 60, 200};
                    auto const_tex = [&](std::string_view name, assets::TextureRole) -> std::shared_ptr<const assets::TextureAsset> {
                        if (name != "const.dds") return nullptr;
                        auto tx = std::make_shared<assets::TextureAsset>();
                        tx->name = "const.dds";
                        tx->format = assets::TextureFormat::RGBA8;
                        tx->width = tx->height = 1;
                        tx->mip_levels = 1;
                        tx->data.assign(texel, texel + 4);
                        tx->levels.push_back({0, 0, 1, 1, 0, 4});
                        return tx;
                    };
                    auto xm = std::make_shared<assets::ModelAsset>();
                    xm->name = "XLAT";
                    xm->skeleton.names = {"ROOT"};
                    xm->skeleton.parents = {-1};
                    xm->skeleton.visible = {1};
                    xm->skeleton.billboard_mode = {0};
                    xm->skeleton.bind_local = {Mat3x4{}};
                    xm->skeleton.finalize();
                    const uint8_t vcol[4] = {255, 255, 255, 255};
                    const float edge = 2.0f * 100.25f / 512.0f - 1.0f;  // world x of D3D9 pixel coordinate 100.25
                    add_quad(*xm, "CMP", 0, 0.1f, 0.9f, -0.9f, -0.1f, 0.0f, vcol, "const.dds");
                    add_quad(*xm, "EDGE", 0, edge, -0.05f, 0.05f, -edge, 0.0f, vcol, "const.dds");
                    for (auto& sm : xm->submeshes) {
                        sm.material.effect = kXlatEffect;
                        sm.vertex_format = "alD3dVertNU2U3U3";
                        sm.material.params.clear();
                        for (const auto& sb : xpass->samplers) {
                            if (sb.texture_param.empty()) continue;
                            assets::MaterialParam tpp;
                            tpp.name = sb.texture_param;
                            tpp.type = assets::ParamType::Texture;
                            tpp.texture = "const.dds";
                            sm.material.params.push_back(tpp);
                        }
                    }
                    xm->bounds = {{-1, -1, -1}, {1, 1, 1}};
                    xm->sphere = {{0, 0, 0}, 1.8f};
                    render::GpuModel gx;
                    CHECK(rr.upload_model(xm, const_tex, gx, &js));
                    for (const auto& t : gx.buffer_tickets) CHECK(g.up.wait(t));
                    for (const auto& t : gx.textures) CHECK(g.up.wait(t.ticket));
                    render::Instance xi;
                    xi.model = &gx;
                    render::OrbitCamera cam;
                    cam.yaw = -1.5707963f;
                    cam.pitch = 0.0f;
                    cam.distance = 5.0f;
                    cam.radius = 2.0f;
                    cam.fov_x = 2.0f * std::atan(0.2f);
                    const render::ViewMatrices vm = render::make_view(cam, 512, 512);
                    render::DrawStats st;
                    for (int k = 0; k < 3 && (k == 0 || st.translated_draws != 2); ++k) {
                        CHECK(render_frame(g, rr, o, vm, {&xi, 1}, &gx, &st));
                        rr.wait_translations();
                        rr.prepare_pipelines(gx);
                        rr.pipelines().wait_idle();
                    }
                    CHECK(render_frame(g, rr, o, vm, {&xi, 1}, &gx, &st));
                    std::printf("xlat: %s pass %s: %u translated draws (%u unlit, %u pending)\n", kXlatEffect, xpass->name.c_str(),
                                st.translated_draws, st.unlit_draws, st.translation_pending + st.pending_pipeline);
                    CHECK(st.translated_draws == 2 && st.unlit_draws == 0);
                    // Q3: the mesh path draws with the instanced form (storage-buffer register images); the CPU reference
                    // below checks its pixels.
                    const materials::TranslatedProgram* tp = rr.translated_mesh_program(gx.submeshes[0].effect, *xpass);
                    CHECK(tp != nullptr && tp->vs.iface.instanced && tp->ps.iface.instanced);
                    // (c) half-pixel sign: edge at D3D9 x = 100.25 / y = 100.25 -> first covered column / row 101.
                    const bool c100 = rgb_close(o.px(100, 200), 0, 0, 64, 1), c101 = !rgb_close(o.px(101, 200), 0, 0, 64, 1);
                    const bool r100 = rgb_close(o.px(200, 100), 0, 0, 64, 1), r101 = !rgb_close(o.px(200, 101), 0, 0, 64, 1);
                    std::printf("xlat: half-pixel offset (+1/W, -1/H): column 100 %s, 101 %s; row 100 %s, 101 %s\n",
                                c100 ? "clear" : "drawn", c101 ? "drawn" : "clear", r100 ? "clear" : "drawn", r101 ? "drawn" : "clear");
                    CHECK(c100 && c101 && r100 && r101);
                    if (tp) {
                        // (b) CPU reference: VS on the quad's vertices, perspective-correct interpolation at pixel
                        // centres, PS with the same constant images and a constant texture.
                        shaderxlat::Program vsp, psp;
                        std::string perr;
                        CHECK(shaderxlat::parse(xpass->vs_bytecode, vsp, &perr) && shaderxlat::parse(xpass->ps_bytecode, psp, &perr));
                        shaderxlat::Interpreter vsi, psi;
                        CHECK(vsi.init(vsp, xpass->vs_options, &perr) && psi.init(psp, xpass->ps_options, &perr));
                        materials::VSConstants vsc;
                        materials::PSConstants psc;
                        CHECK(rr.translated_constants(xi, gx.submeshes[0], gx.submeshes[0].passes[0], vm, vsc, psc));
                        {  // Q3: template + per-draw patch == full fill (other transform, LIGHT_SCALE, team colour too)
                            render::Instance x2 = xi;
                            x2.transform.m[0][3] = 3.0f;
                            x2.transform.m[0][0] = 0.5f;
                            x2.light_scale = {0.5f, 0.75f, 1.0f, 0.6f};
                            x2.team_color_override = true;
                            x2.team_color = {0.1f, 0.9f, 0.3f, 1.0f};
                            bool same = true;
                            for (const render::Instance* xp : {&xi, &x2}) {
                                materials::VSConstants a, b;
                                materials::PSConstants pa, pb;
                                CHECK(rr.translated_constants(*xp, gx.submeshes[0], gx.submeshes[0].passes[0], vm, a, pa));
                                CHECK(rr.translated_constants_cached(*xp, gx.submeshes[0], gx.submeshes[0].passes[0], vm, b, pb));
                                same = same && std::memcmp(&a, &b, sizeof a) == 0 && std::memcmp(&pa, &pb, sizeof pa) == 0;
                            }
                            std::printf("xlat: cached register images + per-draw patch %s the full fill\n", same ? "equal" : "DIFFER FROM");
                            CHECK(same);
                        }
                        shaderxlat::ConstantState csv, csp;
                        for (size_t k = 0; k < 256; ++k) csv.c[k] = {vsc.c[k].x, vsc.c[k].y, vsc.c[k].z, vsc.c[k].w};
                        for (size_t k = 0; k < 32; ++k) csp.c[k] = {psc.c[k].x, psc.c[k].y, psc.c[k].z, psc.c[k].w};
                        shaderxlat::TranslatedPush push = xpass->translated_push;
                        render::Renderer::translated_pixel_offset(512, 512, push.pixel_offset);
                        struct ConstTex : shaderxlat::TextureSource {
                            shaderxlat::F4 c;
                            shaderxlat::F4 sample(uint32_t, shaderxlat::TextureDim d, const shaderxlat::F4&) const override {
                                return d == shaderxlat::TextureDim::Cube ? shaderxlat::F4{1, 1, 1, 1} : c;
                            }
                        } ctex;
                        ctex.c = {texel[0] / 255.0f, texel[1] / 255.0f, texel[2] / 255.0f, texel[3] / 255.0f};
                        const assets::Submesh& sm = xm->submeshes[0];
                        shaderxlat::VsResult vr[3];
                        for (int vi = 0; vi < 3; ++vi) {
                            assets::StaticVertex v;
                            std::memcpy(&v, sm.vertices.data() + vi * 64, 64);
                            std::array<shaderxlat::F4, 16> in{};
                            for (const auto& d : tp->vs.iface.inputs) {
                                shaderxlat::F4 val{0, 0, 0, 0};  // unfed inputs read the zero buffer
                                switch (d.location) {
                                case 0: val = {v.position[0], v.position[1], v.position[2], 1}; break;
                                case 1: val = {v.normal[0], v.normal[1], v.normal[2], 1}; break;
                                case 2: val = {v.uv0[0], v.uv0[1], 0, 1}; break;
                                case 3: val = {v.tangent[0], v.tangent[1], v.tangent[2], 1}; break;
                                case 4: val = {v.binormal[0], v.binormal[1], v.binormal[2], 1}; break;
                                case 5: val = {v.color[0] / 255.0f, v.color[1] / 255.0f, v.color[2] / 255.0f, v.color[3] / 255.0f}; break;
                                default: break;
                                }
                                if (d.reg < 16) in[d.reg] = val;
                            }
                            CHECK(vsi.run_vs(in, csv, push, vr[vi]));
                        }
                        // Screen positions (negative-height viewport: y_fb = (1 - y_ndc) / 2 * H).
                        double sx[3], sy[3], iw[3];
                        for (int vi = 0; vi < 3; ++vi) {
                            const double w = vr[vi].pos[3];
                            iw[vi] = 1.0 / w;
                            sx[vi] = (vr[vi].pos[0] / w + 1.0) * 0.5 * 512.0;
                            sy[vi] = (1.0 - vr[vi].pos[1] / w) * 0.5 * 512.0;
                        }
                        const double cx = (sx[0] + sx[1] + sx[2]) / 3, cy = (sy[0] + sy[1] + sy[2]) / 3;
                        double max_err = 0;
                        int compared = 0;
                        for (int k = 0; k < 4; ++k) {
                            // centroid and points half-way to each vertex, snapped to pixel centres
                            const double tx = k == 0 ? cx : 0.5 * (cx + sx[k - 1]), ty = k == 0 ? cy : 0.5 * (cy + sy[k - 1]);
                            const uint32_t px = static_cast<uint32_t>(tx), py = static_cast<uint32_t>(ty);
                            const double pxc = px + 0.5, pyc = py + 0.5;
                            const double den = (sy[1] - sy[2]) * (sx[0] - sx[2]) + (sx[2] - sx[1]) * (sy[0] - sy[2]);
                            double l0 = ((sy[1] - sy[2]) * (pxc - sx[2]) + (sx[2] - sx[1]) * (pyc - sy[2])) / den;
                            double l1 = ((sy[2] - sy[0]) * (pxc - sx[2]) + (sx[0] - sx[2]) * (pyc - sy[2])) / den;
                            double l2 = 1.0 - l0 - l1;
                            const double p0 = l0 * iw[0], p1 = l1 * iw[1], p2 = l2 * iw[2], ps = p0 + p1 + p2;
                            const double b[3] = {p0 / ps, p1 / ps, p2 / ps};
                            shaderxlat::PsInputs pin;
                            auto lerp4 = [&](auto get) {
                                shaderxlat::F4 r{};
                                for (int ch = 0; ch < 4; ++ch)
                                    r[static_cast<size_t>(ch)] = static_cast<float>(b[0] * get(vr[0])[static_cast<size_t>(ch)] +
                                                                                    b[1] * get(vr[1])[static_cast<size_t>(ch)] +
                                                                                    b[2] * get(vr[2])[static_cast<size_t>(ch)]);
                                return r;
                            };
                            pin.color[0] = lerp4([](const shaderxlat::VsResult& r) { return r.color0; });
                            pin.color[1] = lerp4([](const shaderxlat::VsResult& r) { return r.color1; });
                            for (size_t tc = 0; tc < 8; ++tc)
                                pin.texcoord[tc] = lerp4([tc](const shaderxlat::VsResult& r) { return r.texcoord[tc]; });
                            pin.fog = static_cast<float>(b[0] * vr[0].fog + b[1] * vr[1].fog + b[2] * vr[2].fog);
                            pin.frag_pos = {static_cast<float>(pxc), static_cast<float>(pyc), 0.5f, static_cast<float>(ps)};
                            shaderxlat::PsResult pr;
                            CHECK(psi.run_ps(pin, csp, push, &ctex, pr));
                            if (pr.killed) continue;
                            const uint8_t* gp = o.px(px, py);
                            for (int ch = 0; ch < 3; ++ch) {
                                const double ref = std::clamp(static_cast<double>(pr.color[0][static_cast<size_t>(ch)]), 0.0, 1.0) * 255.0;
                                max_err = std::max(max_err, std::fabs(gp[ch] - ref));
                            }
                            ++compared;
                        }
                        std::printf("xlat: GPU vs interpreter on %d pixels: max error %.2f / 255 (blend %s)\n", compared, max_err,
                                    tp->key(xpass->pso[0]).blend_enable ? "on" : "off");
                        CHECK(compared >= 3 && max_err <= 2.5);
                    }
                    g.ring.wait_idle();
                    rr.destroy_model(gx);
                }
            }
            // -- (e) environments from the maps (TEE records through the VFS): Kuat space and Endor land.
            {
                render::MapEnvironments kuat, endor;
                std::string eerr;
                CHECK(render::load_map_environments(*files, "_MP_SPACE_KUAT", kuat, &eerr));
                CHECK(render::load_map_environments(*files, "_MP_LAND_ENDOR", endor, &eerr));
                auto near3 = [](const float (&v)[3], float a, float b, float c) {
                    return near_eq(v[0], a, 2e-3f) && near_eq(v[1], b, 2e-3f) && near_eq(v[2], c, 2e-3f);
                };
                if (const render::Environment* k = kuat.start()) {
                    std::printf("env: Kuat %zu records, start \"%s\": sun (%.3f,%.3f,%.3f) x %.3f yaw %.1f elev %.1f ambient (%.3f,%.3f,%.3f) "
                                "bloom %.2f/%.2f/%.2f space %d\n",
                                kuat.environments.size(), k->name.c_str(), k->light_color[0][0], k->light_color[0][1], k->light_color[0][2],
                                k->intensity[0], k->yaw[0] * 57.29578f, k->elevation[0] * 57.29578f, k->ambient[0], k->ambient[1],
                                k->ambient[2], k->bloom_strength, k->bloom_cutoff, k->bloom_size, kuat.space ? 1 : 0);
                    CHECK(kuat.space && near3(k->light_color[0], 0.918f, 1.0f, 0.988f) && near_eq(k->intensity[0], 0.646f, 2e-3f));
                    CHECK(near_eq(k->yaw[0] * 57.29578f, 217.1f, 0.1f) && near_eq(k->elevation[0] * 57.29578f, 12.5f, 0.1f));
                    CHECK(near3(k->ambient, 0.027f, 0.051f, 0.055f) && near_eq(k->bloom_strength, 1.0f) && near_eq(k->bloom_size, 0.25f));
                    // g1f t06 capture (Kuat, Admonitor): BloomCutoff 0.9 and the soft-shadow resolve quad's vertex grey 0x8B (the
                    // prim vertices carry the scene shadow colour, TEE 17, truncated to a byte as in Renderer::post_darken).
                    unsigned sh8[3] = {};
                    for (size_t sci = 0; sci < 3; ++sci)
                        sh8[sci] = static_cast<unsigned>(static_cast<uint8_t>(std::clamp(k->shadow_color[sci], 0.0f, 1.0f) * 255.0f));
                    std::printf("env: Kuat cutoff %.3f, shadow colour (%.7f,%.7f,%.7f) -> 0x%02X%02X%02X\n", k->bloom_cutoff,
                                k->shadow_color[0], k->shadow_color[1], k->shadow_color[2], sh8[0], sh8[1], sh8[2]);
                    CHECK(near_eq(k->bloom_cutoff, 0.9f) && sh8[0] == 0x8B && sh8[1] == 0x8B && sh8[2] == 0x8B);
                    const render::Lighting kl = render::Lighting::from_environment(*k, kuat.space);
                    const render::Lighting kd = render::Lighting::space_default();
                    CHECK(kl.fill[0].intensity == 0.0f && near_eq(kl.sun.intensity, kd.sun.intensity, 2e-3f) &&
                          near_eq(kl.sun.direction.x, kd.sun.direction.x, 2e-3f) && near_eq(kl.ambient.y, kd.ambient.y, 2e-3f));

                }
                if (const render::Environment* e = endor.start()) {
                    std::printf("env: Endor %zu records, start \"%s\": sun (%.3f,%.3f,%.3f) x %.3f yaw %.1f elev %.1f fills %.2f/%.2f fog %d "
                                "%.0f..%.0f\n",
                                endor.environments.size(), e->name.c_str(), e->light_color[0][0], e->light_color[0][1], e->light_color[0][2],
                                e->intensity[0], e->yaw[0] * 57.29578f, e->elevation[0] * 57.29578f, e->intensity[1], e->intensity[2],
                                e->fog_enabled ? 1 : 0, e->fog_start, e->fog_end);
                    CHECK(!endor.space && e->name == "Sunrise_Clear" && near3(e->light_color[0], 1.0f, 0.729f, 0.357f));
                    CHECK(near_eq(e->intensity[0], 0.646f, 2e-3f) && near_eq(e->yaw[0] * 57.29578f, 220.0f, 0.1f) &&
                          near_eq(e->elevation[0] * 57.29578f, 33.0f, 0.1f));
                    CHECK(near3(e->ambient, 0.145f, 0.184f, 0.188f) && near_eq(e->intensity[1], 0.33f, 2e-3f) &&
                          near_eq(e->intensity[2], 0.5f, 2e-3f) && e->fog_enabled && near_eq(e->fog_end, 3500.0f, 1.0f));
                    CHECK(endor.environments.size() == 4);
                    const render::Lighting el = render::Lighting::from_environment(*e, endor.space);
                    CHECK(el.fill[0].intensity > 0.3f && el.fill[1].intensity > 0.4f);
                }
            }            // -- A model drawn with translated effects (alamo_render --sweep: most translated submeshes on a ship).
            {
                const char* kXlatModel = "UV_CRUSADERCLASSCORVETTE_DC.ALO";
                render::GpuModel* xm = load(streamer, kXlatModel);
                CHECK(xm != nullptr);
                if (xm) {
                    Capture c;
                    capture(*xm, {}, 0.9f, c, render::DrawMode::Lit, 0.5f, 2.2f);
                    report(kXlatModel, c);
                    std::printf("  %u translated draws\n", c.stats.translated_draws);
                    CHECK(c.stats.translated_draws > 0 && c.stats.pending_pipeline == 0 && c.stats.translation_pending == 0);
                    CHECK(c.coverage > 0.03);
                    save("render_uv_crusaderclasscorvette_dc_translated.png", c);
                }
            }            // ==== Frame graph: stencil shadows, bloom, heat (synthetic scenes on the real FXOs), plus the shots.
            auto empty_model = [](const char* name) {
                auto m = std::make_shared<assets::ModelAsset>();
                m->name = name;
                m->skeleton.names = {"ROOT"};
                m->skeleton.parents = {-1};
                m->skeleton.visible = {1};
                m->skeleton.billboard_mode = {0};
                m->skeleton.bind_local = {Mat3x4{}};
                m->skeleton.finalize();
                return m;
            };
            auto add_mesh = [](assets::ModelAsset& m, const char* name, const std::vector<assets::StaticVertex>& verts,
                               const std::vector<uint16_t>& idx, const char* effect, const char* format, uint32_t flags,
                               const char* texture_param = nullptr, const char* texture = nullptr) {
                assets::MeshInfo mi;
                mi.name = name;
                mi.first_submesh = static_cast<uint32_t>(m.submeshes.size());
                mi.submesh_count = 1;
                m.meshes.push_back(mi);
                assets::Submesh sm;
                sm.mesh = static_cast<uint32_t>(m.meshes.size() - 1);
                sm.flags = flags;
                sm.vertex_format = format;
                sm.layout = assets::VertexLayout::Static;
                sm.vertex_count = static_cast<uint32_t>(verts.size());
                sm.vertices.resize(verts.size() * 64);
                std::memcpy(sm.vertices.data(), verts.data(), sm.vertices.size());
                sm.index_type = assets::IndexType::U16;
                sm.index_count = static_cast<uint32_t>(idx.size());
                sm.indices.resize(idx.size() * 2);
                std::memcpy(sm.indices.data(), idx.data(), sm.indices.size());
                assets::Aabb bb{{1e9f, 1e9f, 1e9f}, {-1e9f, -1e9f, -1e9f}};
                for (const auto& v : verts) {
                    bb.min = {std::min(bb.min.x, v.position[0]), std::min(bb.min.y, v.position[1]), std::min(bb.min.z, v.position[2])};
                    bb.max = {std::max(bb.max.x, v.position[0]), std::max(bb.max.y, v.position[1]), std::max(bb.max.z, v.position[2])};
                }
                sm.bounds = bb;
                if (effect) sm.material.effect = effect;
                if (texture_param) {
                    assets::MaterialParam p;
                    p.name = texture_param;
                    p.type = assets::ParamType::Texture;
                    p.texture = texture;
                    sm.material.params.push_back(p);
                }
                m.submeshes.push_back(std::move(sm));
            };
            auto vtx = [](float x, float y, float z, float nx, float ny, float nz, uint8_t grey = 255, float u = 0, float v = 0) {
                assets::StaticVertex sv{};
                sv.position[0] = x;
                sv.position[1] = y;
                sv.position[2] = z;
                sv.normal[0] = nx;
                sv.normal[1] = ny;
                sv.normal[2] = nz;
                sv.uv0[0] = u;
                sv.uv0[1] = v;
                sv.tangent[0] = 1;
                sv.binormal[2] = 1;
                sv.color[0] = sv.color[1] = sv.color[2] = grey;
                sv.color[3] = 255;
                return sv;
            };
            // Ground plane z = 0 (unlit, grey 200), counter-clockwise seen from +Z.
            auto add_plane = [&](assets::ModelAsset& m, float half) {
                add_mesh(m, "GROUND", {vtx(-half, -half, 0, 0, 0, 1, 200), vtx(half, -half, 0, 0, 0, 1, 200), vtx(half, half, 0, 0, 0, 1, 200),
                                       vtx(-half, half, 0, 0, 0, 1, 200)},
                         {0, 1, 2, 0, 2, 3}, nullptr, "alD3dVertNU2C", 0);
            };
            // Closed shadow volume of a box: faces with their normals + degenerate fin quads along every edge
            // (the exporter's layout), drawn with MeshShadowVolume (z-fail, two-sided stencil).
            auto add_box_volume = [&](assets::ModelAsset& m, const float (&lo)[3], const float (&hi)[3]) {
                struct Face {
                    float v[4][3];
                    float n[3];
                };
                std::vector<Face> faces;
                for (int axis = 0; axis < 3; ++axis)
                    for (int side = 0; side < 2; ++side) {
                        Face f{};
                        const int u = (axis + 1) % 3, w = (axis + 2) % 3;
                        const float uu[4] = {lo[u], hi[u], hi[u], lo[u]}, ww[4] = {lo[w], lo[w], hi[w], hi[w]};
                        for (int k = 0; k < 4; ++k) {
                            f.v[k][axis] = side ? hi[axis] : lo[axis];
                            f.v[k][u] = uu[k];
                            f.v[k][w] = ww[k];
                        }
                        f.n[axis] = side ? 1.0f : -1.0f;
                        const float a[3] = {f.v[1][0] - f.v[0][0], f.v[1][1] - f.v[0][1], f.v[1][2] - f.v[0][2]};
                        const float b[3] = {f.v[2][0] - f.v[0][0], f.v[2][1] - f.v[0][1], f.v[2][2] - f.v[0][2]};
                        const float cr[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
                        if (cr[0] * f.n[0] + cr[1] * f.n[1] + cr[2] * f.n[2] < 0) {  // make it CCW around the outward normal
                            for (int k = 0; k < 3; ++k) std::swap(f.v[1][k], f.v[3][k]);
                        }
                        faces.push_back(f);
                    }
                std::vector<assets::StaticVertex> verts;
                std::vector<uint16_t> idx;
                for (const Face& f : faces) {
                    const uint16_t base = static_cast<uint16_t>(verts.size());
                    for (int k = 0; k < 4; ++k) verts.push_back(vtx(f.v[k][0], f.v[k][1], f.v[k][2], f.n[0], f.n[1], f.n[2]));
                    for (int i : {0, 1, 2, 0, 2, 3}) idx.push_back(static_cast<uint16_t>(base + i));
                }
                auto same = [](const float (&p)[3], const float (&q)[3]) { return p[0] == q[0] && p[1] == q[1] && p[2] == q[2]; };
                for (size_t a = 0; a < faces.size(); ++a)
                    for (int k = 0; k < 4; ++k) {
                        const float(&P)[3] = faces[a].v[k];
                        const float(&Q)[3] = faces[a].v[(k + 1) % 4];
                        for (size_t bf = a + 1; bf < faces.size(); ++bf)
                            for (int j = 0; j < 4; ++j)
                                if (same(faces[bf].v[j], Q) && same(faces[bf].v[(j + 1) % 4], P)) {
                                    // fin loop (Q_A, P_A, P_B, Q_B): consistent with A on either side of the silhouette
                                    const uint16_t qa = static_cast<uint16_t>(a * 4 + static_cast<size_t>((k + 1) % 4)),
                                                   pa = static_cast<uint16_t>(a * 4 + static_cast<size_t>(k)),
                                                   pb = static_cast<uint16_t>(bf * 4 + static_cast<size_t>((j + 1) % 4)),
                                                   qb = static_cast<uint16_t>(bf * 4 + static_cast<size_t>(j));
                                    for (uint16_t i : {qa, pa, pb, qa, pb, qb}) idx.push_back(i);
                                }
                    }
                add_mesh(m, "BOX_SHADOW", verts, idx, "MeshShadowVolume.fx", "alD3dVertN", assets::kSubmeshShadowVolume);
            };
            auto project = [](const render::ViewMatrices& v, float x, float y, float z, uint32_t w, uint32_t h) {
                const render::Float4 cl = v.view_proj * render::Float4{x, y, z, 1.0f};
                return std::array<uint32_t, 2>{static_cast<uint32_t>((cl.x / cl.w + 1.0f) * 0.5f * static_cast<float>(w)),
                                               static_cast<uint32_t>((1.0f - cl.y / cl.w) * 0.5f * static_cast<float>(h))};
            };
            auto upload_wait = [&](std::shared_ptr<assets::ModelAsset> m, const render::TextureSource& texs, render::GpuModel& sg) {
                CHECK(rr.upload_model(m, texs, sg, &js));
                for (const auto& t : sg.buffer_tickets) CHECK(g.up.wait(t));
                for (const auto& t : sg.textures) CHECK(g.up.wait(t.ticket));
            };
            auto settle = [&](const render::ViewMatrices& v, std::span<const render::Instance> in, render::GpuModel& sg,
                              const render::RecordOptions& ro) {
                // Upload acquires + residency, then every translation / pipeline the frame graph asks for.
                render::DrawStats st;
                for (int k = 0; k < 4; ++k) {
                    CHECK(render_frame(g, rr, o, v, in, &sg, &st, render::DrawMode::Lit, ro));
                    rr.wait_translations();
                    rr.prepare_pipelines(sg);
                    rr.pipelines().wait_idle();
                }
            };
            const render::Lighting kuat_light = rr.lighting();
            auto none_tex = [](std::string_view, assets::TextureRole) -> std::shared_ptr<const assets::TextureAsset> { return nullptr; };
            {   // (a) stencil shadow volume over a plane, hard and soft darken
                auto m = empty_model("SHADOW_SCENE");
                add_plane(*m, 6.0f);
                const float lo[3] = {-0.5f, -0.5f, 0.5f}, hi[3] = {0.5f, 0.5f, 1.5f};
                add_box_volume(*m, lo, hi);
                m->bounds = {{-6, -6, 0}, {6, 6, 1.5f}};
                m->sphere = {{0, 0, 0.75f}, 8.5f};
                render::GpuModel sg;
                upload_wait(m, none_tex, sg);
                render::Lighting land = render::Lighting::land_default();
                land.sun.direction = {1.0f, 0.0f, 1.0f};  // towards the light: the shadow falls towards -X
                land.fog_enabled = false;
                land.shadow_color = {0.5f, 0.5f, 0.5f};
                rr.set_lighting(land);
                render::OrbitCamera cam;
                cam.target = {-0.5f, 0.0f, 0.3f};
                cam.yaw = -1.5707963f;
                cam.pitch = 0.7f;
                cam.distance = 9.0f;
                cam.radius = 300.0f;  // far plane beyond the 200-unit extrusion (z-fail needs the back caps)
                const render::ViewMatrices v = render::make_view(cam, 512, 512);
                render::Instance in;
                in.model = &sg;
                render::RecordOptions ro;
                ro.bloom_strength = 0.0f;  // isolate the darken (bloom would lift both sides)
                settle(v, {&in, 1}, sg, ro);
                ro.soft_shadows = true;
                settle(v, {&in, 1}, sg, ro);
                for (float sc : {0.0f, 1.0f}) {
                    render::Lighting l2 = land;
                    l2.shadow_color = {sc, sc, sc};
                    rr.set_lighting(l2);
                    render::DrawStats st;
                    CHECK(render_frame(g, rr, o, v, {&in, 1}, nullptr, &st, render::DrawMode::Lit, ro));
                    const auto p0 = project(v, -1.2f, 0.0f, 0.0f, 512, 512);
                    const uint8_t* q = o.px(p0[0], p0[1]);
                    std::printf("soft shadow, shadow colour %.1f: shadowed px (%d,%d,%d)\n", sc, q[0], q[1], q[2]);
                    CHECK(std::abs(q[0] - static_cast<int>(200.0f * sc)) <= 2);  // darken = back buffer x shadow colour
                }
                rr.set_lighting(land);
                const auto sp = project(v, -1.2f, 0.0f, 0.0f, 512, 512), lp = project(v, 1.6f, 0.0f, 0.0f, 512, 512);
                int shadowed[3], lit[3];
                for (int soft = 0; soft < 3; ++soft) {  // 0 hard, 1 soft, 2 shadows off
                    render::RecordOptions r2;
                    r2.soft_shadows = soft == 1;
                    r2.shadows = soft != 2;
                    r2.bloom_strength = 0.0f;
                    render::DrawStats st;
                    CHECK(render_settled(g, rr, o, v, {&in, 1}, nullptr, st, render::DrawMode::Lit, r2));
                    CHECK(st.post_pending == 0);
                    const uint8_t* ps = o.px(sp[0], sp[1]);
                    const uint8_t* pl = o.px(lp[0], lp[1]);
                    for (int ch = 0; ch < 3; ++ch) {
                        shadowed[ch] = ps[ch];
                        lit[ch] = pl[ch];
                    }
                    std::printf("shadow %s: %u volumes, darken %s; shadowed px (%u,%u) = (%d,%d,%d), lit px (%u,%u) = (%d,%d,%d)\n",
                                soft == 0 ? "hard" : soft == 1 ? "soft" : "off", st.shadow_volumes, st.darkened ? "yes" : "no", sp[0],
                                sp[1], shadowed[0], shadowed[1], shadowed[2], lp[0], lp[1], lit[0], lit[1], lit[2]);
                    CHECK(std::abs(lit[0] - 200) <= 2 && std::abs(lit[1] - 200) <= 2);  // lit ground untouched
                    if (soft < 2) {
                        CHECK(st.shadow_volumes == 1 && st.darkened && st.soft_shadows == (soft == 1));
                        CHECK(std::abs(shadowed[0] - 100) <= 3 && std::abs(shadowed[2] - 100) <= 3);  // 200 x shadow colour 0.5
                    } else {
                        CHECK(std::abs(shadowed[0] - 200) <= 2);
                    }
                    if (soft == 0) {
                        Capture c;
                        c.rgba.resize(static_cast<size_t>(512) * 512 * 4);
                        for (size_t i = 0; i < c.rgba.size(); i += 4) {
                            const uint8_t* p = static_cast<const uint8_t*>(o.readback.mapped()) + i;
                            c.rgba[i] = p[0];
                            c.rgba[i + 1] = p[1];
                            c.rgba[i + 2] = p[2];
                            c.rgba[i + 3] = 255;
                        }
                        save("render_shadow_volume_test.png", c);
                    }
                }
                g.ring.wait_idle();
                rr.destroy_model(sg);
                rr.set_lighting(kuat_light);
            }
            {   // (a2) playtest 3: soft shadows darkened the whole land frame. The blur reads the frame's ALPHA as the shadow
                // mask; terrain / units write texture alpha < 1 outside the shadow. Ground with vertex alpha 100: the soft
                // frame must equal the hard frame at an unshadowed point, and still darken the shadowed one.
                auto m = empty_model("SHADOW_ALPHA_SCENE");
                {
                    const float half = 6.0f;
                    std::vector<assets::StaticVertex> pv = {vtx(-half, -half, 0, 0, 0, 1, 200), vtx(half, -half, 0, 0, 0, 1, 200),
                                                            vtx(half, half, 0, 0, 0, 1, 200), vtx(-half, half, 0, 0, 0, 1, 200)};
                    for (auto& v : pv) v.color[3] = 100;
                    add_mesh(*m, "GROUND_A", pv, {0, 1, 2, 0, 2, 3}, nullptr, "alD3dVertNU2C", 0);
                }
                const float lo[3] = {-0.5f, -0.5f, 0.5f}, hi[3] = {0.5f, 0.5f, 1.5f};
                add_box_volume(*m, lo, hi);
                m->bounds = {{-6, -6, 0}, {6, 6, 1.5f}};
                m->sphere = {{0, 0, 0.75f}, 8.5f};
                render::GpuModel sg;
                upload_wait(m, none_tex, sg);
                render::Lighting land = render::Lighting::land_default();
                land.sun.direction = {1.0f, 0.0f, 1.0f};
                land.fog_enabled = false;
                land.shadow_color = {0.5f, 0.5f, 0.5f};
                rr.set_lighting(land);
                render::OrbitCamera cam;
                cam.target = {-0.5f, 0.0f, 0.3f};
                cam.yaw = -1.5707963f;
                cam.pitch = 0.7f;
                cam.distance = 9.0f;
                cam.radius = 300.0f;
                const render::ViewMatrices v = render::make_view(cam, 512, 512);
                render::Instance in;
                in.model = &sg;
                const auto sp = project(v, -1.2f, 0.0f, 0.0f, 512, 512), lp = project(v, 1.6f, 0.0f, 0.0f, 512, 512);
                int shadowed[2] = {}, lit[2] = {};
                for (int soft = 0; soft < 2; ++soft) {
                    render::RecordOptions r2;
                    r2.soft_shadows = soft == 1;
                    r2.bloom_strength = 0.0f;
                    render::DrawStats st;
                    settle(v, {&in, 1}, sg, r2);
                    CHECK(render_settled(g, rr, o, v, {&in, 1}, nullptr, st, render::DrawMode::Lit, r2));
                    CHECK(st.post_pending == 0 && st.darkened && st.soft_shadows == (soft == 1));
                    shadowed[soft] = o.px(sp[0], sp[1])[0];
                    lit[soft] = o.px(lp[0], lp[1])[0];
                    std::printf("shadow on ground with alpha 100, %s: shadowed %d, lit %d\n", soft ? "soft" : "hard", shadowed[soft], lit[soft]);
                }
                CHECK(std::abs(lit[0] - 200) <= 2);                // hard: lit ground untouched
                CHECK(std::abs(lit[1] - lit[0]) <= 2);             // soft: still untouched (was: the shadow colour everywhere)
                CHECK(shadowed[1] < lit[1] - 60);                  // soft: the shadow still darkens
                CHECK(std::abs(shadowed[1] - shadowed[0]) <= 4);
                g.ring.wait_idle();
                rr.destroy_model(sg);
                rr.set_lighting(kuat_light);
            }
            {   // (b) bloom on a bright spot: strength from the environment (TEE 23) vs 0 (no-op)
                auto m = empty_model("BLOOM_SCENE");
                // Unlit (no effect) white quad facing the camera (-Y): full-white pixels above the cutoff.
                add_mesh(*m, "SPOT", {vtx(-0.05f, 0, -0.05f, 0, -1, 0), vtx(0.05f, 0, -0.05f, 0, -1, 0), vtx(0.05f, 0, 0.05f, 0, -1, 0),
                                      vtx(-0.05f, 0, 0.05f, 0, -1, 0)},
                         {0, 1, 2, 0, 2, 3}, nullptr, "alD3dVertNU2C", 0);
                m->bounds = {{-1, -1, -1}, {1, 1, 1}};
                m->sphere = {{0, 0, 0}, 1.8f};
                render::GpuModel sg;
                upload_wait(m, none_tex, sg);
                render::OrbitCamera cam;
                cam.yaw = -1.5707963f;
                cam.pitch = 0.0f;
                cam.distance = 5.0f;
                cam.radius = 2.0f;
                cam.fov_x = 2.0f * std::atan(0.2f);
                const render::ViewMatrices v = render::make_view(cam, 512, 512);
                render::Instance in;
                in.model = &sg;
                render::RecordOptions on;
                on.bloom_strength = 1.0f;
                settle(v, {&in, 1}, sg, on);
                std::vector<uint8_t> img[3];
                const float strengths[3] = {1.0f, 0.0f, -1.0f};
                bool bloom_ran[3] = {};
                for (int k = 0; k < 3; ++k) {
                    render::RecordOptions ro;
                    ro.bloom_strength = strengths[k];
                    ro.post = k != 2;
                    render::DrawStats st;
                    CHECK(render_frame(g, rr, o, v, {&in, 1}, nullptr, &st, render::DrawMode::Lit, ro));
                    bloom_ran[k] = st.bloom;
                    const uint8_t* px = static_cast<const uint8_t*>(o.readback.mapped());
                    img[k].assign(px, px + static_cast<size_t>(512) * 512 * 4);
                }
                auto at = [&](int k, uint32_t x, uint32_t y) { return &img[k][(static_cast<size_t>(y) * 512 + x) * 4]; };
                const uint8_t* near_on = at(0, 256 + 20, 256);
                const uint8_t* near_off = at(2, 256 + 20, 256);
                size_t diff0 = 0;
                for (size_t i = 0; i < img[1].size(); ++i) diff0 += img[1][i] != img[2][i];
                std::printf("bloom: strength 1 %s, next to the spot (%d,%d,%d) vs no post (%d,%d,%d); strength 0 differs from no post in "
                            "%zu bytes\n",
                            bloom_ran[0] ? "ran" : "skipped", near_on[0], near_on[1], near_on[2], near_off[0], near_off[1], near_off[2],
                            diff0);
                {
                    int mx = 0; size_t nd = 0;
                    for (size_t i = 0; i < img[0].size(); ++i) { const int d = std::abs(img[0][i] - img[2][i]); mx = std::max(mx, d); nd += d > 2; }
                    std::printf("bloom: centre on (%d,%d,%d) off (%d,%d,%d) max diff %d, %zu bytes > 2\n", at(0,256,256)[0], at(0,256,256)[1], at(0,256,256)[2], at(2,256,256)[0], at(2,256,256)[1], at(2,256,256)[2], mx, nd);
                }
                CHECK(bloom_ran[0] && !bloom_ran[1] && !bloom_ran[2]);
                CHECK(near_on[0] > near_off[0] + 5 && near_on[1] > near_off[1] + 5);  // the glow spreads past the spot
                CHECK(diff0 == 0);                                                    // strength 0: no-op
                g.ring.wait_idle();
                rr.destroy_model(sg);
            }
            {   // (c) heat: MeshHeat quad over a striped background; DistortionAmount 0 = the SceneHeat half-texel shift only
                // Technique pick with the original's vendor filter: SceneHeat t0 carries VENDOR_FILTER_0 = 4098 (AMD).
                const auto heat_any = effects.get_for_vendor("SceneHeat", 0), heat_amd = effects.get_for_vendor("SceneHeat", 0x1002),
                           heat_nv = effects.get_for_vendor("SceneHeat", 0x10DE);
                CHECK(heat_any && heat_amd && heat_nv && heat_any->technique_name == "t0" && heat_amd->technique_name == "t1_ati" &&
                      heat_nv == heat_any && heat_any == effects.get("SceneHeat"));
                if (heat_any && heat_amd && heat_any->passes.size() == heat_amd->passes.size() && !heat_any->passes.empty()) {
                    const materials::ResolvedPass &p0 = heat_any->passes[0], &p1 = heat_amd->passes[0];
                    materials::PsoKey k0 = p0.pso[0], k1 = p1.pso[0];
                    k0.vs_id = k1.vs_id = 0;  // program ids follow the bytecode
                    CHECK(p1.program_status == materials::ProgramStatus::Translated && p0.ps_bytecode == p1.ps_bytecode &&
                          p0.vs_bytecode != p1.vs_bytecode && k0 == k1);  // same states and PS, the ATI VS
                }
                const auto pds = effects.get("PrimDepthSpriteAlpha");  // its filters (0x10DE) only reject DX8 techniques
                CHECK(!pds || effects.get_for_vendor("PrimDepthSpriteAlpha", 0x10DE) == pds);
                const bool ati_heat = g.ctx.caps().vendor_id == 0x1002;
                const uint8_t dist_texel[4] = {160, 128, 128, 255};  // +x distortion
                auto heat_tex = [&](std::string_view name, assets::TextureRole) -> std::shared_ptr<const assets::TextureAsset> {
                    auto tx = std::make_shared<assets::TextureAsset>();
                    tx->format = assets::TextureFormat::RGBA8;
                    tx->mip_levels = 1;
                    if (name == "dist.dds") {
                        tx->name = "dist.dds";
                        tx->width = tx->height = 1;
                        tx->data.assign(dist_texel, dist_texel + 4);
                    } else if (name == "stripes.dds") {
                        // 16 x 16 separable stripes (x and y gradients under linear filtering): 30 + 100 (i odd) + 100 (j odd).
                        tx->name = "stripes.dds";
                        tx->width = 16;
                        tx->height = 16;
                        for (int j = 0; j < 16; ++j)
                            for (int i = 0; i < 16; ++i)
                                for (int ch = 0; ch < 4; ++ch)
                                    tx->data.push_back(static_cast<uint8_t>(ch == 3 ? 255 : 30 + (i % 2) * 100 + (j % 2) * 100));
                    } else {
                        return nullptr;
                    }
                    tx->levels.push_back({0, 0, tx->width, tx->height, 0, tx->data.size()});
                    return tx;
                };
                auto m = empty_model("HEAT_SCENE");
                std::vector<assets::StaticVertex> bg = {vtx(-1, 0.5f, -1, 0, -1, 0, 255, 0, 1), vtx(1, 0.5f, -1, 0, -1, 0, 255, 1, 1),
                                                        vtx(1, 0.5f, 1, 0, -1, 0, 255, 1, 0), vtx(-1, 0.5f, 1, 0, -1, 0, 255, 0, 0)};
                add_mesh(*m, "STRIPES", bg, {0, 1, 2, 0, 2, 3}, nullptr, "alD3dVertNU2C", 0, "BaseTexture", "stripes.dds");
                std::vector<assets::StaticVertex> hq = {vtx(-0.4f, 0, -0.4f, 0, -1, 0, 255, 0, 1), vtx(0.4f, 0, -0.4f, 0, -1, 0, 255, 1, 1),
                                                        vtx(0.4f, 0, 0.4f, 0, -1, 0, 255, 1, 0), vtx(-0.4f, 0, 0.4f, 0, -1, 0, 255, 0, 0)};
                add_mesh(*m, "HEAT", hq, {0, 1, 2, 0, 2, 3}, "MeshHeat.fx", "alD3dVertNU2C", 0, "DistortionTexture", "dist.dds");
                m->bounds = {{-1, -1, -1}, {1, 1, 1}};
                m->sphere = {{0, 0, 0}, 1.8f};
                render::GpuModel sg;
                upload_wait(m, heat_tex, sg);
                render::OrbitCamera cam;
                cam.yaw = -1.5707963f;
                cam.pitch = 0.0f;
                cam.distance = 5.0f;
                cam.radius = 2.0f;
                cam.fov_x = 2.0f * std::atan(0.2f);
                const render::ViewMatrices v = render::make_view(cam, 512, 512);
                render::Instance in;
                in.model = &sg;
                settle(v, {&in, 1}, sg, {});
                std::vector<uint8_t> img[3];
                bool heat_ran[3] = {};
                uint32_t heat_draws = 0;
                for (int k = 0; k < 3; ++k) {  // 0: no post, 1: DistortionAmount 0, 2: FXO default
                    render::RecordOptions ro;
                    ro.post = k != 0;
                    ro.bloom_strength = 0.0f;  // isolate heat
                    if (k == 1) ro.heat_amount = 0.0f;
                    render::DrawStats st;
                    CHECK(render_frame(g, rr, o, v, {&in, 1}, nullptr, &st, render::DrawMode::Lit, ro));
                    heat_ran[k] = st.heat;
                    if (k == 2) heat_draws = st.heat_draws;
                    const uint8_t* px = static_cast<const uint8_t*>(o.readback.mapped());
                    img[k].assign(px, px + static_cast<size_t>(512) * 512 * 4);
                }
                auto at = [&](int k, uint32_t x, uint32_t y) { return &img[k][(static_cast<size_t>(y) * 512 + x) * 4]; };
                // Sub-pixel shift (sx, sy) of frame k against the no-post frame 0, out(p) = in(p + s): least squares on the
                // red channel with central-difference gradients of frame 0 over the background, outside the box [skip0, skip1).
                auto measure_shift = [&](int k, uint32_t skip0, uint32_t skip1, double& sx, double& sy) {
                    double gxx = 0, gxy = 0, gyy = 0, bx = 0, by = 0;
                    for (uint32_t y = 48; y < 464; ++y)
                        for (uint32_t x = 48; x < 464; ++x) {
                            if (x >= skip0 && x < skip1 && y >= skip0 && y < skip1) continue;
                            const double gx = 0.5 * (at(0, x + 1, y)[0] - at(0, x - 1, y)[0]);
                            const double gy = 0.5 * (at(0, x, y + 1)[0] - at(0, x, y - 1)[0]);
                            const double d = static_cast<double>(at(k, x, y)[0]) - static_cast<double>(at(0, x, y)[0]);
                            gxx += gx * gx;
                            gxy += gx * gy;
                            gyy += gy * gy;
                            bx += gx * d;
                            by += gy * d;
                        }
                    const double det = gxx * gyy - gxy * gxy;
                    sx = det > 0 ? (bx * gyy - by * gxy) / det : 99.0;
                    sy = det > 0 ? (by * gxx - bx * gxy) / det : 99.0;
                };
                // SceneHeat VS: t0 = uv - (0.5/W, 0.5/H) (t0) or (0.5/W, 0.25/H) (t1_ati, AMD), from RESOLUTION_CONSTANTS of the
                // bound target; D3D9 pixel centres add another -1/2 texel against Vulkan's. PS: uv + 2 DistortionAmount (d - 0.5),
                // the 0x80 clear is d = 128/255 (g1f t06 capture on AMD: +2.765 / +1.264 px at 3840x2054).
                const double y0 = ati_heat ? -0.75 : -1.0, dist80 = 2.0 * 0.25 * (128.0 / 255.0 - 0.5) * 512.0;
                double s1x = 0, s1y = 0, s2x = 0, s2y = 0;
                measure_shift(1, 0, 0, s1x, s1y);      // amount 0: the half-texel terms only, whole background
                measure_shift(2, 130, 382, s2x, s2y);  // FXO default 0.25 outside the heat quad: flat 0x80 buffer
                std::printf("heat shift (%s): amount 0 (%.3f, %.3f) px, expected (-1, %.2f); amount 0.25 over 0x80 (%.3f, %.3f), expected "
                            "(%.3f, %.3f)\n",
                            ati_heat ? "t1_ati" : "t0", s1x, s1y, y0, s2x, s2y, -1.0 + dist80, y0 + dist80);
                // Tolerance 0.08 px (the estimator reads quarter-pixel shifts about 0.04 px short on this pattern); t0 and t1_ati
                // differ by 0.25 px in y.
                CHECK(std::fabs(s1x + 1.0) < 0.08 && std::fabs(s1y - y0) < 0.08);
                CHECK(std::fabs(s2x - (-1.0 + dist80)) < 0.08 && std::fabs(s2y - (y0 + dist80)) < 0.08);
                size_t inside_moved = 0, outside_bad = 0, checked = 0;
                for (uint32_t y = 140; y < 372; y += 3)
                    for (uint32_t x = 20; x < 492; x += 3) {
                        const bool inside = x > 160 && x < 352 && y > 160 && y < 352;
                        const bool edge = (x > 140 && x < 372 && y > 140 && y < 372) && !inside;
                        if (inside) inside_moved += std::abs(at(2, x, y)[0] - at(1, x, y)[0]) > 40;
                        else if (!edge) {
                            // The cleared 0x80 is 128/255, not 0.5: SceneHeat shifts by (2*128/255 - 1) x amount, about
                            // half a texel, which moves the filtered stripe gradients by a few levels. Allow that.
                            outside_bad += std::abs(at(2, x, y)[0] - at(1, x, y)[0]) > 6;
                        }
                        ++checked;
                    }
                std::printf("heat: %u heat draws, composite %s; default: %zu pixels moved inside the heat quad, %zu of %zu changed outside\n",
                            heat_draws, heat_ran[2] ? "ran" : "skipped", inside_moved, outside_bad, checked);
                CHECK(heat_ran[1] && heat_ran[2] && !heat_ran[0] && heat_draws == 1);
                CHECK(inside_moved > 100);   // the stripes behind the quad are displaced
                CHECK(outside_bad * 50 < checked);  // cleared 0x8080FF = (almost) no offset elsewhere
                g.ring.wait_idle();
                rr.destroy_model(sg);
            }
            // -- Shots: Thrawn SD under Kuat with bloom, space backdrop, land unit with shadows, MeshHeat user.
            render::ModelStreamer shots(rr, cache, js);
            if (render::GpuModel* sd = load(shots, "EV_STARDESTOYER_THRAWN.ALO")) {
                Capture c;
                capture(*sd, {}, 0.75f, c, render::DrawMode::Lit, 0.35f, 2.2f);
                report("thrawn post", c);
                std::printf("  bloom %s (Kuat: strength %.2f, cutoff %.2f, size %.2f)\n", c.stats.bloom ? "ran" : "skipped",
                            kuat_light.bloom_strength, kuat_light.bloom_cutoff, kuat_light.bloom_size);
                CHECK(c.stats.bloom && c.coverage > 0.1);
                save("render_thrawn_sd_post.png", c);
                // Backdrop: Kuat's sky objects (STARS_LOW + STAR_BACKDROP_GREEN_ORANGE_2 -> models via the XML).
                std::vector<render::Instance> sky;
                for (int k = 0; k < 2; ++k) {
                    const std::string model = render::find_object_model(*files, kuat_light.skydome[k]);
                    std::printf("  sky object %s -> %s\n", kuat_light.skydome[k].c_str(), model.c_str());
                    CHECK(!model.empty());
                    if (model.empty()) continue;
                    if (render::GpuModel* sm = load(shots, model.c_str())) {
                        render::Instance si;
                        si.model = sm;
                        si.transform = render::skydome_transform(kuat_light, k);
                        sky.push_back(si);
                    }
                }
                Capture b;
                capture(*sd, {}, 0.75f, b, render::DrawMode::Lit, 0.35f, 2.2f, {}, sky);
                report("space backdrop", b);
                std::printf("  %u sky draws\n", b.stats.sky_draws);
                CHECK(b.stats.sky_draws > 0 && b.lit > 0.2);  // stars + nebula behind the ship
                save("render_space_backdrop.png", b);
            }
            {
                const char* kLandUnit = "RV_T2BTANK.ALO";
                render::GpuModel* lu = load(shots, kLandUnit);
                CHECK(lu != nullptr);
                if (lu) {
                    auto pm = empty_model("GROUND_PLANE");
                    const float r = lu->drawn_sphere().radius;
                    add_plane(*pm, r * 4.0f);
                    pm->bounds = {{-r * 4, -r * 4, 0}, {r * 4, r * 4, 0}};
                    pm->sphere = {{0, 0, 0}, r * 5.7f};
                    render::GpuModel gp;
                    upload_wait(pm, none_tex, gp);
                    render::Lighting land = render::Lighting::land_default();
                    land.fog_enabled = false;  // Endor fog ends at 3500; irrelevant at this distance
                    rr.set_lighting(land);
                    render::Instance pi;
                    pi.model = &gp;
                    {   // first frame records the plane's upload acquires; then it can be made resident
                        Capture warm;
                        capture(*lu, {}, 1.0f, warm, render::DrawMode::Lit, 0.6f, 0.6f, {&pi, 1});
                        rr.make_resident(gp);
                    }
                    // Pick the camera yaw that shows the most shadow (shadows on vs off), so the shot shows it.
                    Capture c;
                    size_t best = 0;
                    float best_yaw = 0.6f;
                    render::RecordOptions no_sh;
                    no_sh.shadows = false;
                    for (float yaw : {0.6f, 2.2f, 3.8f, 5.4f}) {
                        Capture on, off;
                        capture(*lu, {}, 1.0f, on, render::DrawMode::Lit, 0.6f, yaw, {&pi, 1});
                        capture(*lu, {}, 1.0f, off, render::DrawMode::Lit, 0.6f, yaw, {&pi, 1}, {}, no_sh);
                        size_t n = 0;
                        for (size_t i = 0; i + 3 < on.rgba.size(); i += 4) n += on.rgba[i + 1] + 20 < off.rgba[i + 1];
                        std::printf("  yaw %.1f: %zu pixels darkened by the stencil shadows (%u draws)\n", yaw, n, on.stats.draws);
                        if (n >= best) {
                            best = n;
                            best_yaw = yaw;
                        }
                    }
                    capture(*lu, {}, 1.0f, c, render::DrawMode::Lit, 0.6f, best_yaw, {&pi, 1});
                    CHECK(best > 500);
                    report(kLandUnit, c);
                    std::printf("  %u shadow volumes, darken %s (Endor Sunrise_Clear, shadow colour %.2f)\n", c.stats.shadow_volumes,
                                c.stats.darkened ? "yes" : "no", land.shadow_color.x);
                    CHECK(c.stats.shadow_volumes > 0 && c.stats.darkened);
                    save("render_land_shadows.png", c);
                    g.ring.wait_idle();
                    rr.destroy_model(gp);
                    rr.set_lighting(kuat_light);
                }
            }
            {
                const char* kHeatUser = "NB_BACTATANK.ALO";  // the only MeshHeat user in the install (alamo_render --sweep)
                render::GpuModel* hu = load(shots, kHeatUser);
                CHECK(hu != nullptr);
                if (hu) {
                    Capture c;
                    capture(*hu, {}, 0.9f, c, render::DrawMode::Lit, 0.35f, 2.2f);
                    report(kHeatUser, c);
                    std::printf("  %u heat draws, composite %s\n", c.stats.heat_draws, c.stats.heat ? "ran" : "skipped");
                    CHECK(c.stats.heat_draws > 0 && c.stats.heat);
                    save("render_nb_bactatank_heat.png", c);
                }
            }
            g.ring.wait_idle();
            shots.destroy_all();
            // ==== World: terrain + water + map objects (engine/terrain data drawn by render::WorldRenderer).
            {
                // Synthetic textures for the synthetic scenes (alpha 0 = no terrain specular), real files otherwise.
                auto solid = [](const char* name, uint8_t r, uint8_t gg, uint8_t b, uint8_t a) {
                    auto tx = std::make_shared<assets::TextureAsset>();
                    tx->name = name;
                    tx->format = assets::TextureFormat::RGBA8;
                    tx->width = tx->height = 4;
                    tx->mip_levels = 1;
                    for (int i = 0; i < 16; ++i) tx->data.insert(tx->data.end(), {r, gg, b, a});
                    tx->levels.push_back({0, 0, 4, 4, 0, tx->data.size()});
                    return tx;
                };
                const auto red = solid("test_red.tga", 255, 0, 0, 0), green = solid("test_green.tga", 0, 255, 0, 0);
                const auto grey = solid("test_grey.tga", 128, 128, 128, 0), flat = solid("test_flatbump.tga", 128, 128, 255, 255);
                const render::TextureSource world_tex = [&](std::string_view n, assets::TextureRole role) -> std::shared_ptr<const assets::TextureAsset> {
                    if (n == "test_red.tga") return red;
                    if (n == "test_green.tga") return green;
                    if (n == "test_grey.tga") return grey;
                    if (n == "test_flatbump.tga") return flat;
                    return cache.texture(n, role);
                };
                auto make_terrain = [](int16_t raw_height, const char* tex0, const char* tex1) {
                    terrain::Terrain t;
                    t.present = true;
                    t.width = t.height = 32;
                    t.header.width = t.header.height = 32;
                    t.header.vertex_count = 32 * 32;
                    t.heights.assign(32 * 32, raw_height);
                    t.material.assign(32 * 32, 0);
                    t.brightness.assign(32 * 32, 255);
                    for (uint32_t y = 0; y < 32; ++y)
                        for (uint32_t x = 16; x < 32 && tex1; ++x) t.material[y * 32 + x] = 1;
                    t.materials[0].present = true;
                    t.materials[0].diffuse_texture = tex0;
                    if (tex1) {
                        t.materials[1].present = true;
                        t.materials[1].diffuse_texture = tex1;
                    }
                    return t;
                };
                // Frames until the world is resident and every program / pipeline is ready.
                auto settle_world = [&](render::WorldRenderer& w, Offscreen& oo, const render::ViewMatrices& v,
                                        std::span<const render::Instance> inst, render::GpuModel* resident, render::DrawStats& st,
                                        const render::RecordOptions& ro, std::span<const render::Instance> sky = {}) {
                    for (int k = 0; k < 400; ++k) {
                        CHECK(render_frame(g, rr, oo, v, inst, resident, &st, render::DrawMode::Lit, ro, nullptr, sky));
                        w.make_resident();
                        const render::WorldStats& ws = w.stats();
                        if (k > 2 && w.resident() && ws.terrain_pending == 0 && ws.water_pending == 0 && st.pending_pipeline == 0 &&
                            st.translation_pending == 0 && st.post_pending == 0)
                            return true;
                        rr.wait_translations();
                        rr.pipelines().wait_idle();
                    }
                    return false;
                };
                const render::Lighting saved_light = rr.lighting();
                render::Lighting wl = render::Lighting::land_default();
                wl.fog_enabled = false;
                wl.skydome[0].clear();
                wl.skydome[1].clear();
                wl.clear_color = {1.0f, 1.0f, 1.0f};
                wl.sun.direction = {0.0f, 0.0f, 1.0f};
                rr.set_lighting(wl);
                render::RecordOptions nopost;
                nopost.post = false;

                // (a) 2-material terrain: the boundary quad is opaque in its lowest material (0, red) with material 1
                //     (green) fading in over that one quad; the quads on either side are pure.
                {
                    const terrain::Terrain t = make_terrain(0, "test_red.tga", "test_green.tga");
                    terrain::TerrainMesh mesh;
                    std::string terr;
                    CHECK(terrain::build_terrain_mesh(t, mesh, {}, alamo::serial_for, &terr));
                    CHECK(mesh.patches.size() == 1 && mesh.patches[0].passes.size() == 2);
                    render::WorldRenderer w;
                    terrain::CloudParams clouds;
                    clouds.texture.clear();
                    CHECK(w.init(rr, mesh, nullptr, clouds, world_tex, {}, &terr));
                    rr.set_world(&w);
                    render::OrbitCamera cam;
                    cam.target = {310.0f, 310.0f, 0.0f};
                    cam.yaw = -1.5707963f;
                    cam.pitch = 1.5f;
                    cam.distance = 500.0f;
                    const render::ViewMatrices v = render::make_view(cam, 512, 512, 10.0f, 7000.0f);
                    render::DrawStats st;
                    CHECK(settle_world(w, o, v, {}, nullptr, st, nopost));
                    auto at = [&](float x) {
                        const auto p = project(v, x, 310.0f, 0.0f, 512, 512);
                        const uint8_t* q = o.px(p[0], p[1]);
                        return std::array<int, 3>{q[0], q[1], q[2]};
                    };
                    const auto r0 = at(290.0f), mid = at(310.0f), g1 = at(330.0f);
                    const double frac = static_cast<double>(mid[0]) / std::max(1, mid[0] + mid[1]);
                    std::printf("terrain blend: %u draws (%u visible patches); quad 14-15 (%d,%d,%d), boundary quad 15-16 (%d,%d,%d) red "
                                "fraction %.2f, quad 16-17 (%d,%d,%d)\n",
                                w.stats().terrain_draws, w.stats().visible_patches, r0[0], r0[1], r0[2], mid[0], mid[1], mid[2], frac, g1[0],
                                g1[1], g1[2]);
                    CHECK(w.stats().terrain_draws == 2 && w.stats().visible_patches == 1);
                    CHECK(r0[0] > 40 && r0[1] * 4 < r0[0]);   // material 0 only
                    CHECK(g1[1] > 40 && g1[0] * 4 < g1[1]);   // material 1 only
                    CHECK(frac > 0.3 && frac < 0.7);          // red opaque underneath, green at alpha 0.5
                    // Moving across the boundary quad the green share rises monotonically (linear blend texel ramp).
                    int prev = -1;
                    bool mono = true;
                    for (float x = 302.0f; x <= 318.0f; x += 4.0f) {
                        const auto c3 = at(x);
                        mono = mono && c3[1] >= prev - 2;
                        prev = c3[1];
                    }
                    CHECK(mono);
                    g.ring.wait_idle();
                    rr.set_world(nullptr);
                    w.shutdown();
                }

                // (b) water over a deep terrain: the reflection shows the object above the surface (mirrored), the
                //     reflection target clips what is below the plane, the refraction target what is above it.
                {
                    terrain::Terrain t = make_terrain(-1024, "test_grey.tga", nullptr);  // -50 world units
                    t.header.water_type = terrain::WaterType::Water;
                    t.header.water_height = 0.0f;
                    t.header.water_texture[0] = "test_flatbump.tga";  // flat bump: no distortion of the lookups
                    terrain::TerrainMesh mesh;
                    terrain::WaterSurface ws;
                    std::string terr;
                    CHECK(terrain::build_terrain_mesh(t, mesh, {}, alamo::serial_for, &terr));
                    CHECK(terrain::build_water(t, ws, {}, alamo::serial_for, &terr) && ws.enabled && ws.targets.reflection &&
                          ws.targets.refraction);
                    render::WorldRenderer w;
                    CHECK(w.init(rr, mesh, &ws, terrain::CloudParams{}, world_tex, {}, &terr));
                    rr.set_world(&w);
                    // A (magenta) above the water, B (cyan) under it: double-sided unlit quads facing the camera (-Y).
                    auto m = empty_model("WATER_OBJECTS");
                    auto quad2 = [&](const char* name, float x0, float x1, float y, float z0, float z1, uint8_t r, uint8_t gg, uint8_t b) {
                        std::vector<assets::StaticVertex> q = {vtx(x0, y, z0, 0, -1, 0), vtx(x1, y, z0, 0, -1, 0), vtx(x1, y, z1, 0, -1, 0),
                                                               vtx(x0, y, z1, 0, -1, 0)};
                        for (auto& sv : q) {
                            sv.color[0] = r;
                            sv.color[1] = gg;
                            sv.color[2] = b;
                        }
                        add_mesh(*m, name, q, {0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2}, nullptr, "alD3dVertNU2C", 0);
                    };
                    quad2("A", 270.0f, 350.0f, 400.0f, 20.0f, 80.0f, 255, 0, 255);
                    quad2("B", 330.0f, 390.0f, 380.0f, -40.0f, -10.0f, 0, 255, 255);
                    m->bounds = {{260, 370, -45}, {400, 410, 85}};
                    m->sphere = {{330, 390, 20}, 110.0f};
                    render::GpuModel wm;
                    upload_wait(m, none_tex, wm);
                    render::Instance wi;
                    wi.model = &wm;
                    render::OrbitCamera cam;
                    cam.target = {310.0f, 380.0f, 0.0f};
                    cam.yaw = -1.5707963f;
                    cam.pitch = 0.35f;
                    cam.distance = 300.0f;
                    const render::ViewMatrices v = render::make_view(cam, 512, 512, 10.0f, 7000.0f);
                    render::DrawStats st;
                    CHECK(settle_world(w, o, v, {&wi, 1}, &wm, st, nopost));
                    const auto pa_m = project(v, 310.0f, 400.0f, -50.0f, 512, 512);  // A's mirror image
                    std::array<int, 3> with{}, without{};
                    {
                        const uint8_t* q = o.px(pa_m[0], pa_m[1]);
                        with = {q[0], q[1], q[2]};
                    }
                    const render::WorldStats wst = w.stats();
                    // Read the water targets of this frame back through the 512x512 offscreen image.
                    auto read_rt = [&](const rhi::RenderTarget& rt, std::vector<uint8_t>& out) {
                        rhi::Frame* f = g.ring.begin_frame();
                        if (!f) return false;
                        VkCommandBuffer cmd = f->cmd();
                        const rhi::DeviceFns& fn = g.ctx.vkd();
                        rhi::CopyImage src;
                        src.image = rt.color().handle;
                        src.extent = rt.extent();
                        src.format = rt.color().vk_format();
                        src.before = rhi::ImageState::ShaderRead;
                        src.after = rhi::ImageState::ShaderRead;
                        rhi::CopyImage dst;
                        dst.image = o.color.handle;
                        dst.extent = {o.w, o.h};
                        dst.format = VK_FORMAT_R8G8B8A8_UNORM;
                        dst.before = rhi::ImageState::Undefined;
                        dst.after = rhi::ImageState::TransferSrc;
                        rhi::cmd_stretch_rect(fn, cmd, src, dst, VK_FILTER_NEAREST);
                        VkBufferImageCopy copy{};
                        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                        copy.imageExtent = {o.w, o.h, 1};
                        fn.vkCmdCopyImageToBuffer(cmd, o.color.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, o.readback.handle, 1, &copy);
                        const VkBufferMemoryBarrier2 hb = rhi::buffer_barrier(o.readback.handle, VK_PIPELINE_STAGE_2_COPY_BIT,
                                                                              VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                                                                              VK_ACCESS_2_HOST_READ_BIT);
                        rhi::cmd_barrier(fn, cmd, {}, std::span<const VkBufferMemoryBarrier2>(&hb, 1));
                        if (!g.ring.end_frame(*f, {})) return false;
                        g.ring.wait_idle();
                        g.alloc.invalidate(o.readback.alloc);
                        const uint8_t* p0 = static_cast<const uint8_t*>(o.readback.mapped());
                        out.assign(p0, p0 + static_cast<size_t>(o.w) * o.h * 4);
                        return true;
                    };
                    std::vector<uint8_t> refl, refr;
                    CHECK(w.reflection_target() && w.refraction_target());
                    if (w.reflection_target() && w.refraction_target()) {
                        CHECK(read_rt(*w.reflection_target(), refl) && read_rt(*w.refraction_target(), refr));
                    }
                    auto rt_px = [&](const std::vector<uint8_t>& img, float x, float y, float z) {
                        const auto p = project(v, x, y, z, 512, 512);
                        const uint8_t* q = img.empty() ? nullptr : &img[(static_cast<size_t>(p[1]) * 512 + p[0]) * 4];
                        return q ? std::array<int, 3>{q[0], q[1], q[2]} : std::array<int, 3>{0, 0, 0};
                    };
                    // Reflection target: A at its mirror position, B (under the plane) clipped away.
                    const auto ra = rt_px(refl, 310.0f, 400.0f, -50.0f), rb = rt_px(refl, 360.0f, 380.0f, 25.0f);
                    // Refraction target: B visible, A (above the plane) clipped away.
                    const auto fa = rt_px(refr, 310.0f, 400.0f, 50.0f), fb = rt_px(refr, 360.0f, 380.0f, -25.0f);
                    // Same frame without the objects: the water at A's mirror image loses the magenta.
                    CHECK(settle_world(w, o, v, {}, nullptr, st, nopost));
                    {
                        const uint8_t* q = o.px(pa_m[0], pa_m[1]);
                        without = {q[0], q[1], q[2]};
                    }
                    std::printf("water: %u patches, %u draws, reflection %s (%u draws), refraction %s (%u draws), sky cube %s; water at A's "
                                "mirror image with A (%d,%d,%d) / without (%d,%d,%d); reflection RT: A' (%d,%d,%d), B' (%d,%d,%d); refraction "
                                "RT: A (%d,%d,%d), B (%d,%d,%d)\n",
                                wst.water_patches, wst.water_draws, wst.reflection ? "yes" : "no", wst.reflection_draws,
                                wst.refraction ? "yes" : "no", wst.refraction_draws, wst.sky_cube_rendered ? "rendered" : "kept", with[0],
                                with[1], with[2], without[0], without[1], without[2], ra[0], ra[1], ra[2], rb[0], rb[1], rb[2], fa[0], fa[1],
                                fa[2], fb[0], fb[1], fb[2]);
                    CHECK(wst.water_draws > 0 && wst.reflection && wst.refraction);
                    CHECK(without[1] - with[1] > 40 && std::abs(without[0] - with[0]) < 30);  // magenta replaces white in the reflection
                    CHECK(ra[0] > 200 && ra[1] < 60 && ra[2] > 200);                          // A mirrored
                    CHECK(!(rb[1] > 200 && rb[0] < 60));                                      // B clipped (below the plane)
                    CHECK(fb[1] > 200 && fb[2] > 200 && fb[0] < 60);                          // B refracted
                    CHECK(!(fa[0] > 200 && fa[1] < 60));                                      // A clipped (above the plane)
                    g.ring.wait_idle();
                    rr.set_world(nullptr);
                    w.shutdown();
                    rr.destroy_model(wm);
                }
                rr.set_lighting(saved_light);

                // (c) Real maps at 1280x720 from the tactical camera.
                data::GameDatabase db;
                CHECK(db.load(*files));
                render::GameTypeResolver resolver(db);
                CHECK(resolver.load_render_xml(*files));
                {   // Hardpoint Damage_Decal rule on a real type + factions / palette from the XML.
                    const render::TypeRenderInfo sd = resolver.render_info(alamo::name_crc("Star_Destroyer"));
                    std::printf("Star_Destroyer: LOD_Bias %.1f, %zu hardpoints, decals:", sd.lod_bias, sd.hardpoints.size());
                    for (const auto& d : sd.damage_decals) std::printf(" %s", d.c_str());
                    std::printf("\n");
                    CHECK(sd.hardpoints_known && !sd.damage_decals.empty() &&
                          std::find(sd.damage_decals.begin(), sd.damage_decals.end(), "HP_F-L_BLAST") != sd.damage_decals.end());
                    {   // full <HardPoint> records per hardpoint of the type
                        const int32_t fl = sd.hardpoint_index("HP_Star_Destroyer_Weapon_FL");
                        const int32_t en = sd.hardpoint_index("hp_star_destroyer_engines");
                        CHECK(sd.hardpoint_defs.size() == sd.hardpoints.size() && fl >= 0 && en >= 0);
                        if (fl >= 0 && en >= 0) {
                            const render::HardpointDef& a = sd.hardpoint_defs[static_cast<size_t>(fl)];
                            const render::HardpointDef& e = sd.hardpoint_defs[static_cast<size_t>(en)];
                            std::printf("  %s: bone %s, decal %s, attach %s, %zu tags\n", a.name.c_str(), a.attachment_bone.c_str(),
                                        a.damage_decal.c_str(), a.model_to_attach.c_str(), a.tags.size());
                            CHECK(a.known && a.attachment_bone == "HP_F-L_BONE" && a.damage_decal == "HP_F-L_BLAST" &&
                                  a.model_to_attach == "EV_StarDestroyer_HP00_L-F.alo" && a.tag("Collision_Mesh") == "HP_F-L_COLL");
                            CHECK(e.known && e.damage_decal.empty() && e.attachment_bone == "HP_E_BONE");
                        }
                        CHECK(resolver.hardpoint("hp_star_destroyer_tractor_beam") && !resolver.hardpoint("HP_NOPE"));
                    }
                    terrain::ResolvedType rt;
                    CHECK(resolver.resolve(alamo::name_crc("Star_Destroyer"), true, rt));
                    if (auto sdm = cache.model(rt.model_name)) {
                        const std::vector<uint8_t> xml_mask = render::hidden_mesh_mask(*sdm, sd.damage_decals);
                        const std::vector<uint8_t> name_mask = render::hidden_mesh_mask(*sdm, &render::default_hidden_mesh_rule);
                        uint32_t nx = 0, nn = 0;
                        for (size_t i = 0; i < xml_mask.size(); ++i) {
                            nx += xml_mask[i];
                            nn += name_mask[i];
                        }
                        std::printf("  %s: %u meshes hidden by the Damage_Decal rule, %u by the name fallback\n", rt.model_name.c_str(), nx, nn);
                        CHECK(nx > 0 && nx <= nn);
                        // Mesh -> bone map on the real model: meshes on / under each hardpoint's attachment bone.
                        const render::MeshBoneMap bm(*sdm);
                        for (const render::HardpointDef& d : sd.hardpoint_defs) {
                            const int32_t b = sdm->skeleton.find(d.attachment_bone);
                            const std::vector<uint32_t> own = bm.meshes_on_bone(b), sub = bm.meshes_in_subtree(b);
                            std::printf("  %s bone %s (%d): %zu own / %zu subtree meshes:", d.name.c_str(), d.attachment_bone.c_str(), b,
                                        own.size(), sub.size());
                            for (uint32_t mi : sub) std::printf(" %s%s", sdm->meshes[mi].name.c_str(), sdm->meshes[mi].hidden ? "(h)" : "");
                            std::printf("\n");
                            CHECK(own.size() <= sub.size());
                        }
                    }
                    // B.10 vanilla examples: destroy one hardpoint on the real owner model, list what changed.
                    auto destroy_case = [&](const char* type, const char* hp, std::vector<std::string> expect_shown) {
                        const render::TypeRenderInfo ti = resolver.render_info(alamo::name_crc(type));
                        terrain::ResolvedType trt;
                        const int32_t h = ti.hardpoint_index(hp);
                        CHECK(resolver.resolve(alamo::name_crc(type), true, trt) && h >= 0);
                        auto om = cache.model(trt.model_name);
                        CHECK(om != nullptr);
                        if (!om || h < 0) return;
                        auto lay = std::make_shared<const render::HardpointLayout>(render::build_hardpoint_layout(*om, ti.hardpoint_defs));
                        render::HardpointVisibility v;
                        v.reset(ti.hardpoints.size());
                        v.init(lay);
                        const std::vector<uint8_t> before(v.mask().begin(), v.mask().end());
                        CHECK(v.set_destroyed(static_cast<size_t>(h), true));
                        auto name_of = [&](uint32_t s) -> std::string {
                            if (s < lay->meshes) return om->meshes[s].name;
                            if (s < lay->meshes + lay->proxies) return om->proxies[s - lay->meshes].name;
                            return "attach:" + lay->attach_model[s - lay->meshes - lay->proxies];
                        };
                        std::vector<std::string> shown, hidden;
                        for (uint32_t s = 0; s < before.size(); ++s) {
                            if (before[s] && !v.mask()[s]) shown.push_back(name_of(s));
                            if (!before[s] && v.mask()[s]) hidden.push_back(name_of(s));
                        }
                        std::printf("  B.10 %s (%s) %s: shown", type, trt.model_name.c_str(), hp);
                        for (const auto& n : shown) std::printf(" %s", n.c_str());
                        std::printf("; hidden %zu; attachments visible", hidden.size());
                        for (uint32_t a = 0; a < lay->hardpoints; ++a)
                            if (v.attachment_visible(a)) std::printf(" %s", lay->attach_model[a].c_str());
                        std::printf("\n");
                        auto lower_sorted = [](std::vector<std::string> x) {
                            for (auto& s : x)
                                for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                            std::sort(x.begin(), x.end());
                            return x;
                        };
                        CHECK(lower_sorted(shown) == lower_sorted(expect_shown) && hidden.empty());
                        const std::string& own_attach = lay->attach_model[static_cast<size_t>(h)];
                        CHECK(own_attach.empty() || v.attachment_visible(static_cast<uint32_t>(h)));  // Model_To_Attach stays
                    };
                    destroy_case("Star_Destroyer", "HP_Star_Destroyer_Weapon_FL", {"HP_F-L_Blast", "p_hp_imperial_damage", "p_hp_imperial_damage"});
                    destroy_case("Star_Destroyer", "HP_Star_Destroyer_Engines",
                                 {"p_hp_imperial_damage", "p_hp_imperial_damage", "p_hp_imperial_damage"});
                    destroy_case("Empire_Star_Base_1", "HP_Empire_Station_One_01", {"HP01_LC_Blast", "p_RB_Station_Damage", "p_RB_Station_Damage"});
                    {   // Star_Destroyer + its Model_To_Attach turrets, intact vs every hardpoint destroyed -> PNGs.
                        const render::TypeRenderInfo ti = resolver.render_info(alamo::name_crc("Star_Destroyer"));
                        terrain::ResolvedType trt;
                        CHECK(resolver.resolve(alamo::name_crc("Star_Destroyer"), true, trt));
                        render::ModelStreamer sds(rr, cache, js);
                        render::GpuModel* owner = load(sds, trt.model_name.c_str());
                        CHECK(owner != nullptr);
                        if (owner) {
                            auto lay = std::make_shared<const render::HardpointLayout>(render::build_hardpoint_layout(*owner->asset, ti.hardpoint_defs));
                            std::vector<render::GpuModel*> att(lay->hardpoints, nullptr);
                            uint32_t n_att = 0;
                            for (uint32_t h = 0; h < lay->hardpoints; ++h)
                                if (!lay->attach_model[h].empty()) {
                                    att[h] = load(sds, lay->attach_model[h].c_str());
                                    n_att += att[h] ? 1u : 0u;
                                }
                            CHECK(n_att >= 7);  // 6 weapons + shield generator + engines carry a Model_To_Attach
                            render::HardpointVisibility v;
                            v.reset(lay->hardpoints);
                            v.init(lay);
                            render::OrbitCamera cam;
                            cam.pitch = 0.45f;
                            cam.yaw = 0.6f;
                            const assets::Sphere sph = owner->drawn_sphere();
                            cam.frame(sph.center, sph.radius, 1.0f);
                            cam.distance *= 0.75f;
                            const render::ViewMatrices vm = render::make_view(cam, 512, 512);
                            auto shoot = [&](bool turrets, Capture& c) {
                                std::vector<render::Instance> all(1);
                                all[0].model = owner;
                                all[0].mesh_hidden = v.mask();
                                for (uint32_t h = 0; h < lay->hardpoints && turrets; ++h) {
                                    if (!att[h] || !v.attachment_visible(h)) continue;
                                    render::Instance a;
                                    a.model = att[h];
                                    a.transform = render::attachment_transform(*owner->asset, all[0].transform, {}, lay->attach_bone[h]);
                                    rr.make_resident(*att[h]);
                                    all.push_back(a);
                                }
                                CHECK(render_settled(g, rr, o, vm, all, owner, c.stats, render::DrawMode::Lit));
                                CHECK(c.stats.post_pending == 0);
                                c.rgba.assign(static_cast<size_t>(512) * 512 * 4, 0);
                                uint64_t covered = 0;
                                for (uint32_t y = 0; y < 512; ++y)
                                    for (uint32_t x = 0; x < 512; ++x) {
                                        const uint8_t* p = o.px(x, y);
                                        uint8_t* q = &c.rgba[(static_cast<size_t>(y) * 512 + x) * 4];
                                        q[0] = p[0];
                                        q[1] = p[1];
                                        q[2] = p[2];
                                        q[3] = 255;
                                        covered += rgb_close(p, 0, 0, 64, 1) ? 0u : 1u;
                                    }
                                c.coverage = static_cast<double>(covered) / (512.0 * 512.0);
                                return all.size() - 1;
                            };
                            auto differ = [](const Capture& a, const Capture& b) {
                                uint32_t n = 0;
                                for (size_t i = 0; i < a.rgba.size(); i += 4)
                                    n += (std::abs(a.rgba[i] - b.rgba[i]) + std::abs(a.rgba[i + 1] - b.rgba[i + 1]) +
                                          std::abs(a.rgba[i + 2] - b.rgba[i + 2])) > 24 ? 1u : 0u;
                                return n;
                            };
                            Capture bare, before, after;
                            shoot(false, bare);
                            const size_t drawn_att = shoot(true, before);
                            for (uint32_t h = 0; h < lay->hardpoints; ++h) v.set_destroyed(h, true);
                            shoot(true, after);
                            const uint32_t d_turrets = differ(bare, before), d_destroy = differ(before, after);
                            std::printf("  Star_Destroyer hardpoints: %zu turrets attached (%u draws vs %u bare, %u pixels differ); all %u destroyed: "
                                        "%u draws, %u pixels differ\n",
                                        drawn_att, before.stats.draws, bare.stats.draws, d_turrets, lay->hardpoints, after.stats.draws, d_destroy);
                            CHECK(drawn_att == n_att && before.stats.draws > bare.stats.draws && d_turrets > 200);
                            CHECK(after.stats.draws > before.stats.draws && d_destroy > 50);  // _Blast decals drawn
                            save("render_sd_hardpoints_before.png", before);
                            save("render_sd_hardpoints_after.png", after);
                        }
                        g.ring.wait_idle();
                        sds.destroy_all();
                    }
                    std::printf("factions:");
                    for (const auto& f : resolver.factions()) std::printf(" %s%s", f.name.c_str(), f.neutral ? " (neutral)" : "");
                    std::printf("\n");
                    CHECK(resolver.owner_neutral(3) && !resolver.owner_neutral(0) && !resolver.owner_neutral(4));
                    const render::MpPalette pal = resolver.palette();
                    CHECK(near_eq(pal.color[7].x, 109.0f / 255.0f) && near_eq(pal.color[8].z, 42.0f / 255.0f));
                    const render::TypeRenderInfo none;
                    const materials::float4 nc = resolver.owner_color(3, none, pal);
                    CHECK(nc.x == 1.0f && nc.y == 1.0f && nc.z == 1.0f && nc.w == 1.0f);  // Neutral No_Colorization_Color 255,255,255
                }
                Offscreen o2;
                CHECK(o2.create(g, 1280, 720));
                render::ModelStreamer map_streamer(rr, cache, js);
                const alamo::ParallelFor pf = jobs::make_parallel_for(js);
                {   // MapScene::load is an exception boundary: a throwing builder (here the ParallelFor, as
                    // std::bad_alloc) returns false with an error and leaves the scene empty.
                    const alamo::ParallelFor throwing = [](uint32_t, uint32_t, const alamo::RangeFn&) { throw std::bad_alloc(); };
                    render::MapScene scene;
                    std::string merr;
                    const bool loaded = scene.load(*files, "_MP_LAND_ENDOR", &resolver, throwing, &merr);
                    std::printf("MapScene::load with a throwing builder: %s (%s)\n", loaded ? "loaded" : "failed", merr.c_str());
                    CHECK(!loaded && !merr.empty() && scene.placements().placements.empty() && scene.mesh().patches.empty());
                }
                struct MapShot {
                    const char* name;
                    const char* png;
                    int water;  // 1 must have water, 0 must not
                };
                const MapShot maps[] = {{"_MP_LAND_ENDOR", "render_map_endor.png", 1},
                                        {"_MP_LAND_DAGOBAH", "render_map_dagobah.png", 1},
                                        {"_MP_LAND_HOTH", "render_map_hoth.png", 0}};
                for (const MapShot& ms : maps) {
                    render::MapScene scene;
                    std::string merr;
                    const bool loaded = scene.load(*files, ms.name, &resolver, pf, &merr);
                    CHECK(loaded);
                    if (!loaded) {
                        std::printf("map %s: %s\n", ms.name, merr.c_str());
                        continue;
                    }
                    CHECK(scene.start(rr, map_streamer, cache, {}, &merr));
                    const render::OrbitCamera cam = scene.tactical_camera(1280.0f / 720.0f);
                    const render::ViewMatrices v = render::make_view(cam, 1280, 720, render::MapScene::kNear, render::MapScene::kFar);
                    render::RecordOptions ro;
                    ro.threads = js.thread_count();
                    render::DrawStats st;
                    int frames = 0;
                    const auto tl = std::chrono::steady_clock::now();
                    for (; frames < 3000 && !scene.ready(); ++frames) {
                        map_streamer.update();
                        scene.update(rr, map_streamer);
                        CHECK(render_frame(g, rr, o2, v, scene.instances(), nullptr, &st, render::DrawMode::Lit, ro, nullptr, scene.sky()));
                    }
                    const double load_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tl).count();
                    CHECK(scene.ready());
                    bool done = false;
                    for (int k = 0; k < 200 && !done; ++k) {
                        map_streamer.update();
                        scene.update(rr, map_streamer);
                        CHECK(render_frame(g, rr, o2, v, scene.instances(), nullptr, &st, render::DrawMode::Lit, ro, nullptr, scene.sky()));
                        const render::WorldStats& ws = scene.world().stats();
                        done = k > 2 && st.pending_pipeline == 0 && st.translation_pending == 0 && st.post_pending == 0 &&
                               ws.terrain_pending == 0 && ws.water_pending == 0;
                        if (!done) {
                            rr.wait_translations();
                            rr.pipelines().wait_idle();
                        }
                    }
                    CHECK(done);
                    // Timing: CPU build + record of the whole frame graph (wall per frame includes the readback wait).
                    render::DrawTiming tm;
                    double build = 0, rec = 0, wall = 0;
                    const int kTimed = 10;
                    for (int k = 0; k < kTimed; ++k) {
                        const auto tw = std::chrono::steady_clock::now();
                        CHECK(render_frame(g, rr, o2, v, scene.instances(), nullptr, &st, render::DrawMode::Lit, ro, &tm, scene.sky()));
                        wall += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tw).count();
                        build += tm.build_ms;
                        rec += tm.record_ms;
                    }
                    CHECK(st.post_pending == 0 && st.pending_pipeline == 0 && st.translation_pending == 0);
                    const std::vector<uint8_t> shot(static_cast<const uint8_t*>(o2.readback.mapped()),
                                                    static_cast<const uint8_t*>(o2.readback.mapped()) + static_cast<size_t>(1280) * 720 * 4);
                    std::vector<uint8_t> rgba = shot;
                    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
                    const std::string png = src_dir + "/logs/" + ms.png;
                    CHECK(render::write_png_rgba(png, 1280, 720, rgba));
                    const render::MapSceneStats& mst = scene.stats();
                    const render::WorldStats& ws = scene.world().stats();
                    std::printf("map %s: grid %ux%u, %u patches (%u visible, %u terrain draws, %llu tris), water %s (%u patches, %u draws, "
                                "reflection %u / refraction %u draws); %u objects -> %u placements (%u distinct models: %u ready, %u failed), "
                                "%u instances, %u mesh draws, %u culled, %u shadow volumes, bloom %s; loaded in %.0f ms (%d frames); CPU "
                                "build %.2f ms + record %.2f ms per frame, %.1f ms wall incl. readback\n",
                                ms.name, scene.map().terrain.width, scene.map().terrain.height, ws.patches, ws.visible_patches,
                                ws.terrain_draws, static_cast<unsigned long long>(ws.terrain_triangles),
                                scene.water().enabled ? scene.water().effect.c_str() : "none", ws.water_patches, ws.water_draws,
                                ws.reflection_draws, ws.refraction_draws, mst.objects, mst.placements, mst.distinct_models, mst.models_ready,
                                mst.models_failed, mst.instances, st.draws, st.culled, st.shadow_volumes, st.bloom ? "yes" : "no", load_ms,
                                frames, build / kTimed, rec / kTimed, wall / kTimed);
                    {   // 0x4B0 mini 03 = starting health fraction (not a scale: placements keep the type's Scale_Factor)
                        uint32_t n_one = 0, n_other = 0, scaled = 0;
                        float lo = 1.0f, hi = 1.0f;
                        for (const terrain::ResolvedPlacement& p : scene.placements().placements) {
                            if (p.initial_health_fraction == 1.0f) {
                                ++n_one;
                                continue;
                            }
                            ++n_other;
                            lo = std::min(lo, p.initial_health_fraction);
                            hi = std::max(hi, p.initial_health_fraction);
                            // Its transform scale is the type's Scale_Factor only (column length of the model -> world matrix).
                            const float sx = std::sqrt(p.transform.m[0][0] * p.transform.m[0][0] + p.transform.m[1][0] * p.transform.m[1][0] +
                                                       p.transform.m[2][0] * p.transform.m[2][0]);
                            if (std::fabs(sx - p.scale) > 1e-4f * std::max(1.0f, p.scale)) ++scaled;
                        }
                        std::printf("  health fraction: %u placements 1, %u other (%g..%g; %u counted by MapSceneStats); %u scaled by it\n",
                                    n_one, n_other, static_cast<double>(lo), static_cast<double>(hi), mst.health_not_full, scaled);
                        CHECK(mst.health_not_full == n_other && scaled == 0);
                        if (std::string_view(ms.name) == "_MP_LAND_ENDOR") CHECK(n_other == 4);  // the Skirmish_Mineral_Processor_Pad props
                    }
                    std::printf("  wrote %s\n", png.c_str());
                    {   // Luma of the shot and attribution of this wave's changes (each variant reverts one).
                        auto luma = [](const uint8_t* p, size_t n, double& mean, double& sd) {
                            double s = 0, s2 = 0;
                            for (size_t i = 0; i < n; ++i) {
                                const double l = 0.299 * p[4 * i] + 0.587 * p[4 * i + 1] + 0.114 * p[4 * i + 2];
                                s += l;
                                s2 += l * l;
                            }
                            mean = s / static_cast<double>(n);
                            sd = std::sqrt(std::max(0.0, s2 / static_cast<double>(n) - mean * mean));
                        };
                        const size_t npx = static_cast<size_t>(1280) * 720;
                        double m0 = 0, s0 = 0;
                        luma(shot.data(), npx, m0, s0);
                        std::printf("  luma %.2f / %.2f (mean / stddev)\n", m0, s0);
                        auto variant = [&](const char* what, std::vector<render::Instance> vi) {
                            CHECK(render_frame(g, rr, o2, v, vi, nullptr, &st, render::DrawMode::Lit, ro, nullptr, scene.sky()));
                            const uint8_t* pv = static_cast<const uint8_t*>(o2.readback.mapped());
                            size_t diff = 0;
                            for (size_t i = 0; i < npx; ++i)
                                diff += (pv[4 * i] != shot[4 * i] || pv[4 * i + 1] != shot[4 * i + 1] || pv[4 * i + 2] != shot[4 * i + 2]) ? 1u : 0u;
                            double m = 0, s = 0;
                            luma(pv, npx, m, s);
                            std::printf("  without %s: luma %.2f / %.2f, %zu pixels differ from the shot\n", what, m, s, diff);
                        };
                        std::vector<render::Instance> base(scene.instances().begin(), scene.instances().end());
                        {
                            std::vector<render::Instance> vi = base;
                            for (auto& in : vi) in.lod_bias = 1.0f;  // previous rule d = min(coverage, ...)
                            variant("LOD_Bias (bias 1)", vi);
                        }
                        {
                            std::vector<render::Instance> vi = base;
                            for (auto& in : vi) in.mesh_hidden = {};  // model default: *_BLAST* name rule + state meshes
                            variant("hardpoint XML masks (name rule)", vi);
                        }
                        {
                            static const materials::float4 kOld[8] = {{1.0f, 1.0f, 1.0f, 1.0f},  {0.85f, 0.1f, 0.1f, 1.0f},
                                                                      {0.1f, 0.3f, 0.9f, 1.0f},  {0.1f, 0.75f, 0.2f, 1.0f},
                                                                      {0.9f, 0.8f, 0.1f, 1.0f},  {0.6f, 0.2f, 0.8f, 1.0f},
                                                                      {0.1f, 0.8f, 0.8f, 1.0f},  {0.9f, 0.5f, 0.1f, 1.0f}};
                            std::vector<render::Instance> vi = base;
                            const auto& pls = scene.placements().placements;
                            CHECK(vi.size() == pls.size());  // every model ready: instance i = placement i
                            for (size_t i = 0; i < pls.size() && i < vi.size(); ++i) vi[i].team_color = kOld[pls[i].owner % 8];
                            variant("9-colour palette (old 8-colour)", vi);
                        }
                        // Owner slots (team-colour rule diagnostics) and the colour fallback textures in view.
                        std::printf("  owners:");
                        for (const auto& oc : scene.owner_histogram())
                            std::printf(" %u x%u%s", oc.owner, oc.objects, oc.neutral ? " (neutral)" : "");
                        std::printf("\n");
                        std::vector<const render::GpuModel*> seen;
                        uint32_t decal_types = 0, decals = 0;
                        for (const auto& p : scene.placements().placements)
                            if (const render::TypeRenderInfo* ti = scene.type_info(p.type_crc)) {
                                decal_types += ti->damage_decals.empty() ? 0u : 1u;
                                decals += static_cast<uint32_t>(ti->damage_decals.size());
                            }
                        std::printf("  hardpoint XML %s: %u placements with Damage_Decal meshes (%u decal names)\n",
                                    resolver.hardpoints_loaded() ? "loaded" : "missing", decal_types, decals);
                        std::printf("  colour fallback textures:");
                        uint32_t fb = 0;
                        for (const render::Instance& in : base) {
                            if (std::find(seen.begin(), seen.end(), in.model) != seen.end()) continue;
                            seen.push_back(in.model);
                            const render::GpuModel& m = *in.model;
                            for (const render::GpuSubmesh& sg : m.submeshes) {
                                auto hit = [&](int t) {
                                    return t >= 0 && static_cast<size_t>(t) < m.textures.size() && m.textures[static_cast<size_t>(t)].fallback &&
                                           m.textures[static_cast<size_t>(t)].role == assets::TextureRole::Color;
                                };
                                int t = sg.base_texture;
                                for (const render::GpuPass& gp : sg.passes)
                                    for (int s = 0; s < 4 && !hit(t); ++s) t = hit(gp.tex[s]) ? gp.tex[s] : t;
                                if (!hit(t)) continue;
                                ++fb;
                                std::printf(" %s:%s->%s", m.asset->name.c_str(), m.asset->meshes[m.asset->submeshes[sg.submesh].mesh].name.c_str(),
                                            m.textures[static_cast<size_t>(t)].name.c_str());
                            }
                        }
                        std::printf("%s\n", fb ? "" : " none");
                        // ALO-hidden shadow volumes are uploaded (masked like render meshes): count them, check the
                        // default mask hides them, and show them through the per-instance mask (Shadow bucket follows).
                        uint32_t hidden_volumes = 0, hidden_volume_models = 0;
                        for (const render::GpuModel* m : seen) {
                            uint32_t here = 0;
                            for (const render::GpuSubmesh& sg : m->submeshes) {
                                const uint32_t fl = m->asset->submeshes[sg.submesh].flags;
                                if ((fl & assets::kSubmeshShadowVolume) && (fl & assets::kSubmeshHidden)) {
                                    ++here;
                                    CHECK(sg.mesh < m->mesh_hidden.size() && m->mesh_hidden[sg.mesh]);
                                }
                            }
                            if (here) std::printf("%s %s (%u)", hidden_volume_models ? "," : "  ALO-hidden shadow volumes uploaded:",
                                                  m->asset->name.c_str(), here);
                            hidden_volumes += here;
                            hidden_volume_models += here ? 1u : 0u;
                        }
                        std::printf("%s %u hidden shadow-volume submeshes in %u of %zu models\n", hidden_volume_models ? ";" : "  ALO-hidden shadow volumes:",
                                    hidden_volumes, hidden_volume_models, seen.size());
                        if (hidden_volumes) {
                            const uint32_t shown_before = st.shadow_volumes;
                            std::vector<std::vector<uint8_t>> masks;
                            masks.reserve(base.size());
                            std::vector<render::Instance> vi = base;
                            for (auto& in : vi) {
                                masks.emplace_back(in.model->asset->meshes.size(), uint8_t{0});  // everything shown
                                in.mesh_hidden = masks.back();
                            }
                            CHECK(render_frame(g, rr, o2, v, vi, nullptr, &st, render::DrawMode::Lit, ro, nullptr, scene.sky()));
                            std::printf("  all meshes shown: %u shadow volumes (masked: %u)\n", st.shadow_volumes, shown_before);
                            CHECK(st.shadow_volumes >= shown_before);
                        }
                        CHECK(render_frame(g, rr, o2, v, scene.instances(), nullptr, &st, render::DrawMode::Lit, ro, nullptr, scene.sky()));
                    }
                    CHECK(ws.terrain_draws > 0 && ws.visible_patches > 0 && mst.instances > 0);
                    CHECK(scene.water().enabled == (ms.water == 1));
                    if (ms.water == 1) CHECK(ws.water_draws > 0 && ws.reflection && ws.refraction);
                    // Thread-count identity on Endor: 1 vs N recording threads, pixel-identical.
                    if (std::string(ms.name) == "_MP_LAND_ENDOR") {
                        render::RecordOptions r1 = ro;
                        r1.threads = 1;
                        CHECK(render_frame(g, rr, o2, v, scene.instances(), nullptr, &st, render::DrawMode::Lit, r1, nullptr, scene.sky()));
                        const uint8_t* p1 = static_cast<const uint8_t*>(o2.readback.mapped());
                        size_t diff = 0;
                        for (size_t i = 0; i < shot.size(); ++i) diff += p1[i] != shot[i];
                        std::printf("  Endor thread identity: 1 vs %u recording threads differ in %zu bytes\n", ro.threads, diff);
                        CHECK(diff == 0);
                        // D.1: inside every non-sorted bucket the (non-translated) lit draws follow the effect
                        // creation order (GpuSubmesh::effect_order), names outside the list last.
                        uint32_t groups_seen = 0, inversions = 0;
                        const render::GpuSubmesh* prev = nullptr;
                        const auto insts = scene.instances();
                        for (const render::DrawListEntry& e : rr.last_draw_list()) {
                            if (e.pass < 0 || e.instance >= insts.size()) continue;
                            const render::GpuSubmesh* gs = nullptr;
                            for (const render::GpuSubmesh& s : insts[e.instance].model->submeshes)
                                if (s.submesh == e.submesh) gs = &s;
                            if (!gs || gs->passes[static_cast<size_t>(e.pass)].translated ||
                                gs->bucket == render::PhaseBucket::Transparent || gs->bucket == render::PhaseBucket::Heat)
                                continue;
                            if (prev && prev->bucket == gs->bucket) {
                                if (gs->effect_order < prev->effect_order) ++inversions;
                                if (gs->effect_name != prev->effect_name) ++groups_seen;
                            }
                            prev = gs;
                        }
                        std::printf("  Endor D.1 effect order: %u effect changes inside buckets, %u inversions\n", groups_seen, inversions);
                        CHECK(inversions == 0 && groups_seen > 0);
                    }
                    // Batched terrain / water draws vs the reference path (one post_translated call per draw):
                    // pixel-identical, with one pipeline / buffer bind per call and no per-draw descriptor work.
                    {
                        const render::WorldBindStats bb = ws.binds;
                        scene.world().set_batch_draws(false);
                        CHECK(render_frame(g, rr, o2, v, scene.instances(), nullptr, &st, render::DrawMode::Lit, ro, nullptr, scene.sky()));
                        const render::WorldBindStats ub = scene.world().stats().binds;
                        const uint8_t* pu = static_cast<const uint8_t*>(o2.readback.mapped());
                        size_t diff = 0;
                        for (size_t i = 0; i < shot.size(); ++i) diff += pu[i] != shot[i];
                        scene.world().set_batch_draws(true);
                        CHECK(render_frame(g, rr, o2, v, scene.instances(), nullptr, &st, render::DrawMode::Lit, ro, nullptr, scene.sky()));
                        const render::WorldBindStats b2 = scene.world().stats().binds;
                        std::printf("  batched vs unbatched world draws: %zu bytes differ; batched %u draws / %u pipeline binds / %u set binds / "
                                    "%u buffer binds / %u set allocs / %u uniform allocs, unbatched %u / %u / %u / %u / %u / %u; "
                                    "cloud texture bindless slot %u\n",
                                    diff, b2.draws, b2.pipeline_binds, b2.set_binds, b2.buffer_binds, b2.set_allocs, b2.uniform_allocs,
                                    ub.draws, ub.pipeline_binds, ub.set_binds, ub.buffer_binds, ub.set_allocs, ub.uniform_allocs,
                                    scene.world().cloud_texture_slot());
                        CHECK(diff == 0);
                        CHECK(bb.draws == ub.draws && b2.draws == ub.draws && ub.draws > 0);
                        CHECK(b2.pipeline_binds <= 4 && b2.buffer_binds <= 8 && b2.uniform_allocs == 0 && b2.set_allocs <= 1);
                        CHECK(ub.pipeline_binds == ub.draws && ub.uniform_allocs >= 2 * ub.draws);
                        CHECK(scene.world().cloud_texture_slot() != 0);
                    }
                    g.ring.wait_idle();
                    scene.stop(rr);
                }
                // (d) Terrain decorations, fog of war and space maps (DECORATIONS.md, SPACE_AND_OPEN.md A).
                {
                    auto luma_of = [](const uint8_t* p, size_t n, double& mean, double& sd) {
                        double s = 0, s2 = 0;
                        for (size_t i = 0; i < n; ++i) {
                            const double l = 0.299 * p[4 * i] + 0.587 * p[4 * i + 1] + 0.114 * p[4 * i + 2];
                            s += l;
                            s2 += l * l;
                        }
                        mean = s / static_cast<double>(n);
                        sd = std::sqrt(std::max(0.0, s2 / static_cast<double>(n) - mean * mean));
                    };
                    struct DecoShot {
                        const char* map;
                        const char* png;
                        bool fow, demo;
                    };
                    const DecoShot dshots[] = {
                        {"_MP_LAND_MANDALORE", "render_map_mandalore_tracks.png", false, false},
                        {"_MP_LAND_BOTHAWUI", "render_map_bothawui_tracks.png", false, false},
                        {"_MP_LAND_MANDALORE", "render_map_mandalore_fow.png", true, true},
                        {"UM11_RAIDERS_OF_THE_LOST_HOLOCRON", "render_map_um11_decals.png", false, false},
                        {"_MP_LAND_ENDOR", "render_map_endor_fow.png", true, true},
                        {"_MP_SPACE_KUAT", "render_space_kuat.png", false, false},
                        {"_MP_SPACE_CORUSCANT", "render_space_coruscant.png", false, false},
                        {"_MP_SPACE_ENDOR", "render_space_endor_fow.png", true, false},
                        {"_MP_SPACE_KESSEL", "render_space_kessel.png", false, false},
                        {"_SPACE_PLANET_YAVIN_01", "render_space_yavin01.png", false, false},
                    };
                    const size_t npx = static_cast<size_t>(1280) * 720;
                    for (const DecoShot& ds : dshots) {
                        render::MapScene scene;
                        std::string merr;
                        if (!scene.load(*files, ds.map, &resolver, pf, &merr)) {
                            std::printf("decorations: map %s not loaded: %s\n", ds.map, merr.c_str());
                            CHECK(std::string(ds.map) == "UM11_RAIDERS_OF_THE_LOST_HOLOCRON" ||
                                  std::string(ds.map) == "_SPACE_PLANET_YAVIN_01");  // story maps: optional
                            continue;
                        }
                        CHECK(scene.start(rr, map_streamer, cache, {}, &merr));
                        render::OrbitCamera cam = scene.tactical_camera(1280.0f / 720.0f);
                        if (!ds.fow) {  // decoration shots look at the first land track / decal (the start view may miss them)
                            const render::WorldDecorations& wd = scene.decorations();
                            const assets::Aabb* b = nullptr;
                            for (const auto& t : wd.tracks)  // shore waves first (type 5), else a land track
                                if (!b && !t.mesh.empty() && t.type == terrain::DecorationType::WaterTrack) b = &t.mesh.bounds;
                            for (const auto& t : wd.tracks)
                                if (!b && !t.mesh.empty()) b = &t.mesh.bounds;
                            if (b) cam.distance = cam.radius = 900.0f;
                            if (!wd.decals.empty()) b = &wd.decals.front().mesh.bounds;
                            if (b) {
                                const float x = 0.5f * (b->min.x + b->max.x), y = 0.5f * (b->min.y + b->max.y);
                                cam.target = {x, y, scene.map().terrain.sample_height(x, y)};
                            }
                        }
                        const render::ViewMatrices v = render::make_view(cam, 1280, 720, render::MapScene::kNear, render::MapScene::kFar);
                        render::RecordOptions ro;
                        ro.threads = js.thread_count();
                        scene.apply(ro);
                        const render::Float3 cc = scene.clear_color();
                        const float clear[4] = {cc.x, cc.y, cc.z, 1.0f};
                        // Dynamic-track demo: a trail driven along a circle around the camera target + a blob at its head.
                        std::vector<terrain::DynamicTrackGenerator> gens;
                        std::vector<terrain::ShadowBlob> blobs;
                        if (ds.demo && !scene.dynamic_track_params().empty()) {
                            const auto& fxs = scene.dynamic_track_params();
                            const terrain::DynamicTrackParams* pick = &fxs.front();
                            for (const auto& p : fxs)
                                if (p.mode == terrain::RenderMode::DiffuseAlpha) {
                                    pick = &p;
                                    break;
                                }
                            gens.emplace_back(*pick);
                            float hx = 0, hy = 0;
                            for (int k = 0; k <= 150; ++k) {
                                const float a = 0.02f * static_cast<float>(k);
                                hx = cam.target.x + 150.0f * std::cos(a);
                                hy = cam.target.y + 150.0f * std::sin(a);
                                gens[0].update(hx, hy, 1.0f / 30.0f);
                            }
                            terrain::ShadowBlob b;
                            b.position[0] = hx;
                            b.position[1] = hy;
                            b.width = b.height = 60.0f;
                            blobs.push_back(b);
                            std::printf("decorations: dynamic track demo with %s (%s, %zu points)\n", pick->name.c_str(), pick->texture.c_str(),
                                        gens[0].points().size());
                        }
                        if (ds.fow) {
                            scene.reveal_circle(cam.target.x, cam.target.y, scene.space() ? 1500.0f : 220.0f);
                            CHECK(scene.apply_fog_of_war());
                        }
                        render::DrawStats st;
                        bool done = false;
                        for (int k = 0; k < 3200 && !done; ++k) {
                            map_streamer.update();
                            scene.update(rr, map_streamer);
                            scene.set_dynamic_tracks(gens);
                            scene.set_shadow_blobs(blobs);
                            CHECK(render_frame(g, rr, o2, v, scene.instances(), nullptr, &st, render::DrawMode::Lit, ro, nullptr, scene.sky(), clear));
                            const render::WorldStats& ws = scene.world().stats();
                            done = scene.ready() && k > 2 && st.pending_pipeline == 0 && st.translation_pending == 0 && st.post_pending == 0 &&
                                   ws.terrain_pending == 0 && ws.water_pending == 0 && ws.decorations.pending == 0 &&
                                   (!ds.fow || scene.space() || ws.decorations.fow_draws > 0) &&
                                   (!ds.fow || !scene.space() || ws.decorations.space_fow_draws > 0);
                            if (!done && scene.ready()) {
                                rr.wait_translations();
                                rr.pipelines().wait_idle();
                            }
                        }
                        CHECK(done);
                        render::DrawTiming tm;
                        double build = 0, rec = 0, wall = 0;
                        const int kTimed = 10;
                        for (int k = 0; k < kTimed; ++k) {
                            const auto tw = std::chrono::steady_clock::now();
                            CHECK(render_frame(g, rr, o2, v, scene.instances(), nullptr, &st, render::DrawMode::Lit, ro, &tm, scene.sky(), clear));
                            wall += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tw).count();
                            build += tm.build_ms;
                            rec += tm.record_ms;
                        }
                        CHECK(st.post_pending == 0);
                        const render::DrawStats shot_st = st;
                        const render::DecorationStats dst = scene.world().stats().decorations;
                        const std::vector<uint8_t> shot(static_cast<const uint8_t*>(o2.readback.mapped()),
                                                        static_cast<const uint8_t*>(o2.readback.mapped()) + npx * 4);
                        std::vector<uint8_t> rgba = shot;
                        for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
                        const std::string png = src_dir + "/logs/" + ds.png;
                        CHECK(render::write_png_rgba(png, 1280, 720, rgba));
                        double m0 = 0, s0 = 0;
                        luma_of(shot.data(), npx, m0, s0);
                        const render::MapSceneStats& mst = scene.stats();
                        std::printf("decorations %s: tracks %u (%u built, %u on water), decals %u (%u built), %llu decoration tris, built in "
                                    "%.1f ms; FOW grid %ux%u; drawn: %u land tracks, %u water tracks, %u dynamic, %u decals, %u blobs -> %u draws "
                                    "(%u FF, %u translated, %u FOW patches, %u water-track, %u space FOW), %u pipeline binds, %llu tris, %u pending; "
                                    "%u instances, %u background (%u placements), %u mesh draws, %u shadow volumes; CPU build %.2f + record %.2f ms, "
                                    "%.1f ms wall; luma %.2f / %.2f\n",
                                    ds.map, mst.tracks, mst.tracks_built, mst.water_tracks, mst.decals, mst.decals_built,
                                    static_cast<unsigned long long>(mst.decoration_triangles), mst.decoration_ms, mst.fow_width, mst.fow_height,
                                    dst.land_tracks, dst.water_tracks, dst.dynamic_tracks, dst.decals, dst.blobs, dst.draws, dst.ff_draws,
                                    dst.translated_draws, dst.fow_draws, dst.water_track_draws, dst.space_fow_draws, dst.pipeline_binds,
                                    static_cast<unsigned long long>(dst.triangles), dst.pending, mst.instances, shot_st.sky_draws,
                                    mst.background_placements, shot_st.draws, shot_st.shadow_volumes, build / kTimed, rec / kTimed, wall / kTimed, m0,
                                    s0);
                        std::printf("  wrote %s\n", png.c_str());
                        if (scene.space()) {
                            // SPACE_AND_OPEN.md D.7 worked numbers (default Space_Mode camera, 16:9): each named backdrop
                            // alone in the background pass (same camera, near 10 / far 100000), lit pixels measured,
                            // next to the analytic sphere (radius Scale_Factor x 4.92 at the .ted position).
                            struct Want {
                                const char* map;
                                const char* type;
                                const char* expect;
                                float ndc_x, ndc_y;  // D.7 table centre
                            };
                            static const Want wants[] = {
                                {"_MP_SPACE_KUAT", "KUAT_BACKDROP_LARGE", "lower ~38% of the screen, ~1.37 screen heights", 0.08f, -1.70f},
                                {"_MP_SPACE_ENDOR", "ENDOR_BACKDROP_LARGE", "centred on the bottom edge right of centre, ~1.2 screen heights", 0.45f, -0.99f},
                                {"_MP_SPACE_ENDOR", "GAS_BACKDROP_LARGE", "half-visible at the left edge, mid-height, ~0.70 screen heights", -1.07f, -0.01f},
                                {"_MP_SPACE_KESSEL", "MOON_LARGE", "upper left, ~0.41 screen heights", -0.33f, 0.67f},
                            };
                            const bool fow_was = scene.world().fow_enabled();
                            scene.set_fow_enabled(false);
                            for (const terrain::ResolvedPlacement& p : scene.placements().placements) {
                                std::string tn = p.type_name;
                                for (char& ch : tn) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                                const Want* w = nullptr;
                                for (const Want& x : wants)
                                    if (tn == x.type && std::string(ds.map) == x.map) w = &x;
                                if (!w) continue;
                                const render::Instance* bi = nullptr;
                                for (const render::Instance& s : scene.sky())
                                    if (std::memcmp(&s.transform, &p.transform, sizeof p.transform) == 0) bi = &s;
                                CHECK(bi != nullptr);  // In_Background -> background scene
                                if (!bi) continue;
                                // Analytic: view-space centre, elevation +- asin(r / d), NDC y = tan(angle) x proj[1][1].
                                const render::Float4 vc = v.view * render::Float4{p.position[0], p.position[1], p.position[2], 1.0f};
                                const float d = std::sqrt(vc.x * vc.x + vc.y * vc.y + vc.z * vc.z);
                                const float r = p.scale * 4.92f;
                                const float el = std::atan2(vc.y, -vc.z), az = std::atan2(vc.x, -vc.z), ar = std::asin(std::min(1.0f, r / d));
                                const float ys = v.proj.m[1][1], xs = v.proj.m[0][0];
                                const float top = std::tan(el + ar) * ys, bot = std::tan(el - ar) * ys;
                                render::DrawStats pst;
                                for (int k = 0; k < 60; ++k) {
                                    CHECK(render_frame(g, rr, o2, v, {}, nullptr, &pst, render::DrawMode::Lit, ro, nullptr,
                                                       std::span<const render::Instance>(bi, 1), clear));
                                    if (k > 1 && pst.pending_pipeline == 0 && pst.translation_pending == 0 && pst.post_pending == 0) break;
                                    rr.wait_translations();
                                    rr.pipelines().wait_idle();
                                }
                                CHECK(pst.post_pending == 0);
                                const uint8_t* pv = static_cast<const uint8_t*>(o2.readback.mapped());
                                uint32_t r0 = 720, r1 = 0, c0 = 1280, c1 = 0;
                                size_t lit = 0;
                                for (uint32_t yy = 0; yy < 720; ++yy)
                                    for (uint32_t xx = 0; xx < 1280; ++xx) {
                                        const uint8_t* q = pv + 4 * (static_cast<size_t>(yy) * 1280 + xx);
                                        if ((q[0] * 299 + q[1] * 587 + q[2] * 114) / 1000 <= 6) continue;
                                        ++lit;
                                        r0 = std::min(r0, yy);
                                        r1 = std::max(r1, yy);
                                        c0 = std::min(c0, xx);
                                        c1 = std::max(c1, xx);
                                    }
                                std::string name = std::string("render_space_") + tn + ".png";
                                for (char& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                                std::vector<uint8_t> prgba(pv, pv + npx * 4);
                                for (size_t i = 3; i < prgba.size(); i += 4) prgba[i] = 255;
                                CHECK(render::write_png_rgba(src_dir + "/logs/" + name, 1280, 720, prgba));
                                std::printf("  D.7 %s (scale %.0f, pos %.0f %.0f %.0f): analytic centre NDC (%.2f, %.2f), d %.0f, angular radius "
                                            "%.1f deg, limbs NDC y %.2f..%.2f, angular diameter %.2f vertical FOVs, %.0f%% of the height from the bottom; "
                                            "measured: %zu px lit (%.1f%%), rows %u..%u (top NDC y %.2f, %.0f%% of the height from the bottom), "
                                            "cols %u..%u; D.7 expects %s; wrote logs/%s\n",
                                            p.type_name.c_str(), static_cast<double>(p.scale), static_cast<double>(p.position[0]),
                                            static_cast<double>(p.position[1]), static_cast<double>(p.position[2]),
                                            static_cast<double>(std::tan(az) * xs), static_cast<double>(std::tan(el) * ys), static_cast<double>(d),
                                            static_cast<double>(ar * 57.29578f), static_cast<double>(bot), static_cast<double>(top),
                                            static_cast<double>(ar / std::atan(1.0f / ys)),  // 2 ar / vertical FOV
                                            static_cast<double>(50.0f * (std::clamp(top, -1.0f, 1.0f) + 1.0f)),
                                            lit, 100.0 * static_cast<double>(lit) / static_cast<double>(npx), r0, r1,
                                            lit ? 1.0 - 2.0 * static_cast<double>(r0) / 720.0 : 0.0,
                                            lit ? 100.0 * (720.0 - static_cast<double>(r0)) / 720.0 : 0.0, c0, c1, w->expect, name.c_str());
                                CHECK(lit > 0 && pst.sky_draws > 0);
                                CHECK(std::fabs(std::tan(az) * xs - w->ndc_x) < 0.03f && std::fabs(std::tan(el) * ys - w->ndc_y) < 0.03f);
                                // Idle spin on the real clip: bone Hoth turns 90 deg about Z between tick 0 and tick 1800 (60 s).
                                const int32_t hb = bi->model && bi->model->asset ? bi->model->asset->skeleton.find("Hoth") : -1;
                                if (hb < 0 || static_cast<size_t>(hb) >= bi->bone_world.size())
                                    std::printf("    idle spin (%s): no Hoth bone / no idle pose (%zu bones posed)\n", p.model_name.c_str(),
                                                bi->bone_world.size());
                                if (hb >= 0 && static_cast<size_t>(hb) < bi->bone_world.size()) {
                                    const uint64_t tick0 = scene.sim_tick();
                                    const float a0 = z_angle_deg(bi->bone_world[static_cast<size_t>(hb)]);
                                    scene.set_sim_tick(tick0 + 1800);
                                    const float a1 = z_angle_deg(bi->bone_world[static_cast<size_t>(hb)]);
                                    scene.set_sim_tick(tick0);
                                    float da = a1 - a0;
                                    while (da > 180.0f) da -= 360.0f;
                                    while (da <= -180.0f) da += 360.0f;
                                    std::printf("    idle spin (%s _IDLE_00): Hoth %.2f -> %.2f deg after 60 s (%.2f)\n", p.model_name.c_str(),
                                                static_cast<double>(a0), static_cast<double>(a1), static_cast<double>(da));
                                    // The vanilla clips key +360 deg at frame 239 of 240 (frame 0 == 239 pose), so frame 60 is
                                    // 60 x 360 / 239 = 90.38 deg; the 1 fps / 30 Hz time base itself is exact (test_idle_spin).
                                    CHECK(std::fabs(std::fabs(da) - 90.0f) < 0.5f);
                                }
                                CHECK(std::fabs(p.scale - (tn == "MOON_LARGE" ? 300.0f : 500.0f)) < 1e-3f);  // Scale_Factor only
                            }
                            // Death_Star_Chunks (D.7 1): Scale_Factor 1, NOT In_Background -> main scene; framed from the
                            // default pitch / yaw at distance 4500 so it lies inside the main far plane / fade range.
                            for (size_t pi = 0; pi < scene.placements().placements.size(); ++pi) {
                                const terrain::ResolvedPlacement& p = scene.placements().placements[pi];
                                std::string tn = p.type_name;
                                for (char& ch : tn) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                                if (tn != "DEATH_STAR_CHUNKS") continue;
                                bool in_main = false, in_bg = false;
                                for (const render::Instance& s : scene.instances())
                                    in_main = in_main || std::memcmp(&s.transform, &p.transform, sizeof p.transform) == 0;
                                for (const render::Instance& s : scene.sky())
                                    in_bg = in_bg || std::memcmp(&s.transform, &p.transform, sizeof p.transform) == 0;
                                render::OrbitCamera dc = scene.tactical_camera(1280.0f / 720.0f);
                                dc.target = {p.position[0], p.position[1], p.position[2]};
                                dc.distance = 4500.0f;
                                const render::ViewMatrices dv = render::make_view(dc, 1280, 720, render::MapScene::kNear, render::MapScene::kFar);
                                render::DrawStats pst;
                                for (int k = 0; k < 60; ++k) {
                                    CHECK(render_frame(g, rr, o2, dv, scene.instances(), nullptr, &pst, render::DrawMode::Lit, ro, nullptr,
                                                       scene.sky(), clear));
                                    if (k > 1 && pst.pending_pipeline == 0 && pst.translation_pending == 0 && pst.post_pending == 0) break;
                                    rr.wait_translations();
                                    rr.pipelines().wait_idle();
                                }
                                CHECK(pst.post_pending == 0);
                                const uint8_t* pv = static_cast<const uint8_t*>(o2.readback.mapped());
                                std::vector<uint8_t> prgba(pv, pv + npx * 4);
                                for (size_t i = 3; i < prgba.size(); i += 4) prgba[i] = 255;
                                CHECK(render::write_png_rgba(src_dir + "/logs/render_space_yavin01_ds2_chunks.png", 1280, 720, prgba));
                                double m1 = 0, s1 = 0;
                                luma_of(pv, npx, m1, s1);
                                std::printf("  D.7 Death_Star_Chunks (%s, scale %.2f, pos %.0f %.0f %.0f): main scene %s, background %s; framed at "
                                            "distance 4500: %u draws, luma %.2f / %.2f; wrote logs/render_space_yavin01_ds2_chunks.png\n",
                                            p.model_name.c_str(), static_cast<double>(p.scale), static_cast<double>(p.position[0]),
                                            static_cast<double>(p.position[1]), static_cast<double>(p.position[2]), in_main ? "yes" : "no",
                                            in_bg ? "yes" : "no", pst.draws, m1, s1);
                                CHECK(in_main && !in_bg && std::fabs(p.scale - 1.0f) < 1e-6f);
                            }
                            scene.set_fow_enabled(fow_was);
                            std::printf("  %u background placements, %u with an idle clip; %u placements with health fraction != 1\n",
                                        mst.background_placements, mst.background_animated, mst.health_not_full);
                        }
                        if (!scene.space() && dst.draws > dst.fow_draws) {  // attribution: types 0-3 / 5 off
                            scene.world().set_decorations_visible(false);
                            CHECK(render_frame(g, rr, o2, v, scene.instances(), nullptr, &st, render::DrawMode::Lit, ro, nullptr, scene.sky(), clear));
                            const uint8_t* pv = static_cast<const uint8_t*>(o2.readback.mapped());
                            size_t diff = 0;
                            for (size_t i = 0; i < npx; ++i)
                                diff += (pv[4 * i] != shot[4 * i] || pv[4 * i + 1] != shot[4 * i + 1] || pv[4 * i + 2] != shot[4 * i + 2]) ? 1u : 0u;
                            std::printf("  without decorations (types 0-3, 5): %zu pixels differ\n", diff);
                            CHECK(diff > 0);
                            scene.world().set_decorations_visible(true);
                        }
                        if (!scene.space()) {
                            CHECK(dst.pending == 0);
                            if (mst.tracks_built > 0) CHECK(dst.land_tracks + dst.water_tracks > 0 && dst.draws > 0);
                            if (mst.decals_built > 0) CHECK(dst.decals > 0);
                        } else {
                            CHECK(scene.space() && mst.background_placements + static_cast<uint32_t>(scene.sky().size()) > 0);
                            CHECK(shot_st.sky_draws > 0);
                        }
                        if (ds.fow) {
                            // The same frame with the fog of war off: FOW_TEXTURE white, no FOW pass -> brighter (land) / no plane.
                            scene.set_fow_enabled(false);
                            CHECK(render_frame(g, rr, o2, v, scene.instances(), nullptr, &st, render::DrawMode::Lit, ro, nullptr, scene.sky(), clear));
                            const uint8_t* pv = static_cast<const uint8_t*>(o2.readback.mapped());
                            double m1 = 0, s1 = 0;
                            luma_of(pv, npx, m1, s1);
                            size_t diff = 0;
                            for (size_t i = 0; i < npx; ++i) diff += (pv[4 * i] != shot[4 * i] || pv[4 * i + 1] != shot[4 * i + 1]) ? 1u : 0u;
                            std::printf("  fog of war off: luma %.2f / %.2f, %zu pixels differ\n", m1, s1, diff);
                            CHECK(diff > 0);
                            if (!scene.space()) CHECK(m1 > m0);
                            scene.set_fow_enabled(true);
                        }
                        // The map stops (world cloud / FOW textures released and, after collect, destroyed); the models stay
                        // with the streamer. Inline draw() frames of the same instances must not bind the dead views or
                        // retired slots (validation would report destroyed image views in the translated texture sets).
                        const std::vector<render::Instance> kept(scene.instances().begin(), scene.instances().end());
                        g.ring.wait_idle();
                        scene.stop(rr);
                        CHECK(rr.world() == nullptr);
                        for (int k = 0; k < 3; ++k) CHECK(draw_frame(g, rr, o2, v, kept));
                    }
                    // W_STARS_BACK02 star backdrop (NU2C, MeshAdditiveVColor): drawn as a space background object at the origin,
                    // camera inside it. Its stored vertex colour 0 (320 vertices) is black under the additive blend.
                    {
                        const uint32_t h = map_streamer.request("W_STARS_BACK02.ALO");
                        for (int k = 0; k < 600 && (map_streamer.state(h) == render::ModelStreamer::State::Loading ||
                                                    map_streamer.state(h) == render::ModelStreamer::State::Uploading);
                             ++k) {
                            map_streamer.update();
                            CHECK(render_frame(g, rr, o2, render::make_view(render::OrbitCamera{}, 1280, 720), {}, nullptr));
                        }
                        render::GpuModel* sm = map_streamer.model(h);
                        if (map_streamer.state(h) == render::ModelStreamer::State::Ready && sm) {
                            uint32_t zero = 0, verts = 0;
                            for (const assets::Submesh& s : sm->asset->submeshes) {
                                const uint32_t stride = assets::vertex_stride(s.layout);
                                for (const assets::VertexAttribute& a : assets::vertex_attributes(s.layout)) {
                                    if (a.semantic != assets::VertexSemantic::Color) continue;
                                    for (uint32_t vi = 0; vi < s.vertex_count; ++vi) {
                                        const uint8_t* c = s.vertices.data() + static_cast<size_t>(vi) * stride + a.offset;
                                        zero += (c[0] | c[1] | c[2]) == 0 ? 1u : 0u;
                                        ++verts;
                                    }
                                }
                            }
                            render::Instance bi;
                            bi.model = sm;
                            for (const float pitch : {0.3f, -1.3f}) {  // towards the horizon, then straight up
                            render::OrbitCamera sc;
                            sc.distance = 1.0f;
                            sc.pitch = pitch;
                            render::RecordOptions ro;
                            ro.space = true;
                            const float black[4] = {0, 0, 0, 1};
                            const render::ViewMatrices sv = render::make_view(sc, 1280, 720, 10.0f, 7000.0f);
                            render::DrawStats st;
                            for (int k = 0; k < 200; ++k) {
                                map_streamer.update();
                                CHECK(render_frame(g, rr, o2, sv, {}, nullptr, &st, render::DrawMode::Lit, ro, nullptr,
                                                   std::span<const render::Instance>(&bi, 1), black));
                                if (k > 2 && st.pending_pipeline == 0 && st.translation_pending == 0 && st.post_pending == 0) break;
                                rr.wait_translations();
                                rr.pipelines().wait_idle();
                            }
                            CHECK(st.post_pending == 0);
                            const uint8_t* p = static_cast<const uint8_t*>(o2.readback.mapped());
                            size_t lit = 0, bright = 0;
                            double mean = 0, sd = 0;
                            luma_of(p, npx, mean, sd);
                            for (size_t i = 0; i < npx; ++i) {
                                const int l = (p[4 * i] * 299 + p[4 * i + 1] * 587 + p[4 * i + 2] * 114) / 1000;
                                lit += l > 8 ? 1u : 0u;
                                bright += l > 64 ? 1u : 0u;
                            }
                            std::vector<uint8_t> rgba(p, p + npx * 4);
                            for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
                            const char* name = pitch > 0.0f ? "render_stars_back02.png" : "render_stars_back02_up.png";
                            CHECK(render::write_png_rgba(src_dir + "/logs/" + name, 1280, 720, rgba));
                            std::printf("W_STARS_BACK02 (pitch %.1f): %u of %u vertices with colour 0; %u sky draws; %zu pixels lit (> 8), %zu "
                                        "bright (> 64) of %zu, luma %.2f / %.2f; wrote logs/%s\n",
                                        static_cast<double>(pitch), zero, verts, st.sky_draws, lit, bright, npx, mean, sd, name);
                            }
                        } else {
                            std::printf("W_STARS_BACK02: not loaded (%s)\n", map_streamer.error(h).c_str());
                        }
                    }
                    // D.2: W_PLANET_DEATHSTAR_II_DESTROYED - 42 alD3dVertN Planet.fx submeshes loaded through the effect's
                    // _ALAMO_VERTEX_TYPE as NU2U3U3 (stored UVs, T = B = 0), bucket 3. D.7 1: its geometry is that of
                    // Death_Star_Chunks (Scale_Factor 1.0, not In_Background), so it is drawn at scale 1 in the MAIN
                    // space scene (model units are world units), framed by an orbit camera.
                    {
                        const uint32_t h = map_streamer.request("W_PLANET_DEATHSTAR_II_DESTROYED.ALO");
                        for (int k = 0; k < 600 && (map_streamer.state(h) == render::ModelStreamer::State::Loading ||
                                                    map_streamer.state(h) == render::ModelStreamer::State::Uploading);
                             ++k) {
                            map_streamer.update();
                            CHECK(render_frame(g, rr, o2, render::make_view(render::OrbitCamera{}, 1280, 720), {}, nullptr));
                        }
                        render::GpuModel* pm = map_streamer.model(h);
                        if (map_streamer.state(h) == render::ModelStreamer::State::Ready && pm) {
                            uint32_t planet = 0, overridden = 0, opaque = 0, lit = 0;
                            for (const render::GpuSubmesh& sg : pm->submeshes) {
                                const assets::Submesh& s = pm->asset->submeshes[sg.submesh];
                                if (sg.effect_name != "PLANET") continue;
                                ++planet;
                                overridden += s.vertex_type_overridden && sg.layout == materials::VertexLayout::NU2U3U3 ? 1u : 0u;
                                opaque += sg.bucket == render::PhaseBucket::Opaque ? 1u : 0u;
                                lit += sg.lit == render::LitStatus::Lit ? 1u : 0u;
                            }
                            const assets::Sphere sp = pm->drawn_sphere();
                            const float kScale = 1.0f;  // Death_Star_Chunks Scale_Factor (D.7 1)
                            render::Instance pi;
                            pi.model = pm;
                            render::OrbitCamera sc;
                            // Framed on half the drawn sphere (outlying chunks reach 4275 from its centre); clip planes
                            // still cover the whole radius.
                            sc.frame({sp.center.x, sp.center.y, sp.center.z}, 0.5f * sp.radius, 1280.0f / 720.0f);
                            sc.radius = sp.radius;
                            render::RecordOptions ro;
                            ro.space = true;
                            const float black[4] = {0, 0, 0, 1};
                            const render::ViewMatrices sv = render::make_view(sc, 1280, 720);  // framing clip planes
                            render::DrawStats st;
                            for (int k = 0; k < 200; ++k) {
                                map_streamer.update();
                                CHECK(render_frame(g, rr, o2, sv, std::span<const render::Instance>(&pi, 1), nullptr, &st,
                                                   render::DrawMode::Lit, ro, nullptr, {}, black));
                                if (k > 2 && st.pending_pipeline == 0 && st.translation_pending == 0 && st.post_pending == 0) break;
                                rr.wait_translations();
                                rr.pipelines().wait_idle();
                            }
                            CHECK(st.post_pending == 0);
                            const uint8_t* p = static_cast<const uint8_t*>(o2.readback.mapped());
                            size_t covered = 0;
                            double mean = 0, sd = 0;
                            luma_of(p, npx, mean, sd);
                            for (size_t i = 0; i < npx; ++i) covered += (p[4 * i] | p[4 * i + 1] | p[4 * i + 2]) > 8 ? 1u : 0u;
                            std::vector<uint8_t> rgba(p, p + npx * 4);
                            for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
                            CHECK(render::write_png_rgba(src_dir + "/logs/render_deathstar2_destroyed.png", 1280, 720, rgba));
                            std::printf("W_PLANET_DEATHSTAR_II_DESTROYED: %u Planet.fx submeshes (%u loaded as NU2U3U3 via _ALAMO_VERTEX_TYPE, "
                                        "%u in bucket 3, %u lit); drawn sphere centre (%.0f, %.0f, %.0f) radius %.0f, scale %.2f (main scene), "
                                        "drawn bounds (%.0f %.0f %.0f)..(%.0f %.0f %.0f), asset bounds (%.0f %.0f %.0f)..(%.0f %.0f %.0f), "
                                        "camera distance %.0f; %u draws, %u sky draws (%u pending), "
                                        "%.1f%% covered, luma %.2f / %.2f; wrote logs/render_deathstar2_destroyed.png\n",
                                        planet, overridden, opaque, lit, static_cast<double>(sp.center.x), static_cast<double>(sp.center.y),
                                        static_cast<double>(sp.center.z), static_cast<double>(sp.radius), static_cast<double>(kScale),
                                        static_cast<double>(pm->drawn_bounds().min.x), static_cast<double>(pm->drawn_bounds().min.y),
                                        static_cast<double>(pm->drawn_bounds().min.z), static_cast<double>(pm->drawn_bounds().max.x),
                                        static_cast<double>(pm->drawn_bounds().max.y), static_cast<double>(pm->drawn_bounds().max.z),
                                        static_cast<double>(pm->asset->bounds.min.x), static_cast<double>(pm->asset->bounds.min.y),
                                        static_cast<double>(pm->asset->bounds.min.z), static_cast<double>(pm->asset->bounds.max.x),
                                        static_cast<double>(pm->asset->bounds.max.y), static_cast<double>(pm->asset->bounds.max.z),
                                        static_cast<double>(sc.distance), st.draws, st.sky_draws, st.pending_pipeline + st.translation_pending,
                                        100.0 * static_cast<double>(covered) / static_cast<double>(npx), mean, sd);
                            CHECK(planet == 42 && overridden == planet && opaque == planet && lit == planet);
                            CHECK(st.draws > 0 && st.sky_draws == 0 && covered > npx / 100);
                        } else {
                            std::printf("W_PLANET_DEATHSTAR_II_DESTROYED: not loaded (%s)\n", map_streamer.error(h).c_str());
                        }
                    }
                }
                g.ring.wait_idle();
                map_streamer.destroy_all();
                // Bindless slot recycling (rhi BindlessTextureTable): load / unload Endor twice; released slots retire
                // through the frame timeline and are reused, so the high-water mark does not grow.
                {
                    render::OrbitCamera empty_cam;
                    const render::ViewMatrices ev = render::make_view(empty_cam, 1280, 720);
                    struct Cycle {
                        uint32_t live = 0, high_water = 0, white = 0, pending_after = 0, live_after = 0;
                    };
                    // One Endor load / unload on `r`; `white` = resident textures that got no slot (table full).
                    auto cycle = [&](render::Renderer& r) {
                        Cycle c;
                        render::ModelStreamer s2(r, cache, js);
                        render::MapScene scene;
                        std::string merr;
                        CHECK(scene.load(*files, "_MP_LAND_ENDOR", &resolver, pf, &merr) && scene.start(r, s2, cache, {}, &merr));
                        for (int k = 0; k < 3000 && !scene.ready(); ++k) {
                            s2.update();
                            scene.update(r, s2);
                            CHECK(render_frame(g, r, o2, ev, scene.instances(), nullptr, nullptr, render::DrawMode::Lit, {}, nullptr, scene.sky()));
                        }
                        CHECK(scene.ready());
                        c.live = r.textures_resident();
                        c.high_water = r.texture_slot_high_water();
                        std::vector<const render::GpuModel*> seen;
                        for (const render::Instance& in : scene.instances()) {
                            if (std::find(seen.begin(), seen.end(), in.model) != seen.end()) continue;
                            seen.push_back(in.model);
                            for (const render::GpuTexture& t : in.model->textures) c.white += (t.resident && t.image && t.slot == 0) ? 1u : 0u;
                        }
                        c.white += scene.world().cloud_texture_slot() == 0 ? 1u : 0u;
                        g.ring.wait_idle();
                        scene.stop(r);
                        s2.destroy_all();
                        CHECK(render_frame(g, r, o2, ev, {}, nullptr));  // begin_frame + collect
                        CHECK(render_frame(g, r, o2, ev, {}, nullptr));  // releases retire one frame later (retire_value)
                        c.pending_after = r.texture_slots_pending();
                        c.live_after = r.textures_resident();
                        return c;
                    };
                    CHECK(render_frame(g, rr, o2, ev, {}, nullptr));  // collect the slots released above
                    CHECK(render_frame(g, rr, o2, ev, {}, nullptr));
                    const uint32_t live0 = rr.textures_resident();
                    const Cycle probe = cycle(rr);
                    const uint32_t need = probe.live - live0;
                    // A renderer whose table holds Endor's textures + 8 slots: without recycling the second load
                    // would run out of slots (textures drawing white); with it both loads fit.
                    render::Renderer r2;
                    render::RendererDesc rd2 = rrd;
                    rd2.bindless_slots = need + 1 + 8;
                    CHECK(r2.init(g.ctx, g.alloc, g.up, rd2));
                    r2.set_frame_ring(&g.ring);
                    r2.set_lighting(rr.lighting());
                    const Cycle c1 = cycle(r2), c2 = cycle(r2);
                    std::printf("bindless recycling: Endor needs %u slots; table of %u slots: load 1 %u live (high water %u, %u white), "
                                "load 2 %u live (high water %u, %u white); after each unload %u / %u pending, %u / %u live\n",
                                need, rd2.bindless_slots, c1.live, c1.high_water, c1.white, c2.live, c2.high_water, c2.white,
                                c1.pending_after, c2.pending_after, c1.live_after, c2.live_after);
                    CHECK(need > 8 && probe.white == 0 && probe.pending_after == 0 && probe.live_after == live0);
                    CHECK(c1.live == need && c2.live == need && c1.white == 0 && c2.white == 0);
                    CHECK(c2.high_water <= rd2.bindless_slots);  // no growth past the table: slots were reused
                    CHECK(c1.pending_after == 0 && c2.pending_after == 0 && c1.live_after == 0 && c2.live_after == 0);
                    g.ring.wait_idle();
                    r2.shutdown();
                }
                o2.destroy(g);
            }            const auto es = effects.stats();
            std::printf("effects: %zu loaded, %zu failed, %zu via FF fallback, %zu untranslated passes\n", es.loaded, es.failed,
                        es.ff_fallback, es.untranslated_passes);
            g.ring.wait_idle();
            o.destroy(g);
            streamer.destroy_all();
            // SH lighting contract over every FrameCB of the real-data frames (main, shadow bucket, decorations,
            // prim geometry, stencil darken; land, space and background passes).
            std::printf("SH contract: %u FrameCBs with zero SH under a lit scene\n", rr.frame_cb_violations());
            CHECK(rr.frame_cb_violations() == 0);
            rr.shutdown();
        }
    } else if (!root || !*root) {
        std::printf("real data: ALAMO_FOC_DATA not set, ship renders skipped\n");
    }

    g.ring.wait_idle();
    g.renderer.shutdown();
    g.ring.shutdown();
    g.up.shutdown();
    g.alloc.shutdown();
    const uint32_t errors = g.ctx.validation_errors(), warnings = g.ctx.validation_warnings();
    std::printf("gpu: validation %s, %u errors, %u warnings\n", g.ctx.validation_enabled() ? "on" : "off", errors, warnings);
    CHECK(errors == 0 && warnings == 0);
    g.ctx.shutdown();
    return 0;
}

}  // namespace test_render_t
