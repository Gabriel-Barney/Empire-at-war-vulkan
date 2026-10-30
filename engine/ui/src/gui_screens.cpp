#include "alamo/ui/gui_screens.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace alamo::ui {

namespace {

bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const auto lo = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
        if (lo(a[i]) != lo(b[i])) return false;
    }
    return true;
}

std::u32string ascii32(std::string_view s) {
    std::u32string o;
    for (char c : s) o.push_back(static_cast<char32_t>(static_cast<unsigned char>(c)));
    return o;
}

std::u32string num32(int64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "%lld", static_cast<long long>(v));
    return ascii32(b);
}

std::string id_n(const char* fmt, int32_t n) {
    char b[48];
    std::snprintf(b, sizeof b, fmt, n);
    return b;
}

// printf-style %d / %0Nd substitution of the shipped TEXT_* patterns (integers only; other text kept as written).
std::u32string format_ints(std::u32string_view pattern, std::initializer_list<int64_t> values) {
    std::u32string out;
    const int64_t* next = values.begin();
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != U'%') {
            out.push_back(pattern[i]);
            continue;
        }
        size_t j = i + 1;
        const bool zero = j < pattern.size() && pattern[j] == U'0';
        size_t width = 0;
        while (j < pattern.size() && pattern[j] >= U'0' && pattern[j] <= U'9') width = width * 10 + static_cast<size_t>(pattern[j++] - U'0');
        if (j < pattern.size() && pattern[j] == U'd' && next != values.end()) {
            std::u32string n = num32(*next++);
            if (n.size() < width) n.insert(0, width - n.size(), zero ? U'0' : U' ');
            out += n;
            i = j;
        } else if (i + 1 < pattern.size() && pattern[i + 1] == U'%') {
            out.push_back(U'%');
            ++i;
        } else {
            out.push_back(U'%');
        }
    }
    return out;
}

// TEXT_BATTLE_TIME is "Battle Time:" in the shipped master text (no number); a pattern with %d takes the h / m / s.
std::u32string battle_time_text(std::u32string_view pattern, int64_t seconds) {
    const int64_t s = seconds < 0 ? 0 : seconds;
    const int64_t h = s / 3600, m = (s / 60) % 60, sec = s % 60;
    if (pattern.find(U'%') != std::u32string_view::npos) return format_ints(pattern, {h, m, sec});
    std::u32string out(pattern);
    if (!out.empty() && out.back() != U' ') out.push_back(U' ');
    return out + format_ints(U"%02d:%02d:%02d", {h, m, sec});
}

constexpr int32_t kLossRows = 12, kHeroRows = 5;

ScreenAction act(ScreenAction::Kind k) {
    ScreenAction a;
    a.kind = k;
    return a;
}

}  // namespace

