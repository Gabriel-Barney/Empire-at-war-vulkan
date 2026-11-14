#include "alamo/game/game_audio_host.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

#include "alamo/sim/tactical_battle.hpp"
#ifdef ALAMO_GAME_HAVE_AUDIO
#include "alamo/audio/audio_db.hpp"
#include "alamo/audio/audio_system.hpp"
#include "alamo/audio/game_audio.hpp"
#include "alamo/audio/game_audio_data.hpp"
#include "alamo/audio/miniaudio_backend.hpp"
#include "alamo/audio/sample_source.hpp"
#include "alamo/vfs/vfs.hpp"
#ifdef ALAMO_AUDIO_HAVE_LUA
#include "alamo/audio/lua_audio.hpp"
#endif
#else
namespace alamo::audio {  // complete stand-ins for the owned pointers (no engine/audio in this build)
class AudioDatabase {};
class AudioSystem {};
class VoiceBackend {};
class SampleSource {};
class GameAudioData {};
class GameAudio {};
struct UnitSounds {};
}  // namespace alamo::audio
#endif

namespace alamo::game {

std::string_view GameAudioHost::control_point_line(int32_t prev, int32_t timer, int32_t leader, int32_t local_player, bool leader_is_enemy) {
    if (timer < 0 || leader < 0 || local_player < 0) return {};
    static constexpr struct {
        int32_t frames;
        const char* owned;
        const char* ally;
        const char* enemy;
    } kMarks[] = {
        {60 * 30, "SFXEvent_HUD_Reinforcement_Point_Owned_60_Seconds", "SFXEvent_HUD_Reinforcement_Point_Ally_Owned_60_Seconds",
         "SFXEvent_HUD_Reinforcement_Point_Enemy_Owned_60_Seconds"},
        {30 * 30, "SFXEvent_HUD_Reinforcement_Point_Owned_30_Seconds", "SFXEvent_HUD_Reinforcement_Point_Ally_Owned_30_Seconds",
         "SFXEvent_HUD_Reinforcement_Point_Enemy_Owned_30_Seconds"},
        {15 * 30, "SFXEvent_HUD_Reinforcement_Point_Owned_15_Seconds", "SFXEvent_HUD_Reinforcement_Point_Ally_Owned_15_Seconds",
         "SFXEvent_HUD_Reinforcement_Point_Enemy_Owned_15_Seconds"},
        {5 * 30, "SFXEvent_HUD_Reinforcement_Point_Owned_05_Seconds", "SFXEvent_HUD_Reinforcement_Point_Ally_Owned_05_Seconds",
         "SFXEvent_HUD_Reinforcement_Point_Enemy_Owned_05_Seconds"},
    };
    const bool armed_now = prev < 0 || timer > prev;
    for (const auto& m : kMarks) {
        const bool hit = armed_now ? (timer == m.frames || timer + 1 == m.frames) : (prev > m.frames && m.frames >= timer);
        if (!hit) continue;
        if (leader == local_player) return m.owned;
        return leader_is_enemy ? m.enemy : m.ally;
    }
    return {};
}

std::string_view GameAudioHost::garrison_line(int32_t unit_owner, int32_t local_player) {
    return local_player >= 0 && unit_owner == local_player ? std::string_view("SFXEvent_Bunker_Garrisoned") : std::string_view();
}

std::string_view GameAudioHost::clash_win_speech(const tactical::combat::HeroClashData* data, uint32_t entry, bool first_won) {
    if (!data || entry >= data->entries.size()) return {};
    return first_won ? data->entries[entry].first_speech : data->entries[entry].second_speech;
}

Vec3 GameAudioHost::position_of(uint64_t id) const {
    const auto it = positions_.find(id);
    return it == positions_.end() ? Vec3{} : it->second;
}

#ifdef ALAMO_GAME_HAVE_AUDIO

namespace {
audio::UnitOrder order_of(UnitResponse k, bool group) {
    switch (k) {
        case UnitResponse::Select: return audio::UnitOrder::Select;
        case UnitResponse::Move: return group ? audio::UnitOrder::GroupMove : audio::UnitOrder::Move;
        case UnitResponse::FleetMove: return audio::UnitOrder::FleetMove;
        case UnitResponse::Attack: return group ? audio::UnitOrder::GroupAttack : audio::UnitOrder::Attack;
        case UnitResponse::Guard: return audio::UnitOrder::Guard;
        case UnitResponse::Stop: return audio::UnitOrder::Stop;
    }
    return audio::UnitOrder::Select;
}
Vec3 v3(const tactical::Vec3& v) { return {v.x, v.y, v.z}; }
}  // namespace

bool GameAudioHost::init(const GameData& data, AudioBackendKind kind, uint32_t seed, std::string* err) {
    rng_.seed(seed);
    if (kind == AudioBackendKind::Off) return true;
    db_ = std::make_unique<audio::AudioDatabase>();
    if (!db_->load(*data.files)) {
        if (err) *err = "SFXEventFiles.xml missing";
        db_.reset();
        return false;
    }
    game_data_ = std::make_unique<audio::GameAudioData>();
    game_data_->load(*data.files);
    audio::GameSampleConfig sc;
    if (data.vfs) sc.sfx_megs = audio::GameSampleSource::default_sfx_megs(*data.vfs, sc.language);
    samples_ = std::make_unique<audio::GameSampleSource>(*data.files, sc);
    if (kind == AudioBackendKind::Device) {
        std::string e;
        backend_ = audio::MiniaudioBackend::create(*samples_, audio::MiniaudioConfig{}, &e);
        if (!backend_) {
            if (err) *err = "device: " + e;
            return false;
        }
    } else {
        const audio::SampleSource* s = samples_.get();
        backend_ = std::make_unique<audio::NullBackend>([s](const audio::SampleRef& r) -> std::optional<double> {
            if (!s->exists(r)) return std::nullopt;
            return 1.0;  // [port] nominal length: the null backend only needs a deterministic duration
        });
    }
    system_ = std::make_unique<audio::AudioSystem>(*db_, *backend_, rng_);
    system_->on_speech_done([this](const std::string& event, bool) { speech_done_.push_back(event); });  // L1/S4 wave 1
    game_ = std::make_unique<audio::GameAudio>(*system_, *game_data_, rng_);
    game_->set_object_query([this](uint64_t owner) -> std::optional<audio::AudioSystem::ObjectAudio> {
        const auto it = positions_.find(owner);
        if (it == positions_.end()) return std::nullopt;
        audio::AudioSystem::ObjectAudio a;
        a.position = it->second;
        a.audible = inaudible_.find(owner) == inaudible_.end();  // §3.5 item 8: fog / hidden / stealth mute, no stop
        a.hidden = hidden_.find(owner) != hidden_.end();          // AUDIO.md §1.6 (b0280): a cinematic unmutes fog only
        return a;
    });
    return true;
}

const audio::UnitSounds& GameAudioHost::sounds(const objects::GameObjectTypes& types, objects::TypeIndex t) {
    auto it = sounds_.find(t);
    if (it != sounds_.end()) return *it->second;
    auto s = std::make_unique<audio::UnitSounds>();
    if (t != objects::kNone && t < types.size()) {
        static constexpr const char* kTags[audio::kUnitOrderCount] = {
            "SFXEvent_Select",     "SFXEvent_Move",        "SFXEvent_Fleet_Move", "SFXEvent_Attack",       "SFXEvent_Guard",
            "SFXEvent_Defend",     "SFXEvent_Stop",        "SFXEvent_Deploy",     "SFXEvent_Undeploy",     "SFXEvent_Move_Opposite",
            "SFXEvent_Group_Move", "SFXEvent_Group_Attack", "SFXEvent_Assist_Move", "SFXEvent_Assist_Attack"};
        for (size_t i = 0; i < audio::kUnitOrderCount; ++i) s->response[i] = std::string(types.get_string(t, kTags[i]));
        s->unit_lost = std::string(types.get_string(t, "SFXEvent_Unit_Lost"));
        s->unit_under_attack = std::string(types.get_string(t, "SFXEvent_Unit_Under_Attack"));
        s->fire = std::string(types.get_string(t, "SFXEvent_Fire"));
        s->death_start = std::string(types.get_string(t, "Death_SFXEvent_Start_Die"));
        s->death_end = std::string(types.get_string(t, "Death_SFXEvent_End_Die"));
        s->ambient_loop = std::string(types.get_string(t, "SFXEvent_Ambient_Loop"));
        s->ambient_moving = std::string(types.get_string(t, "SFXEvent_Ambient_Moving"));
        s->engine_idle_loop = std::string(types.get_string(t, "SFXEvent_Engine_Idle_Loop"));
        s->engine_moving_loop = std::string(types.get_string(t, "SFXEvent_Engine_Moving_Loop"));
        s->engine_cinematic_focus_loop = std::string(types.get_string(t, "SFXEvent_Engine_Cinematic_Focus_Loop"));
        s->projectile_detonate = std::string(types.get_string(t, "Projectile_SFXEvent_Detonate"));
    }
    return *sounds_.emplace(t, std::move(s)).first->second;
}

void GameAudioHost::ui(std::string_view event) {
    if (game_ && !event.empty() && game_->gui_sfx(event)) ++stats_.ui;
}

void GameAudioHost::speech(std::string_view event) {
    if (game_ && !event.empty() && game_->speech(event)) ++stats_.speech;
}

void GameAudioHost::faction_sfx(std::string_view faction_tag) {
    if (!game_) return;
    ++stats_.feedback;
    game_->faction_sfx(faction_tag);
}

void GameAudioHost::battle_pending(bool land) {
    if (!game_) return;
    ++stats_.battle_pending;
    pending_played_ = true;
    game_->battle_pending(land);
}

void GameAudioHost::galactic_resume() {
    if (!game_ || scene_ != MusicScene::Galactic) return;
    pending_played_ = false;
    game_->enter_galactic();
}

void GameAudioHost::movie_started(bool movie_music, bool halt_music) {
    if (!game_) return;
    if (halt_music) {
        game_->halt_game_music();
        movie_halted_ = true;
        movie_music_on_ = false;
        ++stats_.movie_halts;
    }
    if (movie_music && !halt_music) {  // [port] a halted movie plays no music of its own
        game_->movie_music(kCreditsMusicEvent);
        if (system_ && system_->music_mode() == audio::music_mode::kMovie) {
            movie_music_on_ = true;
            ++stats_.movie_music;
        }
    }
}

void GameAudioHost::movie_cue(std::string_view music_event) {
    if (!game_ || music_event.empty()) return;
    const bool before = system_ && system_->music_mode() == audio::music_mode::kMovieCue;
    game_->movie_music_cue(music_event);
    ++stats_.movie_cues;
    if (system_ && system_->music_mode() == audio::music_mode::kMovieCue && !before) movie_music_on_ = true;
}

void GameAudioHost::movie_ended() {
    if (!game_) return;
    if (movie_music_on_ && game_->movie_music_end()) ++stats_.movie_restores;
    movie_music_on_ = false;
    if (movie_halted_) {  // Halt_Game_Music: the silence ends with the movie, nothing else started meanwhile
        movie_halted_ = false;
        if (system_ && system_->current_music().empty()) {
            system_->resume_mode_based_music(false);
            ++stats_.movie_restores;
        }
    }
}

void GameAudioHost::negative_feedback() {
    if (!game_) return;
    ++stats_.feedback;
    game_->negative_feedback();
}

void GameAudioHost::unit_response(const objects::GameObjectTypes& types, objects::TypeIndex type, uint64_t object, UnitResponse kind,
                                  bool group, int32_t owner) {
    if (!game_) return;
    // The mode's "excluded set" = the retreat coordinator's retreating units (STRATEGIC_GAPS §7.4 [M]): no response.
    if (owner >= 0 && owner == retreating_player_) {
        ++stats_.excluded;
        return;
    }
    if (game_->unit_response(object, sounds(types, type), order_of(kind, group), position_of(object), group)) ++stats_.responses;
}

void GameAudioHost::battle_end(bool won, std::string_view enemy_faction) {
    if (!game_) return;
    audio::BattleResult r;
    r.won = won;
    r.enemy_faction = std::string(enemy_faction);
    game_->battle_end(r);  // mode 16 (STRATEGIC_GAPS §7.4)
    ++stats_.battle_end_music;
}

void GameAudioHost::battle_tick(const sim::TacticalBattle& battle, const objects::ObjectStore& store, const objects::GameObjectTypes& types,
                                int32_t local_player, const std::function<bool(objects::ObjectId)>& script_hidden) {
    if (!game_) return;
    retreating_player_ = battle.retreating_player();
    const tactical::combat::CombatSystem& cs = battle.combat();
    const tactical::movement::MovementSystem& mv = battle.movement();
    bool combat = false;
    auto handle_of = [&store](int64_t id) { return store.find(static_cast<objects::ObjectId>(id)); };
    auto owner_of = [&](int64_t id) {
        const objects::ObjectHandle h = handle_of(id);
        return store.exists(h) ? store.owner(h) : -1;
    };
    // Battle units new since the last tick: ambient loop + engine mode enter (object_spawned). STATION_HANGARS_SOUND §3:
    // every locomotor service reports moving (speed >= 1.1 x SpaceIdleMovementSpeed, the sim's threshold) - ambient
    // moving chatter and the audio-side engine switch; outside hyperspace states. Audible = not hidden / fogged /
    // stealthed for the local player (running loops are muted, never stopped).
    const tactical::abilities::AbilitySystem& ab = battle.abilities();
    const float threshold = battle.engine_moving_threshold();
    if (!super_weapon_types_ready_) {  // AUDIO.md §2.3 (b0286): the factions' Faction_Super_Weapon types
        super_weapon_types_ready_ = true;
        for (const audio::FactionAudio& f : game_data_->factions()) {
            const objects::TypeIndex t = types.resolve_name(f.get("Faction_Super_Weapon"));
            if (t != objects::kNone && std::find(super_weapon_types_.begin(), super_weapon_types_.end(), t) == super_weapon_types_.end())
                super_weapon_types_.push_back(t);
        }
    }
    bool super_weapon = false;
    std::vector<objects::ObjectId> live;
    for (const objects::ObjectId id : mv.unit_ids()) {
        const objects::ObjectHandle h = store.find(id);
        if (!store.alive(h)) continue;
        live.push_back(id);
        // [port] any owner: only the Empire names a Faction_Super_Weapon (Death_Star) in shipped data.
        if (std::find(super_weapon_types_.begin(), super_weapon_types_.end(), store.type(h)) != super_weapon_types_.end())
            super_weapon = true;
        if (spawned_.insert(id).second) {
            game_->object_spawned(static_cast<uint64_t>(id), sounds(types, store.type(h)), store.position(h));
            ++stats_.spawned;
        }
        const bool hidden = ab.is_stealthed(id, battle.frame()) ||
                            (script_hidden && script_hidden(id));  // q6 t19: Lua Hide(true) mutes the object's sound
        const bool fogged = battle.fog_object_hidden(local_player, id, false);
        if (hidden || fogged) inaudible_.insert(static_cast<uint64_t>(id));
        else inaudible_.erase(static_cast<uint64_t>(id));
        if (hidden) hidden_.insert(static_cast<uint64_t>(id));
        else hidden_.erase(static_cast<uint64_t>(id));
        if (battle.hyperspace().departing(id)) continue;  // hyperspace states: the locomotor leaves the loops alone
        const bool moving = mv.view(id).speed >= threshold;
        moving_[id] = moving;
        game_->object_moving(static_cast<uint64_t>(id), moving);
    }
    if (!super_weapon && battle.lead_object() != objects::kNoObjectId) {  // the spawned super weapon (§4.8 lead object)
        const objects::ObjectHandle lh = store.find(battle.lead_object());
        super_weapon = store.alive(lh) && std::find(super_weapon_types_.begin(), super_weapon_types_.end(), store.type(lh)) !=
                                              super_weapon_types_.end();
    }
    super_weapon_report(super_weapon);
    // §3.3 engine-loop requests of the tick (TacticalBattle::engine_loop_requests, hangar::EngineLoops): mode enter,
    // locomotor services, engines offline / online - start = fade-in, stop = fade-out. cu8g t01: HOLSTER_WEAPON / Saber_Throw
    // suppress (0x392410: every engine loop stopped with the request's fade, < 0 = the event's own) / unsuppress (0x393970)
    // and the draw / holster SFX attached to the hero.
    for (const tactical::hangar::EngineLoopRequest& r : battle.engine_loop_requests()) {
        using Op = tactical::hangar::EngineLoopOp;
        const auto id = static_cast<uint64_t>(r.id);
        switch (r.op) {
            case Op::Loop:
                if (r.loop == tactical::hangar::EngineLoop::Idle) game_->set_engine_idle_loop(id, r.start);
                else game_->set_engine_moving_loop(id, r.start);
                break;
            case Op::Suppress: game_->suppress_engine_audio(id, r.fade); break;
            case Op::Unsuppress: game_->clear_engine_audio_suppression(id); break;
            case Op::DrawWeapon:
            case Op::HolsterWeapon: {
                const objects::ObjectHandle h = handle_of(r.id);
                if (!store.alive(h)) break;
                const std::string_view ev =
                    types.get_string(store.type(h), r.op == Op::DrawWeapon ? "SFXEvent_Draw_Weapon" : "SFXEvent_Holster_Weapon");
                if (!ev.empty()) game_->object_sfx(id, ev, store.position(h));
                break;
            }
        }
        ++stats_.engine_loop_requests;
    }
    // HARDPOINT_DISABLED.md §9.1: engines back online (an ENGINE hardpoint repaired): idle on (GameAudio::engines_online).
    for (const objects::ObjectId id : battle.engines_restarted()) {
        game_->engines_online(static_cast<uint64_t>(id));
        ++stats_.engine_restarts;
    }
    std::vector<objects::ObjectId> died;
    for (const tactical::combat::DeathEvent& d : cs.deaths()) {
        const objects::ObjectHandle h = handle_of(d.id);
        if (!store.exists(h)) continue;
        if (game_->unit_death(static_cast<uint64_t>(d.id), sounds(types, store.type(h)).death_start, store.position(h))) ++stats_.deaths;
        moving_.erase(static_cast<objects::ObjectId>(d.id));
        died.push_back(static_cast<objects::ObjectId>(d.id));
        combat = combat || store.owner(h) == local_player || owner_of(d.killer) == local_player;
    }
    // Objects that left without dying (retreat / hyperspace out finished, silent removal): every attached instance
    // killed at once and the object forgotten; dead ones are forgotten too once gone from the battle.
    std::sort(live.begin(), live.end());
    for (auto it = spawned_.begin(); it != spawned_.end();) {
        if (std::binary_search(live.begin(), live.end(), *it)) {
            ++it;
            continue;
        }
        game_->object_removed(static_cast<uint64_t>(*it));
        if (std::find(died.begin(), died.end(), *it) == died.end()) ++stats_.removed;
        moving_.erase(*it);
        inaudible_.erase(static_cast<uint64_t>(*it));
        hidden_.erase(static_cast<uint64_t>(*it));
        it = spawned_.erase(it);
    }
    // §1.2 step 11: the 2D station-upgrade announcement for the local player (own / ally / enemy).
    for (; upgrades_seen_ < battle.station_upgrades().size(); ++upgrades_seen_) {
        const int32_t o = battle.station_upgrades()[upgrades_seen_].owner;
        const char* ev = o == local_player ? "SFXEvent_Starbase_Upgraded"
                         : (o >= 0 && local_player >= 0 && !cs.is_enemy(local_player, o)) ? "SFXEvent_Starbase_Ally_Upgraded"
                                                                                          : "SFXEvent_Starbase_Enemy_Upgraded";
        faction_sfx(ev);
        ++stats_.station_upgrades;
    }
    // L1_S4_REMAINING §4.1 (b0289): a unit that actually entered a garrison this tick -> the local faction's
    // SFXEvent_Bunker_Garrisoned [M: played for the local player's own units only].
    for (const sim::TacticalBattle::StoryGarrisonEvent& e : battle.story_garrisoned()) {
        const std::string_view line = garrison_line(owner_of(e.unit), local_player);
        if (line.empty()) continue;
        faction_sfx(line);
        ++stats_.garrison_lines;
    }
    // HERO_CLASH §3.8 (b0300): a clash end plays the winner's First_ / Second_Hero_Win_Speech (speech queue).
    {
        const auto& ends = battle.hero_clashes().ended();
        const tactical::combat::HeroClashData* cd = battle.hero_clashes().data();
        if (ends.size() < clash_ends_seen_) clash_ends_seen_ = 0;  // cleared (load / new battle)
        for (; clash_ends_seen_ < ends.size(); ++clash_ends_seen_) {
            const auto& e = ends[clash_ends_seen_];
            const std::string_view line = clash_win_speech(cd, e.entry, e.first_won);
            if (line.empty()) continue;
            speech(line);
            ++stats_.clash_win_speech;
        }
    }
    // ccpgu t01 (SKIRMISH_SETUP 4.2): the control point domination countdown lines.
    {
        const int32_t timer = battle.control_point_timer(), leader = battle.control_point_leader();
        const std::string_view line =
            control_point_line(cp_timer_, timer, leader, local_player, leader >= 0 && local_player >= 0 && cs.is_enemy(local_player, leader));
        cp_timer_ = timer;
        if (!line.empty()) {
            faction_sfx(line);
            ++stats_.control_point_lines;
        }
    }
    for (const tactical::combat::HitEvent& e : cs.hit_events()) {
        if (e.projectile_desc < cs.data().projectiles.size()) {
            const objects::TypeIndex pt = types.resolve_name(cs.data().projectiles[e.projectile_desc].name);
            if (pt != objects::kNone) {
                std::string_view ev = e.armor_reduced ? types.get_string(pt, "Projectile_SFXEvent_Detonate_Reduced_By_Armor") : std::string_view();
                if (ev.empty()) ev = sounds(types, pt).projectile_detonate;
                if (!ev.empty() && game_->impact(ev, v3(e.position))) ++stats_.hits;
            }
        }
        combat = combat || e.owner == local_player || (e.victim != tactical::combat::kNoObject && owner_of(e.victim) == local_player);
    }
    uint32_t max_serial = last_serial_;
    for (size_t i = 0; i < cs.projectile_count(); ++i) {
        const tactical::combat::ProjectileView v = cs.projectile(i);
        if (have_serial_ && v.serial <= last_serial_) continue;
        max_serial = std::max(max_serial, v.serial);
        const objects::ObjectHandle sh = handle_of(v.shooter);
        if (store.exists(sh)) {
            const std::string& ev = sounds(types, store.type(sh)).fire;
            if (!ev.empty() && game_->weapon_fire(static_cast<uint64_t>(v.shooter), ev, store.position(sh))) ++stats_.fires;
        }
        combat = combat || v.owner == local_player;
    }
    last_serial_ = max_serial;
    have_serial_ = true;
    tsw_audio(tsw_cues(battle.special_weapon_events(), store), types);  // refix t06
    if (combat) {
        audio::CombatEvent ce;
        ce.local_player_involved = true;
        game_->combat(ce);
        ++stats_.combat_notes;
    }
}

void GameAudioHost::super_weapon_report(bool present) {
    const bool first = !battle_reported_;
    battle_reported_ = true;
    if (game_) game_->set_super_weapon_present(present, first);
}

void GameAudioHost::tsw_audio(std::span<const TswCue> cues, const objects::GameObjectTypes& types) {
    if (!game_) return;
    for (const TswCue& c : cues) {
        if (c.type == objects::kNone || c.type >= types.size()) continue;
        const uint64_t id = static_cast<uint64_t>(c.object);
        const Vec3 p = v3(c.position);
        switch (c.kind) {
            case TswCue::Kind::Firing:
                game_->object_sfx(id, types.get_string(c.type, "TSW_SFXEvent_Activate_Voice"), p);
                game_->object_sfx(id, types.get_string(c.type, "TSW_SFXEvent_Weapon_Power_Up"), p);
                game_->super_weapon_activation(types.get_string(c.type, "TSW_MusicEvent_Activation_Start"));
                ++stats_.tsw_activations;
                break;
            case TswCue::Kind::AttackAnim:
                game_->weapon_fire(id, sounds(types, c.type).fire, p);
                ++stats_.tsw_fires;
                break;
            case TswCue::Kind::PlanetDestroyed:
                game_->unit_death(id, sounds(types, c.type).death_start, p);
                ++stats_.tsw_planet_deaths;
                break;
            case TswCue::Kind::LobChargeup:  // S8 t31
                game_->object_sfx(id, types.get_string(c.type, "Lobbing_Superweapon_SFXEvent_Chargeup"), p);
                ++stats_.lob_chargeups;
                break;
            case TswCue::Kind::LobCharged:
                game_->object_sfx(id, types.get_string(c.type, "Lobbing_Superweapon_SFXEvent_Fire"), p);
                ++stats_.lob_charged;
                break;
        }
    }
}

void GameAudioHost::sim_tick() {
    if (game_) game_->sim_tick();
}

void GameAudioHost::music(MusicScene scene, std::string_view faction, bool space, std::string_view environment) {
    if (!game_) return;
    if (!faction.empty()) game_->set_local_faction(faction);
    if (scene == scene_) return;
    const MusicScene before = scene_;
    scene_ = scene;
    const bool pending = pending_played_;
    pending_played_ = false;
    ++stats_.scenes;
    if (before == MusicScene::Battle || before == MusicScene::Galactic) game_->leave_mode();
    switch (scene) {
        case MusicScene::None: game_->leave_mode(); break;
        case MusicScene::MainMenu: game_->enter_front_end(); break;
        case MusicScene::Galactic: game_->enter_galactic(); break;
        case MusicScene::LoadScreen:
            if (!pending) game_->battle_pending(!space);  // the prompt played it already
            game_->battle_load_screen(true);
            break;
        case MusicScene::Battle: {
            game_->battle_load_screen(false);
            audio::TacticalSetup ts;
            ts.land = !space;
            ts.environment = std::string(environment);
            game_->enter_tactical(ts);
            break;
        }
    }
}

void GameAudioHost::set_listener(const Vec3& eye, const Vec3& look_at, bool cinematic) {
    if (!game_) return;
    Vec3 f{look_at.x - eye.x, look_at.y - eye.y, look_at.z - eye.z};
    const float len = std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z);
    f = len > 0.0f ? Vec3{f.x / len, f.y / len, f.z / len} : Vec3{0.0f, 1.0f, 0.0f};
    audio::CameraState c;
    c.position = eye;
    c.look_at = look_at;
    c.forward = f;
    c.up = {0.0f, 0.0f, 1.0f};
    c.cinematic = cinematic;
    game_->set_camera(c);
}

