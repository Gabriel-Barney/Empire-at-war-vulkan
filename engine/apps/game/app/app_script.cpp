// alamo_game App: the scripted smoke steps (split from game_main.cpp). The step helpers shared with the save / load smoke
// (app_script_saveload.cpp) are declared in app_script.hpp.
#include "app_script.hpp"

#include <cctype>

namespace alamo_game_app {

// ------------------------------------------------------------------------------------------------ script
ui::InputFrame App::idle_frame() const {
    ui::InputFrame f;
    f.time = static_cast<double>(frames_) / 30.0;
    f.dt = 1.0f / 30.0f;
    f.width = targets_.width;
    f.height = targets_.height;
    f.mouse_x = static_cast<int32_t>(targets_.width / 2);
    f.mouse_y = static_cast<int32_t>(targets_.height / 2);
    f.mouse_inside = false;  // no edge scrolling
    return f;
}

namespace script {

// A click as three frames: move, press, release (InputState: a click without a drag).
bool click_at(ClickState& c, ui::InputFrame& f, int32_t x, int32_t y, ui::Button b, bool shift) {
    if (shift) f.keys_down.set(ui::vk::kShift);  // the queue modifier held through the click
    f.mouse_x = x;
    f.mouse_y = y;
    f.mouse_inside = true;
    const uint8_t bit = ui::button_bit(b);
    if (c.phase == 1) {
        f.buttons_down = bit;
        f.buttons_pressed = bit;
    } else if (c.phase == 2) {
        f.buttons_released = bit;
    }
    return ++c.phase > 3;
}

ScriptStep step_wait(std::string name, std::function<bool(App&)> cond, uint32_t timeout) {
    return {std::move(name), [cond = std::move(cond)](App& a, ui::InputFrame&) { return cond(a); }, timeout};
}

ScriptStep step_click_control(std::string dialog, std::string control) {
    auto st = std::make_shared<ClickState>();
    std::string name = "click " + dialog + "/" + control;
    auto px = std::make_shared<std::pair<int32_t, int32_t>>(0, 0);
    return {name, [st, px, dialog, control](App& a, ui::InputFrame& f) {
                if (st->phase > 0) return click_at(*st, f, px->first, px->second, ui::Button::Left);  // the dialog may close on release
                ui::GuiDialog* d = a.ui_.gui.find(dialog);
                if (!d || d != a.ui_.gui.top()) return false;  // wait for the dialog
                const int32_t i = d->find(control);
                if (i < 0) {
                    std::fprintf(stderr, "script: %s has no %s\n", dialog.c_str(), control.c_str());
                    a.script_failed_ = true;
                    return true;
                }
                const ui::GuiRect r = d->control_rect(static_cast<size_t>(i));
                const ui::Vec2f p = a.ui_.gui.canvas().canvas_to_pixel(0.5f * (r.min.x + r.max.x), 0.5f * (r.min.y + r.max.y));
                *px = {static_cast<int32_t>(p.x), static_cast<int32_t>(p.y)};
                return click_at(*st, f, px->first, px->second, ui::Button::Left);
            },
            600};
}

// Clicks the first row of a list (the skirmish staging game type list: row 0 = Land).
ScriptStep step_click_first_row(std::string dialog, std::string control) {
    auto st = std::make_shared<ClickState>();
    auto px = std::make_shared<std::pair<int32_t, int32_t>>(0, 0);
    return {"click first row of " + dialog + "/" + control, [st, px, dialog, control](App& a, ui::InputFrame& f) {
                if (st->phase > 0) return click_at(*st, f, px->first, px->second, ui::Button::Left);
                ui::GuiDialog* d = a.ui_.gui.find(dialog);
                if (!d || d != a.ui_.gui.top()) return false;
                const int32_t i = d->find(control);
                if (i < 0) {
                    std::fprintf(stderr, "script: %s has no %s\n", dialog.c_str(), control.c_str());
                    a.script_failed_ = true;
                    return true;
                }
                const ui::GuiRect r = d->control_rect(static_cast<size_t>(i));
                const ui::Vec2f p = a.ui_.gui.canvas().canvas_to_pixel(0.5f * (r.min.x + r.max.x), r.max.y - 6.0f);
                *px = {static_cast<int32_t>(p.x), static_cast<int32_t>(p.y)};
                return click_at(*st, f, px->first, px->second, ui::Button::Left);
            },
            600};
}

// Selects list row `row` like a player: click the first row (selects + focuses the list), then Down until it is reached.
ScriptStep step_select_row(std::string dialog, std::string control, std::function<size_t(App&)> row) {
    auto st = std::make_shared<ClickState>();
    auto px = std::make_shared<std::pair<int32_t, int32_t>>(0, 0);
    auto key_frame = std::make_shared<bool>(false);
    return {"select row of " + dialog + "/" + control, [st, px, key_frame, dialog, control, row](App& a, ui::InputFrame& f) {
                ui::GuiDialog* d = a.ui_.gui.find(dialog);
                if (!d) return false;
                const ui::GuiControl* c = d->get(control);
                if (!c) return false;
                if (st->phase == 0) {
                    const int32_t i = d->find(control);
                    const ui::GuiRect r = d->control_rect(static_cast<size_t>(i));
                    const float cx = 0.5f * (r.min.x + r.max.x);
                    float cy = r.max.y - 6.0f;
                    // q6 t02: a mod layout can put another control over the list's top row (Thrawn's Revenge: the game type list
                    // covers the staging map list's first row): click lower, on the first point the list itself owns.
                    // Vanilla layouts leave the top row free, so the point is unchanged there.
                    for (int32_t k = 0; k < 64 && cy > r.min.y + 6.0f; ++k) {
                        const int32_t hit = d->control_at({cx, cy});
                        if (hit < 0 || hit == i) break;
                        cy -= 4.0f;
                    }
                    const ui::Vec2f p = a.ui_.gui.canvas().canvas_to_pixel(cx, cy);
                    *px = {static_cast<int32_t>(p.x), static_cast<int32_t>(p.y)};
                }
                if (st->phase <= 3) {
                    click_at(*st, f, px->first, px->second, ui::Button::Left);
                    return false;
                }
                const size_t want = row(a);
                if (c->selected >= 0 && static_cast<size_t>(c->selected) >= want) return true;
                *key_frame = !*key_frame;
                if (*key_frame) {
                    f.keys_down.set(ui::vk::kDown);
                    f.keys_pressed.set(ui::vk::kDown);
                }
                return false;
            },
            2000};
}

ScriptStep step_shot(std::string file) {
    auto st = std::make_shared<int>(0);
    return {"shot " + file, [st, file](App& a, ui::InputFrame&) {
                if (a.opt_.png_dir.empty()) return true;
                if (*st == 0) {  // hold the sim while models stream and pipelines compile (presentation-only pause)
                    a.hold_sim_ = true;
                    a.shot_done_ = false;
                    *st = 1;
                    return false;
                }
                if (*st < 1000 && !(a.present_settled() && *st > 3)) {
                    ++*st;
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    return false;
                }
                if (a.shot_path_.empty() && !a.shot_done_) {
                    if (!a.present_settled()) a.notes_.push_back(file + ": taken before everything settled" + std::string(a.gview_.settled() ? " (models ok" : " (models pending") + ", pipelines " + std::to_string(a.draw_stats_.pending_pipeline) + ", translation " + std::to_string(a.draw_stats_.translation_pending) + ")");
                    a.shot_path_ = a.opt_.png_dir + "/" + file;
                    return false;
                }
                if (!a.shot_done_) return false;
                a.hold_sim_ = false;
                return true;
            },
            1200};
}

ScriptStep step_ticks(std::string name, uint64_t n) {
    auto start = std::make_shared<uint64_t>(~uint64_t{0});
    return {std::move(name), [start, n](App& a, ui::InputFrame&) {
                if (!a.session_) return false;
                if (*start == ~uint64_t{0}) *start = a.session_->ticks();
                return a.session_->ticks() >= *start + n;
            },
            20000};
}

// One key press (a frame with the key down and pressed, then idle frames).
ScriptStep step_key(std::string name, uint16_t key) {
    auto st = std::make_shared<int>(0);
    return {std::move(name), [st, key](App&, ui::InputFrame& f) {
                if (*st == 1) {
                    f.keys_down.set(key);
                    f.keys_pressed.set(key);
                }
                return ++*st > 3;
            },
            100};
}

// Pause (the Pause hotkey, slot 153 -> pause_event in the command stream): once the entry drained no tick runs while
// rendering continues; then resume. Checks the sim frame stands still for 20 frames.
void add_pause_steps(std::vector<ScriptStep>& s) {
    s.push_back(step_key("pause key", ui::vk::kPause));
    s.push_back(step_wait("paused", [](App& a) { return a.session_ && a.session_->paused(); }, 300));
    auto frame0 = std::make_shared<uint64_t>(0);
    auto n = std::make_shared<int>(0);
    s.push_back({"sim stands still while paused", [frame0, n](App& a, ui::InputFrame&) {
                     if (*n == 0) *frame0 = a.session_->host().frame();
                     if (++*n < 20) return false;
                     if (a.session_->host().frame() != *frame0) {
                         std::fprintf(stderr, "script: the sim ticked while paused (%" PRIu64 " -> %" PRIu64 ")\n", *frame0, a.session_->host().frame());
                         a.script_failed_ = true;
                     }
                     return true;
                 },
                 100});
    s.push_back(step_key("resume key", ui::vk::kPause));
    s.push_back(step_wait("resumed", [](App& a) { return a.session_ && !a.session_->paused() && !a.session_->pause_requested(); }, 300));
}

// A hotkey slot pressed with its current binding (key + modifier from the HotkeyTable).
ScriptStep step_hotkey(std::string name, uint16_t slot) {
    auto st = std::make_shared<int>(0);
    return {std::move(name), [st, slot](App& a, ui::InputFrame& f) {
                const ui::HotkeyBinding b = a.ui_.hotkeys.binding(slot);
                if (b.key < 0) {
                    std::fprintf(stderr, "script: hotkey slot %u is unbound\n", slot);
                    a.script_failed_ = true;
                    return true;
                }
                if (*st == 1) {
                    if (b.mod == ui::HotkeyMod::Ctrl) f.keys_down.set(ui::vk::kControl);
                    if (b.mod == ui::HotkeyMod::Shift) f.keys_down.set(ui::vk::kShift);
                    if (b.mod == ui::HotkeyMod::Alt) f.keys_down.set(ui::vk::kAlt);
                    f.keys_down.set(static_cast<uint16_t>(b.key));
                    f.keys_pressed.set(static_cast<uint16_t>(b.key));
                }
                return ++*st > 3;
            },
            100};
}


// Battle: control group 1 = the selection (Ctrl+1), then '1' twice within a second: the recall plus the camera jump to the
// group's centroid (SimHost::control_group through CameraFocus::group).
void add_control_group_steps(std::vector<ScriptStep>& s) {
    s.push_back({"assign control group 1 (Ctrl+1)", [](App&, ui::InputFrame& f) {
                     static int n = 0;
                     if (++n == 2) {
                         f.keys_down.set(ui::vk::kControl);
                         f.keys_down.set(ui::vk::key('1'));
                         f.keys_pressed.set(ui::vk::key('1'));
                     }
                     return n > 4;
                 },
                 100});
    s.push_back(step_wait("group 1 assigned", [](App& a) {
        return a.session_ && !a.session_->host().control_group(static_cast<uint8_t>(a.local_player()), 1).empty();
    }, 300));
    s.push_back({"recall group 1 twice (camera jump)", [](App& a, ui::InputFrame& f) {
                     static int n = 0;
                     ++n;
                     if (n == 1) a.bview_.camera().jump_to({0.0f, 0.0f, 0.0f});
                     if (n == 3 || n == 6) {
                         f.keys_down.set(ui::vk::key('1'));
                         f.keys_pressed.set(ui::vk::key('1'));
                     }
                     if (n < 20) return false;
                     const std::vector<objects::ObjectId>& g = a.session_->host().control_group(static_cast<uint8_t>(a.local_player()), 1);
                     double x = 0, y = 0;
                     uint32_t k = 0;
                     for (const render::SnapshotObject& o : a.bview_.frame().objects)
                         if (std::binary_search(g.begin(), g.end(), o.id)) {
                             x += o.position.x;
                             y += o.position.y;
                             ++k;
                         }
                     const ui::CameraParams cp = a.bview_.camera_params(a.targets_.width, a.targets_.height);
                     const double dx = cp.target.x - (k ? x / k : 0.0), dy = cp.target.y - (k ? y / k : 0.0);
                     std::printf("script: control group 1: %zu units, camera %.0f units from their centroid\n", g.size(), std::sqrt(dx * dx + dy * dy));
                     if (k == 0 || dx * dx + dy * dy > 400.0 * 400.0) {
                         std::fprintf(stderr, "script: the camera did not jump to control group 1\n");
                         a.script_failed_ = true;
                     }
                     return true;
                 },
                 100});
}

// Encyclopedia: the camera looks at a world object (battle: a local unit, galactic: a planet) so it sits under the
// cursor at the screen centre (where the idle script frames keep it, through the following shot too); done once the
// pop-up was drawn for a few frames.
ScriptStep step_encyclopedia(std::string name) {
    auto aimed = std::make_shared<bool>(false);
    auto shown0 = std::make_shared<uint64_t>(~uint64_t{0});
    return {std::move(name), [aimed, shown0](App& a, ui::InputFrame&) {
                // The pop-up's delay is wall time: the sim is held meanwhile (presentation-only) so the ticks of the
                // run stay independent of the machine's speed (the CRC log compares across compilers).
                a.hold_sim_ = true;
                if (*shown0 == ~uint64_t{0}) *shown0 = a.ency_shown_;
                if (!*aimed) {
                    if (a.screen_ == Screen::Battle && a.bview_.active()) {
                        for (const render::SnapshotObject& o : a.bview_.frame().objects)
                            if (o.owner == a.local_player() && a.session_ && a.session_->battle() && a.session_->battle()->movement().has_unit(o.id)) {
                                a.bview_.camera().jump_to({o.position.x, o.position.y, o.position.z});
                                *aimed = true;
                                break;
                            }
                    } else if (a.screen_ == Screen::Galactic) {
                        for (const ui::PickSphere& s : a.gview_.pick_items())
                            if (s.kind == ui::PickKind::Planet) {
                                a.gview_.camera().set_look_at(s.center);
                                *aimed = true;
                                break;
                            }
                    }
                    return false;
                }
                if (a.ency_shown_ <= *shown0 + 2) return false;
                a.hold_sim_ = false;
                return true;
            },
            1200};
}

// Screen smokes (opt-in: ALAMO_SMOKE_SCREENS=1, so the default smoke stays as it is; with ALAMO_PSEUDO_LOC=1 they show
// overflow, QA U2 / U4). None sends a command or moves a tick: the CRC log matches the default run's.
// Unit tooltip: the visible unit with the most tooltip text under the cursor with the sim held, `file` when the pop-up is up;
// the camera goes back where it was.
ScriptStep step_unit_tooltip(std::string file) {
    struct State {
        int phase = 0;
        uint32_t target = 0, n = 0;
        ui::Vec3 look{};
        ScriptStep shot;
    };
    auto st = std::make_shared<State>();
    st->shot = step_shot(std::move(file));
    return {"unit tooltip", [st](App& a, ui::InputFrame& f) {
                if (a.screen_ != Screen::Battle || !a.bview_.active()) return false;
                a.hold_sim_ = true;
                if (st->phase == 0) {  // aim: the cursor stays in the middle of the screen
                    size_t most = 0;
                    ui::Vec3 at{};
                    for (const ui::HudUnit& u : a.hud_units_) {
                        if (u.type >= a.data_.types.size()) continue;
                        const ui::EncyclopediaEntry e = encyclopedia_entry(a.data_, a.ui_.text, a.ui_.hud_types, u.type);
                        size_t len = e.header.size() + e.description.size();
                        for (const ui::EncyclopediaRow& r : e.rows) len += r.left.size() + r.right.size() + r.center.size();
                        if (len > most) {
                            most = len;
                            st->target = u.id;
                            at = u.position;
                        }
                    }
                    if (st->target == 0) {
                        a.notes_.push_back("unit tooltip: no unit on the HUD");
                        a.hold_release_ = true;
                        return true;
                    }
                    st->look = a.bview_.camera().look_at();
                    a.bview_.camera().jump_to(at);
                    st->phase = 1;
                    return false;
                }
                if (st->phase == 1) {
                    if (a.ency_.shown_object() != st->target && ++st->n <= 150) return false;
                    if (a.ency_.shown_object() != st->target) a.notes_.push_back("unit tooltip: another object under the cursor");
                    st->phase = 2;
                    return false;
                }
                if (!st->shot.run(a, f)) return false;
                a.bview_.camera().jump_to(st->look);
                a.hold_sim_ = true;
                a.hold_release_ = true;  // no ticks on this frame: the next step runs when it would have without this one
                return true;
            },
            3000};
}

// Victory screen: over the open battle-end screen, the real type names with the longest labels and large counts (a smoke
// battle loses few units, QA U2 needs "Skipray Blastboat x12"), 14 rows on your side so its scrollbar shows too. Continue closes
// it as usual: the screen's data is display only.
ScriptStep step_victory_screen(std::string file) {
    struct State {
        bool shown = false;
        ScriptStep shot;
    };
    auto st = std::make_shared<State>();
    st->shot = step_shot(std::move(file));
    return {"victory screen", [st](App& a, ui::InputFrame& f) {
                if (st->shown) return st->shot.run(a, f);
                std::vector<std::pair<std::u32string, objects::TypeIndex>> names;
                for (objects::TypeIndex t = 0; t < a.data_.types.size() && t < a.ui_.hud_types.size(); ++t)
                    if (const std::string_view id = a.data_.types.get_string(t, "Text_ID");
                        !id.empty() && a.ui_.text.find(id) && !a.ui_.hud_types[t].icon_name.empty())
                        names.emplace_back(type_label(a.data_, a.ui_.text, t), t);
                std::stable_sort(names.begin(), names.end(), [](const auto& x, const auto& y) { return x.first.size() > y.first.size(); });
                ui::BattleEndInfo info;
                info.victory = true;
                info.planet = a.battle_place_;
                info.battle_seconds = 3599;
                info.bounty = 123456;
                info.your_loss_value = 1234567;
                info.enemy_loss_value = 7654321;
                info.your_efficiency = 100;
                info.enemy_efficiency = 100;
                info.can_save_replay = false;
                for (size_t i = 0; i < names.size() && i < 20; ++i) {
                    ui::BattleEndUnit u;
                    u.name = names[i].first;
                    u.icon = a.ui_.hud_types[names[i].second].icon_name;
                    u.count = i % 2 ? 12 : 1;
                    (i < 14 ? info.your_losses : info.enemy_losses).push_back(std::move(u));
                }
                if (!a.screens_.show_battle_end(info)) a.notes_.push_back("victory screen: dialog missing");
                st->shown = true;
                return false;
            },
            1400};
}
// Ctrl+Y (the SelectAll hotkey slot's default).
ScriptStep step_select_all() {
    auto st = std::make_shared<int>(0);
    return {"select all", [st](App&, ui::InputFrame& f) {
                if (*st == 1) {
                    f.keys_down.set(ui::vk::kControl);
                    f.keys_down.set(ui::vk::key('Y'));
                    f.keys_pressed.set(ui::vk::key('Y'));
                }
                return ++*st > 3;
            },
            100};
}

// Battle: aim the camera at the nearest enemy unit, then right-click it (Attack).
ScriptStep step_attack_enemy(bool shift = false) {
    auto st = std::make_shared<ClickState>();
    auto target = std::make_shared<uint32_t>(0);
    auto px = std::make_shared<std::pair<int32_t, int32_t>>(0, 0);
    auto settle = std::make_shared<int>(0);
    return {shift ? "shift+right-click an enemy" : "right-click an enemy", [st, target, px, settle, shift](App& a, ui::InputFrame& f) {
                if (!a.session_ || !a.bview_.active() || a.screen_ != Screen::Battle) return false;
                const render::InterpolatedFrame& fr = a.bview_.frame();
                const render::SnapshotObject* t = nullptr;
                for (const render::SnapshotObject& o : fr.objects)
                    if (*target == 0 ? (o.owner >= 0 && o.owner != a.local_player() && (o.flags & render::kSnapDying) == 0 &&
                                        a.session_->battle() && a.session_->battle()->combat().has_unit(o.id) &&
                                        a.session_->battle()->combat().is_enemy(a.local_player(), o.owner))
                                     : static_cast<uint32_t>(o.id) == *target) {
                        t = &o;
                        break;
                    }
                if (!t) return false;
                if (*target == 0) {
                    *target = static_cast<uint32_t>(t->id);
                    a.bview_.camera().jump_to({t->position.x, t->position.y, 0.0f});
                    a.hold_sim_ = true;  // presentation-only hold: the target stays where the click aims
                    return false;
                }
                if (st->phase == 0 && ++*settle < 10) return false;  // let the camera settle on the target
                if (st->phase == 0) {  // the pixel is fixed for the whole click (the camera may still be moving)
                    const ui::View v = ui::make_view(a.bview_.camera_params(a.targets_.width, a.targets_.height), a.targets_.width, a.targets_.height);
                    float sx = 0, sy = 0;
                    if (!ui::project(v, {t->position.x, t->position.y, t->position.z}, sx, sy)) return false;
                    *px = {static_cast<int32_t>(sx), static_cast<int32_t>(sy)};
                }
                const bool done = click_at(*st, f, px->first, px->second, ui::Button::Right, shift);
                if (done) a.hold_sim_ = false;
                return done;
            },
            600};
}

// Battle: jump the camera to the centroid of the local player's units (presentation only).
void camera_on_local_units(App& a) {
    double x = 0, y = 0;
    uint32_t n = 0;
    for (const render::SnapshotObject& o : a.bview_.frame().objects)
        if (o.owner == a.local_player() && a.session_ && a.session_->battle() && a.session_->battle()->movement().has_unit(o.id)) {
            x += o.position.x;
            y += o.position.y;
            ++n;
        }
    if (n) a.bview_.camera().jump_to({static_cast<float>(x / n), static_cast<float>(y / n), 0.0f});
}

// Battle: right-click the ground next to the local units (MoveToPosition). The pixel is (w / 2, h / 3) unless the world
// pick (HUD_GAPS §2.2, refixf t08: precise model test, then the override sphere) finds an object there, which would turn
// the click into an Attack or a MoveToObject: then the camera goes back to the local units and the click takes the first
// pixel of a fixed spiral around (w / 2, h / 3), inside the middle of the screen, that picks nothing. Fixed for the whole
// click. refixf t08 check: with the model pick the plain ground click finds that pixel free in every smoke run, but the
// queued (shift) click still needs the fallback in the space runs: the enemy click before it aims the camera at the
// enemy and (w / 2, h / 3) then lies on a real hull (skirmish: an Orbital_Construction_Pod mesh hit).
bool ground_pixel_free(App& a, int32_t x, int32_t y) {
    const sim::TacticalMode* tm = a.session_ ? a.session_->host().modes().top_tactical() : nullptr;
    if (!tm) return true;
    const ui::View v = ui::make_view(a.bview_.camera_params(a.targets_.width, a.targets_.height), a.targets_.width, a.targets_.height);
    tactical::QueryContext qc;
    return !ui::pick_world(tm->trees().all(), v, ui::pixel_ray(v, x, y), tactical::kSkipDead, qc, a.world_pick_tests()).has_value();
}
std::pair<int32_t, int32_t> free_ground_pixel(App& a) {
    const int32_t w = static_cast<int32_t>(a.targets_.width), h = static_cast<int32_t>(a.targets_.height);
    const std::pair<int32_t, int32_t> base{w / 2, h / 3};
    const auto free_at = [&](int32_t x, int32_t y) { return ground_pixel_free(a, x, y); };
    if (free_at(base.first, base.second)) return base;
    static constexpr int32_t kDir[8][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}, {1, -1}, {1, 1}, {-1, 1}, {-1, -1}};
    const int32_t step = std::max(h / 24, 1);
    for (int32_t r = 1; r <= 12; ++r)
        for (const auto& d : kDir) {
            const int32_t x = base.first + d[0] * r * step, y = base.second + d[1] * r * step;
            if (x < w / 5 || x > w - w / 5 || y < h / 8 || y > (h * 3) / 5) continue;  // no edge scroll, no HUD
            if (free_at(x, y)) return {x, y};
        }
    return base;
}
ScriptStep step_move_ground(bool shift = false) {
    auto st = std::make_shared<ClickState>();
    auto px = std::make_shared<std::pair<int32_t, int32_t>>(0, 0);
    auto settle = std::make_shared<int>(0);
    return {shift ? "shift+right-click the ground" : "right-click the ground", [st, px, settle, shift](App& a, ui::InputFrame& f) {
                if (!a.bview_.active() || a.screen_ != Screen::Battle) return false;
                if (st->phase == 0) {
                    const int32_t bx = static_cast<int32_t>(a.targets_.width / 2), by = static_cast<int32_t>(a.targets_.height / 3);
                    if (*settle == 0 && !ground_pixel_free(a, bx, by)) {
                        camera_on_local_units(a);
                        *settle = 1;
                        return false;
                    }
                    if (*settle > 0 && ++*settle < 10) return false;  // let the camera settle
                    *px = free_ground_pixel(a);
                }
                return click_at(*st, f, px->first, px->second, ui::Button::Right, shift);
            },
            100};
}

// Drag-select box (Playtest 3, opt-in: ALAMO_SMOKE_DRAG_BOX=1, so the default smoke command stream is unchanged): press the
// left button at 30 % / 25 % of the screen, drag to 70 % / 60 % over 10 frames, take `file` with the button still held (the
// box drawn mid-drag), then release (the box selection).
ScriptStep step_drag_box(std::string file) {
    struct State {
        int phase = 0;
        bool drawn = false;
        ScriptStep shot;
    };
    auto st = std::make_shared<State>();
    st->shot = step_shot(std::move(file));
    return {"drag-select box", [st](App& a, ui::InputFrame& f) {
                if (!a.bview_.active() || a.screen_ != Screen::Battle) return false;
                const auto w = static_cast<float>(a.targets_.width), h = static_cast<float>(a.targets_.height);
                const int kMove = 10;
                const float t = static_cast<float>(std::clamp(st->phase, 0, kMove)) / static_cast<float>(kMove);
                f.mouse_x = static_cast<int32_t>(w * (0.30f + 0.40f * t));
                f.mouse_y = static_cast<int32_t>(h * (0.25f + 0.35f * t));
                f.mouse_inside = true;
                const uint8_t bit = ui::button_bit(ui::Button::Left);
                if (st->phase < 0) {  // released last frame: done
                    std::printf("script: drag box drawn while held %d\n", st->drawn ? 1 : 0);
                    if (!st->drawn) a.notes_.push_back("drag-select box not drawn while dragging");
                    return true;
                }
                f.buttons_down = bit;
                if (st->phase == 0) f.buttons_pressed = bit;
                if (st->phase <= kMove) {
                    ++st->phase;
                    return false;
                }
                st->drawn = st->drawn || a.ui_.tactical_hud.stats().drag_box;
                if (!st->shot.run(a, f)) return false;
                f.buttons_down = 0;
                f.buttons_released = bit;
                st->phase = -1;
                return false;
            },
            1400};
}

// R2 t03 (ALAMO_SMOKE_ZOOM_LIGHT=1, campaign): zoom onto the first planet and shoot it with the camera settled (the galactic
// light record at its zoomed-in angles / intensity: GalacticView::use_scene blends with camera().zoom_blend()), then zoom
// back out. Presentation only (camera + selection), no command.
ScriptStep step_zoom_light(std::string file) {
    struct State {
        int phase = 0, wait = 0;
        ScriptStep shot;
    };
    auto st = std::make_shared<State>();
    st->shot = step_shot(std::move(file));
    return {"zoom light", [st](App& a, ui::InputFrame& f) {
                if (a.screen_ != Screen::Galactic) return false;
                if (st->phase == 0) {
                    for (const ui::PickSphere& p : a.gview_.pick_items()) {
                        bool seen = false;  // a planet the local player sees (a fogged one is a flat wireframe globe)
                        for (const render::GalacticPlanetSnap& ps : a.gview_.frame().planets)
                            seen = seen || (static_cast<uint32_t>(ps.id) == p.id && !(ps.flags & render::kGalFogged));
                        if (p.kind == ui::PickKind::Planet && seen) {
                            a.selected_planet_ = p.id;
                            a.gview_.camera().set_look_at({p.center.x, p.center.y, 0.0f});
                            a.gview_.camera().zoom_to_planet(p.center, p.radius);
                            st->phase = 1;
                            break;
                        }
                    }
                    return false;
                }
                if (st->phase == 1) {  // settled zoomed in (zoom state 3)
                    if (a.gview_.camera().zoom_state() == 3 && ++st->wait > 5) st->phase = 2;
                    return false;
                }
                if (st->phase == 2) {
                    if (!st->shot.run(a, f)) return false;
                    std::printf("script: zoomed light blend %.2f (planet %u)\n", static_cast<double>(a.gview_.camera().zoom_blend()), a.selected_planet_);
                    a.gview_.camera().zoom_out();
                    st->phase = 3;
                    return false;
                }
                if (a.gview_.camera().zoom_state() != 0) return false;
                a.selected_planet_ = 0;
                return true;
            }, 3000};
}

// Playtest 3 fleet slots (ALAMO_SMOKE_FLEET_SLOTS=1, campaign): select an own orbiting fleet (shot: every planet's free
// slots as ovals), then drag its map button over a neighbour's slot (shot: the dragged stack, the hovered oval) and
// release it back over its own planet (no order).
ScriptStep step_fleet_slots(std::string sel_file, std::string drag_file) {
    struct State {
        int phase = 0;
        uint32_t fleet = 0;
        PlanetIndex at = kInvalidPlanet, to = kInvalidPlanet;
        ui::Vec2f from{}, dest{};
        ScriptStep shot_sel, shot_drag;
    };
    auto st = std::make_shared<State>();
    st->shot_sel = step_shot(std::move(sel_file));
    st->shot_drag = step_shot(std::move(drag_file));
    return {"fleet slots", [st](App& a, ui::InputFrame& f) {
                if (st->phase < 0) return true;  // released last frame
                const galactic::GalacticSim* g = a.session_ ? a.session_->galaxy() : nullptr;
                if (!g || a.screen_ != Screen::Galactic) return false;
                const objects::ObjectStore& store = g->store();
                if (st->phase == 0) {
                    for (const galactic::FleetState& fl : g->fleets()) {
                        if (!fl.alive || fl.owner != a.local_player() || fl.moving || fl.planet == kInvalidPlanet || fl.contents.empty()) continue;
                        for (uint32_t e = g->graph().edge_begin(fl.planet); e < g->graph().edge_end(fl.planet) && st->to == kInvalidPlanet; ++e)
                            if (g->planets()[g->graph().neighbour(e)].is_planet) st->to = g->graph().neighbour(e);
                        if (st->to == kInvalidPlanet) continue;
                        st->fleet = static_cast<uint32_t>(store.id(fl.object));
                        st->at = fl.planet;
                        break;
                    }
                    if (st->fleet == 0) return false;
                    const Vec3 c0 = g->planets()[st->at].position, c1 = g->planets()[st->to].position;
                    a.gview_.camera().set_look_at({0.5f * (c0.x + c1.x), 0.5f * (c0.y + c1.y), 0.0f});
                    ui::HudAction act;
                    act.kind = ui::HudAction::Kind::GalacticSelect;
                    act.ids = {st->fleet};
                    a.handle_hud_actions({act});
                    st->phase = 1;
                    return false;
                }
                if (st->phase < 4) {  // camera and HUD catch up
                    ++st->phase;
                    return false;
                }
                if (st->phase == 4) {
                    if (!st->shot_sel.run(a, f)) return false;
                    std::printf("script: fleet %u selected, slots shown %d, empty ovals %u\n", st->fleet,
                                a.ui_.galactic_hud.stats().fleet_slots_shown ? 1 : 0, a.ui_.galactic_hud.stats().empty_slots);
                    a.orders_.set_selection({});
                    st->phase = 5;
                    return false;
                }
                const ui::HudCanvas& cv = a.ui_.galactic_hud.canvas();
                if (st->phase == 5) {  // the fleet's map button: the g_planet_fleet hit nearest above its planet
                    const int32_t gpf = a.ui_.components.find("g_planet_fleet");
                    const ui::View v = ui::make_view(a.gview_.camera_params(a.targets_.width, a.targets_.height), a.targets_.width, a.targets_.height);
                    float px = 0, py = 0, qx = 0, qy = 0;
                    const Vec3 p0 = g->planets()[st->at].position, p1 = g->planets()[st->to].position;
                    if (!ui::project(v, {p0.x, p0.y, p0.z}, px, py) || !ui::project(v, {p1.x, p1.y, p1.z}, qx, qy)) return false;
                    float best = 1e30f;
                    for (const ui::HudHitBox& h : a.ui_.galactic_hud.hit_boxes()) {
                        if (h.component != gpf) continue;
                        const ui::Vec2f c = cv.canvas_to_pixel(0.5f * (h.min.x + h.max.x), 0.5f * (h.min.y + h.max.y));
                        const float d = std::hypot(c.x - px, c.y - py);
                        if (d < best) {
                            best = d;
                            st->from = c;
                        }
                    }
                    if (best > 1e29f) return false;
                    const ui::Vec2f at = cv.pixel_to_canvas(qx, qy);  // the neighbour's middle slot (0, Offset.y 47)
                    st->dest = cv.canvas_to_pixel(at.x, at.y + 47.0f);
                    st->phase = 6;
                }
                const uint8_t bit = ui::button_bit(ui::Button::Left);
                const int kMove = 12;
                f.mouse_inside = true;
                const float t = static_cast<float>(std::clamp(st->phase - 6, 0, kMove)) / static_cast<float>(kMove);
                f.mouse_x = static_cast<int32_t>(st->from.x + (st->dest.x - st->from.x) * t);
                f.mouse_y = static_cast<int32_t>(st->from.y + (st->dest.y - st->from.y) * t);
                f.buttons_down = bit;
                if (st->phase == 6) f.buttons_pressed = bit;
                if (st->phase < 6 + kMove + 2) {
                    ++st->phase;
                    return false;
                }
                if (!st->shot_drag.run(a, f)) return false;
                std::printf("script: fleet drag active %d, over slot %d of planet %u, valid %d, empty ovals %u, highlights %u\n",
                            a.ui_.galactic_hud.drag().active ? 1 : 0, a.ui_.galactic_hud.drag().over_slot,
                            a.ui_.galactic_hud.drag().over_slot_planet, a.ui_.galactic_hud.drag().valid ? 1 : 0,
                            a.ui_.galactic_hud.stats().empty_slots, a.ui_.galactic_hud.stats().slot_highlights);
                f.mouse_x = static_cast<int32_t>(st->from.x);  // back over its own planet: no order
                f.mouse_y = static_cast<int32_t>(st->from.y);
                f.buttons_down = 0;
                f.buttons_released = bit;
                st->phase = -1;
                return false;
            },
            3000};
}

// Battle combat (land and space skirmish): scripted attack orders through the command stream (recorded like any order) -
// every 150 frames the local combat units attack the enemy unit nearest to their centroid; the camera follows them. Done
// 60 frames after the first death (shots fired); the corpse / decal / animation counts are printed. A battle that ends
// first times the step out (the smoke fails): the space skirmish smoke had no combat for a day without failing.
ScriptStep step_combat(bool land) {
    struct State {
        uint32_t n = 0, first_death = 0;
    };
    auto st = std::make_shared<State>();
    return {land ? "land forces engage" : "space forces engage", [st, land](App& a, ui::InputFrame&) {
                const sim::TacticalBattle* b = a.session_ ? a.session_->battle() : nullptr;
                if (!b || !a.bview_.active() || a.screen_ != Screen::Battle) return false;
                // Sim reads only (the battle's movable units, ascending id, store positions): the orders do not depend
                // on the render interpolation.
                const int32_t me = a.local_player();
                const objects::ObjectStore& store = a.session_->host().modes().top_tactical()->primary();
                std::vector<uint32_t> mine;
                double cx = 0, cy = 0;
                for (const objects::ObjectId id : b->movement().unit_ids()) {
                    const objects::ObjectHandle h = store.find(id);
                    if (!store.alive(h) || store.owner(h) != me || !b->combat().has_unit(id)) continue;
                    mine.push_back(static_cast<uint32_t>(id));
                    cx += store.position(h).x;
                    cy += store.position(h).y;
                }
                uint32_t target = 0;  // the enemy unit nearest to the local units' centroid
                double tx = 0, ty = 0;
                if (!mine.empty()) {
                    cx /= static_cast<double>(mine.size());
                    cy /= static_cast<double>(mine.size());
                    double best = 0;
                    for (const objects::ObjectId id : b->movement().unit_ids()) {
                        const objects::ObjectHandle h = store.find(id);
                        if (!store.alive(h) || store.owner(h) < 0 || !b->combat().has_unit(id) || !b->combat().is_enemy(me, store.owner(h))) continue;
                        const double dx = store.position(h).x - cx, dy = store.position(h).y - cy, d = dx * dx + dy * dy;
                        if (target == 0 || d < best) {
                            target = static_cast<uint32_t>(id);
                            best = d;
                            tx = store.position(h).x;
                            ty = store.position(h).y;
                        }
                    }
                    // The camera between the local units and that enemy, so the shot shows the engagement (presentation only).
                    if (target != 0) a.bview_.camera().jump_to({static_cast<float>((cx + tx) / 2), static_cast<float>((cy + ty) / 2), 0.0f});
                    else a.bview_.camera().jump_to({static_cast<float>(cx), static_cast<float>(cy), 0.0f});
                }
                if (st->n++ % 150 == 0 && target != 0) a.submit(commands::ev::attack(target, mine));
                const sim::BattleStats& bs = b->stats();
                if (bs.deaths > 0 && st->first_death == 0) st->first_death = st->n;
                // Sim-only end condition (frame counts, no render state): the run's ticks stay machine independent.
                if (bs.shots == 0 || bs.deaths == 0 || st->n < st->first_death + 60) return false;
                if (land && a.bview_.stats().corpses == 0) a.notes_.push_back("land combat: no corpse drawn yet");
#ifdef ALAMO_SIM_HAVE_ANIM
                // Animation feed: land infantry / vehicles walk, idle and attack (anim plays); every battle's shots flash
                // muzzles. FoC's space capital ships ship no idle / move / attack clips (only _D_DIE_00), so space
                // skirmishes legitimately log 0 anim plays.
                if (const sim::SimAnimation* an = a.session_->animation()) {
                    if (land && bs.anim_plays == 0) a.notes_.push_back("land combat: no animation played");
                    if (an->stats().muzzle_flashes == 0) a.notes_.push_back("combat: no muzzle flash");
                }
#endif
                // q6 t02: a mod may draw its bolts as projectile models (Thrawn's Revenge: Proj_Space_Laser_*.alo) instead of
                // procedural lasers; only a battle with neither is noted. Vanilla lines unchanged.
                if (!land && a.bview_.stats().lasers_peak == 0)
                    a.notes_.push_back(a.bview_.stats().model_projectiles_peak > 0
                                           ? "space combat: no procedural laser bolt drawn (" + std::to_string(a.bview_.stats().model_projectiles_peak) +
                                                 " model projectiles peak)"
                                           : std::string("space combat: no laser bolt drawn"));
                std::printf("script: %s combat - %" PRIu64 " shots, %" PRIu64 " hits, %u deaths, %u corpse draws, %" PRIu64 " scorch decals, %u anim plays, %u peak laser bolts, %u impact / death effects\n",
                            land ? "land" : "space", bs.shots, bs.hits, bs.deaths, a.bview_.stats().corpses, a.bview_.stats().decals, bs.anim_plays,
                            a.bview_.stats().lasers_peak, bs.particles);
                return true;
            },
            6000};
}

// Battle (P4 host): a reinforcement from the pad - the pad's BeginReinforcement action for the first reserve, then a left
// click in the world at the screen centre (the camera is on the local units) = the drop point -> Reinforce in the command
// stream. A drop the cap refuses (QueueFull) is repeated every 90 frames until one deployed or 900 frames passed (the
// battle may end first). Without reserves: a note after 60 frames.
constexpr int kSmokeSkirmishBuys = 14;  // units bought in the space skirmish smoke (more than the space unit cap holds)
constexpr const char* kSmokeSkirmishBuyType = "Corellian_Corvette";
ScriptStep step_reinforce() {
    struct State {
        int phase = 0;
        uint32_t n = 0, next = 0;
        uint64_t rejected0 = 0;
        ClickState click;
    };
    auto st = std::make_shared<State>();
    return {"reinforcement drop", [st](App& a, ui::InputFrame& f) {
                const sim::TacticalBattle* b = a.session_ ? a.session_->battle() : nullptr;
                if (!b || a.screen_ != Screen::Battle || !a.bview_.active()) {
                    if (st->n > 0) a.notes_.push_back("reinforcement: the battle ended before a drop deployed");
                    return st->n > 0;
                }
                if (st->n++ == 0) st->rejected0 = a.session_->order_stats().rejected;
                const uint64_t refused = a.session_->order_stats().rejected - st->rejected0;
                if (a.session_->order_stats().reinforcements > 0 || (a.reinforce_drops_ > 0 && refused > 0 && st->n >= st->next)) {
                    // Deployed, or answered QueueFull (the cap is full until a unit dies; deployment after a loss is covered
                    // by alamo_game_session_tests).
                    std::printf("script: reinforcement drop answered: %s after %" PRIu64 " drop(s), %" PRIu64 " refused\n",
                                a.session_->order_stats().reinforcements > 0 ? "deployed" : "refused (cap full)", a.reinforce_drops_, refused);
                    return true;
                }
                if (st->n > 900) {
                    a.notes_.push_back("reinforcement: no drop deployed within 900 frames");
                    return true;
                }
                if (st->phase == 0) {
                    const auto pool = b->reserves(a.local_player());
                    if (pool.empty()) {
                        if (st->n > 60 && a.reinforce_drops_ == 0) {
                            a.notes_.push_back("reinforcement: no reserves in this battle");
                            return true;
                        }
                        return false;
                    }
                    if (st->n < st->next) return false;
                    ui::HudAction act;
                    act.kind = ui::HudAction::Kind::BeginReinforcement;
                    act.unit = static_cast<uint32_t>(pool[0].galactic_id);
                    a.handle_hud_actions({act});
                    st->phase = 1;
                    return false;
                }
                if (click_at(st->click, f, static_cast<int32_t>(a.targets_.width / 2), static_cast<int32_t>(a.targets_.height / 2), ui::Button::Left)) {
                    st->click = {};
                    st->phase = 0;
                    st->next = st->n + 90;
                }
                return false;
            },
            2400};
}

// Battle (QA D11, opt-in: ALAMO_SMOKE_REINFORCE_HOVER=1, so the default smoke command stream is unchanged): the cursor over
// the first reinforcement button r_0000 with the sim held; the shot shows its hover frame over the unit's icon.
ScriptStep step_reinforce_hover(std::string file) {
    auto n = std::make_shared<uint32_t>(0);
    return {"reinforcement button hover", [n, file](App& a, ui::InputFrame& f) {
                if (!a.session_ || a.screen_ != Screen::Battle || !a.bview_.active()) return false;
                const int32_t ci = a.ui_.components.find("r_0000");
                if (ci < 0 || a.ui_.tactical_hud.stats().reinforcements == 0) {
                    if (++*n > 600) {
                        a.notes_.push_back("reinforcement hover: no reinforcement button shown");
                        return true;
                    }
                    return false;
                }
                const ui::ComponentPlacement& p = a.ui_.tactical_hud.layout().placement(static_cast<uint32_t>(ci));
                const auto px = a.ui_.tactical_hud.canvas().canvas_to_pixel(p.center.x, p.center.y);
                f.mouse_x = static_cast<int32_t>(px.x);
                f.mouse_y = static_cast<int32_t>(px.y);
                f.mouse_inside = true;
                a.hold_sim_ = true;
                if (*n < 1000) {  // a few held frames: the button takes the hover before the shot
                    *n = 1000;
                    a.shot_done_ = false;
                    return false;
                }
                if (++*n == 1005 && !a.opt_.png_dir.empty()) a.shot_path_ = a.opt_.png_dir + "/" + file;
                if (*n < 1005 || (!a.opt_.png_dir.empty() && !a.shot_done_)) return false;
                a.hold_sim_ = false;
                return true;
            },
            1200};
}

// Battle: the targeted-ability pick (TargetingController). Begins it as an ability button's HudAction does, then right-clicks
// the middle of the screen: the pick takes the press (the router gives it the click, so the world issues no move / stop
// order) and the right click cancels it. Sends no command, so the smoke CRCs stay put.
ScriptStep step_ability_pick() {
    struct State {
        int phase = 0;
        ClickState click;
        uint64_t routed0 = 0;
    };
    auto st = std::make_shared<State>();
    return {"targeted ability pick", [st](App& a, ui::InputFrame& f) {
                if (!a.session_ || a.screen_ != Screen::Battle || !a.bview_.active()) return false;
                if (st->phase == 0) {
                    a.hold_sim_ = true;  // presentation-only hold: no tick runs during the step (the later steps keep their ticks)
                    ui::HudAction act;
                    act.kind = ui::HudAction::Kind::BeginAbilityTargeting;
                    act.ability_type = 30;  // TRACTOR_BEAM: an object pick
                    act.ids = a.orders_.selection();
                    st->routed0 = a.session_->order_stats().routed;
                    a.handle_hud_actions({act});
                    if (!a.targeting_.active() || a.targeting_.target_kind() != ui::TargetKind::Object) {
                        a.notes_.push_back("ability pick: the HUD action did not start the pick");
                        a.smoke_ok_ = false;
                        a.hold_release_ = true;
                        return true;
                    }
                    st->phase = 1;
                    return false;
                }
                if (!click_at(st->click, f, static_cast<int32_t>(a.targets_.width / 2), static_cast<int32_t>(a.targets_.height / 2), ui::Button::Right))
                    return false;
                a.hold_release_ = true;  // the next step's first frame runs the ticks the held frames did not
                const bool ok = !a.targeting_.active() && a.targeting_.cancelled() >= 1 && a.session_->order_stats().routed == st->routed0;
                std::printf("script: ability pick - %s (cancelled %" PRIu64 ", orders routed +%" PRIu64 ")\n", ok ? "right click cancelled it, no order" : "FAILED",
                            a.targeting_.cancelled(), a.session_->order_stats().routed - st->routed0);
                if (!ok) {
                    a.notes_.push_back("ability pick: the right click did not cancel the pick cleanly");
                    a.smoke_ok_ = false;
                }
                return true;
            },
            600};
}

// Galactic: pick (human combat fleet, hostile planet with defenders in orbit or a starbase) by BFS hops, select the fleet,
// then right-click the planet.
ScriptStep step_galactic_attack() {
    struct State {
        int phase = 0;
        uint32_t fleet = 0, planet = 0;
        PlanetIndex fleet_at = kInvalidPlanet, target = kInvalidPlanet;
        ClickState click;
    };
    auto st = std::make_shared<State>();
    return {"galactic attack order", [st](App& a, ui::InputFrame& f) {
                const galactic::GalacticSim* g = a.session_ ? a.session_->galaxy() : nullptr;
                if (!g || a.screen_ != Screen::Galactic) return false;
                const objects::ObjectStore& store = g->store();
                const int32_t me = a.local_player();
                if (st->phase == 0) {
                    uint32_t best_hops = 0xFFFFFFFFu;
                    for (const galactic::FleetState& fl : g->fleets()) {
                        if (!fl.alive || fl.owner != me || fl.moving || fl.planet == kInvalidPlanet) continue;
                        bool combat = false;
                        for (const objects::ObjectHandle m : fl.contents)
                            combat = combat || (store.alive(m) && g->rules().types[store.type(m)].ai_combat_power > 0.0f &&
                                                g->rules().types[store.type(m)].hyperspace);
                        if (!combat) continue;
                        std::vector<uint32_t> hops(g->planets().size(), 0xFFFFFFFFu);
                        std::deque<PlanetIndex> q{fl.planet};
                        hops[fl.planet] = 0;
                        while (!q.empty()) {
                            const PlanetIndex at = q.front();
                            q.pop_front();
                            const galactic::PlanetState& p = g->planets()[at];
                            const bool defended = p.starbase_level > 0 || !g->fleets_at(at, p.owner).empty();
                            if (at != fl.planet && p.is_planet && p.owner >= 0 && g->is_hostile(p.owner, me) && defended && hops[at] < best_hops) {
                                best_hops = hops[at];
                                st->fleet = static_cast<uint32_t>(store.id(fl.object));
                                st->fleet_at = fl.planet;
                                st->target = at;
                                st->planet = static_cast<uint32_t>(store.id(p.object));
                            }
                            for (uint32_t e = g->graph().edge_begin(at); e < g->graph().edge_end(at); ++e) {
                                const PlanetIndex nb = g->graph().neighbour(e);
                                if (nb < hops.size() && hops[nb] == 0xFFFFFFFFu) {
                                    hops[nb] = hops[at] + 1;
                                    q.push_back(nb);
                                }
                            }
                        }
                    }
                    if (st->target == kInvalidPlanet) return false;  // nothing to attack yet: wait (the AI moves too)
                    std::printf("script: fleet %u at %s attacks %s (%u hops)\n", st->fleet, g->map().planets.name[st->fleet_at].c_str(),
                                g->map().planets.name[st->target].c_str(), best_hops);
                    const Vec3 c = g->planets()[st->fleet_at].position;
                    a.gview_.camera().set_look_at({c.x, c.y, 0.0f});
                    st->phase = 1;
                    return false;
                }
                const ui::View v = ui::make_view(a.gview_.camera_params(a.targets_.width, a.targets_.height), a.targets_.width, a.targets_.height);
                if (st->phase == 1 || st->phase == 3) {  // let the camera / frame catch up one frame
                    ++st->phase;
                    if (st->phase == 4) {
                        const Vec3 c = g->planets()[st->target].position;
                        a.gview_.camera().set_look_at({c.x, c.y, 0.0f});
                    }
                    return false;
                }
                if (st->phase == 2) {  // select the fleet: an orbiting fleet is picked through its HUD planet fleet button
                    ui::HudAction act;  // (GalacticSelect {fleet}); the map picks transit fleets only
                    act.kind = ui::HudAction::Kind::GalacticSelect;
                    act.ids = {st->fleet};
                    a.handle_hud_actions({act});
                    st->phase = 3;
                    return false;
                }
                if (st->phase == 4 || st->phase == 5) {  // two frames for the camera move, then right-click the planet
                    ++st->phase;
                    return false;
                }
                const Vec3 p = g->planets()[st->target].position;
                float sx = 0, sy = 0;
                if (!ui::project(v, {p.x, p.y, p.z}, sx, sy)) return false;
                return click_at(st->click, f, static_cast<int32_t>(sx), static_cast<int32_t>(sy), ui::Button::Right);
            },
            30000};
}

// q6 t03: `inner`, except that a battle prompt for a conflict the local player did not start (an AI attack; a pending
// battle holds the galaxy, so a fleet in flight never arrives) is answered Auto-Resolve first and its result screen
// closed with Continue. Thrawn's Revenge's Thrawn_Empire AI attacks within the first galactic minutes; in the vanilla
// smoke no such prompt is open during these steps, so the step runs exactly as `inner` there.
ScriptStep answer_ai_attacks(ScriptStep inner) {
    struct State {
        int phase = 0, wait = 0;
        ScriptStep cont;
    };
    auto st = std::make_shared<State>();
    return {inner.name, [st, run = std::move(inner.run)](App& a, ui::InputFrame& f) {
                if (st->phase == 0) {
                    const galactic::ConflictInfo* c = a.choice_open_ && a.session_ ? a.session_->conflict_head() : nullptr;
                    if (!c || c->attacker == a.local_player()) return run(a, f);
                    std::printf("script: battle prompt for an attack by player %d: auto-resolved\n", c->attacker);
                    ui::HudAction act;
                    act.kind = ui::HudAction::Kind::BattleAutoResolve;
                    a.handle_hud_actions({act});
                    st->phase = 1;
                    st->wait = 0;
                    st->cont = step_click_control(ui::dlg::kBattleEnd, "IDC_CONTINUE_BUTTON");
                    return false;
                }
                if (st->phase == 1) {
                    if (a.battle_end_open_ && a.ui_.gui.find(ui::dlg::kBattleEnd) && a.screen_text_.empty()) st->phase = 2;
                    else if (++st->wait > 600) st->phase = 0;  // no result screen came: back to the step
                    return false;
                }
                if (st->cont.run(a, f)) st->phase = 0;
                return false;
            },
            inner.timeout_frames + 2400};
}

// Galactic: a plain fleet hop by input, all the way to arrival (playtest "units can't move planet to planet"). An own
// idle fleet and an adjacent planet that is not hostile (no battle); `drag`: left-drag the fleet onto the planet, else
// left-click the fleet and right-click the planet. Fails the run when the order is refused or the fleet never arrives.
ScriptStep step_galactic_move(bool drag) {
    struct State {
        int phase = 0;
        uint32_t fleet = 0, planet = 0;
        PlanetIndex from = kInvalidPlanet, to = kInvalidPlanet;
        objects::ObjectHandle member;  // the fleet may merge into another one on arrival: followed by its first member
        uint64_t refused0 = 0;
        int drag_frame = 0;
        ClickState click;
    };
    auto st = std::make_shared<State>();
    return {drag ? "galactic move by drag" : "galactic move by click", [st, drag](App& a, ui::InputFrame& f) {
                const galactic::GalacticSim* g = a.session_ ? a.session_->galaxy() : nullptr;
                if (!g || a.screen_ != Screen::Galactic) return false;
                const objects::ObjectStore& store = g->store();
                const int32_t me = a.local_player();
                if (st->phase == 0) {
                    for (const galactic::FleetState& fl : g->fleets()) {
                        if (!fl.alive || fl.owner != me || fl.moving || fl.planet == kInvalidPlanet || fl.contents.empty()) continue;
                        bool hyper = true;
                        for (const objects::ObjectHandle m : fl.contents) hyper = hyper && store.alive(m) && g->rules().types[store.type(m)].hyperspace;
                        if (!hyper) continue;
                        for (uint32_t e = g->graph().edge_begin(fl.planet); e < g->graph().edge_end(fl.planet); ++e) {
                            const PlanetIndex nb = g->graph().neighbour(e);
                            const galactic::PlanetState& p = g->planets()[nb];
                            if (!p.is_planet || (p.owner >= 0 && g->is_hostile(p.owner, me))) continue;
                            if (!g->planet_revealed_raw(nb)) continue;  // GRC 1.1: the hop gate (revealed after a pre-step)
                            bool enemies = false;
                            for (const galactic::FleetState& o : g->fleets())
                                enemies = enemies || (o.alive && o.owner >= 0 && o.owner != me && g->is_hostile(o.owner, me) && (o.planet == nb || o.to == nb));
                            if (enemies) continue;
                            st->fleet = static_cast<uint32_t>(store.id(fl.object));
                            st->member = fl.contents.front();
                            st->from = fl.planet;
                            st->to = nb;
                            st->planet = static_cast<uint32_t>(store.id(p.object));
                            break;
                        }
                        if (st->to != kInvalidPlanet) break;
                    }
                    if (st->to == kInvalidPlanet) return false;  // none idle yet: wait
                    std::printf("script: fleet %u moves %s -> %s by %s\n", st->fleet, g->map().planets.name[st->from].c_str(),
                                g->map().planets.name[st->to].c_str(), drag ? "drag" : "click");
                    // Both planets on screen: the camera looks at their midpoint.
                    const Vec3 c0 = g->planets()[st->from].position, c1 = g->planets()[st->to].position;
                    a.gview_.camera().set_look_at({0.5f * (c0.x + c1.x), 0.5f * (c0.y + c1.y), 0.0f});
                    a.orders_.set_selection({});
                    st->refused0 = a.refused_;
                    st->phase = 1;
                    return false;
                }
                if (st->phase < 3) {  // camera and frame catch up
                    ++st->phase;
                    return false;
                }
                const ui::View v = ui::make_view(a.gview_.camera_params(a.targets_.width, a.targets_.height), a.targets_.width, a.targets_.height);
                float fx = 0, fy = 0, px = 0, py = 0;
                const render::GalacticFleetSnap* fs = nullptr;
                for (const render::GalacticFleetSnap& x : a.gview_.frame().fleets)
                    if (static_cast<uint32_t>(x.id) == st->fleet) fs = &x;
                if (st->phase == 3 || st->phase == 4) {
                    // The orbiting fleet has no map snapshot (only fleets in transit are drawn): only the target planet
                    // needs a screen position.
                    (void)fs;
                    (void)fx;
                    (void)fy;
                    const Vec3 pp = g->planets()[st->to].position;
                    if (!ui::project(v, {pp.x, pp.y, pp.z}, px, py)) return false;
                    // GALAXY_MAP_PRESENTATION §2: an orbiting fleet is no longer a pick target (only fleets in transit are
                    // drawn); it is selected through its HUD planet fleet button = HudAction GalacticSelect {fleet}.
                    // The smoke issues the button's action directly (no screen position for the button), so the drag
                    // variant ends with the same right-click order as the click variant.
                    if (st->phase == 3) {
                        ui::HudAction act;
                        act.kind = ui::HudAction::Kind::GalacticSelect;
                        act.ids = {st->fleet};
                        a.handle_hud_actions({act});
                        st->phase = 4;
                        return false;
                    }
                    if (click_at(st->click, f, static_cast<int32_t>(px), static_cast<int32_t>(py), ui::Button::Right)) st->phase = 5;
                    return false;
                }
                // Order sent: refused -> fail; else wait for the fleet to leave and to orbit the target.
                if (a.refused_ != st->refused0) {
                    std::fprintf(stderr, "script: galactic move refused (see the submit line)\n");
                    a.notes_.push_back("galactic move: order refused");
                    a.smoke_ok_ = false;
                    return true;
                }
                if (!store.alive(st->member)) {
                    std::fprintf(stderr, "script: the moving fleet's unit died\n");
                    a.script_failed_ = true;
                    return true;
                }
                for (const galactic::FleetState& fl : g->fleets())
                    if (fl.alive && std::find(fl.contents.begin(), fl.contents.end(), st->member) != fl.contents.end()) {
                        if (st->phase == 5 && fl.moving) {
                            std::printf("script: fleet %u left %s\n", st->fleet, g->map().planets.name[st->from].c_str());
                            st->phase = 6;
                        }
                        if (st->phase == 6 && !fl.moving && fl.planet == st->to) {
                            std::printf("script: fleet %u arrived at %s\n", st->fleet, g->map().planets.name[st->to].c_str());
                            return true;
                        }
                    }
                return false;
            },
            6000};
}

}  // namespace script

