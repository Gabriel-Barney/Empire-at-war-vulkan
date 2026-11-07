// alamo_game App: --smoke saveload (N1 follow-up t01), the save / load menus driven like a player - every save, overwrite,
// delete and load goes through the in-game menu / main menu dialogs (mouse clicks on their controls, the save name typed
// key by key), never through save_game / load_game directly. Checked at each step (a failed check: a "note:" line,
// smoke_ok_ = false; a step that cannot continue stops the run):
//   1. custom campaign -> galactic map; the HUD's options button (b_option_g) opens the in-game menu; Save, the name typed
//      ("smoke_main"); the file holds the tick of the save.
//   2. the galaxy runs on; Save again on the same name (picked from the list): the overwrite question names the save; Yes
//      replaces the file (its header tick is the new save's tick).
//   3. a second save ("smoke_extra"); Delete on it: the question names it; No keeps the file; Delete + Yes removes it (the
//      list is refreshed, smoke_main stays).
//   4. Load smoke_main from the in-game menu, then Quit to the main menu and load it again from the main menu's Load Game
//      screen: each time the loaded session is at the save's tick and its state CRC (every CrcRegistry system, captured
//      before the first tick after the load) equals the one captured right after the save.
//   5. a human battle (galactic attack order, Fight, Begin); the tactical HUD's options button (b_option_t); Save
//      "smoke_battle" during the battle; Load it from the in-game menu: same tick / state CRC, the session is in the
//      battle, its load screen is up at once (no tick before Begin), Begin resumes the battle, the first tick after the
//      load is a battle tick and it runs on.
// PNGs (--png-dir): game_smoke_saveload_*.png. Saves: --saves-dir (default <png-dir>/saves); the run deletes its own three
// save names there before it starts (a re-run would otherwise meet the overwrite question on the first save).
// The run ends with the usual presentation-on/off check of the last loaded session (its save + the stream recorded after
// the load replayed) and 0 validation messages.
#include <filesystem>

#include "app_script.hpp"

