// alamo_game App: galactic / tactical HUD frame building (split from game_main.cpp).
#include "game_app.hpp"

#include "alamo/assets/texture.hpp"

namespace alamo_game_app {

// ------------------------------------------------------------------------------------------------ HUD frames
// g_planet_fleet faction-emblem index of `player`: GALAXY_MAP_PRESENTATION §1.4, 0x3d18f0 maps the faction NAME to the
// Icon_Alternate list - Rebel 0, Empire 1, Pirate 2, Underworld 3, any other faction 4 (a retail hard-code, so a mod's
// factions get emblem 4 as in the original; modc t20: exact names, any case, instead of the port's old substring match;
// [M] which of the strings "Pirate" / "Pirates" 0x3d18f0 compares - both are in the string table). -1 = no player.
int32_t App::galactic_faction_icon(const galactic::GalacticSim& g, int32_t player) const {
    if (player < 0 || static_cast<size_t>(player) >= g.players().size()) return -1;
    const uint32_t fi = g.players()[static_cast<size_t>(player)].faction;
    if (fi >= g.rules().factions.size()) return -1;
    const std::string_view n = g.rules().factions[fi].name;
    // [name-ok-begin: retail hard-code, GALAXY_MAP_PRESENTATION.md §1.4 0x3d18f0 (emblem index by faction name)]
    if (ascii_iequals(n, "Rebel")) return 0;
    if (ascii_iequals(n, "Empire")) return 1;
    if (ascii_iequals(n, "Pirate") || ascii_iequals(n, "Pirates")) return 2;
    if (ascii_iequals(n, "Underworld")) return 3;
    // [name-ok-end]
    return 4;
}

// R2 t01: the owner faction's colour from the Factions.xml table (render::load_faction_colors), not a guess: text = true
// -> <Display_Font_Color> (planet labels / Planet Info name: GALACTIC_DIALOGS §9.2 [H] reads faction +0xb8..+0xc4;
// that the map labels use it too is [M]), else <Color> (pancake secondary colour, ring / selection tint). An unowned
// planet or a player of the Neutral faction takes the Neutral faction's entry; `fallback` when the table has none.
ui::Rgba8 App::faction_rgba(const galactic::GalacticSim& g, int32_t player, bool text, ui::Rgba8 fallback) const {
    const galactic::GalacticRules& rules = g.rules();
    uint32_t fi = rules.neutral_faction();
    if (player >= 0 && static_cast<size_t>(player) < g.players().size()) fi = g.players()[static_cast<size_t>(player)].faction;
    if (fi >= rules.factions.size()) return fallback;
    materials::float4 c;
    const std::string_view name = rules.factions[fi].name;
    if (!(text ? render::faction_text_color(faction_colors_, name, c) : render::faction_map_color(faction_colors_, name, c))) return fallback;
    auto to8 = [](float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return {to8(c.x), to8(c.y), to8(c.z), 255};
}

// Planet overlay icons (ui pass 2): the strongest garrison unit (AI combat power, then type index), the heroes at the
// planet (surface and orbit), and one stack per fleet (count = members, 1..5) plus one land-force stack per owner.
// GALAXY_MAP_PRESENTATION §1.4 (0x3d15b0): the representative unit of a fleet / land force - every unit competes on max
// tactical health (Tactical_Health; a hero needs >=, a non-hero >); with the hero filter (b_filters3) on, the hero with the
// lowest Ranking_In_Category wins (ui4 t03: GalacticHud::filter_bits fed back as hero_filter_); transports'
// cargo is already in the fleet contents.
std::string App::representative_icon(std::span<const objects::ObjectHandle> units, bool hero_filter) const {
    const galactic::GalacticSim* g = galaxy();
    if (!g) return {};
    const objects::ObjectStore& st = g->store();
    objects::TypeIndex best = objects::kNone, hero = objects::kNone;
    float best_hp = -1.0f;
    int32_t hero_rank = 0;
    for (const objects::ObjectHandle h : units) {
        if (!st.alive(h)) continue;
        const objects::TypeIndex t = st.type(h);
        if (t >= ui_.hud_types.size() || ui_.hud_types[t].icon_name.empty()) continue;
        const bool is_hero = is_named_hero(*g, static_cast<uint32_t>(t)) || ui_.hud_types[t].is_hero;
        const float hp = t < data_.types.size() ? data_.types.get_float(t, "Tactical_Health") : 0.0f;
        if (best == objects::kNone || (is_hero ? hp >= best_hp : hp > best_hp)) {
            best = t;
            best_hp = hp;
        }
        if (is_hero) {
            const int32_t r = t < data_.types.size() ? data_.types.get_int(t, "Ranking_In_Category") : 0;
            if (hero == objects::kNone || r < hero_rank) {
                hero = t;
                hero_rank = r;
            }
        }
    }
    const objects::TypeIndex pick = hero_filter && hero != objects::kNone ? hero : best;
    return pick == objects::kNone ? std::string() : ui_.hud_types[pick].icon_name;
}

void App::planet_icons(const galactic::GalacticSim& g, PlanetIndex row, ui::GalacticPlanetInfo& out) const {
    const objects::ObjectStore& st = g.store();
    const galactic::PlanetState& ps = g.planets()[row];
    auto icon = [this](objects::TypeIndex t) { return type_icon(t); };
    auto is_hero = [this](objects::TypeIndex t) { return t < ui_.hud_types.size() && ui_.hud_types[t].is_hero; };
    // GRC 2 / REMAINING_GAPS §6.3 (can_see_planet_info for the local player): other players' forces show only as far as
    // the planet's visibility allows - fleet stacks with FleetPresence (one stack per owner unless NumFleets), their
    // size and heroes with FleetContents, the stack's representative icon with FleetContents or MostPowerfulShip, land
    // stacks with NumGroundCompanies, the garrison icon and land heroes with GroundCompanyContents. Own and allied
    // objects are always shown [port]. [port: Minor/MajorStealthHeroes not applied - which heroes are minor or major
    // stealth heroes is not traced (STRATEGIC_GAPS); an all-stealthed enemy fleet stays hidden as before.]
    using PI = galactic::GalacticSim::PlanetInfo;
    const int32_t me = local_player();
    auto see = [&](PI what) { return g.can_see_planet_info(me, row, what); };
    const bool fleet_presence = see(PI::FleetPresence), fleet_contents = see(PI::FleetContents);
    const bool ground_count = see(PI::NumGroundCompanies), ground_contents = see(PI::GroundCompanyContents);
    const bool num_fleets = see(PI::NumFleets), most_powerful = see(PI::MostPowerfulShip);
    auto friendly = [&](int32_t owner) { return me >= 0 && owner >= 0 && g.allied(me, owner); };
    float best = -1.0f;
    objects::TypeIndex best_t = objects::kNone;
    std::map<int32_t, uint32_t> land;  // owner -> surface units (owner order)
    for (const objects::ObjectHandle h : ps.surface) {
        if (!st.alive(h)) continue;
        const objects::TypeIndex t = st.type(h);
        if (!friendly(st.owner(h)) && !ground_contents) {
            if (ground_count && !is_hero(t)) ++land[st.owner(h)];
            continue;
        }
        if (is_hero(t)) {
            // GALAXY_MAP_PRESENTATION §1.5: the medal of the hero's faction (faction-icon order: 0 rebel, 1 empire, 3 underworld).
            const int32_t fx = st.owner(h) >= 0 ? galactic_faction_icon(g, st.owner(h)) : -1;
            out.hero_rebel = out.hero_rebel || fx == 0;
            out.hero_empire = out.hero_empire || fx == 1;
            out.hero_underworld = out.hero_underworld || fx == 3;
            if (!friendly(st.owner(h)) && out.enemy_hero < 0) out.enemy_hero = fx == 1 ? 1 : 0;  // ui4 t03 g_enemy_hero
            continue;
        }
        ++land[st.owner(h)];
        const float power = t < g.rules().types.size() ? g.rules().types[t].ai_combat_power : 0.0f;
        if (power > best || (power == best && t < best_t)) {
            best = power;
            best_t = t;
        }
    }
    if (best_t != objects::kNone) out.garrison_icon = icon(best_t);
    // GALAXY_MAP_PRESENTATION §1.3 / §1.4: one button per orbiting fleet - unit count, the representative unit's icon,
    // stealth (every unit galactic-stealthed) / raid (ConflictManager::raid_force) badges, enemy flag; the fleet's own
    // orbit slot comes from the sim (FleetState::slot, playtest 3 t07; -1 = first free; the ui moves enemy fleets to
    // slot 0 when the orbit is not the local player's). A stealthed enemy fleet is not shown.
    out.orbit_local = me >= 0 && !g.blockaded(row, me);
    const galactic::ConflictManager* cm = session_ && session_->battles() ? &session_->battles()->conflicts() : nullptr;
    // Other players' fleets without NumFleets: one merged stack per owner (owner order) so the fleet count stays hidden.
    struct Merged { std::vector<objects::ObjectHandle> live; bool all_stealth = true; uint32_t fleet = 0; };
    std::map<int32_t, Merged> merged;
    auto push_stack = [&](int32_t owner, bool own, const std::vector<objects::ObjectHandle>& live, bool all_stealth, uint32_t fleet,
                          int32_t orbit_slot) {
        const bool contents = own || fleet_contents;
        ui::GalacticPlanetStack s;
        s.faction = std::max(0, galactic_faction_icon(g, owner));
        s.owner_color = faction_rgba(g, owner, false, ui::Rgba8{});  // §1.4 secondary colour (white when the table has none)
        s.units = contents ? static_cast<uint32_t>(live.size()) : 1u;
        s.count = std::clamp<uint32_t>(s.units, 1, 5);
        if (contents || most_powerful) s.icon = representative_icon(live, hero_filter_);  // ui4 t03: b_filters3
        // ui4 t03 g_enemy_hero: a seen enemy hero in orbit too [port] (Icon_Alternate 1 = i_icon_empire_hero).
        if (contents && !friendly(owner) && out.enemy_hero < 0)
            for (const objects::ObjectHandle m : live)
                if (is_hero(st.type(m))) {
                    out.enemy_hero = galactic_faction_icon(g, owner) == 1 ? 1 : 0;
                    break;
                }
        s.enemy = owner != me;
        s.stealth = all_stealth;
        s.raid = !s.stealth && cm && cm->raid_force(owner, live);
        s.selected = fleet != 0 && !orders_.selection().empty() && orders_.selection().front() == fleet;
        // The button's fleet: a click selects it (GalacticSelect), an own fleet's button (enemy = false) drags it.
        s.fleet = fleet;
        s.slot = orbit_slot;
        // Orbiting heroes show only in their fleet's slot, as in the original (Gabriel, playtest 3, 2026-10-29).
        out.stacks.push_back(std::move(s));
    };
    for (const galactic::FleetState& fl : g.fleets()) {
        if (!fl.alive || fl.planet != row || fl.contents.empty()) continue;
        const bool own = friendly(fl.owner);
        if (!own && !fleet_presence) continue;
        std::vector<objects::ObjectHandle> live;
        bool all_stealth = true;
        for (const objects::ObjectHandle m : fl.contents)
            if (st.alive(m)) {
                live.push_back(m);
                all_stealth = all_stealth && g.hidden(m);
            }
        if (live.empty() || (!own && all_stealth)) continue;
        const uint32_t fleet = st.alive(fl.object) ? static_cast<uint32_t>(st.id(fl.object)) : 0u;
        if (!own && !num_fleets) {
            Merged& m = merged[fl.owner];
            m.live.insert(m.live.end(), live.begin(), live.end());
            m.all_stealth = m.all_stealth && all_stealth;
            if (m.fleet == 0) m.fleet = fleet;
            continue;
        }
        push_stack(fl.owner, own, live, all_stealth, fleet, fl.slot);
    }
    for (const auto& [owner, m] : merged) push_stack(owner, false, m.live, m.all_stealth, m.fleet, -1);
    for (const auto& [owner, n] : land) {
        ui::GalacticPlanetStack s;
        s.faction = std::max(0, galactic_faction_icon(g, owner));
        s.owner_color = faction_rgba(g, owner, false, ui::Rgba8{});
        s.units = n;
        s.count = std::clamp<uint32_t>(n, 1, 5);
        s.enemy = owner != me;
        s.land = true;
        // Land forces: a click selects the planet (its land organiser) for the local player's own forces [port: the
        // original's land-forces click target is not specified]; other players' land forces are not clickable. Right
        // presses pass through to the planet (RouteHooks::hud_takes).
        if (owner == me && st.alive(ps.object)) s.fleet = static_cast<uint32_t>(st.id(ps.object));
        if (friendly(owner) || ground_contents) {
            std::vector<objects::ObjectHandle> units;
            for (const objects::ObjectHandle h : ps.surface)
                if (st.alive(h) && st.owner(h) == owner) units.push_back(h);
            s.icon = representative_icon(units, hero_filter_);
        }
        out.stacks.push_back(std::move(s));
    }
}

// ui4 t03 [port] (GALAXY_MAP_PRESENTATION §1.1): the status icons and the corruption line of planet `row` for the local
// player. g_build: one of the planet's queues holds a local entry. g_smuggled: an income siphon runs on the planet and the
// local player is its siphoner or the planet's owner. g_corruption_icon: corrupted (its owner or the corrupter sees it, or
// the planet's CorruptionInfo is visible [port: the gate is the political-control one]). g_corruption_text: while a
// corruption transition runs, (int)(fraction x 100) in TEXT_CORRUPTION_PERCENT / TEXT_CORRUPTION_REMOVE_PERCENT
// (GALACTIC_REVEAL_CORRUPTION §4.6).
void App::planet_status(const galactic::GalacticSim& g, PlanetIndex row, ui::GalacticPlanetInfo& out) const {
    const galactic::PlanetState& ps = g.planets()[row];
    const int32_t me = local_player();
    for (uint32_t q = 0; q < 2 && !out.building; ++q)
        for (const galactic::BuildEntry& b : g.queue_of(row, q))
            if (b.player == me) out.building = true;
    out.smuggled = ps.siphon_player != galactic::kNoPlayer && g.frame() < ps.siphon_until && (ps.siphon_player == me || ps.owner == me);
    const bool seen = ps.owner == me || g.can_see_planet_info(me, row, galactic::GalacticSim::PlanetInfo::PoliticalControl);
    out.corrupted = seen && g.corrupted(row);
    if (seen && g.corruption_transition_active(row)) {
        const auto pct = static_cast<int32_t>(g.corruption_transition_fraction(row) * 100.0f);
        const char* key = ps.corruption_removing ? "TEXT_CORRUPTION_REMOVE_PERCENT" : "TEXT_CORRUPTION_PERCENT";
        const std::u16string* t = ui_.text.find(key);
        std::u32string line = t ? ui::utf16_to_utf32(*t) : std::u32string();
        std::u32string num;
        for (const char c : std::to_string(pct)) num.push_back(static_cast<char32_t>(c));
        // The text carries a printf field (%d / %s) [M]; without one the number and '%' are appended.
        size_t at = std::u32string::npos;
        for (const std::u32string_view f : {std::u32string_view(U"%d"), std::u32string_view(U"%s"), std::u32string_view(U"%i")})
            if (at == std::u32string::npos) at = line.find(f);
        if (at != std::u32string::npos) line.replace(at, 2, num);
        else line += (line.empty() ? U"" : U" ") + num + U"%";
        out.corruption_text = std::move(line);
    }
}

// GALACTIC_DIALOGS §9.2: IDD_PLANET_INFO from the planet TYPE (static XML text); no live sim state and no
// REMAINING_GAPS §6.3 gating (the original shows the same dialog for enemy, fogged or stealthed planets). Only the name
// colour reveals the owner: the owner faction's Factions.xml <Display_Font_Color>, white when unowned or neutral.
// IDC_PLANET_ICON = the planet type's portrait (render::PlanetPortraits, "planet_portrait:<type>"); planet_overlay "" =
// load_overlay.tga.
ui::PlanetInfoData App::planet_info_data(const galactic::GalacticSim& g, PlanetIndex row) const {
    ui::PlanetInfoData d;
    if (row >= g.planets().size()) return d;
    const galactic::PlanetState& ps = g.planets()[row];
    planet_type_info(d, ps.type < data_.types.size() ? static_cast<objects::TypeIndex>(ps.type) : objects::kNone);
    if (d.name.empty() && row < g.map().planets.name.size()) d.name = ui::utf8_to_utf32(g.map().planets.name[row]);
    const int32_t own = ps.owner;
    if (own >= 0 && static_cast<size_t>(own) < g.players().size()) {
        const uint32_t fx = g.players()[static_cast<size_t>(own)].faction;
        if (fx < g.rules().factions.size() && fx != g.rules().neutral_faction()) {
            // R2 t01: <Display_Font_Color> (faction +0xb8..+0xc4, GALACTIC_DIALOGS §9.2 [H]), was <Color>.
            d.name_color = faction_rgba(g, own, true, d.name_color);
        }
    }
    return d;
}

// The static text and icons of a planet type (GALACTIC_DIALOGS §9.2): IDD_PLANET_INFO and the battle load screens.
void App::planet_type_info(ui::PlanetInfoData& d, objects::TypeIndex t) const {
    const bool typed = t != objects::kNone && t < data_.types.size();
    auto field = [&](std::string_view tag) {
        std::string_view v = typed ? data_.types.get_string(t, tag) : std::string_view{};
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '\r' || v.front() == '\n')) v.remove_prefix(1);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r' || v.back() == '\n')) v.remove_suffix(1);
        return v;
    };
    auto text = [&](std::string_view tag) {
        const std::string_view id = field(tag);
        if (id.empty()) return std::u32string();
        const std::u16string* txt = ui_.text.find(id);
        return txt ? ui::utf16_to_utf32(*txt) : ui::utf8_to_utf32(id);
    };
    d.name = text("Text_ID");
    if (std::u32string v = text("Describe_Tactical"); !v.empty()) d.tactical.push_back(std::move(v));
    if (std::u32string v = text("Describe_Advantage"); !v.empty()) d.advantages.push_back(std::move(v));
    if (std::u32string v = text("Describe_History"); !v.empty()) d.history.push_back(std::move(v));
    if (typed && t < data_.rules.types.size()) d.planet_icon = render::planet_portrait_name(data_.rules.types[t].name);
    d.natives = text("Describe_Population");
    d.terrain = text("Describe_Terrain");
    d.weather = text("Describe_Weather");
    d.weather_icon = ui::planet_info_weather_icon(field("Encyclopedia_Weather_Icon"));
    d.native_icon = std::string(field("Native_Icon_Name"));
    // IDC_MAP_ICON: Land_Tactical_Map .ted -> .dds, switched off when the texture does not resolve (the Art/Textures
    // folder is among the lookup candidates, assets::texture_candidates).
    if (const std::string map = ui::planet_info_map_icon(field("Land_Tactical_Map")); !map.empty())
        for (const std::string& c : assets::texture_candidates(map))
            if (data_.files->exists(c)) {
                d.map_icon = map;
                break;
            }
}