const char* screen_action_name(ScreenAction::Kind k) {
    switch (k) {
        case ScreenAction::Kind::MessageBoxResult: return "MessageBoxResult";
        case ScreenAction::Kind::BattleEndContinue: return "BattleEndContinue";
        case ScreenAction::Kind::LandInvade: return "LandInvade";
        case ScreenAction::Kind::SaveReplay: return "SaveReplay";
        case ScreenAction::Kind::CampaignScoreClosed: return "CampaignScoreClosed";
        case ScreenAction::Kind::RetreatConfirmed: return "RetreatConfirmed";
        case ScreenAction::Kind::AutoResolve: return "AutoResolve";
        case ScreenAction::Kind::RetreatCancelled: return "RetreatCancelled";
        case ScreenAction::Kind::SendChat: return "SendChat";
        case ScreenAction::Kind::ChatClosed: return "ChatClosed";
        case ScreenAction::Kind::PlanetInfoClosed: return "PlanetInfoClosed";
        case ScreenAction::Kind::Withdraw: return "Withdraw";
        case ScreenAction::Kind::MapSelected: return "MapSelected";
        case ScreenAction::Kind::MapSelectCancelled: return "MapSelectCancelled";
        case ScreenAction::Kind::BuyUnit: return "BuyUnit";
        case ScreenAction::Kind::SellUnit: return "SellUnit";
        case ScreenAction::Kind::SellAllUnits: return "SellAllUnits";
        case ScreenAction::Kind::BuyUnitsDone: return "BuyUnitsDone";
        case ScreenAction::Kind::BuyUnitsBack: return "BuyUnitsBack";
        case ScreenAction::Kind::StoryAccepted: return "StoryAccepted";
        case ScreenAction::Kind::StoryCancelled: return "StoryCancelled";
        case ScreenAction::Kind::StoryClosed: return "StoryClosed";
        case ScreenAction::Kind::StorySummaryFilter: return "StorySummaryFilter";
        case ScreenAction::Kind::StoryAbandon: return "StoryAbandon";
        case ScreenAction::Kind::StorySummaryClosed: return "StorySummaryClosed";
        case ScreenAction::Kind::PlanetOrganize: return "PlanetOrganize";
        case ScreenAction::Kind::PlanetProduction: return "PlanetProduction";
        case ScreenAction::Kind::PlanetDialogClosed: return "PlanetDialogClosed";
        case ScreenAction::Kind::InfoPageChanged: return "InfoPageChanged";
        case ScreenAction::Kind::InfoClosed: return "InfoClosed";
        case ScreenAction::Kind::InfoHintsToggled: return "InfoHintsToggled";
        case ScreenAction::Kind::SummarySort: return "SummarySort";
        case ScreenAction::Kind::SummaryPlanetary: return "SummaryPlanetary";
        case ScreenAction::Kind::StorySummaryGoal: return "StorySummaryGoal";
    }
    return "?";
}

GuiDialog* GameScreens::open(const char* name) { return gui_ ? gui_->open(name) : nullptr; }

std::u32string GameScreens::text(std::string_view key, std::u32string_view fallback) const {
    if (gui_ && gui_->text_db() && gui_->text_db()->find(key)) return gui_->caption_text(key);
    return std::u32string(fallback);
}

void GameScreens::close(std::string_view dialog) {
    if (!gui_) return;
    gui_->close(dialog);
    if (iequal(dialog, msg_dialog_)) {
        msg_dialog_.clear();
        msg_tag_.clear();
        msg_buttons_.clear();
    }
}

void GameScreens::close_all() {
    if (!gui_) return;
    for (const char* n : {dlg::kBattleEnd, dlg::kCampaignScore, dlg::kRetreat, dlg::kChat, dlg::kPlanetInfo,
                          dlg::kAutoResolve, dlg::kSelectMap})
        gui_->close(n);
    if (!msg_dialog_.empty()) close(std::string(msg_dialog_));
}

// ---- message box ------------------------------------------------------------------------------------------
bool GameScreens::show_message(const MessageBoxSpec& spec) {
    if (!gui_) return false;
    if (!msg_dialog_.empty()) close(std::string(msg_dialog_));
    const size_t nb = std::min<size_t>(spec.buttons.size(), 3);
    std::string name;
    if (spec.small) name = "IDD_DIALOG_BOX_SMALL";
    else name = nb == 0 ? "IDD_DIALOG_BOX0" : nb == 1 ? "IDD_DIALOG_BOX1" : nb == 2 ? "IDD_DIALOG_BOX2" : "IDD_DIALOG_BOX";
    if (spec.centered && !spec.small) name += "_CENTERED";
    GuiDialog* d = gui_->open(name);
    if (!d && spec.centered) {  // the plain variant when the script has no centred one
        name.resize(name.size() - 9);
        d = gui_->open(name);
    }
    if (!d) return false;
    msg_dialog_ = name;
    msg_tag_ = spec.tag;
    msg_buttons_.clear();
    d->set_text("IDC_CAPTION", spec.caption);
    for (int32_t i = 0; i < 3; ++i) {
        const std::string id = id_n("IDC_USER_TEXT_STRING_%d", i + 1);
        const bool has = static_cast<size_t>(i) < spec.lines.size();
        d->show(id, has);
        if (has) d->set_text(id, spec.lines[static_cast<size_t>(i)]);
    }
    static constexpr const char* k1[] = {"IDC_DIALOG_BOX_BUTTON_LEFT"};
    static constexpr const char* k2[] = {"IDC_DIALOG_BOX_BUTTON_LEFT", "IDC_DIALOG_BOX_BUTTON_RIGHT"};
    static constexpr const char* k3[] = {"IDC_DIALOG_BOX_BUTTON_LEFT", "IDC_DIALOG_BOX_BUTTON_CENTER", "IDC_DIALOG_BOX_BUTTON_RIGHT"};
    std::span<const char* const> ids;
    if (nb == 1) ids = k1;
    else if (nb == 2) ids = k2;
    else if (nb == 3) ids = k3;
    if (spec.small) ids = {};
    for (size_t i = 0; i < ids.size(); ++i) {
        d->set_text(ids[i], spec.buttons[i]);
        msg_buttons_.emplace_back(ids[i]);
    }
    if (!msg_buttons_.empty()) d->focus(msg_buttons_.front());
    return true;
}

