// GameAudio: the per-frame event-hook API a game host (engine/apps/game) calls. It turns game happenings
// into AUDIO.md behaviour on top of AudioSystem (SFX / music / speech player), AmbienceManager (weather and
// map ambience) and ConversationPlayer (speech sequences). STABLE API: P3 published 2026-10-27; additions
// only.
//
// Ownership: the host owns AudioDatabase, GameAudioData, the VoiceBackend, the free RNG and AudioSystem;
// GameAudio keeps references (they must outlive it) and owns the ambience and conversation managers.
//
// Threading / determinism (AUDIO.md §0.2, §6.1): call everything from the one audio/main thread. Audio
// only uses GameRandom::free() and never writes sim state. Sim-side happenings (weapon fire, impacts,
// deaths, combat notifications) are collected by the host during the tick and forwarded here after the
// tick, in a stable order (object index / event order), so the free-RNG consumption is repeatable.
//
// Frame (MAIN_LOOP step 2 + step 14, only while the application is active):
//     audio.set_camera(camera);           // listener (§1.8)
//     audio.set_weather(type, intensity); // tactical only
//     ... forward this frame's events (battle, UI, Lua presentation log) ...
//     for each sim tick run this frame: audio.sim_tick();   // music peace timer, announcement delays
//     audio.update(wall_dt);              // AudioSystem::update, conversations, ambience, object loops
//
// Typical calls:
//     front end      : enter_front_end()                    -> Main_Menu_Music_Event
//     galactic       : set_local_faction("Empire"); enter_galactic()
//     battle pending : battle_pending(land)                 -> faction pending music + SFX (mode 5)
//     load screen    : battle_load_screen(true/false)       -> load music, 3D SFX muted behind it
//     tactical       : enter_tactical({land, "Temperate", super_weapon}) -> ambient music (6), ambience
//     combat         : combat({local_involved, local_base_attacked})   -> battle music (7), announcements
//     battle end     : battle_end({won, enemy_faction})     -> win/lose music (mode 16)
//     leaving a mode : leave_mode()                         -> stop all SFX and speech (§1.5, §3.1)
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "alamo/audio/ambience.hpp"
#include "alamo/audio/audio_system.hpp"
#include "alamo/audio/conversation.hpp"
#include "alamo/audio/game_audio_data.hpp"
#include "alamo/data/xml.hpp"