using namespace script;

void App::make_script() {
    auto& s = script_;
    // Intro movies (when FFmpeg's Bink decoder is present): one frame shown, PNG, then Esc skips each one.
    s.push_back({"intro movies skipped", [](App& a, ui::InputFrame& f) {
                     static int n = 0;
                     if (a.screen_ != Screen::Movie) return true;
                     if (++n % 20 == 0) {
                         f.keys_down.set(ui::vk::kEscape);
                         f.keys_pressed.set(ui::vk::kEscape);
                     }
                     // (not the save / load smoke: it shares the PNG folder and writes only game_smoke_saveload_*.png)
                     if (n == 10 && a.full_movie_.has_frame() && !a.opt_.png_dir.empty() && a.opt_.smoke != "saveload")
                         a.shot_path_ = a.opt_.png_dir + "/game_intro_movie.png";
                     return false;
                 },
                 1200});
    s.push_back(step_wait("menu shown", [](App& a) { return a.ui_.gui.top() != nullptr && a.frames_ > 2; }, 600));
    if (opt_.smoke == "saveload") {  // N1 follow-up: the save / load menus driven like a player (app_script_saveload.cpp)
        add_saveload_steps(s);
        return;
    }
    s.push_back(step_shot("game_main_menu.png"));
    if (opt_.smoke == "skirmish") {
        // modc t08: the mod picker from the main menu's Mods button: the rows (base game first, then whatever mods this
        // machine has), the base game's load report once its check finished, PNG, Back. Never launches anything; the
        // skirmish session (and its golden) starts afresh later.
        s.push_back(step_click_control("IDD_MAIN_MENU_DIALOG", "IDC_PORT_MAIN_MENU_MODS"));
        s.push_back(step_wait("mod picker: base game report shown", [](App& a) {
            const ui::GuiDialog* d = a.ui_.gui.find(ui::kPortModsDialog);
            const ui::GuiControl* list = d ? d->get("IDC_PORT_MODS_LIST") : nullptr;
            const ui::GuiControl* rep = d ? d->get("IDC_PORT_MODS_REPORT") : nullptr;
            if (!list || !rep || list->items.empty() || rep->items.empty() || rep->items[0] == U"Checking...") return false;
            std::printf("script: mod picker - %zu rows, base report: %s\n", list->items.size(), utf8_of(rep->items[0]).c_str());
            if (rep->items[0].rfind(U"Loads", 0) != 0) {
                a.notes_.push_back("mod picker: base game report " + utf8_of(rep->items[0]));
                a.smoke_ok_ = false;
            }
            return true;
        }, 6000));
        s.push_back(step_shot("game_mod_picker.png"));
        s.push_back(step_click_control(ui::kPortModsDialog, "IDC_CANCEL_BUTTON"));
        s.push_back(step_wait("main menu again", [](App& a) {
            const ui::GuiDialog* d = a.ui_.gui.top();
            return d && iequal(d->name(), "IDD_MAIN_MENU_DIALOG");
        }, 120));
        // OI host: the attract demo behind the main menu (MAIN_MENU_BACKGROUND.md; started here as the menu-open path would,
        // scripted runs never start it on their own): its battle world is drawn under the menu (world draws > 0, no
        // tactical HUD) and keeps ticking under the menu and its sub-dialogs (input never stops it); the skirmish's start
        // ends it. Its own session: the skirmish's session, CRC log and recording start afresh afterwards.
        s.push_back(step_wait("attract demo starts", [](App& a) {
            if (a.attract_.menu_opened(a.now_s_, true) == AttractController::Request::None || !a.start_attract_map()) {
                a.notes_.push_back("attract demo did not start");
                a.smoke_ok_ = false;
                a.stop_attract();
            }
            return true;
        }));
        auto tick0 = std::make_shared<uint64_t>(0);
        s.push_back(step_wait("attract demo drawn under the menu", [tick0](App& a) {
            if (!a.demo_session()) return true;  // failed above
            const sim::TacticalBattle* b = a.session_->battle();
            const bool drawn = a.screen_ == Screen::Menu && a.bview_.active() && b && a.draw_stats_.draws > 0 &&
                               a.ui_.gui.find("IDD_MAIN_MENU_DIALOG") && a.ui_.tactical_hud.stats().quads == 0;
            if (drawn) {
                *tick0 = b->frame();
                std::printf("script: attract demo - %u world draws under the main menu, battle frame %u\n", a.draw_stats_.draws, b->frame());
            }
            return drawn;
        }, 3000));
        s.push_back(step_shot("game_attract_demo.png"));
        // Playtest 3 t14: the demo's hulls must point along their travel direction (models face -Y, facing = the nose; the
        // same convention as the tactical battle): positions at two ticks 120 apart, the later facing against the displacement.
        {
            struct Orient {
                uint64_t t0 = 0;
                std::map<objects::ObjectId, std::array<float, 2>> pos;
            };
            auto st = std::make_shared<Orient>();
            s.push_back(step_wait("attract hull heading vs travel", [st](App& a) {
                const sim::TacticalBattle* b = a.demo_session() ? a.session_->battle() : nullptr;
                if (!b) return true;  // the demo did not start (noted above)
                const std::vector<render::SnapshotObject>& objs = a.bview_.frame().objects;
                if (st->pos.empty()) {
                    st->t0 = b->frame();
                    for (const render::SnapshotObject& o : objs)
                        if (o.owner >= 0 && b->combat().has_unit(o.id)) st->pos[o.id] = {o.position.x, o.position.y};
                    return false;
                }
                if (b->frame() < st->t0 + 120) return false;
                uint32_t moving = 0, forward = 0, sideways = 0, backward = 0;
                double sum_cos = 0.0;
                for (const render::SnapshotObject& o : objs) {
                    const auto it = st->pos.find(o.id);
                    if (it == st->pos.end()) continue;
                    const float dx = o.position.x - it->second[0], dy = o.position.y - it->second[1];
                    const float dl = std::sqrt(dx * dx + dy * dy), fl = std::sqrt(o.facing.x * o.facing.x + o.facing.y * o.facing.y);
                    if (dl < 5.0f || fl < 0.1f) continue;
                    const float c = (dx * o.facing.x + dy * o.facing.y) / (dl * fl);
                    ++moving;
                    sum_cos += static_cast<double>(c);
                    if (c > 0.7f) ++forward;
                    else if (c < -0.7f) ++backward;
                    else if (std::fabs(c) < 0.5f) ++sideways;
                }
                std::printf("script: attract orientation - %u moving hulls over 120 ticks: %u nose-first (cos > 0.7), %u backwards, %u sideways, mean cos %.3f\n",
                            moving, forward, backward, sideways, moving ? sum_cos / static_cast<double>(moving) : 0.0);
                if (moving > 0 && forward * 2 < moving) {  // most moving hulls must fly nose-first
                    a.notes_.push_back("attract hulls are not oriented along their travel direction");
                    a.smoke_ok_ = false;
                }
                return true;
            }, 3000));
            s.push_back(step_shot("game_attract_demo_later.png"));
        }
        // Playtest 3 t14: the demo camera's shots. Four cuts (a forced new shot every 45 ticks, each on the next combat unit),
        // a PNG per shot; at the end the followed hull must not have been seen nose-on / tail-on (within 30 degrees of the
        // view direction) for more than a quarter of the frames (the stand-in camera keeps a side view, attract_mode.hpp).
        {
            auto shot_tick = std::make_shared<uint64_t>(0);
            for (int k = 0; k < 4; ++k) {
                s.push_back(step_wait("attract camera cut", [shot_tick, k](App& a) {
                    const sim::TacticalBattle* b = a.demo_session() ? a.session_->battle() : nullptr;
                    if (!b) return true;
                    if (k == 0) a.demo_stats_ = {};
                    if (k == 0 || b->frame() >= *shot_tick + 45) {
                        *shot_tick = b->frame();
                        a.demo_shot_t0_ = -1.0;  // the next demo_camera() frame cuts to the next unit
                        return true;
                    }
                    return false;
                }, 1200));
                s.push_back(step_wait("attract camera shot runs", [shot_tick](App& a) {
                    const sim::TacticalBattle* b = a.demo_session() ? a.session_->battle() : nullptr;
                    return !b || b->frame() >= *shot_tick + 44;
                }, 1200));
                s.push_back(step_shot("game_attract_cam_" + std::to_string(k) + ".png"));
            }
            s.push_back(step_wait("attract camera nose-on check", [](App& a) {
                if (!a.demo_session()) return true;
                const auto& st = a.demo_stats_;
                std::printf("script: attract camera (%s) - %u frames, followed hull nose-on/tail-on in %u (longest run %u frames), nearby hulls %u / %u nose-on/tail-on\n",
                            a.demo_side_view_ ? "side view" : "first stand-in", st.frames, st.target_nose_on, st.longest_run, st.hulls_nose_on, st.hulls);
                // q6 t02: a demo map without combat units (Thrawn's Revenge's End_Demo_Attract_* place only props) has
                // nothing to follow: noted, not a failure. Vanilla demos always follow a hull (frames > 0).
                const sim::TacticalBattle* b = a.session_->battle();
                bool any_unit = false;
                for (const render::SnapshotObject& o : a.bview_.frame().objects)
                    if (b && o.owner >= 0 && b->combat().has_unit(o.id)) any_unit = true;
                if (st.frames == 0 && !any_unit) {
                    a.notes_.push_back("attract demo: no combat unit on the demo map, camera check skipped");
                } else if (st.frames == 0 || st.target_nose_on * 4 > st.frames) {
                    a.notes_.push_back("attract camera shows the followed hull nose-on / tail-on for a long stretch");
                    a.smoke_ok_ = false;
                }
                return true;
            }));
        }
        s.push_back(step_wait("attract demo ticks under the menu", [tick0](App& a) {
            const sim::TacticalBattle* b = a.demo_session() ? a.session_->battle() : nullptr;
            return !b || b->frame() >= *tick0 + 30;
        }, 600));
        s.push_back(step_click_control("IDD_MAIN_MENU_DIALOG", "IDC_MAIN_MENU_SINGLE_PLAYER_GAMES"));
        s.push_back(step_wait("attract demo runs under the single-player dialog", [tick0](App& a) {
            const sim::TacticalBattle* b = a.demo_session() ? a.session_->battle() : nullptr;
            if (!b) {
                a.notes_.push_back("attract demo stopped by the menu input");
                a.smoke_ok_ = false;
                return true;
            }
            const bool ok = a.attract_.running() && a.bview_.active() && a.draw_stats_.draws > 0 && b->frame() >= *tick0 + 60 &&
                            a.ui_.gui.find("IDD_SINGLE_PLAYER_GAMES_MENU_DIALOG") != nullptr;
            if (ok) std::printf("script: attract demo - still running under the single-player dialog, battle frame %u\n", b->frame());
            return ok;
        }, 600));
    } else {
        s.push_back(step_click_control("IDD_MAIN_MENU_DIALOG", "IDC_MAIN_MENU_SINGLE_PLAYER_GAMES"));
    }
    auto battle_steps = [&s](const std::string& prefix) {
        s.push_back(step_wait("load screen ready", [](App& a) { return a.screen_ == Screen::BattleLoad && a.ui_.load.ready(); }, 6000));
        s.push_back(step_shot(prefix + "_load.png"));
        s.push_back({"begin", [](App& a, ui::InputFrame& f) {
                         static ClickState c;  // one Begin per run of this step
                         if (a.screen_ != Screen::BattleLoad) return true;
                         ui::GuiDialog* d = a.ui_.gui.find(a.ui_.load.dialog());
                         const int32_t i = d ? d->find("IDC_BEGIN_BUTTON") : -1;
                         if (i < 0) return false;
                         const ui::GuiRect r = d->control_rect(static_cast<size_t>(i));
                         const ui::Vec2f p = a.ui_.gui.canvas().canvas_to_pixel(0.5f * (r.min.x + r.max.x), 0.5f * (r.min.y + r.max.y));
                         if (click_at(c, f, static_cast<int32_t>(p.x), static_cast<int32_t>(p.y), ui::Button::Left)) c = {};
                         return false;
                     },
                     600});
        s.push_back(step_ticks("battle settles", 30));
        s.push_back(step_select_all());
        s.push_back(step_ticks("selection drained", 3));
        add_control_group_steps(s);
        s.push_back(step_encyclopedia("encyclopedia over a unit"));
        s.push_back(step_shot(prefix + "_encyclopedia.png"));
        if (env_or_empty("ALAMO_SMOKE_SCREENS") == "1") s.push_back(step_unit_tooltip(prefix + "_screen_tooltip.png"));
        s.push_back(step_attack_enemy());
        s.push_back(step_wait("attack routed into the battle", [](App& a) { return a.session_ && a.session_->order_stats().attacks >= 1; }, 600));
        s.push_back(step_ticks("battle runs", 60));
        s.push_back(step_wait("camera on the local units", [](App& a) {
            camera_on_local_units(a);
            return true;
        }));
        s.push_back(step_shot(prefix + "_battle.png"));
        if (env_or_empty("ALAMO_SMOKE_DRAG_BOX") == "1") s.push_back(step_drag_box(prefix + "_drag_box.png"));
        s.push_back(step_move_ground());
        s.push_back(step_wait("move routed", [](App& a) { return a.session_ && a.session_->order_stats().moves >= 1; }, 600));
        // Queueable actions: shift+right-click an enemy (queued attack), then shift+right-click the ground (queued move);
        // the HUD shows the selected units' queue markers.
        s.push_back(step_attack_enemy(true));
        s.push_back(step_move_ground(true));
        s.push_back(step_wait("queued orders routed", [](App& a) {
            return a.session_ && a.session_->order_stats().attacks >= 2 && a.session_->order_stats().moves >= 2;
        }, 600));
        s.push_back(step_wait("queue markers shown", [](App& a) {
            if (a.queues_.empty()) return false;
            // Movement path lines of the moving selection (HudWaypointLine, same frame: no extra step / tick).
            const ui::TacticalHudStats& hs = a.ui_.tactical_hud.stats();
            std::printf("script: path lines - %zu units, %u waypoint dots, %u ability buttons\n", a.waypoints_.size(), hs.waypoint_dots,
                        hs.ability_buttons);
            if (a.waypoints_.empty()) a.notes_.push_back("no movement path line for the moving selection");
            return true;
        }, 600));
        s.push_back(step_shot(prefix + "_queued.png"));
        if (env_or_empty("ALAMO_SMOKE_NO_ABILITY_PICK") != "1") s.push_back(step_ability_pick());  // (=1: A/B check, the step moves no CRC)
        if (prefix == "game_skirmish") {  // P4 host: the bought fleet overflows the cap - a reserve dropped from the pad
            if (env_or_empty("ALAMO_SMOKE_REINFORCE_HOVER") == "1") s.push_back(step_reinforce_hover(prefix + "_reinforce_hover.png"));
            s.push_back(step_reinforce());
            s.push_back(step_shot(prefix + "_reinforced.png"));
        }
        add_pause_steps(s);
        if (prefix == "game_land" || prefix == "game_skirmish") {  // combat: the forces engage (shots, deaths, animation, decals)
            s.push_back(step_combat(prefix == "game_land"));
            s.push_back(step_shot(prefix + "_combat.png"));
        }
        // Battle end (time limit): the battle-end screen, then Continue.
        s.push_back(step_wait("battle-end screen", [](App& a) { return a.battle_end_open_ && a.ui_.gui.find(ui::dlg::kBattleEnd); }, 20000));
        s.push_back(step_shot(prefix + "_battle_end.png"));
        if (env_or_empty("ALAMO_SMOKE_SCREENS") == "1") s.push_back(step_victory_screen(prefix + "_screen_victory.png"));
        s.push_back(step_click_control(ui::dlg::kBattleEnd, "IDC_CONTINUE_BUTTON"));
    };
    // Skirmish buy-units screen: buy the first row, sell everything again (the default fleet stays), Start game.
    // P4 host: the space skirmish keeps a bought fleet instead, larger than the space unit cap (reinforcement pool).
    auto buy_steps = [&s](const std::string& prefix) {
        // Row 0; the space skirmish buys corvettes (kSmokeSkirmishBuyType, a cap-counted combat ship) when the list has them.
        const bool space_buy = prefix == "game_skirmish";
        s.push_back(step_select_row(ui::dlg::kBuyUnits, "IDC_SKIRMISH_BUY_FORCES_LISTBOX", [space_buy](App& a) {
            // q6 t02: data without the stock corvette (mods): ALAMO_SMOKE_BUY_TYPE names the type; else the first listed
            // *_Corvette, else the first row that is not Build_Initially_Locked (Thrawn's Revenge lists a locked cheat unit
            // first). Vanilla always finds kSmokeSkirmishBuyType.
            const std::string want = env_or_empty("ALAMO_SMOKE_BUY_TYPE");
            const std::vector<ui::BuyUnitEntry>& u = a.buy_.units;
            for (size_t i = 0; i < u.size(); ++i)
                if (!want.empty() && iequal(u[i].type, want)) return i;
            if (!space_buy) return size_t{0};
            for (size_t i = 0; i < u.size(); ++i)
                if (u[i].type == kSmokeSkirmishBuyType) return i;
            const auto upper_name = [](std::string n) {
                for (char& c : n) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                return n;
            };
            for (size_t i = 0; i < u.size(); ++i)
                if (upper_name(u[i].type).find("_CORVETTE") != std::string::npos) return i;
            for (size_t i = 0; i < u.size(); ++i) {
                const uint32_t t = a.data_.rules.find_type(u[i].type);
                if (t != galactic::kNoIndex && !a.data_.rules.types[t].build_initially_locked) return i;
            }
            return size_t{0};
        }));
        s.push_back(step_click_control(ui::dlg::kBuyUnits, "IDC_BUY_BUTTON"));
        s.push_back(step_wait("unit bought", [](App& a) { return !a.bought_.empty(); }, 120));
        s.push_back(step_shot(prefix + "_buy_units.png"));
        if (prefix == "game_skirmish") {
            for (int i = 0; i < kSmokeSkirmishBuys - 1; ++i) s.push_back(step_click_control(ui::dlg::kBuyUnits, "IDC_BUY_BUTTON"));
        } else {
            s.push_back(step_click_control(ui::dlg::kBuyUnits, "IDC_SELL_ALL_BUTTON"));
        }
        s.push_back(step_click_control(ui::dlg::kBuyUnits, "IDC_NEXT_BUTTON"));
    };
    if (opt_.smoke == "land") {  // land skirmish: the first land map of the list (rows after the space maps)
        s.push_back(step_click_control("IDD_SINGLE_PLAYER_GAMES_MENU_DIALOG", "IDC_SKIRMISH_GAMES_BUTTON"));
        s.push_back(step_click_first_row("IDD_SKIRMISH_STAGING_DIALOG", "IDC_GAME_TYPE_LIST"));  // p4uim t01: the Land game type
        s.push_back(step_wait("land game type listed", [](App& a) { return !a.ui_.front.skirmish_space_type(); }, 60));
        s.push_back(step_select_row("IDD_SKIRMISH_STAGING_DIALOG", "IDC_STAGING_MAP_LIST", [](App& a) {
            // The first land map by name (the old list's first land row): its row in the staging list as the front end sorted it;
            // else the first land row of the Official tab.
            const std::vector<std::string> land = skirmish_land_maps(a.data_);
            int32_t row = land.empty() ? -1 : a.ui_.front.skirmish_map_row(land.front());
            if (row < 0 && a.skirmish_catalog_)
                for (const SkirmishMapEntry& e : skirmish_map_rows(*a.skirmish_catalog_, false))
                    if (e.kind == 1 && row < 0) row = a.ui_.front.skirmish_map_row(e.map);
            return static_cast<size_t>(std::max<int32_t>(row, 0));
        }));
        s.push_back(step_shot("game_land_staging.png"));
        s.push_back(step_click_control("IDD_SKIRMISH_STAGING_DIALOG", "IDC_SKIRMISH_START_GAME_BUTTON"));
        buy_steps("game_land");
        battle_steps("game_land");
        s.push_back(step_wait("land skirmish over", [](App& a) { return a.session_ && a.skirmish_ended_ && a.screen_ == Screen::Menu; }, 20000));
    } else if (opt_.smoke == "skirmish") {
        s.push_back(step_click_control("IDD_SINGLE_PLAYER_GAMES_MENU_DIALOG", "IDC_SKIRMISH_GAMES_BUTTON"));
        // The dialog opens on Land (UI.md 9.7): select the Space game type (row 1) so the run starts on Kuat.
        s.push_back(step_select_row("IDD_SKIRMISH_STAGING_DIALOG", "IDC_GAME_TYPE_LIST", [](App&) { return size_t{1}; }));
        s.push_back(step_wait("space game type listed", [](App& a) { return a.ui_.front.skirmish_space_type(); }, 60));
        s.push_back(step_shot("game_skirmish_staging.png"));
        s.push_back(step_click_control("IDD_SKIRMISH_STAGING_DIALOG", "IDC_SKIRMISH_START_GAME_BUTTON"));
        buy_steps("game_skirmish");
        battle_steps("game_skirmish");
        s.push_back(step_wait("skirmish over", [](App& a) { return a.session_ && a.skirmish_ended_ && a.screen_ == Screen::Menu; }, 20000));
    } else {
        // Selects row `row` exactly: step_select_row stops at the first row >= the wanted one (its click can land below
        // it), then Up keys back to it.
        auto exact_row = [&s](const char* dialog, const char* control, std::shared_ptr<int64_t> row) {
            s.push_back(step_select_row(dialog, control, [row](App&) { return static_cast<size_t>(std::max<int64_t>(*row, 0)); }));
            auto key_frame = std::make_shared<bool>(false);
            s.push_back({"campaign set row", [row, key_frame, dialog, control](App& a, ui::InputFrame& f) {
                             ui::GuiDialog* d = a.ui_.gui.find(dialog);
                             const ui::GuiControl* c = d ? d->get(control) : nullptr;
                             if (!c || c->selected < 0 || static_cast<int64_t>(c->selected) <= *row) return true;
                             *key_frame = !*key_frame;
                             if (*key_frame) {
                                 f.keys_down.set(ui::vk::kUp);
                                 f.keys_pressed.set(ui::vk::kUp);
                             }
                             return false;
                         },
                         2000});
        };
        // q6 t02: ALAMO_SMOKE_CAMPAIGN_SET=<Campaign_Set> plays that custom set instead of the list's first one (a mod's
        // first set can be a scripted menu: Thrawn's Revenge's GCMenu). q6 t03: a New Campaign (story) set, or one campaign
        // of such a set (Thrawn's Revenge: Thrawn_Empire), is played through New Campaign: its row, then the faction button
        // of that campaign (the set's first faction for a set name). A mod menu without the New Campaign button (Thrawn's
        // Revenge reaches its historical campaigns through the GCMenu set's story selector) gets the StartCampaign action
        // that button path sends (normal difficulty). Unset: the first custom row, as before.
        const std::string want = env_or_empty("ALAMO_SMOKE_CAMPAIGN_SET");
        auto custom_row = std::make_shared<int64_t>(-1), story_row = std::make_shared<int64_t>(-1);
        size_t story_faction = 0;
        ui::CampaignChoice story_choice;
        if (!want.empty()) {
            const ui::CampaignMenu menu = ui::build_campaign_menu(data_.db);
            for (size_t i = 0; i < menu.custom.size() && *custom_row < 0; ++i)
                if (iequal(menu.custom[i].set, want)) *custom_row = static_cast<int64_t>(i);
            for (size_t i = 0; i < menu.story.size() && *custom_row < 0 && *story_row < 0; ++i) {
                if (iequal(menu.story[i].set, want)) *story_row = static_cast<int64_t>(i);
                for (size_t j = 0; j < menu.story[i].factions.size() && *story_row < 0; ++j)
                    if (iequal(menu.story[i].factions[j].campaign, want)) *story_row = static_cast<int64_t>(i), story_faction = j;
                if (*story_row >= 0 && story_faction < menu.story[i].factions.size()) story_choice = menu.story[i].factions[story_faction];
            }
            if (*custom_row < 0 && *story_row < 0)
                notes_.push_back("ALAMO_SMOKE_CAMPAIGN_SET " + want + ": no such campaign set or campaign, first custom row kept");
            if (*story_row >= 0) std::printf("script: campaign smoke plays New Campaign row %" PRId64 ", faction button %zu\n", *story_row, story_faction);
        }
        if (*story_row >= 0) {
            auto direct = std::make_shared<bool>(false);
            s.push_back({"New Campaign button or the campaign's start action", [direct, story_choice](App& a, ui::InputFrame&) {
                             const ui::GuiDialog* d = a.ui_.gui.find("IDD_SINGLE_PLAYER_GAMES_MENU_DIALOG");
                             if (!d || d != a.ui_.gui.top()) return false;
                             if (d->find("IDC_NEW_CAMPAIGN_BUTTON") >= 0) return true;
                             *direct = true;
                             std::printf("script: the menu has no New Campaign button: StartCampaign %s (%s)\n", story_choice.campaign.c_str(),
                                         story_choice.faction.c_str());
                             ui::MenuAction act;
                             act.kind = ui::MenuAction::Kind::StartCampaign;
                             act.campaign = story_choice.campaign;
                             act.faction = story_choice.faction;
                             a.handle_menu_actions({act});
                             return true;
                         },
                         600});
            const auto unless_direct = [direct](ScriptStep st) {
                return ScriptStep{st.name, [direct, run = std::move(st.run)](App& a, ui::InputFrame& f) { return *direct || run(a, f); },
                                  st.timeout_frames};
            };
            s.push_back(unless_direct(step_click_control("IDD_SINGLE_PLAYER_GAMES_MENU_DIALOG", "IDC_NEW_CAMPAIGN_BUTTON")));
            s.push_back(unless_direct(step_shot("game_campaign_select.png")));
            const size_t first = s.size();
            exact_row("IDD_CAMPAIGN_SELECTION_DIALOG", "IDC_CAMPAIGNS_LIST", story_row);
            for (size_t k = first; k < s.size(); ++k) s[k] = unless_direct(std::move(s[k]));
            s.push_back(unless_direct(
                step_click_control("IDD_CAMPAIGN_SELECTION_DIALOG", story_faction == 0 ? "IDC_PLAY_FACTION_A_BUTTON" : "IDC_PLAY_FACTION_B_BUTTON")));
        } else {
            s.push_back(step_click_control("IDD_SINGLE_PLAYER_GAMES_MENU_DIALOG", "IDC_CUSTOM_CAMPAIGN_BUTTON"));
            s.push_back(step_shot("game_campaign_select.png"));
            if (*custom_row >= 0) exact_row("IDD_CUSTOM_CAMPAIGN_STAGING_DIALOG", "IDC_STAGING_MAP_LIST", custom_row);
            s.push_back(step_click_control("IDD_CUSTOM_CAMPAIGN_STAGING_DIALOG", "IDC_PLAY_BUTTON"));
        }
        s.push_back(step_wait("galactic map", [](App& a) { return a.screen_ == Screen::Galactic; }, 600));
        s.push_back(step_ticks("galaxy runs", 30));
        // GALACTIC_DIALOGS §2: no dialog opens at campaign start; the opening Story_Dialog event adds its chapter to the
        // mission holocron (b_story_arc_g flashes). Clicking the holocron opens the story summary, which holds the ticks.
        {
            auto n = std::make_shared<int>(0);
            s.push_back({"holocron updated", [n](App& a, ui::InputFrame&) { return a.holocron_updates_ > 0 || ++*n > 300; }, 400});
        }
        // q6 t03: without a chapter (Thrawn's Revenge's historical campaigns add none) the summary is not opened, so its shot
        // and close steps are skipped. Vanilla campaigns always have one.
        auto no_summary = std::make_shared<bool>(false);
        s.push_back(step_wait("holocron button opens the story summary", [no_summary](App& a) {
            if (a.holocron_chapters_.empty()) return *no_summary = true;  // no story chapter in this campaign
            ui::HudAction act;
            act.kind = ui::HudAction::Kind::Button;
            act.name = "b_story_arc_g";
            a.handle_hud_actions({act});
            return a.story_summary_open_ && a.ui_.gui.find(ui::dlg::kStorySummary) != nullptr;
        }, 10));
        for (ScriptStep st : {step_shot("game_campaign_story.png"), step_click_control(ui::dlg::kStorySummary, "IDC_SUMMARY_DIALOG_CLOSE_BUTTON"),
                              step_wait("story summary closed", [](App& a) { return !a.story_summary_open_; }, 300)})
            s.push_back({st.name, [no_summary, run = std::move(st.run)](App& a, ui::InputFrame& f) { return *no_summary || run(a, f); }, st.timeout_frames});
        s.push_back(step_shot("game_campaign_galactic.png"));
        if (env_or_empty("ALAMO_SMOKE_FLEET_SLOTS") == "1")
            s.push_back(step_fleet_slots("game_campaign_fleet_slots.png", "game_campaign_fleet_slot_drag.png"));
        if (env_or_empty("ALAMO_SMOKE_ZOOM_LIGHT") == "1") s.push_back(step_zoom_light("game_campaign_zoom_light.png"));
        if (env_or_empty("ALAMO_SMOKE_TRANSIT") == "1") {  // t32: a shot with any fleet in flight (opt-in, gives up after 3000 frames)
            auto n = std::make_shared<int>(0);
            s.push_back({"a fleet in transit", [n](App& a, ui::InputFrame&) {
                             for (const render::GalacticFleetSnap& fl : a.gview_.frame().fleets)
                                 if (fl.flags & render::kGalFleetMoving) {
                                     a.gview_.camera().set_look_at({fl.position.x, fl.position.y, 0.0f});
                                     return true;
                                 }
                             return ++*n > 3000;
                         }, 3100});
            s.push_back(step_shot("game_campaign_transit.png"));
        }
        add_pause_steps(s);  // galactic pause / resume
        s.push_back(step_encyclopedia("encyclopedia over a planet"));
        s.push_back(step_shot("game_campaign_encyclopedia.png"));
        // q6 t13: the fleet steps below (hops, attacks, the battle, the second attack and its auto-resolve) need a fleet of
        // the local player. A campaign that gives it none (Thrawn's Revenge's only custom set, GCMenu_Rebel, is the mod's
        // scripted selection menu: two planets, no starting forces) skips them with a log line; the pause, encyclopedia,
        // options, video modes, black market, holocron, story summary and campaign score checks before and after still run.
        // Decided on the first frame of the first fleet step, which then runs in that same frame: a campaign with a fleet
        // (every vanilla one) keeps its frame-for-frame step timing and command stream.
        auto no_fleet = std::make_shared<int>(-1);  // -1 not decided yet, 0 the local player has a fleet, 1 none
        auto gated = std::make_shared<size_t>(0);   // how many steps need the fleet
        const auto needs_fleet = [no_fleet, gated](ScriptStep st) {
            return ScriptStep{st.name, [no_fleet, gated, run = std::move(st.run)](App& a, ui::InputFrame& f) {
                                  if (*no_fleet < 0) {
                                      const galactic::GalacticSim* g = a.session_ ? a.session_->galaxy() : nullptr;
                                      if (!g) return run(a, f);
                                      bool own = false;
                                      for (const galactic::FleetState& fl : g->fleets())
                                          own = own || (fl.alive && fl.owner == a.local_player() && !fl.contents.empty());
                                      *no_fleet = own ? 0 : 1;
                                      if (!own)
                                          std::printf("script: the local player owns no fleet in %s: the %zu galactic move, attack and battle steps "
                                                      "are skipped (each logs done without running)\n",
                                                      a.session_->spec().campaign.c_str(), *gated);
                                  }
                                  return *no_fleet == 1 || run(a, f);
                              },
                              st.timeout_frames};
        };
        const size_t fleet_steps = s.size();
        // Plain fleet hops by input, to arrival (click, then drag).
        s.push_back(answer_ai_attacks(step_galactic_move(false)));
        s.push_back(answer_ai_attacks(step_galactic_move(true)));
        s.push_back(answer_ai_attacks(step_galactic_attack()));
        // The pending-battle choice (message box): Fight.
        s.push_back(step_wait("battle prompt shown", [](App& a) { return a.choice_open_; }, 6000));
        s.push_back(step_shot("game_campaign_battle_choice.png"));
        s.push_back(step_wait("Begin", [](App& a) {
            ui::HudAction act;
            act.kind = ui::HudAction::Kind::BattleBegin;
            a.handle_hud_actions({act});
            return !a.choice_open_;
        }, 10));
        battle_steps("game_campaign");
        s.push_back(step_wait("back on the galactic map", [](App& a) { return a.screen_ == Screen::Galactic && !a.bview_.active() && !a.battle_end_open_; }, 20000));
        // Shot at once: the AI may start the next conflict within a few ticks (the shot holds the sim).
        s.push_back(step_shot("game_campaign_galactic_after.png"));
        // GALACTIC_DIALOGS §3.1: a second attack answered Auto-Resolve - resolved at once (no progress dialog), the red
        // result line, then the battle-end screen.
        s.push_back(step_galactic_attack());
        s.push_back(step_wait("battle prompt shown again", [](App& a) { return a.choice_open_; }, 6000));
        s.push_back(step_wait("Auto-Resolve", [](App& a) {
            ui::HudAction act;
            act.kind = ui::HudAction::Kind::BattleAutoResolve;
            a.handle_hud_actions({act});
            return !a.choice_open_;
        }, 10));
        s.push_back(step_wait("auto-resolve battle-end screen", [](App& a) { return a.auto_results_ > 0 && a.battle_end_open_ && a.ui_.gui.find(ui::dlg::kBattleEnd) && a.screen_text_.empty(); }, 600));
        s.push_back(step_shot("game_campaign_auto_result.png"));
        s.push_back(step_click_control(ui::dlg::kBattleEnd, "IDC_CONTINUE_BUTTON"));
        *gated = s.size() - fleet_steps;
        for (size_t k = fleet_steps; k < s.size(); ++k) s[k] = needs_fleet(std::move(s[k]));
        // The galaxy stands still from here (presentation-only hold: no AI conflict interrupts the screen checks).
        s.push_back(step_wait("hold the galaxy", [](App& a) { return a.script_hold_ = true; }, 10));
        // In-game options (MAIN_LOOP.md §1.5): the HUD's options button opens the game menu, which pauses the session; the
        // options and their video page keep it paused (the video page lists the display modes); Esc x3 closes them all and
        // the session runs again. The galaxy is held already (no tick moves).
        s.push_back(step_wait("options button opens the game menu", [](App& a) {
            ui::HudAction act;
            act.kind = ui::HudAction::Kind::Button;
            act.name = "b_option_g";
            a.handle_hud_actions({act});
            return a.paused_ && a.ui_.front.pause_menu_open();
        }, 10));
        s.push_back(step_click_control("IDD_GAME_OPTIONS_DIALOG", "IDC_INGAME_OPTIONS"));
        s.push_back(step_click_control("IDD_IN_GAME_OPTIONS_DIALOG", "IDC_IN_GAME_OPTIONS_VIDEO"));
        s.push_back(step_wait("video options list the resolutions", [](App& a) {
            const ui::GuiDialog* d = a.ui_.gui.find("IDD_VIDEO_OPTIONS_DIALOG");
            const ui::GuiControl* l = d ? d->get("IDC_RESOLUTION_LIST") : nullptr;
            if (!d || !a.paused_) return false;
            const size_t n = l ? l->items.size() : 0;
            std::printf("script: video options - %zu resolutions (%zu display modes), selected %d, session paused\n", n, a.display_modes_.size(),
                        l ? l->selected : -1);
            if (n != a.display_modes_.size()) {
                a.notes_.push_back("video options: resolution list not filled");
                a.smoke_ok_ = false;
            }
            return true;
        }, 120));
        s.push_back(step_shot("game_campaign_video_options.png"));
        if (env_or_empty("ALAMO_SMOKE_SCREENS") == "1") s.push_back(step_shot("game_campaign_screen_video.png"));
        s.push_back(step_key("close the video options", ui::vk::kEscape));
        s.push_back(step_key("close the options", ui::vk::kEscape));
        s.push_back(step_key("close the game menu", ui::vk::kEscape));
        s.push_back(step_wait("game menu closed, session unpaused", [](App& a) { return !a.menu_dialog_open() && !a.paused_; }, 120));
        // P4 host: the black-market panel with the local player's real offers (bm_offers) at a planet of another playable
        // faction - a presentation-only preview: the scripted campaign has no natural black-market dialog (that needs an
        // Underworld hero at a Corruption_Black_Market planet), and the preview sends no command.
        s.push_back(step_wait("black market panel", [](App& a) {
            const galactic::GalacticSim* g = a.session_ ? a.session_->galaxy() : nullptr;
            if (!g) return true;
            if (!a.panel_preview_) {
                for (size_t i = 0; i < g->planets().size() && !a.preview_planet_; ++i) {
                    const galactic::PlanetState& p = g->planets()[i];
                    if (!p.is_planet || p.owner == a.local_player() || p.owner < 0 || static_cast<size_t>(p.owner) >= g->players().size()) continue;
                    const uint32_t f = g->players()[static_cast<size_t>(p.owner)].faction;
                    if (f < g->rules().factions.size() && g->rules().factions[f].is_playable) a.preview_planet_ = static_cast<uint32_t>(g->store().id(p.object));
                }
                a.panel_preview_ = galactic::kSpecialBlackMarket;
            }
            return a.market_.open() || !a.preview_planet_;  // no such planet: nothing to show
        }, 120));
        s.push_back(step_shot("game_campaign_black_market.png"));
        s.push_back(step_wait("black market closed", [](App& a) {
            a.panel_preview_ = 0;
            return true;
        }, 10));
        // Holocron (hotkey): droid log, then the tech tree and the production summary pages; Esc closes.
        s.push_back(step_hotkey("advisor key", ui::hk::kDroidAdvisor));
        s.push_back(step_wait("droid log shown", [](App& a) { return a.info_.info_dialog() == ui::dlg::kDroidLog; }, 300));
        s.push_back(step_shot("game_campaign_droid_log.png"));
        s.push_back(step_wait("tech tree page", [](App& a) {
            a.open_holocron(ui::InfoPage::TechTree);
            return !a.info_.info_dialog().empty() && a.info_.info_page() == ui::InfoPage::TechTree;
        }, 300));
        s.push_back(step_shot("game_campaign_tech_tree.png"));
        s.push_back(step_wait("production summary page", [](App& a) {
            a.open_holocron(ui::InfoPage::SummaryProduction);
            return a.info_.info_dialog() == ui::dlg::kSummaryProduction;
        }, 300));
        s.push_back(step_shot("game_campaign_summary.png"));
        s.push_back(step_key("close the holocron", ui::vk::kEscape));
        s.push_back(step_wait("holocron closed", [](App& a) { return a.info_.info_dialog().empty(); }, 300));
        // Story summary (b_story_arc_g opens it; [smoke] called directly): objectives + recap of the local plot set.
        s.push_back({"story summary", [n = std::make_shared<int>(0)](App& a, ui::InputFrame&) {
                         if (*n == 1) a.open_story_summary();
                         return ++*n > 3;
                     },
                     100});
        s.push_back(step_wait("story summary shown", [](App& a) { return a.ui_.gui.find(ui::dlg::kStorySummary) != nullptr; }, 300));
        s.push_back(step_shot("game_campaign_story_summary.png"));
        s.push_back(step_key("close the story summary", ui::vk::kEscape));
        s.push_back(step_wait("story summary closed", [](App& a) { return a.ui_.gui.find(ui::dlg::kStorySummary) == nullptr; }, 300));
        // Campaign end: [smoke] forced (the galaxy is not conquered in a smoke run) - the campaign score, then Quit.
        s.push_back(step_wait("campaign score", [](App& a) {
            if (!a.campaign_over_) a.show_campaign_end(true);
            return a.ui_.gui.find(ui::dlg::kCampaignScore) != nullptr;
        }, 300));
        s.push_back(step_shot("game_campaign_score.png"));
        s.push_back(step_click_control(ui::dlg::kCampaignScore, "IDC_QUIT_BUTTON"));
        s.push_back(step_wait("main menu after the campaign", [](App& a) { return a.screen_ == Screen::Menu; }, 300));
    }
}

}  // namespace alamo_game_app
