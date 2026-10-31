// alamo_game App: render, frame and the presentation on/off check (split from game_main.cpp).
#include "game_app.hpp"

namespace alamo_game_app {

// ------------------------------------------------------------------------------------------------ render
bool App::present_settled() const {
    if (screen_ == Screen::Galactic) return gview_.settled() && draw_stats_.pending_pipeline == 0 && draw_stats_.translation_pending == 0;
    if (screen_ == Screen::Battle || screen_ == Screen::BattleLoad || (screen_ == Screen::Menu && demo_session() && bview_.active()))
        return bview_.settled(draw_stats_);
    return true;
}

bool App::render_frame() {
    if (!sc_.valid() || win_.minimized()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        return true;
    }
    if (bview_.active()) bview_.pre_frame();  // fog uploads before this frame's acquire barriers
    rhi::Frame* f = gpu_.ring.begin_frame();
    if (!f) return false;
    uint32_t image = 0;
    const rhi::SwapchainStatus as = sc_.acquire(f->acquire_semaphore(), image);
    if (as == rhi::SwapchainStatus::OutOfDate || as == rhi::SwapchainStatus::Minimized || as == rhi::SwapchainStatus::Error) {
        gpu_.ring.end_frame(*f);
        gpu_.ring.wait_idle();
        sc_.recreate(win_.width(), win_.height());
        return as != rhi::SwapchainStatus::Error;
    }
    VkCommandBuffer cmd = f->cmd();
    const rhi::DeviceFns& fn = ctx_.vkd();
    const rhi::AcquireResult acq = gpu_.up.record_acquires(cmd);
    gpu_.renderer.set_time(static_cast<float>(now_s_));
    gview_.record_portraits(*f, cmd);  // planet portraits: before the main pass
    ui_.upload_pages(gpu_, gview_.portraits());
    const FrameTargets::Slot& slot = targets_.slots[f->slot() % targets_.slots.size()];
    const uint32_t w = targets_.width, h = targets_.height;
    render::PassTarget pt;
    pt.color = slot.color.handle;
    pt.color_view = slot.color.view;
    pt.extent = {w, h};
    pt.color_after = rhi::ImageState::TransferSrc;
    // World.
    bool ok = true;
    draw_stats_ = {};
    const bool galactic = screen_ == Screen::Galactic && gview_.attached();
    const bool battle = (screen_ == Screen::Battle || screen_ == Screen::BattleLoad || (screen_ == Screen::Menu && demo_session())) && bview_.active();
    const int world_view = galactic ? 1 : battle ? 2 : 0;
    if (world_view != world_view_) gpu_.renderer.reset_taa_history();
    world_view_ = world_view;
    apply_aa_options(gview_.ropt, adv_video_, video_profile_.anti_alias);  // AA combo + slider (t11)
    bview_.ropt.taa = gview_.ropt.taa;
    bview_.ropt.taa_weight = gview_.ropt.taa_weight;
    bview_.ropt.ambient_occlusion = adv_video_.ambient_occlusion;  // land battles only (render skips space / galactic)
    // The original's detail options on top of each view's own frame options (copies: the views keep theirs).
    render::RecordOptions gro = gview_.ropt, bro = bview_.ropt;
    if (detail_options_) {
        apply_detail_options(gro, adv_video_);
        apply_detail_options(bro, adv_video_);
        // t11: the settings with their own renderer entry points (texture mips, particle slots).
        gpu_.renderer.set_texture_reduction(texture_reduction(adv_video_.texture));
        gview_.set_particle_detail(particle_detail(adv_video_.particle));
        bview_.set_particle_detail(particle_detail(adv_video_.particle));
    }
    if (galactic) {
        const ui::CameraParams cp = gview_.camera_params(w, h);
        gview_.prepare(render_alpha(), now_s_, selected_planet_, &cp);
        gview_.use_scene(gpu_);
        ok = gpu_.renderer.render(*f, cmd, pt, ui::to_view_matrices(cp, w, h), gview_.main, render::DrawMode::Lit, gro, &draw_stats_,
                                  nullptr, gview_.background);
    } else if (battle) {
        if (screen_ == Screen::Battle) bview_.set_selection_blobs(ui_.tactical_hud.selection_blobs());  // last HUD build's circles (one frame behind)
        else bview_.set_selection_blobs({});  // load screen / attract demo: no selection
        bview_.prepare(gpu_, render_alpha());
        bview_.use_scene(gpu_);
        ui::CameraParams cp = bview_.camera_params(w, h);
        if (shake_.active()) {  // refix t06: the TSW camera shake moves the drawn view only
            const tactical::Vec3 o = shake_.offset(cp.eye, cp.target);
            cp.eye = cp.eye + o;
            cp.target = cp.target + o;
        }
        const float env_scale = detail_options_ ? environment_fade_scale(adv_video_.environment) : 1.0f;  // Environment detail [M]
        gpu_.renderer.set_object_fade(cp.fade_begin * env_scale, cp.fade_end * env_scale);
        for (int i = 0; i < 4; ++i) pt.clear[i] = bview_.clear_color(i);
        ok = gpu_.renderer.render(*f, cmd, pt, ui::to_view_matrices(cp, w, h), bview_.main, render::DrawMode::Lit, bro, &draw_stats_,
                                  nullptr, bview_.sky);
    } else {
        render::RecordOptions ro;
        ro.shadows = false;
        ro.space = true;
        ro.post = false;
        gpu_.renderer.set_scene_hook(nullptr);
        ok = gpu_.renderer.render(*f, cmd, pt, ui::hud_ortho_view(ui_.gui.canvas(), ui::kHudCameraComponents), {}, render::DrawMode::Unlit, ro,
                                  &draw_stats_);
    }
    // HUD + dialogs.
    ui::HudDrawList hud_list, dialogs;
    const ui::HudCanvas* canvas = &ui_.gui.canvas();
    ui::ShellDrawer* drawer = nullptr;
    const ui::ShellSet* shells = nullptr;
    const ui::HudLayout* layout = nullptr;
    std::vector<ui::GalacticPlanetInfo> planets;
    if (galactic) {
        ui::GalacticHudFrame gf;
        build_galactic_hud(gf, planets);
        gf.advisor_page = advisor_loop_shown() ? static_cast<int32_t>(kAdvisorPage) : -1;  // FIXPASS_GALACTIC §4.2: Movie_galactic
        gf.advisor_alpha = advisor_movie_.alpha();
        ui_.galactic_hud.build(gf);
        // The shells the build wants shown (organiser, the two battle-prompt shells); re-applied to the layout on a change.
        bool shells_changed = false;
        for (const ui::GalacticShellVisibility& v : ui_.galactic_hud.shell_visibility())
            for (const ui::ShellSet::Shell& s : ui_.galactic_shells.shells())
                if (s.component == v.component && s.visible != v.visible) {
                    ui_.galactic_shells.set_visible(v.component, v.visible);
                    shells_changed = true;
                }
        if (shells_changed) ui_.galactic_shells.apply(ui_.galactic_hud.layout());
        hud_list = ui_.galactic_hud.draw_list();
        canvas = &ui_.galactic_hud.canvas();
        if (ui_.galactic_shells_ok) {
            drawer = &ui_.galactic_drawer;
            shells = &ui_.galactic_shells;
            layout = &ui_.galactic_hud.layout();
        }
    } else if (screen_ == Screen::Battle && battle) {
        build_tactical_hud();
        hud_list = ui_.tactical_hud.draw_list();
        canvas = &ui_.tactical_hud.canvas();
        if (ui_.tactical_shells_ok) {
            drawer = &ui_.tactical_drawer;
            shells = &ui_.tactical_shells;
            layout = &ui_.tactical_hud.layout();
        }
    }
    // HUD advisor loop: the galactic and battle HUDs draw it in Movie_galactic / Movie_tactical (FIXPASS_GALACTIC §4.2);
    // elsewhere 200 x 200 px, top-left [port].
    if (screen_ != Screen::Battle && !galactic && advisor_loop_shown()) {
        // STRATEGIC_GAPS §7.3: PrimAlpha (straight alpha) for a Movies.xml Alpha movie, PrimOpaque otherwise.
        ui::add_movie_quad(dialogs, 0, canvas->pixel_to_canvas(16.0f, 216.0f), canvas->pixel_to_canvas(216.0f, 16.0f), kAdvisorPage,
                           advisor_movie_.alpha());
    }
    if (full_movie_.has_frame() && (screen_ == Screen::Movie || full_movie_.playing())) {  // full-screen movie (2D scene layer 0)
        ui::HudQuad q;
        q.min = canvas->pixel_to_canvas(0.0f, static_cast<float>(h));
        q.max = canvas->pixel_to_canvas(static_cast<float>(w), 0.0f);
        q.color = {255, 255, 255, 255};
        q.page = kMoviePage;
        dialogs.add(0, ui::HudList::Quads, q);
    }
    // Panels (hero roster, black market, sabotage, corruption, hack) on top of the HUD list; their shells draw at the
    // HUD's ShellModels step.
    std::vector<ui::HudDrawList> panel_lists;
    if (galactic || (screen_ == Screen::Battle && battle)) build_panels(panel_lists);
    for (const ui::HudDrawList& pl : panel_lists) merge_lists(hud_list, pl);
    if (galactic || (screen_ == Screen::Battle && battle)) build_encyclopedia(hud_list, *canvas);
    // OI host: the attract demo world under the menu. GuiManager's backdrop paints an opaque black fill under the
    // MenuBack_overlay quad, which would hide the world: left out while the demo is drawn [port: until engine/ui draws the
    // overlay alone - stretched to the window, straight alpha - over a world, MAIN_MENU_BACKGROUND.md §6.4].
    // MAIN_MENU_BACKGROUND.md §5 / §6.4: the overlay itself (title wordmark top, LucasArts / Petroglyph logos bottom left;
    // 86 % transparent) stays over the demo for as long as the menu shows: one quad stretched to the window, straight
    // alpha, under the dialogs, without the backdrop's black fill.
    std::string backdrop;
    if (battle && screen_ == Screen::Menu) {
        backdrop = ui_.gui.backdrop();
        ui_.gui.set_backdrop({});
        if (const ui::GuiImage img = ui_.gui.image(backdrop); img.valid) {
            ui::HudQuad q;
            q.min = {-0.5f * canvas->width, -0.5f * canvas->height};
            q.max = {0.5f * canvas->width, 0.5f * canvas->height};
            q.uv0 = img.uv0;
            q.uv1 = img.uv1;
            q.color = {255, 255, 255, 255};
            q.page = img.page;
            dialogs.add(0, ui::HudList::Alpha, q);
        }
    }
    ui_.gui.build(dialogs, now_s_);
    if (!backdrop.empty()) ui_.gui.set_backdrop(backdrop);
    if (fade_state_ != 0) {  // first main-menu open: black over the menu until the demo ticked 4 frames, then a 2 s fade
        const sim::TacticalBattle* fb = demo_session() ? session_->battle() : nullptr;
        if (!demo_session()) {
            fade_state_ = 0;
        } else if (fade_state_ == 1 && fb && fb->frame() >= 4) {
            fade_state_ = 2;
            fade_t0_ = now_s_;
        }
        const double alpha = fade_state_ == 1 ? 1.0 : 1.0 - (now_s_ - fade_t0_) / 2.0;
        if (fade_state_ == 2 && alpha <= 0.0) fade_state_ = 0;
        if (fade_state_ != 0) {
            ui::HudQuad q;
            q.min = canvas->pixel_to_canvas(0.0f, static_cast<float>(h));
            q.max = canvas->pixel_to_canvas(static_cast<float>(w), 0.0f);
            q.color = {0, 0, 0, static_cast<uint8_t>(std::clamp(alpha, 0.0, 1.0) * 255.0 + 0.5)};
            dialogs.add(ui::kHudLayers - 1, ui::HudList::Alpha, q);
        }
    }
    // Playtest QA D1: the end-of-battle hold ends in a fade to black over its last 2 s [port]
    if (const sim::TacticalBattle* hb = session_ ? session_->battle() : nullptr; hb && hb->end_hold_active()) {
        const float fade = 1.0f - std::min(static_cast<float>(hb->end_hold_frames_left()) / 60.0f, 1.0f);
        ui::HudQuad q;
        q.min = canvas->pixel_to_canvas(0.0f, static_cast<float>(h));
        q.max = canvas->pixel_to_canvas(static_cast<float>(w), 0.0f);
        q.color = {0, 0, 0, static_cast<uint8_t>(fade * 255.0f + 0.5f)};
        dialogs.add(ui::kHudLayers - 1, ui::HudList::Alpha, q);
    }
    info_.build_overlay(dialogs);  // data-chart lines over IDC_DATA_CHART_IMAGE
    ui::HudDrawList cursor_list;
    update_cursor(cursor_list);
    if (drawer) drawer->make_resident(gpu_.renderer);
    if (panel_shells_ok_) panel_drawer_.make_resident(gpu_.renderer);
    // Radar event models (tactical HUD only): uploaded the first time a ping names them, drawn at the Models2 step.
    std::span<const ui::HudModelDraw> radar_models;
    if (ui_.radar_models_ok && screen_ == Screen::Battle && battle && !galactic) {
        radar_models = ui_.tactical_hud.radar_models();
        std::string merr;
        if (!radar_models.empty() && !ui_.radar_drawer.prepare(gpu_.renderer, radar_models, &merr))
            std::fprintf(stderr, "radar event models: %s\n", merr.c_str());
        ui_.radar_drawer.make_resident(gpu_.renderer);
    }
    ui::HudTarget ht;
    ht.color = slot.color.handle;
    ht.color_view = slot.color.view;
    ht.depth = slot.depth.handle;
    ht.depth_view = slot.depth.view;
    ht.extent = {w, h};
    ht.before = ht.after = rhi::ImageState::TransferSrc;
    render::DrawStats shell_stats{};
    ok = gpu_.hud.record(
             *f, cmd, ht, *canvas, hud_list, ui_.fonts.get(),
             [&](VkCommandBuffer c, const ui::HudFrameStep& step) {
                 if (step.kind == ui::HudStepKind::Models2 && !radar_models.empty()) {
                     render::DrawStats s3;
                     ui_.radar_drawer.draw(gpu_.renderer, *f, c, *canvas, radar_models, &s3);
                     shell_stats.pending_pipeline += s3.pending_pipeline;
                 }
                 if (step.kind == ui::HudStepKind::ShellModels && drawer && shells && layout) {
                     render::DrawStats s1;
                     drawer->draw(gpu_.renderer, *f, c, *canvas, *shells, *layout, step.first, &s1);
                     shell_stats.pending_pipeline += s1.pending_pipeline;
                 }
                 if (step.kind == ui::HudStepKind::ShellModels && panel_shells_ok_ && !panel_lists.empty()) {
                     for (ui::HudPanel* p : panels())
                         if (p->open()) {
                             render::DrawStats s2;
                             panel_drawer_.draw(gpu_.renderer, *f, c, p->canvas(), panel_shells_, p->layout(), step.first, &s2);
                         }
                 }
             },
             nullptr, &dialogs, cursor_ok_ ? &cursor_list : nullptr) &&
         ok;
    draw_stats_.pending_pipeline += shell_stats.pending_pipeline;
    // Options > Video > Gamma over the finished frame (world + HUD), 50 = identity (t11).
    if (detail_options_ && ok) {
        const float gamma = gamma_value(video_profile_.gamma);
        (void)gpu_.renderer.apply_gamma(*f, cmd, slot.color.handle, slot.color.view, slot.color.vk_format(), {w, h},
                                        rhi::ImageState::TransferSrc, rhi::ImageState::TransferSrc, gamma);  // a failure logs; the frame goes out ungammed
    }
    // Present: blit the frame into the swapchain image; optional readback for a PNG.
    const VkImage sw = sc_.image(image);
    rhi::cmd_barrier(fn, cmd, rhi::transition(sw, rhi::ImageState::Undefined, rhi::ImageState::TransferDst));
    VkImageBlit2 region{};
    region.sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2;
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[1] = {static_cast<int32_t>(w), static_cast<int32_t>(h), 1};
    region.dstOffsets[1] = {static_cast<int32_t>(sc_.extent().width), static_cast<int32_t>(sc_.extent().height), 1};
    VkBlitImageInfo2 bi{};
    bi.sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2;
    bi.srcImage = slot.color.handle;
    bi.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bi.dstImage = sw;
    bi.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bi.regionCount = 1;
    bi.pRegions = &region;
    bi.filter = VK_FILTER_LINEAR;
    fn.vkCmdBlitImage2(cmd, &bi);
    rhi::cmd_barrier(fn, cmd, rhi::transition(sw, rhi::ImageState::TransferDst, rhi::ImageState::Present));
    const bool readback = !shot_path_.empty();
    if (readback) {
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {w, h, 1};
        fn.vkCmdCopyImageToBuffer(cmd, slot.color.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, targets_.readback.handle, 1, &copy);
        const VkBufferMemoryBarrier2 hb = rhi::buffer_barrier(targets_.readback.handle, VK_PIPELINE_STAGE_2_COPY_BIT,
                                                              VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                                                              VK_ACCESS_2_HOST_READ_BIT);
        rhi::cmd_barrier(fn, cmd, {}, std::span<const VkBufferMemoryBarrier2>(&hb, 1));
    }
    VkSemaphoreSubmitInfo waits[2];
    uint32_t nw = 0;
    waits[nw++] = rhi::semaphore_info(f->acquire_semaphore(), 0, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    if (acq.wait_value) waits[nw++] = rhi::semaphore_info(gpu_.up.timeline(), acq.wait_value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    const VkSemaphoreSubmitInfo signal = rhi::semaphore_info(sc_.render_finished(image), 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    rhi::FrameSubmit fs;
    fs.waits = {waits, nw};
    fs.signals = {&signal, 1};
    if (!gpu_.ring.end_frame(*f, fs)) return false;
    const rhi::SwapchainStatus ps = sc_.present(image);
    if (ps == rhi::SwapchainStatus::OutOfDate || ps == rhi::SwapchainStatus::Suboptimal || as == rhi::SwapchainStatus::Suboptimal) {
        gpu_.ring.wait_idle();
        sc_.recreate(win_.width(), win_.height());
    } else if (ps == rhi::SwapchainStatus::Error) {
        return false;
    }
    if (readback) {
        gpu_.ring.wait_idle();
        gpu_.alloc.invalidate(targets_.readback.alloc);
        const auto* px = static_cast<const uint8_t*>(targets_.readback.mapped());
        std::vector<uint8_t> rgba(px, px + static_cast<size_t>(w) * h * 4);
        for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
        const bool wrote = render::write_png_rgba(shot_path_, w, h, rgba);
        std::printf("%s %s (%s: %u draws, %u instances)\n", wrote ? "wrote" : "FAILED", shot_path_.c_str(), screen_name(screen_), draw_stats_.draws,
                    draw_stats_.instances);
        if (!wrote) smoke_ok_ = false;
        shot_path_.clear();
        shot_done_ = true;
    }
    return ok;
}

// ------------------------------------------------------------------------------------------------ frame
void App::frame() {
    // Scripted runs are frame-driven (30 frames per presentation second): camera smoothing, double-tap windows and
    // pop-up delays then advance identically on every machine, so the recorded command stream is machine independent.
    const double now = script_.empty() ? ms_since(t0_) / 1000.0 : static_cast<double>(frames_) / 30.0;
    dt_ = static_cast<float>(std::clamp(now - now_s_, 0.0, 0.1));
    now_s_ = now;
    shake_.update(dt_);  // refix t06 (presentation)
    if (win_.take_resized() && win_.width() > 0 && win_.height() > 0) {
        gpu_.ring.wait_idle();
        sc_.recreate(win_.width(), win_.height());
        targets_.destroy(gpu_.alloc);
        targets_.create(gpu_.alloc, gpu_.ring.frames_in_flight(), win_.width(), win_.height());
        ui_.resize(win_.width(), win_.height());
    }
    // 1. Input (script frames replace the window in smoke runs).
    ui::InputFrame raw;
    if (!script_.empty()) {
        raw = idle_frame();
        if (script_step_ < script_.size()) {
            if (hold_release_) hold_sim_ = hold_release_ = false;
            ScriptStep& s = script_[script_step_];
            // Story dialogs fire whenever the plots say so: outside the story steps the script closes one after 30 frames.
            if (ui_.gui.find(ui::dlg::kStory) && s.name.find("story") == std::string::npos) {
                if (++story_dialog_frames_ > 30) {
                    ui_.gui.close(ui::dlg::kStory);
                    std::printf("script: story dialog closed\n");
                }
            } else {
                story_dialog_frames_ = 0;
            }
            if (s.run(*this, raw)) {
                std::printf("script: %s done (frame %" PRIu64 ", %s, tick %" PRIu64 ")\n", s.name.c_str(), frames_, screen_name(screen_),
                            session_ ? session_->host().frame() : 0);
                ++script_step_;
                step_frames_ = 0;
            } else if (++step_frames_ > s.timeout_frames) {
                std::fprintf(stderr, "script: step '%s' timed out (%s); dialogs:", s.name.c_str(), screen_name(screen_));
                for (const std::string& n : ui_.gui.open_names()) std::fprintf(stderr, " %s", n.c_str());
                std::fprintf(stderr, "\n");
                script_failed_ = true;
                screen_ = Screen::Quit;
                return;
            }
        }
        (void)ui::capture_input(win_, now_s_, dt_);  // consume the window's edges
    } else {
        raw = ui::capture_input(win_, now_s_, dt_);
    }
    std::u32string typed = printable(win_.take_text());
    // OI host: attract demo (idle clock, idle pause, map cycle); input reaches the menus as usual.
    attract_step(raw);
    // 2. Routing: overlay -> dialogs -> HUD -> world.
    ui::RouteHooks hooks;
    hooks.dialog_hit = [this](int32_t x, int32_t y) { return ui_.gui.hit(x, y); };
    hooks.dialog_focus = [this] { return ui_.gui.has_focus(); };
    // A running targeted-ability pick (not under a dialog or the pause menu) takes the presses before dialogs and the HUD.
    hooks.targeting_active = [this] { return screen_ == Screen::Battle && targeting_.active() && !paused_ && !ui_.gui.has_focus(); };
    // HUD hit test: open panels first, then the galactic / tactical HUD (hud_panels.hpp host wiring).
    auto panel_hit = [this](int32_t x, int32_t y) {
        for (ui::HudPanel* p : panels())
            if (p->open())
                if (const int32_t c = p->hit_test(x, y); c >= 0) return c;
        return -1;
    };
    if (screen_ == Screen::Galactic)
        hooks.hud_hit = [this, panel_hit](int32_t x, int32_t y) {
            const int32_t c = panel_hit(x, y);
            return c >= 0 ? c : ui_.galactic_hud.hit_test(x, y);
        };
    if (screen_ == Screen::Galactic)  // right presses on the planet fleet / land-forces buttons reach the planet picking
        hooks.hud_takes = [this, panel_hit](int32_t c, ui::Button b) {
            if (panel_hit(input_.mouse_x(), input_.mouse_y()) >= 0) return true;  // an open panel's component
            return ui_.galactic_hud.takes_button(c, b);
        };
    else if (screen_ == Screen::Battle)
        hooks.hud_hit = [this, panel_hit](int32_t x, int32_t y) {
            const int32_t c = panel_hit(x, y);
            return c >= 0 ? c : ui_.tactical_hud.hit_test(x, y);
        };
    router_.route(raw, hooks, routed_);
    target_input_.update(raw);
    gui_events_.clear();
    ui_.gui.update(raw, routed_, now_s_, gui_events_, typed);
    std::vector<ui::MenuAction> acts;
    ui_.front.update(gui_events_, raw, acts);
    handle_menu_actions(acts);
    {  // in-game screens: battle end, retreat, planet info, buy units, story, holocron (P4)
        std::vector<ui::ScreenAction> sacts;
        std::vector<commands::EventData> scmds;
        screens_.update(gui_events_, sacts, scmds);
        info_.update(gui_events_, sacts);
        handle_screen_actions(sacts);
        for (commands::EventData& e : scmds) submit(std::move(e));  // e.g. the confirmed Retreat
    }
    if (ui_.load.is_open()) {
        std::vector<ui::LoadScreenEvent> lev;
        ui_.load.update(gui_events_, lev);
        for (const ui::LoadScreenEvent& e : lev)
            if (e.kind == ui::LoadScreenEvent::Kind::Begin && screen_ == Screen::BattleLoad) {
                screen_ = Screen::Battle;
                audio_.music(MusicScene::Battle, local_faction(), bview_.space());
            }
    }
    input_.update(routed_.world);
    if (screen_ == Screen::Movie) movie_step();
    if (session_ && !paused_ && (screen_ == Screen::Galactic || screen_ == Screen::Battle) && routed_.keys_to_world &&
        input_.key_pressed(ui::vk::kEscape) && script_.empty() && !(screen_ == Screen::Battle && targeting_.active())) {
        paused_ = true;  // (Esc during a targeted-ability pick only cancels the pick: battle_input)
        ui_.front.show_pause_menu();
    }
    // MAIN_LOOP.md §1.5: the pause menu and the full-screen dialogs it opens (options and their pages, save / load, quit
    // confirm) set the tactical pause in single player - no ticks while any of them is open, whatever opened it.
    if (session_ && !demo_session() && (screen_ == Screen::Galactic || screen_ == Screen::Battle) && menu_dialog_open()) paused_ = true;
    if (paused_ && !menu_dialog_open()) paused_ = false;
    if (paused_ || ui_.gui.has_focus()) targeting_.cancel();  // a pick does not outlive the pause menu / a dialog
    if (session_ && !paused_) {
        if (screen_ == Screen::Galactic) galactic_input();
        else if (screen_ == Screen::Battle) battle_input();
    }
    // The space reinforcement grid only while a drop point is being picked (battle_input does not run while paused or
    // outside the battle screen, so the flag is kept here too).
    if (bview_.active()) bview_.set_space_reinforcement_overlay(screen_ == Screen::Battle && bview_.space() && reinforce_unit_ != 0);
    // 3. Sim, then the story's presentation ops (movies, audio) of the ticks just run; Lua movies advance.
    if (battle_view_pending_ && load_intro_frames_ == 0) start_battle_view();  // QA t11: the load screen is already up
    step_sim();
    if (screen_ == Screen::Menu && demo_session()) {  // OI host: the attract demo's camera; a finished demo battle restarts
        if (session_->skirmish_over() && !start_attract_map()) stop_attract();
        else demo_camera();
    }
    lua_presentation();
    story_reward_step();  // L1/S4 wave 1: START_MOVIE / MULTIMEDIA reward media
    if (screen_ == Screen::Galactic && session_) galactic_screens();
    if (screen_ != Screen::Movie) {
        if (full_movie_.playing() && !full_movie_.update(dt_, gpu_.hud, kMoviePage)) {
            story_ui_.movie_done(full_movie_.name());  // L1/S4 wave 1: STORY_MOVIE_DONE (an in-game full-screen movie ended)
            full_movie_.stop(audio_);
        }
        if (advisor_movie_.playing()) {
            const uint16_t page = story_movie_ && story_page_ >= 0 ? static_cast<uint16_t>(story_page_) : kAdvisorPage;
            advisor_movie_.update(dt_, gpu_.hud, page);
            if (page != kAdvisorPage && advisor_movie_.has_frame()) ui_.pages.set_loaded(page, advisor_movie_.width(), advisor_movie_.height());
        }
    }
    story_ui_step();  // L1/S4 wave 1: this frame's UI notifications for the story
    // 4. Render, 5. audio (listener from the camera).
    if (screen_ == Screen::Quit) return;
    if (!render_frame()) {
        std::fprintf(stderr, "render failed\n");
        smoke_ok_ = false;
        screen_ = Screen::Quit;
        return;
    }
    end_load_intro_frame();
    const bool cinematic = presentation_cinematic();  // OI host: Lua cinematic camera -> listener rules
    if (screen_ == Screen::Galactic) {
        const ui::CameraParams cp = gview_.camera_params(targets_.width, targets_.height);
        audio_.set_listener({cp.eye.x, cp.eye.y, cp.eye.z}, {cp.target.x, cp.target.y, cp.target.z}, cinematic);
    } else if ((screen_ == Screen::Battle || (screen_ == Screen::Menu && demo_session())) && bview_.active()) {
        const ui::CameraParams cp = bview_.camera_params(targets_.width, targets_.height);
        audio_.set_listener({cp.eye.x, cp.eye.y, cp.eye.z}, {cp.target.x, cp.target.y, cp.target.z}, cinematic);
    }
    audio_flags();  // OI host: modal / GUI-only / attract
    audio_.update(dt_);
    ++frames_;
    if (frames_ % 60 == 0 && !opt_.hidden) {
        char title[256];
        std::snprintf(title, sizeof title, "Alamo - %s - tick %" PRIu64 ", %u draws", screen_name(screen_), session_ ? session_->host().frame() : 0,
                      draw_stats_.draws);
        win_.set_title(title);
    }
}

// ------------------------------------------------------------------------------------------------ presentation on/off check
bool App::crc_check() {
    if (!session_) {
        std::fprintf(stderr, "crc check: no session\n");
        return false;
    }
    const CrcLog& on = session_->host().crc_log();
    const uint64_t ticks = session_->ticks();
    CrcLog off;
    if (session_->loaded()) {  // N1 / t09: the same save loaded again, then the stream recorded after the load
        jobs::JobSystem js(jobs::JobSystemConfig{opt_.workers});
        std::string err;
        std::unique_ptr<GameSession> replay = load_game(data_, js, loaded_save_, &err, &recording_);
        if (!replay) {
            std::fprintf(stderr, "replay: %s\n", err.c_str());
            return false;
        }
        for (uint64_t i = 0; i < ticks; ++i) replay->tick();
        off = replay->host().crc_log();
    } else {
        jobs::JobSystem js(jobs::JobSystemConfig{opt_.workers});
        GameSession replay(data_, js, session_->spec());
        std::vector<std::string> diag;
        if (!replay.start(&diag, &recording_, nullptr)) {
            for (const std::string& d : diag) std::fprintf(stderr, "replay: %s\n", d.c_str());
            return false;
        }
        for (uint64_t i = 0; i < ticks; ++i) replay.tick();
        off = replay.host().crc_log();
    }
    if (!opt_.png_dir.empty()) {  // the run's CRC log and command stream, for cross-build comparisons
        std::string e;
        const std::string base = opt_.png_dir + "/game_smoke_" + opt_.smoke;
        if (!on.save(base + ".crc", &e) || !recording_.save(base + ".rec", &e)) std::fprintf(stderr, "crc / recording save: %s\n", e.c_str());
    }
    const CrcDivergence d = first_divergence(on, off);
    std::printf("presentation on vs off (replay of %zu recorded batches): %zu vs %zu records, final %016" PRIx64 " vs %016" PRIx64 ": %s\n",
                recording_.batches.size(), on.records.size(), off.records.size(), final_crc(on), final_crc(off), d.message.c_str());
    return !d.diverged && on.records.size() == off.records.size() && !on.records.empty();
}

}  // namespace alamo_game_app
