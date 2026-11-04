#include "alamo/ui/gui_info_screens.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

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
bool istarts(std::string_view s, std::string_view p) { return s.size() >= p.size() && iequal(s.substr(0, p.size()), p); }
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
std::string id_nn(const char* fmt, int32_t a, int32_t b2) {
    char b[48];
    std::snprintf(b, sizeof b, fmt, a, b2);
    return b;
}
ScreenAction act(ScreenAction::Kind k) {
    ScreenAction a;
    a.kind = k;
    return a;
}
// Shows an image control with `tex`, hides it when empty.
void image(GuiDialog& d, const std::string& id, const std::string& tex) {
    d.set_image(id, tex);
    d.show(id, !tex.empty());
}

constexpr int32_t kProductionRows = 6, kFleetRows = 10, kEconomyRows = 6;
constexpr const char* kTabs[4] = {"IDC_DROID_LOG_RADIO", "IDC_DATA_CHART_RADIO", "IDC_TECH_TREE_RADIO", "IDC_SUMMARY_SCREEN_RADIO"};

bool is_info_dialog(std::string_view n) {
    return iequal(n, dlg::kDroidLog) || iequal(n, dlg::kDataChart) || istarts(n, "IDD_TECH_TREE_") ||
           istarts(n, "IDD_SUMMARY_SCREEN_");
}

}  // namespace

std::string tech_tree_dialog(TechFaction f, bool space) {
    std::string n = f == TechFaction::Rebel ? "IDD_TECH_TREE_REBEL" : f == TechFaction::Empire ? "IDD_TECH_TREE_EMPIRE" : "IDD_TECH_TREE_BLACKMARKET";
    if (space) n += "_SPACE";
    return n;
}

std::u32string InfoScreens::text(std::string_view key, std::u32string_view fallback) const {
    if (gui_ && gui_->text_db() && gui_->text_db()->find(key)) return gui_->caption_text(key);
    return std::u32string(fallback);
}

void InfoScreens::close_all() {
    if (!gui_) return;
    for (const char* n : {dlg::kBuyUnits, dlg::kStory, dlg::kStorySummary, dlg::kPlanetDialog}) gui_->close(n);
    if (!info_open_.empty()) gui_->close(info_open_);
    info_open_.clear();
}

// ---- skirmish buy units -------------------------------------------------------------------------------------------
void InfoScreens::fill_buy(GuiDialog& d) {
    std::vector<std::u32string> rows;
    for (const BuyUnitEntry& u : buy_.units) {
        std::u32string r = u.name + U"  " + num32(u.cost);
        if (u.owned > 0) r += U"  x" + num32(u.owned);
        rows.push_back(std::move(r));
    }
    const GuiControl* l = d.get("IDC_SKIRMISH_BUY_FORCES_LISTBOX");
    const int32_t sel = l && l->selected >= 0 && static_cast<size_t>(l->selected) < rows.size() ? l->selected : (rows.empty() ? -1 : 0);
    d.set_items("IDC_SKIRMISH_BUY_FORCES_LISTBOX", std::move(rows), sel);
    d.set_text("IDC_SKIRMISH_BUY_UNITS_CREDITS_TEXT", text("TEXT_SKIRMISH_BUY_UNITS_CREDITS_BALANCE", U"Credits:") + U" " + num32(buy_.credits));
    const BuyUnitEntry* u = sel >= 0 ? &buy_.units[static_cast<size_t>(sel)] : nullptr;
    image(d, "IDC_SKIRMISH_BUY_MENU_UNIT_IMAGE", u ? u->icon : std::string());
    d.enable("IDC_BUY_BUTTON", u && u->can_buy && buy_.credits >= u->cost);
    d.enable("IDC_SELL_BUTTON", u && u->owned > 0);
    bool any = false;
    for (const BuyUnitEntry& e : buy_.units) any = any || e.owned > 0;
    d.enable("IDC_SELL_ALL_BUTTON", any);
}

bool InfoScreens::show_buy_units(const BuyUnitsState& s) {
    GuiDialog* d = gui_ ? gui_->open(dlg::kBuyUnits) : nullptr;
    if (!d) return false;
    buy_ = s;
    fill_buy(*d);
    return true;
}

void InfoScreens::set_buy_units(const BuyUnitsState& s) {
    buy_ = s;
    if (GuiDialog* d = gui_ ? gui_->find(dlg::kBuyUnits) : nullptr) fill_buy(*d);
}

