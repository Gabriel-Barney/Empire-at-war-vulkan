// alamo_game UI resources (P2 / P4): GUI dialogs + menus + load screens (GuiManager / FrontEnd / LoadScreen), the
// galactic HUD and the tactical HUD with their command-bar shells, fonts, the MT_CommandBar atlas, MasterTextFile and
// the GUI image pages. Presentation only: HUD / menu actions leave as commands or MenuActions for the host.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "alamo/game/game_data.hpp"
#include "alamo/ui/galactic_hud.hpp"
#include "alamo/ui/gui_dialog.hpp"
#include "alamo/ui/gui_menus.hpp"
#include "alamo/ui/gui_resource.hpp"
#include "alamo/ui/hud_font.hpp"
#include "alamo/ui/hud_model_draw.hpp"
#include "alamo/ui/hud_shell.hpp"
#include "alamo/ui/hud_shell_draw.hpp"
#include "alamo/ui/hud_world.hpp"
#include "alamo/ui/load_screen.hpp"
#include "alamo/ui/tactical_hud.hpp"
#include "game_render.hpp"

namespace alamo::game {

class GameUi {
public:
    // Loads every UI table through data.files and uploads the atlas / shells. False + err when the GUI script or the
    // atlas is missing (the game cannot show its menus then).
    bool init(GpuCore& gpu, const GameData& data, ui::MenuProviders providers, std::string* err);
    void shutdown(GpuCore& gpu);
    void resize(uint32_t w, uint32_t h);
    // Uploads image pages the dialogs asked for since the last call (MenuBack_overlay, load-screen icons, ...).
    // `portraits`: serves "planet_portrait:<type>" pages (null = they fail like a missing texture).
    void upload_pages(GpuCore& gpu, render::PlanetPortraits* portraits = nullptr);
    // Faction look of both HUDs ([port] 0 Empire, 1 Rebel, 2 Underworld faceplates; pad lists 0 rebel / 1 empire).
    void set_faction(std::string_view faction);
    uint8_t faction_alt() const { return faction_alt_; }

    ui::GuiData gui_data;
    ui::TextureAtlas atlas;
    ui::TextDatabase text;
    ui::ComponentTable components;
    std::unique_ptr<ui::GlyphRasterizer> raster;
    std::unique_ptr<ui::FontCache> fonts;
    ui::GuiManager gui;
    ui::GuiImagePages pages;
    // GUI image pages served from memory instead of the data files, by the name the dialog asked for (the staging map
    // preview's embedded image, UI.md 9.6); upload_pages checks it before the asset cache.
    std::map<std::string, std::shared_ptr<const assets::TextureAsset>> memory_textures;
    ui::FrontEnd front;
    ui::LoadScreen load;
    ui::GalacticHud galactic_hud;
    ui::TacticalHud tactical_hud;
    ui::ShellSet galactic_shells, tactical_shells;
    ui::ShellDrawer galactic_drawer, tactical_drawer;
    bool galactic_shells_ok = false, tactical_shells_ok = false;
    ui::HudModelDrawer radar_drawer;  // tactical radar event .alo models (RadarMap.xml Event_Model_Name), Models2 step
    bool radar_models_ok = false;
    std::vector<ui::HudTypeInfo> hud_types;
    ui::HotkeyTable hotkeys;
    ui::Bindings bindings;

private:
    uint8_t faction_alt_ = 1;
};

}  // namespace alamo::game
