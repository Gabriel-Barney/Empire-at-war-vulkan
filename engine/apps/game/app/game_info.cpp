#include "game_info.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>

#include "alamo/galactic/campaign.hpp"
#include "alamo/galactic/conflicts.hpp"
#include "alamo/galactic/corruption.hpp"  // P4 host: corruption mission lines
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
    auto ieq = [](std::string_view a, std::string_view b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
               });
    };
    std::vector<uint8_t> slots;
    std::vector<std::string> out;
    for (const objects::FieldIndex sub : d.types.get_sub_objects(t, "Unit_Abilities_Data")) {
        uint8_t type = 0;
        std::string alternate;
        for (objects::FieldIndex f = sub + 1; f < fc.size() && fc.depth[f] > fc.depth[sub]; ++f) {
            if (fc.parent[f] != sub) continue;
            if (ieq(raw.tag_name(f), "Type")) {
                for (const std::string& w : words(fc.text[f])) type = tactical::abilities::ability_type_from_name(w);
            } else if (ieq(raw.tag_name(f), "Alternate_Icon_Name")) {
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
    (void)d;
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
#else
    (void)text;
#endif
    if (const sim::CampaignBattles* b = s.battles())
        for (const galactic::ConflictLogEntry& c : b->conflicts().log()) lines.push_back(u32(c.text(*g)));
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
            const std::u16string* t = text.find("TEXT_FACTION_" + d.rules.factions[f].name);
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
    const std::string fac = f < d.rules.factions.size() ? d.rules.factions[f].name : std::string();
    t.faction = fac == "Rebel" ? ui::TechFaction::Rebel : fac == "Empire" ? ui::TechFaction::Empire : ui::TechFaction::BlackMarket;
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
// The story dialog script chapter (story_script.hpp) as dialog lines: text ids resolved, TEXTCOLOR colours, WAIT times,
// SFX / DIALOG cues, IMAGE; `extra` (the event's Lua dialog lines) at EXTRA_TEXT [port: appended when there is none].
// False when the file or the chapter is missing.
bool story_dialog_chapter(const GameData& d, const ui::TextDatabase& text, std::string_view dialog, int32_t chapter,
                          const std::vector<std::u32string>& extra, ui::StoryDialogInfo& out, StoryMovie* movie) {
    std::vector<uint8_t> bytes;
    if (!d.files || !d.files->read("Data\\Scripts\\Story\\" + std::string(dialog) + ".txt", bytes)) return false;
    StoryScript sc;
    if (!parse_story_script(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), chapter, sc)) return false;
    if (!sc.title.empty()) out.title = text_or(text, sc.title);
    bool extra_done = false;
    auto push = [&out](std::u32string l, const uint8_t* rgba, double at) {
        out.lines.push_back(std::move(l));
        out.line_colors.push_back(ui::Rgba8{rgba[0], rgba[1], rgba[2], rgba[3]});
        out.line_at.push_back(at);
    };
    for (const StoryScriptLine& l : sc.lines) {
        if (l.kind == StoryScriptLine::Kind::Extra) {
            for (const std::u32string& e : extra) push(e, l.rgba, l.at);
            extra_done = true;
        } else {
            push(l.kind == StoryScriptLine::Kind::Text ? text_or(text, l.text_id) : std::u32string(), l.rgba, l.at);
        }
    }
    if (!extra_done) {
        const uint8_t none[4] = {0, 0, 0, 0};
        const double at = sc.lines.empty() ? 0.0 : sc.lines.back().at;
        for (const std::u32string& e : extra) push(e, none, at);
    }
    for (const StoryScriptCue& c : sc.cues)
        out.cues.push_back({c.at, c.kind == StoryScriptCue::Kind::Speech ? ui::StoryDialogCue::Kind::Speech : ui::StoryDialogCue::Kind::Sfx, c.event});
    if (!sc.image.empty()) out.image = sc.image;
    if (movie && !sc.movie.empty()) *movie = {sc.movie, sc.movie_loop};
    return true;
}

std::vector<ui::StoryDialogInfo> new_story_dialogs(const GameSession& s, const GameData& d, const ui::TextDatabase& text, size_t& cursor,
                                                   std::vector<StoryMovie>* movies, std::vector<StoryDialogMeta>* meta) {
    std::vector<ui::StoryDialogInfo> out;
#ifdef ALAMO_SIM_HAVE_STORY
    const sim::StorySession* story = s.host().story();
    if (!story) return out;
    size_t seen = 0;  // dialog entries of the local plot sets, set order then firing order
    for (size_t set = 0; set < story->plot_set_count(); ++set) {
        if (story->plot_set_player(set) != s.local_player()) continue;
        const lua::StoryModel& m = story->model(set);
        for (const lua::StoryDialogEntry& e : m.dialog_log()) {
            if (seen++ < cursor) continue;
            ui::StoryDialogInfo i;
            i.tag = e.tag;
            // The dialog script's chapter with the Lua dialog lines (Add_Dialog_Text) at EXTRA_TEXT, else the Lua lines,
            // else the dialog name.
            const lua::StoryEventDef* def = e.plot < m.plot_count() && e.event < m.plot(e.plot).events.size() ? &m.plot(e.plot).events[e.event] : nullptr;
            std::vector<std::u32string> extra;
            if (def)
                for (const std::string& l : def->dialog) extra.push_back(text_or(text, l));
            StoryMovie movie;
            if (e.dialog.empty() || !story_dialog_chapter(d, text, e.dialog, e.chapter, extra, i, &movie)) {
                i.lines = std::move(extra);
                if (i.lines.empty() && !e.dialog.empty()) i.lines.push_back(text_or(text, e.dialog));
            }
            if (!movie.name.empty() && movies) movies->push_back(movie);
            if (i.title.empty() && e.chapter >= 0) i.title = U"Chapter " + number(e.chapter);
            i.accept = true;
            i.close = true;
            if (meta) {
                StoryDialogMeta md;
                md.popup = e.popup;
                const std::string& pn = e.plot < m.plot_count() ? m.plot(e.plot).name : std::string();
                md.random_plot = pn.size() >= 11 && std::equal(pn.begin(), pn.begin() + 11, "random_plot",
                                                               [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; });
                md.dialog = e.dialog;
                md.chapter = e.chapter;
                meta->push_back(std::move(md));
            }
            out.push_back(std::move(i));
        }
    }
    cursor = seen;
#else
    (void)s;
    (void)d;
    (void)text;
    (void)cursor;
    (void)movies;
    (void)meta;
#endif
    return out;
}

ui::StorySummaryInfo story_summary(const GameSession& s, const ui::TextDatabase& text, bool completed) {
    ui::StorySummaryInfo out;
    out.completed = completed;  // (Abandon: permanently disabled, P4_SIM_SYSTEMS §9.1)
#ifdef ALAMO_SIM_HAVE_STORY
    if (const sim::StorySession* story = s.host().story())
        for (size_t set = 0; set < story->plot_set_count(); ++set) {
            if (story->plot_set_player(set) != s.local_player()) continue;
            const lua::StoryModel& m = story->model(set);
            for (const lua::StoryObjective& o : m.objectives())
                if ((o.state == lua::StoryObjectiveState::Active) != completed) out.goals.push_back(text_or(text, o.text));
            for (const lua::StoryDialogEntry& e : m.dialog_log())
                if (!e.dialog.empty()) out.recap.push_back(text_or(text, e.dialog));
        }
#else
    (void)s;
    (void)text;
#endif
    return out;
}

// ---- campaign end -----------------------------------------------------------------------------------------------
ui::CampaignScoreInfo campaign_score(const GameSession& s, const galactic::GalacticSim& g, const ui::TextDatabase& text, bool victory,
                                     const BattleTally& tally) {
    (void)text;
    ui::CampaignScoreInfo c;
    c.victory = victory;
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
    const int32_t mean = (c.military + c.conquest + c.kill) / 3;
    c.rank_title = mean >= 80 ? U"Grand Admiral" : mean >= 60 ? U"Admiral" : mean >= 40 ? U"Commodore" : mean >= 20 ? U"Captain" : U"Commander";
    c.list.push_back(U"Battles fought: " + number(fought) + U", won: " + number(won));
    c.list.push_back(U"Planets controlled: " + number(g.planets_owned(me)) + U" / " + number(planets));
    c.list.push_back(U"Enemy units destroyed: " + number(tally.kills) + U", units lost: " + number(tally.losses));
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
                const std::string fac = it.faction < g.rules().factions.size() ? g.rules().factions[it.faction].name : std::string();
                m.tech_alt = fac == "Rebel" ? 0 : fac == "Empire" ? 1 : -1;
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
            const std::vector<galactic::CorruptionOption> missions = galactic::corruption_options(d.types, ps.type);
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
                if (const galactic::CorruptionOption* m = galactic::find_corruption_option(missions, c.type); m && m->tactical)
                    o.lines.push_back(u32(m->kind == galactic::ConflictKind::Space ? "Space mission: " : "Land mission: ") + u32(m->map));
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
