#include "alamo/ui/load_screen.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace alamo::ui {

const char* load_screen_dialog(LoadScreenKind k) {
    switch (k) {
        case LoadScreenKind::LandBattle: return "IDD_LAND_BATTLE_LOAD_DIALOG";
        case LoadScreenKind::SpaceBattle: return "IDD_SPACE_BATTLE_LOAD_DIALOG";
        case LoadScreenKind::Skirmish: return "IDD_SKIRMISH_BATTLE_LOAD_DIALOG";
        case LoadScreenKind::LandMission: return "IDD_LAND_MISSION_LOAD_DIALOG";
        case LoadScreenKind::SpaceMission: return "IDD_SPACE_MISSION_LOAD_DIALOG";
    }
    return "";
}

namespace {
int32_t pct(float f) { return static_cast<int32_t>(std::lround(std::clamp(f, 0.0f, 1.0f) * 100.0f)); }
}  // namespace

bool LoadScreen::open(GuiManager& gui, LoadScreenKind kind, const LoadScreenInfo& info) {
    close();
    GuiOpenOptions opt;
    opt.opaque_backdrop = true;
    GuiDialog* d = gui.open(load_screen_dialog(kind), opt);
    if (!d) return false;
    gui_ = &gui;
    gui.set_backdrop({});  // the load dialog covers the screen; no front-end backdrop under it [port]
    dialog_ = std::string(d->name());
    info_ = info;
    local_ = 0.0f;
    ready_ = false;
    players_.assign(info.multiplayer ? std::min<size_t>(info.players.size(), 9) : 0, 0.0f);
    d->show("IDC_BEGIN_BUTTON", false);  // hidden while loading (§5.1 step 1)
    d->set_text("IDC_STATIC_PLANET_NAME", gui_no_asset_name(info.planet_name));
    // Caption + its label (IDC_STATIC_MED*, GUIDialogs.rc): an empty text hides both.
    const std::pair<const char*, const char*> captions[] = {{"IDC_STATIC_TERRAIN", "IDC_STATIC_MED3"},
                                                            {"IDC_STATIC_WEATHER", "IDC_STATIC_MED2"},
                                                            {"IDC_STATIC_NATIVE", "IDC_STATIC_MED1"}};
    const std::u32string* caption_text[] = {&info.terrain, &info.weather, &info.natives};
    for (size_t k = 0; k < 3; ++k) {
        d->set_text(captions[k].first, *caption_text[k]);
        d->show(captions[k].first, !caption_text[k]->empty());
        d->show(captions[k].second, !caption_text[k]->empty());
    }
    const std::string overlay = info.planet_overlay.empty() ? std::string("load_overlay.tga") : info.planet_overlay;
    const std::pair<const char*, const std::string*> images[] = {{"IDC_PLANET_ICON", &info.planet_icon},
                                                                 {"IDC_PLANET_OVERLAY", &overlay},
                                                                 {"IDC_WEATHER_ICON", &info.weather_icon},
                                                                 {"IDC_MAP_ICON", &info.map_icon},
                                                                 {"IDC_NATIVE_ICON", &info.native_icon}};
    for (const auto& [id, tex] : images) {
        d->set_image(id, *tex);
        d->show(id, !tex->empty());
    }
    constexpr Rgba8 kSkinColor{0, 0, 0, 0};
    gui.set_paragraphs(*d, "IDC_LIST_TACTICAL", info.tactical, Rgba8{50, 200, 255, 255}, kPlanetListWidth);  // as IDD_PLANET_INFO
    gui.set_paragraphs(*d, "IDC_LIST_ADVANTAGE", info.advantages, kSkinColor, kPlanetListWidth);
    gui.set_paragraphs(*d, "IDC_LIST_REFERENCE", info.history, kSkinColor, kPlanetListWidth);
    d->set_value("IDC_PROGRESS_SOLO", 0);
    d->show("IDC_PROGRESS_SOLO", !info.multiplayer);
    char id[32];
    for (size_t p = 0; p < 9; ++p) {
        const bool used = info.multiplayer && p < players_.size();
        std::snprintf(id, sizeof id, "IDC_PROGRESS%zu", p);
        d->show(id, used);
        d->set_value(id, 0);
        std::snprintf(id, sizeof id, "IDC_PLAYER_NAME_%zu", p);
        d->show(id, used);
        if (used) d->set_text(id, info.players[p]);
    }
    return true;
}

void LoadScreen::close() {
    if (gui_ && !dialog_.empty()) gui_->close(dialog_);
    gui_ = nullptr;
}

void LoadScreen::set_player_progress(size_t player, float fraction) {
    if (player < players_.size()) players_[player] = std::clamp(fraction, 0.0f, 1.0f);
}

void LoadScreen::update(std::span<const GuiEvent> events, std::vector<LoadScreenEvent>& out) {
    if (!gui_) return;
    GuiDialog* d = gui_->find(dialog_);
    if (!d) {
        gui_ = nullptr;
        return;
    }
    if (progress_) local_ = std::clamp(progress_(), 0.0f, 1.0f);
    bool done = local_ >= 1.0f;
    if (info_.multiplayer) {
        char id[32];
        done = !players_.empty();
        for (size_t p = 0; p < players_.size(); ++p) {
            std::snprintf(id, sizeof id, "IDC_PROGRESS%zu", p);
            d->set_value(id, pct(players_[p]));
            done = done && players_[p] >= 1.0f;
        }
    } else {
        d->set_value("IDC_PROGRESS_SOLO", pct(local_));
    }
    if (done && !ready_) {
        ready_ = true;
        d->set_value("IDC_PROGRESS_SOLO", 100);
        out.push_back({LoadScreenEvent::Kind::Ready});
        if (info_.multiplayer || !info_.begin_button) {
            out.push_back({LoadScreenEvent::Kind::Begin});
            close();
            return;
        }
        d->show("IDC_BEGIN_BUTTON", true);
        d->focus("IDC_BEGIN_BUTTON");
        gui_->play_sfx(kLoadScreenReadySfx);  // through the GUI's SFX hook (§5.1 step 4)
    }
    for (const GuiEvent& e : events) {
        if (e.kind == GuiEvent::Kind::Clicked && e.dialog == dialog_ && e.control == "IDC_BEGIN_BUTTON" && ready_) {
            out.push_back({LoadScreenEvent::Kind::Begin});
            close();
            return;
        }
    }
}

}  // namespace alamo::ui