// --- OI host: audio game-state flags and the device latency ---
void GameAudioHost::set_flags(bool modal, bool gui_only, bool attract) {
    if (modal == modal_ && gui_only == gui_only_ && attract == attract_) return;
    if (attract && !attract_) ++stats_.attract_starts;
    const bool attract_changed = attract != attract_;
    modal_ = modal;
    gui_only_ = gui_only;
    attract_ = attract;
    ++stats_.flag_changes;
    if (!system_) return;
    system_->set_modal_dialog(modal);
    system_->set_gui_only(gui_only);
    system_->set_attract_mode(attract);
    // STRATEGIC_GAPS §7.4: the music lock is set when the attract loop starts and cleared by the front-end action that
    // ends it [port: the attract demo ending]; while set, unforced front-end (mode 13) music is refused.
    if (attract_changed) system_->set_music_lock(attract);
}

double GameAudioHost::output_latency_seconds() const { return backend_ ? backend_->output_latency_seconds() : 0.0; }
// --- end OI host ---

void GameAudioHost::set_volumes(float master, float sfx, float speech, float music) {
    if (!system_) return;
    system_->set_bus_level(audio::AudioBus::Master, master);
    system_->set_bus_level(audio::AudioBus::Sfx, sfx);
    system_->set_bus_level(audio::AudioBus::Speech, speech);
    system_->set_bus_level(audio::AudioBus::Music, music);
}

