#include "alamo/ui/tactical_hud.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "alamo/ui/gui_dialog.hpp"
#include "alamo/ui/hud_font.hpp"
#include "alamo/ui/picking.hpp"

namespace alamo::ui {

namespace {
constexpr Rgba8 kPanelBack{12, 18, 28, 190}, kPanelEdge{150, 170, 190, 230};
constexpr Vec2f kSlotFallback{40, 40};

std::u32string ascii32(std::string_view s) {
    std::u32string o;
    for (char c : s) o.push_back(static_cast<char32_t>(static_cast<unsigned char>(c)));
    return o;
}

// A rectangle outline of 2-unit lines plus a translucent fill (the [port] stand-in frame).
void dot(HudDrawList& dl, uint32_t layer, Vec2f c, float half, Rgba8 color) {
    HudQuad q;
    q.min = {c.x - half, c.y - half};
    q.max = {c.x + half, c.y + half};
    q.color = color;
    dl.add(layer, HudList::Alpha, q);
}

std::vector<uint32_t> capped(std::vector<uint32_t> ids, size_t n = 127) {
    if (ids.size() > n) ids.resize(n);
    return ids;
}
}  // namespace

std::vector<SelectedGroup> group_selection(std::span<const HudUnit> units, size_t max_groups) {
    std::vector<SelectedGroup> groups;
    std::map<uint32_t, size_t> by_type;  // lookup only
    std::vector<double> hsum, ssum;
    std::vector<uint32_t> scount;
    for (const HudUnit& u : units) {  // ascending id: first occurrence = lowest id
        if (!u.selected) continue;
        auto it = by_type.find(u.type);
        if (it == by_type.end()) {
            if (groups.size() >= max_groups) continue;
            it = by_type.emplace(u.type, groups.size()).first;
            groups.push_back(SelectedGroup{u.type, {}, 1.0f, -1.0f});
            hsum.push_back(0.0);
            ssum.push_back(0.0);
            scount.push_back(0);
        }
        const size_t g = it->second;
        groups[g].ids.push_back(u.id);
        hsum[g] += u.health;
        if (u.shield >= 0.0f) {
            ssum[g] += u.shield;
            ++scount[g];
        }
    }
    for (size_t g = 0; g < groups.size(); ++g) {
        std::sort(groups[g].ids.begin(), groups[g].ids.end());
        groups[g].health = static_cast<float>(hsum[g] / static_cast<double>(groups[g].ids.size()));
        if (scount[g]) groups[g].shield = static_cast<float>(ssum[g] / static_cast<double>(scount[g]));
    }
    return groups;
}

uint32_t add_drag_box(HudDrawList& dl, const HudCanvas& canvas, const DragRect& rect, uint32_t layer) {
    if (canvas.screen_width == 0 || canvas.screen_height == 0) return 0;
    const PixelBounds b = drag_select_bounds(rect);
    uint32_t n = 0;
    // Pixel-edge rectangle (+Y down) -> canvas quad (+Y up, min = bottom-left).
    auto quad = [&](float px0, float py0, float px1, float py1, Rgba8 c) {
        if (px1 <= px0 || py1 <= py0) return;
        HudQuad q;
        q.min = canvas.pixel_to_canvas(px0, py1);
        q.max = canvas.pixel_to_canvas(px1, py0);
        q.color = c;
        q.page = kHudPageWhite;
        dl.add(layer, HudList::Alpha, q);
        ++n;
    };
    quad(b.x0 + 1.0f, b.y0 + 1.0f, b.x1 - 1.0f, b.y1 - 1.0f, kDragBoxFill);
    quad(b.x0, b.y0, b.x1, b.y0 + 1.0f, kDragBoxOutline);                                      // top
    if (b.y1 - 1.0f > b.y0) quad(b.x0, b.y1 - 1.0f, b.x1, b.y1, kDragBoxOutline);              // bottom
    quad(b.x0, b.y0 + 1.0f, b.x0 + 1.0f, b.y1 - 1.0f, kDragBoxOutline);                        // left
    if (b.x1 - 1.0f > b.x0) quad(b.x1 - 1.0f, b.y0 + 1.0f, b.x1, b.y1 - 1.0f, kDragBoxOutline);  // right
    return n;
}

std::u32string hud_thousands(int64_t v) {
    char raw[32];
    std::snprintf(raw, sizeof raw, "%lld", static_cast<long long>(v));
    const size_t n = std::strlen(raw), lead = raw[0] == '-' ? 1u : 0u;
    std::u32string o;
    for (size_t k = 0; k < n; ++k) {
        if (k > lead && (n - k) % 3 == 0) o.push_back(U',');
        o.push_back(static_cast<char32_t>(raw[k]));
    }
    return o;
}

commands::EventData reinforce_event(uint32_t unit, Vec3 p) {
    commands::EventData e(commands::EventType::Reinforce);
    e.set("unit", unit).set_f("x", p.x).set_f("y", p.y).set_f("z", p.z);
    return e;
}

commands::EventData ability_mode_event(std::span<const uint32_t> units, uint8_t ability, bool on) {
    commands::EventData e(commands::EventType::SetUnitAbilityMode);
    e.set("ability", ability).set("on", on ? 1u : 0u).set("play_response", 1u);
    e.set_ids("units", capped(std::vector<uint32_t>(units.begin(), units.end())));
    return e;
}

commands::EventData ability_autofire_event(std::span<const uint32_t> units, uint8_t ability, bool on) {
    commands::EventData e(commands::EventType::SetAbilityAutofire);
    e.set_ids("units", capped(std::vector<uint32_t>(units.begin(), units.end())));
    e.set("ability", ability).set("on", on ? 1u : 0u);
    return e;
}

void TacticalHud::init(const ComponentTable& table, const TextureAtlas& atlas, FontCache& fonts, const TextDatabase* text) {
    table_ = &table;
    atlas_ = &atlas;
    fonts_ = &fonts;
    text_ = text;
    world_.bind(table);
    buttons_.bind(table);
    slot_component_.assign(48, -1);
    health_component_.assign(48, -1);
    shield_component_.assign(48, -1);
    special_button_.assign(48, -1);
    special_border_.assign(24, -1);
    queue_component_.assign(20, -1);
    reinforce_component_.clear();
    for (int i = 0; i < 48; ++i) {
        const auto s = static_cast<size_t>(i);
        slot_component_[s] = find_fmt(table, "s_select_%02d", i);
        health_component_[s] = find_fmt(table, "s_health_%02d", i);
        shield_component_[s] = find_fmt(table, "s_shield_%02d", i);
        special_button_[s] = find_fmt(table, "special_button_%02d", i);
    }
    for (int i = 0; i < 24; ++i) special_border_[static_cast<size_t>(i)] = find_fmt(table, "special_border_%02d", i);
    for (int i = 0; i < 20; ++i) queue_component_[static_cast<size_t>(i)] = find_fmt(table, "tqueue%02d", i);
    for (int i = 0; i < 6; ++i) buy_component_[static_cast<size_t>(i)] = find_fmt(table, "Build_%02d", i);
    for (int r = 0; r < 5; ++r)
        for (int c = 0; c < 4; ++c) reinforce_component_.push_back(find_fmt(table, "r_%02d%02d", r, c));
    radar_ = table.find("radar");
    radar_blip_ = table.find("radar_blip");
    credits_ = table.find("Text_Credits_tactical");
    planet_text_ = table.find("Text_Planet_tactical");
    movie_ = table.find("Movie_tactical");
    tech_text_ = table.find("Text_Tactical_Tech");
    // i_tactical_controls.alo has a Text_Planetary_Pop bone (icon + count) and no Text_Pop_Cap bone.
    pop_text_ = table.find("Text_Planetary_Pop");
    if (pop_text_ < 0) pop_text_ = table.find("Text_Pop_Cap");
    shell_ = table.find("i_main_skirmish");
    retreat_ = table.find("b_retreat");
    for (int i = 0; i < 6; ++i) order_button_[i] = find_fmt(table, "c_button%02d", i);
    pause_ = table.find("b_play_pause_t");
    pause_g_ = table.find("b_play_pause");
    pause_text_ = table.find("text_attack");
    resume_ = table.find("attack_button");
    obj_back_ = table.find("objective_back");
    obj_header_ = table.find("objective_header_text");
    obj_text_ = table.find("objective_text");
    obj_icon_ = table.find("objective_icon");
    // Components drawn by a panel / the world layer, not by build_buttons.
    dynamic_.assign(table.size(), 0);
    auto mark = [&](int32_t i) {
        if (i >= 0) dynamic_[static_cast<size_t>(i)] = 1;
    };
    for (const auto* v : {&slot_component_, &health_component_, &shield_component_, &special_button_, &special_border_,
                          &queue_component_, &reinforce_component_})
        for (int32_t i : *v) mark(i);
    for (int32_t i : buy_component_) mark(i);
    for (int32_t i : {radar_, radar_blip_, credits_, pause_text_, resume_, obj_back_, obj_header_, obj_text_, obj_icon_, movie_})
        mark(i);
    for (uint32_t i = 0; i < table.size(); ++i) {
        const std::string& n = table[i].name;
        auto starts = [&](std::string_view p) {
            if (n.size() < p.size()) return false;
            for (size_t k = 0; k < p.size(); ++k)
                if (std::tolower(static_cast<unsigned char>(n[k])) != p[k]) return false;
            return true;
        };
        if (starts("st_") || starts("tooltip_") || starts("encyclopedia_") || starts("generic_flytext") ||
            starts("reinforcement_window"))
            dynamic_[i] = 1;
    }
    // [port] mission / mode specific components the code shows on demand (Death Star countdown and fire switches,
    // story holocron tab, movie window): hidden until the host / Lua clears ButtonRuntime::hidden.
    for (const char* n : {"DS_Countdown_Timer", "deathstar_switch", "pb_switch", "sl_switch", "pb_switch_recharge",
                          "sl_switch_recharge", "b_story_arc_t", "Movie_tactical"}) {
        const int32_t i = table.find(n);
        if (i >= 0) buttons_.state(static_cast<uint32_t>(i)).hidden = true;
    }
    resize(canvas_.screen_width, canvas_.screen_height);
}

void TacticalHud::resize(uint32_t width, uint32_t height) {
    resize_canvas(canvas_, fonts_, width, height);
    if (table_) {
        apply_tactical_fallback_layout(*table_, canvas_, layout_);
        layout_.update(canvas_, *table_);
    }
}

void TacticalHud::hit(int32_t component, uint32_t layer, Vec2f center, Vec2f size, uint32_t slot) {
    hits_.push_back(make_hit_box(component, layer, center, size, slot));
}

int32_t TacticalHud::hit_test(int32_t px, int32_t py) const { return topmost_hit(hits_, canvas_, px, py); }

std::vector<uint32_t> TacticalHud::selection_ids() const {
    std::vector<uint32_t> ids;
    for (const SelectedGroup& g : groups_) ids.insert(ids.end(), g.ids.begin(), g.ids.end());
    std::sort(ids.begin(), ids.end());
    return ids;
}

Vec2f TacticalHud::component_size(uint32_t component, const ComponentPlacement& p) const {
    return component_art_size((*table_)[component], p, *atlas_, {});
}

void TacticalHud::on_pointer(std::span<const HudPointerEvent> events, std::vector<commands::EventData>& out) const {
    for (const HudPointerEvent& e : events) {
        if (e.kind != HudPointerEvent::Kind::Click || e.button != Button::Left || e.component < 0) continue;
        for (size_t s = 0; s < groups_.size() && s < slot_component_.size(); ++s) {
            if (slot_component_[s] == e.component) {
                std::vector<commands::ObjectId> ids(groups_[s].ids.begin(), groups_[s].ids.end());
                out.push_back(commands::ev::select(capped(std::move(ids))));
            }
        }
    }
}

void TacticalHud::update_input(const RoutedInput& routed, bool shift, bool ctrl, double now,
                               std::vector<commands::EventData>& out, std::vector<HudAction>* actions) {
    if (!table_) return;
    std::vector<ButtonClick> clicks;
    buttons_.update(routed.hover_component, routed.hud, shift, ctrl, now, clicks);
    auto act = [&](HudAction a) {
        if (actions) actions->push_back(std::move(a));
    };
    // Radar: left press / drag = camera jump to the cursor (UI.md §1.7 click-to-move-camera).
    for (const HudPointerEvent& e : routed.hud) {
        if (e.button != Button::Left) continue;
        if (e.kind == HudPointerEvent::Kind::Press) radar_drag_ = radar_ >= 0 && e.component == radar_;
        if (e.kind == HudPointerEvent::Kind::Release) radar_drag_ = false;
    }
    if (radar_drag_ && routed.owner[static_cast<size_t>(Button::Left)] == PointerTarget::Hud && radar_view_.valid()) {
        const Vec2f cp = canvas_.pixel_to_canvas(static_cast<float>(routed.world.mouse_x) + 0.5f,
                                                 static_cast<float>(routed.world.mouse_y) + 0.5f);
        const Vec2f w = radar_view_.to_world(cp);
        HudAction a;
        a.kind = HudAction::Kind::RadarCameraJump;
        a.component = radar_;
        a.x = w.x;
        a.y = w.y;
        act(std::move(a));
    } else if (routed.owner[static_cast<size_t>(Button::Left)] != PointerTarget::Hud) {
        radar_drag_ = false;
    }

    for (const ButtonClick& k : clicks) {
        const auto ci = static_cast<uint32_t>(k.component);
        const CommandBarComponent& c = (*table_)[ci];
        // Selection portraits.
        bool handled = false;
        for (size_t s = 0; s < groups_.size() && s < slot_component_.size(); ++s) {
            if (slot_component_[s] != k.component || k.button != Button::Left) continue;
            handled = true;
            std::vector<uint32_t> ids;
            if (k.shift) {  // [port] shift: remove the group from the selection
                for (uint32_t id : selection_ids())
                    if (!std::binary_search(groups_[s].ids.begin(), groups_[s].ids.end(), id)) ids.push_back(id);
            } else if (k.ctrl) {  // [port] ctrl: the group's first unit only
                ids.push_back(groups_[s].ids.front());
            } else {
                ids = groups_[s].ids;
            }
            out.push_back(commands::ev::select(capped(std::move(ids))));
        }
        if (handled) continue;
        // Ability buttons.
        for (const AbilitySlot& a : ability_slots_) {
            if (a.component != k.component) continue;
            handled = true;
            if (!a.ability.enabled) break;
            if (k.button == Button::Right) {
                out.push_back(ability_autofire_event(a.users, a.ability.ability_type, !a.ability.autofire));
            } else if (a.ability.targeted) {
                HudAction h;
                h.kind = HudAction::Kind::BeginAbilityTargeting;
                h.component = k.component;
                h.name = c.name;
                h.ability_type = a.ability.ability_type;
                h.value = a.ability.target_kind != 0 ? a.ability.target_kind : -1;  // TargetingController: -1 = by type
                h.ids = a.users;
                h.queued = k.shift;  // [port: Gabriel] queue modifier at the click (the host may apply its own binding)
                act(std::move(h));
            } else {
                out.push_back(ability_mode_event(a.users, a.ability.ability_type, !a.ability.active));
            }
            break;
        }
        if (handled) continue;
        // Build queue: left click cancels the entry (CommandBarClass::Cancel_Production, UI.md §2.3) -> TacticalCancel.
        for (const QueueSlot& q : queue_slots_) {
            if (q.component != k.component) continue;
            handled = true;
            if (k.button == Button::Left) {
                HudAction h;
                h.kind = HudAction::Kind::TacticalCancel;
                h.component = k.component;
                h.name = c.name;
                h.unit = q.entry.location;
                h.value = q.entry.type_index;
                h.ids = {q.entry.queue, q.entry.index};
                act(std::move(h));
            }
            break;
        }
        if (handled) continue;
        // Buy buttons: left click on an enabled option -> TacticalBuy (disabled ones never click).
        for (const BuySlot& b : buy_slots_) {
            if (b.component != k.component) continue;
            handled = true;
            if (k.button == Button::Left && b.option.block == HudBuildOption::Block::None) {
                HudAction h;
                h.kind = HudAction::Kind::TacticalBuy;
                h.component = k.component;
                h.name = c.name;
                h.unit = builder_;
                h.value = b.option.type_index;
                h.shift = k.shift;
                h.ctrl = k.ctrl;
                act(std::move(h));
            }
            break;
        }
        if (handled) continue;
        for (const ReinforceSlot& r : reinforce_slots_) {
            if (r.component != k.component) continue;
            handled = true;
            if (k.button == Button::Left && r.entry.available) {
                HudAction h;
                h.kind = HudAction::Kind::BeginReinforcement;
                h.component = k.component;
                h.name = c.name;
                h.unit = r.entry.unit;
                act(std::move(h));
            }
            break;
        }
        if (handled) continue;
        if (k.component == radar_ && radar_view_.valid()) {
            const Vec2f w = radar_view_.to_world(canvas_.pixel_to_canvas(static_cast<float>(k.x) + 0.5f,
                                                                         static_cast<float>(k.y) + 0.5f));
            if (k.button == Button::Right && !groups_.empty()) {  // [port] move order on the radar point
                out.push_back(commands::ev::move_to_position(w.x, w.y, radar_ground_z_));
                HudAction h;
                h.kind = HudAction::Kind::RadarPing;
                h.name = "Default_Click";
                h.x = w.x;
                h.y = w.y;
                act(std::move(h));
            }
            continue;
        }
        if (k.component == retreat_ && k.button == Button::Left) {
            out.push_back(commands::ev::retreat(true));
            continue;
        }
        // [port: Gabriel] Game Paused banner: Resume Game -> HudAction::ResumeGame (the host unpauses).
        if (k.component == resume_ && k.button == Button::Left) {
            if (paused_) {
                HudAction h;
                h.kind = HudAction::Kind::ResumeGame;
                h.component = k.component;
                h.name = c.name;
                act(std::move(h));
            }
            continue;
        }
        // [port: Gabriel] pause button (b_play_pause_t / b_play_pause): pause / resume through the command stream.
        if ((k.component == pause_ || k.component == pause_g_) && k.button == Button::Left) {
            out.push_back(pause_event(!paused_));
            continue;
        }
        HudAction h;
        h.kind = HudAction::Kind::Button;
        h.component = k.component;
        h.name = c.name;
        h.button = k.button;
        h.shift = k.shift;
        h.ctrl = k.ctrl;
        act(std::move(h));
    }
    tooltip_.update(buttons_, *table_, tooltip_cfg_, now);
}

void TacticalHud::build(const TacticalHudFrame& frame, std::span<const HudUnit> units, std::span<const HudTypeInfo> types,
                        const FlyTextList* flytext) {
    dl_.clear();
    blobs_.clear();
    hits_.clear();
    stats_ = {};
    if (!table_ || !atlas_ || !fonts_) return;
    faction_alt_ = frame.faction_alt;
    radar_ground_z_ = frame.ground_z;
    paused_ = frame.paused;
    for (int32_t pc : {pause_, pause_g_})
        if (pc >= 0) buttons_.state(static_cast<uint32_t>(pc)).selected = frame.paused;
    layout_.update(canvas_, *table_);
    WorldHudContext wc = frame.world;
    wc.canvas = &canvas_;
    world_.build(wc, units, types, *atlas_, *fonts_, dl_, &blobs_, flytext);
    stats_.world = world_.stats();
    groups_ = group_selection(units);
    if (!frame.world.interface_hidden) {
        build_markers(frame, units);
        build_skeleton(frame);
        build_radar(frame, units, types);
        build_panel(frame, types);
        build_abilities(frame);
        build_queue(frame);
        build_reinforcements(frame);
        build_buy_buttons(frame);
        // Planet panel texts (drawn by build_buttons as TextButtons at their bones).
        auto set_text = [&](int32_t ci, std::u32string t) {
            if (ci >= 0) buttons_.state(static_cast<uint32_t>(ci)).text = std::move(t);
        };
        set_text(planet_text_, frame.planet_name);
        if (frame.tech >= 0) {
            std::u32string t = text_ && text_->find("TEXT_TECH") ? utf16_to_utf32(*text_->find("TEXT_TECH")) : std::u32string(U"Tech:");
            char n[16];
            std::snprintf(n, sizeof n, " %d", frame.tech);
            set_text(tech_text_, t + ascii32(n));
        } else {
            set_text(tech_text_, {});
        }
        if (frame.pop >= 0) {
            char n[32];
            if (frame.pop_cap >= 0) std::snprintf(n, sizeof n, "%d/%d", frame.pop, frame.pop_cap);
            else std::snprintf(n, sizeof n, "%d", frame.pop);
            set_text(pop_text_, ascii32(n));
        } else {
            set_text(pop_text_, {});
        }
        apply_order_buttons(frame.order_buttons);
        build_buttons(frame);
        // C2 (FIXPASS_GALACTIC §4.2): the name's line is centred on the bone (250, 745 vs bone 252, 744). Drawn here because the
        // generic button text hangs a Max_Text_Width block below its anchor, half a line low (QA D12 on the galactic strip).
        if (planet_text_ >= 0 && !frame.planet_name.empty()) {
            const auto u = static_cast<uint32_t>(planet_text_);
            const ComponentPlacement& p = layout_.placement(u);
            const CommandBarComponent& c = (*table_)[u];
            HudTextStyle st;
            st.font = font_of(planet_text_, 10.0f);
            if (p.placed && st.font >= 0 && !buttons_.state(u).hidden) {
                st.color = c.has_text_color ? c.text_color : Rgba8{};
                st.outline = c.text_outline;
                st.align = TextAlign::Center;
                // t47 [port]: one line, narrowed into Max_Text_Width; a wrapped name covered the rows next to it.
                st.width_scale = fit_width_scale(measure_text_block(*fonts_, canvas_, frame.planet_name, 0.0f, st).width, c.max_text_width);
                const TextBlock tb = measure_text_block(*fonts_, canvas_, frame.planet_name, 0.0f, st);
                add_text_block(dl_, layer_of(c, p), HudList::Text, *fonts_, canvas_, frame.planet_name,
                               {p.center.x, p.center.y + 0.5f * tb.height}, 0.0f, st);
            }
        }
        // FIXPASS_GALACTIC §4.2 (capture C2): the advisor *_LOOP movie in Movie_tactical (Icon, Size 200 x 200) with its
        // bone at the movie's BOTTOM-LEFT corner (the projector beam starts at the droid), straight alpha for an Alpha
        // movie, no frame of its own; nothing while no loop plays (advisor_page < 0).
        if (movie_ >= 0 && frame.advisor_page >= 0 && frame.advisor_page < static_cast<int32_t>(kHudPageCount)) {
            const auto mu = static_cast<uint32_t>(movie_);
            const ComponentPlacement& p = layout_.placement(mu);
            if (p.placed) {
                const CommandBarComponent& c = (*table_)[mu];
                const Vec2f sz = c.size.x > 0 && c.size.y > 0 ? c.size : Vec2f{200.0f, 200.0f};
                add_movie_quad(dl_, layer_of(c, p), p.center, {p.center.x + sz.x, p.center.y + sz.y},
                               static_cast<uint16_t>(frame.advisor_page), frame.advisor_alpha);
                stats_.advisor_movie = true;
            }
        }
        build_objectives(frame);
        build_pause_banner(frame);
        // UI.md §1.1 5h: the drag rectangle is queued with the main HUD pass (not while the interface is hidden).
        if (frame.drag_select) stats_.drag_box = add_drag_box(dl_, canvas_, frame.drag_rect) > 0;
        tooltip_.update(buttons_, *table_, tooltip_cfg_, frame.time);
        stats_.tooltip = tooltip_.draw(dl_, *atlas_, *fonts_, canvas_, *table_, buttons_, text_, frame.cursor_px,
                                       frame.faction_alt, tooltip_cfg_);
    }
    stats_.hit_boxes = static_cast<uint32_t>(hits_.size());
    stats_.quads = static_cast<uint32_t>(dl_.total());
}

void TacticalHud::build_markers(const TacticalHudFrame& f, std::span<const HudUnit> units) {
    const View& v = f.world.view;
    stats_.waypoint_dots = add_waypoint_lines(dl_, canvas_, v, f.waypoints, 0);
    if (f.move_acks) stats_.ack_dots = f.move_acks->draw(dl_, canvas_, v, 0);
    if (f.reticles && !f.hardpoints.empty()) {
        const std::vector<HudHardPoint> shown = filter_hardpoints(f.hardpoints, units);
        stats_.reticles = add_hardpoint_reticles(dl_, *atlas_, canvas_, v, *f.reticles, shown, 1, f.images, f.time);
    }
    // [port: Gabriel] queued orders of the selected units (sim queued_orders(unit), from the host).
    if (!f.order_queues.empty()) {
        const QueueMarkerStats qs = add_queue_markers(dl_, fonts_, canvas_, v, f.order_queues, 1);
        stats_.queue_dots = qs.dots;
        stats_.queue_markers = qs.markers;
    }
}

void TacticalHud::build_skeleton(const TacticalHudFrame& f) {
    // [port] stand-in for the i_main_skirmish shell model: a translucent bottom band (layer 2, drawn after the
    // first half of the shell models in the original order) when the shell models are not drawn.
    const float hw = 0.5f * canvas_.width, hh = 0.5f * canvas_.height;
    if (!f.shells_loaded)
        frame_rect(dl_, 2, {0.0f, -hh + 104.0f}, {canvas_.width + 4.0f, 208.0f}, Rgba8{8, 12, 20, 150}, kPanelEdge);
    // Credits / top bar.
    if (f.credits >= 0) {
        const CommandBarComponent* c = credits_ >= 0 ? &(*table_)[static_cast<size_t>(credits_)] : nullptr;
        const ComponentPlacement p = credits_ >= 0 ? layout_.placement(static_cast<uint32_t>(credits_)) : ComponentPlacement{};
        const Vec2f at = p.placed ? p.center : Vec2f{hw - 70.0f, hh - 16.0f};
        const uint32_t layer = c ? layer_of(*c, p) : 2u;
        if (c && !c->icon_texture.empty())
            add_atlas_quad(dl_, layer, HudList::Alpha, *atlas_, c->icon_texture[0], {at.x + 14.0f, at.y}, {}, c->scale, Rgba8{});
        HudTextStyle st;
        st.font = fonts_->font(c && !c->font_name.empty() ? c->font_name : std::string(kFallbackFontFace),
                               c && c->font_point_size > 0 ? c->font_point_size : 7.0f, canvas_.screen_height);
        st.color = c && c->has_text_color ? c->text_color : Rgba8{255, 212, 33, 255};
        st.outline = true;
        st.align = c && c->right_justified ? TextAlign::Right : TextAlign::Left;
        const Vec2f off = c ? c->text_offset : Vec2f{};
        add_text(dl_, layer, HudList::Text, *fonts_, canvas_, hud_thousands(f.credits), {at.x + off.x, at.y - off.y}, st);
        if (credits_ >= 0 && c && !c->tooltip_text.empty()) {
            const Vec2f sz = p.size.x > 0 ? p.size : Vec2f{90, 20};
            hit(credits_, layer, p.size.x > 0 ? p.rect_center : at, sz);
        }
    }
}

void TacticalHud::build_radar(const TacticalHudFrame& f, std::span<const HudUnit> units, std::span<const HudTypeInfo> types) {
    radar_view_ = {};
    radar_models_.clear();
    if (radar_ < 0) return;
    const CommandBarComponent& rc = (*table_)[static_cast<size_t>(radar_)];
    const ComponentPlacement& p = layout_.placement(static_cast<uint32_t>(radar_));
    if (!p.placed) return;
    const Vec2f size = p.size.x > 0 ? p.size : rc.has_size ? rc.size : Vec2f{171, 175};
    const Vec2f center = p.size.x > 0 ? p.rect_center : p.center;
    const uint32_t layer = std::max(3u, layer_of(rc, p));
    if (!f.shells_loaded) frame_rect(dl_, 2, center, {size.x + 10.0f, size.y + 10.0f}, kPanelBack, kPanelEdge);
    const std::string backdrop = f.radar_settings ? (f.world.land ? f.radar_settings->land_backdrop : f.radar_settings->space_backdrop)
                                                  : std::string("i_radar_map_grid.tga");
    if (!add_atlas_quad(dl_, layer, HudList::Alpha, *atlas_, backdrop, center, size, 1.0f, Rgba8{255, 255, 255, 160}))
        add_atlas_quad(dl_, layer, HudList::Alpha, *atlas_, {}, center, {}, 1.0f, Rgba8{4, 10, 18, 200}, size);
    // Terrain + fog of war: the dynamic radar page.
    if (f.radar_image && !f.radar_image->empty()) {
        HudQuad q;
        q.min = {center.x - 0.5f * size.x, center.y - 0.5f * size.y};
        q.max = {center.x + 0.5f * size.x, center.y + 0.5f * size.y};
        q.page = kHudPageRadar;
        dl_.add(layer, HudList::Alpha, q);
    }
    hit(radar_, layer, center, size);
    radar_view_.center = center;
    radar_view_.size = size;
    radar_view_.min_x = f.map_min_x;
    radar_view_.min_y = f.map_min_y;
    radar_view_.max_x = f.map_max_x;
    radar_view_.max_y = f.map_max_y;
    if (!radar_view_.valid()) return;
    // Blips (radar_blip, layer 5): every unit in its team colour; selected units colorized (Radar_Colorize_Selected_Units).
    const CommandBarComponent* bc = radar_blip_ >= 0 ? &(*table_)[static_cast<size_t>(radar_blip_)] : nullptr;
    const uint32_t blayer = bc ? std::max<uint32_t>(static_cast<uint32_t>(bc->base_layer), layer + 1) : 5u;
    // Per type (UI.md §1.7): Is_Visible_On_Radar / Is_Visible_On_Enemy_Radar, Radar_Icon_Name at Radar_Icon_Size
    // (logical -1..1 radar space -> canvas: x size / 2) [port: tinted with the blip colour]; no icon: the dot.
    const int32_t local = f.world.local_player;
    for (const HudUnit& u : units) {
        const HudTypeInfo* t = u.type < types.size() ? &types[u.type] : nullptr;
        if (t && !t->visible_on_radar) continue;
        const bool hostile = f.world.hostile ? f.world.hostile(u.owner) : (u.owner >= 0 && u.owner != local);
        if (t && hostile && !t->visible_on_enemy_radar) continue;
        if (hostile && u.jammed) continue;  // R5 sensor jamming: the jammer's side drops these units from the enemy radar (0x35b4d0)
        if (hostile && u.stealthed) continue;  // R5 stealth: the same filter drops hostile Is_Stealthed units (ABILITY_VISUALS 1.4)
        // q6 t19 (OBJECT_SCRIPTS §10.3): a Lua-hidden unit (model hidden) has no blip unless Visible_On_Radar_When_Fogged.
        if (u.script_hidden && !(t && t->visible_on_radar_when_fogged)) continue;
        const Vec2f c = radar_view_.to_canvas(u.position.x, u.position.y);
        if (!radar_view_.inside(c)) continue;
        Rgba8 col = u.selected && f.radar_colorize_selected ? f.radar_selected_color : u.team_color;
        col.a = 255;
        bool drawn = false;
        if (t && !t->radar_icon.empty()) {
            const Vec2f sz = t->radar_icon_size.x > 0.0f && t->radar_icon_size.y > 0.0f
                                 ? Vec2f{t->radar_icon_size.x * 0.5f * radar_view_.size.x, t->radar_icon_size.y * 0.5f * radar_view_.size.y}
                                 : Vec2f{};
            drawn = add_atlas_quad(dl_, blayer, HudList::Alpha, *atlas_, t->radar_icon, c, sz, 1.0f, col);
            if (drawn) ++stats_.radar_icons;
        }
        if (!drawn) dot(dl_, blayer, c, u.selected ? 2.5f : 1.5f, col);
        ++stats_.radar_blips;
    }
    // Pings: the event type's .alo (Event_Model_Name, UI.md §1.7) goes to radar_models() for the host's HudModelDrawer;
    // [port] an expanding, fading ring of dots stands in when the type has no model or the host draws no models.
    if (f.radar_events) {
        for (const RadarEvent& e : f.radar_events->items()) {
            const Vec2f c = radar_view_.to_canvas(e.x, e.y);
            if (!radar_view_.inside(c)) continue;
            const RadarEventType* et = f.radar_settings ? f.radar_settings->event(e.type) : nullptr;
            if (et && !et->model.empty()) {
                // Event_Model_Scale is in the RadarMap.xml's logical screen space (-1..1 = the screen): the vanilla models are one
                // ~20-60 unit quad, so x 0.001..0.003 only gives a visible ping (10 .. 90 canvas units) against the screen
                // half width (against the radar half width a click would be 1.7 units) [M].
                radar_models_.push_back({et->model, c, et->scale * 0.5f * canvas_.width, e.age, e.id});
                ++stats_.radar_models;
                if (f.radar_event_models) {
                    ++stats_.radar_events;
                    continue;
                }
            }
            const float t = e.persistent ? std::fmod(e.age, 1.0f) : std::clamp(e.age / std::max(e.duration, 1e-3f), 0.0f, 1.0f);
            const float r = 3.0f + 9.0f * t;
            const auto a = static_cast<uint8_t>(std::lround(255.0f * (1.0f - 0.8f * t)));
            // Story blips (Lua Add_Radar_Blip, [port] visual): a gold ring around a centre dot.
            const Rgba8 ring = e.marker ? Rgba8{255, 214, 64, a} : Rgba8{255, 255, 255, a};
            for (int d = 0; d < 12; ++d) {
                const float ang = static_cast<float>(d) * (6.28318530717958647692f / 12.0f);
                const Vec2f pnt{c.x + r * std::cos(ang), c.y + r * std::sin(ang)};
                if (radar_view_.inside(pnt)) dot(dl_, blayer, pnt, 1.0f, ring);
            }
            if (e.marker) dot(dl_, blayer, c, 1.5f, Rgba8{255, 214, 64, 255});
            ++stats_.radar_events;
        }
    }
    // Camera footprint (the view's ground quad) as dotted lines.
    if (f.radar_footprint) {
        const std::array<Vec2f, 4> fp = camera_footprint(f.world.view, f.ground_z);
        for (size_t i = 0; i < 4; ++i) {
            const Vec2f a = radar_view_.to_canvas(fp[i].x, fp[i].y), b = radar_view_.to_canvas(fp[(i + 1) % 4].x, fp[(i + 1) % 4].y);
            const float len = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
            const int n = std::min(400, static_cast<int>(len / 3.0f) + 1);
            for (int k = 0; k <= n; ++k) {
                const float t = static_cast<float>(k) / static_cast<float>(n);
                const Vec2f q{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
                if (radar_view_.inside(q)) dot(dl_, blayer, q, 0.75f, Rgba8{255, 255, 255, 200});
            }
        }
    }
}

void TacticalHud::build_panel(const TacticalHudFrame& f, std::span<const HudTypeInfo> types) {
    static const HudTypeInfo kNoType{};
    for (size_t s = 0; s < groups_.size() && s < slot_component_.size(); ++s) {
        const int32_t ci = slot_component_[s];
        if (ci < 0) continue;
        const ComponentPlacement& p = layout_.placement(static_cast<uint32_t>(ci));
        if (!p.placed) continue;
        const CommandBarComponent& c = (*table_)[static_cast<size_t>(ci)];
        const SelectedGroup& g = groups_[s];
        const HudTypeInfo& t = g.type < types.size() ? types[g.type] : kNoType;
        const uint32_t layer = layer_of(c, p);
        // Pad (Blank_Texture_Name per faction), portrait (type Icon_Name), count text, health / shield bars.
        const std::string* pad =
            c.blank_texture.empty() ? nullptr : &c.blank_texture[std::min<size_t>(f.faction_alt, c.blank_texture.size() - 1)];
        Vec2f size = p.size.x > 0 ? p.size : kSlotFallback;
        if (p.size.x <= 0)
            if (const AtlasEntry* e = pad ? atlas_->find(*pad) : nullptr)
                size = {static_cast<float>(e->width) * c.scale, static_cast<float>(e->height) * c.scale};
        const ButtonVisual vis = buttons_.visual(static_cast<uint32_t>(ci));
        if ((vis == ButtonVisual::MouseOver || vis == ButtonVisual::Pressed) && !c.lower_effect_texture.empty())
            add_atlas_quad(dl_, layer, HudList::Additive, *atlas_, c.lower_effect_texture[0], p.center, {}, c.scale, Rgba8{});
        if (!pad || !add_atlas_quad(dl_, layer, HudList::Alpha, *atlas_, *pad, p.center, size, 1.0f, Rgba8{}))
            frame_rect(dl_, layer, p.center, size, kPanelBack, kPanelEdge);
        if (t.icon_name.empty() ||
            !add_atlas_quad(dl_, layer, HudList::Alpha, *atlas_, t.icon_name, p.center, {size.x - 6.0f, size.y - 6.0f}, 1.0f,
                            Rgba8{}))
            add_atlas_quad(dl_, layer, HudList::Alpha, *atlas_, {}, p.center, {}, 1.0f, Rgba8{90, 110, 140, 255}, {size.x - 10.0f, size.y - 10.0f});
        if (vis == ButtonVisual::MouseOver || vis == ButtonVisual::Pressed)
            frame_rect(dl_, layer, p.center, size, Rgba8{255, 255, 255, vis == ButtonVisual::Pressed ? uint8_t{60} : uint8_t{30}},
                       Rgba8{220, 230, 255, 255});
        if (g.ids.size() > 1) {
            HudTextStyle st;
            st.font = fonts_->font(c.font_name.empty() ? std::string(kFallbackFontFace) : c.font_name,
                                   c.font_point_size > 0 ? c.font_point_size : 6.0f, canvas_.screen_height);
            st.color = c.has_text_color ? c.text_color : Rgba8{};
            st.outline = c.text_outline;
            st.align = TextAlign::Right;
            char buf[16];
            std::snprintf(buf, sizeof buf, "%zu", g.ids.size());
            add_text(dl_, layer, HudList::Text, *fonts_, canvas_, ascii32(buf),
                     {p.center.x + 0.5f * size.x - 3.0f, p.center.y - 0.5f * size.y + 7.0f}, st);
        }
        hit(ci, layer, p.center, size, static_cast<uint32_t>(s));
        // Bars under the portrait (their own bones when the shell is loaded).
        const int32_t bars[2] = {health_component_[s], shield_component_[s]};
        const float vals[2] = {g.health, g.shield};
        const Rgba8 fb[2] = {Rgba8{40, 220, 40, 255}, Rgba8{60, 140, 255, 255}};
        for (int k = 0; k < 2; ++k) {
            if (vals[k] < 0.0f) continue;
            Vec2f at{p.center.x, p.center.y - 0.5f * size.y - 3.0f - static_cast<float>(k) * 5.0f};
            Vec2f bar_size{size.x - 6.0f, 3.0f};
            uint32_t bl = layer + 1;
            const CommandBarComponent* bc = bars[k] >= 0 ? &(*table_)[static_cast<size_t>(bars[k])] : nullptr;
            if (bc) {
                const ComponentPlacement& bp = layout_.placement(static_cast<uint32_t>(bars[k]));
                if (bp.placed && bp.from_bone) {
                    at = bp.center;
                    if (bp.size.x > 0) bar_size = {bp.size.x - 2.0f, std::max(2.0f, bp.size.y - 4.0f)};
                }
                bl = std::max(layer_of(*bc, bp), layer + 1);
            }
            const std::string* ov = bc && !bc->bar_overlay.empty() ? &bc->bar_overlay.back() : nullptr;
            if (!ov || !add_atlas_quad_clipped(dl_, bl, HudList::Alpha, *atlas_, *ov, at, {}, bc->scale, vals[k], Rgba8{}))
                add_atlas_quad_clipped(dl_, bl, HudList::Alpha, *atlas_, {}, at, {}, 1.0f, vals[k], fb[k], bar_size);
        }
        ++stats_.panel_slots;
    }
}

void TacticalHud::build_abilities(const TacticalHudFrame& f) {
    ability_slots_.clear();
    for (int32_t ci : special_button_)
        if (ci >= 0) buttons_.state(static_cast<uint32_t>(ci)).hidden = true;
    if (!f.providers.abilities) return;
    // [port] abilities of the selection: per group (panel order), its abilities; distinct ability types across the
    // selection take one button each (users = every selected unit of the groups having it), in first-seen order.
    std::vector<HudAbility> list;
    for (const SelectedGroup& g : groups_) {
        list.clear();
        f.providers.abilities(g, list);
        for (const HudAbility& a : list) {
            auto it = std::find_if(ability_slots_.begin(), ability_slots_.end(),
                                   [&](const AbilitySlot& s) { return s.ability.ability_type == a.ability_type; });
            if (it == ability_slots_.end()) {
                if (ability_slots_.size() >= special_button_.size()) continue;
                AbilitySlot s;
                s.ability = a;
                ability_slots_.push_back(std::move(s));
                it = ability_slots_.end() - 1;
            } else {  // merged: ready when any group is; active when any is
                it->ability.recharge = std::max(it->ability.recharge, a.recharge);
                it->ability.active = it->ability.active || a.active;
                it->ability.enabled = it->ability.enabled || a.enabled;
            }
            it->users.insert(it->users.end(), g.ids.begin(), g.ids.end());
        }
    }
    std::vector<uint8_t> column_used(special_border_.size(), 0);
    for (size_t i = 0; i < ability_slots_.size(); ++i) {
        AbilitySlot& s = ability_slots_[i];
        std::sort(s.users.begin(), s.users.end());
        s.component = special_button_[i];
        if (s.component < 0) continue;
        const auto ci = static_cast<uint32_t>(s.component);
        const ComponentPlacement& p = layout_.placement(ci);
        if (!p.placed) continue;
        ButtonRuntime& r = buttons_.state(ci);
        r.hidden = false;
        r.disabled = !s.ability.enabled;
        r.selected = s.ability.active;
        r.icon_override = s.ability.icon;
        r.build_fill = s.ability.recharge < 1.0f ? std::clamp(s.ability.recharge, 0.0f, 1.0f) * 100.0f : -1.0f;
        r.overlay = s.ability.autofire;
        const CommandBarComponent& c = (*table_)[ci];
        // [port] Can_Animate on special_button_NN (sa_ami_outline frames, Animate_Upper_Effect): the autofire outline
        // animates while autofire is on.
        if (c.can_animate) {
            if (s.ability.autofire && r.anim_start < 0.0) {
                r.anim = "autofire";
                r.anim_start = f.time;
            } else if (!s.ability.autofire && r.anim == "autofire") {
                r.anim.clear();
                r.anim_start = -1.0;
            }
        }
        const Vec2f size = buttons_.draw(dl_, *atlas_, fonts_, canvas_, ci, p.center, p.size, f.faction_alt, f.time,
                                         static_cast<int32_t>(layer_of(c, p)) - c.base_layer);
        // [port] i_special_border_button_dark is the icon's size, so HudButtons::draw takes it for a full-art variant and
        // drops the icon. It is only the button's dark back: the disabled ability keeps its icon, dimmed, on top.
        if (r.disabled && !c.disabled_texture.empty() && !s.ability.icon.empty())
            add_atlas_quad(dl_, layer_of(c, p), HudList::Alpha, *atlas_, s.ability.icon,
                           {p.center.x + c.icon_offset.x, p.center.y - c.icon_offset.y}, {}, c.scale, Rgba8{128, 128, 128, 255});
        hit(s.component, layer_of(c, p), p.size.x > 0 ? p.rect_center : p.center, size);
        if (i / 2 < column_used.size()) column_used[i / 2] = 1;
        ++stats_.ability_buttons;
    }
    // special_border_NN behind the used columns: full / left / centre / right pieces (Icon_Alternate 0..3).
    for (size_t k = 0; k < special_border_.size(); ++k) {
        if (!column_used[k] || special_border_[k] < 0) continue;
        const auto ci = static_cast<uint32_t>(special_border_[k]);
        const ComponentPlacement& p = layout_.placement(ci);
        if (!p.placed) continue;
        const bool left = k == 0 || !column_used[k - 1], right = k + 1 >= column_used.size() || !column_used[k + 1];
        const int32_t piece = left && right ? 0 : left ? 1 : right ? 3 : 2;
        const CommandBarComponent& c = (*table_)[ci];
        ButtonRuntime& r = buttons_.state(ci);
        r.hidden = false;
        r.alt = piece;
        buttons_.draw(dl_, *atlas_, fonts_, canvas_, ci, p.size.x > 0 ? p.rect_center : p.center, p.size, f.faction_alt, f.time,
                      static_cast<int32_t>(layer_of(c, p)) - c.base_layer);
    }
}

void TacticalHud::build_queue(const TacticalHudFrame& f) {
    queue_slots_.clear();
    if (!f.providers.build_queue) return;
    std::vector<HudQueueEntry> entries;
    f.providers.build_queue(entries);
    for (size_t i = 0; i < entries.size() && i < queue_component_.size(); ++i) {
        const int32_t ci = queue_component_[i];
        if (ci < 0) continue;
        const ComponentPlacement& p = layout_.placement(static_cast<uint32_t>(ci));
        if (!p.placed) continue;
        const HudQueueEntry& e = entries[i];
        ButtonRuntime& r = buttons_.state(static_cast<uint32_t>(ci));
        r.hidden = false;
        r.icon_override = e.icon;
        r.build_fill = std::clamp(e.progress, 0.0f, 1.0f) * 100.0f;
        r.text.clear();
        if (e.count > 1) {
            char buf[16];
            std::snprintf(buf, sizeof buf, "%u", e.count);
            r.text = ascii32(buf);
        }
        const CommandBarComponent& c = (*table_)[static_cast<size_t>(ci)];
        const Vec2f size = buttons_.draw(dl_, *atlas_, fonts_, canvas_, static_cast<uint32_t>(ci), p.center, p.size,
                                         f.faction_alt, f.time, static_cast<int32_t>(layer_of(c, p)) - c.base_layer);
        hit(ci, layer_of(c, p), p.size.x > 0 ? p.rect_center : p.center, size);
        queue_slots_.push_back({ci, e});
        ++stats_.queue_entries;
    }
}

// Tactical buy buttons Build_00..05 (i_build_buttons.alo bone grid, model units = canvas units) around the builder's
// projected pixel, or the [port] bottom-centre point without one. Text2 = cost (Text_Offset2, Text_Color2 gold), text =
// queued count (> 1); a blocked option is disabled with Tooltip_Text[0..2] (queue full / pop cap / funds) or no tooltip.
void TacticalHud::build_buy_buttons(const TacticalHudFrame& f) {
    buy_slots_.clear();
    builder_ = f.builder;
    for (int32_t ci : buy_component_)
        if (ci >= 0) buttons_.state(static_cast<uint32_t>(ci)).hidden = true;
    if (f.builder == commands::kNoObject || f.builder == 0 || f.build_options.empty()) return;
    static constexpr Vec2f kBone[6] = {{-54.0f, -67.0f}, {-0.5f, -67.0f}, {52.0f, -67.0f},
                                       {-54.0f, -120.0f}, {-0.5f, -120.0f}, {52.0f, -120.0f}};
    static constexpr Vec2f kHit{52.0f, 52.0f};          // bone meshes -25..27 x -30..22
    static constexpr Vec2f kHitOffset{1.0f, -4.0f};     // their centre relative to the bone
    const Vec2f anchor = f.has_build_anchor ? canvas_.pixel_to_canvas(f.build_anchor_px.x, f.build_anchor_px.y)
                                            : Vec2f{0.0f, -0.5f * canvas_.height + 430.0f};
    for (size_t i = 0; i < f.build_options.size() && i < buy_component_.size(); ++i) {
        const int32_t ci = buy_component_[i];
        if (ci < 0) continue;
        const HudBuildOption& o = f.build_options[i];
        const auto u = static_cast<uint32_t>(ci);
        ButtonRuntime& r = buttons_.state(u);
        r.hidden = false;
        r.disabled = o.block != HudBuildOption::Block::None;
        r.icon_override = o.icon;
        r.text.clear();
        if (o.queued > 1) {
            char buf[16];
            std::snprintf(buf, sizeof buf, "%u", o.queued);
            r.text = ascii32(buf);
        }
        r.text2 = o.cost >= 0 ? ascii32(std::to_string(o.cost)) : std::u32string();
        switch (o.block) {
            case HudBuildOption::Block::QueueFull: r.tooltip_index = 0; break;
            case HudBuildOption::Block::PopCap: r.tooltip_index = 1; break;
            case HudBuildOption::Block::Funds: r.tooltip_index = 2; break;
            default: r.tooltip_index = -1; break;
        }
        const CommandBarComponent& c = (*table_)[u];
        const Vec2f center{anchor.x + kBone[i].x, anchor.y + kBone[i].y};
        const uint32_t layer = static_cast<uint32_t>(std::clamp(std::max(c.base_layer, kShellLayerMin), 0, static_cast<int32_t>(kHudLayers) - 1));
        buttons_.draw(dl_, *atlas_, fonts_, canvas_, u, center, kHit, f.faction_alt, f.time, static_cast<int32_t>(layer) - c.base_layer);
        hit(ci, layer, {center.x + kHitOffset.x, center.y + kHitOffset.y}, kHit);
        buy_slots_.push_back({ci, o});
        ++stats_.build_options;
    }
}

void TacticalHud::build_reinforcements(const TacticalHudFrame& f) {
    reinforce_slots_.clear();
    if (!f.providers.reinforcements) return;
    std::vector<HudReinforcement> entries;
    f.providers.reinforcements(entries);
    for (size_t i = 0; i < entries.size() && i < reinforce_component_.size(); ++i) {
        const int32_t ci = reinforce_component_[i];
        if (ci < 0) continue;
        const ComponentPlacement& p = layout_.placement(static_cast<uint32_t>(ci));
        if (!p.placed) continue;
        const HudReinforcement& e = entries[i];
        ButtonRuntime& r = buttons_.state(static_cast<uint32_t>(ci));
        r.hidden = false;
        r.disabled = !e.available;
        r.icon_override = e.icon;
        r.text.clear();
        if (e.count > 1) {
            char buf[16];
            std::snprintf(buf, sizeof buf, "%u", e.count);
            r.text = ascii32(buf);
        }
        const CommandBarComponent& c = (*table_)[static_cast<size_t>(ci)];
        const Vec2f size = buttons_.draw(dl_, *atlas_, fonts_, canvas_, static_cast<uint32_t>(ci), p.center, p.size,
                                         f.faction_alt, f.time, static_cast<int32_t>(layer_of(c, p)) - c.base_layer);
        hit(ci, layer_of(c, p), p.size.x > 0 ? p.rect_center : p.center, size);
        reinforce_slots_.push_back({ci, e});
        ++stats_.reinforcements;
    }
}

bool order_button_enabled(uint32_t button, const HudOrderButtons& s) {
    if (s.controls_locked) return false;
    switch (button) {
        case 0: return s.can_attack;                 // attack
        case 1: case 5: return s.can_attack || s.can_move;  // attack-move, guard
        case 2: case 3: case 4: return s.can_move;   // move, waypoint, full stop
        default: return true;
    }
}

void TacticalHud::apply_order_buttons(const HudOrderButtons& s) {
    for (uint32_t k = 0; k < 6; ++k) {
        const int32_t ci = order_button_[k];
        if (ci < 0) continue;
        ButtonRuntime& r = buttons_.state(static_cast<uint32_t>(ci));
        r.hidden = r.script_visible == 0;
        r.disabled = r.script_enabled >= 0 ? r.script_enabled == 0 : !order_button_enabled(k, s);
    }
}

void TacticalHud::build_buttons(const TacticalHudFrame& f) {
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
        // The planet name is drawn by the caller (centred on its bone); the button keeps its hit box.
        std::u32string held;
        if (static_cast<int32_t>(i) == planet_text_) held.swap(buttons_.state(i).text);
        Vec2f size = buttons_.draw(dl_, *atlas_, fonts_, canvas_, i, p.center, p.size, f.faction_alt, f.time,
                                   static_cast<int32_t>(layer) - c.base_layer);
        if (static_cast<int32_t>(i) == planet_text_) held.swap(buttons_.state(i).text);
        if (p.size.x > 0) size = p.size;
        hit(static_cast<int32_t>(i), layer, p.size.x > 0 ? p.rect_center : p.center, size);
        ++stats_.buttons;
    }
}

int32_t TacticalHud::font_of(int32_t component, float fallback_points) const {
    const CommandBarComponent* c = component >= 0 ? &(*table_)[static_cast<size_t>(component)] : nullptr;
    return fonts_->font(c && !c->font_name.empty() ? c->font_name : std::string(kFallbackFontFace),
                        c && c->font_point_size > 0 ? c->font_point_size : fallback_points, canvas_.screen_height);
}

namespace {
std::u32string text_or(const TextDatabase* text, const char* key, const char32_t* fallback) {
    if (text)
        if (const std::u16string* v = text->find(key)) return utf16_to_utf32(*v);
    return fallback;
}

// Upper-cases ASCII letters (the original draws the banner / header captions in capitals).
std::u32string upper32(std::u32string s) {
    for (char32_t& c : s)
        if (c >= U'a' && c <= U'z') c = static_cast<char32_t>(c - U'a' + U'A');
    return s;
}
}  // namespace

// [port: Gabriel, original screenshot] pause_shell stand-in: the i_attack_mode.alo shell is drawn as flat frames -
// a yellow bar at the top centre with TEXT_GAME_PAUSED and the red TEXT_BUTTON_RESUME_GAME button under it. Sizes
// and colours measured from the original at a 1365 x 768 canvas.
void TacticalHud::build_pause_banner(const TacticalHudFrame& f) {
    if (!f.paused || !f.pause_banner) return;
    constexpr uint32_t kLayer = 11;
    const float hh = 0.5f * canvas_.height;
    const Vec2f bar_c{0.0f, hh - 17.0f}, bar_sz{424.0f, 34.0f};
    const Vec2f btn_c{6.0f, hh - 55.0f}, btn_sz{168.0f, 26.0f};
    const Rgba8 metal{36, 40, 46, 235}, metal_edge{96, 102, 110, 255};
    frame_rect(dl_, kLayer, {bar_c.x, bar_c.y + 2.0f}, {bar_sz.x + 14.0f, bar_sz.y + 10.0f}, metal, metal_edge);
    frame_rect(dl_, kLayer, bar_c, bar_sz, Rgba8{196, 176, 8, 245}, Rgba8{250, 228, 40, 255});
    const bool over = resume_ >= 0 && buttons_.hover() == resume_;
    frame_rect(dl_, kLayer, btn_c, {btn_sz.x + 10.0f, btn_sz.y + 8.0f}, metal, metal_edge);
    frame_rect(dl_, kLayer, btn_c, btn_sz, over ? Rgba8{170, 24, 24, 245} : Rgba8{120, 10, 12, 245}, Rgba8{230, 60, 50, 255});
    HudTextStyle st;
    st.font = font_of(pause_text_, 9.0f);
    st.align = TextAlign::Center;
    st.color = Rgba8{214, 92, 26, 255};
    add_text(dl_, kLayer, HudList::Text, *fonts_, canvas_, upper32(text_or(text_, "TEXT_GAME_PAUSED", U"Game Paused")), bar_c, st);
    const CommandBarComponent* rc = resume_ >= 0 ? &(*table_)[static_cast<size_t>(resume_)] : nullptr;
    st.font = font_of(resume_, 9.0f);
    st.color = over ? Rgba8{255, 150, 140, 255} : (rc && rc->has_text_color ? rc->text_color : Rgba8{255, 64, 64, 255});
    st.emboss = rc ? rc->text_emboss : true;
    add_text(dl_, kLayer, HudList::Text, *fonts_, canvas_,
             upper32(text_or(text_, "TEXT_BUTTON_RESUME_GAME", U"Resume Game")), btn_c, st);
    if (resume_ >= 0) hit(resume_, kLayer, btn_c, btn_sz);
    stats_.pause_banner = true;
}

// [port: Gabriel, original screenshot] Mission Objectives box: objective_back width (Size x 266), right edge 24 units
// inside the canvas, bottom 30 units above the canvas bottom; header + 1-unit rule + "n. text" lines wrapped to the
// box (objective_text Size y = line spacing, objective_header_text Size y = header spacing).
void TacticalHud::build_objectives(const TacticalHudFrame& f) {
    if (f.objectives.empty()) return;
    const CommandBarComponent* back = obj_back_ >= 0 ? &(*table_)[static_cast<size_t>(obj_back_)] : nullptr;
    const CommandBarComponent* hdr = obj_header_ >= 0 ? &(*table_)[static_cast<size_t>(obj_header_)] : nullptr;
    const CommandBarComponent* txt = obj_text_ >= 0 ? &(*table_)[static_cast<size_t>(obj_text_)] : nullptr;
    const CommandBarComponent* ico = obj_icon_ >= 0 ? &(*table_)[static_cast<size_t>(obj_icon_)] : nullptr;
    const float width = back && back->has_size && back->size.x > 100.0f ? back->size.x : 266.0f;
    const float pad_x = back && back->offset.x > 0 ? back->offset.x : 5.0f, pad_y = 4.0f;
    const float header_h = hdr && hdr->has_size && hdr->size.y > 0 ? hdr->size.y : 18.0f;
    const float line_h = txt && txt->has_size && txt->size.y > 0 ? txt->size.y : 16.0f;
    const int32_t hf = font_of(obj_header_, 8.0f), tf = font_of(obj_text_, 7.0f);
    if (hf < 0 || tf < 0) return;
    struct Line {
        std::u32string text;
        HudObjective::State state;
        bool first;
    };
    std::vector<Line> lines;
    const float wrap_px = (width - 2.0f * pad_x - 12.0f) / canvas_.units_per_pixel_x;
    int n = 0;
    for (const HudObjective& o : f.objectives) {
        std::u32string t = o.text;
        if (t.empty()) t = text_ ? gui_unescape(utf16_to_utf32(text_->text(o.text_key))) : utf8_to_utf32(o.text_key);
        char num[16];
        std::snprintf(num, sizeof num, "%d. ", ++n);
        bool first = true;
        for (std::u32string& w : wrap_text(*fonts_, tf, ascii32(num) + t, wrap_px)) {
            lines.push_back({std::move(w), o.state, first});
            first = false;
        }
        ++stats_.objectives;
    }
    stats_.objective_lines = static_cast<uint32_t>(lines.size());
    const float height = pad_y + header_h + 4.0f + line_h * static_cast<float>(lines.size()) + pad_y;
    const float right = 0.5f * canvas_.width - 24.0f, bottom = -0.5f * canvas_.height + 30.0f;
    const float left = right - width, top = bottom + height;
    const auto layer = [](const CommandBarComponent* c, int32_t def) {
        return static_cast<uint32_t>(std::clamp(c ? c->base_layer : def, 0, static_cast<int32_t>(kHudLayers) - 1));
    };
    const uint32_t bl = layer(back, 3), tl = layer(txt, 4);
    {  // translucent backing [port: the original's dark box]
        HudQuad q;
        q.min = {left, bottom};
        q.max = {right, top};
        q.color = Rgba8{0, 0, 0, 110};
        dl_.add(bl, HudList::Alpha, q);
    }
    HudTextStyle hs;
    hs.font = hf;
    hs.color = hdr && hdr->has_text_color ? hdr->text_color : Rgba8{};
    hs.outline = hdr ? hdr->text_outline : true;
    hs.align = TextAlign::Left;
    std::u32string header = f.objectives_header;
    if (header.empty()) header = text_or(text_, "TEXT_TEST_OBJECTIVE_HEADER", U"Mission Objectives");
    const float hy = top - pad_y - 0.5f * header_h;
    add_text(dl_, tl, HudList::Text, *fonts_, canvas_, upper32(std::move(header)), {left + pad_x, hy}, hs);
    {  // rule under the header
        HudQuad q;
        q.min = {left + pad_x - 2.0f, hy - 0.5f * header_h - 1.0f};
        q.max = {right - 2.0f, hy - 0.5f * header_h};
        q.color = Rgba8{200, 204, 210, 220};
        dl_.add(tl, HudList::Alpha, q);
    }
    float y = hy - 0.5f * header_h - 4.0f - 0.5f * line_h;
    for (const Line& l : lines) {
        const bool done = l.state == HudObjective::State::Complete, failed = l.state == HudObjective::State::Failed;
        HudTextStyle st;
        st.font = tf;
        st.color = txt && txt->has_text_color ? txt->text_color : Rgba8{};
        if (done) st.color = txt && txt->has_text_color2 ? txt->text_color2 : Rgba8{128, 128, 128, 255};
        if (failed) st.color = hdr && hdr->has_text_color2 ? hdr->text_color2 : Rgba8{255, 0, 0, 255};
        st.outline = txt ? txt->text_outline : true;
        st.align = TextAlign::Left;
        if (l.first && ico && (done || failed)) {
            const std::vector<std::string>& v = done ? ico->icon_texture : ico->icon_alternate_texture;
            if (!v.empty())
                add_atlas_quad(dl_, tl, HudList::Alpha, *atlas_, v[0], {left - 8.0f, y}, {12.0f, 12.0f}, 1.0f, Rgba8{},
                               {12.0f, 12.0f});
        }
        add_text(dl_, tl, HudList::Text, *fonts_, canvas_, l.text, {left + pad_x + 6.0f, y}, st);
        y -= line_h;
    }
}

}  // namespace alamo::ui
