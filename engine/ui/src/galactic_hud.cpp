#include "alamo/ui/galactic_hud.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "alamo/core/ascii.hpp"
#include "alamo/data/values.hpp"
#include "alamo/ui/gui_dialog.hpp"
#include "alamo/ui/hud_font.hpp"
#include "alamo/ui/hud_radar.hpp"
#include "ui_strings.hpp"

namespace alamo::ui {

namespace {

// Credits symbol + space before HUD credit amounts: '$' is drawn as the credits sign by the EmpireAtWar fonts.
constexpr const char32_t* kCreditsPrefix = U"$ ";

// [port: Gabriel, original screenshot] credit amounts with a thousands separator ("13,000").
std::u32string grouped32(int64_t v) {
    const std::u32string d = num32(v < 0 ? -v : v);
    std::u32string o;
    if (v < 0) o.push_back(U'-');
    for (size_t i = 0; i < d.size(); ++i) {
        if (i > 0 && (d.size() - i) % 3 == 0) o.push_back(U',');
        o.push_back(d[i]);
    }
    return o;
}

Rgba8 with_alpha(Rgba8 c, uint8_t a) {
    c.a = static_cast<uint8_t>((static_cast<uint32_t>(c.a) * a + 127) / 255);
    return c;
}

const std::string* pick(const std::vector<std::string>& v, int32_t i) {
    if (v.empty()) return nullptr;
    const size_t k = i < 0 ? 0 : std::min(static_cast<size_t>(i), v.size() - 1);
    return &v[k];
}

constexpr Rgba8 kGalacticRadarBackdrop{6, 10, 22, 255};  // [port] radar ground under the faceplate (build_radar)
// §5.2 queue00..09 bone x (i_galactic_controls.alo, canvas units from the left edge).
constexpr float kQueueDomeX[10] = {343.0f, 410.0f, 475.0f, 539.9f, 607.9f, 699.0f, 765.0f, 832.0f, 898.0f, 960.0f};
// §1.2 top-anchored attack-mode shells: model y 92 sits on the screen's top edge [port: 28.jpg].
constexpr float kAttackShellTop = 92.0f;
// §5.1 build pad cost text [port: green sampled by eye from 25.jpg; unaffordable red is L].
constexpr Rgba8 kCostAffordable{96, 236, 96, 255};
constexpr Rgba8 kCostUnaffordable{236, 72, 64, 255};
// GALAXY_MAP_PRESENTATION §1.4: planet fleet badges and the Ships_Per_Stack divisor (GameConstants, 5 in FoC [M]).
constexpr const char* kStealthBadge = "i_icon_stealth.tga";
constexpr const char* kRaidBadge = "i_icon_raid.tga";
constexpr uint32_t kShipsPerStack = 5;

}  // namespace

float planet_label_alpha(int32_t zoom_state, float z) {
    switch (zoom_state) {
        case 1: return 255.0f * std::clamp(1.0f - 10.0f * z, 0.0f, 1.0f);
        case 2: return 255.0f * std::clamp(10.0f * (z - 0.9f), 0.0f, 1.0f);  // [port] clamped at 255
        case 3: return 0.0f;
        default: return 255.0f;  // [port] no zoom state
    }
}

void apply_galactic_fallback_layout(const ComponentTable& table, const HudCanvas& cv, HudLayout& layout) {
    const float W = 0.5f * cv.width, H = 0.5f * cv.height;
    auto put = [&](std::string_view name, float x, float y) {
        const int32_t i = table.find(name);
        if (i >= 0) layout.set_fallback(static_cast<uint32_t>(i), {x, y});
    };
    put("Text_Credits", W - 70.0f, H - 14.0f);
    put("Text_Day_Credits", W - 70.0f, H - 30.0f);  // [port]
    put("Text_Pop_Cap", W - 190.0f, H - 14.0f);
    put("Text_Tech_Level", W - 280.0f, H - 14.0f);
    put("Text_Day_Counter", -W + 150.0f, H - 14.0f);
    put("b_option_g", -W + 18.0f, H - 18.0f);
    put("b_camera_g", -W + 50.0f, H - 18.0f);
    put("b_play_pause", -W + 82.0f, H - 18.0f);
    put("b_fast_forward", -W + 112.0f, H - 18.0f);
    put("radar_galactic", -W + 92.0f, -H + 92.0f);
    put("Text_Planet", -60.0f, -H + 160.0f);
    put("Text_Planetary_Income", -60.0f, -H + 144.0f);
    put("Movie_galactic", -W + 51.0f, -H + 288.0f);  // i_galactic_controls.alo bone (FIXPASS_GALACTIC §4.2)
    // GALACTIC_DIALOGS §5.4: planet_info has no fallback: it shows only on its organiser / map_shell bone.
    for (int k = 0; k < 26; ++k) {  // GALACTIC_DIALOGS §5.1: 13 columns, 54-unit pitch from x 307; even = row 1, odd = row 2
        char n[32];
        std::snprintf(n, sizeof n, "b_Create_%02d", k);
        put(n, -W + 307.0f + 54.0f * static_cast<float>(k / 2), -H + (k % 2 == 0 ? 92.0f : 37.0f));
    }
    for (int k = 0; k < 10; ++k) {
        char n[32];
        std::snprintf(n, sizeof n, "queue%02d", k);
        put(n, -W + kQueueDomeX[k], -H + 174.0f);  // §5.2: the queue domes on the bar's top edge (bones queue00..09)
    }
    for (int f = 0; f < 3; ++f) {
        char n[32];
        std::snprintf(n, sizeof n, "q%d_fleet", f + 1);
        put(n, -W + 200.0f, 150.0f - 100.0f * static_cast<float>(f));
        for (int r = 0; r < 5; ++r)
            for (int c = 0; c < 7; ++c) {
                std::snprintf(n, sizeof n, "q%d_%02d%02d", f + 1, r, c);
                put(n, -W + 250.0f + 44.0f * static_cast<float>(c), 170.0f - 100.0f * static_cast<float>(f) - 44.0f * static_cast<float>(r));
            }
    }
    for (int k = 0; k < 20; ++k) {
        char n[32];
        std::snprintf(n, sizeof n, "land_fleet_%02d", k);
        put(n, -W + 250.0f + 44.0f * static_cast<float>(k % 10), -170.0f - 44.0f * static_cast<float>(k / 10));
    }
    // §1.2 battle prompt without its shells: the bones of i_attack_mode_choice / _single.alo at the top-anchored origin
    // (0, H - 92) [port: calibrated on 28.jpg].
    const float top = H - kAttackShellTop;
    put("text_attack_choice", 0.0f, top + 84.0f);
    put("choice_button_left", -133.0f, top + 47.0f);
    put("choice_button_right", 132.0f, top + 47.0f);
    put("graph_left", -20.0f, top + 49.0f);
    put("graph_right", 20.0f, top + 49.0f);
    put("text_attack_single", 0.0f, top + 67.0f);
    put("attack_button_single", 7.0f, top + 27.0f);
}

void GalacticHud::init(const ComponentTable& table, const TextureAtlas& atlas, FontCache& fonts, const TextDatabase* text) {
    table_ = &table;
    atlas_ = &atlas;
    fonts_ = &fonts;
    text_ = text;
    buttons_.bind(table);
    auto f = [&](const char* n) { return table.find(n); };
    name_ = f("g_planet_name");
    value_ = f("g_planet_value");
    credit_filter_ = f("b_filters0");
    for (int k = 0; k < 4; ++k) filters_[k] = find_fmt(table, "b_filters%d", k);
    build_icon_ = f("g_build");
    smuggled_ = f("g_smuggled");
    corruption_icon_ = f("g_corruption_icon");
    corruption_text_ = f("g_corruption_text");
    enemy_hero_ = f("g_enemy_hero");
    weather_ = f("g_weather");
    ability_ = f("g_planet_ability");
    credit_bar_ = f("g_credit_bar");
    control_ = f("g_political_control");
    ground_ = f("g_ground_level");
    space_ = f("g_space_level");
    conflict_ = f("g_conflict");
    hero_ = f("g_hero");
    smuggler_ = f("g_smuggler");
    bounty_ = f("g_bounty_hunter");
    ring_ = f("g_planet_ring");
    credits_ = f("Text_Credits");
    pop_ = f("Text_Pop_Cap");
    day_ = f("Text_Day_Counter");
    planet_text_ = f("Text_Planet");
    income_ = f("Text_Planetary_Income");
    tech_ = f("Text_Tech_Level");
    day_credits_ = f("Text_Day_Credits");
    planet_info_ = f("planet_info");
    pause_ = f("b_play_pause");
    balance_pip_ = f("balance_pip");
    map_shell_ = f("map_shell");
    special_ = f("g_special_ability");
    obj_back_ = f("objective_back");
    obj_header_ = f("objective_header_text");
    obj_text_ = f("objective_text");
    obj_icon_ = f("objective_icon");
    commandbar_ = f("i_main_commandbar");
    radar_ = f("radar_galactic");
    radar_blip_ = f("g_radar_blip");
    radar_view_ = f("g_radar_view");
    movie_ = f("Movie_galactic");
    planet_fleet_ = f("g_planet_fleet");
    planet_land_ = f("g_planet_land_forces");
    organize_ = f("i_main_organize");
    pending_shell_ = f("pending_battle_shell");
    story_pending_shell_ = f("story_pending_battle_shell");
    choice_text_ = f("text_attack_choice");
    choice_left_ = f("choice_button_left");
    choice_right_ = f("choice_button_right");
    graph_left_ = f("graph_left");
    graph_right_ = f("graph_right");
    single_text_ = f("text_attack_single");
    single_button_ = f("attack_button_single");
    holocron_ = f("b_story_arc_g");
    allegiance_ = f("Text_Allegiance_A");
    build_pad_.clear();
    create_.clear();
    queue_.clear();
    land_.clear();
    fleet_.assign(3, {});
    for (int k = 0; k < 80; ++k)
        if (const int32_t i = find_fmt(table, "build_pad%02d", k); i >= 0) build_pad_.push_back(i);
    for (int k = 0; k < 48; ++k)
        if (const int32_t i = find_fmt(table, "b_Create_%02d", k); i >= 0) create_.push_back(i);
    for (int k = 0; k < 10; ++k)
        if (const int32_t i = find_fmt(table, "queue%02d", k); i >= 0) queue_.push_back(i);
    for (int k = 0; k < 20; ++k)
        if (const int32_t i = find_fmt(table, "land_fleet_%02d", k); i >= 0) land_.push_back(i);
    fleet_header_.clear();
    for (int k = 1; k <= 3; ++k) fleet_header_.push_back(find_fmt(table, "q%d_fleet", k));
    pathfinder_.assign(3, -1);
    for (int fl = 0; fl < 3; ++fl)
        for (int r = 0; r < 5; ++r)
            for (int c = 0; c < 7; ++c) {
                char n[32];
                std::snprintf(n, sizeof n, "q%d_%02d%02d", fl + 1, r, c);
                const int32_t i = table.find(n);
                if (i < 0) continue;
                if (r == 0 && c == 0) pathfinder_[static_cast<size_t>(fl)] = i;  // UI.md §7.4: the header pad is no unit pad
                else fleet_[static_cast<size_t>(fl)].push_back(i);
            }
    dynamic_.assign(table.size(), 0);
    auto mark = [&](int32_t i) {
        if (i >= 0) dynamic_[static_cast<size_t>(i)] = 1;
    };
    for (int32_t i : {name_, value_, control_, ground_, space_, conflict_, hero_, smuggler_, bounty_, ring_, planet_text_, obj_back_, obj_header_,
                      obj_text_, obj_icon_, balance_pip_, choice_text_, choice_left_, choice_right_, graph_left_, graph_right_,
                      single_text_, single_button_})
        mark(i);
    for (const auto* v : {&build_pad_, &create_, &queue_, &land_, &fleet_header_})
        for (int32_t i : *v) mark(i);
    for (const auto& v : fleet_)
        for (int32_t i : v) mark(i);
    for (int32_t i : pathfinder_) mark(i);
    // Every g_* label component and the tactical-only components are not command-bar buttons here.
    for (uint32_t i = 0; i < table.size(); ++i) {
        const std::string& n = table[i].name;
        if (n.size() > 2 && (n[0] == 'g' || n[0] == 'G') && n[1] == '_') dynamic_[i] = 1;
    }
}

void GalacticHud::resize(uint32_t width, uint32_t height) {
    resize_canvas(canvas_, fonts_, width, height);
    if (table_) {
        apply_galactic_fallback_layout(*table_, canvas_, layout_);
        layout_.update(canvas_, *table_);
    }
}

void GalacticHud::hit(int32_t component, uint32_t layer, Vec2f center, Vec2f size) {
    hits_.push_back(make_hit_box(component, layer, center, size));
}

int32_t GalacticHud::hit_test(int32_t px, int32_t py) const { return topmost_hit(hits_, canvas_, px, py); }

Vec2f GalacticHud::component_size(uint32_t component, const ComponentPlacement& p) const {
    return component_art_size((*table_)[component], p, *atlas_, {40.0f, 40.0f});  // [port] pads without art
}

int32_t GalacticHud::font_of(int32_t component) const {
    if (component < 0 || !fonts_) return -1;
    const CommandBarComponent& c = (*table_)[static_cast<size_t>(component)];
    return fonts_->font(c.font_name.empty() ? std::string(kFallbackFontFace) : c.font_name,
                        c.font_point_size > 0 ? c.font_point_size : 7.0f, canvas_.screen_height);
}

void GalacticHud::text_at(int32_t component, std::u32string_view text, Vec2f anchor, uint32_t layer, uint8_t alpha,
                          const Rgba8* color, bool second_color) {
    if (component < 0 || text.empty() || !fonts_) return;
    const CommandBarComponent& c = (*table_)[static_cast<size_t>(component)];
    HudTextStyle st;
    st.font = font_of(component);
    if (st.font < 0) return;
    Rgba8 col = color ? *color : (second_color && c.has_text_color2 ? c.text_color2 : (c.has_text_color ? c.text_color : Rgba8{}));
    st.color = with_alpha(col, alpha);
    st.outline = c.text_outline;
    st.outline_color = with_alpha(st.outline_color, alpha);
    st.emboss = c.text_emboss;
    st.align = c.left_justified ? TextAlign::Left : c.right_justified ? TextAlign::Right : TextAlign::Center;
    add_text(dl_, layer, HudList::Text, *fonts_, canvas_, text, anchor, st);
}

// GALAXY_MAP_PRESENTATION §1.2-1.4: up to three g_planet_fleet buttons around the projected planet - slot 0 at
// (-Default_Offset.x, Default_Offset.y) = (-36, 42), slot 1 at (0, Offset.y) = (0, 47), slot 2 at (36, 42) - and the
// g_planet_land_forces button at (0, Offset.y) = (0, 5); +Y up. Each: the pancake stack fleet_stacks_0n (n = layers,
// fleet_empty for 0) at Size x scale, the representative unit's icon on top (icon bottom = stack bottom + 3 x layers,
// [port] reading of "y = -h/2 + layers x 3"), the stealth / raid badge (Lower_Effect spot). Faction emblems are
// tooltip-only and not drawn.
// Playtest 3 (base 40.jpg): while slots_shown_, every free slot draws the empty oval (Blank_Texture_Name[0] fleet_empty,
// also on fogged planets), clickable as a drop target; the slot under the cursor gets the Mouse_Over_Texture_Name frame
// (i_button_unit_frame_space). The whole fleet being dragged leaves its slot blank (it follows the cursor).
// Base 40.jpg / C2_galactic_silent.png: the 36 x 45 pancake art is drawn at Scale (1.0, ovals 34 units wide); Size.x 0.75 is
// "icon scale" (the XML's own note) and applies to the unit icon only. fleet_empty's fill is opaque in the art and in the
// base shots (same colour over sky, dust and planet): the empty ovals are dark discs tinted like a stack, not outlines.
void GalacticHud::build_planet_stacks(const GalacticPlanetInfo& p, Vec2f at, uint8_t alpha, Vec2f cursor) {
    if (p.stacks.empty() && !slots_shown_) return;
    // Slot rule (§1.3): with the orbit not the local player's, enemy fleets take slot 0 and an own slot-0 fleet moves to
    // slot 2; otherwise fleets keep their slots. Fleets without a slot (or on a taken slot) take the first free one. The
    // slots are the sim's (playtest 3 t07: GalacticPlanetStack::slot = FleetState::slot); a drop only issues commands.
    const GalacticPlanetStack* slot[3] = {nullptr, nullptr, nullptr};
    std::vector<const GalacticPlanetStack*> later;
    for (const GalacticPlanetStack& st : p.stacks) {
        if (st.land) continue;
        if (p.fogged) continue;  // fogged: the ovals only
        int32_t s = st.slot;
        if (!p.orbit_local) s = st.enemy ? 0 : (s == 0 ? 2 : s);
        if (s >= 0 && s < 3 && !slot[s]) slot[s] = &st;
        else later.push_back(&st);
    }
    for (const GalacticPlanetStack* st : later)
        for (auto& sl : slot)
            if (!sl) {
                sl = st;
                break;
            }
    auto hovered = [&](Vec2f c, Vec2f size) {
        return std::fabs(cursor.x - c.x) <= 0.5f * size.x && std::fabs(cursor.y - c.y) <= 0.5f * size.y;
    };
    auto draw = [&](const GalacticPlanetStack& st, int32_t ci, Vec2f c, int32_t slot_index) {
        const CommandBarComponent& cc = (*table_)[static_cast<size_t>(ci)];
        // The whole fleet being dragged: its slot stays blank (Drag_Back draws its pancakes at the cursor).
        if (drag_.active && drag_.unit == 0 && st.fleet != 0 && st.fleet == drag_.fleet && slot_index >= 0) return;
        const float scale = cc.scale;
        const float icon_scale = cc.has_size && cc.size.x > 0.0f ? cc.size.x : 0.75f;
        const float step = cc.offset.x > 0.0f ? cc.offset.x : 3.0f;
        int32_t layers = 0;
        if (st.units > 0) layers = static_cast<int32_t>(std::min<uint32_t>(5u, (st.units + kShipsPerStack - 1) / kShipsPerStack));
        else layers = static_cast<int32_t>(std::min(st.count, 5u));
        const Rgba8 white = with_alpha(Rgba8{}, alpha);
        float bottom = c.y - 14.0f;  // [port] stack art missing: a 28-unit button
        Vec2f hit_size{28.0f, 28.0f};
        if (const std::string* b = pick(cc.blank_texture, layers)) {
            const AtlasEntry* e = atlas_->find(*b);
            if (e) {
                bottom = c.y - 0.5f * static_cast<float>(e->height) * scale;
                hit_size = {static_cast<float>(e->width) * scale, static_cast<float>(e->height) * scale};
            }
            // §1.4 secondary colour = owner faction colour [M: on the stack texture]; the icon and badges stay white.
            add_atlas_quad(dl_, 0, HudList::Alpha, *atlas_, *b, c, {}, scale, with_alpha(Rgba8{st.owner_color.r, st.owner_color.g, st.owner_color.b, 255}, alpha));
        }
        const char* badge = st.stealth ? kStealthBadge : st.raid ? kRaidBadge : nullptr;
        if (badge) add_atlas_quad(dl_, 0, HudList::Alpha, *atlas_, badge, Vec2f{c.x + cc.lower_effect_offset.x, c.y - cc.lower_effect_offset.y}, {}, scale, white);
        if (!st.icon.empty()) {
            const AtlasEntry* e = atlas_->find(st.icon);
            const float ih = e ? static_cast<float>(e->height) * icon_scale : 0.0f;
            add_atlas_quad(dl_, 0, HudList::Alpha, *atlas_, st.icon, {c.x, bottom + step * static_cast<float>(layers) + 0.5f * ih}, {},
                           icon_scale, white);
        }
        const bool hover = slots_shown_ && slot_index >= 0 && hovered(c, hit_size);
        if (st.selected || hover)
            if (const std::string* mo = pick(cc.mouse_over_texture, 0)) add_atlas_quad(dl_, 0, HudList::Alpha, *atlas_, *mo, c, {}, scale, white);
        if (hover) ++stats_.slot_highlights;
        ++stats_.stack_icons;
        // Clickable when the host named the fleet (several instances of one component: the click position picks one).
        if (st.fleet != 0 && st.fleet != commands::kNoObject && alpha > 0) {
            hit(ci, 0, c, hit_size);
            StackHit sh;
            sh.min = {c.x - 0.5f * hit_size.x, c.y - 0.5f * hit_size.y};
            sh.max = {c.x + 0.5f * hit_size.x, c.y + 0.5f * hit_size.y};
            sh.component = ci;
            sh.slot = slot_index;
            sh.fleet = st.fleet;
            sh.planet = p.planet;
            const std::string_view dd = cc.raw_value("DRAG_AND_DROP");
            sh.draggable = !st.land && !st.enemy && data::parse_flag_char(dd);
            sh.enemy = st.enemy;
            sh.layers = layers;
            sh.tint = st.owner_color;
            sh.icon = st.icon;
            stack_hits_.push_back(std::move(sh));
        }
    };
    if (planet_fleet_ >= 0) {
        const CommandBarComponent& fc = (*table_)[static_cast<size_t>(planet_fleet_)];
        const float side_x = fc.default_offset.x != 0.0f ? fc.default_offset.x : 36.0f;
        const float side_y = fc.default_offset.y != 0.0f ? fc.default_offset.y : 42.0f;
        const float mid_y = fc.offset.y != 0.0f ? fc.offset.y : 47.0f;
        const Vec2f pos[3] = {{at.x - side_x, at.y + side_y}, {at.x, at.y + mid_y}, {at.x + side_x, at.y + side_y}};
        for (int k = 0; k < 3; ++k)
            if (slot[k]) draw(*slot[k], planet_fleet_, pos[k], k);
        // Empty slot ovals (fleet_empty) while the slots are shown: drop targets, not draggable, no click.
        if (slots_shown_ && alpha > 0)
            if (const std::string* empty = pick(fc.blank_texture, 0)) {
                const float scale = fc.scale;
                const AtlasEntry* e = atlas_->find(*empty);
                const Vec2f size = e ? Vec2f{static_cast<float>(e->width) * scale, static_cast<float>(e->height) * scale} : Vec2f{27.0f, 20.0f};
                for (int k = 0; k < 3; ++k) {
                    if (slot[k]) continue;
                    add_atlas_quad(dl_, 0, HudList::Alpha, *atlas_, *empty, pos[k], {}, scale,
                                   with_alpha(Rgba8{empty_slot_color_.r, empty_slot_color_.g, empty_slot_color_.b, 255}, alpha));
                    if (hovered(pos[k], size)) {
                        if (const std::string* mo = pick(fc.mouse_over_texture, 0))
                            add_atlas_quad(dl_, 0, HudList::Alpha, *atlas_, *mo, pos[k], {}, scale, with_alpha(Rgba8{}, alpha));
                        ++stats_.slot_highlights;
                    }
                    ++stats_.empty_slots;
                    hit(planet_fleet_, 0, pos[k], size);
                    StackHit sh;
                    sh.min = {pos[k].x - 0.5f * size.x, pos[k].y - 0.5f * size.y};
                    sh.max = {pos[k].x + 0.5f * size.x, pos[k].y + 0.5f * size.y};
                    sh.component = planet_fleet_;
                    sh.slot = k;
                    sh.planet = p.planet;
                    stack_hits_.push_back(std::move(sh));
                }
            }
    }
    if (p.fogged) return;
    if (planet_land_ >= 0) {
        const CommandBarComponent& lc = (*table_)[static_cast<size_t>(planet_land_)];
        for (const GalacticPlanetStack& st : p.stacks)
            if (st.land) {
                draw(st, planet_land_, {at.x, at.y + (lc.offset.y != 0.0f ? lc.offset.y : 5.0f)}, -1);
                break;  // one land-forces button per planet
            }
    }
}

uint32_t GalacticHud::filter_bits() const {
    uint32_t bits = 0;
    for (int k = 0; k < 4; ++k)
        if (filters_[k] >= 0)
            if (const ButtonRuntime* r = buttons_.find_state(static_cast<uint32_t>(filters_[k])); r && r->selected) bits |= 1u << k;
    return bits;
}

void GalacticHud::build_labels(const GalacticHudFrame& f) {
    const float a = planet_label_alpha(f.zoom_state, f.zoom);
    const auto alpha = static_cast<uint8_t>(std::clamp(std::lround(a), 0L, 255L));
    if (alpha == 0) return;
    auto comp = [&](int32_t i) -> const CommandBarComponent* { return i >= 0 ? &(*table_)[static_cast<size_t>(i)] : nullptr; };
    auto up = [](Vec2f o) { return o; };  // [port] label offsets are +Y up
    const Vec2f cursor = canvas_.pixel_to_canvas(f.cursor_px.x + 0.5f, f.cursor_px.y + 0.5f);
    for (size_t pi = 0; pi < f.planets.size(); ++pi) {
        const GalacticPlanetInfo& p = f.planets[pi];
        if (!p.visible) continue;
        float sx = 0, sy = 0;
        if (!project(f.view, p.position, sx, sy)) continue;
        if (sx < -200 || sy < -200 || sx > static_cast<float>(canvas_.screen_width) + 200 ||
            sy > static_cast<float>(canvas_.screen_height) + 200)
            continue;
        const Vec2f at = canvas_.pixel_to_canvas(sx, sy);
        ++stats_.labels;
        // g_planet_ring round every planet, fogged too (C2_galactic_silent.png, ui_005_gc_running.png [H screenshot]): the
        // ellipse is this atlas art at its own size, so all planets get the same one whatever their size or distance. The
        // data says "Only alpha is used to color rings": colour = the name's; 173 / 255 fits both captures on green, blue and
        // grey rings [H screenshot, measured]. Selected planets are marked by the render selection model (§3), not here.
        if (const CommandBarComponent* c = comp(ring_))
            if (const std::string* t = pick(c->icon_texture, 0)) {
                const Rgba8 rc{p.owner_color.r, p.owner_color.g, p.owner_color.b, 173};
                if (add_atlas_quad(dl_, 0, HudList::Alpha, *atlas_, *t, at, {}, c->scale, with_alpha(rc, alpha))) ++stats_.planet_rings;
            }
        // Projected planet radius (canvas units) for the icons inside / around the planet.
        float rad = 30.0f;
        {
            float rx = 0, ry = 0;
            const Vec3 edge{p.position.x + f.view.right.x * p.radius, p.position.y + f.view.right.y * p.radius,
                            p.position.z + f.view.right.z * p.radius};
            if (project(f.view, edge, rx, ry)) rad = std::max(4.0f, std::hypot(rx - sx, ry - sy) * canvas_.units_per_pixel_x);
        }
        if (const CommandBarComponent* c = comp(name_)) {
            const Vec2f o = up(c->offset);
            std::u32string upper = p.name;  // [port: original screenshot] names in capitals
            for (char32_t& ch : upper)
                if (ch >= U'a' && ch <= U'z') ch = static_cast<char32_t>(ch - U'a' + U'A');
            text_at(name_, upper, {at.x + o.x, at.y + o.y}, 0, alpha, &p.owner_color);
        }
        if (!p.fogged) {
            // [port: original screenshot] garrison icon inside the planet (0.8 x radius square).
            if (!p.garrison_icon.empty()) {
                const float s = 0.8f * rad;
                add_atlas_quad(dl_, 0, HudList::Alpha, *atlas_, p.garrison_icon, at, {s, s}, 1.0f, with_alpha(Rgba8{}, alpha), {s, s});
                ++stats_.garrison_icons;
            }
        }
        build_planet_stacks(p, at, alpha, cursor);
        const uint32_t filters = filter_bits();
        // §1.1 / §1.2: the credit filter b_filters0 (Toggle, off at start) shows it.
        if (const CommandBarComponent* c = comp(value_); c && p.credits >= 0 && (filters & kFilterCredit)) {
            const Vec2f o = up(c->offset);
            text_at(value_, num32(p.credits), {at.x + o.x, at.y + o.y}, 0, alpha);
            ++stats_.planet_values;
        }
        // ui4 t03 [port]: g_credit_bar with the credit filter, its Default_Offset (0, 8) below the value text, left edge on it.
        if (const CommandBarComponent* c = comp(credit_bar_); c && p.credit_level >= 0 && (filters & kFilterCredit) && !c->bar_texture.empty()) {
            const Vec2f vo = value_ >= 0 ? (*table_)[static_cast<size_t>(value_)].offset : Vec2f{30, -11};
            const std::string* t = pick(c->bar_texture, std::clamp(p.credit_level, 0, std::max(0, c->max_bar_level)));
            const AtlasEntry* e = t ? atlas_->find(*t) : nullptr;
            const float w = e ? static_cast<float>(e->width) : 30.0f;
            if (t && add_atlas_quad(dl_, 0, HudList::Alpha, *atlas_, *t, {at.x + vo.x + 0.5f * w, at.y + vo.y - c->default_offset.y}, {}, 1.0f,
                                    with_alpha(Rgba8{}, alpha)))
                ++stats_.credit_bars;
        }
        // ui4 t03 [port]: b_filters2 shows the planet's weather (g_weather, Offset -20 -31) and ability (g_planet_ability,
        // Offset 0 -34) icons below the planet; not on a fogged planet.
        if ((filters & kFilterAbilityWeather) && !p.fogged)
            for (auto [ci, tex, n] : {std::tuple{weather_, &p.weather_icon, &stats_.weather_icons}, std::tuple{ability_, &p.ability_icon, &stats_.ability_icons}}) {
                const CommandBarComponent* c = comp(ci);
                if (!c || tex->empty()) continue;
                const Vec2f o = up(c->offset);
                if (add_atlas_quad(dl_, 0, HudList::Alpha, *atlas_, *tex, {at.x + o.x, at.y + o.y}, {}, c->scale, with_alpha(c->color, alpha), {16, 16}))
                    ++*n;
            }
        // ui4 t03: g_corruption_text (yellow, left-justified, Offset 30 15 above the name) - the transition percentage.
        if (const CommandBarComponent* c = comp(corruption_text_); c && !p.corruption_text.empty() && !p.fogged) {
            const Vec2f o = up(c->offset);
            text_at(corruption_text_, p.corruption_text, {at.x + o.x, at.y + o.y}, 0, alpha);
            ++stats_.corruption_texts;
        }
        if (const CommandBarComponent* c = comp(control_); c && p.political_control >= 0.0f && !c->bar_overlay.empty()) {
            const auto n = static_cast<int32_t>(c->bar_overlay.size());
            const auto lvl = static_cast<int32_t>(std::lround(std::clamp(p.political_control, 0.0f, 1.0f) * static_cast<float>(n - 1)));
            const Vec2f vo = value_ >= 0 ? (*table_)[static_cast<size_t>(value_)].offset : Vec2f{30, -11};
            const Vec2f o{vo.x + c->default_offset.x, vo.y - c->default_offset.y};
            const AtlasEntry* e = atlas_->find(*pick(c->bar_overlay, lvl));
            const float w = e ? static_cast<float>(e->width) : 30.0f;
            add_atlas_quad(dl_, 0, HudList::Alpha, *atlas_, *pick(c->bar_overlay, lvl), {at.x + o.x + 0.5f * w, at.y + o.y}, {}, 1.0f,
                           with_alpha(Rgba8{}, alpha));
        }
        // ui4 t03 [port]: the ground / space level pips need the base-level filter b_filters1 (§1.2 "ground/space level pips
        // need" a filter bit; b_filters1's tooltip is TEXT_TOOLTIP_BASE_LEVEL_FILTER).
        for (auto [ci, level] : {std::pair{ground_, p.groundbase_info ? p.ground_level : -1},
                                 std::pair{space_, p.starbase_info ? p.space_level : -1}}) {
            const CommandBarComponent* c = comp(ci);
            if (!c || level < 0 || c->bar_texture.empty() || !(filters & kFilterBaseLevel)) continue;
            const Vec2f o = up(c->default_offset);
            add_atlas_quad(dl_, 0, HudList::Alpha, *atlas_, *pick(c->bar_texture, level), {at.x + o.x, at.y + o.y}, {}, 1.0f,
                           with_alpha(Rgba8{}, alpha));
            ++stats_.level_pips;
        }
        // Status icons in a row from g_conflict's Offset.
        Vec2f pen = conflict_ >= 0 ? up(comp(conflict_)->offset) : Vec2f{30, -16};
        pen = {at.x + pen.x, at.y + pen.y};
        auto icon = [&](int32_t ci, bool on, int32_t alt) {
            const CommandBarComponent* c = comp(ci);
            if (!c || !on) return;
            const std::string* t = c->icon_texture.empty() ? pick(c->icon_alternate_texture, alt) : pick(c->icon_texture, 0);
            if (!t) return;
            const AtlasEntry* e = atlas_->find(*t);
            const float w = e ? static_cast<float>(e->width) : 12.0f;
            add_atlas_quad(dl_, 0, HudList::Alpha, *atlas_, *t, {pen.x + 0.5f * w, pen.y}, {}, c->scale, with_alpha(c->color, alpha), {12, 12});
            pen.x += w + 2.0f;
            ++stats_.label_icons;
        };
        icon(conflict_, p.conflict, 0);
        icon(hero_, p.hero_rebel && !p.fogged, 2);  // g_hero alternates: you, enemy, rebel, empire, underworld
        icon(hero_, p.hero_empire && !p.fogged, 3);
        icon(hero_, p.hero_underworld && !p.fogged, 4);
        icon(smuggler_, p.smuggler, 0);
        icon(bounty_, p.bounty_hunter, 0);
        // ui4 t03 [port: the row order after the original's groups is not traced].
        icon(build_icon_, p.building && !p.fogged, 0);
        icon(smuggled_, p.smuggled && !p.fogged, 0);
        icon(corruption_icon_, p.corrupted && !p.fogged, 0);
        icon(enemy_hero_, p.enemy_hero >= 0 && !p.fogged, std::max(0, p.enemy_hero));
    }
}

void GalacticHud::build_bar(const GalacticHudFrame& f) {
    auto set_text = [&](int32_t ci, std::u32string t) {
        if (ci >= 0) buttons_.state(static_cast<uint32_t>(ci)).text = std::move(t);
    };
    // [port: Gabriel, original screenshot] "Day: 1" (TEXT_CYCLES), "Tech: 3" (TEXT_TECH), "13,000", income in
    // Text_Day_Credits (TEXT_TOOLTIP_TOTAL_INCOME).
    auto label = [&](const char* key, const char32_t* fallback, int64_t v, const char32_t* sep) {
        std::u32string t = text_ && text_->find(key) ? utf16_to_utf32(*text_->find(key)) : std::u32string(fallback);
        while (!t.empty() && t.back() == U' ') t.pop_back();
        if (!t.empty() && t.back() != U':') t.push_back(U':');
        return t + sep + num32(v);
    };
    // [port: Gabriel, original screenshot] the credit amounts (reserve, income, planet income) start with the credits
    // symbol, which is the '$' glyph of the EmpireAtWar fonts these components use (register_game_fonts).
    set_text(credits_, f.credits != kNoGalacticAmount ? kCreditsPrefix + grouped32(f.credits) : std::u32string());
    set_text(day_credits_, f.income != kNoGalacticAmount ? kCreditsPrefix + grouped32(f.income) : std::u32string());
    if (f.pop >= 0) {
        std::u32string t = num32(f.pop);
        if (f.pop_cap >= 0) t += U" / " + num32(f.pop_cap);
        set_text(pop_, std::move(t));
    } else {
        set_text(pop_, {});
    }
    set_text(day_, f.day >= 0 ? label("TEXT_CYCLES", U"Day", f.day, U" ") : std::u32string());
    set_text(tech_, f.tech >= 0 ? label("TEXT_TECH", U"Tech", f.tech, U" ") : std::u32string());
    const GalacticPlanetInfo* sel = selected_ >= 0 ? &f.planets[static_cast<size_t>(selected_)] : nullptr;
    // GALACTIC_DIALOGS §5.3 (25.jpg): planet name in capitals and planet income, both in the owner's colour; the owner's
    // emblem on Text_Allegiance_A; the credits symbol is the font's '$' (blue like the number in 25.jpg, so not the fixed
    // yellow i_icon_credit.tga).
    std::u32string pname = sel ? sel->name : std::u32string();
    for (char32_t& ch : pname)
        if (ch >= U'a' && ch <= U'z') ch = static_cast<char32_t>(ch - U'a' + U'A');
    set_text(planet_text_, std::move(pname));
    set_text(income_, sel && sel->income != kNoGalacticAmount ? kCreditsPrefix + grouped32(sel->income) : std::u32string());
    // D12 (25.jpg, bone (175, 257)): the name's line is centred on the bone, inside the top strip. Drawn here because the generic
    // button text hangs a Max_Text_Width block below its anchor, half a line low: onto the Day bar. Bold 9, outlined and the
    // owner's colour are as in 25.jpg and the XML.
    if (planet_text_ >= 0 && sel && fonts_) {
        const auto u = static_cast<uint32_t>(planet_text_);
        const ComponentPlacement& p = layout_.placement(u);
        const CommandBarComponent& c = (*table_)[u];
        const std::u32string& name = buttons_.state(u).text;
        HudTextStyle st;
        st.font = font_of(planet_text_);
        if (p.placed && st.font >= 0) {
            st.color = Rgba8{sel->owner_color.r, sel->owner_color.g, sel->owner_color.b, 255};
            st.outline = c.text_outline;
            st.align = TextAlign::Center;
            // t47 [port]: one line, narrowed into Max_Text_Width; a wrapped name covered the Day row below.
            st.width_scale = fit_width_scale(measure_text_block(*fonts_, canvas_, name, 0.0f, st).width, c.max_text_width);
            const TextBlock tb = measure_text_block(*fonts_, canvas_, name, 0.0f, st);
            add_text_block(dl_, layer_of(c, p), HudList::Text, *fonts_, canvas_, name, {p.center.x, p.center.y + 0.5f * tb.height}, 0.0f, st);
        }
    }
    for (const int32_t ci : {planet_text_, income_}) {
        if (ci < 0) continue;
        ButtonRuntime& r = buttons_.state(static_cast<uint32_t>(ci));
        r.text_color_set = sel != nullptr;
        if (sel) r.text_color = Rgba8{sel->owner_color.r, sel->owner_color.g, sel->owner_color.b, 255};
    }
    if (allegiance_ >= 0) {
        ButtonRuntime& r = buttons_.state(static_cast<uint32_t>(allegiance_));
        r.hidden = !sel || sel->allegiance < 0;
        r.alt = sel ? sel->allegiance : -1;
    }
    // §2.1: the holocron flashes while a chapter is unread.
    if (holocron_ >= 0) buttons_.state(static_cast<uint32_t>(holocron_)).flash = f.holocron_flash;
    // §5.4: planet_info only on a shell bone (organiser / map_shell) with a planet selected - never at a fallback spot.
    if (planet_info_ >= 0)
        buttons_.state(static_cast<uint32_t>(planet_info_)).hidden =
            sel == nullptr || !layout_.placement(static_cast<uint32_t>(planet_info_)).from_bone;
    // 26.jpg: the button caption "Planet Info" (its tooltip key) and the organiser tabs "Fleet 1..3" (TEXT_FLEET_PANE_0..2).
    set_text(planet_info_, text_or("TEXT_BUTTON_PLANET_INFO", U"Planet Info"));
    // [port: 26.jpg] the tab labels start at their bone (Text_Fleet1 x -471.4 = the text's left edge) with the line's
    // vertical centre on it, so they are drawn here rather than by the generic (centred, wrapped) button text.
    for (int k = 0; k < 3 && fonts_; ++k) {
        const int32_t ci = find_fmt(*table_, "text_fleet%d", k + 1);
        if (ci < 0) continue;
        dynamic_[static_cast<size_t>(ci)] = 1;
        const ComponentPlacement& p = layout_.placement(static_cast<uint32_t>(ci));
        if (!p.from_bone || !stats_.organize_shown) continue;
        char key[32];
        std::snprintf(key, sizeof key, "TEXT_FLEET_PANE_%d", k);
        const CommandBarComponent& c = (*table_)[static_cast<size_t>(ci)];
        HudTextStyle st;
        st.font = font_of(ci);
        if (st.font < 0) continue;
        st.color = c.has_text_color ? c.text_color : Rgba8{0, 0, 0, 255};
        st.align = TextAlign::Left;
        add_text(dl_, layer_of(c, p), HudList::Text, *fonts_, canvas_, text_or(key, U"Fleet " + num32(k + 1)), p.center, st);
    }

    // Generic buttons / texts of the placed components (shell bones or fallbacks).
    for (uint32_t i = 0; i < table_->size(); ++i) {
        if (dynamic_[i]) continue;
        const CommandBarComponent& c = (*table_)[i];
        if (c.type != ComponentType::Button && c.type != ComponentType::TextButton && c.type != ComponentType::Icon) continue;
        const ComponentPlacement& p = layout_.placement(i);
        if (!p.placed) continue;
        const ButtonRuntime* r = buttons_.find_state(i);
        if (r ? r->hidden : c.hidden) continue;
        const bool has_art = !c.blank_texture.empty() || !c.icon_texture.empty() || !c.icon_alternate_texture.empty() ||
                             (r && (!r->icon_override.empty() || !r->text.empty()));
        if (!has_art && c.tooltip_text.empty()) continue;
        const uint32_t layer = layer_of(c, p);
        Vec2f size = buttons_.draw(dl_, *atlas_, fonts_, canvas_, i, p.center, p.size, f.faction_alt, f.time,
                                   static_cast<int32_t>(layer) - c.base_layer);
        if (p.size.x > 0) size = p.size;
        hit(static_cast<int32_t>(i), layer, p.size.x > 0 ? p.rect_center : p.center, size);
        ++stats_.buttons;
    }

    // Build pads (selected planet): GALACTIC_DIALOGS §5.1 one grid, b_Create_00.. filled from the left. [inferred] structures
    // first, then units, in provider order; the shell has no build_padNN bones, so units never use those components here.
    std::vector<GalacticBuildOption> options;
    if (sel && f.providers.build_options) f.providers.build_options(sel->planet, options);
    std::stable_partition(options.begin(), options.end(), [](const GalacticBuildOption& o) { return o.structure; });
    size_t slots_used = 0;
    auto pad = [&](int32_t ci, PadSlot slot) {
        const auto u = static_cast<uint32_t>(ci);
        const ComponentPlacement& p = layout_.placement(u);
        ButtonRuntime& r = buttons_.state(u);
        r.hidden = false;
        if (!p.placed) return false;
        const CommandBarComponent& c = (*table_)[u];
        const uint32_t layer = layer_of(c, p);
        const Vec2f sz = buttons_.draw(dl_, *atlas_, fonts_, canvas_, u, p.center, p.size, f.faction_alt, f.time,
                                       static_cast<int32_t>(layer) - c.base_layer);
        hit(ci, layer, p.size.x > 0 ? p.rect_center : p.center, p.size.x > 0 ? p.size : (sz.x > 0 ? sz : component_size(u, p)));
        slot.component = ci;
        pads_.push_back(std::move(slot));
        return true;
    };
    for (const GalacticBuildOption& o : options) {
        if (o.production_disabled && !f.show_production_disabled) continue;
        if (slots_used >= create_.size()) break;
        const int32_t ci = create_[slots_used++];
        ButtonRuntime& r = buttons_.state(static_cast<uint32_t>(ci));
        r.icon_override = o.icon;
        r.disabled = !o.enabled || o.production_disabled || o.block != HudBuildOption::Block::None;
        // ui4 t02: the Tooltip_Text entry by block reason (as the tactical Build_NN pads); an enabled pad has none.
        switch (o.block) {
            case HudBuildOption::Block::QueueFull: r.tooltip_index = 0; break;
            case HudBuildOption::Block::PopCap: r.tooltip_index = 1; break;
            case HudBuildOption::Block::Funds: r.tooltip_index = 2; break;
            default: r.tooltip_index = -1; break;
        }
        // GALACTIC_DIALOGS §5.1 second line (Text_Offset2 0 -20, Text_Color2 gold) = the queued count [L; port: > 0].
        r.text2 = o.queued > 0 ? num32(static_cast<int32_t>(o.queued)) : std::u32string();
        r.alt = o.space ? 1 : 0;  // §5.1: amber i_button_pad_rebel = ground tab, blue i_button_pad_empire = space tab
        PadSlot s;
        s.option = o;
        if (!pad(ci, std::move(s))) continue;
        ++stats_.build_buttons;
        // §5.1 (25.jpg): the cost centred along the pad's bottom edge, inside it; green when affordable [port: red when
        // not, L].
        if (o.cost >= 0 && fonts_) {
            const auto u = static_cast<uint32_t>(ci);
            const ComponentPlacement& p = layout_.placement(u);
            const CommandBarComponent& c = (*table_)[u];
            const Vec2f box = p.size.x > 0 ? p.size : component_size(u, p);
            const Vec2f mid = p.size.x > 0 ? p.rect_center : p.center;
            HudTextStyle st;
            st.font = font_of(ci);
            if (st.font < 0) continue;
            // ui4 t02: the colour says affordable or not (Funds, or a host without a block reason that disabled it).
            const bool unaffordable = o.production_disabled || o.block == HudBuildOption::Block::Funds ||
                                      (!o.enabled && o.block == HudBuildOption::Block::None);
            st.color = unaffordable ? kCostUnaffordable : kCostAffordable;
            st.outline = c.text_outline;
            st.align = TextAlign::Center;
            add_text(dl_, layer_of(c, p), HudList::Text, *fonts_, canvas_, num32(o.cost), {mid.x, mid.y - 0.5f * box.y + 7.0f}, st);
        }
    }
    for (const int32_t ci : build_pad_) buttons_.state(static_cast<uint32_t>(ci)).hidden = true;
    for (size_t k = slots_used; k < create_.size(); ++k) buttons_.state(static_cast<uint32_t>(create_[k])).hidden = true;

    // Build queue.
    std::vector<HudQueueEntry> queue;
    if (f.providers.build_queue) f.providers.build_queue(queue);
    // GALACTIC_DIALOGS §5.2: space entries on the blue domes queue00..04 (Build_Queue), ground entries on the orange domes
    // queue05..09 (Build_Queue_Land), each a hologram cone rising from its dome with the unit icon and the build dial.
    const size_t half = (queue_.size() + 1) / 2;
    size_t used[2] = {0, 0};
    std::vector<const HudQueueEntry*> slot_entry(queue_.size(), nullptr);
    for (const HudQueueEntry& e : queue) {
        const size_t side = e.land ? 1 : 0;
        const size_t base = side == 0 ? 0 : half, cap = side == 0 ? half : queue_.size() - half;
        if (used[side] >= cap) continue;
        slot_entry[base + used[side]++] = &e;
    }
    for (size_t k = 0; k < queue_.size(); ++k) {
        const auto u = static_cast<uint32_t>(queue_[k]);
        ButtonRuntime& r = buttons_.state(u);
        if (!slot_entry[k]) {
            r.hidden = true;
            continue;
        }
        const HudQueueEntry& e = *slot_entry[k];
        r.icon_override = e.icon;
        r.build_fill = std::clamp(e.progress, 0.0f, 1.0f) * 100.0f;
        r.text = e.count > 1 ? num32(e.count) : std::u32string();
        PadSlot s;
        s.is_queue = true;
        s.queue = e;
        if (pad(queue_[k], std::move(s))) ++stats_.queue_entries;
    }
}

void GalacticHud::build_fleets(const GalacticHudFrame& f) {
    const GalacticPlanetInfo* sel = selected_ >= 0 ? &f.planets[static_cast<size_t>(selected_)] : nullptr;
    std::vector<GalacticFleet> fleets;
    std::vector<GalacticFleetSlot> land;
    if (sel && stats_.organize_shown) {
        if (f.providers.fleets) f.providers.fleets(sel->planet, fleets);
        if (f.providers.land_forces) f.providers.land_forces(sel->planet, land);
    }
    // UI.md §7.2: pane k = frame q{k+1}; a fleet's pane comes from its orbit slot, not from the provider order. Orbit owner
    // = the local player (the planet's orbit_local): slot k -> pane k, every slot-0 fleet merged into pane 0, non-local fleets
    // (allied ones too) not shown. Otherwise every
    // non-local fleet -> pane 0 (merged), slot 1 -> pane 1, slots 0 and 2 -> pane 2 (merged) [M]. The same rule as the map's
    // build_planet_stacks (§1.3). Fleets without a slot or whose pane is taken take the first empty pane in feed order [port,
    // as the map]; with none left they are not shown.
    constexpr size_t kPanes = 3;
    std::vector<const GalacticFleet*> panes[kPanes];
    {
        std::vector<const GalacticFleet*> later;
        for (const GalacticFleet& gf : fleets) {
            std::vector<const GalacticFleet*>* to = nullptr;
            if (sel->orbit_local) {
                if (gf.enemy) continue;  // §7.2 first row: only the local player's fleets fill the panes
                if (gf.slot == 0) to = &panes[0];
                else if (gf.slot > 0 && gf.slot < 3 && panes[gf.slot].empty()) to = &panes[gf.slot];
            } else if (gf.enemy) {
                to = &panes[0];
            } else if (gf.slot == 0 || gf.slot == 2) {
                to = &panes[2];
            } else if (gf.slot == 1 && panes[1].empty()) {
                to = &panes[1];
            }
            if (to) to->push_back(&gf);
            else later.push_back(&gf);
        }
        for (const GalacticFleet* gf : later)
            for (auto& pane : panes)
                if (pane.empty()) {
                    pane.push_back(gf);
                    break;
                }
    }
    auto draw_slot = [&](int32_t ci, const GalacticFleetSlot& s, std::vector<uint32_t> select, int32_t fleet_index, uint32_t fleet,
                         uint32_t unit, bool foreign, bool empty = false) {
        const auto u = static_cast<uint32_t>(ci);
        ButtonRuntime& r = buttons_.state(u);
        r.hidden = false;
        r.icon_override = s.icon;
        r.selected = s.selected;
        r.text = s.count > 1 ? num32(s.count) : std::u32string();
        const ComponentPlacement& p = layout_.placement(u);
        if (!p.placed) return false;
        const CommandBarComponent& c = (*table_)[u];
        const uint32_t layer = layer_of(c, p);
        const Vec2f sz = buttons_.draw(dl_, *atlas_, fonts_, canvas_, u, p.center, p.size, f.faction_alt, f.time,
                                       static_cast<int32_t>(layer) - c.base_layer);
        hit(ci, layer, p.size.x > 0 ? p.rect_center : p.center, p.size.x > 0 ? p.size : (sz.x > 0 ? sz : component_size(u, p)));
        PadSlot ps;
        ps.component = ci;
        ps.select = std::move(select);
        const std::string_view dd = c.raw_value("DRAG_AND_DROP");
        ps.draggable = !foreign && !empty && data::parse_flag_char(dd);
        ps.foreign = foreign;
        ps.fleet_index = fleet_index;
        ps.fleet = fleet;
        ps.unit = unit;
        ps.empty = empty;
        ps.icon = s.icon;
        pads_.push_back(std::move(ps));
        return true;
    };
    for (size_t fl = 0; fl < fleet_.size() && fl < kPanes; ++fl) {  // all three panes always: the frames and tabs are shell geometry (§7.1)
        const std::vector<int32_t>& slots = fleet_[fl];
        const std::vector<const GalacticFleet*>& in_pane = panes[fl];
        const GalacticFleet* fd = in_pane.empty() ? nullptr : in_pane.front();
        const auto fi = static_cast<int32_t>(fl);
        // Fleet header (q{n}_fleet): the faction's big fleet icon of the pane's first fleet; dragging it drags the whole fleet
        // (not for a foreign one, whose header is shown but inert) [port].
        if (fl < fleet_header_.size() && fleet_header_[fl] >= 0) {
            if (fd) {
                GalacticFleetSlot hs;
                hs.object = fd->fleet;
                draw_slot(fleet_header_[fl], hs, fd->enemy ? std::vector<uint32_t>{} : std::vector<uint32_t>{fd->fleet}, fi, fd->fleet, 0, fd->enemy);
            } else {
                buttons_.state(static_cast<uint32_t>(fleet_header_[fl])).hidden = true;
            }
        }
        const uint32_t pane_fleet = fd ? fd->fleet : 0;
        const bool pane_foreign = fd && fd->enemy;
        // UI.md §7.4: q{n}_0000 ("Fleet PF", the pathfinder) is the header pad, not a unit pad; the unit pads are q{n}_0001
        // upward. It is always shown, empty (no unit is a pathfinder until flagship battles exist), and inert [port]: no
        // drag, click or drop (a foreign-style empty pad).
        if (pathfinder_[fl] >= 0) draw_slot(pathfinder_[fl], GalacticFleetSlot{}, {}, fi, pane_fleet, 0, true, true);
        // UI.md §7.2: the pane's units, grouped per fleet and type into stacks (one pad each, the count as text), fill the unit
        // pads upward, fleet after fleet; further stacks are not drawn. Dragging a stack takes its first unit.
        struct Stack {
            const GalacticFleet* fleet;
            GalacticFleetSlot slot;  // the first unit, count summed
            std::vector<uint32_t> members;
        };
        std::vector<Stack> stacks;
        for (const GalacticFleet* gf : in_pane) {
            const size_t first = stacks.size();
            for (const GalacticFleetSlot& u : gf->units) {
                Stack* into = nullptr;
                if (u.type != kNoSlotType)
                    for (size_t k = first; k < stacks.size(); ++k)
                        if (stacks[k].slot.type == u.type) into = &stacks[k];
                if (into) {
                    into->slot.count += u.count;
                    into->slot.selected = into->slot.selected || u.selected;
                    into->members.push_back(u.object);
                } else {
                    stacks.push_back({gf, u, {u.object}});
                }
            }
        }
        size_t next = 0;
        for (const Stack& st : stacks) {
            if (next >= slots.size()) break;
            if (draw_slot(slots[next], st.slot, st.fleet->enemy ? std::vector<uint32_t>{} : st.members, fi, st.fleet->fleet, st.slot.object,
                          st.fleet->enemy))
                ++stats_.fleet_slots;
            ++next;
        }
        // UI.md §7.3: the pads without a stack show (empty, as drop targets) only while something is dragged.
        for (; next < slots.size(); ++next) {
            if (drag_.active) draw_slot(slots[next], GalacticFleetSlot{}, {}, fi, pane_fleet, 0, pane_foreign, true);
            else buttons_.state(static_cast<uint32_t>(slots[next])).hidden = true;
        }
    }
    for (size_t k = 0; k < land_.size(); ++k) {
        if (k >= land.size()) {
            buttons_.state(static_cast<uint32_t>(land_[k])).hidden = true;
            continue;
        }
        if (draw_slot(land_[k], land[k], {land[k].object}, -1, 0, land[k].object, false)) ++stats_.land_slots;
    }
}

// ---- fleet drag and drop ----------------------------------------------------------------------------------------
// The slot a new fleet should take (payload `b`, galactic campaign.hpp "kFleetSlot").
static commands::EventData with_slot_request(commands::EventData e, int32_t slot) {
    if (slot >= 0) e.set("b", static_cast<uint32_t>(slot));
    return e;
}

// The order a drop on a fleet slot issues (rules: galactic_hud.hpp, "Fleet-slot drops"); out = nullptr only tests it.
bool GalacticHud::slot_drop(const StackHit& sh, std::vector<commands::EventData>* out) {
    constexpr uint32_t kNone = commands::kNoObject;
    const bool land = drag_.fleet_index < 0;
    const uint32_t home = drag_.planet != 0 ? drag_.planet : selected_id_;  // the dragged fleet's / unit's planet
    const uint32_t unit = drag_.unit != 0 ? drag_.unit : kNone;
    const bool empty = sh.fleet == 0 || sh.fleet == kNone;
    if (sh.planet == 0 || home == 0) return false;
    if (land && drag_.unit == 0) return false;
    if (sh.planet == home) {
        if (!empty) {
            if (sh.enemy || sh.fleet == drag_.fleet) return false;
            if (out) out->push_back(land ? fleet_management_event(commands::kFleetLaunch, sh.fleet, kNone, unit)
                                         : fleet_management_event(commands::kFleetMerge, sh.fleet, drag_.fleet, unit));
            return true;
        }
        if (!land && drag_.unit == 0) {  // re-slot the whole fleet
            if (out) out->push_back(fleet_management_event(commands::kFleetSlot, home, drag_.fleet, kNone, kNone, static_cast<uint8_t>(sh.slot)));
            return true;
        }
        if (out) {
            // The unit becomes a new fleet in that slot: detach from its fleet, or launch from the surface (op 6 with the
            // destination = its own planet); the slot rides in the payload's `b` and the sim gives it to the new fleet.
            out->push_back(with_slot_request(land ? fleet_management_event(commands::kFleetMoveNew, home, kNone, unit, home)
                                                  : fleet_management_event(commands::kFleetDetach, home, drag_.fleet, unit),
                                             sh.slot));
        }
        return true;
    }
    // Another planet: a move order through the command stream (the sim splits a unit off its fleet).
    if (out) {
        // [inferred] the slot chosen (an empty one, or an own fleet's: merge on arrival) is the sim's to apply on arrival;
        // an enemy fleet's slot is no choice.
        const bool choose = sh.slot >= 0 && (empty || !sh.enemy);
        if (land || drag_.unit != 0) {  // the unit becomes a new fleet that moves (op 6; `b` = its slot at the destination)
            commands::EventData e = fleet_management_event(commands::kFleetMoveNew, home, land ? kNone : drag_.fleet, unit, sh.planet);
            out->push_back(choose ? with_slot_request(std::move(e), sh.slot) : std::move(e));
        } else {
            if (choose)  // op 8 with a destination planet: the slot it takes on arrival, ahead of the move order
                out->push_back(fleet_management_event(commands::kFleetSlot, home, drag_.fleet, kNone, sh.planet, static_cast<uint8_t>(sh.slot)));
            out->push_back(commands::ev::move_object_to_object(drag_.fleet, sh.planet));
        }
    }
    return true;
}

// The slot a drop on an empty organiser pane addresses: the selected planet's slot of that pane (UI.md §7.2, pane k = slot k).
GalacticHud::StackHit GalacticHud::pane_hit(const PadSlot& pad) const {
    StackHit h;
    h.component = pad.component;
    h.planet = selected_id_;
    h.slot = pad.fleet_index;
    return h;
}

const GalacticHud::PadSlot* GalacticHud::pad_of(int32_t component) const {
    if (component < 0) return nullptr;
    for (const PadSlot& s : pads_)
        if (s.component == component) return &s;
    return nullptr;
}

const GalacticHud::StackHit* GalacticHud::stack_at(int32_t component, float px, float py) const {
    if (component < 0) return nullptr;
    const Vec2f c = canvas_.pixel_to_canvas(px + 0.5f, py + 0.5f);
    const StackHit* best = nullptr;
    for (const StackHit& h : stack_hits_)  // last drawn wins (drawn on top)
        if (h.component == component && c.x >= h.min.x && c.x <= h.max.x && c.y >= h.min.y && c.y <= h.max.y) best = &h;
    return best;
}

uint32_t GalacticHud::stack_under(float px, float py, bool& land) const {
    land = false;
    const Vec2f c = canvas_.pixel_to_canvas(px + 0.5f, py + 0.5f);
    const StackHit* best = nullptr;
    for (const StackHit& h : stack_hits_)  // last drawn wins (drawn on top)
        if ((h.component == planet_fleet_ || h.component == planet_land_) && c.x >= h.min.x && c.x <= h.max.x && c.y >= h.min.y &&
            c.y <= h.max.y)
            best = &h;
    if (!best) return 0;
    land = best->component == planet_land_;
    return best->fleet;
}

bool GalacticHud::takes_button(int32_t component, Button b) const {
    if (b == Button::Right && component >= 0 && (component == planet_fleet_ || component == planet_land_)) return false;
    return true;
}

bool GalacticHud::draggable(int32_t component) const {
    const PadSlot* p = pad_of(component);
    if (p) return p->draggable;
    for (const StackHit& h : stack_hits_)  // a map fleet button: any draggable instance (the press picks the one)
        if (h.component == component && h.draggable) return true;
    return false;
}

int32_t GalacticHud::planet_at(float px, float py) const {
    int32_t best = -1;
    float best_d = 1e30f;
    for (size_t i = 0; i < planet_px_.size(); ++i) {
        const PlanetScreen& p = planet_px_[i];
        const float d = std::hypot(p.x - px, p.y - py);
        if (d <= std::max(p.r * 1.2f, 24.0f) && d < best_d) {
            best_d = d;
            best = static_cast<int32_t>(i);
        }
    }
    return best;
}

void GalacticHud::drag_update(const RoutedInput& routed, std::vector<commands::EventData>& out, bool& dropped) {
    const Vec2f cur{static_cast<float>(routed.world.mouse_x), static_cast<float>(routed.world.mouse_y)};
    for (const HudPointerEvent& e : routed.hud) {
        if (e.button != Button::Left) continue;
        if (e.kind == HudPointerEvent::Kind::Press) {
            press_component_ = draggable(e.component) ? e.component : -1;
            press_px_ = {static_cast<float>(e.x), static_cast<float>(e.y)};
            press_stack_ = {};
            if (press_component_ >= 0 && !pad_of(press_component_)) {
                const StackHit* sh = stack_at(press_component_, press_px_.x, press_px_.y);
                if (sh && sh->draggable) press_stack_ = *sh;
                else press_component_ = -1;
            }
        } else if (e.kind == HudPointerEvent::Kind::Release) {
            if (drag_.active) {
                drag_.cursor_px = {static_cast<float>(e.x), static_cast<float>(e.y)};
                drag_.over_component = e.component;
                drop(&out);
                dropped = true;
            }
            press_component_ = -1;
            drag_ = {};
        }
    }
    if (press_component_ >= 0 && !drag_.active && routed.owner[static_cast<size_t>(Button::Left)] == PointerTarget::Hud &&
        std::hypot(cur.x - press_px_.x, cur.y - press_px_.y) > static_cast<float>(kDragThresholdPx)) {
        if (const PadSlot* p = pad_of(press_component_)) {
            drag_.active = true;
            drag_.component = p->component;
            drag_.fleet_index = p->fleet_index;
            drag_.fleet = p->fleet;
            drag_.unit = p->unit;
            drag_.icon = p->icon;
        } else if (press_stack_.component == press_component_) {
            drag_.active = true;
            drag_.component = press_stack_.component;
            drag_.fleet_index = kMapFleetIndex;
            drag_.fleet = press_stack_.fleet;
            drag_.unit = 0;
            drag_.planet = press_stack_.planet;
            drag_.icon = press_stack_.icon;
            drag_.layers = press_stack_.layers;
            drag_.tint = press_stack_.tint;
        }
    }
    if (!drag_.active) return;
    drag_.cursor_px = cur;
    drag_.over_component = routed.hover_component;
    const PadSlot* over = pad_of(drag_.over_component);
    drag_.over_fleet = over && over->fleet_index >= 0 && !over->foreign && over->fleet != drag_.fleet;  // never merge into a foreign fleet
    drag_.over_planet = drag_.over_component < 0 ? planet_at(cur.x, cur.y) : -1;
    // Over a fleet slot (an empty oval or a fleet button): its planet counts as the planet under the cursor.
    drag_.over_slot = -1;
    drag_.over_slot_planet = drag_.over_slot_fleet = 0;
    if (over && over->empty && over->fleet == 0 && !over->foreign) {  // an empty pane: the drop makes a new fleet in its slot
        drag_.valid = drop(nullptr);
        drag_.over_fleet = drag_.valid;
        return;
    }
    if (drag_.over_component >= 0 && drag_.over_component == planet_fleet_)
        if (const StackHit* sh = stack_at(planet_fleet_, cur.x, cur.y)) {
            drag_.over_slot = sh->slot;
            drag_.over_slot_planet = sh->planet;
            drag_.over_slot_fleet = sh->fleet;
            for (size_t i = 0; i < planet_px_.size(); ++i)
                if (planet_px_[i].planet == sh->planet) drag_.over_planet = static_cast<int32_t>(i);
            drag_.valid = drop(nullptr);
            return;
        }
    drag_.valid = drop(nullptr);
}

bool GalacticHud::drop(std::vector<commands::EventData>* out) {
    constexpr uint32_t kNone = commands::kNoObject;
    const auto emit = [out](commands::EventData e) {
        if (out) out->push_back(std::move(e));
        return true;
    };
    const PadSlot* over = pad_of(drag_.over_component);
    const bool from_fleet = drag_.fleet_index >= 0;
    const uint32_t unit = drag_.unit != 0 ? drag_.unit : kNone;
    if (over && over->fleet_index >= 0) {  // onto a fleet pad
        if (over->foreign) return false;  // a foreign pad is no drop target (UI.md §7.2)
        if (over->empty && over->fleet == 0)  // an empty pane k: a new fleet in slot k (UI.md §7.2, §7.3)
            return selected_id_ != 0 && slot_drop(pane_hit(*over), out);
        if (over->fleet == drag_.fleet) return false;
        if (from_fleet) return emit(fleet_management_event(commands::kFleetMerge, over->fleet, drag_.fleet, unit));
        return emit(fleet_management_event(commands::kFleetLaunch, over->fleet, kNone, unit));
    }
    if (over) {  // onto the land forces (a map fleet: only at the selected planet)
        if (from_fleet && selected_id_ != 0 && (drag_.planet == 0 || drag_.planet == selected_id_))
            return emit(fleet_management_event(commands::kFleetLand, selected_id_, drag_.fleet, unit));
        return false;
    }
    if (drag_.over_component >= 0 && drag_.over_component == planet_fleet_) {  // onto a fleet slot
        const StackHit* sh = stack_at(planet_fleet_, drag_.cursor_px.x, drag_.cursor_px.y);
        return sh && slot_drop(*sh, out);
    }
    if (drag_.over_component >= 0 || !from_fleet) return false;  // another HUD component / land unit into the world
    const int32_t pi = planet_at(drag_.cursor_px.x, drag_.cursor_px.y);
    if (pi >= 0) {
        const uint32_t planet = planet_px_[static_cast<size_t>(pi)].planet;
        if (drag_.unit != 0) return emit(commands::ev::move_object_to_object(drag_.unit, planet));
        if (planet != (drag_.planet != 0 ? drag_.planet : selected_id_)) return emit(commands::ev::move_object_to_object(drag_.fleet, planet));
        return false;
    }
    if (drag_.unit != 0 && selected_id_ != 0) return emit(fleet_management_event(commands::kFleetDetach, selected_id_, kNone, drag_.unit));
    return false;
}

void GalacticHud::build_objectives(const GalacticHudFrame& f) {
    if (!f.objectives_open || !f.providers.objectives || obj_back_ < 0) return;
    std::vector<HudObjective> objs;
    f.providers.objectives(objs);
    if (objs.empty()) return;
    const CommandBarComponent& back = (*table_)[static_cast<size_t>(obj_back_)];
    const Vec2f off = canvas_.wider_than_4_3() && back.has_default_offset_widescreen ? back.default_offset_widescreen
                                                                                       : back.default_offset;
    // Top-left of the panel: canvas centre + offset (+Y down) [port], then clamped inside the canvas.
    Vec2f tl{off.x - 0.5f * back.size.x, -off.y};
    tl.x = std::clamp(tl.x, -0.5f * canvas_.width, 0.5f * canvas_.width - back.size.x);
    tl.y = std::clamp(tl.y, -0.5f * canvas_.height + 40.0f, 0.5f * canvas_.height);
    const float width = back.has_size ? back.size.x : 266.0f;
    const CommandBarComponent* hdr = obj_header_ >= 0 ? &(*table_)[static_cast<size_t>(obj_header_)] : nullptr;
    const CommandBarComponent* txt = obj_text_ >= 0 ? &(*table_)[static_cast<size_t>(obj_text_)] : nullptr;
    const CommandBarComponent* ico = obj_icon_ >= 0 ? &(*table_)[static_cast<size_t>(obj_icon_)] : nullptr;
    const float header_spacing = hdr && hdr->has_size ? hdr->size.y : 18.0f;
    const float line = txt && txt->has_size ? txt->size.y : 16.0f;
    const float pad_x = back.offset.x, pad_y = back.offset.y;
    float y = tl.y - pad_y;
    std::u32string header = f.objectives_header;
    if (header.empty())  // [port] TEXT_OBJECTIVES is not in the FoC MasterTextFile: English fallback
        header = text_ && text_->find("TEXT_OBJECTIVES") ? utf16_to_utf32(*text_->find("TEXT_OBJECTIVES")) : std::u32string(U"Objectives");
    const float y_top = tl.y;
    // Lines first (to size the background).
    struct Line {
        std::u32string text;
        float y;
        HudObjective::State state;
        bool first;
    };
    std::vector<Line> lines;
    y -= header_spacing;
    const int32_t tf = font_of(obj_text_);
    for (const HudObjective& o : objs) {
        std::u32string t = o.text;
        if (t.empty()) t = text_ ? gui_unescape(utf16_to_utf32(text_->text(o.text_key))) : utf8_to_utf32(o.text_key);
        const float wrap_px = (width - 24.0f) / canvas_.units_per_pixel_x;
        std::vector<std::u32string> wrapped = tf >= 0 ? wrap_text(*fonts_, tf, t, wrap_px) : std::vector<std::u32string>{t};
        bool first = true;
        for (std::u32string& w : wrapped) {
            lines.push_back({std::move(w), y - 0.5f * line, o.state, first});
            first = false;
            y -= line;
        }
        ++stats_.objectives;
    }
    const float bottom = y - pad_y;
    // Background (Blank_Texture_Name stretched, Color alpha) on Base_Layer.
    const uint32_t bl = static_cast<uint32_t>(std::clamp(back.base_layer, 0, 12));
    if (const std::string* t = pick(back.blank_texture, 0)) {
        const Vec2f c{tl.x + 0.5f * width, 0.5f * (y_top + bottom)};
        add_atlas_quad(dl_, bl, HudList::Alpha, *atlas_, *t, c, {width, y_top - bottom}, 1.0f, back.has_color ? back.color : Rgba8{}, {width, y_top - bottom});
    }
    const uint32_t tl_layer = static_cast<uint32_t>(std::clamp(hdr ? hdr->base_layer : 4, 0, 12));
    text_at(obj_header_, header, {tl.x + pad_x, y_top - pad_y - 0.5f * header_spacing}, tl_layer);
    for (const Line& l : lines) {
        const bool done = l.state == HudObjective::State::Complete, failed = l.state == HudObjective::State::Failed;
        Rgba8 col = txt && txt->has_text_color ? txt->text_color : Rgba8{};
        if (done && txt && txt->has_text_color2) col = txt->text_color2;
        if (failed && hdr && hdr->has_text_color2) col = hdr->text_color2;
        if (l.first && ico && (done || failed)) {
            const std::string* t = done ? pick(ico->icon_texture, 0) : pick(ico->icon_alternate_texture, 0);
            if (t)
                add_atlas_quad(dl_, static_cast<uint32_t>(std::clamp(ico->base_layer, 0, 12)), HudList::Alpha, *atlas_, *t,
                               {tl.x + pad_x + 8.0f, l.y}, ico->has_size ? ico->size : Vec2f{16, 16}, 1.0f, Rgba8{},
                               {12, 12});
        }
        text_at(obj_text_, l.text, {tl.x + pad_x + 20.0f, l.y}, tl_layer, 255, &col);
    }
}

void GalacticHud::build_special_abilities(const GalacticHudFrame& f) {
    if (special_ < 0 || selected_ < 0 || !f.providers.special_abilities) return;
    const GalacticPlanetInfo& p = f.planets[static_cast<size_t>(selected_)];
    std::vector<GalacticSpecialAbility> rows;
    f.providers.special_abilities(p.planet, rows);
    if (rows.empty() || !p.visible) return;
    float sx = 0, sy = 0;
    if (!project(f.view, p.position, sx, sy)) return;
    const auto alpha = static_cast<uint8_t>(std::clamp(std::lround(planet_label_alpha(f.zoom_state, f.zoom)), 0L, 255L));
    if (alpha == 0) return;
    const auto u = static_cast<uint32_t>(special_);
    const CommandBarComponent& c = (*table_)[u];
    const ComponentPlacement& pl = layout_.placement(u);
    const uint32_t layer = static_cast<uint32_t>(std::clamp(c.base_layer, 0, static_cast<int32_t>(kHudLayers) - 1));
    const Vec2f at = canvas_.pixel_to_canvas(sx, sy);
    const Vec2f size = component_size(u, pl);
    const Vec2f cur = canvas_.pixel_to_canvas(f.cursor_px.x + 0.5f, f.cursor_px.y + 0.5f);
    constexpr size_t kMaxRows = 8;  // [port]
    for (size_t i = 0; i < rows.size() && i < kMaxRows; ++i) {
        const GalacticSpecialAbility& r = rows[i];
        const Vec2f ctr{at.x + c.offset.x, at.y + c.offset.y * static_cast<float>(i + 1)};
        const Vec2f mn{ctr.x - 0.5f * size.x, ctr.y - 0.5f * size.y}, mx{ctr.x + 0.5f * size.x, ctr.y + 0.5f * size.y};
        const bool over = r.enabled && cur.x >= mn.x && cur.x <= mx.x && cur.y >= mn.y && cur.y <= mx.y;
        const Rgba8 tint = with_alpha(r.enabled ? Rgba8{} : Rgba8{128, 128, 128, 255}, alpha);
        if (const std::string* b = pick(c.blank_texture, 0))
            add_atlas_quad(dl_, layer, HudList::Alpha, *atlas_, *b, ctr, {}, c.scale, tint, size);
        const std::string* icon = !r.icon.empty() ? &r.icon : pick(c.icon_alternate_texture, f.faction_alt);
        if (icon) add_atlas_quad(dl_, layer, HudList::Alpha, *atlas_, *icon, ctr, {}, c.scale, tint);
        if (over)
            if (const std::string* mo = pick(c.mouse_over_texture, f.faction_alt))
                add_atlas_quad(dl_, layer, HudList::Alpha, *atlas_, *mo, ctr, {}, c.scale, with_alpha(Rgba8{}, alpha));
        std::u32string text = r.text;
        if (text.empty() && !r.text_key.empty())
            text = text_ && text_->find(r.text_key) ? gui_unescape(utf16_to_utf32(*text_->find(r.text_key))) : utf8_to_utf32(r.text_key);
        if (!text.empty()) {
            Rgba8 col = c.has_text_color ? c.text_color : Rgba8{};
            if (!r.enabled) col = {static_cast<uint8_t>(col.r / 2), static_cast<uint8_t>(col.g / 2), static_cast<uint8_t>(col.b / 2), col.a};
            text_at(special_, text, {ctr.x + c.text_offset.x, ctr.y - c.text_offset.y}, layer, alpha, &col);
        }
        hit(special_, layer, ctr, size);
        ability_rows_.push_back({mn, mx, r.hero, r.enabled});
        ++stats_.ability_rows;
    }
}

// [port: Gabriel, original screenshot] galactic minimap over radar_galactic: one g_radar_blip per visible planet in its
// owner colour (Selected_Texture for the selected planet) and the camera's footprint on the galaxy plane as a g_radar_view
// outline (i_radar_view is in no atlas: 1-unit lines in the same layer). Mapping [port]: the planets' bounding box,
// squared and padded 10 %, fills the radar rectangle (+X right, +Y up).
void GalacticHud::build_radar(const GalacticHudFrame& f) {
    if (radar_ < 0) return;
    const ComponentPlacement& rp = layout_.placement(static_cast<uint32_t>(radar_));
    if (!rp.placed) return;
    const CommandBarComponent& rc = (*table_)[static_cast<size_t>(radar_)];
    const Vec2f size = rp.size.x > 0 ? rp.size : rc.has_size ? rc.size : Vec2f{175, 175};
    const Vec2f rc_at = rp.size.x > 0 ? rp.rect_center : rp.center;
    // Opaque backdrop (playtest: trade routes showed through the radar box). In i_galactic_controls.alo the radar_galactic
    // bone's own mesh is visible but MeshAdditive (i_scan_lines.tga) - it cannot hide the world; what makes the box opaque
    // in the original is the faction faceplate (Empire/Rebel/Underworld_Faceplate_ALT<n>, MeshAlpha,
    // i_galactic_dashboard_wide*.tga), whose texels over the whole 175 x 175 radar rectangle have alpha 255 (checked on
    // the vanilla DDS). The HUD draws an opaque (PrimOpaque list) rectangle of the radar size on layer 0, i.e. before the
    // shell models (UI.md §1.1 step 5i < 6): with the faceplate drawn it is covered and invisible; without it (shells not
    // loaded / not drawn) it still stops the world from showing through. Blips (layer 3) and the view rectangle (layer 4)
    // draw over both. [port] colour = the faceplate's dark radar ground.
    add_atlas_quad(dl_, 0, HudList::Opaque, *atlas_, {}, rc_at, {}, 1.0f, kGalacticRadarBackdrop, size);
    stats_.radar_backdrop = true;
    if (f.planets.empty()) return;
    float x0 = f.planets[0].position.x, x1 = x0, y0 = f.planets[0].position.y, y1 = y0, z = 0.0f;
    for (const GalacticPlanetInfo& p : f.planets) {
        x0 = std::min(x0, p.position.x);
        x1 = std::max(x1, p.position.x);
        y0 = std::min(y0, p.position.y);
        y1 = std::max(y1, p.position.y);
        z += p.position.z;
    }
    z /= static_cast<float>(f.planets.size());
    const float half = 0.5f * std::max({x1 - x0, y1 - y0, 1.0f}) * 1.2f;
    const float cx = 0.5f * (x0 + x1), cy = 0.5f * (y0 + y1);
    auto to_radar = [&](float wx, float wy) {
        return Vec2f{rc_at.x + (wx - cx) / half * 0.5f * size.x, rc_at.y + (wy - cy) / half * 0.5f * size.y};
    };
    const CommandBarComponent* bc = radar_blip_ >= 0 ? &(*table_)[static_cast<size_t>(radar_blip_)] : nullptr;
    const uint32_t blip_layer = bc ? static_cast<uint32_t>(bc->base_layer) : 3u;
    for (const GalacticPlanetInfo& p : f.planets) {
        if (!p.visible) continue;
        const Vec2f at = to_radar(p.position.x, p.position.y);
        const std::string* t = nullptr;
        if (bc) t = p.selected && !bc->selected_texture.empty() ? &bc->selected_texture[0] : pick(bc->icon_texture, 0);
        Rgba8 col = p.owner_color;
        col.a = 255;
        if (!t || !add_atlas_quad(dl_, blip_layer, HudList::Alpha, *atlas_, *t, at, {}, bc->scale, col))
            add_atlas_quad(dl_, blip_layer, HudList::Alpha, *atlas_, {}, at, {}, 1.0f, col, {6, 6});
        ++stats_.radar_blips;
    }
    // Camera footprint (bounding box of the four ground corners), clipped to the radar.
    const std::array<Vec2f, 4> fp = camera_footprint(f.view, z);
    float fx0 = fp[0].x, fx1 = fx0, fy0 = fp[0].y, fy1 = fy0;
    for (const Vec2f& c : fp) {
        fx0 = std::min(fx0, c.x);
        fx1 = std::max(fx1, c.x);
        fy0 = std::min(fy0, c.y);
        fy1 = std::max(fy1, c.y);
    }
    Vec2f a = to_radar(fx0, fy0), b = to_radar(fx1, fy1);
    const Vec2f lo{rc_at.x - 0.5f * size.x, rc_at.y - 0.5f * size.y}, hi{rc_at.x + 0.5f * size.x, rc_at.y + 0.5f * size.y};
    a = {std::clamp(a.x, lo.x, hi.x), std::clamp(a.y, lo.y, hi.y)};
    b = {std::clamp(b.x, lo.x, hi.x), std::clamp(b.y, lo.y, hi.y)};
    if (b.x - a.x < 1.0f || b.y - a.y < 1.0f) return;
    const CommandBarComponent* vc = radar_view_ >= 0 ? &(*table_)[static_cast<size_t>(radar_view_)] : nullptr;
    const uint32_t view_layer = vc ? static_cast<uint32_t>(vc->base_layer) : 4u;
    const Rgba8 line{255, 255, 255, 220};
    auto bar = [&](Vec2f mn, Vec2f mx) {
        add_atlas_quad(dl_, view_layer, HudList::Alpha, *atlas_, {}, {0.5f * (mn.x + mx.x), 0.5f * (mn.y + mx.y)}, {}, 1.0f,
                       line, {mx.x - mn.x, mx.y - mn.y});
    };
    bar({a.x, a.y}, {b.x, a.y + 1.0f});
    bar({a.x, b.y - 1.0f}, {b.x, b.y});
    bar({a.x, a.y}, {a.x + 1.0f, b.y});
    bar({b.x - 1.0f, a.y}, {b.x, b.y});
    stats_.radar_view = true;
}

// Day pips (HUD_GAPS §4.2): ten balance_pip clones from the fight_bar_g bone of i_main_commandbar. [port] without the
// shell bones: right of Text_Day_Counter's fallback position.
void GalacticHud::build_day_pips(const GalacticHudFrame& f) {
    if (balance_pip_ < 0 || f.cycle_fraction < 0.0f) return;
    // Canvas position of bone `name` (case-insensitive) of shell component `shell`.
    auto bone_at = [&](int32_t shell, std::string_view name, Vec2f& out) {
        if (shell < 0) return false;
        const auto sc = static_cast<uint32_t>(shell);
        const Vec2f off = layout_.shell_offset(sc);
        for (const ShellBone& b : layout_.shell_bones(sc))
            if (ascii_iequals(b.name, name)) {
                out = {b.x + off.x, b.y + off.y};
                return true;
            }
        return false;
    };
    Vec2f at{};
    GalacticHudStats::DayPipAnchor anchor = GalacticHudStats::DayPipAnchor::None;
    if (f.advanced_map_open && bone_at(map_shell_, "day_clock", at))
        anchor = GalacticHudStats::DayPipAnchor::DayClock;
    else if (bone_at(commandbar_, "fight_bar_g", at))
        anchor = GalacticHudStats::DayPipAnchor::FightBar;
    if (anchor == GalacticHudStats::DayPipAnchor::None) {
        if (day_ < 0) return;
        const ComponentPlacement& dp = layout_.placement(static_cast<uint32_t>(day_));
        if (!dp.placed) return;
        at = {dp.center.x + 80.0f, dp.center.y};
        anchor = GalacticHudStats::DayPipAnchor::Fallback;
    }
    // Pixel_Align: snap to the pixel centre.
    const Vec2f px = canvas_.canvas_to_pixel(at.x, at.y);
    at = canvas_.pixel_to_canvas(std::floor(px.x) + 0.5f, std::floor(px.y) + 0.5f);
    stats_.day_pip_anchor = anchor;
    stats_.day_pip_origin = at;
    const auto ci = static_cast<uint32_t>(balance_pip_);
    const CommandBarComponent& c = (*table_)[ci];
    float w = 6.0f;  // [port] when the art is missing
    if (const std::string* t = pick(c.icon_alternate_texture, 1))
        if (const AtlasEntry* e = atlas_->find(*t)) w = static_cast<float>(e->width) * c.scale;
    const int32_t k = std::clamp(static_cast<int32_t>(std::floor(10.0f * f.cycle_fraction)), 0, 9);
    const uint32_t layer = static_cast<uint32_t>(std::max(c.base_layer, kShellLayerMin));
    ButtonRuntime& r = buttons_.state(ci);
    r.hidden = false;
    for (int32_t i = 0; i < 10; ++i) {
        r.alt = i <= k ? 1 : 0;
        r.selected = i == k && f.day_pip_flash;
        buttons_.draw(dl_, *atlas_, fonts_, canvas_, ci, {at.x + w * static_cast<float>(i), at.y}, {}, f.faction_alt, f.time,
                      static_cast<int32_t>(layer) - c.base_layer);
        ++stats_.day_pips;
        if (i <= k) ++stats_.day_pips_lit;
    }
    r.selected = false;
}

std::u32string GalacticHud::text_or(const char* key, std::u32string_view fallback) const {
    if (text_)
        if (const std::u16string* t = text_->find(key)) return utf16_to_utf32(*t);
    return std::u32string(fallback);
}

namespace {
std::u32string replace_all(std::u32string s, std::u32string_view from, std::u32string_view to) {
    for (size_t at = s.find(from); at != std::u32string::npos; at = s.find(from, at + to.size())) s.replace(at, from.size(), to);
    return s;
}
std::u32string lookup32(const TextDatabase* text, const char* key, std::u32string_view fallback) {
    if (text)
        if (const std::u16string* t = text->find(key)) return utf16_to_utf32(*t);
    return std::u32string(fallback);
}
}  // namespace

std::u32string auto_resolve_text(const TextDatabase* text, bool space, bool win, int32_t magnitude, std::u32string_view faction,
                                 std::u32string_view planet, int64_t losses) {
    static const char* const kMag[3] = {"MAJOR", "REGULAR", "MINOR"};
    static const char32_t* const kMagText[3] = {U"major", U"", U"minor"};
    const int32_t m = std::clamp(magnitude, 0, 2);
    char key[48];
    std::snprintf(key, sizeof key, "TEXT_AR_%s_%s_%s", space ? "SPACE" : "LAND", win ? "WIN" : "LOSS", kMag[m]);
    std::u32string fb = U"##0 ";
    if (*kMagText[m]) fb += std::u32string(kMagText[m]) + U" ";
    fb += space ? U"space " : U"land ";
    fb += win ? U"victory at ##1. " : U"defeat at ##1. ";
    fb += space ? U"Ships lost: ##2" : U"Units lost: ##2";
    std::u32string s = lookup32(text, key, fb);
    s = replace_all(std::move(s), U"##0", faction);
    s = replace_all(std::move(s), U"##1", planet);
    return replace_all(std::move(s), U"##2", num32(losses));
}

std::u32string auto_resolve_hero_loss_text(const TextDatabase* text, std::u32string_view hero, std::u32string_view planet) {
    std::u32string s = lookup32(text, "TEXT_AR_HERO_LOSS", U"##0 defeated at ##1");
    s = replace_all(std::move(s), U"##0", hero);
    return replace_all(std::move(s), U"##1", planet);
}

// GALACTIC_DIALOGS §1: the battle-pending banner. With its shell loaded the plate is the .alo and the components sit on
// its bones; without it [port] a flat stand-in plate at the same top-centre spot (as the tactical pause banner).
void GalacticHud::build_battle_prompt(const GalacticHudFrame& f) {
    const GalacticBattlePrompt& b = f.battle;
    if (!b.active) return;
    const bool single = b.single;
    const int32_t title_c = single ? single_text_ : choice_text_;
    constexpr uint32_t kLayer = 9;  // Base_Layer 9 of the prompt components
    const float hh = 0.5f * canvas_.height;
    const bool from_shell = title_c >= 0 && layout_.placement(static_cast<uint32_t>(title_c)).from_bone;
    if (!from_shell) {
        // Plate: frame mesh of i_attack_mode_choice.alo [-218..218 x -94..16] around choice_frame (0, 106) [port stand-in].
        const float top = hh - kAttackShellTop;
        const Vec2f c{0.0f, top + (single ? 85.0f - 30.0f : 106.0f - 39.0f)}, sz{436.0f, single ? 70.0f : 110.0f};
        frame_rect(dl_, kLayer - 1, c, sz, Rgba8{70, 64, 30, 235}, Rgba8{196, 176, 70, 255});
    }
    // Title: planet + TEXT_TACTICAL_{SPACE,LAND}_BATTLE_IS_PENDING in Battle_Pending_Message_Color.
    std::u32string title = b.title;
    if (title.empty())
        title = b.planet + (b.space ? text_or("TEXT_TACTICAL_SPACE_BATTLE_IS_PENDING", U": SPACE BATTLE IMMINENT!")
                                    : text_or("TEXT_TACTICAL_LAND_BATTLE_IS_PENDING", U": PLANETARY BATTLE IMMINENT!"));
    if (title_c >= 0) {
        const ComponentPlacement& p = layout_.placement(static_cast<uint32_t>(title_c));
        if (p.placed) text_at(title_c, title, p.center, kLayer, 255, &b.color);
    }
    // Buttons (TextButton, caption in the component's red embossed text, i_attack_mode_rollover on hover).
    auto button = [&](int32_t ci, const char* key, const char32_t* fallback) {
        if (ci < 0) return;
        const auto u = static_cast<uint32_t>(ci);
        const ComponentPlacement& p = layout_.placement(u);
        if (!p.placed) return;
        ButtonRuntime& r = buttons_.state(u);
        r.hidden = false;
        r.text = text_or(key, fallback);
        Vec2f size = p.size.x > 0 ? p.size : Vec2f{158.0f, 20.0f};  // hidden 158 x 20 button mesh / rollover art
        const Vec2f at = p.size.x > 0 ? p.rect_center : p.center;
        if (!from_shell) frame_rect(dl_, kLayer, at, {size.x + 6.0f, size.y + 6.0f}, Rgba8{30, 26, 14, 235}, Rgba8{150, 130, 60, 255});
        buttons_.draw(dl_, *atlas_, fonts_, canvas_, u, at, size, f.faction_alt, f.time,
                      static_cast<int32_t>(kLayer) - (*table_)[u].base_layer);
        hit(ci, kLayer, at, size);
    };
    if (single) {
        button(single_button_, "TEXT_BUTTON_BEGIN", U"Begin");
    } else {
        button(choice_left_, "TEXT_BUTTON_BEGIN", U"Begin");
        button(choice_right_, "TEXT_BUTTON_RESOLVE_BATTLE", U"Auto-Resolve");
        // Commander icons on their force stacks.
        auto graph = [&](int32_t ci, const GalacticBattleSide& side) {
            if (ci < 0) return;
            const auto u = static_cast<uint32_t>(ci);
            const ComponentPlacement& p = layout_.placement(u);
            if (!p.placed) return;
            const CommandBarComponent& c = (*table_)[u];
            int32_t layers = side.stack;
            if (layers < 0) layers = static_cast<int32_t>(std::min<uint32_t>(5, (side.units + 4) / 5));
            layers = std::clamp(layers, 0, 5);
            if (const std::string* t = pick(c.blank_texture, layers))
                add_atlas_quad(dl_, kLayer, HudList::Alpha, *atlas_, *t, p.center, {}, c.scale, Rgba8{255, 255, 255, 255});
            const float icon_scale = c.has_size && c.size.x > 0 ? c.size.x : 0.75f;
            if (!side.icon.empty())
                add_atlas_quad(dl_, kLayer, HudList::Alpha, *atlas_, side.icon, {p.center.x, p.center.y + 3.0f * static_cast<float>(layers)},
                               {}, icon_scale, Rgba8{255, 255, 255, 255});  // no stand-in square for a missing icon
            ++stats_.battle_graphs;
        };
        graph(graph_left_, b.left);
        graph(graph_right_, b.right);
    }
    stats_.battle_prompt = true;
    stats_.battle_prompt_shell = from_shell;
}

// Screen-text lines (§2.1 / §3.1) [port placement]: centred, stacked down from 140 units below the top edge. Layer
// kScreenTextLayer, the last HUD layer drawn before the GUI dialogs (hud_frame_steps: L9-10, dialogs, L11-12), so an open
// dialog (the in-game menu, Save / Load) covers the lines like the rest of the HUD; L11-12 stay for what follows the
// cursor (drag icons) [port].
void GalacticHud::build_screen_text(const GalacticHudFrame& f) {
    if (f.screen_text.empty() || !fonts_) return;
    HudTextStyle st;
    st.font = fonts_->font("EmpireAtWar-Medium", 9.0f, canvas_.screen_height);
    if (st.font < 0) return;
    st.outline = true;
    st.align = TextAlign::Center;
    float y = 0.5f * canvas_.height - 140.0f;
    for (const GalacticScreenText& l : f.screen_text) {
        if (l.text.empty() || l.alpha == 0) continue;
        st.color = with_alpha(l.color, l.alpha);
        st.outline_color = with_alpha(Rgba8{0, 0, 0, 255}, l.alpha);
        add_text(dl_, kScreenTextLayer, HudList::Text, *fonts_, canvas_, l.text, {0.0f, y}, st);
        y -= 16.0f;
        ++stats_.screen_text_lines;
    }
}

void GalacticHud::build(const GalacticHudFrame& f) {
    dl_.clear();
    hits_.clear();
    pads_.clear();
    stack_hits_.clear();
    ability_rows_.clear();
    stats_ = {};
    if (!table_) return;
    // Selected planet: the frame's flag, kept by id across frames.
    selected_ = -1;
    for (size_t i = 0; i < f.planets.size(); ++i)
        if (f.planets[i].selected) selected_ = static_cast<int32_t>(i);
    selected_id_ = selected_ >= 0 ? f.planets[static_cast<size_t>(selected_)].planet : 0;
    // Fleet slots (base 40.jpg): shown with a fleet selection or while dragging a fleet / unit.
    slots_shown_ = f.show_fleet_slots || drag_.active;
    empty_slot_color_ = f.empty_slot_color;
    for (const GalacticPlanetInfo& p : f.planets)
        for (const GalacticPlanetStack& st : p.stacks)
            slots_shown_ = slots_shown_ || (st.selected && !st.land);
    stats_.fleet_slots_shown = slots_shown_;
    paused_ = f.paused;
    if (pause_ >= 0) buttons_.state(static_cast<uint32_t>(pause_)).selected = f.paused;
    // GALACTIC_DIALOGS §4: the organiser shows with a selected planet in the planet close-up (host: organize_open) while
    // the advanced planet map is closed. §1: one of the two battle-prompt shells while the prompt is up.
    const bool organize = f.organize_open && !f.advanced_map_open;
    const bool prompt2 = f.battle.active && !f.battle.single, prompt1 = f.battle.active && f.battle.single;
    shell_vis_.clear();
    auto shell = [&](int32_t ci, bool on) {
        if (ci < 0) return;
        layout_.set_shell_active(static_cast<uint32_t>(ci), on);
        shell_vis_.push_back({static_cast<uint32_t>(ci), on});
    };
    shell(organize_, organize);
    shell(pending_shell_, prompt2);
    shell(story_pending_shell_, prompt1);
    stats_.organize_shown = organize;
    // Prompt buttons not shown this frame take no clicks.
    for (const int32_t ci : {choice_left_, choice_right_, single_button_})
        if (ci >= 0) buttons_.state(static_cast<uint32_t>(ci)).hidden = true;
    layout_.update(canvas_, *table_);
    build_labels(f);
    build_special_abilities(f);
    build_bar(f);
    build_radar(f);
    build_day_pips(f);
    build_fleets(f);
    build_objectives(f);
    build_battle_prompt(f);
    build_screen_text(f);
    // FIXPASS_GALACTIC §4.2 (capture C2): as Movie_tactical, the bone at the movie's bottom-left corner, 200 x 200.
    if (movie_ >= 0 && f.advisor_page >= 0 && f.advisor_page < static_cast<int32_t>(kHudPageCount)) {
        const auto mu = static_cast<uint32_t>(movie_);
        const ComponentPlacement& p = layout_.placement(mu);
        if (p.placed) {
            const CommandBarComponent& c = (*table_)[mu];
            const Vec2f sz = c.size.x > 0 && c.size.y > 0 ? c.size : Vec2f{200.0f, 200.0f};
            add_movie_quad(dl_, layer_of(c, p), p.center, {p.center.x + sz.x, p.center.y + sz.y},
                           static_cast<uint16_t>(f.advisor_page), f.advisor_alpha);
            stats_.advisor_movie = true;
        }
    }
    // Planet screen discs for drop picking.
    planet_px_.clear();
    for (const GalacticPlanetInfo& p : f.planets) {
        if (!p.visible) continue;
        float sx = 0, sy = 0, rx = 0, ry = 0;
        if (!project(f.view, p.position, sx, sy)) continue;
        const Vec3 edge{p.position.x + f.view.right.x * p.radius, p.position.y + f.view.right.y * p.radius,
                        p.position.z + f.view.right.z * p.radius};
        const float r = project(f.view, edge, rx, ry) ? std::hypot(rx - sx, ry - sy) : 0.0f;
        planet_px_.push_back({p.planet, sx, sy, r});
    }
    // The dragged pad's icon follows the cursor (Should_Render_At_Drag_Pos), topmost HUD layer.
    if (drag_.active && drag_.component >= 0) {
        const CommandBarComponent& c = (*table_)[static_cast<size_t>(drag_.component)];
        std::string icon = drag_.icon;
        // Drag_Back ("show the pancakes while dragging"): a map fleet's stack under its icon.
        const std::string_view db = c.raw_value("DRAG_BACK");
        if (drag_.fleet_index == kMapFleetIndex && data::parse_flag_char(db))
            if (const std::string* b = pick(c.blank_texture, drag_.layers)) {
                add_atlas_quad(dl_, kHudLayers - 1, HudList::Alpha, *atlas_, *b, canvas_.pixel_to_canvas(drag_.cursor_px.x, drag_.cursor_px.y), {},
                               c.scale, Rgba8{drag_.tint.r, drag_.tint.g, drag_.tint.b, 200});  // the stack keeps its owner tint
            }
        if (icon.empty())
            if (const std::string* a = pick(c.icon_alternate_texture, f.faction_alt)) icon = *a;
        if (!icon.empty())
            add_atlas_quad(dl_, kHudLayers - 1, HudList::Alpha, *atlas_, icon, canvas_.pixel_to_canvas(drag_.cursor_px.x, drag_.cursor_px.y),
                           {}, c.scale, Rgba8{255, 255, 255, 200}, {32, 32});
    }
    tooltip_.update(buttons_, *table_, tooltip_cfg_, f.time);
    if (fonts_)
        stats_.tooltip = tooltip_.draw(dl_, *atlas_, *fonts_, canvas_, *table_, buttons_, text_, f.cursor_px, f.faction_alt,
                                       tooltip_cfg_);
    stats_.hit_boxes = static_cast<uint32_t>(hits_.size());
}

void GalacticHud::update_input(const RoutedInput& routed, bool shift, bool ctrl, double now,
                               std::vector<commands::EventData>& out, std::vector<HudAction>* actions) {
    if (!table_) return;
    bool dropped = false;
    drag_update(routed, out, dropped);
    std::vector<ButtonClick> clicks;
    buttons_.update(routed.hover_component, routed.hud, shift, ctrl, now, clicks);
    if (dropped) clicks.clear();  // the release ended a drag, not a click
    auto act = [&](HudAction a) {
        if (actions) actions->push_back(std::move(a));
    };
    // ui4 t02: a left click on a greyed build pad (HudButtons drops clicks on disabled components) is refused here with
    // the GUI negative feedback instead of a Production command the sim would refuse silently.
    if (!dropped)
        for (const HudPointerEvent& e : routed.hud) {
            if (e.kind != HudPointerEvent::Kind::Click || e.button != Button::Left || e.component < 0) continue;
            for (const PadSlot& s : pads_) {
                if (s.component != e.component || s.is_queue || s.foreign || !s.select.empty()) continue;
                if (s.option.enabled && !s.option.production_disabled && s.option.block == HudBuildOption::Block::None) break;
                HudAction a;
                a.kind = HudAction::Kind::BuildRefused;
                a.component = e.component;
                a.name = (*table_)[static_cast<size_t>(e.component)].name;
                a.value = static_cast<int32_t>(s.option.block);
                a.unit = s.option.type_index;
                act(std::move(a));
                break;
            }
        }
    for (const ButtonClick& k : clicks) {
        bool handled = false;
        for (const PadSlot& s : pads_) {
            if (s.component != k.component) continue;
            handled = true;
            if (s.foreign) break;  // UI.md §7.2 [port]: a foreign fleet's pads are drawn, not clickable
            if (s.is_queue) {
                if (k.button == Button::Left)
                    out.push_back(commands::ev::production(s.queue.type_index, s.queue.location, true, s.queue.queue, s.queue.index));
            } else if (!s.select.empty()) {
                HudAction a;
                a.kind = HudAction::Kind::GalacticSelect;
                a.component = k.component;
                a.ids = s.select;
                a.button = k.button;
                a.shift = k.shift;
                a.ctrl = k.ctrl;
                act(std::move(a));
            } else if (k.button == Button::Left && s.option.enabled && !s.option.production_disabled &&
                       s.option.block == HudBuildOption::Block::None && selected_id_ != 0) {
                out.push_back(commands::ev::production(s.option.type_index, selected_id_));
            } else if (k.button == Button::Right && selected_id_ != 0) {
                out.push_back(commands::ev::production(s.option.type_index, selected_id_, true));  // [port] right click cancels
            }
            break;
        }
        if (handled) continue;
        if (k.component >= 0 && (k.component == planet_fleet_ || k.component == planet_land_)) {
            // GALAXY_MAP_PRESENTATION §1 map fleet / land-forces button: left click selects that fleet [port: right = none].
            const StackHit* sh = stack_at(k.component, static_cast<float>(k.x), static_cast<float>(k.y));
            if (sh && sh->fleet != 0 && k.button == Button::Left) {  // [inferred] an empty slot oval: no click action
                HudAction a;
                a.kind = HudAction::Kind::GalacticSelect;
                a.component = k.component;
                a.name = (*table_)[static_cast<size_t>(k.component)].name;
                a.ids = {sh->fleet};
                a.unit = sh->planet;
                a.value = sh->slot;
                a.button = k.button;
                a.shift = k.shift;
                a.ctrl = k.ctrl;
                act(std::move(a));
            }
            continue;
        }
        if (k.component == pause_ && k.button == Button::Left) {  // [port: Gabriel] pause through the command stream
            out.push_back(pause_event(!paused_));
            continue;
        }
        if (k.component == special_ && special_ >= 0) {  // g_special_ability: the row under the click
            const Vec2f c = canvas_.pixel_to_canvas(static_cast<float>(k.x) + 0.5f, static_cast<float>(k.y) + 0.5f);
            for (size_t i = 0; i < ability_rows_.size(); ++i) {
                const AbilityRow& r = ability_rows_[i];
                if (c.x < r.min.x || c.x > r.max.x || c.y < r.min.y || c.y > r.max.y) continue;
                if (r.enabled && k.button == Button::Left) {
                    HudAction a;
                    a.kind = HudAction::Kind::Button;
                    a.component = k.component;
                    a.name = (*table_)[static_cast<size_t>(k.component)].name;
                    a.unit = r.hero;
                    a.value = static_cast<int32_t>(i);
                    if (selected_id_ != 0) a.ids = {selected_id_};
                    a.button = k.button;
                    a.shift = k.shift;
                    a.ctrl = k.ctrl;
                    act(std::move(a));
                }
                break;
            }
            continue;
        }
        HudAction a;
        a.component = k.component;
        a.name = (*table_)[static_cast<size_t>(k.component)].name;
        a.button = k.button;
        a.shift = k.shift;
        a.ctrl = k.ctrl;
        if (k.component == planet_info_) {
            a.kind = HudAction::Kind::PlanetInfo;
            a.unit = selected_id_;
        } else if (k.button == Button::Left && (k.component == choice_left_ || k.component == single_button_)) {
            a.kind = HudAction::Kind::BattleBegin;  // GALACTIC_DIALOGS §1.5
        } else if (k.button == Button::Left && k.component == choice_right_) {
            a.kind = HudAction::Kind::BattleAutoResolve;
        } else {
            a.kind = HudAction::Kind::Button;
        }
        act(std::move(a));
    }
}

}  // namespace alamo::ui