void GameAudioHost::update(double dt) {
    if (game_) game_->update(dt);
}

void GameAudioHost::apply_presentation(const std::vector<lua::PresentationEvent>& events, uint32_t* next_seq) {
#ifdef ALAMO_AUDIO_HAVE_LUA
    if (game_ && next_seq)
        audio::apply_presentation_audio(*game_, events, next_seq, [this](int32_t id) -> std::optional<Vec3> {
            const auto it = positions_.find(static_cast<uint64_t>(id));
            if (it == positions_.end()) return std::nullopt;
            return it->second;
        });
#else
    (void)events;
    (void)next_seq;
#endif
}

#else  // no engine/audio: inactive host

bool GameAudioHost::init(const GameData&, AudioBackendKind kind, uint32_t seed, std::string* err) {
    rng_.seed(seed);
    if (kind != AudioBackendKind::Off && err) *err = "built without engine/audio";
    return kind == AudioBackendKind::Off;
}
const audio::UnitSounds& GameAudioHost::sounds(const objects::GameObjectTypes&, objects::TypeIndex) {
    static const audio::UnitSounds none;
    return none;
}
void GameAudioHost::ui(std::string_view) {}
void GameAudioHost::speech(std::string_view) {}
void GameAudioHost::faction_sfx(std::string_view) {}
void GameAudioHost::negative_feedback() {}
void GameAudioHost::battle_pending(bool) { pending_played_ = true; }  // same bookkeeping as with audio
void GameAudioHost::galactic_resume() { pending_played_ = false; }
void GameAudioHost::movie_started(bool movie_music, bool halt_music) {  // same bookkeeping as with audio (no music here)
    movie_halted_ = halt_music;
    movie_music_on_ = movie_music && !halt_music;
}
void GameAudioHost::movie_cue(std::string_view) {}
void GameAudioHost::movie_ended() { movie_music_on_ = movie_halted_ = false; }
void GameAudioHost::unit_response(const objects::GameObjectTypes&, objects::TypeIndex, uint64_t, UnitResponse, bool, int32_t) {}
void GameAudioHost::battle_tick(const sim::TacticalBattle&, const objects::ObjectStore&, const objects::GameObjectTypes&, int32_t,
                                const std::function<bool(objects::ObjectId)>&) {}