// ---- story --------------------------------------------------------------------------------------------------------
bool InfoScreens::show_story(const StoryDialogInfo& s) {
    GuiDialog* d = gui_ ? gui_->open(dlg::kStory) : nullptr;
    if (!d) return false;
    story_tag_ = s.tag;
    if (!s.title.empty())  // the title is a CTEXT with IDC_STATIC: set the first static text
        for (size_t i = 0; i < d->size(); ++i)
            if ((*d)[i].type == GuiGadgetType::Text) {
                (*d)[i].text = s.title;
                break;
            }
    // [port: Gabriel playtest] the title's Global_Default dark gold (195,171,2) is unreadable on the dialog's gold top
    // art: brighter HUD gold (CommandBarComponents Text_Color 255,212,33) with an outline.
    for (size_t i = 0; i < d->size(); ++i)
        if ((*d)[i].type == GuiGadgetType::Text) {
            (*d)[i].tint = Rgba8{255, 212, 33, 255};
            (*d)[i].text_outline = true;
            break;
        }
    story_ = s;
    story_t0_ = -1.0;
    story_cue_ = 0;
    // Lines without a time (or at 0) show at once; the rest wait for update_story.
    story_shown_ = 0;
    while (story_shown_ < s.lines.size() && (story_shown_ >= s.line_at.size() || s.line_at[story_shown_] <= 0.0)) ++story_shown_;
    fill_story_lines(*d);
    image(*d, "IDC_IMAGE", s.image);
    d->show("IDC_STORY_DIALOG_ACCEPT", s.accept);
    d->show("IDC_STORY_DIALOG_CANCEL", s.cancel);
    d->show("IDC_STORY_DIALOG_CLOSE_BUTTON", s.close);
    return true;
}

void InfoScreens::fill_story_lines(GuiDialog& d) {
    const size_t n = std::min(story_shown_, story_.lines.size());
    d.set_items("IDC_STORY_TEXT", std::vector<std::u32string>(story_.lines.begin(), story_.lines.begin() + static_cast<std::ptrdiff_t>(n)), -1);
    if (GuiControl* c = d.get("IDC_STORY_TEXT")) {
        c->item_text_colors.assign(story_.line_colors.begin(), story_.line_colors.begin() + static_cast<std::ptrdiff_t>(std::min(n, story_.line_colors.size())));
        if (gui_) {
            gui_->wrap_list_items(d, "IDC_STORY_TEXT");  // [port: Gabriel] paragraphs wrap inside the list
            gui_->scroll_to_end(d, "IDC_STORY_TEXT");
        }
    }
}

bool InfoScreens::update_story(double now, std::vector<StoryDialogCue>* due) {
    GuiDialog* d = gui_ ? gui_->find(dlg::kStory) : nullptr;
    if (!d) return false;
    if (story_t0_ < 0.0) story_t0_ = now;
    const double t = now - story_t0_;
    const size_t before = story_shown_;
    while (story_shown_ < story_.lines.size() && (story_shown_ >= story_.line_at.size() || story_.line_at[story_shown_] <= t)) ++story_shown_;
    if (story_shown_ != before) fill_story_lines(*d);
    while (story_cue_ < story_.cues.size() && story_.cues[story_cue_].at <= t) {
        if (due) due->push_back(story_.cues[story_cue_]);
        ++story_cue_;
    }
    return story_shown_ < story_.lines.size() || story_cue_ < story_.cues.size();
}

