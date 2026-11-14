// alamo_game (P host): the playable game exe. Per frame (MAIN_LOOP.md order): pump the window, capture input (WM_CHAR
// text included), route it (dialogs -> HUD -> world), menus / load screens / HUD / camera / click orders, at most the
// paced sim tick(s), snapshots of the finished ticks, render (world + HUD + dialogs), audio update.
//
//   main menu -> Single Player -> Custom Campaign  -> galactic map -> a human conflict -> load screen -> tactical battle
//                                                      on the planet's map -> back to the galactic map
//             -> Single Player -> Skirmish         -> load screen -> space battle on the chosen map -> main menu
//
// Usage: alamo_game [--game DIR] [--width W --height H] [--hidden] [--audio device|null|off] [--seed S] [--workers N]
//                   [--priority above|normal]   process priority class; default above (normal for --smoke)
//                   [--frames N] [--battle-max-frames N] [--cache-dir DIR] [--mailbox|--immediate] [MODPATH=..]
//                   [--frame-overlay] [--profile [--profile-out FILE] [--profile-budget MS]]   OPT1 frame-time overlay
//                   (F11 toggles it) / tick profiler + per-phase frame report at exit (frame_overlay.hpp)
//        alamo_game --smoke skirmish|campaign|land|saveload --png-dir DIR [...]   scripted input through the real menus
//                   (tests; saveload: the save / load menus, app_script_saveload.cpp);
//                   ends with the presentation-on/off check (the run's per-tick CRC log == a fresh presentation-free
//                   replay of its recorded command stream) and 0 validation messages. Exit 0 "skipped" without data.
//        alamo_game --export-save SAVE.alsv --out FILE.json [--game DIR] [MODPATH=..]   a save's galactic state as JSON for
//                   tools (save_export.hpp): no window, Vulkan or audio; exit 0, or 1 + a message on stderr.
// Game data: --game DIR or ALAMO_FOC_DATA (the FoC "corruption" folder).
#include "game_app.hpp"
#include "alamo/vfs/steam_workshop.hpp"

#include "alamo/game/save_export.hpp"
#include "alamo/render/galactic/galactic_snapshot.hpp"