namespace alamo::audio {

// Unit orders that have a response line (GameObject SFXEvent_* tags, AUDIO.md §3.3).
enum class UnitOrder : uint8_t {
    Select, Move, FleetMove, Attack, Guard, Defend, Stop, Deploy, Undeploy, MoveOpposite,
    GroupMove, GroupAttack, AssistMove, AssistAttack, Count
};
inline constexpr size_t kUnitOrderCount = static_cast<size_t>(UnitOrder::Count);

// A unit type's sound events, read from its GameObject XML element (unit_sounds_from_xml) or filled by the
// host. Empty string = the type has none.
struct UnitSounds {
    std::array<std::string, kUnitOrderCount> response;  // SFXEvent_Select, _Move, _Fleet_Move, ...
    std::string select_with_non_hero_ability, move_with_non_hero_ability, attack_with_non_hero_ability;
    std::string unit_lost, unit_under_attack;            // SFXEvent_Unit_Lost / _Unit_Under_Attack
    std::string fire;                                    // SFXEvent_Fire
    std::string death_start, death_end;                  // Death_SFXEvent_Start_Die / _End_Die
    std::string ambient_loop;                            // SFXEvent_Ambient_Loop
    std::string ambient_moving;                          // SFXEvent_Ambient_Moving
    float ambient_moving_min_seconds = 0.0f, ambient_moving_max_seconds = 0.0f;
    std::string engine_idle_loop, engine_moving_loop;    // SFXEvent_Engine_Idle_Loop / _Moving_Loop
    std::string engine_cinematic_focus_loop;             // SFXEvent_Engine_Cinematic_Focus_Loop (only ever stopped)
    std::string projectile_detonate;                     // Projectile_SFXEvent_Detonate (projectile types)
};
// Reads the tags above from a GameObject type element. `base` = the type it is a variant of (its values
// are the defaults); tags present in `type` override them.
UnitSounds unit_sounds_from_xml(data::XmlNode type, const UnitSounds* base = nullptr);

struct CameraState {
    Vec3 position{};
    Vec3 look_at{};
    Vec3 forward{0.0f, 0.0f, 1.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};
    bool cinematic = false;  // cinematic camera: listener at the camera, gate 2/9 + visibility rules (§1.2, §1.6)
};

struct TacticalSetup {
    bool land = true;                // false = space battle
    std::string environment;         // Temperate, Arctic, Desert, Forest, Swamp, Volcanic, Urban (land); ignored in space
    bool super_weapon_present = false;  // any player has its faction super weapon on the map (§2.3 mode 6/7)
};

struct CombatEvent {
    // The Land/Space slot-76 notification (§2.3): either object belongs to the local player, or is an enemy
    // the local player can see. Starts battle music and resets the peace counter.
    bool local_player_involved = false;
    // The victim is one of the local player's base structures: base-under-attack announcement (faction
    // SFXEvent_<Land/Space>_Base_Under_Attack_Announcement), rate-limited by Audio.xml
    // Delay_Between_<Land/Space>_Base_Attack_Announcement_Seconds x 30 sim ticks. The host does the radar ping.
    bool local_base_attacked = false;
};

struct BattleResult {
    bool won = false;
    std::string enemy_faction;  // picks Music_Event_<Tactical/Strategic>_<Win/Lose>_Vs_Faction first
    bool strategic = false;     // campaign won/lost (Music_Event_Strategic_*) instead of a battle
};

// Music modes: audio_system.hpp music_mode (STRATEGIC_GAPS §7.4). Front end / credits = 13, splash = 11,
// battle load screen = 15, battle / campaign end = 16, super weapon activation = 8, production complete = 4,
// movie music = 14, movie cues = 17. (P3's first choice "front end = 8, battle end = 11" was wrong.)

class GameAudio {
public:
    GameAudio(AudioSystem& audio, const GameAudioData& data, SimRng& free_rng, AmbienceConfig ambience = {});
    ~GameAudio();
    GameAudio(const GameAudio&) = delete;
    GameAudio& operator=(const GameAudio&) = delete;