bool InfoScreens::show_story_summary(const StorySummaryInfo& s) {
    GuiDialog* d = gui_ ? gui_->open(dlg::kStorySummary) : nullptr;
    if (!d) return false;
    const int32_t n = static_cast<int32_t>(s.goals.size());
    const int32_t sel = n == 0 ? -1 : (s.selected >= 0 && s.selected < n ? s.selected : n - 1);
    d->set_items("IDC_GOAL_LIST", s.goals, sel);
    if (GuiControl* c = d->get("IDC_GOAL_LIST"))  // §2.8 row colours (read / completed rows dimmed)
        c->item_text_colors.assign(s.goal_colors.begin(),
                                   s.goal_colors.begin() + static_cast<std::ptrdiff_t>(std::min(s.goals.size(), s.goal_colors.size())));
    d->set_items("IDC_STORY_RECAP", s.recap, -1);
    if (GuiControl* c = d->get("IDC_STORY_RECAP")) {
        c->item_text_colors.assign(s.recap_colors.begin(), s.recap_colors.begin() + static_cast<std::ptrdiff_t>(std::min(s.recap.size(), s.recap_colors.size())));
        gui_->wrap_list_items(*d, "IDC_STORY_RECAP");  // [port] paragraphs wrap inside the list (as the story dialog)
    }
    // §2.2: the IDC_STATIC above the recap (placeholder TEXT_STORY_GOALS, not the dialog title IDC_PLANET_REFERENCE_TITLE)
    // shows the chapter's TITLE line.
    if (!s.title.empty())
        for (size_t i = 0; i < d->size(); ++i) {
            GuiControl& c = (*d)[i];
            if (c.def && c.def->id == "IDC_STATIC" && c.def->caption == "TEXT_STORY_GOALS") {
                c.text = s.title;
                break;
            }
        }
    d->enable("IDC_RADIO_COMPLETED", s.has_completed || s.completed);
    d->set_checked(s.completed ? "IDC_RADIO_COMPLETED" : "IDC_RADIO_ACTIVE", true);
    d->enable("IDC_SUMMARY_DIALOG_ABANDON", false);  // dead in retail (P4_SIM_SYSTEMS §9.1); 23.jpg shows none
    d->show("IDC_SUMMARY_DIALOG_ABANDON", false);
    return true;
}

// ---- planet dialog --------------------------------------------------------------------------------------------------
bool InfoScreens::show_planet(const PlanetDialogInfo& p) {
    GuiDialog* d = gui_ ? gui_->open(dlg::kPlanetDialog) : nullptr;
    if (!d) return false;
    planet_ = p.planet;
    d->set_text("IDC_PLANET_MENU_PLANET_NAME", p.name);
    image(*d, "IDC_PLANET_MENU_PLANET_IMAGE", p.image);
    d->set_text("IDC_TEXT_PLANET_CREDITS", p.credits >= 0 ? U"+" + num32(p.credits) : std::u32string());
    static const std::vector<std::string> kHidden;
    const std::pair<const char*, const std::vector<std::string>*> rows[3] = {
        {"IDC_POLITICAL_ICON_%d", &p.political},
        {"IDC_STARBASE_ICON_%d", p.starbase_info ? &p.starbase : &kHidden},
        {"IDC_GROUND_BASE_ICON_%d", p.groundbase_info ? &p.ground_base : &kHidden}};
    for (const auto& [fmt, list] : rows)
        for (int32_t k = 0; k < 5; ++k) image(*d, id_n(fmt, k), static_cast<size_t>(k) < list->size() ? (*list)[static_cast<size_t>(k)] : std::string());
    image(*d, "IDC_GROUND_COMPANIES_ICON", p.companies_icon);
    image(*d, "IDC_FLEETS_ICON", p.fleets_icon);
    d->set_text("IDC_TEXT_PLANET_COMPANY_COUNT", p.companies >= 0 ? num32(p.companies) : std::u32string());
    d->set_text("IDC_TEXT_PLANET_FLEET_COUNT", p.fleets >= 0 ? num32(p.fleets) : std::u32string());
    d->set_text("IDC_TEXT_PRODUCTION_OBJECT_BUILDING", p.producing);
    d->show("IDC_PRODUCTION_MENU_PROGRESS", p.progress >= 0.0f);
    d->set_value("IDC_PRODUCTION_MENU_PROGRESS", static_cast<int32_t>(std::lround(std::clamp(p.progress, 0.0f, 1.0f) * 100.0f)));
    return true;
}

// ---- holocron pages -------------------------------------------------------------------------------------------------
std::string InfoScreens::info_dialog() const { return info_open_; }

GuiDialog* InfoScreens::go_info(const std::string& name) {
    if (!gui_) return nullptr;
    if (!info_open_.empty() && !iequal(info_open_, name)) gui_->close(info_open_);
    GuiDialog* d = gui_->open(name);
    info_open_ = d ? name : std::string();
    return d;
}

void InfoScreens::fill_tabs(GuiDialog& d) {
    const int32_t tab = page_ == InfoPage::DroidLog ? 0 : page_ == InfoPage::DataChart ? 1 : page_ == InfoPage::TechTree ? 2 : 3;
    d.set_checked(kTabs[tab], true);
    if (page_ == InfoPage::SummaryProduction) d.set_checked("IDC_PRODUCTION_RADIO", true);
    if (page_ == InfoPage::SummaryFleets) d.set_checked("IDC_FLEETS_RADIO", true);
    if (page_ == InfoPage::SummaryEconomy) d.set_checked("IDC_ECONOMY_RADIO", true);
}