// ---- battle end -------------------------------------------------------------------------------------------
void GameScreens::fill_battle_end_units(GuiDialog& d) {
    const char* img[2] = {"IDC_YOUR_UNIT%d", "IDC_ENEMY_UNIT%d"};
    const char* nam[2] = {"IDC_YOUR_UNIT_NAME%d", "IDC_ENEMY_UNIT_NAME%d"};
    const std::vector<BattleEndUnit>* lists[2] = {&end_.your_losses, &end_.enemy_losses};
    for (int side = 0; side < 2; ++side) {
        const std::vector<BattleEndUnit>& l = *lists[side];
        for (int32_t r = 0; r < kLossRows; ++r) {
            const auto k = static_cast<size_t>(end_off_[side] + r);
            const bool has = k < l.size();
            const std::string ii = id_n(img[side], r), nn = id_n(nam[side], r);
            d.show(ii, has);
            d.show(nn, has);
            if (!has) continue;
            d.set_image(ii, l[k].icon);
            std::u32string t = l[k].name;
            const size_t name_size = t.size();
            if (l[k].count > 1) t += U" x" + num32(l[k].count);
            const size_t tail = t.size() - name_size;  // t47 [port]: a long name is cut, the count stays
            d.set_text(nn, std::move(t));
            if (GuiControl* c = d.get(nn)) c->fit_keep_tail = tail;
        }
    }
}

// QA U2 / U3 [port]: the names wrap (never narrowed) in a box as tall as their icon, and a side without a hero row starts
// its rows right under the loss header (the .rc keeps the space above them for IDC_*_HERO0..4).
void GameScreens::layout_battle_end_rows(GuiDialog& d, const BattleEndInfo& info) {
    const char* img[2] = {"IDC_YOUR_UNIT%d", "IDC_ENEMY_UNIT%d"};
    const char* nam[2] = {"IDC_YOUR_UNIT_NAME%d", "IDC_ENEMY_UNIT_NAME%d"};
    const char* bars[2] = {"IDC_YOUR_SCROLLBAR", "IDC_ENEMY_SCROLLBAR"};
    const bool heroes[2] = {!info.your_heroes.empty(), !info.enemy_heroes.empty()};
    for (int side = 0; side < 2; ++side) {
        const float up = heroes[side] ? 0.0f : kBattleEndRowsUp;
        for (int32_t r = 0; r < kLossRows; ++r) {
            GuiControl* icon = d.get(id_n(img[side], r));
            GuiControl* name = d.get(id_n(nam[side], r));
            if (icon) icon->offset.y = up;
            if (!icon || !name || !icon->def || !name->def || name->def->h <= 0) continue;
            name->fit_lines = 2;
            name->offset.y = static_cast<float>(name->def->y - icon->def->y) + up;
            name->scale.y = static_cast<float>(icon->def->h) / static_cast<float>(name->def->h);
        }
        if (GuiControl* b = d.get(bars[side])) b->offset.y = up;
    }
}

