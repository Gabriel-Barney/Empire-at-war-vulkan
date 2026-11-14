// alamo_game App: ui3 t01 - t04 - the tactical hotkeys and command-bar buttons beyond the camera keys and the move toggles
// (app_input.cpp): order buttons c_button00 / 02 / 04, ability hotkeys, selection keys, retreat / reinforcement / beacon /
// movie-mode buttons, the reinforcement pad's availability and the drop-point preview's state.
//
// Retail references: HUD_GAPS §5.1 (who enables the buttons), BATTLE_TRANSITIONS §3.4 (retreat), TACTICAL_GAPS §4.2.1
// (the human drop preview 0x345c0), UI.md §4.3 (slots). What a key does in the hotkey dispatch 0x63f00 is not traced
// for most slots: those are [port] choices, listed in docs/PHASE4.md ("ui3 tactical hotkeys").
#include <filesystem>

#include "alamo/tactical/abilities/ability_types.hpp"
#include "game_app.hpp"

namespace alamo_game_app {

bool App::reinforce_available(objects::ObjectId galactic_id) const {
    const sim::TacticalBattle* b = battle();
    return b && !story_controls_locked() && b->reinforce_block(local_player(), galactic_id) == commands::RejectReason::None;
}

// R / b_reinforcement [port]: the first callable entry of the pad (pad order = TacticalBattle::reserves) starts its
// drop pick, as a click on that pad entry; a second press while a drop point is being picked cancels it.
bool App::begin_first_reinforcement() {
    if (reinforce_unit_ != 0) {
        reinforce_unit_ = 0;
        reinforce_blob_.reset();
        return true;
    }
    const sim::TacticalBattle* b = battle();
    if (!b) return false;
    for (const sim::TacticalBattle::Reserve& r : b->reserves(local_player()))
        if (reinforce_available(r.galactic_id)) {
            ui::HudAction a;
            a.kind = ui::HudAction::Kind::BeginReinforcement;
            a.unit = static_cast<uint32_t>(r.galactic_id);
            handle_hud_actions({a});
            return true;
        }
    audio_.negative_feedback();
    return false;
}

bool App::cancel_picks() {
    bool any = false;
    if (reinforce_unit_ != 0) {
        reinforce_unit_ = 0;
        reinforce_blob_.reset();
        any = true;
    }
    if (bombing_targeting_) {
        bombing_end(true);
        ++bombing_cancels_;
        any = true;
    }
    if (beacon_pick_) {
        beacon_pick_ = false;
        any = true;
    }
    return any;
}

// NextUnit / PreviousUnit [port]: the local player's movable units (one per squadron, TacticalBattle::icon_links, ascending
// id) in turn: the one after (before) the unit selected last, wrapping; it is selected and the camera centres on it.
void App::select_cycle(int32_t step) {
    const sim::TacticalBattle* b = battle();
    if (!b || !bview_.active()) return;
    const std::vector<render::SnapshotObject>& objs = bview_.frame().objects;  // ascending id
    std::vector<const render::SnapshotObject*> mine;
    for (const sim::TacticalBattle::IconLink& l : b->icon_links()) {
        const auto it = std::lower_bound(objs.begin(), objs.end(), l.unit,
                                         [](const render::SnapshotObject& x, objects::ObjectId v) { return x.id < v; });
        if (it != objs.end() && it->id == l.unit && it->owner == local_player()) mine.push_back(&*it);
    }
    if (mine.empty()) return;
    const uint32_t anchor = cycle_unit_ != 0 ? cycle_unit_ : orders_.selection().empty() ? 0 : orders_.selection().front();
    size_t pick = step > 0 ? 0 : mine.size() - 1;
    if (step > 0) {
        for (size_t i = 0; i < mine.size(); ++i)
            if (static_cast<uint32_t>(mine[i]->id) > anchor) {
                pick = i;
                break;
            }
    } else {
        for (size_t i = mine.size(); i-- > 0;)
            if (static_cast<uint32_t>(mine[i]->id) < anchor) {
                pick = i;
                break;
            }
    }
    const render::SnapshotObject& o = *mine[pick];
    cycle_unit_ = static_cast<uint32_t>(o.id);
    submit(commands::ev::select({static_cast<uint32_t>(o.id)}));
    bview_.camera().jump_to({o.position.x, o.position.y, o.position.z});
}

// SelectLike [port]: every unit of the local player on screen whose type is the type of a selected own unit.
void App::select_like() {
    if (!bview_.active() || orders_.selection().empty()) return;
    const std::vector<render::SnapshotObject>& objs = bview_.frame().objects;
    std::vector<objects::TypeIndex> types;
    for (const uint32_t id : orders_.selection())
        for (const render::SnapshotObject& o : objs)
            if (static_cast<uint32_t>(o.id) == id && o.owner == local_player()) types.push_back(o.type);
    if (types.empty()) return;
    std::sort(types.begin(), types.end());
    const uint32_t w = targets_.width, h = targets_.height;
    const ui::View view = ui::make_view(bview_.camera_params(w, h), w, h);
    std::vector<uint32_t> ids;
    for (const render::SnapshotObject& o : objs) {
        if (o.owner != local_player() || !std::binary_search(types.begin(), types.end(), o.type)) continue;
        float sx = 0.0f, sy = 0.0f;
        if (!ui::project(view, {o.position.x, o.position.y, o.position.z}, sx, sy)) continue;
        if (sx < 0.0f || sy < 0.0f || sx >= static_cast<float>(w) || sy >= static_cast<float>(h)) continue;
        ids.push_back(static_cast<uint32_t>(o.id));
        if (ids.size() >= ui::OrderTranslator::kMaxSelect) break;
    }
    if (!ids.empty()) submit(commands::ev::select(ids));
}

// [port: Gabriel, 2026-11-13] Squadron wings: every own squadron (icon_links: unit -> its squadron's lead) whose member type
// is listed in some Squadron_Units joins the bomber wing when the member's CategoryMask has Bomber, else the fighter wing
// when it has Fighter. A wing's ids hold whole squadrons up to the selection cap (OrderTranslator::kMaxSelect); its count
// is every squadron. Selected look = every id of the wing selected.
void App::fill_wings(ui::TacticalHudFrame& hf) {
    wings_.assign(2, ui::HudWing{});
    wings_[0].label = U"FIGHTERS";
    wings_[0].hotkey = U"F2";
    wings_[1].label = U"BOMBERS";
    wings_[1].hotkey = U"F3";
    const sim::TacticalBattle* b = battle();
    if (b && bview_.active()) {
        const std::vector<render::SnapshotObject>& objs = bview_.frame().objects;  // ascending id
        const objects::EnumConverter& cat = data_.types.enums().get(objects::EnumId::GameObjectCategory);
        const auto fighter = static_cast<uint64_t>(cat.lookup("Fighter").value_or(0x1));
        const auto bomber = static_cast<uint64_t>(cat.lookup("Bomber").value_or(0x2));
        struct Squad {
            objects::ObjectId lead = objects::kNoObjectId;
            size_t wing = 0;
            std::vector<uint32_t> ids;
        };
        std::vector<Squad> squads;  // first-seen order (ascending member id)
        for (const sim::TacticalBattle::IconLink& l : b->icon_links()) {
            const auto it = std::lower_bound(objs.begin(), objs.end(), l.unit,
                                             [](const render::SnapshotObject& x, objects::ObjectId v) { return x.id < v; });
            if (it == objs.end() || it->id != l.unit || it->owner != local_player()) continue;
            const objects::TypeIndex t = it->type;
            if (t >= data_.types.size() || !data_.types.squadron_member(t)) continue;
            const uint64_t mask = data_.types.get_mask(t, "CategoryMask");
            const size_t wing = (mask & bomber) != 0 ? 1u : (mask & fighter) != 0 ? 0u : 2u;
            if (wing > 1) continue;
            Squad* sq = nullptr;
            for (Squad& s : squads)
                if (s.lead == l.squad) sq = &s;
            if (!sq) {
                squads.push_back(Squad{l.squad, wing, {}});
                sq = &squads.back();
            }
            sq->ids.push_back(static_cast<uint32_t>(l.unit));
        }
        for (const Squad& s : squads) {
            ui::HudWing& w = wings_[s.wing];
            ++w.squadrons;
            if (w.ids.size() + s.ids.size() <= ui::OrderTranslator::kMaxSelect) w.ids.insert(w.ids.end(), s.ids.begin(), s.ids.end());
        }
        std::vector<uint32_t> sel = orders_.selection();
        std::sort(sel.begin(), sel.end());
        for (ui::HudWing& w : wings_) {
            std::sort(w.ids.begin(), w.ids.end());
            w.selected = !w.ids.empty() && std::includes(sel.begin(), sel.end(), w.ids.begin(), w.ids.end());
        }
    }
    hf.wings = wings_;
}

// A wing button / F2 / F3: select the wing (`add`: the selection plus the wing); a second press of the same wing within
// 0.4 s [port], or `centre_only`, centres the camera on the wing's members.
void App::select_wing(size_t wing, bool add, bool centre_only) {
    if (screen_ != Screen::Battle || !bview_.active() || wing >= wings_.size() || wings_[wing].ids.empty()) return;
    const std::vector<uint32_t>& members = wings_[wing].ids;
    const bool again = wing_last_ == static_cast<int32_t>(wing) && now_s_ - wing_last_s_ <= 0.4;
    wing_last_ = static_cast<int32_t>(wing);
    wing_last_s_ = now_s_;
    if (!centre_only) {
        std::vector<uint32_t> ids = members;
        if (add) {
            const std::vector<uint32_t> sel = orders_.selection();
            ids.insert(ids.end(), sel.begin(), sel.end());
            std::sort(ids.begin(), ids.end());
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        }
        if (ids.size() > ui::OrderTranslator::kMaxSelect) ids.resize(ui::OrderTranslator::kMaxSelect);
        submit(commands::ev::select(ids));
    }
    if (again || centre_only) {
        double x = 0.0, y = 0.0, z = 0.0;
        size_t n = 0;
        for (const render::SnapshotObject& o : bview_.frame().objects)
            if (std::binary_search(members.begin(), members.end(), static_cast<uint32_t>(o.id))) {
                x += o.position.x;
                y += o.position.y;
                z += o.position.z;
                ++n;
            }
        if (n > 0) {
            const double k = 1.0 / static_cast<double>(n);
            bview_.camera().jump_to({static_cast<float>(x * k), static_cast<float>(y * k), static_cast<float>(z * k)});
        }
    }
}

// QuickSave (F6) / QuickLoad (F5) / Screenshot (F12), galactic and tactical [port]: the save slot "QuickSave" through the
// save / load dialogs' path; the screenshot is a PNG of the next presented frame in a Screenshots folder beside the saves.
void App::interface_hotkey(uint16_t slot) {
    if (slot == ui::hk::kQuickSave) {
        save_game_as("QuickSave");
    } else if (slot == ui::hk::kQuickLoad) {
        std::error_code ec;
        if (!saves_dir_.empty() && std::filesystem::exists(save_path(saves_dir_, "QuickSave"), ec)) load_saved_game("QuickSave");
        else audio_.negative_feedback();
    } else if (slot == ui::hk::kScreenshot && shot_path_.empty() && !saves_dir_.empty()) {
        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::path(saves_dir_).parent_path() / "Screenshots";
        std::filesystem::create_directories(dir, ec);
        for (uint32_t i = 0; i < 10000; ++i) {
            char name[32];
            std::snprintf(name, sizeof name, "ScreenShot%04u.png", i);
            if (!std::filesystem::exists(dir / name, ec)) {
                shot_path_ = (dir / name).string();
                break;
            }
        }
    }
}

void App::tactical_hotkeys(const std::vector<ui::HotkeyEvent>& hk, std::vector<commands::EventData>& cmds,
                           std::vector<ui::HudAction>& acts) {
    namespace hk_ = ui::hk;
    for (const ui::HotkeyEvent& e : hk) {
        if (e.kind == ui::HotkeyKind::Ability || e.slot == hk_::kDeploySquad) {
            // t02: the ability button of that type in the selection panel, as a left click (targeted abilities start
            // their pick). The slot's modifier is part of the key, so it is never the queue modifier here.
            const std::string_view name = ui::hotkey_ability_name(e.slot);
            const uint8_t type = name.empty() ? uint8_t{0} : tactical::abilities::ability_type_from_name(name);
            if (type != 0 && !story_controls_locked() && ui_.tactical_hud.activate_ability(type, false, cmds, &acts)) ++hotkey_abilities_;
            continue;
        }
        switch (e.slot) {
            case hk_::kQuickSave:
            case hk_::kQuickLoad:
            case hk_::kScreenshot: interface_hotkey(e.slot); break;
            case hk_::kAttack:  // t01: as c_button00 (only while it is enabled)
                if (ui_.tactical_hud.order_button_live(0)) orders_.arm_move_toggle(ui::MoveToggle::Attack);
                break;
            case hk_::kMove:  // as c_button02
                if (ui_.tactical_hud.order_button_live(2)) orders_.arm_move_toggle(ui::MoveToggle::Move);
                break;
            case hk_::kSelectLike: select_like(); break;
            case hk_::kNextUnit: select_cycle(1); break;
            case hk_::kPrevUnit: select_cycle(-1); break;
            case hk_::kReinforce: begin_first_reinforcement(); break;
            case hk_::kBeacon: {
                ui::HudAction a;
                a.name = "b_special_weapon2";
                tactical_button(a);
                break;
            }
            case hk_::kCameraTether: camera_tether_ = !camera_tether_; break;
            case hk_::kMovieMode: movie_mode_ = !movie_mode_; break;
            case hk_::kToggleInterface: interface_hidden_ = !interface_hidden_; break;
            case hk_::kHolocron: objectives_hidden_ = !objectives_hidden_; break;  // [port] the battle's holocron = the objectives box
            // CinematicView, taunts, chat, give credits, player list: multiplayer / not traced - no action (docs/PHASE4.md).
            default: break;
        }
    }
}

// Command-bar buttons with no HudAction kind of their own (HudAction::Kind::Button by name). True = handled.
bool App::tactical_button(const ui::HudAction& a) {
    if (screen_ != Screen::Battle) return false;
    const std::string& n = a.name;
    if (ascii_iequals(n, "c_button00")) {  // t01 attack: the next right click attacks (an object) / attack-moves (ground)
        orders_.arm_move_toggle(ui::MoveToggle::Attack);
        return true;
    }
    if (ascii_iequals(n, "c_button02")) {  // move: the next right click is a plain ground move
        orders_.arm_move_toggle(ui::MoveToggle::Move);
        return true;
    }
    if (ascii_iequals(n, "c_button04")) {  // full stop: StopMovement for the selection (as the Stop key)
        if (!orders_.selection().empty()) submit(commands::ev::stop());
        return true;
    }
    if (ascii_iequals(n, "b_reinforcement")) {  // t03 [port]: as the Reinforce key
        begin_first_reinforcement();
        return true;
    }
    if (ascii_iequals(n, "b_camera_t")) {  // TEXT_MOVIE_MODE
        movie_mode_ = !movie_mode_;
        return true;
    }
    if (ascii_iequals(n, "b_special_weapon2") || ascii_iequals(n, "b_beacon_t")) {  // TEXT_PLACE_BEACON
        if (beacon_pick_) {
            beacon_pick_ = false;
        } else if (now_s_ - beacon_last_s_ >= static_cast<double>(beacon_delay_s_) && !story_controls_locked()) {
            cancel_picks();
            targeting_.cancel();
            beacon_pick_ = true;
        } else {
            audio_.negative_feedback();
        }
        return true;
    }
    if (ascii_iequals(n, "b_droid_help_tactical")) {  // the advisor dome, as the Droid Advisor key (capture C2)
        open_holocron(ui::InfoPage::DroidLog);
        return true;
    }
    return false;
}

void App::fill_command_buttons(ui::TacticalHudFrame& hf) const {
    hf.world.interface_hidden = interface_hidden_ || movie_mode_;
    hf.objectives_hidden = objectives_hidden_;
    const sim::TacticalBattle* b = battle();
    if (!b) return;
    ui::HudCommandButtons& c = hf.command_buttons;
    c.present = true;
    c.controls_locked = story_controls_locked();
    const int32_t me = local_player();
    c.retreat_enabled = b->retreat_block(me) == 0;
    const float running = b->retreat_countdown_progress();
    const float allowed = b->retreat_allowed_progress();
    c.retreat_dial = running >= 0.0f ? running : allowed < 1.0f ? allowed : -1.0f;
    for (const sim::TacticalBattle::Reserve& r : b->reserves(me)) {
        ++c.reinforce_count;
        if (reinforce_available(r.galactic_id)) c.reinforce_enabled = true;
    }
    c.reinforce_selected = reinforce_unit_ != 0;
    c.beacon_enabled = now_s_ - beacon_last_s_ >= static_cast<double>(beacon_delay_s_);
    c.beacon_selected = beacon_pick_;
    c.movie_mode = movie_mode_;
    const float cap = b->population_cap(me);
    if (cap >= 0.0f) {
        c.pop_used = static_cast<int32_t>(std::lround(b->population_used(me)));
        c.pop_cap = static_cast<int32_t>(std::lround(cap));
    }
}

}  // namespace alamo_game_app