void InfoScreens::fill_droid(GuiDialog& d) {
    d.set_items("IDC_DROID_LIST", droid_.entries, -1);
    image(d, "IDC_DROID_PICTURE_", droid_.picture);
    d.set_checked("IDC_HINTS_CHECK", droid_.hints);
    for (int32_t k = 0; k < 25; ++k) {
        const bool has = static_cast<size_t>(k) < droid_.legend.size();
        image(d, id_n("IDC_PA_ICON%d", k), has ? droid_.legend[static_cast<size_t>(k)].first : std::string());
        d.show(id_n("IDC_PA_TEXT%d", k), has);
        if (has) d.set_text(id_n("IDC_PA_TEXT%d", k), droid_.legend[static_cast<size_t>(k)].second);
    }
}

void InfoScreens::fill_chart(GuiDialog& d) {
    const std::vector<ChartSeries>& s = chart_mode_ == 0 ? chart_.economy : chart_mode_ == 1 ? chart_.military : chart_.planets;
    const char* radios[3] = {"IDC_RADIO_ECONOMY", "IDC_RADIO_MILITARY", "IDC_RADIO_PLANETS"};
    d.set_checked(radios[std::clamp(chart_mode_, 0, 2)], true);
    float mx = 0.0f;
    size_t n = 0;
    for (const ChartSeries& c : s) {
        for (float v : c.values) mx = std::max(mx, v);
        n = std::max(n, c.values.size());
    }
    d.set_text("IDC_Y_MAX", num32(static_cast<int64_t>(std::lround(mx))));
    d.set_text("IDC_Y_MIN", U"0");
    d.set_text("IDC_Y_CAPTION", chart_mode_ == 0 ? chart_.economy_caption : chart_mode_ == 1 ? chart_.military_caption : chart_.planets_caption);
    const int32_t last = chart_.first_cycle + static_cast<int32_t>(n > 0 ? n - 1 : 0);
    d.set_text("IDC_STATIC_START", num32(chart_.first_cycle));
    d.set_text("IDC_STATIC_MIDDLE", num32((chart_.first_cycle + last) / 2));
    d.set_text("IDC_STATIC_END", num32(last));
}

void InfoScreens::fill_tech(GuiDialog& d) {
    d.set_checked(tech_.space ? "IDC_TECH_TREE_SPACE" : "IDC_TECH_TREE_LAND", true);
    // UI.md §3.7.2: every visible arrow image n = 0..99 gets its art, white (no per-state tint); the rectangle comes from the .rc.
    struct ArrowArt {
        const char* fmt;
        const char* texture;
    };
    static constexpr ArrowArt kArrows[] = {{"IDC_RIGHT_ARROW%d", "i_tree_arrow_right.tga"},
                                           {"IDC_DRIGHT_ARROW%d", "i_tree_arrow_2right.tga"},
                                           {"IDC_RRIGHT_ARROW%d", "i_tree_arrow_3right.tga"},
                                           {"IDC_DOWN_ARROW%d", "i_tree_arrow_down.tga"}};
    for (const ArrowArt& a : kArrows)
        for (int32_t n = 0; n < 100; ++n)
            if (GuiControl* c = d.get(id_n(a.fmt, n)); c && c->visible) {
                c->image = a.texture;
                c->tint = Rgba8{255, 255, 255, 255};
            }
    for (size_t i = 0; i < d.size(); ++i) {
        GuiControl& c = d[i];
        const std::string_view id = c.id();
        if (!istarts(id, "IDC_TECHTREE_")) continue;
        TechTreeItem it;
        const bool has = tech_.item && tech_.item(id.substr(13), it) && !it.icon.empty();
        c.visible = has;
        if (!has) continue;
        c.image = it.icon;
        c.tint = it.state == TechState::Locked ? Rgba8{90, 90, 90, 255} : Rgba8{255, 255, 255, 255};  // no "researched" colour
    }
}