// The loss values and efficiencies (playtest 2 t13). The shipped GUIDialogs.rc battle-end template has no
// IDC_*_LOSS_VAL_STATIC / IDC_EFFICIENCY_STATIC controls (the binary's handler looks them up), so when a control is
// missing the figure is appended to the matching loss header (IDC_STATIC_LOSS1 yours, IDC_STATIC_LOSS2 the enemy's).
void GameScreens::fill_battle_end_values(GuiDialog& d, const BattleEndInfo& info) {
    const int64_t value[2] = {info.your_loss_value, info.enemy_loss_value};
    const int32_t eff[2] = {info.your_efficiency, info.enemy_efficiency};
    const char* val_id[2] = {"IDC_YOUR_LOSS_VAL_STATIC", "IDC_ENEMY_LOSS_VAL_STATIC"};
    const char* eff_id[2] = {"IDC_EFFICIENCY_STATIC", "IDC_EFFICIENCY_STATIC2"};
    const char* head_id[2] = {"IDC_STATIC_LOSS1", "IDC_STATIC_LOSS2"};
    for (int side = 0; side < 2; ++side) {
        std::u32string tail;
        if (value[side] >= 0) {
            const std::u32string v = U"$ " + num32(value[side]);
            if (d.get(val_id[side])) {
                d.show(val_id[side], true);
                d.set_text(val_id[side], v);
            } else {
                tail += U"   " + v;
            }
        }
        if (eff[side] >= 0) {
            const std::u32string v = num32(eff[side]) + U"%";
            if (d.get(eff_id[side])) {
                d.show(eff_id[side], true);
                d.set_text(eff_id[side], v);
            } else {
                tail += U"   " + v;  // QA t12: bare %, as the missing control shows it; label + figures crossed the divider
            }
        }
        GuiControl* h = d.get(head_id[side]);
        if (!h) continue;
        const std::u32string& old_tail = end_tail_[side];
        if (!old_tail.empty() && h->text.size() >= old_tail.size() &&
            h->text.compare(h->text.size() - old_tail.size(), old_tail.size(), old_tail) == 0)
            h->text.resize(h->text.size() - old_tail.size());
        h->text += tail;
        end_tail_[side] = tail;
    }
}

bool GameScreens::show_battle_end(const BattleEndInfo& info) {
    GuiDialog* d = open(dlg::kBattleEnd);
    if (!d) return false;
    end_ = info;
    end_off_[0] = end_off_[1] = 0;
    std::u32string title = info.title;
    if (title.empty()) title = info.victory ? text("TEXT_VICTORY", U"Victory") : text("TEXT_DEFEAT", U"Defeat");
    d->set_text("IDC_TITLE_STATIC", std::move(title));
    d->set_text("IDC_BATTLE_END_PLANET_STATIC", gui_no_asset_name(info.planet));
    std::u32string time = info.time;
    if (time.empty() && info.battle_seconds >= 0) time = battle_time_text(text("TEXT_BATTLE_TIME", U"Battle Time:"), info.battle_seconds);
    d->set_text("IDC_TIME_STATIC", std::move(time));
    d->show("IDC_STATIC_BOUNTY", info.bounty >= 0);
    if (info.bounty >= 0) d->set_text("IDC_STATIC_BOUNTY", format_ints(text("TEXT_SUMMARY_BOUNTY_COLLECTED", U"%d"), {info.bounty}));
    fill_battle_end_values(*d, info);
    d->show("IDC_LAND_INVADE_BUTTON", info.can_invade);
    d->show("IDC_SAVE_REPLAY_BUTTON", info.can_save_replay);
    const char* hero[2] = {"IDC_YOUR_HERO%d", "IDC_ENEMY_HERO%d"};
    const std::vector<std::string>* heroes[2] = {&info.your_heroes, &info.enemy_heroes};
    for (int side = 0; side < 2; ++side)
        for (int32_t r = 0; r < kHeroRows; ++r) {
            const std::string id = id_n(hero[side], r);
            const bool has = static_cast<size_t>(r) < heroes[side]->size();
            d->show(id, has);
            if (has) d->set_image(id, (*heroes[side])[static_cast<size_t>(r)]);
        }
    const char* bars[2] = {"IDC_YOUR_SCROLLBAR", "IDC_ENEMY_SCROLLBAR"};
    const size_t n[2] = {info.your_losses.size(), info.enemy_losses.size()};
    for (int side = 0; side < 2; ++side) {
        d->set_value(bars[side], 0);
        d->show(bars[side], n[side] > static_cast<size_t>(kLossRows));
    }
    for (const char* a : {"IDC_RIGHT_ARROW1", "IDC_RIGHT_ARROW2"}) d->show(a, false);  // [port] no art known
    layout_battle_end_rows(*d, info);
    fill_battle_end_units(*d);
    set_countdown(info.countdown);
    d->focus("IDC_CONTINUE_BUTTON");
    return true;
}