namespace alamo_game_app {

// Launcher support: --export-save (save_export.hpp). Loads the game data the save needs (the same --game / MODPATH= as the
// game it was saved from), loads the save headless and writes the JSON; faction colours from Factions.xml.
static int export_save_main(const Options& opt, const std::string& save, const std::string& out) {
    const std::string root = opt.game.empty() ? env_or_empty("ALAMO_FOC_DATA") : opt.game;
    if (root.empty() || out.empty()) {
        std::fprintf(stderr, "--export-save needs --out FILE and --game <corruption dir> (or ALAMO_FOC_DATA)\n");
        return 2;
    }
    GameData data;
    std::string err;
    if (!load_game_data(root, opt.game_args, data, &err)) {
        std::fprintf(stderr, "game data: %s\n", err.c_str());
        return 1;
    }
    std::vector<ExportColor> colors;
    for (const render::FactionColor& f : render::load_faction_colors(*data.files)) {
        if (!f.has_color) continue;
        const auto byte = [](float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
        colors.push_back({f.name, byte(f.color.x), byte(f.color.y), byte(f.color.z)});
    }
    jobs::JobSystem js(jobs::JobSystemConfig{opt.workers});
    if (!export_save(data, js, save, out, colors, &err)) {
        std::fprintf(stderr, "export: %s\n", err.c_str());
        return 1;
    }
    std::printf("exported %s\n", out.c_str());
    return 0;
}

// ------------------------------------------------------------------------------------------------ init
bool App::init() {
    const std::string root = opt_.game.empty() ? env_or_empty("ALAMO_FOC_DATA") : opt_.game;
    if (root.empty()) {
        std::fprintf(stderr, "alamo_game needs --game <corruption dir> or ALAMO_FOC_DATA\n");
        return false;
    }
    game_root_ = root;
    const auto t0 = Clock::now();
    std::string err;
    if (!load_game_data(root, opt_.game_args, data_, &err)) {
        std::fprintf(stderr, "game data: %s\n", err.c_str());
        return false;
    }
    std::printf("data: %zu object types, %zu factions, %zu campaigns in %.0f ms\n", data_.types.size(), data_.rules.factions.size(),
                data_.db.campaigns().size(), ms_since(t0));
    js_ = std::make_unique<jobs::JobSystem>(jobs::JobSystemConfig{opt_.workers});
    if (bench_active()) bench_apply_window();  // BM2a: the preset's window size, present mode and frame cap
    // [name-ok: the window title (the game's name), not a faction]
    if (!win_.create({"Alamo - Empire at War", opt_.width, opt_.height, true, !opt_.hidden})) return false;
    const bool strict = render::setup_validation_env(ALAMO_RENDER_SOURCE_DIR);
    rhi::ContextDesc cd;
    cd.app_name = "alamo_game";
    cd.presentation = true;
    cd.upscaler_features = true;  // [port] FSR upscaling (the Anti-aliasing list's FSR rows): the ML upscaler's device features
    cd.validation = strict ? rhi::Validation::Required : rhi::Validation::IfAvailable;
    if (!ctx_.init(cd)) {
        std::fprintf(stderr, "no suitable Vulkan 1.3 device%s\n", strict ? " (or the validation layer failed to load)" : "");
        return false;
    }
    std::printf("device %s, validation %s%s\n", ctx_.caps().name.c_str(), ctx_.validation_enabled() ? "on" : "off", strict ? " (required)" : "");
    // [port] the saved video options: present mode, frame cap, advanced video (the resolution below).
    // BM2a: a benchmark run reads no video options file (its preset sets everything, bench_apply_video).
    video_profile_path_ = bench_active()                 ? std::string()
                          : !opt_.video_options.empty() ? opt_.video_options
                          : live_input()                ? default_video_profile_path()
                                                        : std::string();
    have_video_profile_ = load_video_profile(video_profile_path_, video_profile_);
    detail_options_ = live_input() || have_video_profile_;
    if (have_video_profile_) {
        if (!opt_.mode_set)
            opt_.mode = !video_profile_.vsync ? rhi::PresentMode::Immediate
                        : video_profile_.vsync_mode == ui::VsyncMode::Mailbox ? rhi::PresentMode::Mailbox : rhi::PresentMode::Fifo;
        if (!opt_.frame_cap_set) opt_.frame_cap = video_profile_.frame_cap;
        window_mode_ = video_profile_.window_mode;
        sync_detail(video_profile_);  // t43
        adv_video_ = video_profile_.advanced;
        std::printf("video options: %s\n", video_profile_path_.c_str());
    }
    if (bench_active()) bench_apply_video();
    rhi::SwapchainDesc sd;
    sd.hinstance = win_.native_instance();
    sd.hwnd = win_.native_handle();
    sd.width = win_.width();
    sd.height = win_.height();
    sd.mode = opt_.mode;
    sd.exclusive_fullscreen = window_mode_ == ui::WindowMode::Fullscreen;  // [port: Gabriel 2026-11-13]
    if (!sc_.init(ctx_, sd)) return false;
    vsync_mode_ = opt_.mode == rhi::PresentMode::Mailbox ? ui::VsyncMode::Mailbox : ui::VsyncMode::Fifo;
    if (have_video_profile_ && !opt_.mode_set) vsync_mode_ = video_profile_.vsync_mode;  // kept while v-sync is off
    update_display_rate(true);
    std::printf("present modes: mailbox %s, immediate %s\n", sc_.supports(rhi::PresentMode::Mailbox) ? "yes" : "no",
                sc_.supports(rhi::PresentMode::Immediate) ? "yes" : "no");
    if (!gpu_.init(ctx_, data_, *js_, opt_.cache_dir, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return false;
    }
    if (!targets_.create(gpu_.alloc, gpu_.ring.frames_in_flight(), win_.width(), win_.height())) return false;
    gpu_.renderer.set_anisotropy(static_cast<uint32_t>(adv_video_.anisotropy));
    // Menus: skirmish maps from the data, audio volumes from the host.
    ui::MenuProviders mp;
    // Skirmish maps (UI.md section 9, p4ui t06): the catalog of Data\Art\Maps, Data\CustomMaps and the mod folders' maps, read once;
    // the front end filters the rows of the Official / Custom tab. Without a single catalog row the name lists stand in:
    // space maps, then land maps, each sorted by name (game_data.hpp; modc t19: no map moved to the front). cu9 t05: kept,
    // because list_maps sees every VFS layer while the catalog's official list sees only the base and MEG layers, so a data
    // set whose MP maps have no decodable header there still gets rows.
    skirmish_catalog_ = std::make_shared<const SkirmishMapCatalog>(build_skirmish_map_catalog(data_));
    std::vector<ui::SkirmishMapInfo> map_rows;
    for (const bool custom_tab : {false, true}) {
        for (const SkirmishMapEntry& e : skirmish_map_rows(*skirmish_catalog_, custom_tab)) {
            ui::SkirmishMapInfo i;
            i.map = e.map;
            i.max_players = e.players;
            i.players = e.players;
            i.name_key = e.name_key;
            i.custom = custom_tab;
            i.space = e.kind == 2;
            // ccpgt t02 (UI.md 9.4): a land map whose upper-cased tag contains CONTROL is listed under Capture Control Points.
            i.control = e.kind == 1 && e.mode_tag.find("CONTROL") != std::string::npos;
            map_rows.push_back(std::move(i));
        }
    }
    if (map_rows.empty()) {
        std::vector<std::string> names = skirmish_space_maps(data_);
        const size_t space_count = names.size();
        for (const std::string& m : skirmish_land_maps(data_)) names.push_back(m);
        for (size_t k = 0; k < names.size(); ++k) {
            ui::SkirmishMapInfo i;
            i.map = names[k];
            i.space = k < space_count;
            map_rows.push_back(std::move(i));
        }
    }
    std::printf("skirmish maps: %zu official, %zu custom rows (%zu + %zu catalog entries)\n",
                static_cast<size_t>(std::count_if(map_rows.begin(), map_rows.end(), [](const ui::SkirmishMapInfo& m) { return !m.custom; })),
                static_cast<size_t>(std::count_if(map_rows.begin(), map_rows.end(), [](const ui::SkirmishMapInfo& m) { return m.custom; })),
                skirmish_catalog_->official.size(), skirmish_catalog_->custom_scan.size());
    // p4uia t01: the GameConstants MP_Color_* palette (byte RGB) for the staging colour combos.
    mp.player_colors = [this] {
        const render::MpPalette pal = gpu_.resolver ? gpu_.resolver->palette() : render::default_mp_palette();
        const auto byte = [](float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
        std::vector<ui::Rgba8> out;
        for (const materials::float4& c : pal.color) out.push_back({byte(c.x), byte(c.y), byte(c.z), 255});
        return out;
    };
    // p4uim t03 (UI.md 9.6): <stem>.tga, else the map's embedded image, else Generic_Space / Generic_Land.tga; plus the start
    // positions. Cached per map (a selection change asks again; the embedded image is decoded once).
    auto previews = std::make_shared<std::map<std::string, ui::SkirmishPreview>>();
    mp.skirmish_preview = [this, previews](const ui::SkirmishMapInfo& m) {
        if (const auto it = previews->find(m.map); it != previews->end()) return it->second;
        ui::SkirmishPreview pv;
        const size_t slash = m.map.find_last_of(std::string_view("\\/"));
        std::string stem = m.map.substr(slash == std::string::npos ? 0 : slash + 1);
        if (stem.size() > 4 && ascii_iequals(stem.substr(stem.size() - 4), ".ted")) stem.resize(stem.size() - 4);
        SkirmishMapPreview file;
        if (data_.files) file = read_skirmish_map_preview(*data_.files, m.map, m.space);
        if (data_.files)
            for (const assets::TextureCandidate& c : assets::texture_lookup(stem + ".tga"))
                if (data_.files->exists(c.path)) {
                    pv.texture = stem + ".tga";
                    break;
                }
        if (pv.texture.empty() && !file.embedded_image.empty()) {
            auto t = std::make_shared<assets::TextureAsset>();
            if (assets::decode_tga(file.embedded_image, *t)) {
                pv.texture = "temp_map_preview_texture:" + stem;
                ui_.memory_textures[pv.texture] = std::move(t);
            }
        }
        if (pv.texture.empty()) pv.texture = m.space ? "Generic_Space.tga" : "Generic_Land.tga";
        for (const auto& [x, y] : file.starts) pv.starts.push_back({x, y});
        previews->emplace(m.map, pv);
        return pv;
    };
    mp.camera_settings = [this] { return camera_; };
    mp.gameplay = [this] { return gameplay_; };
    mp.audio = [this] { return audio_opts_; };  // ui1 t02
    {  // ui1 t06: the advanced game options' defaults (GameConstants MP_Default_*) and the credits range
        const data::GameConstants& gc = data_.db.constants();
        ui::GameOptionsSetup& o = mp.game_option_defaults;
        o.credits = gc.get_int("MP_Default_Credits", o.credits);
        o.start_tech = gc.get_int("MP_Default_Start_Tech_Level", o.start_tech);
        o.max_tech = gc.get_int("MP_Default_Max_Tech_Level", o.max_tech);
        o.auto_resolve = std::clamp(gc.get_int("MP_Default_Allow_Auto_Resolve", o.auto_resolve), 0, 3);
        o.random_events = gc.get_bool("MP_Default_Allow_Random_Events", o.random_events);
        o.min_credits = gc.get_int("Min_Skirmish_Credits", o.min_credits);
        o.max_credits = std::max(o.min_credits, gc.get_int("Max_Skirmish_Credits", o.max_credits));
    }
    // Video options: the primary display's modes (the current window size selected, else the largest mode under it); the
    // display mode list picks fullscreen (exclusive when the driver offers it) / borderless / windowed; v-sync = a FIFO or Mailbox present mode (the port's
    // V-sync mode combo picks which; off = Immediate), plus the port's frame rate cap and display line.
    display_modes_ = display_modes(win_.native_handle());
    if (live_input()) ui_scale_ = video_profile_.ui_scale;  // 100 % without a profile
    if (have_video_profile_ && live_input() && ((video_profile_.mode.width && video_profile_.mode.height) || !windowed())) {  // the saved mode
        DisplayMode m = video_profile_.mode;  // full screen without a stored mode: the display's largest mode
        if ((!m.width || !m.height) && !display_modes_.empty()) m = display_modes_.back();
        apply_display(m);
        update_display_rate(true);
    }
    mp.video = [this] {
        ui::VideoSettings v;
        for (const DisplayMode& m : display_modes_) v.resolutions.push_back(ui::utf8_to_utf32(display_mode_label(m)));
        v.resolution = display_mode_index(display_modes_, win_.width(), win_.height(), refresh_hz_);
        v.detail = video_profile_.detail;  // the original's sliders: saved with the profile, applied each frame (t11)
        v.anti_alias = video_profile_.anti_alias;
        v.upscaler = video_profile_.upscaler;  // [port: Gabriel 2026-11-13] the Upscaler / Upscaler Quality lists
        v.upscale_quality = video_profile_.upscale_quality;
        v.gamma = video_profile_.gamma;
        v.window_mode = window_mode_;
        v.ui_scale = ui_scale_ > 0 ? ui_scale_ : video_profile_.ui_scale;
        v.vsync = opt_.mode != rhi::PresentMode::Immediate;
        v.vsync_mode = vsync_mode_;
        v.mailbox_available = sc_.supports(rhi::PresentMode::Mailbox);
        v.frame_cap = opt_.frame_cap;
        const VkPresentModeKHR pm = sc_.present_mode();
        const char* used = pm == VK_PRESENT_MODE_MAILBOX_KHR ? "v-sync (Mailbox)" : pm == VK_PRESENT_MODE_IMMEDIATE_KHR ? "no v-sync" : "v-sync (FIFO)";
        v.display_info = ui::utf8_to_utf32((refresh_hz_ ? "Display " + std::to_string(refresh_hz_) + " Hz, " : std::string("Display ")) +
                                           "presenting with " + used + (sc_.exclusive() ? ", exclusive full screen" : "") +
                                           (limiter_.fps() ? ", capped at " + std::to_string(limiter_.fps()) + " fps" : std::string()));
        v.device = ui::utf8_to_utf32(ctx_.caps().name);
        return v;
    };
    mp.advanced_video = [this] { return adv_video_; };
    // N1 / t09: the Save / Load dialogs' list (newest first), the Save dialog's blocker and the overwrite question.
    saves_dir_ = !opt_.saves_dir.empty()                          ? opt_.saves_dir
                 : !opt_.smoke.empty() && !opt_.png_dir.empty() ? opt_.png_dir + "/saves"
                                                                : default_saves_dir();
    std::printf("saves: %s\n", saves_dir_.empty() ? "(no folder: %LOCALAPPDATA% unset)" : saves_dir_.c_str());
    mp.saves = [this] {
        std::vector<ui::SaveGameInfo> v;
        for (const SaveEntry& e : list_saves(saves_dir_)) {
            ui::SaveGameInfo i;
            i.name = e.name;
            if (!e.readable) {  // top level, so the player can still delete it
                i.label = ui::utf8_to_utf32(e.name + "  -  (unreadable: " + e.error + ")");
                v.push_back(std::move(i));
                continue;
            }
            // [port: Gabriel 2026-11-13] the tree (save_games.hpp TREE): campaign / skirmish map > faction > playthrough.
            const SaveMeta& m = e.meta;
            const bool skirmish = m.kind == "skirmish";
            const galactic::FactionRules* fr = nullptr;
            for (const galactic::FactionRules& f : data_.rules.factions)
                if (ascii_iequals(f.name, m.faction)) fr = &f;
            i.icon = !m.faction_icon.empty() ? m.faction_icon : fr ? fr->icon : std::string();
            i.label = ui::utf8_to_utf32(save_row_label(m));
            const std::string what = m.title.empty() ? m.campaign : m.title;
            i.groups.push_back({m.kind + ":" + m.campaign, ui::utf8_to_utf32(skirmish ? "Skirmish: " + what : what)});
            std::u32string faction = ui::utf8_to_utf32(m.faction.empty() ? std::string("(no faction)") : m.faction);
            if (fr && !fr->text_id.empty() && ui_.gui.text_db() && ui_.gui.text_db()->find(fr->text_id))
                faction = ui_.gui.caption_text(fr->text_id);
            i.groups.push_back({m.faction, std::move(faction), i.icon});
            if (!skirmish) {
                const std::string started = format_local_time(m.playthrough_started);
                i.groups.push_back({m.playthrough, ui::utf8_to_utf32(m.playthrough.empty() ? std::string("Earlier saves")
                                                                     : started.empty()  ? std::string("Game")
                                                                                        : "Game started " + started)});
            }
            v.push_back(std::move(i));
        }
        return v;
    };
    mp.save_blocker = [this] {
        if (saves_dir_.empty()) return std::u32string(U"No saved games folder (%LOCALAPPDATA% is not set)");
        if (!session_ || demo_session()) return std::u32string(U"No game is running");
        return ui::utf8_to_utf32(session_->check_save());
    };
    mp.save_exists = [this](const std::string& name) { return game::save_exists(saves_dir_, name); };
    mp.skirmish_maps = [map_rows] { return map_rows; };
    if (!opt_.bench.active()) init_mod_picker(mp);  // modc t08: the Mods button (scripted runs open it, never Launch)
    // modc t20: the staging faction list from the data (Is_Playable, non-neutral, FactionFiles.xml order), not built-in names.
    for (const galactic::FactionRules& f : data_.rules.factions)
        if (f.is_playable && !f.is_neutral) mp.skirmish_factions.push_back({f.name, f.text_id});
    if (!ui_.init(gpu_, data_, std::move(mp), &err)) {
        std::fprintf(stderr, "UI: %s\n", err.c_str());
        return false;
    }
    resize_ui();
    // OI host: the hotkey table's defaults (W A S D movement, arrows / numpad yaw-pitch-roll, Pause) and the queue
    // modifier drive the camera / order bindings from the first frame (before: ui::Bindings' own defaults until the
    // controls dialog was accepted).
    sync_control_bindings(ui_.hotkeys, camera_, ui_.bindings);
    attract_.set_config(load_attract_config(data_.db.constants()));
    {  // P1 move acknowledgement constants (P1_DOUBLE_CLICK_MOVE.md §2.3 / §5)
        const data::GameConstants& gc = data_.db.constants();
        for (size_t i = 0; i < ack_fx_.size(); ++i)
            if (const auto v = gc.get(ui::ack_effect_constant(static_cast<ui::AckEffect>(i)))) ack_fx_[i] = std::string(*v);
        for (size_t i = 0; i < ack_radar_.size(); ++i)
            if (const auto v = gc.get(ui::ack_radar_constant(static_cast<ui::AckRadar>(i)))) ack_radar_[i] = std::string(*v);
        ack_scale_land_ = gc.get_float("GUI_Move_Acknowledge_Scale_Land", ack_scale_land_);
        ack_scale_space_ = gc.get_float("GUI_Move_Acknowledge_Scale_Space", ack_scale_space_);
        // refixf t05: the in-game cinematic camera (TSW_PLANET_DESTROY.md §11.3)
        cine_.enabled = gc.get_bool("In_Game_Cinematics", true);
        cine_.letterbox_height = gc.get_float("Camera_FX_Manager_Letterbox_Height", 0.085f);
        beacon_delay_s_ = gc.get_float("BeaconPlaceDelay", 5000.0f) / 1000.0f;  // ui3 t03: ms between two beacons
    }
    // The unsynced presentation generator (0x140a2ab18, seeded from the performance counter at boot); smoke runs fixed.
    cine_.rng.reseed(opt_.smoke.empty() && !bench_active() ? static_cast<uint64_t>(Clock::now().time_since_epoch().count()) : 0xf0f0f0f0u);
    // Live play: the user's Windows double-click time / rectangle (P1_DOUBLE_CLICK_MOVE.md §0); smoke runs keep the defaults.
    if (live_input()) input_ = ui::InputState(ui::system_input_config());
    if (!audio_.init(data_, opt_.audio, opt_.seed ^ 0xA0D10u, &err)) std::fprintf(stderr, "audio: %s (continuing without)\n", err.c_str());
    audio_.set_log_limit(kGameAudioLogLimit);  // [port] the newest few thousand events only (GameAudioHost::set_log_limit)
    ui_.gui.set_sfx_hook([this](std::string_view ev, int32_t) { audio_.ui(ev); });
    if (live_input()) ui_.gui.set_clipboard([] { return clipboard_text(); }, [](std::u32string_view t) { set_clipboard_text(t); });
    ui_.galactic_hud.set_sfx_hook([this](std::string_view ev, int32_t) { audio_.ui(ev); });
    ui_.tactical_hud.set_sfx_hook([this](std::string_view ev, int32_t) { audio_.ui(ev); });
    load_game_options();  // ui1 t02: audio, gameplay, camera and key bindings from the last run (live play)
    faction_colors_ = render::load_faction_colors(*data_.files);
    gview_.init(gpu_, data_, faction_colors_, render::load_route_line_style(*data_.files));
    apply_camera_tuning();
    // P4 screens / panels / cursor / radar / hotkeys.
    screens_.init(ui_.gui);
    info_.init(ui_.gui);
    {
        std::vector<uint8_t> xml;
        if (data_.files->read("Data\\XML\\MousePointers.xml", xml)) {
            const data::XmlDocument doc = data::XmlDocument::parse(xml, "MousePointers.xml");
            if (doc.ok() && cursor_table_.parse(doc.root()) > 0) {
                cursor_table_.resolve_frames([this](std::string_view n) {
                    if (ui_.atlas.find(n)) return true;
                    // ui ask 1: a silent existence check (the texture cache logged every missing *_01.TGA frame).
                    for (const std::string& c : assets::texture_candidates(n))
                        if (data_.files->exists(c)) return true;
                    return false;
                });
                cursor_.init(cursor_table_, &ui_.atlas, [this](std::string_view t) { return ui_.pages.resolve(t); });
                cursor_ok_ = true;
            }
        }
        std::string e;
        radar_settings_.load(*data_.files, &e);
        // HUD_GAPS §1.5: the hard-point reticle textures (GameConstants) and their stand-alone DDS pages. cu5 t03: the
        // <GameConstants> root GameData parsed (GameDatabase::constants().element), not a second read of the file.
        if (const data::XmlNode& gc = data_.db.constants().element) {
            reticles_.parse(gc);
            ency_cfg_ = ui::tooltip_config_from(gc);  // HUD_GAPS §2.2 delays / fade / min display
        }
        hud_images_ = [this](std::string_view t) { return ui_.pages.resolve(t); };
        if (const char* dbg = std::getenv("ALAMO_HUD_HIT_DEBUG")) {
            hud_hit_debug_ = dbg[0] != '\0' && dbg[0] != '0';
            int x = -1, y = -1;
            if (std::strchr(dbg, ',') && std::sscanf(dbg, "%d,%d", &x, &y) == 2) {
                hud_hit_px_ = x;
                hud_hit_py_ = y;
            }
        }
        ency_.init(ui_.components, ui_.atlas, *ui_.fonts, &ui_.text);
        std::vector<std::string> names;
        for (ui::HudPanel* p : panels()) {
            p->init(ui_.components, ui_.atlas, *ui_.fonts, &ui_.text);
            p->set_sfx_hook([this](std::string_view ev, int32_t) { audio_.ui(ev); });
            for (const std::string& n : p->shell_names()) names.push_back(n);
        }
        apply_tooltip_config();  // encyclopedia, both HUDs and the panels (after the panels' init)
        std::vector<std::string> missing;
        panel_shells_.load(ui_.components, [this](std::string_view n) { return gpu_.cache->model(n); }, names, &missing);
        const render::TextureSource tex = [this](std::string_view n, assets::TextureRole role) { return gpu_.cache->texture(n, role); };
        panel_shells_ok_ = !panel_shells_.empty() && panel_drawer_.upload(gpu_.renderer, panel_shells_, tex, &e);
        panel_drawer_.wait_uploads(gpu_.up);
        for (const ui::ShellSet::Shell& s : panel_shells_.shells()) panel_shells_.set_visible(s.component, false);
        for (ui::HudPanel* p : panels()) {
            p->set_ui_scale(static_cast<float>(ui_scale_) / 100.0f);
            p->resize(win_.width(), win_.height());
            panel_shells_.apply(p->layout());
        }
    }
    // OPT1: the tick profiler (wall clock; the sim only writes it) and the frame-time overlay.
    if (opt_.profile) {
        profile::start(65536);
        overlay_.set_log(true);
    }
    if (opt_.frame_overlay) overlay_.set_shown(true, opt_.profile);
    if (!opt_.smoke.empty()) make_script();
    if (bench_active()) return bench_start();  // BM2a: straight into the save, no intro or menus
    if (!opt_.movies.empty() && next_movie()) screen_ = Screen::Movie;
    else show_menu_after_intro();
    return true;
}

// The main menu after the intro movies (or at once without them).

// ------------------------------------------------------------------------------------------------ run
int App::run() {
    if (!init()) return 1;
    while (screen_ != Screen::Quit && win_.pump()) {
        frame();
        update_display_rate(false);
        limiter_.wait();  // [port: Gabriel] frame rate cap (live play only)
        if (opt_.frames >= 0 && static_cast<long>(frames_) >= opt_.frames) break;
        if (!script_.empty() && script_step_ >= script_.size()) break;
        if (bench_active() && bench_done()) break;
        if (script_.empty() && screen_ == Screen::Menu && !ui_.gui.top() && input_.key_pressed(ui::vk::kEscape)) break;
    }
    if (profile::enabled()) {
        profile::stop();  // the CRC check's replays below are not part of the run
        if (opt_.profile) {
            const std::string text = overlay_.report() + profile::format_report(profile::build_report(profile::snapshot(), opt_.profile_budget));
            std::printf("%s", text.c_str());
            if (!opt_.profile_out.empty()) {
                if (FILE* pf = std::fopen(opt_.profile_out.c_str(), "wb")) {
                    std::fwrite(text.data(), 1, text.size(), pf);
                    std::fclose(pf);
                } else {
                    std::fprintf(stderr, "cannot write %s\n", opt_.profile_out.c_str());
                }
            }
        }
    }
    int rc = bench_active() ? bench_finish() : 0;
    if (!script_.empty()) {
        const bool finished = script_step_ >= script_.size() && !script_failed_;
        std::printf("smoke %s: %zu / %zu steps, %" PRIu64 " frames, %" PRIu64 " battles, galactic AI orders %" PRIu64 "\n", opt_.smoke.c_str(),
                    script_step_, script_.size(), frames_, battles_seen_, session_ ? session_->galactic_ai_orders() : 0);
        if (session_)
            std::printf("session: %" PRIu64 " ticks, orders routed %" PRIu64 " (moves %" PRIu64 ", attacks %" PRIu64 ", rejected %" PRIu64
                        "), commands applied %" PRIu64 " rejected %" PRIu64 "; audio: %" PRIu64 " ui, %" PRIu64 " responses, %" PRIu64
                        " deaths, %" PRIu64 " hits, %" PRIu64 " fires, %" PRIu64 " mode scenes\n",
                        session_->ticks(), session_->order_stats().routed, session_->order_stats().moves, session_->order_stats().attacks,
                        session_->order_stats().rejected, session_->host().commands_applied(), session_->host().commands_rejected(), audio_.stats().ui,
                        audio_.stats().responses, audio_.stats().deaths, audio_.stats().hits, audio_.stats().fires, audio_.stats().scenes);
        std::printf("screens: %" PRIu64 " battle choices, %" PRIu64 " auto-resolve choices, %" PRIu64 " story offers, %" PRIu64
                    " encyclopedia frames, %" PRIu64 " retry boxes, %" PRIu64 " reinforcement drops, holocron %zu chart samples\n",
                    choices_, auto_resolves_, story_dialogs_, ency_shown_, retry_boxes_, reinforce_drops_, holo_.samples());
        std::printf("reveal: %" PRIu64 " planet-revealed sfx, %" PRIu64 " moves to unrevealed planets dropped\n", reveal_sfx_,
                    hidden_moves_dropped_);
        std::printf("build pads: %" PRIu64 " refused clicks (greyed pads)\n", build_refusals_);  // ui4 t02
        std::printf("announcer: %" PRIu64 " galactic announcements\n", announcements_);       // ui4 t08
        std::printf("hud feeds: %" PRIu64 " reticle frames, %" PRIu64 " reticle orders, %" PRIu64 " Lua GUI calls, %zu galactic heroes, "
                    "reticle table %zu, buy bar hidden %" PRIu64 " frames (builder behind the camera)\n",
                    reticle_frames_, reticle_orders_, gui_calls_applied_, galactic_heroes_.size(), reticles_.size(), buy_bar_behind_camera_);
        std::printf("move acks: %" PRIu64 " (%" PRIu64 " particles spawned, %" PRIu64 " normal acks killed by a double click)\n",
                    move_acks_, move_acks_drawn_, move_ack_kills_);
        // OI host: the camera / controls decisions hold on the tables the run used (scripts never rebind).
        for (const std::string& v : check_control_decisions(ui_.hotkeys, ui_.bindings)) {
            std::fprintf(stderr, "controls: %s\n", v.c_str());
            smoke_ok_ = false;
        }
        if (session_) {
            const PortEventStats& ps = session_->port_stats();
            const SpecialStats& ss = session_->special_stats();
            std::printf("P4 host: %" PRIu64 " auto-resolve results, %" PRIu64 " story cues (%" PRIu64 " speech); withdraw %" PRIu64 " (%" PRIu64
                        " accepted, %" PRIu64 " dropped), battle auto-resolve %" PRIu64 " (%" PRIu64 " refused)\n",
                        auto_results_, story_cues_, audio_.stats().speech, ps.withdraw, ps.withdraw_accepted, ps.withdraw_dropped,
                        ps.tactical_auto_resolve, ps.tactical_auto_resolve_refused);
            std::printf("P4 specials: %" PRIu64 " activations, %" PRIu64 " dialogs, %" PRIu64 " errors, %" PRIu64 " purchases, %" PRIu64
                        " sabotages, %" PRIu64 " hacks, %" PRIu64 " slices (%" PRIu64 " ok), %" PRIu64 " unlocks, %" PRIu64
                        " respawns scheduled / %" PRIu64 " done, %" PRIu64 " SFX, %" PRIu64 " messages\n",
                        activations_, ss.dialogs, ss.errors, ss.purchases, ss.sabotages, ss.hacks, ss.slices, ss.slice_successes,
                        ss.types_unlocked, ss.respawns_scheduled, ss.respawned, special_sfx_, special_msgs_);
            std::printf("OI host: %" PRIu64 " ticks under the cinematic rate lock, %" PRIu64 " fast-forward toggles, %" PRIu64
                        " attract demos, %" PRIu64 " audio flag changes\n",
                        session_->rate_lock_ticks(), fast_toggles_, attract_runs_, audio_.stats().flag_changes);
            std::printf("story UI: %" PRIu64 " raised, %" PRIu64 " sent, %" PRIu64 " applied, %" PRIu64 " dropped; reward media: %" PRIu64
                        " movies, %" PRIu64 " speeches\n",
                        story_ui_.raised_total(), story_ui_sent_, ps.story_ui, ps.story_ui_dropped, reward_movies_,
                        reward_speeches_);  // L1/S4 wave 1
        }
        for (const std::string& n : notes_) std::printf("note: %s\n", n.c_str());
        const bool crc = finished && crc_check();
        if (!finished || !crc || !smoke_ok_) rc = 1;
    }
    shutdown();
    const uint32_t ve = ctx_.validation_errors(), vw = ctx_.validation_warnings();
    std::printf("validation: %s, %u errors, %u warnings\n", ctx_.validation_enabled() ? "on" : "off", ve, vw);
    if (ve || vw) rc = 1;
    ctx_.shutdown();
    win_.destroy();
    if (!opt_.smoke.empty()) std::printf(rc == 0 ? "alamo_game smoke %s: OK\n" : "alamo_game smoke %s: FAILED\n", opt_.smoke.c_str());
    return rc;
}

}  // namespace alamo_game_app

using namespace alamo_game_app;

// BM3: `--bench-new-save FILE ...` / `--bench-maps`: loads the game data and works on it without a window or a renderer.
static int bench_new_main(const Options& opt) {
    if (const std::string why = check_bench_new_save(opt.bench_new); !why.empty()) {
        std::fprintf(stderr, "benchmark: %s\n", why.c_str());
        return 2;
    }
    const std::string root = opt.game.empty() ? env_or_empty("ALAMO_FOC_DATA") : opt.game;
    if (root.empty()) {
        std::printf("alamo_game --bench-new-save: ALAMO_FOC_DATA / --game not set: skipped\n");
        return 0;
    }
    GameData data;
    std::string err;
    if (!load_game_data(root, opt.game_args, data, &err)) {
        std::fprintf(stderr, "game data: %s\n", err.c_str());
        return 1;
    }
    if (opt.bench_new.maps) {
        for (const std::string& line : bench_map_lines(data)) std::printf("%s\n", line.c_str());
        return 0;
    }
    jobs::JobSystem js(jobs::JobSystemConfig{opt.workers});
    std::string summary;
    if (!make_bench_save(data, js, opt.bench_new, &summary, &err)) {
        std::fprintf(stderr, "bench-new-save: %s\n", err.c_str());
        return 1;
    }
    std::printf("bench-new-save: %s\n", summary.c_str());
    return 0;
}

// bm3 --bench-regen MANIFEST: loads the game data, makes the scenario's session again from its make line (benchmark_save.hpp) and turns the
// command line into `--bench-make MANIFEST --bench-save <the new save>` (the other flags stay). 0 = ready, else the exit code.
static int bench_regen_prepare(const Options& opt, const std::vector<std::string>& args, const std::string& manifest, std::string& regen_save,
                               std::vector<std::string>& scenario_args) {
    const std::string root = opt.game.empty() ? env_or_empty("ALAMO_FOC_DATA") : opt.game;
    if (root.empty()) {
        std::fprintf(stderr, "alamo_game --bench-regen needs --game <corruption dir> or ALAMO_FOC_DATA\n");
        return 2;
    }
    std::vector<std::string> game_args;
    for (const std::string& a : args)
        if (a.find('=') != std::string::npos && a.rfind("--", 0) != 0) game_args.push_back(a);
    GameData data;
    std::string err;
    if (!load_game_data(root, game_args, data, &err)) {
        std::fprintf(stderr, "game data: %s\n", err.c_str());
        return 1;
    }
    jobs::JobSystem js(jobs::JobSystemConfig{opt.workers});
    std::string summary;
    if (!bench_regen_save(data, js, manifest, &regen_save, &summary, &err)) {
        std::fprintf(stderr, "bench-regen: %s\n", err.c_str());
        return 1;
    }
    std::printf("bench-regen: %s\n", summary.c_str());
    scenario_args = {args.empty() ? std::string("alamo_game") : args[0], "--bench-make", manifest, "--bench-save", regen_save};
    return 0;
}

int main(int argc, char** argv) {
    alamo::vfs::install_steam_workshop_lookup();  // modc t10: STEAMMOD= from the Steam library folders
    enable_dpi_awareness();  // before any window: sizes and the video modes in physical pixels
    Options opt;
    const std::vector<std::string> args(argv, argv + argc);
    opt.argv.assign(args.begin() + (argc > 0 ? 1 : 0), args.end());
    std::string regen_save;  // bm3: --bench-regen S: the save made again from the scenario's make line (deleted at the end)
    {  // bm2: --bench-scenario / --bench-make FILE first; the flags below override what the file sets
        std::string serr;
        std::vector<std::string> scenario_args = args;
        for (size_t k = 1; k + 1 < args.size(); ++k)
            if (args[k] == "--bench-regen") {  // bm3: made as `--bench-make S --bench-save <the remade save>`
                if (const int rc = bench_regen_prepare(opt, args, args[k + 1], regen_save, scenario_args); rc != 0) return rc;
                break;
            }
        if (load_bench_scenario_arg(scenario_args, opt.bench, &serr) < 0) {
            std::fprintf(stderr, "benchmark: %s\n", serr.c_str());
            if (!regen_save.empty()) std::remove(regen_save.c_str());
            return 2;
        }
        if (!regen_save.empty()) opt.bench.save = regen_save;
    }
    std::string export_in, export_out;  // --export-save / --out (save_export.hpp)
    for (int i = 1; i < argc; ++i) {
        {  // BM2a: the benchmark flags (benchmark.hpp)
            size_t bi = static_cast<size_t>(i);
            std::string berr;
            const int br = parse_benchmark_arg(args, bi, opt.bench, &berr);
            if (br < 0) {
                std::fprintf(stderr, "%s\n", berr.c_str());
                return usage();
            }
            if (br > 0) {
                i = static_cast<int>(bi);
                continue;
            }
        }
        {  // BM3: --bench-new-save / --bench-maps and their options (benchmark_save.hpp)
            size_t bi = static_cast<size_t>(i);
            std::string berr;
            const int br = parse_bench_new_arg(args, bi, opt.bench_new, &berr);
            if (br < 0) {
                std::fprintf(stderr, "%s\n", berr.c_str());
                return usage();
            }
            if (br > 0) {
                i = static_cast<int>(bi);
                continue;
            }
        }
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--game") opt.game = next();
        else if (a == "--width") opt.width = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10)), opt.size_set = true;
        else if (a == "--height") opt.height = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10)), opt.size_set = true;
        else if (a == "--hidden") opt.hidden = true;
        else if (a == "--fleet-groups") opt.fleet_groups = true;  // [port] fleet groups (off by default)
        else if (a == "--audio") {
            const std::string m = next();
            if (m == "device") opt.audio = AudioBackendKind::Device;
            else if (m == "null") opt.audio = AudioBackendKind::Null;
            else if (m == "off") opt.audio = AudioBackendKind::Off;
            else return usage();
        } else if (a == "--seed") opt.seed = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--workers") opt.workers = static_cast<int32_t>(std::strtol(next().c_str(), nullptr, 10));
        else if (a == "--priority") {
            const std::string m = next();
            if (m == "above") opt.priority = 1;
            else if (m == "normal") opt.priority = 0;
            else return usage();
        } else if (a == "--frames") opt.frames = std::atol(next().c_str());
        else if (a == "--battle-max-frames") opt.battle_max_frames = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--cache-dir") opt.cache_dir = next();
        else if (a == "--png-dir") opt.png_dir = next();
        else if (a == "--smoke") opt.smoke = next();
        else if (a == "--movie") opt.movies.push_back(next());
        else if (a == "--no-intro") opt.intro = false;
        else if (a == "--attract") opt.attract = true;
        else if (a == "--mailbox") opt.mode = rhi::PresentMode::Mailbox, opt.mode_set = true;
        else if (a == "--immediate") opt.mode = rhi::PresentMode::Immediate, opt.mode_set = true;
        else if (a == "--frame-cap") {
            const std::string v = next();
            opt.frame_cap = v == "display" ? ui::kFrameCapDisplay : std::max(0, std::atoi(v.c_str()));
            opt.frame_cap_set = true;
        } else if (a == "--video-options") opt.video_options = next();
        else if (a == "--saves-dir") opt.saves_dir = next();
        else if (a == "--frame-overlay") opt.frame_overlay = true;
        else if (a == "--profile") opt.profile = true;
        else if (a == "--profile-out") opt.profile = true, opt.profile_out = next();
        else if (a == "--profile-budget") opt.profile_budget = std::strtod(next().c_str(), nullptr);
        else if (a == "--bench-scenario" || a == "--bench-make" || a == "--bench-regen") (void)next();  // bm2 / bm3: read before this loop
        else if (a == "--export-save") export_in = next();  // launcher support (save_export.hpp)
        else if (a == "--out") export_out = next();
        else if (a.find('=') != std::string::npos) opt.game_args.push_back(a);
        else return usage();
    }
    if (!export_in.empty()) {  // launcher support: no window, Vulkan or audio
        log::set_min_level(log::Level::Warn);
        return export_save_main(opt, export_in, export_out);
    }
    if (opt.bench_new.active()) return bench_new_main(opt);  // BM3: no window, no renderer
    if (opt.width == 0 || opt.height == 0) return usage();
    if (!opt.smoke.empty() && opt.smoke != "skirmish" && opt.smoke != "campaign" && opt.smoke != "land" && opt.smoke != "saveload") return usage();
    if (opt.bench.active()) {  // BM2a: no smoke script, intro, attract demo or frame-time overlay in a benchmark run
        if (const std::string why = check_benchmark_options(opt.bench); !why.empty() || !opt.smoke.empty()) {
            std::fprintf(stderr, "benchmark: %s\n", why.empty() ? "--benchmark and --smoke do not mix" : why.c_str());
            return usage();
        }
        if (const std::string why = check_bench_scenario(opt.bench); !why.empty()) {  // bm2
            std::fprintf(stderr, "benchmark: %s\n", why.c_str());
            return 2;
        }
        opt.intro = opt.attract = opt.frame_overlay = false;
        opt.movies.clear();
    }
    log::set_min_level(log::Level::Warn);
    if (opt.movies.empty() && opt.intro) opt.movies = {"Logo1", "Logo2"};  // Movies.xml names (MAIN_LOOP 5.1 step 13): LucasArtsLogo.bik, PetroglyphLogo.bik
    if (!opt.smoke.empty()) {
        if ((opt.game.empty() ? env_or_empty("ALAMO_FOC_DATA") : opt.game).empty()) {
            std::printf("alamo_game smoke %s: ALAMO_FOC_DATA / --game not set: skipped\n", opt.smoke.c_str());
            return 0;
        }
        // Smoke battles end on the time limit; the skirmish battles get longer for their forces to meet and fight (the
        // space fleets start ~5000 units apart).
        if (opt.battle_max_frames == 27000) opt.battle_max_frames = opt.smoke == "campaign" ? 900u : 4800u;
    }
    set_process_priority(opt.priority >= 0 ? opt.priority == 1 : opt.smoke.empty());
    App app(std::move(opt));
    const int rc = app.run();
    if (!regen_save.empty()) std::remove(regen_save.c_str());  // bm3: --bench-regen's temporary save
    return rc;
}