    // ---------------- session / modes / music state
    void set_local_faction(std::string_view faction);  // Faction name (Rebel, Empire, Underworld, ...)
    const std::string& local_faction() const { return faction_; }
    // Front-end menu music, mode 13 (main menu, staging / campaign / internet dialogs, "Game_Credits_Music_Event"
    // [port: credits in 13]); refused while the attract lock is set (AudioSystem::set_music_lock).
    void enter_front_end(std::string_view music_event = "Main_Menu_Music_Event");
    // Splash screen music, mode 11 (excluded from automatic mode music).
    void splash_screen(std::string_view music_event = "Splash_E3_Music_Event");
    // Tactical super weapon activation (the TSW type's TSW_MusicEvent_Activation_Start), mode 8 (excluded from
    // automatic mode music, so combat / peace switches do not replace it until the next mode entry).
    void super_weapon_activation(std::string_view music_event);
    // Production complete (the built type's Build_Music_Completed), mode 4, resumes the previous music after it.
    void build_complete(std::string_view music_event);
    // A movie with the movie-music flag starts: mode 14 (one pass of a looping playlist, then the previous music).
    void movie_music(std::string_view music_event);
    // A music cue embedded in a playing movie, mode 17.
    void movie_music_cue(std::string_view music_event);
    // [port] The movie that started mode 14 / 17 music ended (or was skipped): that music stops and the music it replaced
    // comes back. False when no movie music was playing (the one pass already ended and resumed on its own).
    bool movie_music_end();
    // Movies.xml Halt_Game_Music (flag +0xD4): every music stream stops while the movie plays.
    void halt_game_music();
    // Galactic mode entered or resumed: listener mode galactic, local faction Strategic_Map_Music_Event (mode 1).
    void enter_galactic();
    // Tactical battle pending/loading (mode 5): Music_Event_Tactical_<Land/Space>_Battle_Pending and the
    // SFX_Event_Tactical_<Land/Space>_Battle_Pending announcement.
    void battle_pending(bool land);
    // Battle load screen open/closed: Music_Event_Battle_Load_Screen (faction, else Audio.xml) in mode 15;
    // 3D SFX are silent while it is open (§1.6 global 3D scale).
    void battle_load_screen(bool open);
    // Tactical mode started/resumed: listener mode land/space, ambient music (mode 6), weather/map ambience.
    void enter_tactical(const TacticalSetup& setup);
    // §2.3 (b0286): whether any player has its faction super weapon on the map; mode 6 / 7 music then picks the faction's
    // Music_Event_<Land/Space>_<Ambient/Battle>_Super_Weapon. `restart_music`: the battle's first report (the host learns
    // the start forces one tick after enter_tactical) - a change restarts the current mode's music with the new choice;
    // otherwise the flag is read at the next mode switch, as retail reads it when it picks the event.
    void set_super_weapon_present(bool present, bool restart_music = false);
    bool super_weapon_present() const { return super_weapon_; }
    // Battle / campaign end music (mode 16, forced); combat notifications stop changing music afterwards.
    void battle_end(const BattleResult& result);
    // Battle-end summary dialog: Audio.xml Music_Event_Battle_End_Summary_Screen_<Win/Lose> (mode 16).
    void battle_summary_screen(bool won);
    // Mode push/pop, load, quit (§1.5, §3.1): stops every SFX instance, kills speech and conversations,
    // ends the tactical ambience. Music keeps playing until the next mode picks its own.
    void leave_mode();
    void set_music_enabled(bool on) { audio_->set_music_enabled(on); }

    // ---------------- per frame
    void set_camera(const CameraState& camera);
    // Current weather from the sim (tactical): CLEAR/RAIN/SNOW/SANDSTORM/ASH and 0..1 intensity. Also plays
    // the faction's SFXEvent_Weather_Begin/End announcement when weather (type != CLEAR and intensity > 0)
    // starts / stops [inferred].
    void set_weather(std::string_view weather, float intensity);
    void sim_tick();                  // once per sim tick (music peace counter, announcement delays)
    void update(double wall_dt_seconds);

    // ---------------- battle events (sim -> audio, forwarded after the tick in a stable order)
    // Weapon / hard point fire: attached to the shooter (3D-attached when the event Is_3D). Suppressed by
    // Audio.xml Test_Disable_Weapon_Fire_SFX (§6.2). Returns the instance id (0 = refused).
    uint32_t weapon_fire(uint64_t shooter, std::string_view event, const Vec3& position);
    // Projectile detonation / impact and explosions: 3D at a world position (fog gate 9 applies).
    uint32_t impact(std::string_view event, const Vec3& position);
    uint32_t explosion(std::string_view event, const Vec3& position) { return impact(event, position); }
    // Death: every sound attached to the object is stopped (DeathBehavior, §1.5), then the death event
    // (Death_SFXEvent_Start_Die) plays at the death position [port: positional, so it survives the object's
    // removal]. Same as object_dead (the dead bit blocks engine loop restarts) followed by the death event.
    uint32_t unit_death(uint64_t object, std::string_view event, const Vec3& position);
    // Any other object-attached event (abilities, shields, hyperspace, production, Lua Play_SFX_Event, ...).
    uint32_t object_sfx(uint64_t object, std::string_view event, const Vec3& position, float fade_in_seconds = 0.0f);
    void stop_object_sfx(uint64_t object, std::string_view event, float fade_seconds = 0.0f);
    // Any other world-position 3D event.
    uint32_t world_sfx(std::string_view event, const Vec3& position);
    // Animation events (AnimationSFXMaps.txt SOUND rows, §6.2): every row of `animation` with
    // prev_ms < time <= now_ms plays attached to the object (prev_ms = -1 on the first frame; a wrapped loop
    // is two calls). SURFACE rows are for the particle host (data().animation_sfx()). Returns the number of
    // instances started.
    uint32_t animation_sounds(uint64_t object, std::string_view animation, int64_t prev_ms, int64_t now_ms,
                              const Vec3& position);
    // Combat notification (see CombatEvent).
    void combat(const CombatEvent& e);