void InfoScreens::fill_summary(GuiDialog& d) {
    auto set = [&](const std::string& id, const std::u32string& t) { d.set_text(id, t); };
    if (page_ == InfoPage::SummaryProduction) {
        const auto& rows = summary_.production;
        for (int32_t r = 0; r < kProductionRows; ++r) {
            const auto k = static_cast<size_t>(summary_off_ + r);
            const SummaryProductionRow* row = k < rows.size() ? &rows[k] : nullptr;
            image(d, id_n("IDC_PLANET_ICON%d", r), row ? row->icon : std::string());
            d.show(id_n("IDC_PLANET_NAME%d", r), row != nullptr);
            d.show(id_n("IDC_PER_SP_STATIC%d", r), row != nullptr);
            d.show(id_n("IDC_PER_GP_STATIC%d", r), row != nullptr);
            d.show(id_n("IDC_POP_CAP_%d", r), row != nullptr);
            image(d, id_n("IDC_SP_ICON%d", r), row ? row->space_icon : std::string());
            image(d, id_n("IDC_GP_ICON%d", r), row ? row->ground_icon : std::string());
            for (int32_t h = 0; h < 8; ++h)
                image(d, id_nn("IDC_HERO%d_ICON%d", r, h), row && static_cast<size_t>(h) < row->heroes.size() ? row->heroes[static_cast<size_t>(h)] : std::string());
            if (!row) continue;
            set(id_n("IDC_PLANET_NAME%d", r), row->name);
            set(id_n("IDC_PER_SP_STATIC%d", r), row->space_text);
            set(id_n("IDC_PER_GP_STATIC%d", r), row->ground_text);
            set(id_n("IDC_POP_CAP_%d", r), row->pop);
        }
        set("IDC_POP_TOTAL_STATIC", summary_.pop_total);
    } else if (page_ == InfoPage::SummaryFleets) {
        const auto& rows = summary_.fleets;
        for (int32_t r = 0; r < kFleetRows; ++r) {
            const auto k = static_cast<size_t>(summary_off_ + r);
            const SummaryFleetRow* row = k < rows.size() ? &rows[k] : nullptr;
            image(d, id_n("IDC_PLANET_ICON%d", r), row ? row->icon : std::string());
            d.show(id_n("IDC_PLANET_NAME%d", r), row != nullptr);
            if (row) set(id_n("IDC_PLANET_NAME%d", r), row->name);
            for (int32_t f = 0; f < 15; ++f)
                image(d, id_nn("IDC_ROW%d_FLEET%d", r, f), row && static_cast<size_t>(f) < row->fleets.size() ? row->fleets[static_cast<size_t>(f)] : std::string());
            if (r >= 1) {  // UI.md §3.7.2: the divider above a filled row is the right arrow, white; hidden with the row
                const std::string div = id_n("IDC_HEROES_DIVIDERS%d", r - 1);
                image(d, div, row ? std::string("i_tree_arrow_right.tga") : std::string());
                if (GuiControl* dc = d.get(div)) dc->tint = Rgba8{255, 255, 255, 255};
            }
        }
    } else if (page_ == InfoPage::SummaryEconomy) {
        const auto& rows = summary_.economy;
        for (int32_t r = 0; r < kEconomyRows; ++r) {
            const auto k = static_cast<size_t>(summary_off_ + r);
            const SummaryEconomyRow* row = k < rows.size() ? &rows[k] : nullptr;
            image(d, id_n("IDC_PLANET_ICON%d", r), row ? row->icon : std::string());
            image(d, id_n("IDC_MINING_FACILITY_ICON_%d", r), row && row->mining_count > 0 ? row->mining_icon : std::string());
            for (const char* f : {"IDC_PLANET_NAME%d", "IDC_TOTAL_INCOME_%d", "IDC_BASE_CREDIT_VALUE_%d", "IDC_TRADE_ROUTE_INCOME_%d",
                                  "IDC_MINING_FACILITY_INCOME_%d"})
                d.show(id_n(f, r), row != nullptr);
            d.show(id_n("IDC_NUM_MINING_FACILITIES_STATIC_%d", r), row && row->mining_count > 0);
            if (!row) continue;
            set(id_n("IDC_PLANET_NAME%d", r), row->name);
            set(id_n("IDC_TOTAL_INCOME_%d", r), row->total);
            set(id_n("IDC_BASE_CREDIT_VALUE_%d", r), row->base);
            set(id_n("IDC_TRADE_ROUTE_INCOME_%d", r), row->trade);
            set(id_n("IDC_MINING_FACILITY_INCOME_%d", r), row->mining);
            set(id_n("IDC_NUM_MINING_FACILITIES_STATIC_%d", r), U"x" + num32(row->mining_count));
        }
        set("IDC_TOTAL_INCOME_TOTAL", summary_.total_income);
        set("IDC_BASE_CREDIT_VALUE_TOTAL", summary_.total_base);
        set("IDC_TRADE_ROUTE_INCOME_TOTAL", summary_.total_trade);
        set("IDC_MINING_FACILITIES_INCOME_TOTAL", summary_.total_mining);
    }
}