void GameScreens::set_countdown(float seconds) {
    GuiDialog* d = gui_ ? gui_->find(dlg::kBattleEnd) : nullptr;
    if (!d) return;
    end_.countdown = seconds;
    d->show("IDC_COUNTDOWN_TIMER", seconds >= 0.0f);
    if (seconds >= 0.0f) d->set_text("IDC_COUNTDOWN_TIMER", num32(static_cast<int64_t>(std::ceil(seconds))));
}

// ---- campaign score ---------------------------------------------------------------------------------------
bool GameScreens::show_campaign_score(const CampaignScoreInfo& info) {
    GuiDialog* d = open(dlg::kCampaignScore);
    if (!d) return false;
    std::u32string h = info.headline;
    if (h.empty()) h = info.victory ? text("TEXT_VICTORY", U"Victory") : text("TEXT_DEFEAT", U"Defeat");
    d->set_text("IDC_VICTORY_STATIC", std::move(h));
    d->set_text("IDC_TITLE_STATIC", info.rank_title);
    auto pct = [](int32_t v) { return v < 0 ? std::u32string() : num32(v) + U"%"; };
    d->set_text("IDC_MILITARY_EFFICIENCY_STATIC", pct(info.military));
    d->set_text("IDC_CONQUEST_EFFICIENCY_STATIC", pct(info.conquest));
    d->set_text("IDC_KILL_EFFICIENCY_STATIC", pct(info.kill));
    d->set_items("IDC_VICTORY_LIST", info.list, -1);
    d->show("IDC_BATTLE_VICTORY_IMAGE", !info.image.empty());
    d->set_image("IDC_BATTLE_VICTORY_IMAGE", info.image);
    d->focus("IDC_QUIT_BUTTON");
    return true;
}

// ---- retreat / chat / planet info / auto-resolve ----------------------------------------------------------
bool GameScreens::show_retreat_confirm() {
    GuiDialog* d = open(dlg::kRetreat);
    if (!d) return false;
    d->focus("IDC_RETRY_CANCEL");
    return true;
}

bool GameScreens::open_chat(bool team_only) {
    GuiDialog* d = open(dlg::kChat);
    if (!d) return false;
    d->set_text("IDC_IME_EDITBOX", {});
    d->set_checked("IDC_TEAM_MEMBERS_ONLY", team_only);
    d->focus("IDC_IME_EDITBOX");
    return true;
}

bool GameScreens::chat_open() const { return gui_ && gui_->find(dlg::kChat) != nullptr; }

std::string planet_info_weather_icon(std::string_view icon) {
    if (icon.empty()) return {};
    return "load" + std::string(icon.size() > 4 ? icon.substr(4) : std::string_view());
}

std::string planet_info_map_icon(std::string_view map) {
    if (map.empty()) return {};
    if (map.size() >= 4 && iequal(map.substr(map.size() - 4), ".ted")) return std::string(map.substr(0, map.size() - 4)) + ".dds";
    return std::string(map) + ".dds";
}

