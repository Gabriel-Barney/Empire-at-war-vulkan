#include "game_info.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>

#include "alamo/core/ascii.hpp"
#include "alamo/galactic/campaign.hpp"
#include "alamo/galactic/conflicts.hpp"
#include "alamo/game/hud_feeds.hpp"
#include "alamo/game/story_script.hpp"
#include "alamo/tactical/abilities/ability_types.hpp"
#include "alamo/ui/hud_abilities.hpp"
#ifdef ALAMO_SIM_HAVE_STORY
#include "alamo/lua/story_model.hpp"
#include "alamo/sim/story_session.hpp"
#endif

namespace alamo::game {

namespace {

std::u32string u32(std::string_view s) { return ui::utf8_to_utf32(s); }

std::u32string number(double v, int decimals = 0) {
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", decimals, v);
    return u32(b);
}

// A text id resolved through the MasterTextFile, else the id itself.
std::u32string text_or(const ui::TextDatabase& text, std::string_view id) {
    if (const std::u16string* s = text.find(id)) return ui::utf16_to_utf32(*s);
    return u32(id);
}

// Whitespace / comma separated words.
std::vector<std::string> words(std::string_view s) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : s) {
        if (c == ' ' || c == ',' || c == '\t' || c == '\n' || c == '\r') {
            if (!cur.empty()) out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

std::string icon_of(std::span<const ui::HudTypeInfo> hud_types, objects::TypeIndex t) {
    return t < hud_types.size() ? hud_types[t].icon_name : std::string();
}

// t40: the button art of a type's Unit_Abilities_Data, list order; a later entry of a type replaces the earlier one, as on
// the object (AbilitySystem). Alternate_Icon_Name wins over the type's art, as on the command bar (t42).
std::vector<std::string> ability_icons(const GameData& d, objects::TypeIndex t) {
    const objects::ObjectTypeDatabase& raw = d.types.raw();
    const objects::FieldColumns& fc = raw.fields();
    std::vector<uint8_t> slots;
    std::vector<std::string> out;
    for (const objects::FieldIndex sub : d.types.get_sub_objects(t, "Unit_Abilities_Data")) {
        uint8_t type = 0;
        std::string alternate;
        for (objects::FieldIndex f = sub + 1; f < fc.size() && fc.depth[f] > fc.depth[sub]; ++f) {
            if (fc.parent[f] != sub) continue;
            if (ascii_iequals(raw.tag_name(f), "Type")) {
                for (const std::string& w : words(fc.text[f])) type = tactical::abilities::ability_type_from_name(w);
            } else if (ascii_iequals(raw.tag_name(f), "Alternate_Icon_Name")) {
                for (const std::string& w : words(fc.text[f])) alternate = w;
            }
        }
        if (type == 0) continue;
        std::string icon = alternate.empty() ? ui::ability_icon(type) : std::move(alternate);
        const auto it = std::find(slots.begin(), slots.end(), type);
        if (it != slots.end()) {
            out[static_cast<size_t>(it - slots.begin())] = std::move(icon);
        } else {
            slots.push_back(type);
            out.push_back(std::move(icon));
        }
    }
    return out;
}

}  // namespace

std::u32string type_label(const GameData& d, const ui::TextDatabase& text, objects::TypeIndex t) {
    if (t == objects::kNone || t >= d.types.size()) return {};
    if (const std::string_view id = d.types.get_string(t, "Text_ID"); !id.empty())
        if (const std::u16string* s = text.find(id)) return ui::utf16_to_utf32(*s);
    return u32(d.rules.types.size() > t ? d.rules.types[t].name : std::string());
}

// ---- encyclopedia -----------------------------------------------------------------------------------------------
ui::EncyclopediaEntry encyclopedia_entry(const GameData& d, const ui::TextDatabase& text, std::span<const ui::HudTypeInfo> hud_types,
                                         objects::TypeIndex t) {
    ui::EncyclopediaEntry e;
    if (t == objects::kNone || t >= d.types.size()) return e;
    e.header = type_label(d, text, t);
    e.icon = icon_of(hud_types, t);
    const galactic::TypeInfo* ti = t < d.rules.types.size() ? &d.rules.types[t] : nullptr;
    if (ti && ti->build_cost > 0) e.cost = number(ti->build_cost);
    if (ti && ti->kind == galactic::UnitKind::Planet) {
        ui::EncyclopediaRow income;  // [port] no capture of a planet's pop-up
        income.left = U"Income";
        income.right = number(ti->credit_value);
        e.rows.push_back(std::move(income));
    } else {
        // t40 (HUD_GAPS §2.2, captures C3_*tooltip*): class, ability icons and the Strong / Weak icon rows, no stat rows.
        e.population = static_cast<int32_t>(d.types.get_float(t, "Population_Value"));
        for (const std::string& id : words(d.types.get_string(t, "Encyclopedia_Unit_Class")))
            if (const std::u16string* s = text.find(id)) e.unit_class = ui::utf16_to_utf32(*s);
        e.abilities = ability_icons(d, t);
        auto icons = [&](const char* tag) {
            std::vector<std::string> out;
            for (const std::string& w : words(d.types.get_string(t, tag)))
                if (const uint32_t o = d.rules.find_type(w); o != galactic::kNoIndex)
                    if (std::string i = icon_of(hud_types, static_cast<objects::TypeIndex>(o)); !i.empty()) out.push_back(std::move(i));
            return out;
        };
        e.strong = icons("Encyclopedia_Good_Against");
        e.weak = icons("Encyclopedia_Vulnerable_To");
        e.strong_label = text_or(text, "TEXT_GOOD_AGAINST");
        e.weak_label = text_or(text, "TEXT_VULNERABLE_TO");
    }
    for (const std::string& id : words(d.types.get_string(t, "Encyclopedia_Text"))) {
        if (!e.description.empty()) e.description += U"\n";
        e.description += text_or(text, id);
    }
    return e;
}

// ---- holocron ---------------------------------------------------------------------------------------------------
void Holocron::sample(const galactic::GalacticSim& g) {
    const int32_t day = g.galactic_day();
    if (!days_.empty() && days_.back() == day) return;
    if (players_.empty()) {
        for (size_t p = 0; p < g.players().size(); ++p) {
            if (static_cast<int32_t>(p) == g.neutral_player()) continue;
            // [port] a faction that can neither play nor hold planets (the campaign's Hostile player) has no statistics.
            const uint32_t f = g.players()[p].faction;
            if (f < g.rules().factions.size() && !g.rules().factions[f].is_playable && !g.rules().factions[f].can_control_planets) continue;
            players_.push_back(static_cast<int32_t>(p));
        }
        econ_.assign(players_.size(), {});
        mil_.assign(players_.size(), {});
        planets_.assign(players_.size(), {});
    }
    days_.push_back(day);
    const objects::ObjectStore& st = g.store();
    for (size_t k = 0; k < players_.size(); ++k) {
        const int32_t p = players_[k];
        float power = 0.0f;
        auto add = [&](objects::ObjectHandle h) {
            if (st.alive(h) && static_cast<size_t>(st.type(h)) < g.rules().types.size()) power += g.rules().types[st.type(h)].ai_combat_power;
        };
        for (const galactic::FleetState& f : g.fleets())
            if (f.alive && f.owner == p)
                for (const objects::ObjectHandle h : f.contents) add(h);
        for (const galactic::PlanetState& pl : g.planets())
            for (const objects::ObjectHandle h : pl.surface)
                if (st.alive(h) && st.owner(h) == p) add(h);
        econ_[k].push_back(g.credits(p));
        mil_[k].push_back(power);
        planets_[k].push_back(static_cast<float>(g.planets_owned(p)));
    }
}

ui::DroidLogInfo Holocron::droid_log(const GameSession& s, const GameData& d, const ui::TextDatabase& text) const {
    ui::DroidLogInfo out;
    const galactic::GalacticSim* g = s.galaxy();
    if (!g) return out;
    std::vector<std::u32string> lines;
#ifdef ALAMO_SIM_HAVE_STORY
    if (const sim::StorySession* story = s.host().story())
        for (size_t set = 0; set < story->plot_set_count(); ++set) {
            if (story->plot_set_player(set) != s.local_player()) continue;
            for (const lua::StoryDialogEntry& e : story->model(set).dialog_log())
                if (!e.dialog.empty()) lines.push_back(text_or(text, e.dialog));
        }
#endif
    // ui4 t05 [port]: a readable battle line (conflict_log_line) instead of the conflict log's debug text.
    const auto planet_name = [g, &d, &text](PlanetIndex p) -> std::u32string {
        if (p >= g->planets().size()) return U"?";
        const uint32_t t = g->planets()[p].type;
        if (t < d.types.size())
            if (const std::string_view id = d.types.get_string(static_cast<objects::TypeIndex>(t), "Text_ID"); !id.empty())
                if (const std::u16string* str = text.find(id)) return ui::utf16_to_utf32(*str);
        return p < g->map().planets.name.size() ? u32(g->map().planets.name[p]) : std::u32string(U"?");
    };
    const auto faction_name = [g, &text](int32_t p) -> std::u32string {
        if (p < 0 || static_cast<size_t>(p) >= g->players().size()) return U"?";
        const uint32_t f = g->players()[static_cast<size_t>(p)].faction;
        if (f >= g->rules().factions.size()) return U"?";
        const galactic::FactionRules& fr = g->rules().factions[f];
        if (!fr.text_id.empty())
            if (const std::u16string* str = text.find(fr.text_id)) return ui::utf16_to_utf32(*str);
        return u32(fr.name);
    };
    if (const sim::CampaignBattles* b = s.battles())
        for (const galactic::ConflictLogEntry& c : b->conflicts().log()) lines.push_back(conflict_log_line(c, planet_name, faction_name));
    for (auto it = lines.rbegin(); it != lines.rend() && out.entries.size() < 40; ++it) out.entries.push_back(*it);
    return out;
}

ui::DataChartInfo Holocron::chart(const galactic::GalacticSim& g, const GameData& d, const ui::TextDatabase& text) const {
    ui::DataChartInfo c;
    c.first_cycle = days_.empty() ? 0 : days_.front();
    static constexpr ui::Rgba8 kColors[] = {{80, 160, 255, 255}, {255, 80, 64, 255}, {96, 220, 96, 255}, {255, 200, 64, 255}, {200, 120, 255, 255}};
    for (size_t k = 0; k < players_.size(); ++k) {
        const int32_t p = players_[k];
        const uint32_t f = g.players()[static_cast<size_t>(p)].faction;
        std::u32string name = u32("Player " + std::to_string(p));
        if (f < d.rules.factions.size()) {
            // The lookup is case-sensitive (GALACTIC_DIALOGS §2.7).
            const std::string key = ascii_upper("TEXT_FACTION_" + d.rules.factions[f].name);
            const std::u16string* t = text.find(key);
            name = t ? ui::utf16_to_utf32(*t) : u32(d.rules.factions[f].name);
        }
        const ui::Rgba8 col = kColors[k % std::size(kColors)];
        c.economy.push_back({name, col, econ_[k]});
        c.military.push_back({name, col, mil_[k]});
        c.planets.push_back({name, col, planets_[k]});
    }
    return c;
}

ui::TechTreeInfo Holocron::tech_tree(const galactic::GalacticSim& g, const GameData& d, std::span<const ui::HudTypeInfo> hud_types,
                                     int32_t player, bool space) const {
    ui::TechTreeInfo t;
    const uint32_t f = g.players()[static_cast<size_t>(player)].faction;
    // modc t20: the faction's own Tech_Tree_Dialog_Name (FoC IDD_TECH_TREE_REBEL / _EMPIRE / _BLACKMARKET), not its name.
    t.dialog = f < d.rules.factions.size() ? d.rules.factions[f].tech_tree_dialog : std::string();
    t.space = space;
    const int32_t level = g.players()[static_cast<size_t>(player)].tech_level;
    const GameData* dp = &d;
    std::vector<std::string> icons(hud_types.size());
    for (size_t i = 0; i < hud_types.size(); ++i) icons[i] = hud_types[i].icon_name;
    t.item = [dp, level, icons](std::string_view type, ui::TechTreeItem& out) {
        const uint32_t ti = dp->rules.find_type(type);
        if (ti == galactic::kNoIndex) return false;
        out.icon = ti < icons.size() ? icons[ti] : std::string();
        const int32_t need = dp->rules.types[ti].tech_level;
        out.state = need <= level ? ui::TechState::Researched : need == level + 1 ? ui::TechState::Available : ui::TechState::Locked;
        return true;
    };
    return t;
}

ui::SummaryInfo Holocron::summary(const galactic::GalacticSim& g, const GameData& d, const ui::TextDatabase& text,
                                  std::span<const ui::HudTypeInfo> hud_types, int32_t player) const {
    ui::SummaryInfo s;
    const objects::ObjectStore& st = g.store();
    double t_income = 0, t_base = 0, t_trade = 0;
    uint32_t pop = 0;
    const uint32_t mining_tag = galactic::economy_mining_tag();
    for (PlanetIndex i = 0; i < g.planets().size(); ++i) {
        const galactic::PlanetState& p = g.planets()[i];
        if (!p.is_planet || p.owner != player) continue;
        const objects::TypeIndex pt = p.type < d.types.size() ? static_cast<objects::TypeIndex>(p.type) : objects::kNone;
        const std::u32string name = pt != objects::kNone ? type_label(d, text, pt) : u32(i < g.map().planets.name.size() ? g.map().planets.name[i] : "");
        const std::string icon = icon_of(hud_types, pt);
        // Production: the queue heads (0 ground, 1 space).
        ui::SummaryProductionRow pr;
        pr.icon = icon;
        pr.name = name;
        if (!p.queue[1].empty()) {
            pr.space_icon = icon_of(hud_types, static_cast<objects::TypeIndex>(p.queue[1].front().type));
            pr.space_text = type_label(d, text, static_cast<objects::TypeIndex>(p.queue[1].front().type));
        }
        if (!p.queue[0].empty()) {
            pr.ground_icon = icon_of(hud_types, static_cast<objects::TypeIndex>(p.queue[0].front().type));
            pr.ground_text = type_label(d, text, static_cast<objects::TypeIndex>(p.queue[0].front().type));
        }
        uint32_t units = 0;
        auto hero = [&](objects::ObjectHandle h) {
            if (!st.alive(h) || st.owner(h) != player) return;
            ++units;
            if (d.types.get_bool(st.type(h), "Is_Named_Hero") && pr.heroes.size() < 8) pr.heroes.push_back(icon_of(hud_types, st.type(h)));
        };
        for (const objects::ObjectHandle h : p.surface) hero(h);
        ui::SummaryFleetRow fr;
        fr.icon = icon;
        fr.name = name;
        for (const uint32_t fi : p.orbit) {
            const galactic::FleetState& f = g.fleets()[fi];
            if (!f.alive || f.owner != player) continue;
            for (const objects::ObjectHandle h : f.contents) hero(h);
            if (!f.contents.empty() && fr.fleets.size() < 15) fr.fleets.push_back(icon_of(hud_types, st.type(f.contents.front())));
        }
        pr.pop = number(units);
        pop += units;
        s.production.push_back(std::move(pr));
        s.fleets.push_back(std::move(fr));
        // Economy: base value + the income list by kind.
        // P4 host: the mining column = this planet's entries tagged TEXT_ECONOMY_MINING_FACILITIES (P4_SIM_SYSTEMS §4.3).
        double trade = 0, other = 0, mining = 0;
        for (const galactic::IncomeEntry& e : p.income) {
            (e.kind == galactic::kIncomeTrade ? trade : other) += e.value;
            if (e.kind == galactic::kIncomeAbility && e.tag == mining_tag) mining += e.value;
        }
        const double base = p.base_value;
        ui::SummaryEconomyRow er;
        er.icon = icon;
        er.name = name;
        er.base = number(base);
        er.trade = number(trade);
        er.mining = number(mining);
        er.total = number(base + trade + other);
        s.economy.push_back(std::move(er));
        t_income += base + trade + other;
        t_base += base;
        t_trade += trade;
    }
    s.pop_total = number(pop);
    s.total_income = number(t_income);
    s.total_base = number(t_base);
    s.total_trade = number(t_trade);
    s.total_mining = number(g.mining_income(player));  // P4 host: GalacticSim::mining_income (P4_SIM_SYSTEMS §4.3)
    return s;
}

// ---- story ------------------------------------------------------------------------------------------------------
namespace {
// A text id through the text system's add-if-missing lookup (0x1fa460, GALACTIC_DIALOGS §2.7): a missing id shows as itself.
std::u32string lookup(const ui::TextDatabase& text, std::string_view id) { return ui::utf16_to_utf32(text.text(id)); }
// The TEXT command's text: looked up, then printed with Story_Var as the argument when it is set (0x1089e0).
std::u32string text_line(const ui::TextDatabase& text, std::string_view id, std::string_view var) {
    std::u32string t = lookup(text, id);
    return var.empty() ? t : story_var_printf(t, u32(var));
}
}  // namespace

// The story dialog script chapter (story_script.hpp, from the loaded scripts) as dialog lines: TEXT looked up (Story_Var
// printf), TEXTCOLOR colours, WAIT times, SFX / DIALOG cues, IMAGE; `extra` (the event's Add_Dialog_Text lines, already
// formatted) as they are at EXTRA_TEXT, nowhere else. False when the script is not loaded or the chapter is missing.
bool story_dialog_chapter(const StoryDialogCache& cache, const ui::TextDatabase& text, std::string_view dialog, int32_t chapter,
                          std::string_view var, const std::vector<std::string>& extra, ui::StoryDialogInfo& out, StoryMovie* movie) {
    StoryScript sc;
    if (!cache.chapter(dialog, chapter, sc)) return false;
    if (!sc.title.empty()) out.title = lookup(text, sc.title);
    auto push = [&out](std::u32string l, const uint8_t* rgba, double at) {
        out.lines.push_back(std::move(l));
        out.line_colors.push_back(ui::Rgba8{rgba[0], rgba[1], rgba[2], rgba[3]});
        out.line_at.push_back(at);
    };
    for (const StoryScriptLine& l : sc.lines) {
        if (l.kind == StoryScriptLine::Kind::Extra) {
            for (const std::string& e : extra) push(u32(e), l.rgba, l.at);
        } else {
            push(l.kind == StoryScriptLine::Kind::Text ? text_line(text, l.text_id, var) : std::u32string(), l.rgba, l.at);
        }
    }
    for (const StoryScriptCue& c : sc.cues)
        out.cues.push_back({c.at, c.kind == StoryScriptCue::Kind::Speech ? ui::StoryDialogCue::Kind::Speech : ui::StoryDialogCue::Kind::Sfx, c.event});
    if (!sc.image.empty()) out.image = sc.image;
    if (movie && !sc.movie.empty()) *movie = {sc.movie, sc.movie_loop};
    return true;
}

void sync_story_scripts(const GameSession& s, const GameData& d, StoryDialogCache& cache, StoryScriptLoads& loads) {
#ifdef ALAMO_SIM_HAVE_STORY
    const sim::StorySession* story = s.host().story();
    if (!story || !d.files) return;
    auto drain = [&](const lua::StoryModel& m, size_t& cursor) {
        const std::vector<std::string>& names = m.dialog_loads();
        for (; cursor < names.size(); ++cursor) cache.load(*d.files, names[cursor]);
    };
    loads.cursor.resize(story->plot_set_count(), 0);
    for (size_t set = 0; set < story->plot_set_count(); ++set) drain(story->model(set), loads.cursor[set]);
    const lua::StoryModel* tac = story->tactical_model();
    if (tac != loads.tactical) {
        loads.tactical = tac;
        loads.tactical_cursor = 0;
    }
    if (tac) drain(*tac, loads.tactical_cursor);
#else
    (void)s;
    (void)d;
    (void)cache;
    (void)loads;
#endif
}

std::vector<ui::StoryDialogInfo> new_story_dialogs(const GameSession& s, const StoryDialogCache& cache, const ui::TextDatabase& text, size_t& cursor,
                                                   std::vector<StoryMovie>* movies, std::vector<StoryDialogMeta>* meta) {
    std::vector<ui::StoryDialogInfo> out;
#ifdef ALAMO_SIM_HAVE_STORY
    const sim::StorySession* story = s.host().story();
    const lua::StoryHolocron* holo = story ? story->holocron() : nullptr;
    if (!holo) return out;
    // Fire step 6 records of EVERY plot set in firing order (the holocron list is global, GALACTIC_DIALOGS §2.8).
    const std::vector<lua::StoryDialogEntry>& log = holo->log();
    for (; cursor < log.size(); ++cursor) {
        const lua::StoryDialogEntry& e = log[cursor];
        const size_t at = holo->index_of(e.holocron_id);
        const lua::StoryHolocronEntry* rec = at < holo->size() ? &holo->entries()[at] : nullptr;
        static const std::vector<std::string> kNone;
        const std::vector<std::string>& extra = rec ? holo->lines(*rec) : kNone;
        ui::StoryDialogInfo i;
        i.tag = e.tag;
        StoryMovie movie;
        if (!story_dialog_chapter(cache, text, e.dialog, e.chapter, rec ? std::string_view(rec->var) : std::string_view(), extra, i, &movie)) {
            // [port] no loaded chapter (the original shows nothing): the Lua lines, else the dialog name.
            for (const std::string& l : extra) i.lines.push_back(u32(l));
            if (i.lines.empty()) i.lines.push_back(u32(e.dialog));
        }
        if (!movie.name.empty() && movies) movies->push_back(movie);
        if (i.title.empty() && e.chapter >= 0) i.title = U"Chapter " + number(e.chapter);
        i.accept = true;
        i.close = true;
        if (meta) {
            StoryDialogMeta md;
            md.popup = e.popup;
            md.sfx = e.sfx;
            const std::string& pn = e.plot_name;
            md.random_plot = ascii_istarts_with(pn, "random_plot");
            md.dialog = e.dialog;
            md.tag = e.tag;
            md.chapter = e.chapter;
            md.holocron_id = e.holocron_id;
            meta->push_back(std::move(md));
        }
        out.push_back(std::move(i));
    }
#else
    (void)s;
    (void)cache;
    (void)text;
    (void)cursor;
    (void)movies;
    (void)meta;
#endif
    return out;
}

HolocronSummaryView holocron_summary(const GameSession& s, const StoryDialogCache& cache, const ui::TextDatabase& text, bool completed,
                                     uint64_t selected_id) {
    HolocronSummaryView v;
    v.info.completed = completed;  // (Abandon: permanently disabled, P4_SIM_SYSTEMS §9.1)
    v.info.has_completed = false;
#ifdef ALAMO_SIM_HAVE_STORY
    const sim::StorySession* story = s.host().story();
    const lua::StoryHolocron* holo = story ? story->holocron() : nullptr;
    if (!holo) return v;
    std::vector<HolocronRecord> records;
    records.reserve(holo->size());
    for (const lua::StoryHolocronEntry& e : holo->entries()) {
        HolocronRecord r;
        r.id = e.id;
        r.dialog = e.dialog;
        r.var = e.var;
        r.tag = e.tag;
        r.chapter = e.chapter;
        r.completed = e.completed;
        r.read = e.read;
        r.lines = holo->lines(e);
        records.push_back(std::move(r));
    }
    const HolocronSummaryFill f = fill_holocron_summary(cache, records, completed, selected_id);
    v.info.has_completed = f.has_completed;
    for (const HolocronRow& row : f.rows) {
        const HolocronRecord& r = records[row.record];
        // [port] a chapter without TEXT (or a script that did not load) has no label in the original: its tag stands in.
        v.info.goals.push_back(row.text_id.empty() ? u32(r.tag) : lookup(text, row.text_id));
        v.info.goal_colors.push_back(ui::Rgba8{row.rgba[0], row.rgba[1], row.rgba[2], row.rgba[3]});
        v.row_ids.push_back(r.id);
    }
    v.info.selected = f.selected;
    if (f.selected < 0) return v;
    const size_t rec = f.rows[static_cast<size_t>(f.selected)].record;
    v.selected_index = rec;
    if (!f.title_id.empty()) v.info.title = lookup(text, f.title_id);
    for (const HolocronRecapLine& l : f.recap) {
        v.info.recap.push_back(l.kind == StoryScriptLine::Kind::Text    ? text_line(text, l.text, records[rec].var)
                               : l.kind == StoryScriptLine::Kind::Extra ? u32(l.text)
                                                                        : std::u32string());
        v.info.recap_colors.push_back(ui::Rgba8{l.rgba[0], l.rgba[1], l.rgba[2], l.rgba[3]});
    }
#else
    (void)s;
    (void)cache;
    (void)text;
    (void)selected_id;
#endif
    return v;
}

// ---- campaign end -----------------------------------------------------------------------------------------------
ui::CampaignScoreInfo campaign_score(const GameSession& s, const galactic::GalacticSim& g, const ui::TextDatabase& text, bool victory,
                                     const BattleTally& tally, const ScoringStats& stats) {
    ui::CampaignScoreInfo c;
    c.victory = victory;
    if (stats.ok) {
        auto pct = [](double e) { return static_cast<int32_t>(std::lround(std::clamp(e * 100.0, 0.0, 100.0))); };
        c.military = pct(stats.military);
        c.conquest = pct(stats.conquest);
        c.kill = pct(stats.kill);
        if (!stats.title_key.empty()) c.rank_title = lookup(text, stats.title_key);
        return c;
    }
    const int32_t me = s.local_player();
    uint32_t fought = 0, won = 0;
    if (const sim::CampaignBattles* b = s.battles())
        for (const galactic::ConflictLogEntry& e : b->conflicts().log()) {
            if (e.attacker != me && e.defender != me) continue;
            ++fought;
            if (e.winner == me) ++won;
        }
    uint32_t planets = 0;
    for (const galactic::PlanetState& p : g.planets()) planets += p.is_planet ? 1u : 0u;
    c.military = fought ? static_cast<int32_t>(std::lround(100.0 * won / fought)) : 0;
    c.conquest = planets ? static_cast<int32_t>(std::lround(100.0 * g.planets_owned(me) / planets)) : 0;
    const uint32_t all = tally.kills + tally.losses;
    c.kill = all ? static_cast<int32_t>(std::lround(100.0 * tally.kills / all)) : 0;
    return c;
}

// ---- P4 host: FoC galactic special panels ----------------------------------------------------------------------------
namespace {
const char* corruption_name(int8_t t) {
    static const char* const kNames[] = {"Intimidation", "Piracy", "Kidnapping", "Racketeering", "Corrupt Militia", "Bribery", "Black Market",
                                         "Bonded Citizens"};
    return t >= 0 && t < 8 ? kNames[t] : "Corruption";
}
std::u32string fraction(const char* label, int32_t have, int32_t need) { return u32(label) + number(have) + U"/" + number(need); }
}  // namespace

SpecialPanelData special_panel(const galactic::GalacticSim& g, const GameData& d, const ui::TextDatabase& text,
                               std::span<const ui::HudTypeInfo> hud_types, int32_t player, uint32_t kind, PlanetIndex planet,
                               uint32_t hero_id) {
    SpecialPanelData out;
    if (planet >= g.planets().size() || player < 0 || static_cast<size_t>(player) >= g.players().size()) return out;
    const objects::ObjectStore& st = g.store();
    const objects::ObjectHandle hero = hero_id ? st.find(static_cast<objects::ObjectId>(hero_id)) : objects::ObjectHandle{};
    const float credits = g.credits(player);
    const galactic::PlanetState& ps = g.planets()[planet];
    out.subtitle = ps.type < d.types.size() ? type_label(d, text, static_cast<objects::TypeIndex>(ps.type))
                                            : u32(planet < g.map().planets.name.size() ? g.map().planets.name[planet] : std::string());
    if (st.alive(hero)) out.hero = type_label(d, text, st.type(hero));
    const galactic::SpecialRules& sp = g.rules().special;
    switch (kind) {
        case galactic::kSpecialSlice:
            // REMAINING_GAPS 1.3 step 5: TEXT_SLICE_DIALOG_TITLE, one row per slice_entries entry (type, price, chance).
            out.title = text.find("TEXT_SLICE_DIALOG_TITLE") ? text_or(text, "TEXT_SLICE_DIALOG_TITLE") : std::u32string(U"Slice");
            if (text.find("TEXT_SLICE_DIALOG_DESCRIPTION")) out.subtitle = text_or(text, "TEXT_SLICE_DIALOG_DESCRIPTION");
            for (const galactic::SliceEntry& e : slice_rows(g, player, planet, hero_id))  // every row (the panel scrolls)
                out.slice.push_back({e.type, icon_of(hud_types, static_cast<objects::TypeIndex>(e.type)),
                                     type_label(d, text, static_cast<objects::TypeIndex>(e.type)), std::u32string(),
                                     static_cast<int32_t>(std::lround(e.price)), e.chance, e.price <= credits});
            break;
        case galactic::kSpecialBlackMarket:
            out.title = U"Black Market";
            for (const galactic::BlackMarketOffer& o : g.bm_offers(player, planet, hero)) {
                if (o.item >= sp.items.size() || out.market.size() >= 5) continue;
                const galactic::BlackMarketItemDef& it = sp.items[o.item];
                ui::BlackMarketItem m;
                m.item = o.item;
                m.icon = it.icon;
                m.name = it.localized_name.empty() ? u32(it.name) : text_or(text, it.localized_name);
                if (!it.localized_description.empty()) m.description = text_or(text, it.localized_description);
                m.cost = static_cast<int32_t>(std::lround(o.price));
                m.enabled = !o.purchased && o.price <= credits;
                // modc t20: the item faction's Tech_Level_Icon_Name picks the market_tech_level alternate (by prefix).
                m.tech_icon = it.faction < g.rules().factions.size() ? g.rules().factions[it.faction].tech_level_icon : std::string();
                out.market.push_back(std::move(m));
            }
            break;
        case galactic::kSpecialSabotage:
            out.title = U"Sabotage";
            for (const galactic::SabotageTarget& t : g.sabotage_targets(player, planet, hero)) {
                ui::SabotageTarget r;
                r.object = static_cast<uint32_t>(st.id(t.structure));
                r.icon = icon_of(hud_types, static_cast<objects::TypeIndex>(t.type));
                r.name = type_label(d, text, static_cast<objects::TypeIndex>(t.type));
                r.cost = t.cost;
                r.enabled = static_cast<float>(t.cost) <= credits;
                out.sabotage.push_back(std::move(r));
            }
            break;
        case galactic::kSpecialCorruption: {
            out.title = U"Corruption";
            for (const galactic::CorruptionChoiceDef& c : g.corruption_choices(planet)) {
                if (out.corruption.size() >= 3) break;
                const galactic::CorruptionRequirements r = g.corruption_requirements(player, planet, c);
                ui::CorruptionOption o;
                o.corruption_type = c.type;
                o.title = u32(corruption_name(c.type));
                o.cost = static_cast<int32_t>(std::lround(c.credit_cost));
                if (r.mission_need > 0) o.lines.push_back(fraction("Missions ", r.mission_have, r.mission_need));
                if (r.units_need > 0) o.lines.push_back(fraction("Units ", r.units_have, r.units_need));
                if (r.planets_need > 0) o.lines.push_back(fraction("Planets ", r.planets_have, r.planets_need));
                if (const galactic::CorruptionChoiceDef* m = g.corruption_choice(planet, c.type); m && m->tactical)
                    o.lines.push_back(u32(m->tactical_space ? "Space mission: " : "Land mission: ") + u32(m->tactical_map));
                o.enabled = r.all();
                out.corruption.push_back(std::move(o));
            }
            break;
        }
        case galactic::kSpecialHack: {
            out.title = U"Hack Super Weapon";
            const objects::ObjectHandle w = g.hack_find_super_weapon(planet);
            out.hack_cost = number(g.hack_cost());
            if (st.alive(w)) {
                out.hack_target = static_cast<uint32_t>(st.id(w));
                out.hack_lines.push_back(type_label(d, text, st.type(w)));
            }
            out.hack_enabled = st.alive(w) && g.hack_cost() <= credits;
            break;
        }
        default: break;
    }
    return out;
}

}  // namespace alamo::game
