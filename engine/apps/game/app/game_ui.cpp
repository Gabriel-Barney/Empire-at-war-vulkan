#include "game_ui.hpp"

#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "alamo/core/ascii.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/vfs/vfs.hpp"

namespace alamo::game {

bool GameUi::init(GpuCore& gpu, const GameData& data, ui::MenuProviders providers, std::string* err) {
    std::string e;
    if (!ui::load_gui_data(*data.files, gui_data, &e)) {
        if (err) *err = "GUI dialogs: " + e;
        return false;
    }
    if (!gui_data.script.merge(ui::kPortDialogsRc, &e)) std::fprintf(stderr, "port dialogs: %s\n", e.c_str());  // camera / controls
    if (!gui_data.script.extend(ui::kPortDialogExtensionsRc, &e)) std::fprintf(stderr, "port dialog controls: %s\n", e.c_str());  // v-sync mode
    std::vector<uint8_t> mtd;
    if (!data.files->read("Data\\Art\\Textures\\MT_CommandBar.mtd", mtd) || !atlas.parse(mtd, &e)) {
        if (err) *err = "MT_CommandBar.mtd: " + e;
        return false;
    }
    const auto atlas_tex = gpu.cache->texture("MT_CommandBar.tga");
    if (!atlas_tex || atlas_tex->is_fallback || !gpu.hud.set_atlas_texture(*atlas_tex)) {
        if (err) *err = "MT_CommandBar.tga not loaded";
        return false;
    }
    atlas.texture_width = atlas_tex->width;
    atlas.texture_height = atlas_tex->height;
    if (!ui::load_master_text(*data.files, "ENGLISH", text)) std::fprintf(stderr, "MasterTextFile not loaded: keys shown as text\n");
    {  // [port] dev tool, off by default: ALAMO_PSEUDO_LOC=1 makes every MasterTextFile string 35 % longer (overflow check)
        char* v = nullptr;
        size_t len = 0;
        if (_dupenv_s(&v, &len, "ALAMO_PSEUDO_LOC") == 0 && v) {
            if (std::string_view(v) == "1") {
                text.pseudo_localize_all();
                std::printf("pseudo-localization on: %zu strings\n", text.size());
            }
            std::free(v);
        }
    }
    // The game's own fonts (EmpireAtWar-Bold ...) live inside StarWarsG.exe: register them as private fonts before the
    // rasterizer is made (mod dirs first, then the FoC root, then EaW's GameData; earlier roots win per name).
    {
        std::vector<std::string> font_roots = data.vfs ? data.vfs->mod_dirs() : std::vector<std::string>{};
        font_roots.push_back(data.root);
        if (data.vfs && !data.vfs->eaw_root().empty()) font_roots.push_back(data.vfs->eaw_root());
        const std::vector<ui::GameFont> fonts_found = ui::register_game_fonts(font_roots);
        std::printf("fonts: %zu game fonts registered\n", fonts_found.size());
    }
    raster = ui::make_system_rasterizer();
    fonts = std::make_unique<ui::FontCache>(*raster);
    gui.init(gui_data.script, gui_data.skin, atlas, *fonts, &text);
    gui.set_image_resolver([this](std::string_view t) { return pages.resolve(t); });
    providers.hotkeys = &hotkeys;
    providers.bindings = &bindings;
    front.init(gui, ui::build_campaign_menu(data.db), std::move(providers));

    // HUD tables and shells.
    if (!components.load(*data.files, &e)) std::fprintf(stderr, "CommandBarComponents: %s\n", e.c_str());
    const render::TextureSource tex = [&gpu](std::string_view n, assets::TextureRole role) { return gpu.cache->texture(n, role); };
    auto loader = [&gpu](std::string_view n) { return gpu.cache->model(n); };
    {
        // HUD_GAPS §4.2: map_shell (planetary_mode.alo) = the advanced planet map; its day_clock bone hosts the first day pip.
        // GALACTIC_DIALOGS §1.2: the battle-prompt shells (top-centre banners), shown by GalacticHud::shell_visibility().
        const std::string names[] = {"i_main_commandbar", "i_main_organize", "map_shell", "pending_battle_shell", "story_pending_battle_shell"};
        std::vector<std::string> missing;
        galactic_shells.load(components, loader, names, &missing);
        for (const std::string& m : missing) std::fprintf(stderr, "galactic HUD: shell %s missing\n", m.c_str());
        galactic_shells_ok = !galactic_shells.empty() && galactic_drawer.upload(gpu.renderer, galactic_shells, tex, &e);
    }
    {
        const std::string names[] = {"i_main_skirmish"};
        std::vector<std::string> missing;
        tactical_shells.load(components, loader, names, &missing);
        for (const std::string& m : missing) std::fprintf(stderr, "tactical HUD: shell %s missing\n", m.c_str());
        tactical_shells_ok = !tactical_shells.empty() && tactical_drawer.upload(gpu.renderer, tactical_shells, tex, &e);
    }
    radar_drawer.set_loader(loader, tex);  // models are uploaded the first time a ping names them (prepare)
    radar_models_ok = true;
    galactic_drawer.wait_uploads(gpu.up);
    tactical_drawer.wait_uploads(gpu.up);
    // The command bar is always shown; the fleet organiser only with a selected planet ([port]: no planet selection yet),
    // map_shell only while the advanced planet map is open (the host's build_galactic_hud).
    for (const ui::ShellSet::Shell& s : galactic_shells.shells())
        galactic_shells.set_visible(s.component, s.model_name.find("organize") == std::string::npos &&
                                                     static_cast<size_t>(s.component) < components.size() &&
                                                     components[s.component].name != "i_main_organize" &&
                                                     components[s.component].name != "map_shell" &&
                                                     components[s.component].name != "pending_battle_shell" &&
                                                     components[s.component].name != "story_pending_battle_shell");
    for (const ui::ShellSet::Shell& s : tactical_shells.shells()) tactical_shells.set_visible(s.component, true);
    galactic_hud.init(components, atlas, *fonts, &text);
    tactical_hud.init(components, atlas, *fonts, &text);
    galactic_shells.apply(galactic_hud.layout());
    tactical_shells.apply(tactical_hud.layout());
    hud_types = ui::build_hud_type_table(data.types);
    // UI sounds: the GUI / HUD SFX hooks are installed by the host (audio).
    return true;
}

void GameUi::set_faction(std::string_view faction, std::string_view good_side, std::string_view evil_side) {
    const bool good = !good_side.empty() && ascii_iequals(faction, good_side), evil = !evil_side.empty() && ascii_iequals(faction, evil_side);
    const int32_t shell_alt = good ? 1 : evil ? 0 : 2;
    faction_alt_ = good ? 0 : 1;
    for (const ui::ShellSet::Shell& s : galactic_shells.shells()) galactic_shells.set_alt(s.component, shell_alt);
    for (const ui::ShellSet::Shell& s : tactical_shells.shells()) tactical_shells.set_alt(s.component, shell_alt);
}

void GameUi::resize(uint32_t w, uint32_t h, int32_t ui_scale_pct) {
    const float s = static_cast<float>(ui_scale_pct) / 100.0f;
    gui.set_ui_scale(s);
    galactic_hud.set_ui_scale(s);
    tactical_hud.set_ui_scale(s);
    gui.resize(w, h);
    galactic_hud.resize(w, h);
    tactical_hud.resize(w, h);
    galactic_shells.apply(galactic_hud.layout());
    tactical_shells.apply(tactical_hud.layout());
    galactic_hud.layout().update(galactic_hud.canvas(), components);
    tactical_hud.layout().update(tactical_hud.canvas(), components);
}

void GameUi::upload_pages(GpuCore& gpu, render::PlanetPortraits* portraits) {
    for (ui::GuiImagePages::Entry& e : pages.entries()) {
        if (e.width != 0 || e.failed) continue;
        if (render::is_planet_portrait_name(e.name)) {  // rendered planet model (pending for a few frames)
            const auto t = portraits ? portraits->texture(e.name) : nullptr;
            if (!t) {
                if (!portraits || portraits->failed(e.name)) pages.set_failed(e.page);
                continue;
            }
            if (gpu.hud.set_image_page(e.page, *t)) pages.set_loaded(e.page, t->width, t->height);
            else pages.set_failed(e.page);
            continue;
        }
        if (const auto m = memory_textures.find(e.name); m != memory_textures.end()) {
            if (gpu.hud.set_image_page(e.page, *m->second)) pages.set_loaded(e.page, m->second->width, m->second->height);
            else pages.set_failed(e.page);
            continue;
        }
        const auto tex = gpu.cache->texture(e.name);
        if (!tex || tex->is_fallback || !gpu.hud.set_image_page(e.page, *tex)) pages.set_failed(e.page);
        else pages.set_loaded(e.page, tex->width, tex->height);
    }
}

void GameUi::shutdown(GpuCore& gpu) {
    galactic_drawer.destroy(gpu.renderer);
    tactical_drawer.destroy(gpu.renderer);
    radar_drawer.destroy(gpu.renderer);
}

}  // namespace alamo::game