bool GameScreens::planet_info_open() const { return gui_ && gui_->find(dlg::kPlanetInfo) != nullptr; }

void GameScreens::close_planet_info() {
    if (gui_) gui_->close(dlg::kPlanetInfo);
}

bool GameScreens::show_planet_info(const PlanetInfoData& p) {
    if (planet_info_open()) return false;  // §9.4 single instance: the dialog keeps its first planet
    GuiDialog* d = open(dlg::kPlanetInfo);
    if (!d) return false;
    planet_info_ = p.planet;
    d->set_text("IDC_STATIC_PLANET_NAME", text("TEXT_BUTTON_PLANET_INFO", U"Planet Info") + U" " + p.name);
    if (GuiControl* c = d->get("IDC_STATIC_PLANET_NAME")) c->tint = p.name_color;
    // Paragraph items ('\n' splits), one colour per list, word-wrapped to the list width, scrolled to the top.
    auto list = [&](const char* id, const std::vector<std::u32string>& paragraphs, Rgba8 color) {
        gui_->set_paragraphs(*d, id, paragraphs, color, kPlanetListWidth);
    };
    list("IDC_LIST_TACTICAL", p.tactical, kPlanetInfoTacticalColor);
    list("IDC_LIST_ADVANTAGE", p.advantages, Rgba8{0, 0, 0, 0});
    list("IDC_LIST_REFERENCE", p.history, Rgba8{0, 0, 0, 0});
    d->set_text("IDC_STATIC_TERRAIN", p.terrain);
    d->set_text("IDC_STATIC_WEATHER", p.weather);
    d->set_text("IDC_STATIC_NATIVE", p.natives);
    const std::string overlay = p.planet_overlay.empty() ? std::string("load_overlay.tga") : p.planet_overlay;
    const std::pair<const char*, const std::string*> images[] = {{"IDC_PLANET_ICON", &p.planet_icon},
                                                                 {"IDC_PLANET_OVERLAY", &overlay},
                                                                 {"IDC_WEATHER_ICON", &p.weather_icon},
                                                                 {"IDC_MAP_ICON", &p.map_icon},
                                                                 {"IDC_NATIVE_ICON", &p.native_icon}};
    for (const auto& [id, tex] : images) {
        d->set_image(id, *tex);
        d->show(id, !tex->empty());
    }
    d->focus("IDC_CLOSE_BUTTON");
    return true;
}

bool GameScreens::show_auto_resolve(const AutoResolveInfo& info) {
    GuiDialog* d = open(dlg::kAutoResolve);
    if (!d) return false;
    d->set_items("IDC_INVADING_FORCES_LIST", info.local_side ? info.invaders : std::vector<std::u32string>{}, -1);
    d->set_items("IDC_DEFENDING_FORCES_LIST", info.local_side ? info.defenders : std::vector<std::u32string>{}, -1);
    d->enable("IDC_WITHDRAW_BUTTON", info.local_side);  // P4_SIM_SYSTEMS §9.2 enable rule
    d->set_image("IDC_INVADERL_ICON", info.invader_icon);
    d->set_image("IDC_DEFENDER_ICON", info.defender_icon);
    d->show("IDC_INVADERL_ICON", !info.invader_icon.empty());
    d->show("IDC_DEFENDER_ICON", !info.defender_icon.empty());
    d->show("IDC_ADVANCED_RESOLVE_INFO", false);  // [port] advanced info not modelled
    set_auto_resolve_progress(0.0f, 0.0f);
    return true;
}

void GameScreens::set_auto_resolve_progress(float invader, float defender) {
    GuiDialog* d = gui_ ? gui_->find(dlg::kAutoResolve) : nullptr;
    if (!d) return;
    d->set_value("IDC_AUTO_RESOLVE_PROGRESS_INVADER", static_cast<int32_t>(std::lround(std::clamp(invader, 0.0f, 1.0f) * 100.0f)));
    d->set_value("IDC_AUTO_RESOLVE_PROGRESS_DEFENDER", static_cast<int32_t>(std::lround(std::clamp(defender, 0.0f, 1.0f) * 100.0f)));
}

