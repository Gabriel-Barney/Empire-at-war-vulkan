// alamo_game host, ui1 t02: the options kept between runs (game_display.hpp OptionsProfile) and the gameplay options the
// sim host applies - alternate mouse controls, the tactical / galactic game speed and the abilities' default autofire.
#include "game_app.hpp"

namespace alamo_game_app {

void App::load_game_options() {
    // Live play only: smoke, replay and benchmark runs keep the defaults (machine and profile independent).
    options_path_ = live_input() && !bench_active() ? default_options_profile_path() : std::string();
    OptionsProfile p;
    p.audio = audio_opts_;
    p.gameplay = gameplay_;
    p.camera = camera_;
    p.tactical_fov_set = tactical_fov_set_;
    if (!load_options_profile(options_path_, p)) return;
    std::printf("options: %s\n", options_path_.c_str());
    audio_opts_ = p.audio;
    audio_.set_volumes(audio_opts_.master, audio_opts_.sfx, audio_opts_.speech, audio_opts_.music);
    gameplay_ = p.gameplay;
    camera_ = p.camera;
    tactical_fov_set_ = p.tactical_fov_set;
    gview_.camera().set_fov_deg(camera_.galactic_fov_deg);
    bview_.camera().set_fov_override(tactical_fov_set_ ? camera_.tactical_fov_deg : 0.0f);
    if (!p.keys.empty()) ui_.hotkeys.load(p.keys);
    sync_control_bindings(ui_.hotkeys, camera_, ui_.bindings);
    apply_gameplay_options();
}

void App::save_game_options() {
    if (options_path_.empty()) return;
    OptionsProfile p;
    p.audio = audio_opts_;
    p.gameplay = gameplay_;
    p.camera = camera_;
    p.tactical_fov_set = tactical_fov_set_;
    ui_.hotkeys.save(p.keys);
    if (!save_options_profile(options_path_, p)) std::printf("options: %s not written\n", options_path_.c_str());
}

void App::apply_gameplay_options() {
    // IDC_ALTERNATE_MOUSE_CONTROLS (P1_DOUBLE_CLICK_MOVE.md §1.2): ground orders on the left button.
    orders_.set_alternate_mouse(gameplay_.alternate_mouse);
    // IDC_GAME_SPEED_SLIDER / IDC_STRATEGIC_GAME_SPEED_SLIDER (MAIN_LOOP.md §1.5): the speed table indices; wall-clock
    // pacing only. Retail sends them as GameOptions (39), which the port's command stream does not carry [port]: the
    // indices go straight to the pacing (a session start resets them to 2, so they are applied again then).
    if (session_ && live_input()) {
        sim::FrameSynchronizer& fs = session_->host().pacing();
        fs.set_tactical_speed(game_speed_index(gameplay_.game_speed));
        fs.set_galactic_speed(game_speed_index(gameplay_.strategic_speed));
    }
}

// ABILITIES_HARDPOINTS.md §2.3 "Autofire default at spawn": with IDC_ABILITIES_DEFAULT_TO_AUTOFIRE on, every new object of
// the local player gets SetAbilityAutofire (58) for each of its abilities with Supports_Autofire, through the command
// stream. [port] the host sees new objects in the local player's selection tree once per frame and sends one command
// per ability type for all of them (retail queues it per object at spawn).
void App::queue_default_autofire(std::vector<commands::EventData>& cmds) {
    if (!gameplay_.abilities_autofire || !live_input()) return;
    const sim::TacticalMode* tm = session_ ? session_->host().modes().top_tactical() : nullptr;
    const sim::TacticalBattle* tb = battle();
    const int32_t me = local_player();
    if (!tm || !tb || me < 0 || static_cast<uint32_t>(me) >= tm->trees().player_count()) return;
    tactical::QueryContext qc;
    const tactical::Aabb all{{-1e9f, -1e9f, -1e9f}, {1e9f, 1e9f, 1e9f}};
    std::vector<uint32_t> fresh;
    for (const tactical::EntityHandle id : tm->trees().player(static_cast<uint32_t>(me)).query_aabb(all, tactical::kSkipDead, qc))
        if (!std::binary_search(autofire_seen_.begin(), autofire_seen_.end(), id)) fresh.push_back(id);
    if (fresh.empty()) return;
    std::sort(fresh.begin(), fresh.end());
    const auto& ab = tb->abilities();
    for (uint32_t t = 1; t <= 75; ++t) {
        const auto type = static_cast<uint8_t>(t);
        std::vector<uint32_t> ids;
        for (const uint32_t id : fresh) {
            const auto oid = static_cast<tactical::abilities::ObjectId>(id);
            if (!ab.has_ability(oid, type) || ab.is_autofire(oid, type)) continue;
            if (const tactical::abilities::UnitAbilityRecord* r = ab.record(oid, type); r && r->supports_autofire) ids.push_back(id);
        }
        for (size_t at = 0; at < ids.size(); at += ui::OrderTranslator::kMaxSelect) {
            const size_t n = std::min(ids.size() - at, ui::OrderTranslator::kMaxSelect);
            commands::EventData e(commands::EventType::SetAbilityAutofire);
            e.set_ids("units", std::vector<uint32_t>(ids.begin() + static_cast<std::ptrdiff_t>(at),
                                                     ids.begin() + static_cast<std::ptrdiff_t>(at + n)));
            e.set("ability", t).set("on", 1u);
            cmds.push_back(std::move(e));
        }
    }
    std::vector<uint32_t> merged;
    std::merge(autofire_seen_.begin(), autofire_seen_.end(), fresh.begin(), fresh.end(), std::back_inserter(merged));
    autofire_seen_ = std::move(merged);
}

// ui1 t04 (HUD_GAPS §9.1, SCRIPT_GAPS §1.1 steps 4 / 5): the local player's space special weapon - a SPECIAL_WEAPON object
// (Special_Weapon_Index = the button icon and pointer index) or a station with a special-weapon hard point (icon 2,
// "station turret"). [port] several: the first ready one in id order, else the first.
objects::ObjectId App::space_weapon(int32_t& icon, float& readiness) const {
    icon = -1;
    readiness = 0.0f;
    const sim::TacticalMode* tm = session_ ? session_->host().modes().top_tactical() : nullptr;
    const sim::TacticalBattle* tb = session_ ? session_->battle() : nullptr;
    const int32_t me = local_player();
    if (!tm || !tb || !bview_.space() || me < 0 || static_cast<uint32_t>(me) >= tm->trees().player_count()) return objects::kNoObjectId;
    tactical::QueryContext qc;
    const tactical::Aabb all{{-1e9f, -1e9f, -1e9f}, {1e9f, 1e9f, 1e9f}};
    std::vector<uint32_t> ids;
    for (const tactical::EntityHandle id : tm->trees().player(static_cast<uint32_t>(me)).query_aabb(all, tactical::kSkipDead, qc))
        ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    const objects::ObjectStore& store = tm->primary();
    objects::ObjectId best = objects::kNoObjectId;
    for (const uint32_t u : ids) {
        const auto id = static_cast<objects::ObjectId>(u);
        int32_t ic = -1;
        float r = -1.0f;
        if ((tb->special_weapons().kinds(id) & tactical::abilities::kSwSpecialWeapon) != 0) {
            const objects::ObjectHandle h = store.find(id);
            if (!store.exists(h)) continue;
            ic = std::clamp(data_.types.get_int(store.type(h), "Special_Weapon_Index"), 0, 2);
            r = tb->special_weapons().readiness(id, tb->frame());
        } else if (const float sr = tb->combat().special_weapon_readiness(static_cast<tactical::combat::ObjectId>(id)); sr >= 0.0f) {
            ic = 2;
            r = sr;
        }
        if (ic < 0) continue;
        if (best == objects::kNoObjectId || (readiness < 1.0f && r >= 1.0f)) {
            best = id;
            icon = ic;
            readiness = r;
        }
    }
    return best;
}

// 0x346a80 / hotkey 22 in space: a second press cancels the aim; a ready weapon starts it (the button is disabled while
// none is ready, the hotkey then does nothing).
void App::space_weapon_toggle() {
    if (sw_weapon_ != objects::kNoObjectId) {
        sw_weapon_ = objects::kNoObjectId;
        return;
    }
    const sim::TacticalBattle* tb = battle();
    int32_t icon = -1;
    float ready = 0.0f;
    const objects::ObjectId w = space_weapon(icon, ready);
    if (w == objects::kNoObjectId || ready < 1.0f || !tb || !tb->special_weapons_allowed(local_player()) || story_controls_locked()) return;
    sw_weapon_ = w;
    sw_icon_ = icon;
    bombing_targeting_ = false;  // one modal pick at a time
    reinforce_unit_ = 0;
    targeting_.cancel();
}

// ui1 t03: IDC_MENU_CREDITS -> the credits roll (credits_roll.hpp) over the menu backdrop; the menu dialogs close while it
// runs and the main menu comes back when it ends or is clicked away. CreditsText_<LANGUAGE>.DAT of the loaded text
// language, else ENGLISH.
void App::show_credits() {
    if (screen_ != Screen::Menu || !data_.files) return;
    std::vector<uint8_t> bytes;
    const std::string lang = ui_.text.language.empty() ? std::string("ENGLISH") : ui_.text.language;
    if (!data_.files->read(ui::credits_path(lang), bytes) && !data_.files->read(ui::credits_path("ENGLISH"), bytes)) {
        notes_.push_back("credits: no CreditsText DAT");
        return;
    }
    std::vector<ui::CreditsLine> lines;
    if (!ui::parse_credits(bytes, lines) || lines.empty()) {
        notes_.push_back("credits: CreditsText DAT not readable");
        return;
    }
    ui_.gui.close_all();
    credits_.start(std::move(lines), ui::credits_style(data_.db.constants()), now_s_);
}

}  // namespace alamo_game_app