// QA U1: the place a battle is fought at, localized; a skirmish map's planet type (_MP_SPACE_KUAT -> Kuat). "" when
// unknown: never the map file name.
std::u32string App::battle_place_name(PlanetIndex planet) const {
    if (!session_) return {};
    if (session_->spec().kind == SessionKind::Skirmish) {
        const uint32_t t = data_.rules.find_type(game::skirmish_map_planet_type(data_.rules, session_->spec().skirmish_map));
        return t != galactic::kNoIndex && t < data_.types.size() ? type_label(data_, ui_.text, static_cast<objects::TypeIndex>(t)) : std::u32string();
    }
    const galactic::GalacticSim* g = session_->galaxy();
    return g && planet < g->planets().size() ? planet_label(data_, ui_.text, *g, planet) : std::u32string();
}

// QA t16 (UI.md §5.1 step 1, §5.2): fills the load dialog from the planet type's text, as IDD_PLANET_INFO (GALACTIC_DIALOGS
// §9.2) [inferred: the same fields; the fill function of the load dialog is not traced field by field]. QA t11: the sim
// is held until it has been presented (end_load_intro_frame); no load moved off this thread.
bool App::open_battle_load(PlanetIndex planet, bool space, bool mission) {
    if (ui_.load.is_open()) return true;
    if (!session_) return false;
    const bool skirmish = session_->spec().kind == SessionKind::Skirmish;
    ui::PlanetInfoData d;
    if (skirmish) {
        const uint32_t t = data_.rules.find_type(game::skirmish_map_planet_type(data_.rules, session_->spec().skirmish_map));
        if (t != galactic::kNoIndex) planet_type_info(d, static_cast<objects::TypeIndex>(t));
    } else if (const galactic::GalacticSim* g = session_->galaxy()) {
        d = planet_info_data(*g, planet);
        // The displayed planet type's portrait (render::PlanetPortraits) as the planet icon.
        if (planet < g->planets().size())
            if (const uint32_t pt = display_planet_type(*g, planet); pt < g->rules().types.size())
                d.planet_icon = render::planet_portrait_name(g->rules().types[pt].name);
    }
    // UI.md §5.1 step 1: the land or space picture by battle kind; IDD_PLANET_INFO's land map on a space battle would show
    // the wrong place [inferred: Space_Tactical_Map, hidden when its .dds does not exist].
    if (space) {
        d.map_icon.clear();
        objects::TypeIndex t = objects::kNone;
        if (skirmish) {
            const uint32_t st = data_.rules.find_type(game::skirmish_map_planet_type(data_.rules, session_->spec().skirmish_map));
            if (st != galactic::kNoIndex) t = static_cast<objects::TypeIndex>(st);
        } else if (const galactic::GalacticSim* g = session_->galaxy(); g && planet < g->planets().size()) {
            t = static_cast<objects::TypeIndex>(g->planets()[planet].type);
        }
        if (t != objects::kNone && t < data_.types.size())
            if (const std::string map = ui::planet_info_map_icon(data_.types.get_string(t, "Space_Tactical_Map")); !map.empty())
                for (const std::string& c : assets::texture_candidates(map))
                    if (data_.files->exists(c)) {
                        d.map_icon = map;
                        break;
                    }
    }
    ui::LoadScreenInfo info;
    info.planet_name = battle_place_name(planet);
    info.tactical = std::move(d.tactical);
    info.advantages = std::move(d.advantages);
    info.history = std::move(d.history);
    info.terrain = std::move(d.terrain);
    info.weather = std::move(d.weather);
    info.natives = std::move(d.natives);
    info.planet_icon = std::move(d.planet_icon);
    info.weather_icon = std::move(d.weather_icon);
    info.map_icon = std::move(d.map_icon);
    info.native_icon = std::move(d.native_icon);
    info.begin_button = !session_->spec().attract;  // OI host: the attract demo starts as soon as it is loaded
    // UI.md §5.1 step 1 names five dialogs; [port] UI5 t07: a story mission (LINK_TACTICAL battle) takes the Mission pair,
    // a skirmish the skirmish dialog, every other battle the Battle pair (which battles retail sends to the Mission dialogs
    // is an RE gap).
    const ui::LoadScreenKind kind = skirmish ? ui::LoadScreenKind::Skirmish
                                    : mission ? (space ? ui::LoadScreenKind::SpaceMission : ui::LoadScreenKind::LandMission)
                                    : space   ? ui::LoadScreenKind::SpaceBattle
                                              : ui::LoadScreenKind::LandBattle;
    screens_.close_planet_info();  // GALACTIC_DIALOGS §9.4: IDD_PLANET_INFO never survives into a battle
    ui_.load.set_progress_source([this] { return bview_.active() ? bview_.load_progress() : 0.0f; });
    if (!ui_.load.open(ui_.gui, kind, info)) return false;
    audio_.music(MusicScene::LoadScreen, local_faction());
    load_intro_frames_ = kLoadIntroMaxFrames;
    load_intro_shown_ = 0;
    load_open_tick_ = session_->host().frame();
    return true;
}