bool InfoScreens::show_info(InfoPage page) {
    page_ = page;
    std::string name;
    switch (page) {
        case InfoPage::DroidLog: name = dlg::kDroidLog; break;
        case InfoPage::DataChart: name = dlg::kDataChart; break;
        case InfoPage::TechTree: name = tech_tree_dialog(tech_.faction, tech_.space); break;
        case InfoPage::SummaryProduction: name = dlg::kSummaryProduction; break;
        case InfoPage::SummaryFleets: name = dlg::kSummaryFleets; break;
        case InfoPage::SummaryEconomy: name = dlg::kSummaryEconomy; break;
    }
    GuiDialog* d = go_info(name);
    if (!d) return false;
    summary_off_ = 0;
    fill_tabs(*d);
    switch (page) {
        case InfoPage::DroidLog: fill_droid(*d); break;
        case InfoPage::DataChart: fill_chart(*d); break;
        case InfoPage::TechTree: fill_tech(*d); break;
        default:
            d->set_value("IDC_SCROLLBAR_SLIDER", 0);
            fill_summary(*d);
            break;
    }
    return true;
}

size_t InfoScreens::build_overlay(HudDrawList& dl) const {
    if (!gui_ || page_ != InfoPage::DataChart || info_open_.empty()) return 0;
    const GuiDialog* d = gui_->find(info_open_);
    if (!d) return 0;
    const int32_t ci = d->find("IDC_DATA_CHART_IMAGE");
    if (ci < 0) return 0;
    const GuiRect r = d->control_rect(static_cast<size_t>(ci));
    const std::vector<std::string> names = gui_->open_names();
    uint32_t layer = 0;
    for (size_t k = 0; k < names.size(); ++k)
        if (iequal(names[k], info_open_)) layer = static_cast<uint32_t>(std::min<size_t>(k, 11));
    const std::vector<ChartSeries>& s = chart_mode_ == 0 ? chart_.economy : chart_mode_ == 1 ? chart_.military : chart_.planets;
    float mx = 0.0f;
    size_t n = 0;
    for (const ChartSeries& c : s) {
        for (float v : c.values) mx = std::max(mx, v);
        n = std::max(n, c.values.size());
    }
    if (n < 2 || mx <= 0.0f) return 0;
    size_t dots = 0;
    auto at = [&](size_t i, float v) {
        return Vec2f{r.min.x + r.width() * static_cast<float>(i) / static_cast<float>(n - 1), r.min.y + r.height() * std::clamp(v / mx, 0.0f, 1.0f)};
    };
    for (const ChartSeries& c : s) {
        for (size_t i = 1; i < c.values.size(); ++i) {
            const Vec2f a = at(i - 1, c.values[i - 1]), b = at(i, c.values[i]);
            const float len = std::hypot(b.x - a.x, b.y - a.y);
            const auto steps = static_cast<int32_t>(std::max(1.0f, std::ceil(len / 2.0f)));
            for (int32_t k = 0; k <= steps; ++k) {
                const float t = static_cast<float>(k) / static_cast<float>(steps);
                HudQuad q;
                q.min = {a.x + (b.x - a.x) * t - 1.0f, a.y + (b.y - a.y) * t - 1.0f};
                q.max = {q.min.x + 2.0f, q.min.y + 2.0f};
                q.color = c.color;
                dl.add(layer, HudList::Alpha, q);
                ++dots;
            }
        }
    }
    return dots;
}