void GameAudioHost::battle_end(bool, std::string_view) {}
void GameAudioHost::tsw_audio(std::span<const TswCue>, const objects::GameObjectTypes&) {}
void GameAudioHost::sim_tick() {}
void GameAudioHost::super_weapon_report(bool) { battle_reported_ = true; }
void GameAudioHost::music(MusicScene scene, std::string_view, bool, std::string_view) {
    if (scene != scene_) pending_played_ = false;
    scene_ = scene;
}
void GameAudioHost::set_listener(const Vec3&, const Vec3&, bool) {}
void GameAudioHost::set_flags(bool modal, bool gui_only, bool attract) {  // same bookkeeping as with audio
    if (modal == modal_ && gui_only == gui_only_ && attract == attract_) return;
    if (attract && !attract_) ++stats_.attract_starts;
    modal_ = modal;
    gui_only_ = gui_only;
    attract_ = attract;
    ++stats_.flag_changes;
}
double GameAudioHost::output_latency_seconds() const { return 0.0; }
void GameAudioHost::set_volumes(float, float, float, float) {}
void GameAudioHost::update(double) {}
void GameAudioHost::apply_presentation(const std::vector<lua::PresentationEvent>&, uint32_t*) {}

#endif

// Shared by both builds (cu9 t09): the same bookkeeping with and without engine/audio (nothing reads it without).
void GameAudioHost::battle_reset() {
    last_serial_ = 0;
    have_serial_ = false;
    spawned_.clear();
    moving_.clear();
    inaudible_.clear();
    hidden_.clear();
    battle_reported_ = false;
    clash_ends_seen_ = 0;
    upgrades_seen_ = 0;
    cp_timer_ = -1;
    retreating_player_ = -1;
}

void GameAudioHost::set_object_positions(std::vector<std::pair<uint64_t, Vec3>> positions) {
    positions_.clear();
    for (const auto& [id, p] : positions) positions_[id] = p;
}

std::vector<std::string> GameAudioHost::take_speech_done() {
    std::vector<std::string> v;
    v.swap(speech_done_);
    return v;
}

GameAudioHost::GameAudioHost() = default;
GameAudioHost::~GameAudioHost() {
    game_.reset();    // GameAudio references the system, the data and the RNG
    system_.reset();  // before the backend / database / samples it references
    backend_.reset();
}

}  // namespace alamo::game