// After each presented frame: the hold ends once the dialog has been drawn complete (its textures and the planet portrait
// are uploaded over the first frames) or after kLoadIntroMaxFrames. Also: a Fight answer that never became a battle (the
// conflict was dropped or auto-resolved instead) closes the load screen again.
void App::end_load_intro_frame() {
    if (load_intro_frames_ > 0) {
        ++load_intro_shown_;
        bool pending = load_intro_shown_ < 2;
        for (const ui::GuiImagePages::Entry& e : ui_.pages.entries()) pending = pending || (e.width == 0 && !e.failed);
        load_intro_frames_ = pending ? load_intro_frames_ - 1 : 0;
        if (load_intro_frames_ == 0) std::printf("load screen: %u frames presented before the blocking load\n", load_intro_shown_);
        return;
    }
    if (screen_ == Screen::Galactic && ui_.load.is_open() && session_ && !session_->in_battle() &&
        session_->host().frame() > load_open_tick_ + kLoadIntroMaxTicks) {
        ui_.load.close();
        audio_.music(MusicScene::Galactic, local_faction());
    }
}

void App::build_galactic_hud(ui::GalacticHudFrame& f, std::vector<ui::GalacticPlanetInfo>& planets) {
    const uint32_t w = targets_.width, h = targets_.height;
    const galactic::GalacticSim* g = galaxy();
    f.view = ui::make_view(gview_.camera_params(w, h), w, h);
    planets.clear();
    for (const render::GalacticPlanetSnap& p : gview_.frame().planets) {
        if (!(p.flags & render::kGalPlanet) || (p.flags & render::kGalHidden) || p.id == objects::kNoObjectId) continue;
        ui::GalacticPlanetInfo i;
        i.planet = static_cast<uint32_t>(p.id);
        i.position = {p.position.x, p.position.y, p.position.z};
        i.radius = gview_.planet_radius(p.type);
        std::string row = g && p.row < g->map().planets.name.size() ? g->map().planets.name[p.row] : std::string();
        // Display name: the planet type's Text_ID in MasterTextFile, else the row name.
        const std::u16string* txt = nullptr;
        if (p.type != objects::kNone && p.type < data_.types.size())
            if (const std::string_view id = data_.types.get_string(p.type, "Text_ID"); !id.empty()) txt = ui_.text.find(id);
        i.name = txt ? ui::utf16_to_utf32(*txt) : ui::utf8_to_utf32(row);
        auto to8 = [](float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
        i.owner_color = {to8(p.team_color.x), to8(p.team_color.y), to8(p.team_color.z), 255};
        // R2 t01: the name in the owner faction's Factions.xml <Display_Font_Color> (Neutral faction's for an unowned planet);
        // the palette colour stays the fallback when the table has no such faction. [M: that the map labels read
        // Display_Font_Color is inferred from GALACTIC_DIALOGS §9.2; these values replace the ones read off a screenshot.]
        if (g && p.row < g->planets().size()) {
            const int32_t own = g->planets()[p.row].owner;
            const int32_t fx = own >= 0 ? galactic_faction_icon(*g, own) : -1;
            i.owner_color = faction_rgba(*g, own, true, i.owner_color);
            // GALACTIC_DIALOGS §5.3: the allegiance emblem index (0 rebel, 1 empire, 2 underworld, 3 pirate, 4 hutt; the
            // faction-icon order above swaps pirate / underworld); neutral or unowned -> none.
            static constexpr int32_t kAllegiance[] = {0, 1, 3, 2, 4};
            if (fx >= 0 && g->players()[static_cast<size_t>(own)].faction != g->rules().neutral_faction())
                i.allegiance = kAllegiance[fx];
        }
        // Fog (render apply_galactic_fog: kGalFogged), garrison / hero icons and the fleet / land-force stacks.
        i.fogged = (p.flags & render::kGalFogged) != 0;
        if (g && p.row < g->planets().size()) planet_icons(*g, p.row, i);
        i.political_control = p.control;
        i.space_level = p.starbase_level > 0 ? p.starbase_level : -1;
        i.ground_level = p.ground_level > 0 ? p.ground_level : -1;
        i.conflict = (p.flags & render::kGalInBattle) != 0;
        // ui4 t03 [port]: weather / ability icons from the planet type (b_filters2), the status icons and the corruption line.
        if (p.type != objects::kNone && p.type < data_.types.size()) {
            i.weather_icon = std::string(data_.types.get_string(p.type, "Encyclopedia_Weather_Icon"));
            i.ability_icon = std::string(data_.types.get_string(p.type, "Planet_Ability_Icon"));
        }
        if (g && p.row < g->planets().size()) planet_status(*g, p.row, i);
        if (g && p.row < g->planets().size()) {  // OI host: REMAINING_GAPS §6.3 display gates for the local player
            i.starbase_info = g->can_see_starbase_info(local_player(), p.row);
            i.groundbase_info = g->can_see_groundbase_info(local_player(), p.row);
            // GRC 2: the ui gets only what the local player can see.
            if (!i.starbase_info) i.space_level = -1;
            if (!i.groundbase_info) i.ground_level = -1;
            if (!g->can_see_planet_info(local_player(), p.row, galactic::GalacticSim::PlanetInfo::PoliticalControl))
                i.political_control = -1.0f;
            // Text_Planetary_Income (GALACTIC_DIALOGS §5.3), behind the same gate as the planet tooltips. GalacticSim::planet_income
            // refreshes the sim's lists, so the host only reads: the last fiscal cycle's entries, else Planet_Credit_Value [port].
            if (!i.fogged && g->can_see_planet_info(local_player(), p.row, galactic::GalacticSim::PlanetInfo::CreditIncome)) {
                const galactic::PlanetState& ps = g->planets()[p.row];
                float sum = ps.income.empty() && ps.type < g->rules().types.size() ? static_cast<float>(g->rules().types[ps.type].credit_value) : 0.0f;
                for (const galactic::IncomeEntry& e : ps.income) sum += e.value;
                float siphoned = 0.0f;
                const float net = g->frame() <= ps.halt_until ? 0.0f
                                  : galactic::econ::income_finish(sum, ps.min_income_set, ps.min_income, ps.max_income_set, ps.max_income, false, 0.0f, &siphoned);
                i.income = static_cast<int64_t>(std::lround(net));
                // g_planet_value: TEXT_TOOLTIP_CREDIT_FILTER "credit value of planets" = the planet base value (GALACTIC_ECONOMY §1.4)
                // [M: which number the text prints is read from the tooltip, no capture with the filter on].
                if (ps.type < g->rules().types.size()) i.credits = ps.destroyed ? g->rules().types[ps.type].destroyed_credit_value : ps.base_value;
            }
        }
        planets.push_back(std::move(i));
    }
    // ui4 t03 [port]: g_credit_bar level = the planet's credit value against the highest value on the map, 0..10.
    {
        int32_t top = 0;
        for (const ui::GalacticPlanetInfo& i : planets) top = std::max(top, i.credits);
        if (top > 0)
            for (ui::GalacticPlanetInfo& i : planets)
                if (i.credits >= 0) i.credit_level = static_cast<int32_t>(std::lround(10.0 * static_cast<double>(i.credits) / static_cast<double>(top)));
    }
    hero_filter_ = (ui_.galactic_hud.filter_bits() & ui::kFilterHero) != 0;  // read by planet_icons next frame
    f.planets = planets;
    hud_planet_ids_.clear();
    for (const ui::GalacticPlanetInfo& i : planets) hud_planet_ids_.push_back(i.planet);
    if (g) {
        f.credits = static_cast<int64_t>(g->credits(local_player()));
        f.day = g->galactic_day();
        f.tech = g->players()[static_cast<size_t>(local_player())].tech_level;
        // ui ask 7: population used / capacity and the income per fiscal cycle (the last cycle's gain) [port: per day].
        f.pop = g->population_used(local_player());
        f.pop_cap = g->population_capacity(local_player());
        f.income = static_cast<int64_t>(std::lround(g->last_income(local_player())));
        // HUD_GAPS §4: the day pips = the fiscal-cycle timer (mode frame - cycle start) / round(30 x Fiscal_Cycle_Time).
        f.cycle_fraction = galactic_cycle_fraction(*g);
        f.day_pip_flash = true;
    }
    f.faction_alt = ui_.faction_alt();
    f.shells_loaded = ui_.galactic_shells_ok;
    // Playtest 2 (base 24 / 26.jpg): the fleet organiser (Fleet 1..3 boxes) belongs to the zoomed planet view only - never
    // over the galaxy view, even with a fleet selected (ui ask 9's fleet-selection trigger opened it there).
    f.organize_open = gview_.camera().zoomed() && selected_planet_ != 0;
    // Playtest 3 (base 40.jpg): a selected fleet / fleet unit shows every planet's fleet-slot ovals.
    f.show_fleet_slots = !orders_.selection().empty();
    if (g) f.empty_slot_color = faction_rgba(*g, local_player(), false, ui::Rgba8{});
    f.paused = session_ && session_->pause_requested();
    for (ui::GalacticPlanetInfo& p : planets) p.selected = p.planet == selected_planet_;
    for (const ui::ShellSet::Shell& s : ui_.galactic_shells.shells())
        if (s.model_name.find("organize") != std::string::npos)
            ui_.galactic_shells.set_visible(s.component, f.organize_open && !planet_map_open_);
    // HUD_GAPS §4.2: the advanced planet map = the zoomed planet view [port]; map_shell is shown with it and its bones
    // (day_clock) re-applied to the layout.
    // GALACTIC_DIALOGS §4: the advanced planet map is the FoC planet map entered by the organiser's advanced_planetary
    // ("Base Layout") button - not the zoomed planet view. [port] it closes when the close-up or the selection ends; the
    // map screen itself is not built yet (only map_shell is shown).
    if (planet_map_open_ && (!gview_.camera().zoomed() || selected_planet_ == 0)) planet_map_open_ = false;
    f.advanced_map_open = planet_map_open_;
    if (f.advanced_map_open != map_shell_open_) {
        map_shell_open_ = f.advanced_map_open;
        for (const ui::ShellSet::Shell& s : ui_.galactic_shells.shells())
            if (static_cast<size_t>(s.component) < ui_.components.size() && ui_.components[s.component].name == "map_shell")
                ui_.galactic_shells.set_visible(s.component, map_shell_open_);
        ui_.galactic_shells.apply(ui_.galactic_hud.layout());
    }
    // GALACTIC_DIALOGS §1 banner, §2.1 holocron flash, screen text ([port] each line 6 s, the last second fading).
    if (choice_open_) {
        f.battle.active = true;
        f.battle.single = prompt_.story_pending || prompt_.no_autoresolve;
        f.battle.space = !prompt_.land;
        f.battle.planet = prompt_.planet_name;
        f.battle.left = {prompt_.left.icon, prompt_.left.units, static_cast<int32_t>(prompt_.left.stacks)};
        f.battle.right = {prompt_.right.icon, prompt_.right.units, static_cast<int32_t>(prompt_.right.stacks)};
    }
    f.holocron_flash = holocron_flash_;
    screen_text_.clear();
    // t47 [port]: GALACTIC_DIALOGS §3.1 posts the red result line and opens the battle-end screen in one step; the screen fills
    // the view and the line would sit on its header, so the line waits (clock held) and shows for its 6 s once the screen closes.
    if (battle_end_open_)
        for (ScreenLine& l : screen_lines_) l.at = now_s_;
    std::erase_if(screen_lines_, [this](const ScreenLine& l) { return now_s_ - l.at > 6.0; });
    for (const ScreenLine& l : screen_lines_) {
        if (battle_end_open_) break;
        const double left = 6.0 - (now_s_ - l.at);
        screen_text_.push_back({l.text, l.color, static_cast<uint8_t>(std::clamp(left, 0.0, 1.0) * 255.0)});
    }
    f.screen_text = screen_text_;
    f.time = now_s_;
    f.cursor_px = {static_cast<float>(input_.mouse_x()), static_cast<float>(input_.mouse_y())};
    // HUD_GAPS §6: no galactic hover (planet rings, tooltips) under an open GUI dialog (the story dialog).
    if (ui_.gui.hit(input_.mouse_x(), input_.mouse_y()) || bench_active()) f.cursor_px = {-1.0e6f, -1.0e6f};  // BM2a: no hover
    if (hud_hit_debug_) {
        const bool fixed = hud_hit_px_ >= 0;
        const int32_t px = fixed ? hud_hit_px_ : input_.mouse_x(), py = fixed ? hud_hit_py_ : input_.mouse_y();
        const int32_t before = hud_hit_last_;
        hud_hit_dump("galactic", ui_.galactic_hud.hit_test(px, py), px, py);
        if (fixed && before != hud_hit_last_) {
            std::printf("hud hit: galactic shells visible:");
            for (const ui::ShellSet::Shell& s : ui_.galactic_shells.shells())
                if (s.visible && static_cast<size_t>(s.component) < ui_.components.size())
                    std::printf(" %s", ui_.components[s.component].name.c_str());
            std::printf(" (planet %u, selection %zu)\n", selected_planet_, orders_.selection().size());
        }
    }
    const GameSession* s = session_.get();
    const std::vector<ui::HudTypeInfo>* types = &ui_.hud_types;
    // UI.md §7.5: every fleet in orbit that the local player may see, with its slot and enemy flag; the ui places them in the
    // three panes. The visibility is the map stacks' rule (planet_icons): own and allied fleets always, others with
    // FleetPresence and not all-stealthed, their units only with FleetContents (else one blank unit, as the map's single stack).
    f.providers.fleets = [s, types](uint32_t planet, std::vector<ui::GalacticFleet>& out) {
        const galactic::GalacticSim* gs = s ? s->galaxy() : nullptr;
        if (!gs) return;
        const PlanetIndex row = planet_index(*gs, planet);
        if (row == kInvalidPlanet) return;
        using PI = galactic::GalacticSim::PlanetInfo;
        const int32_t me = s->local_player();
        const bool presence = gs->can_see_planet_info(me, row, PI::FleetPresence);
        const bool contents = gs->can_see_planet_info(me, row, PI::FleetContents);
        const objects::ObjectStore& st = gs->store();
        for (const galactic::FleetState& fl : gs->fleets()) {
            if (!fl.alive || fl.planet != row || fl.contents.empty()) continue;
            const bool own = me >= 0 && fl.owner >= 0 && gs->allied(me, fl.owner);
            if (!own && !presence) continue;
            ui::GalacticFleet gf;
            gf.fleet = st.alive(fl.object) ? static_cast<uint32_t>(st.id(fl.object)) : 0u;
            gf.slot = fl.slot;
            gf.enemy = fl.owner != me;
            bool all_stealth = true;
            for (const objects::ObjectHandle m : fl.contents) {
                if (!st.alive(m)) continue;
                all_stealth = all_stealth && gs->hidden(m);
                if (!own && !contents) {
                    if (gf.units.empty()) gf.units.push_back({0u, std::string(), 1, false});
                    continue;
                }
                const objects::TypeIndex t = st.type(m);
                gf.units.push_back({static_cast<uint32_t>(st.id(m)), t < types->size() ? (*types)[t].icon_name : std::string(), 1, false, t});
            }
            if (gf.units.empty() || gf.fleet == 0 || (!own && all_stealth)) continue;
            out.push_back(std::move(gf));
        }
    };
    // OI host: build bar = GalacticSim::buildable (type-index order) at the planet for the local player; cost =
    // GalacticSim::build_cost_for at the planet (ui4 t02, A2K_T01 §1: planet / player price modifiers such as the Emperor's
    // discount, the corruption price multiplier, the tech-upgrade multiplier), block = GalacticSim::can_enqueue's reason
    // (queue full, pop cap, funds; const reads, no side effects), enabled = no block, queued = the local player's entries of
    // the type in the planet's queues; structure = ground / orbital structure or starbase (b_Create_NN);
    // production_disabled = the player's production-disabled list (REMAINING_GAPS §1.5).
    f.providers.build_options = [s, types](uint32_t planet, std::vector<ui::GalacticBuildOption>& out) {
        const galactic::GalacticSim* gs = s ? s->galaxy() : nullptr;
        if (!gs) return;
        const PlanetIndex pi = planet_index(*gs, planet);
        if (pi == kInvalidPlanet) return;
        out = galactic_build_options(*gs, s->local_player(), pi,
                                     [types](uint32_t t) { return t < types->size() ? (*types)[t].icon_name : std::string(); });
    };
    // GALACTIC_DIALOGS §5.2: the selected planet's build queues (the local player's entries, head first): space queue on
    // queue00..04, ground queue on queue05..09 (HudQueueEntry::land); the head's timer gives the dial. A click cancels
    // through Production {cancel, queue, index} at the planet.
    f.providers.build_queue = [s, types, sel = selected_planet_](std::vector<ui::HudQueueEntry>& out) {
        const galactic::GalacticSim* gs = s ? s->galaxy() : nullptr;
        if (!gs || sel == 0) return;
        const PlanetIndex pi = planet_index(*gs, sel);
        if (pi == kInvalidPlanet) return;
        const int32_t me = s->local_player();
        const int32_t now = gs->frame();
        for (const uint32_t q : {static_cast<uint32_t>(galactic::kSpaceQueue), static_cast<uint32_t>(galactic::kGroundQueue)}) {
            const std::span<const galactic::BuildEntry> queue = gs->queue_of(pi, q);
            for (size_t i = 0; i < queue.size() && i <= 0xFFu; ++i) {
                const galactic::BuildEntry& b = queue[i];
                if (b.player != me || b.type > 0xFFFFu) continue;
                ui::HudQueueEntry e;
                e.type_index = static_cast<uint16_t>(b.type);
                e.icon = b.type < types->size() ? (*types)[b.type].icon_name : std::string();
                e.progress = (i == 0 && b.duration > 0)
                                 ? std::clamp(static_cast<float>(now - b.start) / static_cast<float>(b.duration), 0.0f, 1.0f)
                                 : 0.0f;
                e.location = sel;
                e.queue = static_cast<uint8_t>(q);
                e.index = static_cast<uint8_t>(i);
                e.land = q == galactic::kGroundQueue;
                out.push_back(std::move(e));
            }
        }
    };
    // OI host: g_special_ability rows = the local player's Ground_Activated special-ability units at the planet.
    f.providers.special_abilities = [this](uint32_t planet, std::vector<ui::GalacticSpecialAbility>& out) {
        const galactic::GalacticSim* gs = galaxy();
        if (!gs) return;
        const objects::ObjectStore& st = gs->store();
        for (const objects::ObjectHandle h : special_units(*gs, planet_index(*gs, planet))) {
            ui::GalacticSpecialAbility r;
            r.hero = static_cast<uint32_t>(st.id(h));
            const objects::TypeIndex t = st.type(h);
            r.text = type_label(data_, ui_.text, t);
            if (t < ui_.hud_types.size()) r.icon = ui_.hud_types[t].icon_name;
            out.push_back(std::move(r));
        }
    };
}

void App::build_tactical_hud() {
    const sim::TacticalMode* tm = session_ ? session_->host().modes().top_tactical() : nullptr;
    const uint32_t w = targets_.width, h = targets_.height;
    ui::TacticalHudFrame hf;
    hf.world.view = ui::make_view(bview_.camera_params(w, h), w, h);
    hf.world.land = !bview_.space();
    hf.world.local_player = local_player();
    // UI.md selection circle: the owner faction's Land_ / Space_Mode_Selection_Blob_Material_Name (modc t20: by data).
    if (const galactic::GalacticSim* gs = galaxy())
        hf.world.selection_material = [gs](int32_t owner, bool land, bool) {
            if (owner < 0 || static_cast<size_t>(owner) >= gs->players().size()) return std::string();
            const uint32_t f = gs->players()[static_cast<size_t>(owner)].faction;
            if (f >= gs->rules().factions.size()) return std::string();
            const galactic::FactionRules& fr = gs->rules().factions[f];
            return land ? fr.land_selection_blob : fr.space_selection_blob;
        };
    if (tm && tm->bounds().max.x - tm->bounds().min.x < 1.0e5f) {
        hf.map_min_x = tm->bounds().min.x;
        hf.map_min_y = tm->bounds().min.y;
        hf.map_max_x = tm->bounds().max.x;
        hf.map_max_y = tm->bounds().max.y;
    }
    if (const galactic::GalacticSim* g = galaxy()) {
        hf.credits = static_cast<int64_t>(g->credits(local_player()));
        // ui ask 7: the battle's planet, tech level and population (used / space unit cap).
        hf.tech = g->players()[static_cast<size_t>(local_player())].tech_level;
        if (const sim::TacticalBattle* b = session_->battle()) {
            if (b->request().info.planet < g->planets().size()) {
                const uint32_t pt = display_planet_type(*g, b->request().info.planet);
                hf.planet_name = pt != g->planets()[b->request().info.planet].type && pt < data_.types.size()
                                     ? type_label(data_, ui_.text, static_cast<objects::TypeIndex>(pt))
                                     : planet_label(data_, ui_.text, *g, b->request().info.planet);
            }
            hf.pop = static_cast<int32_t>(std::lround(b->population_used(local_player())));
            const float cap = b->population_cap(local_player());
            hf.pop_cap = cap >= 0.0f ? static_cast<int32_t>(std::lround(cap)) : -1;
        }
    }
    // ui pass 2: the mission objectives box - the story's objectives, else the skirmish win condition: ui3 t07 the retail
    // TEXT_TACTICAL_PRIMARY_SPACE_ATTACK / _LAND_ATTACK (the HUD numbers the lines itself).
    hud_objectives_.clear();
#if defined(ALAMO_SIM_HAVE_STORY) && defined(ALAMO_UI_HAVE_LUA)
    if (const sim::StorySession* story = session_ ? session_->host().story() : nullptr)
        if (const lua::StoryModel* m = story->tactical_model()) hud_objectives_ = ui::hud_objectives(*m);
#endif
    if (hud_objectives_.empty() && session_ && session_->spec().kind == SessionKind::Skirmish) {
        ui::HudObjective o;
        o.text_key = bview_.space() ? "TEXT_TACTICAL_PRIMARY_SPACE_ATTACK" : "TEXT_TACTICAL_PRIMARY_LAND_ATTACK";
        hud_objectives_.push_back(std::move(o));
    }
    hf.objectives = hud_objectives_;
    hf.faction_alt = ui_.faction_alt();
    hf.shells_loaded = ui_.tactical_shells_ok;
    hf.time = now_s_;
    hf.cursor_px = {static_cast<float>(input_.mouse_x()), static_cast<float>(input_.mouse_y())};
    // Drag-select box: the world-owned left drag that OrderTranslator box-selects on release (same rect and threshold).
    hf.drag_select = input_.button(ui::Button::Left).dragging;
    hf.drag_rect = input_.button(ui::Button::Left).drag;
    std::vector<ui::HudUnit> units;
    const std::vector<uint32_t> sel = orders_.selection();
    // ui asks 3 / 4: the bracket radius from the model's bounding sphere x scale; one unit icon per squadron / unit.
    ui::hud_units_from_frame(bview_.frame(), sel, {}, 0, [this](objects::TypeIndex t) { return bview_.type_radius(t); }, units);
    tactical_hover(hf, units);
    if (const sim::TacticalBattle* b = battle()) {
        std::vector<ui::UnitIconLink> icons;  // QA U7: one marker per squadron, at its centre
        for (const sim::TacticalBattle::IconLink& l : b->icon_links())
            icons.push_back({static_cast<uint32_t>(l.unit), static_cast<uint32_t>(l.squad)});
        ui::assign_unit_icons(units, icons);
    }
    // Pause look, radar picture (fog of war), queue markers of the selected units (sim queued_orders, const reads).
    hf.paused = session_ && session_->pause_requested();
    update_radar();
    if (!radar_.empty()) hf.radar_image = &radar_;
    hf.radar_events = &radar_events_;
    hf.radar_settings = &radar_settings_;
    hf.radar_event_models = ui_.radar_models_ok;  // the event .alo models go through ui_.radar_drawer (Models2 step)
    radar_events_.update(dt_);
    queues_.clear();
    const std::vector<render::SnapshotObject>& objs = bview_.frame().objects;  // ascending id
    auto obj_of = [&objs](objects::ObjectId id) -> const render::SnapshotObject* {
        const auto it = std::lower_bound(objs.begin(), objs.end(), id, [](const render::SnapshotObject& x, objects::ObjectId v) { return x.id < v; });
        return it != objs.end() && it->id == id ? &*it : nullptr;
    };
    if (const sim::TacticalBattle* b = battle())
        for (const uint32_t id : sel) {
            const auto q = b->queued_orders(static_cast<objects::ObjectId>(id));
            const render::SnapshotObject* o = obj_of(static_cast<objects::ObjectId>(id));
            if (q.empty() || !o) continue;
            ui::HudOrderQueue hq;
            hq.unit = id;
            hq.position = {o->position.x, o->position.y, o->position.z};
            using QK = sim::TacticalBattle::QueuedOrder::Kind;
            for (const sim::TacticalBattle::QueuedOrder& e : q) {
                ui::HudQueuedStep s;
                s.kind = e.kind == QK::Attack ? ui::HudQueuedStep::Kind::Attack
                         : e.kind == QK::Ability ? ui::HudQueuedStep::Kind::Ability
                         : e.kind == QK::Move    ? ui::HudQueuedStep::Kind::Move
                                                 : ui::HudQueuedStep::Kind::Other;
                if (e.kind == QK::Move) {
                    s.point = {e.x, e.y, o->position.z};
                } else if (const render::SnapshotObject* tg = obj_of(e.target)) {
                    s.point = {tg->position.x, tg->position.y, tg->position.z};
                } else {
                    s.has_point = false;
                }
                hq.steps.push_back(s);
            }
            queues_.push_back(std::move(hq));
        }
    hf.order_queues = queues_;
    // Movement path lines (HudWaypointLine dots): every selected local unit with queued move points - its position, its
    // current movement goal, then those points (sim const reads between ticks). [port] goal height = the unit's.
    // [port] A plain move draws no line (playtest video R1: with a whole fleet selected, one 3 px dotted line per fighter
    // covered the battle and read as laser fire); the original's waypoint line belongs to the Alt-queued path (UI.md 1.5).
    waypoints_.clear();
    if (const sim::TacticalBattle* b = battle())
        for (const uint32_t id : sel) {
            const auto oid = static_cast<objects::ObjectId>(id);
            const render::SnapshotObject* o = obj_of(oid);
            if (!o || o->owner != local_player() || !b->movement().has_unit(oid)) continue;
            const tactical::movement::UnitView v = b->movement().view(oid);
            if (!v.has_goal || v.arrived) continue;
            ui::HudWaypointLine l;
            l.points.push_back({o->position.x, o->position.y, o->position.z});
            l.points.push_back({v.goal.x, v.goal.y, o->position.z});
            for (const sim::TacticalBattle::QueuedOrder& e : b->queued_orders(oid)) {
                if (e.kind != sim::TacticalBattle::QueuedOrder::Kind::Move) continue;
                const ui::Vec3 p{e.x, e.y, o->position.z};
                if (p.x != l.points.back().x || p.y != l.points.back().y) l.points.push_back(p);
            }
            if (l.points.size() > 2) waypoints_.push_back(std::move(l));
        }
    hf.waypoints = waypoints_;
    // Reinforcement pad: the local player's undeployed reserves (TacticalBattle::reserves, const read between ticks).
    const GameSession* sess = session_.get();
    const std::vector<ui::HudTypeInfo>* htypes = &ui_.hud_types;
    hf.providers.reinforcements = [this, sess, htypes](std::vector<ui::HudReinforcement>& out) {
        const sim::TacticalBattle* b = sess ? sess->battle() : nullptr;
        if (!b) return;
        for (const sim::TacticalBattle::Reserve& r : b->reserves(sess->local_player())) {
            ui::HudReinforcement e;
            e.unit = static_cast<uint32_t>(r.galactic_id);
            e.icon = r.type < htypes->size() ? (*htypes)[r.type].icon_name : std::string();
            e.count = r.units;
            e.available = reinforce_available(r.galactic_id);  // ui3 t04: the unit cap / retreat / hold gate of order_reinforce
            out.push_back(std::move(e));
        }
    };
    // Tactical buy bar: the first selected own builder with production options (skirmish space stations), its options
    // with price / queued count / block reason, anchored at the builder's projected pixel.
    // q6 [port]: the buttons are the i_build_buttons model at the builder in the world (UI.md: tactical build pads), so a
    // builder behind the camera shows none. Before, ui::TacticalHud put them at its no-anchor stand-in point near the
    // screen centre, where they took the right clicks meant for the world (Thrawn's Revenge land smoke: a select-all
    // holding the Rebel barracks, the camera on an enemy). A builder in front of the camera is drawn as before.
    build_opts_.clear();
    if (const sim::TacticalBattle* b = battle(); b && tm && b->production_enabled()) {
        const objects::ObjectStore& st = tm->primary();
        for (const uint32_t id : orders_.selection()) {
            if (id > 0x7FFFFFFFu) continue;
            const auto opts = b->station_build_options(st, local_player(), static_cast<objects::ObjectId>(id));
            if (opts.empty()) continue;
            const Vec3 p = st.position(st.find(static_cast<objects::ObjectId>(id)));
            float sx = 0.0f, sy = 0.0f;
            if (!ui::project(hf.world.view, {p.x, p.y, p.z}, sx, sy)) {
                ++buy_bar_behind_camera_;
                break;
            }
            hf.has_build_anchor = true;
            hf.build_anchor_px = {sx, sy};
            hf.builder = id;
            for (const sim::TacticalBattle::BuildOptionInfo& o : opts) {
                ui::HudBuildOption e;
                e.type_index = static_cast<uint16_t>(o.type);
                e.icon = static_cast<size_t>(o.type) < ui_.hud_types.size() ? ui_.hud_types[static_cast<size_t>(o.type)].icon_name : std::string();
                e.cost = o.cost;
                e.queued = o.queued;
                e.block = static_cast<ui::HudBuildOption::Block>(o.block);
                // ui3 t07: a buyable option's tooltip = the type's name text (Text_ID) [port: the original's unit tooltip
                // also draws the price / icon rows of tooltip_price / tooltip_icon]
                if (static_cast<size_t>(o.type) < data_.types.size())
                    e.tooltip_key = std::string(ascii_trim(data_.types.get_string(static_cast<objects::TypeIndex>(o.type), "Text_ID")));
                build_opts_.push_back(std::move(e));
            }
            break;
        }
    }
    hf.build_options = build_opts_;
    // Build queue: the local player's skirmish station production orders (queue order); index = the order's position
    // among the player's orders at its builder (the Production cancel index).
    hf.providers.build_queue = [sess, htypes](std::vector<ui::HudQueueEntry>& out) {
        const sim::TacticalBattle* b = sess ? sess->battle() : nullptr;
        if (!b || !b->production_enabled()) return;
        const auto orders = b->production()->queue(sess->local_player());
        for (size_t i = 0; i < orders.size(); ++i) {
            const tactical::hangar::BuildOrder& o = orders[i];
            if (o.type < 0) continue;
            uint8_t index = 0;
            for (size_t k = 0; k < i; ++k)
                if (orders[k].builder == o.builder) ++index;
            ui::HudQueueEntry e;
            e.type_index = static_cast<uint16_t>(o.type);
            e.icon = static_cast<size_t>(o.type) < htypes->size() ? (*htypes)[static_cast<size_t>(o.type)].icon_name : std::string();
            e.progress = o.total_frames > 0 ? 1.0f - static_cast<float>(o.frames_left) / static_cast<float>(o.total_frames) : 0.0f;
            e.location = static_cast<uint32_t>(o.builder);
            e.index = index;
            out.push_back(std::move(e));
        }
    };
#ifdef ALAMO_UI_HAVE_ABILITIES
    // Command bar ability buttons (special_button_NN) of the selection: S9 read-only queries between ticks.
    if (const sim::TacticalBattle* b = battle())
        hf.providers.abilities = ui::s9_ability_provider(b->abilities(), [b] { return b->frame(); });
#endif
    hf.hardpoints = hud_hardpoints_;
    hf.reticles = &reticles_;
    hf.images = &hud_images_;
    // HUD_GAPS §5: order buttons from the local player's own selected units; story / Lua Lock_Controls disables all.
    {
        std::vector<OrderUnitState> st;
        const sim::TacticalBattle* b = battle();
        for (const uint32_t id : sel) {
            const render::SnapshotObject* o = obj_of(static_cast<objects::ObjectId>(id));
            if (!o) continue;
            OrderUnitState u;
            u.owner = o->owner;
            u.can_attack = b && b->combat().can_attack(static_cast<tactical::combat::ObjectId>(id));
            u.can_move = b && b->movement().has_unit(static_cast<objects::ObjectId>(id));
            st.push_back(u);
        }
        hf.order_buttons = order_buttons(st, local_player(), story_controls_locked());
    }
    // cu9g t03 (HUD_GAPS §9): the land b_special_weapon = the bombing-run button (disabled, icon 3, tooltip 3, cooldown dial).
    if (const sim::TacticalBattle* tb = battle()) {
        hf.bombing_run = bombing_run_button(!bview_.space(), story_controls_locked(), tb->bombing_run_available(local_player()),
                                            tb->bombing_run_cooldown_fraction(local_player()));
        if (bview_.space()) {  // ui1 t04 (HUD_GAPS §9.1): the space special-weapon button
            int32_t icon = -1;
            float ready = 0.0f;
            space_weapon(icon, ready);
            hf.bombing_run = space_weapon_button(true, icon, tb->special_weapons_allowed(local_player()), story_controls_locked(), ready);
        }
    }
    fill_command_buttons(hf);  // ui3 t03 / t04: b_retreat / b_reinforcement / beacon / movie mode, reinforcement texts
    hud_units_ = units;
    // FIXPASS_GALACTIC §4.2: the advisor *_LOOP Bink in Movie_tactical (the HUD's own component), only while it plays.
    hf.advisor_page = advisor_loop_shown() ? static_cast<int32_t>(kAdvisorPage) : -1;
    hf.advisor_alpha = advisor_movie_.alpha();
    ui_.tactical_hud.build(hf, units, ui_.hud_types);
    if (ui_.tactical_hud.stats().reticles > 0) ++reticle_frames_;
}

// HUD_GAPS §1.1 / §2.2: the hovered unit = the world pick 0x36600 (refixf t08: pick_world with world_pick_tests(), the
// precise model test then the Mouse_Collide_Override_Sphere_Radius sphere, the same [port] rules as the order picks;
// neutral objects skipped), replaced by the owner of the reticle under the cursor (the last build's reticles: what the
// player sees) so the set does not flicker; nothing over the HUD or a dialog. Then the hovered ship's hard-point
// reticles (§1.2: live targetable ones; destroyed ones get none) with their tint / texture inputs (§1.4 / §1.5) and
// flash start (§1.3).
void App::tactical_hover(ui::TacticalHudFrame& hf, std::vector<ui::HudUnit>& units) {
    const int32_t mx = input_.mouse_x(), my = input_.mouse_y();
    const bool over_ui = routed_.hover_component >= 0 || ui_.gui.hit(mx, my);
    uint32_t hover = 0;
    int32_t reticle = -1;
    if (!over_ui && !bench_active()) {  // BM2a: nothing hovered in a benchmark run
        reticle = ui::reticle_at(ui_.tactical_hud.canvas(), hf.world.view, reticles_, hud_hardpoints_, static_cast<float>(mx),
                                 static_cast<float>(my), now_s_);
        if (reticle >= 0) hover = hud_hardpoints_[static_cast<size_t>(reticle)].parent;
        else {
            // Only objects with an encyclopedia identity (a HUD type with an Icon_Name): props such as the space
            // backdrops (Forest_Backdrop_Large: a huge bounding sphere owned by the neutral player) are never hovered.
            // [port] The candidates are those HUD units (owned, with an icon) among the world pick's tree candidates.
            std::vector<uint32_t> pickable;
            pickable.reserve(units.size());
            for (const ui::HudUnit& u : units)
                if (u.owner >= 0 && u.type < ui_.hud_types.size() && !ui_.hud_types[u.type].icon_name.empty()) pickable.push_back(u.id);
            std::sort(pickable.begin(), pickable.end());
            const sim::TacticalMode* tm = session_ ? session_->host().modes().top_tactical() : nullptr;
            if (const ui::WorldPickTests* base = tm ? world_pick_tests() : nullptr) {
                ui::WorldPickTests tests = *base;
                tests.skip = [&pickable, inner = base->skip](uint32_t id) {
                    return !std::binary_search(pickable.begin(), pickable.end(), id) || (inner && inner(id));
                };
                tactical::QueryContext qc;
                if (const auto hit = ui::pick_world(tm->trees().all(), hf.world.view, ui::pixel_ray(hf.world.view, mx, my),
                                                    tactical::kSkipDead, qc, &tests))
                    hover = hit->id;
            }
        }
    }
    const HardPointRef hovered_ref = reticle >= 0 ? hud_hp_refs_[static_cast<size_t>(reticle)] : HardPointRef{};
    hover_ship_ = hover;
    for (ui::HudUnit& u : units) u.hovered = hover != 0 && u.id == hover;
    hud_hardpoints_.clear();
    hud_hp_refs_.clear();
    const sim::TacticalBattle* b = battle();
    if (!b || hover == 0 || !bview_.space()) return;
    const tactical::combat::CombatSystem& cs = b->combat();
    const auto id = static_cast<tactical::combat::ObjectId>(hover);
    const uint32_t n = cs.hardpoint_count(id);
    if (n == 0) return;
    const tactical::combat::CombatData& cd = cs.data();
    const uint32_t desc = cs.desc_index(id);
    const int32_t owner = cs.owner(id);
    const bool enemy = owner != local_player() && cs.is_enemy(local_player(), owner);
    ui::Vec3 pos = cs.position(id);
    for (const ui::HudUnit& u : units)
        if (u.id == hover) pos = u.position;  // the interpolated draw position
    const ui::Vec3 facing = cs.facing(id);
    for (uint32_t hp = 0; hp < n; ++hp) {
        if (!cs.hardpoint_targetable(id, hp)) continue;  // destroyed or not Is_Targetable (§1.2)
        const bool disabled = cs.hardpoint_disabled(id, hp);  // HARDPOINT_DISABLED.md: grey / Friendly_Disabled
        if (enemy && disabled) continue;                       // §1.2: no reticle for an enemy ship's disabled hard point
        ui::Vec3 offset{};
        if (desc < cd.units.size() && hp < cd.units[desc].hardpoints.size() && cd.units[desc].hardpoints[hp] < cd.hardpoints.size())
            offset = cd.hardpoints[cd.units[desc].hardpoints[hp]].attach_offset;
        const auto kind = static_cast<uint8_t>(cs.hardpoint_kind(id, hp));
        ui::HudHardPoint p;
        p.position = hardpoint_world(pos, facing, offset);
        p.type = hardpoint_type_name(kind);
        p.enemy = enemy;
        p.parent = hover;
        const float mx_hp = cs.hardpoint_max(id, hp);
        p.health = mx_hp > 0.0f ? std::clamp(cs.hardpoint_health(id, hp) / mx_hp, 0.0f, 1.0f) : 1.0f;
        p.disabled = disabled;
        p.repairing = !enemy && cs.hardpoint_repaired_by(id, hp, local_player());  // §1.3 blink: repaired by the local player
        p.tracked = hovered_ref.ship == hover && hovered_ref.index == hp && reticle >= 0;
        const auto f = hp_flash_.find((static_cast<uint64_t>(hover) << 8) | hp);
        p.flash_start = f != hp_flash_.end() ? f->second : -1.0;
        hud_hardpoints_.push_back(std::move(p));
        hud_hp_refs_.push_back({hover, hp, kind});
    }
}

// HUD_GAPS §1.6: a world order while the cursor is on a reticle targets the reticle's ship with that hard point - an
// attack carries ref2 = 1 + HardPointType (the sim's first targetable hard point of that type), a targeted ability its
// target; both flash the reticle. An own / allied damaged hard point that is not being repaired takes a right click as
// RepairHardpoint (command 45) when some unit of the local player's selection can repair hard points (HUD_GAPS §1.4:
// CombatSystem::can_repair_hardpoints, DUMMY_STAR_BASE). Returns true when the orders were rewritten.
bool App::reticle_orders(const ui::OrderContext& ctx, std::vector<commands::EventData>& cmds) {
    const ui::ButtonState& rb = input_.button(ui::Button::Right);
    const ui::ButtonState& lb = input_.button(ui::Button::Left);
    const bool rclick = rb.click && !rb.in_double_press;  // the release after a double press is swallowed (spec §1.1)
    if (!rclick && !lb.click) return false;
    const int32_t mx = input_.mouse_x(), my = input_.mouse_y();
    if (routed_.hover_component >= 0 || ui_.gui.hit(mx, my) || orders_.selection().empty()) return false;
    const int32_t r = ui::reticle_at(ui_.tactical_hud.canvas(), ctx.view, reticles_, hud_hardpoints_, static_cast<float>(mx),
                                     static_cast<float>(my), now_s_);
    if (r < 0) return false;
    const ui::HudHardPoint& hp = hud_hardpoints_[static_cast<size_t>(r)];
    const HardPointRef ref = hud_hp_refs_[static_cast<size_t>(r)];
    bool changed = false;
    if (rclick && selection_can_repair() && !hp.enemy && hp.health < 1.0f && !hp.repairing) {
        cmds.clear();
        commands::EventData e(commands::EventType::RepairHardpoint);
        e.set("object", ref.ship).set("hardpoint", ref.index).set("player", static_cast<uint32_t>(local_player()));
        cmds.push_back(std::move(e));
        ++reticle_orders_;
        return true;
    }
    for (commands::EventData& e : cmds) {
        if (e.type() == commands::EventType::Attack) {
            e.set("target", ref.ship).set("ref2", 1u + ref.kind);
            changed = true;
        } else if (e.type() == commands::EventType::TacticalSpecialAbility ||
                   e.type() == commands::EventType::TacticalSpecialAbilityWithDummyTarget) {
            e.set("target", ref.ship);
            changed = true;
        }
    }
    if (!changed && rclick && hp.enemy) {  // the translator saw no attack (cursor on the reticle, off the hull)
        std::erase_if(cmds, [](const commands::EventData& e) {
            using T = commands::EventType;
            const T t = e.type();
            return t == T::MoveToPosition || t == T::MoveToObject || t == T::MoveToPositionFacing || t == T::MoveToRay ||
                   t == T::MoveToRayFacing || t == T::Facing;
        });
        commands::EventData a = commands::ev::attack(static_cast<commands::ObjectId>(ref.ship), {});
        a.set("ref2", 1u + ref.kind);
        a.set_queued(input_.shift());
        cmds.push_back(std::move(a));
        changed = true;
    }
    if (changed) {
        hp_flash_[(static_cast<uint64_t>(ref.ship) << 8) | ref.index] = now_s_;
        ++reticle_orders_;
    }
    return changed;
}

// HUD_GAPS §1.4: OR of CombatSystem::can_repair_hardpoints over the local player's selected units.
bool App::selection_can_repair() const {
    const sim::TacticalBattle* b = battle();
    if (!b) return false;
    for (const uint32_t u : orders_.selection()) {
        const auto uid = static_cast<tactical::combat::ObjectId>(u);
        if (b->combat().owner(uid) == local_player() && b->combat().can_repair_hardpoints(uid)) return true;
    }
    return false;
}

void App::hud_hit_dump(const char* where, int32_t component, int32_t px, int32_t py) {
    if (component == hud_hit_last_) return;
    hud_hit_last_ = component;
    const char* name = component >= 0 && static_cast<size_t>(component) < ui_.components.size() ? ui_.components[static_cast<size_t>(component)].name.c_str() : "-";
    std::printf("hud hit (%s): %d %s at %d,%d\n", where, component, name, px, py);
}

void App::sync_os_cursor() {
    void* want = nullptr;
    if (hardware_mouse_ && cursor_ok_ && !opt_.hidden) {
        const std::string tex = cursor_.texture(now_s_);
        const ui::CursorDef* def = cursor_.current_def();
        if (!tex.empty() && def) {
            auto it = hw_cursors_.find(tex);
            if (it == hw_cursors_.end()) {
                void* made = nullptr;
                std::vector<uint8_t> bytes;
                for (const std::string& c : assets::texture_candidates(tex)) {
                    if (!data_.files->read(c, bytes)) continue;
                    assets::TextureAsset t;
                    if (assets::decode_texture(std::move(bytes), t) && !t.levels.empty() &&
                        (t.format == assets::TextureFormat::BGRA8 || t.format == assets::TextureFormat::RGBA8)) {
                        const std::span<const uint8_t> px = t.level_bytes(t.levels[0]);
                        std::vector<uint8_t> bgra(px.begin(), px.end());
                        if (t.format == assets::TextureFormat::RGBA8)
                            for (size_t k = 0; k + 3 < bgra.size(); k += 4) std::swap(bgra[k], bgra[k + 2]);
                        if (bgra.size() >= static_cast<size_t>(t.width) * t.height * 4u)
                            made = create_os_cursor(bgra.data(), t.width, t.height, def->hot_x, def->hot_y);
                    }
                    break;
                }
                if (!made) std::printf("hardware mouse: no cursor from %s (arrow)\n", tex.c_str());
                it = hw_cursors_.emplace(tex, made).first;
            }
            want = it->second;
        }
    }
    set_os_cursor_image(win_.native_handle(), want);
}

}  // namespace alamo_game_app