// ---- events ---------------------------------------------------------------------------------------------------------
void InfoScreens::on_click(const GuiEvent& e, std::vector<ScreenAction>& out) {
    const std::string_view n = e.dialog, c = e.control;
    auto is = [&](std::string_view id) { return iequal(c, id); };
    GuiDialog* d = gui_->find(n);
    if (!d) return;
    if (iequal(n, dlg::kBuyUnits)) {
        const GuiControl* l = d->get("IDC_SKIRMISH_BUY_FORCES_LISTBOX");
        const BuyUnitEntry* u = l && l->selected >= 0 && static_cast<size_t>(l->selected) < buy_.units.size()
                                    ? &buy_.units[static_cast<size_t>(l->selected)]
                                    : nullptr;
        if ((is("IDC_BUY_BUTTON") || is("IDC_SELL_BUTTON")) && u) {
            ScreenAction a = act(is("IDC_BUY_BUTTON") ? ScreenAction::Kind::BuyUnit : ScreenAction::Kind::SellUnit);
            a.map = u->type;
            out.push_back(std::move(a));
        } else if (is("IDC_SELL_ALL_BUTTON")) {
            out.push_back(act(ScreenAction::Kind::SellAllUnits));
        } else if (is("IDC_NEXT_BUTTON")) {
            gui_->close(n);
            out.push_back(act(ScreenAction::Kind::BuyUnitsDone));
        } else if (is("IDC_BACK_BUTTON")) {
            gui_->close(n);
            out.push_back(act(ScreenAction::Kind::BuyUnitsBack));
        }
    } else if (iequal(n, dlg::kStory)) {
        ScreenAction a;
        if (is("IDC_STORY_DIALOG_ACCEPT")) a.kind = ScreenAction::Kind::StoryAccepted;
        else if (is("IDC_STORY_DIALOG_CANCEL")) a.kind = ScreenAction::Kind::StoryCancelled;
        else if (is("IDC_STORY_DIALOG_CLOSE_BUTTON")) a.kind = ScreenAction::Kind::StoryClosed;
        else return;
        a.tag = story_tag_;
        gui_->close(n);
        out.push_back(std::move(a));
    } else if (iequal(n, dlg::kStorySummary)) {
        if (is("IDC_SUMMARY_DIALOG_CLOSE_BUTTON")) {
            gui_->close(n);
            out.push_back(act(ScreenAction::Kind::StorySummaryClosed));
        }  // IDC_SUMMARY_DIALOG_ABANDON: no handler (P4_SIM_SYSTEMS §9.1)
    } else if (iequal(n, dlg::kPlanetDialog)) {
        ScreenAction a;
        if (is("IDC_PLANET_ORGANIZATION_BUTTON")) a.kind = ScreenAction::Kind::PlanetOrganize;
        else if (is("IDC_PLANET_PRODUCTION_BUTTON")) a.kind = ScreenAction::Kind::PlanetProduction;
        else if (is("IDC_PLANET_EXIT_BUTTON")) {
            a.kind = ScreenAction::Kind::PlanetDialogClosed;
            gui_->close(n);
        } else {
            return;
        }
        a.object = planet_;
        out.push_back(std::move(a));
    } else if (is_info_dialog(n)) {
        if (is("IDC_BUTTON_CLOSE") || is("IDC_BUTTON_EXIT")) {
            gui_->close(n);
            info_open_.clear();
            out.push_back(act(ScreenAction::Kind::InfoClosed));
        } else if (is("IDC_BUTTON_PLANETARY")) {
            out.push_back(act(ScreenAction::Kind::SummaryPlanetary));
        } else if (istarts(c, "IDC_BUTTON_")) {
            ScreenAction a = act(ScreenAction::Kind::SummarySort);
            a.tag = std::string(c);
            out.push_back(std::move(a));
        } else {
            on_change(e, out);  // pushlike tab radios report clicks
        }
    }
}