// ---- select map -------------------------------------------------------------------------------------------
bool GameScreens::show_select_map(std::span<const SkirmishMapInfo> maps) {
    GuiDialog* d = open(dlg::kSelectMap);
    if (!d) return false;
    maps_.assign(maps.begin(), maps.end());
    std::vector<std::u32string> items;
    for (const SkirmishMapInfo& m : maps_) items.push_back(m.label.empty() ? ascii32(m.map) : m.label);
    d->set_items("IDC_SAVED_MAPS_LIST", std::move(items), maps_.empty() ? -1 : 0);
    return true;
}

// ---- events -----------------------------------------------------------------------------------------------
void GameScreens::on_click(const GuiEvent& e, std::vector<ScreenAction>& out, std::vector<commands::EventData>& cmds) {
    const std::string_view n = e.dialog, c = e.control;
    auto is = [&](std::string_view id) { return iequal(c, id); };
    GuiDialog* d = gui_->find(n);
    if (!msg_dialog_.empty() && iequal(n, msg_dialog_)) {
        for (size_t i = 0; i < msg_buttons_.size(); ++i) {
            if (!is(msg_buttons_[i])) continue;
            ScreenAction a = act(ScreenAction::Kind::MessageBoxResult);
            a.tag = msg_tag_;
            a.button = static_cast<int32_t>(i);
            close(std::string(msg_dialog_));
            out.push_back(std::move(a));
            return;
        }
        return;
    }
    if (iequal(n, dlg::kBattleEnd)) {
        if (is("IDC_CONTINUE_BUTTON")) {
            gui_->close(n);
            out.push_back(act(ScreenAction::Kind::BattleEndContinue));
        } else if (is("IDC_LAND_INVADE_BUTTON")) {
            gui_->close(n);
            out.push_back(act(ScreenAction::Kind::LandInvade));
        } else if (is("IDC_SAVE_REPLAY_BUTTON")) {
            out.push_back(act(ScreenAction::Kind::SaveReplay));
        }
    } else if (iequal(n, dlg::kCampaignScore)) {
        if (is("IDC_QUIT_BUTTON")) {
            gui_->close(n);
            out.push_back(act(ScreenAction::Kind::CampaignScoreClosed));
        }
    } else if (iequal(n, dlg::kRetreat)) {
        if (is("IDC_RETREAT")) {
            gui_->close(n);
            cmds.push_back(commands::ev::retreat(true));
            out.push_back(act(ScreenAction::Kind::RetreatConfirmed));
        } else if (is("IDC_AUTORESOLVE")) {
            gui_->close(n);
            out.push_back(act(ScreenAction::Kind::AutoResolve));
        } else if (is("IDC_RETRY_CANCEL")) {
            gui_->close(n);
            out.push_back(act(ScreenAction::Kind::RetreatCancelled));
        }
    } else if (iequal(n, dlg::kPlanetInfo)) {
        if (is("IDC_CLOSE_BUTTON")) {
            gui_->close(n);
            ScreenAction a = act(ScreenAction::Kind::PlanetInfoClosed);
            a.object = planet_info_;
            out.push_back(std::move(a));
        }
    } else if (iequal(n, dlg::kAutoResolve)) {
        const GuiControl* w = d ? d->get("IDC_WITHDRAW_BUTTON") : nullptr;
        if (is("IDC_WITHDRAW_BUTTON") && (!w || w->enabled)) out.push_back(act(ScreenAction::Kind::Withdraw));
    } else if (iequal(n, dlg::kSelectMap) && d) {
        if (is("IDC_LOAD_MAP_BUTTON")) {
            const GuiControl* l = d->get("IDC_SAVED_MAPS_LIST");
            if (l && l->selected >= 0 && static_cast<size_t>(l->selected) < maps_.size()) {
                ScreenAction a = act(ScreenAction::Kind::MapSelected);
                a.map = maps_[static_cast<size_t>(l->selected)].map;
                gui_->close(n);
                out.push_back(std::move(a));
            }
        } else if (is("IDC_LOAD_CANCEL_BUTTON")) {
            gui_->close(n);
            out.push_back(act(ScreenAction::Kind::MapSelectCancelled));
        }
    }
}