    // ---------------- objects (loops and ambient chatter)
    // Object created / entered the tactical map: SFXEvent_Ambient_Loop starts attached with its
    // Loop_Fade_In_Seconds, then engine_mode_enter (idle loop on, moving loop off).
    void object_spawned(uint64_t object, const UnitSounds& sounds, const Vec3& position);
    // Locomotor service result (see engine_service); additionally, while moving, SFXEvent_Ambient_Moving plays
    // every float(min, max) seconds [inferred: AmbientSFXBehavior fields], the timer re-drawn when moving
    // turns on. Idempotent: call it every service, not only on changes.
    void object_moving(uint64_t object, bool moving);
    // Object deleted, or left the battle (retreat / hyperspace out finished): every attached instance is
    // killed at once without fade (§1.5) and the object is forgotten (later calls for it do nothing).
    void object_removed(uint64_t object);

    // ---------------- engine loops (STATION_HANGARS_SOUND.md §3)
    // Two slots per object, idle and moving, each with a requested-state flag. Set_Engine_*_Loop(on) (§3.2):
    // nothing while the object is dead or its engine audio is suppressed; on with flag 0 starts the event
    // attached with its Loop_Fade_In_Seconds, off with flag 1 stops it with Loop_Fade_Out_Seconds. The flag
    // changes even when the type has no event or the start is refused by a gate (it is the requested state,
    // not "audible"; a refused start is retried only when the flag goes 0 -> 1 again). Objects not
    // registered by object_spawned are ignored. A stop is a fade-out: the fading instance no longer counts as
    // attached nor for Max_Instances, so a restart during the fade starts a second instance (crossfade).
    // Fog / hidden / stealth only mute running loops (ObjectAudio::audible); camera distance only attenuates
    // and decides 3D voice stealing (AudioSystem). Neither starts nor stops loops.
    void set_engine_idle_loop(uint64_t object, bool on);
    void set_engine_moving_loop(uint64_t object, bool on);
    // SimpleSpace (and land locomotor) service, after the motion step, outside hyperspace states (§3.3):
    // moving := speed >= 1.1 x SpaceIdleMovementSpeed (the sim decides). Moving call first, then idle.
    void engine_service(uint64_t object, bool moving);
    // Object enters the active tactical mode (creation, reinforcement, hyperspace-in start, load): idle on,
    // moving off.
    void engine_mode_enter(uint64_t object);
    // Last engine hardpoint destroyed, timed engine disable, land transform: moving off, idle off.
    void engines_offline(uint64_t object);
    // Engines repaired to full (HARDPOINT_DISABLED §9.1): idle on.
    void engines_online(uint64_t object);
    // §3.4 suppression (+0x3B9): stops moving, idle and cinematic-focus loops with `fade_seconds` (each event's
    // own Loop_Fade_Out_Seconds when < 0), clears both flags, sets suppressed. Clearing it restarts nothing.
    void suppress_engine_audio(uint64_t object, float fade_seconds = -1.0f);
    void clear_engine_audio_suppression(uint64_t object);
    // Death (DeathBehavior): every attached instance killed at once, no fade; the dead bit then blocks engine
    // restarts. The object stays known (flags readable) until object_removed.
    void object_dead(uint64_t object);
    struct EngineLoopState {
        bool idle = false, moving = false;  // requested-state flags (+0x3B6 / +0x3B5)
        bool dead = false, suppressed = false;
    };
    std::optional<EngineLoopState> engine_loop_state(uint64_t object) const;
    // Position/visibility of attached instances, queried by AudioSystem every update (§1.4, §1.6).
    void set_object_query(std::function<std::optional<AudioSystem::ObjectAudio>(uint64_t)> q) {
        audio_->set_object_query(std::move(q));
    }
    // Gate 9: 3D-positional starts fail under the local player's fog of war.
    void set_position_fogged(std::function<bool(const Vec3&)> f) { audio_->set_position_fogged(std::move(f)); }