namespace alamo_game_app::script {

namespace {

constexpr const char* kPause = "IDD_GAME_OPTIONS_DIALOG";
constexpr const char* kSave = "IDD_SAVE_GAME_DIALOG";
constexpr const char* kLoad = "IDD_LOAD_GAME_DIALOG";
constexpr const char* kConfirm = "IDD_DIALOG_BOX_QUIT_CONFIRM";
constexpr const char* kMainMenu = "IDD_MAIN_MENU_DIALOG";
constexpr const char* kList = "IDC_SAVE_GAME_LIST";
constexpr const char* kEdit = "IDC_SAVE_GAME_NAME_EDIT";
constexpr const char* kMainSave = "smoke_main";
constexpr const char* kExtraSave = "smoke_extra";
constexpr const char* kBattleSave = "smoke_battle";

// What a save looked like when it was written (the loads compare against it).
struct Mark {
    std::string name;
    uint64_t tick = 0;
    CrcRecord crc;
    bool battle = false;
    uintmax_t size = 0;
    std::filesystem::file_time_type mtime{};
    bool set = false;
};
struct Run {
    Mark first, main, extra, battle;
};

void fail(App& a, const std::string& step, const std::string& why) {
    std::fprintf(stderr, "saveload: %s: FAILED: %s\n", step.c_str(), why.c_str());
    a.notes_.push_back("saveload " + step + ": " + why);
    a.smoke_ok_ = false;
}
// A failure the rest of the script cannot build on: the run stops here.
bool stop(App& a, const std::string& step, const std::string& why) {
    fail(a, step, why);
    std::fprintf(stderr, "saveload: dialogs:");
    for (const std::string& n : a.ui_.gui.open_names()) std::fprintf(stderr, " %s", n.c_str());
    std::fprintf(stderr, "\n");
    a.script_failed_ = true;
    a.screen_ = Screen::Quit;
    return true;
}

bool top_is(App& a, std::string_view dialog) {
    const ui::GuiDialog* d = a.ui_.gui.top();
    return d && ascii_iequals(d->name(), dialog);
}
const ui::GuiControl* control(App& a, std::string_view dialog, std::string_view id) {
    ui::GuiDialog* d = a.ui_.gui.find(dialog);
    return d ? d->get(id) : nullptr;
}
// The list row of save `name` in an open Save / Load dialog (labels start with the save name), -1 when not listed.
int32_t list_row(App& a, std::string_view dialog, std::string_view name) {
    const ui::GuiControl* c = control(a, dialog, kList);
    if (!c) return -1;
    const std::u32string n = ui::utf8_to_utf32(name);
    for (size_t i = 0; i < c->items.size(); ++i) {
        const std::u32string& l = c->items[i];
        if (l.compare(0, n.size(), n) == 0 && (l.size() == n.size() || l[n.size()] == U' ')) return static_cast<int32_t>(i);
    }
    return -1;
}
std::string list_text(App& a, std::string_view dialog) {
    const ui::GuiControl* c = control(a, dialog, kList);
    std::string out;
    if (c)
        for (const std::u32string& l : c->items) out += (out.empty() ? "" : " | ") + utf8_of(l);
    return out.empty() ? "(empty)" : out;
}

// The state CRC of the session now (every registered system, as the per-tick CRC record of this tick would hold it).
CrcRecord state_crc(App& a) { return a.session_->host().crc_registry().capture(a.session_->host().frame()); }
std::string crc_diff(App& a, const CrcRecord& want, const CrcRecord& got) {
    std::string out;
    const CrcRegistry& reg = a.session_->host().crc_registry();
    for (size_t i = 0; i < want.systems.size() || i < got.systems.size(); ++i) {
        const uint64_t w = i < want.systems.size() ? want.systems[i] : 0, g = i < got.systems.size() ? got.systems[i] : 0;
        if (w != g) out += (out.empty() ? "" : ", ") + (i < reg.size() ? reg.name(static_cast<CrcRegistry::SystemId>(i)) : "#" + std::to_string(i));
    }
    if (want.parity != got.parity) out += (out.empty() ? "" : ", ") + std::string("parity");
    return out;
}

// One key press per character (a frame with the key down, then an idle frame): the edit box derives the text from the
// key presses as it does for a player's keyboard without WM_CHAR text (letters, digits, '-', '_' = Shift + '-', space).
ScriptStep step_type(std::string text) {
    auto i = std::make_shared<size_t>(0);
    auto down = std::make_shared<bool>(false);
    return {"type \"" + text + "\"", [text, i, down](App&, ui::InputFrame& f) {
                if (*down) {
                    *down = false;
                    ++*i;
                    return false;
                }
                if (*i >= text.size()) return true;
                const char ch = text[*i];
                uint16_t key = ui::vk::kSpace;
                if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z')) key = ui::vk::key(ch);
                if (ch == '_' || ch == '-') key = ui::vk::kOemMinus;
                const bool shift = (ch >= 'A' && ch <= 'Z') || ch == '_';
                if (shift) f.keys_down.set(ui::vk::kShift);
                f.keys_down.set(key);
                f.keys_pressed.set(key);
                *down = true;
                return false;
            },
            600};
}

// A left click on a HUD button (galactic: b_option_g, battle: b_option_t) at the middle of its hit box, once no dialog is
// open (a story dialog is closed by the frame's script driver first).
ScriptStep step_click_hud(std::string component) {
    struct State {
        ClickState click;
        int32_t x = 0, y = 0;
        bool aimed = false;
        uint32_t waited = 0;
    };
    auto st = std::make_shared<State>();
    return {"click the HUD's " + component, [st, component](App& a, ui::InputFrame& f) {
                if (st->aimed) return click_at(st->click, f, st->x, st->y, ui::Button::Left);
                const bool gal = a.screen_ == Screen::Galactic, bat = a.screen_ == Screen::Battle && a.bview_.active();
                if ((!gal && !bat) || a.ui_.gui.top() != nullptr) return false;
                const int32_t ci = a.ui_.components.find(component);
                if (ci < 0) return stop(a, "HUD button", component + " is not a HUD component");
                const std::vector<ui::HudHitBox>& hits = gal ? a.ui_.galactic_hud.hit_boxes() : a.ui_.tactical_hud.hit_boxes();
                const ui::HudCanvas& cv = gal ? a.ui_.galactic_hud.canvas() : a.ui_.tactical_hud.canvas();
                for (const ui::HudHitBox& h : hits)
                    if (h.component == ci) {
                        const ui::Vec2f p = cv.canvas_to_pixel(0.5f * (h.min.x + h.max.x), 0.5f * (h.min.y + h.max.y));
                        st->x = static_cast<int32_t>(p.x);
                        st->y = static_cast<int32_t>(p.y);
                        st->aimed = true;
                        break;
                    }
                if (!st->aimed) {
                    if (++st->waited > 60) return stop(a, "HUD button", component + " has no hit box on the " + screen_name(a.screen_) + " HUD");
                    return false;
                }
                return click_at(st->click, f, st->x, st->y, ui::Button::Left);
            },
            600};
}

// The in-game menu through the HUD's options button. `save`: first wait until the session may be saved (SimHost save
// blockers, e.g. an animation chunk not landed yet), then hold the sim (presentation only) until the menu's own pause.
void open_game_menu(std::vector<ScriptStep>& s, const std::string& button, bool save) {
    if (save) {
        auto last = std::make_shared<std::string>();
        s.push_back(step_wait("the game may be saved", [last](App& a) {
            if (!a.session_) return false;
            const std::string why = a.session_->check_save();
            if (why.empty()) {
                a.script_hold_ = true;
                return true;
            }
            if (why != *last) std::printf("saveload: save not possible yet at tick %" PRIu64 ": %s\n", a.session_->host().frame(), why.c_str());
            *last = why;
            return false;
        }, 6000));
    }
    s.push_back(step_click_hud(button));
    s.push_back(step_wait("in-game menu open, session paused", [](App& a) {
        if (!a.ui_.front.pause_menu_open() || !a.paused_) return false;
        a.script_hold_ = false;  // the menu pauses the session from here on
        return true;
    }, 120));
}

ScriptStep step_dialog_open(std::string dialog) {
    return step_wait(dialog + " open", [dialog](App& a) { return top_is(a, dialog); }, 120);
}

// The Save dialog opened from the in-game menu: Save enabled (no blocker shown in the name field).
void open_save_dialog(std::vector<ScriptStep>& s) {
    s.push_back(step_click_control(kPause, "IDC_SAVE_GAME"));
    s.push_back(step_wait("Save dialog open, Save enabled", [](App& a) {
        if (!top_is(a, kSave)) return false;
        const ui::GuiControl* b = control(a, kSave, "IDC_SAVE_GAME_BUTTON");
        const ui::GuiControl* e = control(a, kSave, kEdit);
        if (!b || !e) return stop(a, "Save dialog", "IDC_SAVE_GAME_BUTTON / IDC_SAVE_GAME_NAME_EDIT missing");
        if (!b->enabled) return stop(a, "Save dialog", "Save is disabled: " + utf8_of(e->text));
        std::printf("saveload: Save dialog lists %s\n", list_text(a, kSave).c_str());
        return true;
    }, 120));
}

// Selects save `name` in the open Save / Load dialog's list (click the first row, Down to it) and checks the selection.
void select_save(std::vector<ScriptStep>& s, const char* dialog, const char* name) {
    s.push_back(step_wait(std::string("list shows ") + name, [dialog, name](App& a) {
        if (list_row(a, dialog, name) >= 0) return true;
        return stop(a, dialog, std::string(name) + " not listed: " + list_text(a, dialog));
    }, 10));
    s.push_back(step_select_row(dialog, kList, [dialog, name](App& a) { return static_cast<size_t>(std::max(0, list_row(a, dialog, name))); }));
    s.push_back(step_wait(std::string(name) + " selected", [dialog, name](App& a) {
        const ui::GuiControl* c = control(a, dialog, kList);
        if (!c || c->selected != list_row(a, dialog, name)) return stop(a, dialog, std::string("selection is not on ") + name);
        if (ascii_iequals(dialog, kSave)) {  // the Save dialog puts the selected save's name into the name field
            const ui::GuiControl* e = control(a, kSave, kEdit);
            if (!e || e->text != ui::utf8_to_utf32(name)) fail(a, "Save dialog", std::string("selecting ") + name + " did not put its name in the name field");
        }
        return true;
    }, 10));
}

// After Save / Yes: the dialog closed on the in-game menu, `written` saves so far; the file holds this tick; the mark.
ScriptStep step_saved(const char* name, uint64_t written, Mark* m) {
    auto n = std::make_shared<uint32_t>(0);
    return {std::string("saved ") + name, [name, written, m, n](App& a, ui::InputFrame&) {
                if (a.saves_written_ < written || a.ui_.gui.find(kSave)) {
                    if (++*n > 90) return stop(a, std::string("save ") + name, "no save written (saves written " + std::to_string(a.saves_written_) + ")");
                    return false;
                }
                if (a.saves_written_ > written) fail(a, std::string("save ") + name, "more saves written than requested");
                if (!top_is(a, kPause)) fail(a, std::string("save ") + name, "the in-game menu is not back on top");
                const std::string path = save_path(a.saves_dir_, name);
                uint64_t tick = 0;
                std::string desc, err;
                if (!read_save_header(path, tick, desc, &err)) return stop(a, std::string("save ") + name, path + ": " + err);
                m->name = name;
                m->tick = a.session_->host().frame();
                m->crc = state_crc(a);
                m->battle = a.session_->in_battle();
                std::error_code ec;
                m->size = std::filesystem::file_size(path, ec);
                m->mtime = std::filesystem::last_write_time(path, ec);
                m->set = true;
                if (tick != m->tick) fail(a, std::string("save ") + name, "the file holds tick " + std::to_string(tick) + ", the session is at " + std::to_string(m->tick));
                const CrcLog& log = a.session_->host().crc_log();
                if (!log.records.empty() && log.records.back().tick == m->tick && log.records.back().combined != m->crc.combined)
                    std::printf("saveload: note: state CRC now %016" PRIx64 " vs the tick's CRC record %016" PRIx64 "\n", m->crc.combined, log.records.back().combined);
                std::printf("saveload: saved %s at tick %" PRIu64 "%s, %" PRIuMAX " bytes, state CRC %016" PRIx64 "\n", name, m->tick, m->battle ? " (in battle)" : "",
                            m->size, m->crc.combined);
                return true;
            },
            200};
}

// The Save dialog from the in-game menu: the name field clicked, `name` typed, Save.
void save_typed(std::vector<ScriptStep>& s, const char* name, uint64_t written, Mark* m, const std::string& shot) {
    open_save_dialog(s);
    s.push_back(step_click_control(kSave, kEdit));
    s.push_back(step_type(name));
    s.push_back(step_wait("name typed", [name](App& a) {
        const ui::GuiControl* e = control(a, kSave, kEdit);
        if (!e || e->text != ui::utf8_to_utf32(name)) return stop(a, "typing", std::string("the name field holds \"") + (e ? utf8_of(e->text) : "") + "\", not " + name);
        return true;
    }, 10));
    if (!shot.empty()) s.push_back(step_shot(shot));
    s.push_back(step_click_control(kSave, "IDC_SAVE_GAME_BUTTON"));
    s.push_back(step_saved(name, written, m));
}

// The overwrite / delete question (IDD_DIALOG_BOX_QUIT_CONFIRM) naming `name`; nothing written or deleted yet.
ScriptStep step_question(std::string what, const char* name, uint64_t written) {
    auto n = std::make_shared<uint32_t>(0);
    return {what + " question shown", [what, name, written, n](App& a, ui::InputFrame&) {
                if (!top_is(a, kConfirm)) {
                    if (++*n > 60) return stop(a, what, "no question dialog came up");
                    return false;
                }
                const ui::GuiControl* cap = control(a, kConfirm, "IDC_CAPTION");
                const ui::GuiControl* t = control(a, kConfirm, "IDC_USER_TEXT_STRING_1");
                std::printf("saveload: %s question: \"%s\" / \"%s\"\n", what.c_str(), cap ? utf8_of(cap->text).c_str() : "-", t ? utf8_of(t->text).c_str() : "-");
                if (!t || !t->visible || t->text != ui::utf8_to_utf32(name)) fail(a, what, std::string("the question does not name ") + name);
                if (!cap || cap->text.empty()) fail(a, what, "the question has no caption");
                if (a.saves_written_ != written) fail(a, what, "a save was written before the answer");
                if (!save_exists(a.saves_dir_, name)) fail(a, what, std::string(name) + " is gone before the answer");
                return true;
            },
            100};
}

// After a load through a menu: `loads` loads so far, the session at the save's tick with the save's state CRC (checked
// before any tick: the sim is held from the click on), in the battle when the save was; the menus closed.
ScriptStep step_loaded(std::string where, uint64_t loads, const Mark* m) {
    auto n = std::make_shared<uint32_t>(0);
    return {"load checked (" + where + ")", [where, loads, m, n](App& a, ui::InputFrame&) {
                const std::string step = "load from the " + where;
                if (a.saves_loaded_ < loads) {
                    if (++*n > 90) return stop(a, step, "nothing loaded (loads " + std::to_string(a.saves_loaded_) + ")");
                    return false;
                }
                a.script_hold_ = false;
                if (!m->set) return stop(a, step, "no save mark");
                if (!a.session_ || !a.session_->loaded()) return stop(a, step, "no loaded session");
                const uint64_t tick = a.session_->host().frame();
                const CrcRecord crc = state_crc(a);
                const bool battle = a.session_->in_battle();
                std::printf("saveload: loaded %s from the %s: tick %" PRIu64 " (saved %" PRIu64 "), state CRC %016" PRIx64 " (saved %016" PRIx64 ")%s, screen %s\n",
                            m->name.c_str(), where.c_str(), tick, m->tick, crc.combined, m->crc.combined, battle ? ", in battle" : "", screen_name(a.screen_));
                if (a.loaded_save_ != save_path(a.saves_dir_, m->name)) fail(a, step, "loaded " + a.loaded_save_ + ", not " + m->name);
                if (tick != m->tick || a.session_->load_point().tick != m->tick) fail(a, step, "tick " + std::to_string(tick) + " != saved " + std::to_string(m->tick));
                if (crc.combined != m->crc.combined || crc.parity != m->crc.parity) fail(a, step, "state CRC differs from the save's: " + crc_diff(a, m->crc, crc));
                if (battle != m->battle) fail(a, step, battle ? "in a battle the save was not in" : "not in the saved battle");
                if (!m->battle && a.screen_ != Screen::Galactic) fail(a, step, std::string("screen ") + screen_name(a.screen_) + ", not the galactic map");
                // A battle save opens its battle at once (the load screen, no tick on the galactic map first).
                if (m->battle && a.screen_ != Screen::BattleLoad) fail(a, step, std::string("screen ") + screen_name(a.screen_) + ", not the battle's load screen");
                if (a.session_->ticks() != 0) fail(a, step, "the session ticked before the check");
                if (a.menu_dialog_open() || a.paused_) fail(a, step, "the menus are still open / the session is paused");
                return true;
            },
            200};
}

// The load step itself: the sim is held (presentation only) from the click to the check, so the loaded state is
// compared before its first tick.
void load_selected(std::vector<ScriptStep>& s, const char* dialog, const std::string& where, uint64_t loads, const Mark* m) {
    s.push_back(step_wait("hold the sim for the load check", [](App& a) { return a.script_hold_ = true; }, 10));
    s.push_back(step_click_control(dialog, "IDC_LOAD_GAME_BUTTON"));
    s.push_back(step_loaded(where, loads, m));
}

ScriptStep step_begin_battle() {
    auto c = std::make_shared<ClickState>();
    return {"Begin on the battle load screen", [c](App& a, ui::InputFrame& f) {
                if (a.screen_ != Screen::BattleLoad) return true;
                ui::GuiDialog* d = a.ui_.gui.find(a.ui_.load.dialog());
                const int32_t i = d ? d->find("IDC_BEGIN_BUTTON") : -1;
                if (i < 0) return false;
                const ui::GuiRect r = d->control_rect(static_cast<size_t>(i));
                const ui::Vec2f p = a.ui_.gui.canvas().canvas_to_pixel(0.5f * (r.min.x + r.max.x), 0.5f * (r.min.y + r.max.y));
                if (click_at(*c, f, static_cast<int32_t>(p.x), static_cast<int32_t>(p.y), ui::Button::Left)) *c = {};
                return false;
            },
            600};
}

}  // namespace

void add_saveload_steps(std::vector<ScriptStep>& s) {
    auto run = std::make_shared<Run>();
    const std::string png = "game_smoke_saveload_";
    // Setup: this run's save names are not in the folder (a re-run).
    s.push_back(step_wait("saves folder ready", [](App& a) {
        if (a.saves_dir_.empty()) return stop(a, "setup", "no saves folder");
        for (const char* n : {kMainSave, kExtraSave, kBattleSave}) {
            std::string err;
            if (save_exists(a.saves_dir_, n) && !delete_save(a.saves_dir_, n, &err)) return stop(a, "setup", "cannot remove the old " + std::string(n) + ": " + err);
        }
        return true;
    }, 10));
    // ---- 1. a custom campaign, the in-game menu, Save "smoke_main" ----
    s.push_back(step_click_control(kMainMenu, "IDC_MAIN_MENU_SINGLE_PLAYER_GAMES"));
    s.push_back(step_click_control("IDD_SINGLE_PLAYER_GAMES_MENU_DIALOG", "IDC_CUSTOM_CAMPAIGN_BUTTON"));
    s.push_back(step_click_control("IDD_CUSTOM_CAMPAIGN_STAGING_DIALOG", "IDC_PLAY_BUTTON"));
    s.push_back(step_wait("galactic map", [](App& a) { return a.screen_ == Screen::Galactic; }, 600));
    s.push_back(step_ticks("galaxy runs", 30));
    open_game_menu(s, "b_option_g", true);
    s.push_back(step_shot(png + "game_menu.png"));
    save_typed(s, kMainSave, 1, &run->first, png + "save_typed.png");
    s.push_back(step_click_control(kPause, "IDC_GAME_OPTIONS_RESUME"));
    s.push_back(step_wait("menu closed, session runs", [](App& a) { return !a.menu_dialog_open() && !a.paused_; }, 120));
    s.push_back(step_ticks("galaxy runs on", 30));
    // ---- 2. Save on the same name: the overwrite question, Yes replaces the file ----
    open_game_menu(s, "b_option_g", true);
    open_save_dialog(s);
    select_save(s, kSave, kMainSave);
    s.push_back(step_click_control(kSave, "IDC_SAVE_GAME_BUTTON"));
    s.push_back(step_question("overwrite", kMainSave, 1));
    s.push_back(step_shot(png + "overwrite_prompt.png"));
    s.push_back(step_click_control(kConfirm, "IDC_DIALOG_BOX_BUTTON_LEFT"));
    s.push_back(step_saved(kMainSave, 2, &run->main));
    s.push_back(step_wait("smoke_main replaced", [run](App& a) {
        const Mark &o = run->first, &n = run->main;
        std::printf("saveload: overwrite: tick %" PRIu64 " -> %" PRIu64 ", %" PRIuMAX " -> %" PRIuMAX " bytes, write time %s\n", o.tick, n.tick, o.size, n.size,
                    n.mtime > o.mtime ? "later" : n.mtime == o.mtime ? "unchanged" : "earlier");
        if (n.tick <= o.tick) fail(a, "overwrite", "the file still holds the first save's tick");
        if (n.mtime < o.mtime) fail(a, "overwrite", "the file's write time went back");
        if (list_saves(a.saves_dir_).size() != 1) fail(a, "overwrite", "the folder does not hold exactly one save");
        return true;
    }, 10));
    // ---- 3. a second save; Delete it: No keeps it, Yes removes it ----
    save_typed(s, kExtraSave, 3, &run->extra, "");
    open_save_dialog(s);
    select_save(s, kSave, kExtraSave);
    s.push_back(step_click_control(kSave, "IDC_DELETE_GAME_BUTTON"));
    s.push_back(step_question("delete", kExtraSave, 3));
    s.push_back(step_shot(png + "delete_prompt.png"));
    s.push_back(step_click_control(kConfirm, "IDC_DIALOG_BOX_BUTTON_RIGHT"));
    s.push_back(step_wait("delete answered No: the save stays", [](App& a) {
        if (!top_is(a, kSave)) return false;
        if (!save_exists(a.saves_dir_, kExtraSave)) fail(a, "delete No", "smoke_extra was deleted");
        if (list_row(a, kSave, kExtraSave) < 0) fail(a, "delete No", "smoke_extra left the list");
        return true;
    }, 120));
    select_save(s, kSave, kExtraSave);
    s.push_back(step_click_control(kSave, "IDC_DELETE_GAME_BUTTON"));
    s.push_back(step_question("delete", kExtraSave, 3));
    s.push_back(step_click_control(kConfirm, "IDC_DIALOG_BOX_BUTTON_LEFT"));
    s.push_back(step_wait("delete answered Yes: the save is gone", [](App& a) {
        if (!top_is(a, kSave) || save_exists(a.saves_dir_, kExtraSave)) return false;
        std::printf("saveload: after the delete the Save dialog lists %s\n", list_text(a, kSave).c_str());
        if (list_row(a, kSave, kExtraSave) >= 0) fail(a, "delete Yes", "smoke_extra is still listed");
        if (list_row(a, kSave, kMainSave) < 0 || !save_exists(a.saves_dir_, kMainSave)) fail(a, "delete Yes", "smoke_main went too");
        return true;
    }, 120));
    s.push_back(step_shot(png + "after_delete.png"));
    s.push_back(step_click_control(kSave, "IDC_SAVE_CANCEL_BUTTON"));
    s.push_back(step_dialog_open(kPause));
    // ---- 4. Load from the in-game menu, then from the main menu ----
    s.push_back(step_click_control(kPause, "IDC_LOAD_GAME"));
    s.push_back(step_dialog_open(kLoad));
    select_save(s, kLoad, kMainSave);
    s.push_back(step_shot(png + "load_in_game.png"));
    load_selected(s, kLoad, "in-game menu", 1, &run->main);
    s.push_back(step_shot(png + "loaded_in_game.png"));
    s.push_back(step_ticks("the loaded galaxy runs", 30));
    open_game_menu(s, "b_option_g", false);
    s.push_back(step_click_control(kPause, "IDC_GAME_OPTIONS_QUIT"));
    s.push_back(step_dialog_open(kConfirm));
    s.push_back(step_click_control(kConfirm, "IDC_DIALOG_BOX_BUTTON_LEFT"));
    s.push_back(step_wait("back on the main menu", [](App& a) { return a.screen_ == Screen::Menu && !a.session_ && top_is(a, kMainMenu); }, 300));
    s.push_back(step_click_control(kMainMenu, "IDC_LOAD_GAME_MAIN"));
    s.push_back(step_dialog_open(kLoad));
    select_save(s, kLoad, kMainSave);
    s.push_back(step_shot(png + "load_main_menu.png"));
    load_selected(s, kLoad, "main menu", 2, &run->main);
    s.push_back(step_shot(png + "loaded_main_menu.png"));
    // ---- 5. a battle: Save during it, Load it, the battle resumes ----
    s.push_back(step_ticks("the loaded galaxy runs", 30));
    s.push_back(step_galactic_attack());
    s.push_back(step_wait("battle prompt shown", [](App& a) { return a.choice_open_; }, 6000));
    s.push_back(step_wait("Fight", [](App& a) {
        ui::HudAction act;
        act.kind = ui::HudAction::Kind::BattleBegin;
        a.handle_hud_actions({act});
        return !a.choice_open_;
    }, 10));
    s.push_back(step_wait("load screen ready", [](App& a) { return a.screen_ == Screen::BattleLoad && a.ui_.load.ready(); }, 6000));
    s.push_back(step_begin_battle());
    s.push_back(step_wait("in the battle", [](App& a) { return a.screen_ == Screen::Battle && a.bview_.active(); }, 600));
    s.push_back(step_ticks("battle runs", 60));
    open_game_menu(s, "b_option_t", true);
    save_typed(s, kBattleSave, 4, &run->battle, png + "battle_save.png");
    s.push_back(step_wait("saved in the battle", [run](App& a) {
        if (!run->battle.battle) fail(a, "battle save", "the session was not in a battle when it saved");
        return true;
    }, 10));
    s.push_back(step_click_control(kPause, "IDC_LOAD_GAME"));
    s.push_back(step_dialog_open(kLoad));
    select_save(s, kLoad, kBattleSave);
    load_selected(s, kLoad, "in-game menu (battle save)", 3, &run->battle);
    s.push_back(step_wait("the battle's load screen, no tick yet", [run](App& a) {
        if (a.screen_ != Screen::BattleLoad) return stop(a, "battle load", std::string("screen ") + screen_name(a.screen_) + " before the load screen was ready");
        if (!a.ui_.load.ready()) return false;
        if (a.session_->ticks() != 0 || a.session_->host().frame() != run->battle.tick)
            fail(a, "battle load", "ticked before Begin (" + std::to_string(a.session_->ticks()) + " ticks, frame " + std::to_string(a.session_->host().frame()) + ")");
        return true;
    }, 600));
    s.push_back(step_shot(png + "battle_load_screen.png"));
    s.push_back(step_begin_battle());
    s.push_back(step_wait("back in the battle", [](App& a) { return a.screen_ == Screen::Battle && a.bview_.active(); }, 600));
    s.push_back(step_wait("the first tick after the load is a battle tick", [run](App& a) {
        if (a.session_->ticks() == 0) return false;
        std::printf("saveload: first tick after the battle load: tick %" PRIu64 " on the %s screen\n", a.session_->host().frame(), screen_name(a.screen_));
        if (a.screen_ != Screen::Battle || !a.session_->in_battle() || a.session_->host().frame() <= run->battle.tick)
            fail(a, "battle load", "the first tick after the load did not run in the battle");
        return true;
    }, 120));
    s.push_back(step_ticks("the loaded battle runs", 30));
    s.push_back(step_wait("the battle resumed", [run](App& a) {
        const bool in = a.session_ && a.session_->in_battle() && a.screen_ == Screen::Battle;
        std::printf("saveload: battle resumed: %s at tick %" PRIu64 " (saved at %" PRIu64 ")\n", in ? "in the battle" : "NOT in the battle",
                    a.session_ ? a.session_->host().frame() : 0, run->battle.tick);
        if (!in) fail(a, "battle load", "the loaded battle did not run on");
        return true;
    }, 10));
    s.push_back(step_shot(png + "battle_resumed.png"));
}

}  // namespace alamo_game_app::script