void GameScreens::on_cancel(const GuiEvent& e, std::vector<ScreenAction>& out) {
    const std::string_view n = e.dialog;
    if (!msg_dialog_.empty() && iequal(n, msg_dialog_)) {
        ScreenAction a = act(ScreenAction::Kind::MessageBoxResult);
        a.tag = msg_tag_;
        a.button = -1;
        close(std::string(msg_dialog_));
        out.push_back(std::move(a));
    } else if (iequal(n, dlg::kRetreat)) {
        gui_->close(n);
        out.push_back(act(ScreenAction::Kind::RetreatCancelled));
    } else if (iequal(n, dlg::kChat)) {
        gui_->close(n);
        out.push_back(act(ScreenAction::Kind::ChatClosed));
    } else if (iequal(n, dlg::kPlanetInfo)) {
        gui_->close(n);
        ScreenAction a = act(ScreenAction::Kind::PlanetInfoClosed);
        a.object = planet_info_;
        out.push_back(std::move(a));
    } else if (iequal(n, dlg::kSelectMap)) {
        gui_->close(n);
        out.push_back(act(ScreenAction::Kind::MapSelectCancelled));
    }
    // Battle end, campaign score and auto-resolve ignore Esc (the player must choose) [port].
}

void GameScreens::update(std::span<const GuiEvent> events, std::vector<ScreenAction>& out,
                         std::vector<commands::EventData>& cmds) {
    if (!gui_) return;
    for (const GuiEvent& e : events) {
        switch (e.kind) {
            case GuiEvent::Kind::Clicked: on_click(e, out, cmds); break;
            case GuiEvent::Kind::Cancel: on_cancel(e, out); break;
            case GuiEvent::Kind::Submitted:
                if (iequal(e.dialog, dlg::kChat)) {
                    const GuiDialog* d = gui_->find(e.dialog);
                    ScreenAction a = act(ScreenAction::Kind::SendChat);
                    a.text = e.text;
                    if (a.text.empty() && d)
                        if (const GuiControl* ed = d->get("IDC_IME_EDITBOX")) a.text = ed->text;
                    if (d)
                        if (const GuiControl* t = d->get("IDC_TEAM_MEMBERS_ONLY")) a.team_only = t->checked;
                    gui_->close(e.dialog);
                    if (!a.text.empty()) out.push_back(std::move(a));
                    else out.push_back(act(ScreenAction::Kind::ChatClosed));
                }
                break;
            case GuiEvent::Kind::ValueChanged:
                if (iequal(e.dialog, dlg::kBattleEnd)) {
                    const bool enemy = iequal(e.control, "IDC_ENEMY_SCROLLBAR");
                    if (!enemy && !iequal(e.control, "IDC_YOUR_SCROLLBAR")) break;
                    const size_t n = enemy ? end_.enemy_losses.size() : end_.your_losses.size();
                    const int32_t extra = n > static_cast<size_t>(kLossRows) ? static_cast<int32_t>(n) - kLossRows : 0;
                    end_off_[enemy ? 1 : 0] = static_cast<int32_t>(std::lround(std::clamp(e.value, 0, 100) / 100.0 * extra));
                    if (GuiDialog* d = gui_->find(e.dialog)) fill_battle_end_units(*d);
                }
                break;
            case GuiEvent::Kind::DoubleClicked:
                if (iequal(e.dialog, dlg::kSelectMap)) {
                    GuiEvent b = e;
                    b.kind = GuiEvent::Kind::Clicked;
                    b.control = "IDC_LOAD_MAP_BUTTON";
                    on_click(b, out, cmds);
                }
                break;
            default: break;
        }
    }
}

}  // namespace alamo::ui