    // ---------------- UI and local-player feedback (2D)
    // Unit response on a local selection or order (§3.3): `representative` is the one unit the UI picked
    // from the selection; group = more than one unit selected (Group_Move / Group_Attack when the type has
    // them); non_hero_ability = the selection has a non-hero ability active (*_With_Non_Hero_Ability).
    // FleetMove falls back to Move. Plays attached 2D (priority/overlap/max-instance rules come from the
    // event data). Returns the instance id (0 = refused or the type has no line).
    uint32_t unit_response(uint64_t representative, const UnitSounds& sounds, UnitOrder order,
                           const Vec3& position, bool group = false, bool non_hero_ability = false);
    // Command bar order sound (Audio.xml SFXEvent_Command_Bar_*): Attack, Move, Guard, Stop; attack_move /
    // waypoint variants via the flags.
    uint32_t command_bar_sfx(UnitOrder order, bool attack_move = false, bool waypoint = false);
    // GUI sounds by name (Click_SFX, Mouse_Over_SFX, ...): 2D, pumped once so clicks play this frame (§1.2).
    uint32_t gui_sfx(std::string_view event);
    void negative_feedback();  // Audio.xml SFXEvent_GUI_Negative_Feedback
    // Local faction announcement by its Faction tag (e.g. "SFXEvent_Planet_Gained_Control",
    // "Reinforcements_Enroute_SFXEvent"): 2D; HUD VO rules apply (refused while speech plays, §1.2 test 5).
    uint32_t faction_sfx(std::string_view faction_tag);
    // Enemy spotted (faction SFXEvent_Unit_Type_Spotted for the type, else SFXEvent_Enemy_Spotted); gated by
    // Lua SFXManager.Allow_Enemy_Sighted_VO [inferred].
    uint32_t enemy_spotted(std::string_view object_type_name);
    void allow_enemy_sighted_vo(bool on) { enemy_sighted_vo_ = on; }

    // ---------------- speech
    uint32_t speech(std::string_view event) { return audio_->queue_speech(event); }  // FIFO ticket (§3.1)
    uint32_t conversation(Conversation c) { return conversations_.start(std::move(c)); }

    // ---------------- Lua presentation ops that need more than AudioSystem (lua_audio.hpp routes them)
    void weather_audio_pause(bool paused) { ambience_.set_paused(paused); }
    void master_volume_restore() { audio_->master_volume_restore(); }

    // ---------------- access
    AudioSystem& audio() { return *audio_; }
    AmbienceManager& ambience() { return ambience_; }
    ConversationPlayer& conversations() { return conversations_; }
    const GameAudioData& data() const { return *data_; }
    bool in_tactical() const { return tactical_; }

private:
    struct ObjectState {
        UnitSounds sounds;
        Vec3 position{};
        uint32_t ambient_loop = 0;
        EngineLoopState engine;
        bool moving = false;
        double moving_timer = 0.0;
    };
    void set_engine_loop(ObjectState& o, uint64_t object, bool moving_slot, bool on);
    std::string resolve_music(uint8_t mode);
    std::string pick(const std::vector<std::string>& list);
    float loop_fade(std::string_view event, bool in) const;
    uint32_t play_2d(std::string_view event, uint32_t pump = 0);

    AudioSystem* audio_;
    const GameAudioData* data_;
    SimRng* rng_;
    AmbienceManager ambience_;
    ConversationPlayer conversations_;
    std::string faction_;
    bool tactical_ = false, land_ = true, super_weapon_ = false, battle_over_ = false;
    std::string environment_;
    bool weather_active_ = false;
    std::string weather_type_ = "CLEAR";
    uint64_t sim_ticks_ = 0, last_base_announcement_ = 0;
    bool base_announced_ = false;
    bool enemy_sighted_vo_ = true;
    std::map<uint64_t, ObjectState> objects_;  // id-ordered: deterministic iteration
};

}  // namespace alamo::audio
