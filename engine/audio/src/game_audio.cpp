// Game-host event hooks (see alamo/audio/game_audio.hpp).
#include "alamo/audio/game_audio.hpp"

#include "alamo/core/hash.hpp"
#include "alamo/data/values.hpp"

namespace alamo::audio {
namespace {

constexpr const char* kResponseTags[kUnitOrderCount] = {
    "SFXEvent_Select",       "SFXEvent_Move",        "SFXEvent_Fleet_Move",   "SFXEvent_Attack",
    "SFXEvent_Guard",        "SFXEvent_Defend",      "SFXEvent_Stop",         "SFXEvent_Deploy",
    "SFXEvent_Undeploy",     "SFXEvent_Move_Opposite", "SFXEvent_Group_Move", "SFXEvent_Group_Attack",
    "SFXEvent_Assist_Move",  "SFXEvent_Assist_Attack",
};

void read_tag(data::XmlNode type, std::string_view tag, std::string& out) {
    if (data::XmlNode c = type.child(tag)) out = std::string(c.text_trimmed());
}

void read_seconds(data::XmlNode type, std::string_view tag, float& out) {
    if (data::XmlNode c = type.child(tag)) {
        if (auto f = data::parse_float(c.text_trimmed())) out = *f;
    }
}

}  // namespace

UnitSounds unit_sounds_from_xml(data::XmlNode type, const UnitSounds* base) {
    UnitSounds s = base ? *base : UnitSounds{};
    if (!type) return s;
    for (size_t i = 0; i < kUnitOrderCount; ++i) read_tag(type, kResponseTags[i], s.response[i]);
    read_tag(type, "SFXEvent_Select_With_Non_Hero_Ability", s.select_with_non_hero_ability);
    read_tag(type, "SFXEvent_Move_With_Non_Hero_Ability", s.move_with_non_hero_ability);
    read_tag(type, "SFXEvent_Attack_With_Non_Hero_Ability", s.attack_with_non_hero_ability);
    read_tag(type, "SFXEvent_Unit_Lost", s.unit_lost);
    read_tag(type, "SFXEvent_Unit_Under_Attack", s.unit_under_attack);
    read_tag(type, "SFXEvent_Fire", s.fire);
    read_tag(type, "Death_SFXEvent_Start_Die", s.death_start);
    read_tag(type, "Death_SFXEvent_End_Die", s.death_end);
    read_tag(type, "SFXEvent_Ambient_Loop", s.ambient_loop);
    read_tag(type, "SFXEvent_Ambient_Moving", s.ambient_moving);
    read_seconds(type, "SFXEvent_Ambient_Moving_Min_Delay_Seconds", s.ambient_moving_min_seconds);
    read_seconds(type, "SFXEvent_Ambient_Moving_Max_Delay_Seconds", s.ambient_moving_max_seconds);
    read_tag(type, "SFXEvent_Engine_Idle_Loop", s.engine_idle_loop);
    read_tag(type, "SFXEvent_Engine_Moving_Loop", s.engine_moving_loop);
    read_tag(type, "SFXEvent_Engine_Cinematic_Focus_Loop", s.engine_cinematic_focus_loop);
    read_tag(type, "Projectile_SFXEvent_Detonate", s.projectile_detonate);
    return s;
}

// ---------------------------------------------------------------- setup

GameAudio::GameAudio(AudioSystem& audio, const GameAudioData& data, SimRng& free_rng, AmbienceConfig ambience)
    : audio_(&audio), data_(&data), rng_(&free_rng), ambience_(audio, data, free_rng, ambience),
      conversations_(audio) {
    audio_->set_music_resolver([this](uint8_t mode, SimRng&) { return resolve_music(mode); });
}

GameAudio::~GameAudio() { audio_->set_music_resolver({}); }

void GameAudio::set_local_faction(std::string_view faction) { faction_ = std::string(faction); }

std::string GameAudio::pick(const std::vector<std::string>& list) {
    if (list.empty()) return {};
    // §2.3 "random free-RNG pick"; int(lo, hi) draws nothing when there is one entry.
    return list[static_cast<size_t>(rng_->range_int(0, static_cast<int32_t>(list.size()) - 1))];
}

// The mode music table (AUDIO.md §2.3) from the local faction's Faction tags.
std::string GameAudio::resolve_music(uint8_t mode) {
    const FactionAudio* f = data_->find_faction(faction_);
    if (!f) return {};
    const char* lands = land_ ? "Land" : "Space";
    switch (mode) {
        case music_mode::kGalactic: return std::string(f->get("Strategic_Map_Music_Event"));
        case music_mode::kBattlePending:
            return std::string(f->get(std::string("Music_Event_Tactical_") + lands + "_Battle_Pending"));
        case music_mode::kTacticalAmbient:
        case music_mode::kTacticalBattle: {
            const bool battle = mode == music_mode::kTacticalBattle;
            if (super_weapon_) {
                const std::string_view sw =
                    f->get(std::string("Music_Event_") + lands + (battle ? "_Battle" : "_Ambient") + "_Super_Weapon");
                if (!sw.empty()) return std::string(sw);
            }
            // Space maps use the "Space" slot of the environment lists.
            return pick(f->pair_values(battle ? "Music_Event_List_Battle" : "Music_Event_List_Ambient",
                                       land_ ? std::string_view(environment_) : std::string_view("Space")));
        }
        default: return {};
    }
}

float GameAudio::loop_fade(std::string_view event, bool in) const {
    const uint32_t idx = audio_->database().find_sfx(event);
    if (idx == kNoEvent) return 0.0f;
    const SfxEvent& e = audio_->database().sfx()[idx];
    return in ? e.loop_fade_in_seconds : e.loop_fade_out_seconds;
}

uint32_t GameAudio::play_2d(std::string_view event, uint32_t pump) {
    if (event.empty()) return 0;
    SfxPlay p;
    p.pump = pump;
    return audio_->play_sfx(event, p);
}

// ---------------------------------------------------------------- modes

void GameAudio::enter_front_end(std::string_view music_event) {
    tactical_ = false;
    ambience_.end(false);
    audio_->set_mode(AudioMode::Galactic);
    audio_->set_3d_scale(1.0f);
    if (!music_event.empty()) audio_->play_music(music_event, music_mode::kFrontEnd, false, false);
}

void GameAudio::splash_screen(std::string_view music_event) {
    if (!music_event.empty()) audio_->play_music(music_event, music_mode::kSplash, false, false);
}

void GameAudio::super_weapon_activation(std::string_view music_event) {
    if (!music_event.empty()) audio_->play_music(music_event, music_mode::kSuperWeapon, false, false);
}

void GameAudio::build_complete(std::string_view music_event) {
    if (!music_event.empty()) audio_->play_music(music_event, music_mode::kBuildComplete, true, false);
}

void GameAudio::movie_music(std::string_view music_event) {
    // STRATEGIC_GAPS §7.4: mode 14 with resume previous [M for the name].
    if (!music_event.empty()) audio_->play_music(music_event, music_mode::kMovie, true, false);
}

void GameAudio::movie_music_cue(std::string_view music_event) {
    if (!music_event.empty()) audio_->play_music(music_event, music_mode::kMovieCue, false, false);
}

bool GameAudio::movie_music_end() {
    bool any = false;
    // A cue (17) started over the movie's own music (14) leaves that in the chain: unwind both, the newest first.
    for (int guard = 0; guard < 4; ++guard) {
        if (!audio_->end_music_mode(music_mode::kMovieCue) && !audio_->end_music_mode(music_mode::kMovie)) break;
        any = true;
    }
    return any;
}

void GameAudio::halt_game_music() { audio_->stop_all_music(); }

void GameAudio::enter_galactic() {
    tactical_ = false;
    battle_over_ = false;
    ambience_.end(false);
    audio_->set_mode(AudioMode::Galactic);
    audio_->set_3d_scale(1.0f);
    audio_->set_base_music_mode(music_mode::kGalactic);
    // [port] mode entry forces the mode music (the original's resume is unforced: after a splash (11) or super
    // weapon (8) event an unforced resume would keep that music after the mode change).
    audio_->resume_mode_based_music(true);
}

void GameAudio::battle_pending(bool land) {
    land_ = land;
    const std::string music = resolve_music(music_mode::kBattlePending);
    if (!music.empty()) audio_->play_music(music, music_mode::kBattlePending);
    if (const FactionAudio* f = data_->find_faction(faction_))
        play_2d(f->get(land ? "SFX_Event_Tactical_Land_Battle_Pending" : "SFX_Event_Tactical_Space_Battle_Pending"));
}

void GameAudio::battle_load_screen(bool open) {
    audio_->set_3d_scale(open ? 0.0f : 1.0f);  // §1.6: 3D SFX silent behind the load screen
    if (!open) return;
    std::string music;
    if (const FactionAudio* f = data_->find_faction(faction_)) music = std::string(f->get("Music_Event_Battle_Load_Screen"));
    if (music.empty()) music = audio_->database().constants().music_event_battle_load_screen;
    if (!music.empty()) audio_->play_music(music, music_mode::kLoadScreen);
}

void GameAudio::enter_tactical(const TacticalSetup& setup) {
    tactical_ = true;
    battle_over_ = false;
    land_ = setup.land;
    environment_ = setup.environment;
    super_weapon_ = setup.super_weapon_present;
    weather_active_ = false;
    weather_type_ = "CLEAR";
    base_announced_ = false;
    audio_->set_mode(land_ ? AudioMode::Land : AudioMode::Space);
    audio_->set_3d_scale(1.0f);
    audio_->set_base_music_mode(music_mode::kTacticalAmbient);
    ambience_.begin(land_ ? std::string_view(environment_) : std::string_view("Space"));
    audio_->resume_mode_based_music(true);  // [port] forced, see enter_galactic
}

void GameAudio::set_super_weapon_present(bool present, bool restart_music) {
    const bool changed = present != super_weapon_;
    super_weapon_ = present;
    if (!changed || !restart_music || !tactical_ || battle_over_) return;
    const uint8_t m = audio_->music_mode();
    if (m == music_mode::kTacticalAmbient || m == music_mode::kTacticalBattle) {
        const std::string music = resolve_music(m);
        if (!music.empty()) audio_->play_music(music, m);
    }
}

void GameAudio::battle_end(const BattleResult& result) {
    battle_over_ = true;
    const FactionAudio* f = data_->find_faction(faction_);
    if (!f) return;
    const std::string base = std::string(result.strategic ? "Music_Event_Strategic_" : "Music_Event_Tactical_") +
                             (result.won ? "Win" : "Lose");
    std::string music;
    if (!result.enemy_faction.empty()) {
        const std::vector<std::string> vs = f->pair_values(base + "_Vs_Faction", result.enemy_faction);
        if (!vs.empty()) music = vs.front();  // [inferred] first matching row
    }
    if (music.empty()) music = std::string(f->get(base));
    if (!music.empty()) audio_->play_music(music, music_mode::kBattleEnd, false, true);
}

void GameAudio::battle_summary_screen(bool won) {
    const AudioConstants& c = audio_->database().constants();
    const std::string& music =
        won ? c.music_event_battle_end_summary_screen_win : c.music_event_battle_end_summary_screen_lose;
    if (!music.empty()) audio_->play_music(music, music_mode::kBattleEnd);
}

void GameAudio::leave_mode() {
    conversations_.cancel_all();
    audio_->stop_all_sfx();
    audio_->stop_all_speech();
    ambience_.end(false);
    objects_.clear();
    tactical_ = false;
}

// ---------------------------------------------------------------- per frame

void GameAudio::set_camera(const CameraState& camera) {
    const int m = static_cast<int>(audio_->mode());
    const float pullback = audio_->database().constants().listener_z_pullback[m];
    audio_->set_cinematic(camera.cinematic);
    audio_->set_listener(
        listener_from_camera(camera.position, camera.look_at, camera.forward, camera.up, pullback, camera.cinematic));
}

void GameAudio::set_weather(std::string_view weather, float intensity) {
    ambience_.set_weather(weather, intensity);
    if (!tactical_) return;
    const bool active = !iequals(weather, "CLEAR") && intensity > 0.0f;
    const bool changed_type = active && weather_active_ && !iequals(weather, weather_type_);
    if (active == weather_active_ && !changed_type) return;
    const FactionAudio* f = data_->find_faction(faction_);
    if (weather_active_ && f) {
        const std::vector<std::string> v = f->pair_values("SFXEvent_Weather_End", weather_type_);
        if (!v.empty()) play_2d(v.front());
    }
    if (active && f) {
        const std::vector<std::string> v = f->pair_values("SFXEvent_Weather_Begin", weather);
        if (!v.empty()) play_2d(v.front());
    }
    weather_active_ = active;
    weather_type_ = active ? std::string(weather) : std::string("CLEAR");
}

void GameAudio::sim_tick() {
    ++sim_ticks_;
    audio_->music_sim_tick();
}

void GameAudio::update(double wall_dt_seconds) {
    // AUDIO.md §6.1 step 14 order: audio system + SFX + music + speech, conversation managers, ambience.
    audio_->update(wall_dt_seconds);
    conversations_.update(wall_dt_seconds);
    ambience_.update(wall_dt_seconds);
    for (auto& [id, o] : objects_) {
        if (!o.moving || o.engine.dead || o.sounds.ambient_moving.empty()) continue;
        o.moving_timer -= wall_dt_seconds;
        if (o.moving_timer > 0.0) continue;
        object_sfx(id, o.sounds.ambient_moving, o.position);
        o.moving_timer = static_cast<double>(
            rng_->range_float(o.sounds.ambient_moving_min_seconds, o.sounds.ambient_moving_max_seconds));
    }
}

// ---------------------------------------------------------------- battle events

uint32_t GameAudio::weapon_fire(uint64_t shooter, std::string_view event, const Vec3& position) {
    if (event.empty() || audio_->database().constants().test_disable_weapon_fire_sfx) return 0;
    return object_sfx(shooter, event, position);
}

uint32_t GameAudio::impact(std::string_view event, const Vec3& position) {
    if (event.empty()) return 0;
    SfxPlay p;
    p.kind = SfxPlay::Kind::Position;
    p.position = position;
    return audio_->play_sfx(event, p);
}

uint32_t GameAudio::world_sfx(std::string_view event, const Vec3& position) { return impact(event, position); }

uint32_t GameAudio::animation_sounds(uint64_t object, std::string_view animation, int64_t prev_ms, int64_t now_ms,
                                     const Vec3& position) {
    uint32_t n = 0;
    for (const AnimSfxEntry* e : data_->animation_sfx().crossed(animation, prev_ms, now_ms))
        if (e->kind == AnimSfxEntry::Kind::Sound && object_sfx(object, e->name, position) != 0) ++n;
    return n;
}

uint32_t GameAudio::unit_death(uint64_t object, std::string_view event, const Vec3& position) {
    object_dead(object);
    return impact(event, position);
}

uint32_t GameAudio::object_sfx(uint64_t object, std::string_view event, const Vec3& position, float fade_in_seconds) {
    if (event.empty()) return 0;
    SfxPlay p;
    p.kind = SfxPlay::Kind::Object;
    p.owner = object;
    p.position = position;
    p.fade_in_seconds = fade_in_seconds;
    return audio_->play_sfx(event, p);
}

void GameAudio::stop_object_sfx(uint64_t object, std::string_view event, float fade_seconds) {
    audio_->stop_object_sfx(object, event, fade_seconds);
}

void GameAudio::combat(const CombatEvent& e) {
    if (!tactical_) return;
    if (e.local_player_involved && !battle_over_) audio_->notify_combat(true);
    if (!e.local_base_attacked) return;
    const AudioConstants& c = audio_->database().constants();
    const int32_t delay_s = land_ ? c.delay_between_land_base_attack_announcement_seconds
                                  : c.delay_between_space_base_attack_announcement_seconds;
    const uint64_t delay_ticks = delay_s > 0 ? static_cast<uint64_t>(delay_s) * 30u : 0u;
    if (base_announced_ && sim_ticks_ - last_base_announcement_ < delay_ticks) return;
    base_announced_ = true;
    last_base_announcement_ = sim_ticks_;
    faction_sfx(land_ ? "SFXEvent_Land_Base_Under_Attack_Announcement" : "SFXEvent_Space_Base_Under_Attack_Announcement");
}

// ---------------------------------------------------------------- objects

void GameAudio::object_spawned(uint64_t object, const UnitSounds& sounds, const Vec3& position) {
    ObjectState o;
    o.sounds = sounds;
    o.position = position;
    if (!sounds.ambient_loop.empty())
        o.ambient_loop = object_sfx(object, sounds.ambient_loop, position, loop_fade(sounds.ambient_loop, true));
    ObjectState& placed = objects_[object] = std::move(o);
    // STATION_HANGARS_SOUND §3.3: entering the active tactical mode = idle on, moving off.
    set_engine_loop(placed, object, false, true);
    set_engine_loop(placed, object, true, false);
}

void GameAudio::object_moving(uint64_t object, bool moving) {
    auto it = objects_.find(object);
    if (it == objects_.end() || it->second.engine.dead) return;
    ObjectState& o = it->second;
    engine_service(object, moving);
    if (o.moving == moving) return;
    o.moving = moving;
    if (moving && !o.sounds.ambient_moving.empty())
        o.moving_timer = static_cast<double>(
            rng_->range_float(o.sounds.ambient_moving_min_seconds, o.sounds.ambient_moving_max_seconds));
}

void GameAudio::object_removed(uint64_t object) {
    audio_->object_removed(object);
    objects_.erase(object);
}

// ---------------------------------------------------------------- engine loops (STATION_HANGARS_SOUND §3)

void GameAudio::set_engine_loop(ObjectState& o, uint64_t object, bool moving_slot, bool on) {
    // §3.2: nothing while dead (+0x3A0 bit 0x02) or suppressed (+0x3B9).
    if (o.engine.dead || o.engine.suppressed) return;
    bool& flag = moving_slot ? o.engine.moving : o.engine.idle;
    if (flag == on) return;
    flag = on;  // the requested state, even when there is no event or a gate refuses the start
    const std::string& event = moving_slot ? o.sounds.engine_moving_loop : o.sounds.engine_idle_loop;
    if (event.empty()) return;
    if (on) object_sfx(object, event, o.position, loop_fade(event, true));
    else audio_->stop_object_sfx(object, event, loop_fade(event, false));
}

void GameAudio::set_engine_idle_loop(uint64_t object, bool on) {
    auto it = objects_.find(object);
    if (it != objects_.end()) set_engine_loop(it->second, object, false, on);
}

void GameAudio::set_engine_moving_loop(uint64_t object, bool on) {
    auto it = objects_.find(object);
    if (it != objects_.end()) set_engine_loop(it->second, object, true, on);
}

void GameAudio::engine_service(uint64_t object, bool moving) {
    set_engine_moving_loop(object, moving);  // §3.3: moving call first, then idle
    set_engine_idle_loop(object, !moving);
}

void GameAudio::engine_mode_enter(uint64_t object) {
    set_engine_idle_loop(object, true);
    set_engine_moving_loop(object, false);
}

void GameAudio::engines_offline(uint64_t object) {
    set_engine_moving_loop(object, false);
    set_engine_idle_loop(object, false);
}

void GameAudio::engines_online(uint64_t object) { set_engine_idle_loop(object, true); }

void GameAudio::suppress_engine_audio(uint64_t object, float fade_seconds) {
    auto it = objects_.find(object);
    if (it == objects_.end()) return;
    ObjectState& o = it->second;
    // §3.4 (0x392410): the stops happen whatever the flags say.
    for (const std::string* event :
         {&o.sounds.engine_moving_loop, &o.sounds.engine_idle_loop, &o.sounds.engine_cinematic_focus_loop}) {
        if (event->empty()) continue;
        audio_->stop_object_sfx(object, *event, fade_seconds < 0.0f ? loop_fade(*event, false) : fade_seconds);
    }
    o.engine.moving = false;
    o.engine.idle = false;
    o.engine.suppressed = true;
}

void GameAudio::clear_engine_audio_suppression(uint64_t object) {
    auto it = objects_.find(object);
    if (it != objects_.end()) it->second.engine.suppressed = false;  // restarts nothing (§3.4)
}

void GameAudio::object_dead(uint64_t object) {
    audio_->object_removed(object);  // §1.5: every attached instance, at once, no fade
    auto it = objects_.find(object);
    if (it == objects_.end()) return;
    it->second.engine.dead = true;
    it->second.moving = false;
    it->second.ambient_loop = 0;
}

std::optional<GameAudio::EngineLoopState> GameAudio::engine_loop_state(uint64_t object) const {
    auto it = objects_.find(object);
    if (it == objects_.end()) return std::nullopt;
    return it->second.engine;
}

// ---------------------------------------------------------------- UI

uint32_t GameAudio::unit_response(uint64_t representative, const UnitSounds& sounds, UnitOrder order,
                                  const Vec3& position, bool group, bool non_hero_ability) {
    if (order == UnitOrder::Count) return 0;
    auto resp = [&](UnitOrder o) -> const std::string& { return sounds.response[static_cast<size_t>(o)]; };
    std::string event;
    // [inferred] AUDIO.md §3.3: the non-hero-ability override first, then the group variants, then the order's
    // own line; FleetMove falls back to Move.
    if (non_hero_ability) {
        if (order == UnitOrder::Select) event = sounds.select_with_non_hero_ability;
        else if (order == UnitOrder::Move) event = sounds.move_with_non_hero_ability;
        else if (order == UnitOrder::Attack) event = sounds.attack_with_non_hero_ability;
    }
    if (event.empty() && group) {
        if (order == UnitOrder::Move) event = resp(UnitOrder::GroupMove);
        else if (order == UnitOrder::Attack) event = resp(UnitOrder::GroupAttack);
    }
    if (event.empty()) event = resp(order);
    if (event.empty() && order == UnitOrder::FleetMove) event = resp(UnitOrder::Move);
    if (event.empty()) return 0;
    SfxPlay p;
    p.kind = SfxPlay::Kind::Object;
    p.owner = representative;
    p.position = position;
    return audio_->play_sfx(event, p);
}

uint32_t GameAudio::command_bar_sfx(UnitOrder order, bool attack_move, bool waypoint) {
    const AudioConstants& c = audio_->database().constants();
    switch (order) {
        case UnitOrder::Attack: return play_2d(attack_move ? c.sfx_command_bar_attack_move : c.sfx_command_bar_attack, 1);
        case UnitOrder::Move: return play_2d(waypoint ? c.sfx_command_bar_waypoint : c.sfx_command_bar_move, 1);
        case UnitOrder::Guard: return play_2d(c.sfx_command_bar_guard, 1);
        case UnitOrder::Stop: return play_2d(c.sfx_command_bar_stop, 1);
        default: return 0;
    }
}

uint32_t GameAudio::gui_sfx(std::string_view event) { return play_2d(event, 1); }

void GameAudio::negative_feedback() { play_2d(audio_->database().constants().sfx_gui_negative_feedback, 1); }

uint32_t GameAudio::faction_sfx(std::string_view faction_tag) {
    const FactionAudio* f = data_->find_faction(faction_);
    return f ? play_2d(f->get(faction_tag)) : 0u;
}

uint32_t GameAudio::enemy_spotted(std::string_view object_type_name) {
    if (!enemy_sighted_vo_) return 0;
    const FactionAudio* f = data_->find_faction(faction_);
    if (!f) return 0;
    const std::vector<std::string> v = f->pair_values("SFXEvent_Unit_Type_Spotted", object_type_name);
    return play_2d(v.empty() ? f->get("SFXEvent_Enemy_Spotted") : std::string_view(v.front()));
}

}  // namespace alamo::audio