void InfoScreens::on_change(const GuiEvent& e, std::vector<ScreenAction>& out) {
    const std::string_view n = e.dialog, c = e.control;
    auto is = [&](std::string_view id) { return iequal(c, id); };
    GuiDialog* d = gui_->find(n);
    if (!d) return;
    if (iequal(n, dlg::kBuyUnits) && is("IDC_SKIRMISH_BUY_FORCES_LISTBOX")) {
        if (e.kind == GuiEvent::Kind::DoubleClicked && e.value >= 0 && static_cast<size_t>(e.value) < buy_.units.size() &&
            buy_.units[static_cast<size_t>(e.value)].can_buy) {
            ScreenAction a = act(ScreenAction::Kind::BuyUnit);
            a.map = buy_.units[static_cast<size_t>(e.value)].type;
            out.push_back(std::move(a));
        }
        fill_buy(*d);
        return;
    }
    if (iequal(n, dlg::kStorySummary)) {
        if (is("IDC_GOAL_LIST") && e.kind == GuiEvent::Kind::SelectionChanged && e.value >= 0) {
            ScreenAction a = act(ScreenAction::Kind::StorySummaryGoal);
            a.button = e.value;
            out.push_back(std::move(a));
            return;
        }
        if ((is("IDC_RADIO_ACTIVE") || is("IDC_RADIO_COMPLETED")) && (e.kind != GuiEvent::Kind::Checked || e.value != 0)) {
            ScreenAction a = act(ScreenAction::Kind::StorySummaryFilter);
            a.button = is("IDC_RADIO_COMPLETED") ? 1 : 0;
            out.push_back(std::move(a));
        }
        return;
    }
    if (!is_info_dialog(n)) return;
    if (e.kind == GuiEvent::Kind::Checked && e.value == 0) return;  // radios unchecking
    if (is("IDC_HINTS_CHECK")) {
        droid_.hints = e.value != 0;
        ScreenAction a = act(ScreenAction::Kind::InfoHintsToggled);
        a.button = e.value != 0 ? 1 : 0;
        out.push_back(std::move(a));
        return;
    }
    if (is("IDC_SCROLLBAR_SLIDER") && e.kind == GuiEvent::Kind::ValueChanged) {
        const int32_t rows = page_ == InfoPage::SummaryProduction ? static_cast<int32_t>(summary_.production.size()) - kProductionRows
                             : page_ == InfoPage::SummaryFleets   ? static_cast<int32_t>(summary_.fleets.size()) - kFleetRows
                                                                  : static_cast<int32_t>(summary_.economy.size()) - kEconomyRows;
        summary_off_ = rows > 0 ? static_cast<int32_t>(std::lround(std::clamp(e.value, 0, 100) / 100.0 * rows)) : 0;
        fill_summary(*d);
        return;
    }
    const char* modes[3] = {"IDC_RADIO_ECONOMY", "IDC_RADIO_MILITARY", "IDC_RADIO_PLANETS"};
    for (int32_t m = 0; m < 3; ++m)
        if (is(modes[m])) {
            chart_mode_ = m;
            fill_chart(*d);
            return;
        }
    if ((is("IDC_TECH_TREE_LAND") || is("IDC_TECH_TREE_SPACE")) && tech_.space != is("IDC_TECH_TREE_SPACE")) {
        tech_.space = is("IDC_TECH_TREE_SPACE");
        show_info(InfoPage::TechTree);
        return;
    }
    InfoPage next = page_;
    bool change = true;
    if (is(kTabs[0])) next = InfoPage::DroidLog;
    else if (is(kTabs[1])) next = InfoPage::DataChart;
    else if (is(kTabs[2])) next = InfoPage::TechTree;
    else if (is(kTabs[3]) || is("IDC_PRODUCTION_RADIO")) next = is(kTabs[3]) && (page_ == InfoPage::SummaryFleets || page_ == InfoPage::SummaryEconomy) ? page_ : InfoPage::SummaryProduction;
    else if (is("IDC_FLEETS_RADIO")) next = InfoPage::SummaryFleets;
    else if (is("IDC_ECONOMY_RADIO")) next = InfoPage::SummaryEconomy;
    else change = false;
    if (!change || next == page_) return;
    if (show_info(next)) {
        ScreenAction a = act(ScreenAction::Kind::InfoPageChanged);
        a.button = static_cast<int32_t>(next);
        out.push_back(std::move(a));
    }
}

void InfoScreens::update(std::span<const GuiEvent> events, std::vector<ScreenAction>& out) {
    if (!gui_) return;
    for (const GuiEvent& e : events) {
        switch (e.kind) {
            case GuiEvent::Kind::Clicked: on_click(e, out); break;
            case GuiEvent::Kind::Checked:
            case GuiEvent::Kind::SelectionChanged:
            case GuiEvent::Kind::DoubleClicked:
            case GuiEvent::Kind::ValueChanged: on_change(e, out); break;
            case GuiEvent::Kind::Cancel:
                if (iequal(e.dialog, dlg::kBuyUnits)) {
                    gui_->close(e.dialog);
                    out.push_back(act(ScreenAction::Kind::BuyUnitsBack));
                } else if (iequal(e.dialog, dlg::kStory)) {
                    ScreenAction a = act(ScreenAction::Kind::StoryClosed);
                    a.tag = story_tag_;
                    gui_->close(e.dialog);
                    out.push_back(std::move(a));
                } else if (iequal(e.dialog, dlg::kStorySummary)) {
                    gui_->close(e.dialog);
                    out.push_back(act(ScreenAction::Kind::StorySummaryClosed));
                } else if (iequal(e.dialog, dlg::kPlanetDialog)) {
                    ScreenAction a = act(ScreenAction::Kind::PlanetDialogClosed);
                    a.object = planet_;
                    gui_->close(e.dialog);
                    out.push_back(std::move(a));
                } else if (is_info_dialog(e.dialog)) {
                    gui_->close(e.dialog);
                    info_open_.clear();
                    out.push_back(act(ScreenAction::Kind::InfoClosed));
                }
                break;
            default: break;
        }
    }
}

}  // namespace alamo::ui
