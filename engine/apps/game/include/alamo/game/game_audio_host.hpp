// P host / P3 hookup: drives engine/audio (AudioSystem + the game-host layer audio::GameAudio) from the game.
// Presentation only: it reads the sim between ticks through const references, never writes it, and every random
// draw is on its own FREE (unsynced) SimRng - so the sim's per-tick CRC is the same with audio on or off.
//   UI            GUI / HUD SFX hooks -> GameAudio::gui_sfx.
//   unit response the local player's select / move / attack / guard / stop order -> GameAudio::unit_response on the
//                 representative unit = the lowest id of the order's units [port; AUDIO.md 3.3 M]; played before the
//                 command is submitted (local only).
//   battle events after every battle tick, in a stable order: new battle units -> object_spawned (engine / ambient
//                 loops) and object_moving from the movement state (ascending id); deaths -> unit_death
//                 (Death_SFXEvent_Start_Die); projectile hits -> impact (Projectile_SFXEvent_Detonate); new projectiles
//                 -> weapon_fire (shooter's SFXEvent_Fire [port: the hardpoint's Fire_SFXEvent is not in the battle
//                 catalog]); any of them involving the local player -> combat (battle music, mode 7); the tactical super
//                 weapon's cues (battle_fx.hpp tsw_cues -> tsw_audio).
//   modes         front end / galactic / battle pending + load screen / tactical (enter_tactical) / leave_mode.
//   listener      set_camera from the active camera (AUDIO.md 1.8).
// Unit sounds come from the typed object view (GameObjectTypes::get_string), cached per type.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "alamo/core/sim_rng.hpp"
#include "alamo/game/battle_fx.hpp"
#include "alamo/game/game_data.hpp"
#include "alamo/objects/object_store.hpp"

namespace alamo::audio {
class AudioDatabase;
class AudioSystem;
class VoiceBackend;
class SampleSource;
class GameAudioData;
class GameAudio;
struct UnitSounds;
}  // namespace alamo::audio
namespace alamo::sim {
class TacticalBattle;
}
namespace alamo::lua {
struct PresentationEvent;
}

namespace alamo::game {

enum class AudioBackendKind : uint8_t { Device = 0, Null = 1, Off = 2 };
enum class UnitResponse : uint8_t { Select, Move, FleetMove, Attack, Guard, Stop };
enum class MusicScene : uint8_t { None, MainMenu, Galactic, LoadScreen, Battle };

struct GameAudioStats {
    uint64_t ui = 0, responses = 0, deaths = 0, hits = 0, fires = 0, combat_notes = 0, spawned = 0, scenes = 0, speech = 0, engine_restarts = 0;
    uint64_t feedback = 0;  // P4 host: faction_sfx / negative_feedback calls
    uint64_t flag_changes = 0, attract_starts = 0;  // OI host: set_flags changes / attract demo starts
    uint64_t excluded = 0, battle_end_music = 0;    // responses skipped for retreating units / battle_end calls
    uint64_t battle_pending = 0;                    // GALACTIC_DIALOGS §1.3 battle prompt sound + music
    uint64_t engine_loop_requests = 0, removed = 0, station_upgrades = 0;  // STATION_HANGARS_SOUND §1.2 / §3
    uint64_t movie_music = 0, movie_halts = 0, movie_cues = 0, movie_restores = 0;  // movie music modes 14 / 17 (STRATEGIC_GAPS §7.4)
    uint64_t tsw_activations = 0, tsw_fires = 0, tsw_planet_deaths = 0;  // refix t06: tsw_audio cues played
};

class GameAudioHost {
public:
    GameAudioHost();
    ~GameAudioHost();
    GameAudioHost(const GameAudioHost&) = delete;
    GameAudioHost& operator=(const GameAudioHost&) = delete;

    // Loads the audio databases through data.files; the device / null backend; `seed` seeds the free RNG.
    // Off (or no engine/audio) -> inactive (every call is a no-op). False + err when a requested backend failed.
    bool init(const GameData& data, AudioBackendKind backend, uint32_t seed, std::string* err = nullptr);
    bool active() const { return game_ != nullptr; }
    audio::AudioSystem* system() { return system_.get(); }  // movie streams (null when inactive)

    void ui(std::string_view event);
    void speech(std::string_view event);  // a queued speech event (story dialog scripts' DIALOG lines)
    // L1/S4 wave 1: speech events that ended since the last call (AudioSystem::on_speech_done, finished or killed by
    // Stop_All_Speech / a mode change - AUDIO.md §3.1 reports both to the story), in end order. Empty when inactive.
    std::vector<std::string> take_speech_done();
    // P4 host: the local faction's announcement by its Faction tag (GameAudio::faction_sfx, e.g.
    // SFXEvent_Sabotage_Success) and the GUI negative feedback (the special systems' local error SFX, [port]).
    void faction_sfx(std::string_view faction_tag);
    void negative_feedback();
    // owner = the unit's player (-1 unknown): units of the battle's retreating player give no response (the mode's
    // "excluded set" = the retreat coordinator's retreating units, STRATEGIC_GAPS §7.4 [M]; stats().excluded).
    void unit_response(const objects::GameObjectTypes& types, objects::TypeIndex type, uint64_t object, UnitResponse kind,
                       bool group = false, int32_t owner = -1);
    // Battle-end dialog opened: the local faction's win / lose music (Music_Event_Tactical_<Win/Lose>[_Vs_Faction]),
    // music mode 16 (STRATEGIC_GAPS §7.4).
    void battle_end(bool won, std::string_view enemy_faction = {});
    // GALACTIC_DIALOGS §1.3: the battle prompt opened - the local faction's SFX_Event_Tactical_{Space,Land}_Battle_Pending
    // and the pending-battle music (GameAudio::battle_pending, mode 5); the load screen after Begin does not repeat them.
    // galactic_resume: back to the galactic mode music after an auto-resolve's battle-end screen (the scene stayed Galactic).
    void battle_pending(bool land);
    void galactic_resume();
    // Movie music (STRATEGIC_GAPS §7.3 / §7.4), called by MovieSlot for the movie's Movies.xml flags: movie_started with the
    // movie-music flag (Has_Credits, +0xD0) starts mode 14 music (kCreditsMusicEvent [M for the name]; one pass, then the
    // previous music resumes), with Halt_Game_Music (+0xD4) stops the game's music; movie_cue = a MusicEvent_Frame cue
    // (mode 17); movie_ended = the movie stopped (finished, skipped, replaced): its music stops now and the music it
    // replaced restarts, a halted movie's silence ends with the mode-based music [port]. Movies without those flags
    // (the logo / intro movies, the advisor loops) never reach here and leave the music alone.
    static constexpr const char* kCreditsMusicEvent = "Game_Credits_Music_Event";
    void movie_started(bool movie_music, bool halt_music);
    void movie_cue(std::string_view music_event);
    void movie_ended();
    // After every tick of a running battle (const reads).
    void battle_tick(const sim::TacticalBattle& battle, const objects::ObjectStore& store, const objects::GameObjectTypes& types,
                     int32_t local_player);
    void battle_reset();
    // refix t06 (TSW_PLANET_DESTROY §3 / §4 step 5; battle_fx.hpp): the tactical super weapon's sounds, in cue order.
    // Firing: the TSW type's TSW_SFXEvent_Activate_Voice and TSW_SFXEvent_Weapon_Power_Up attached to the TSW (2D or 3D
    // by the event's data) and TSW_MusicEvent_Activation_Start as music mode 8; AttackAnim: its SFXEvent_Fire (weapon
    // fire rules); PlanetDestroyed: the target dummy's Death_SFXEvent_Start_Die at the dummy (unit_death: the dummy's own
    // sounds stop). Every owner's TSW is heard [M: the spec lists no owner test]. battle_tick calls it.
    void tsw_audio(std::span<const TswCue> cues, const objects::GameObjectTypes& types);
    void sim_tick();  // once per sim tick

    // Mode / music scene with the local faction ("" keeps the previous); `space` / `environment` for battles.
    void music(MusicScene scene, std::string_view faction = {}, bool space = true, std::string_view environment = {});

    // Positions of objects for attached instances (copied from the render snapshot after each capture).
    void set_object_positions(std::vector<std::pair<uint64_t, Vec3>> positions);
    // `cinematic` = a Lua cinematic camera is active (AUDIO.md §1.2 / §1.6: listener at the camera, gates 2 / 9).
    void set_listener(const Vec3& eye, const Vec3& look_at, bool cinematic = false);
    // OI host: the game-state flags the AudioSystem gates and gains read (AUDIO.md §1.1 gate 4, §1.6, §3), owned by the
    // host. modal = a modal GUI dialog is open (speech ducking off); gui_only = gate 4's "flag +0x94 AND a modal dialog"
    // (the flag's retail owner is open [AUDIO.md §8]; [port] the host sets it while the in-game pause menu / its option
    // dialogs are up); attract = the main-menu attract demo runs (attract_mode.hpp). Each changes only on a new value.
    void set_flags(bool modal, bool gui_only, bool attract);
    bool attract() const { return attract_; }
    bool gui_only() const { return gui_only_; }
    // OI host (P3 video item 3): the output device's buffering in seconds (period size x periods / rate; 0 for the
    // null backend / inactive audio), subtracted from a movie's audio clock (MoviePlayer::set_audio_latency).
    double output_latency_seconds() const;
    void set_volumes(float master, float sfx, float speech, float music);
    // Lua presentation audio ops (Play_Music, Stop_All_Speech, SFX permissions, ...) of events with seq >= *next_seq
    // through GameAudio (audio::apply_presentation_audio); *next_seq advances. No-op without engine/lua.
    void apply_presentation(const std::vector<lua::PresentationEvent>& events, uint32_t* next_seq);
    void update(double dt_seconds);

    const GameAudioStats& stats() const { return stats_; }

private:
    const audio::UnitSounds& sounds(const objects::GameObjectTypes& types, objects::TypeIndex t);
    Vec3 position_of(uint64_t id) const;

    std::unique_ptr<audio::AudioDatabase> db_;
    std::unique_ptr<audio::GameAudioData> game_data_;
    std::unique_ptr<audio::SampleSource> samples_;
    std::unique_ptr<audio::VoiceBackend> backend_;
    std::unique_ptr<audio::AudioSystem> system_;
    std::unique_ptr<audio::GameAudio> game_;
    SimRng rng_;  // FREE: presentation only
    std::vector<std::string> speech_done_;  // ended speech events not taken yet
    std::map<objects::TypeIndex, std::unique_ptr<audio::UnitSounds>> sounds_;  // lookup only
    std::unordered_map<uint64_t, Vec3> positions_;                             // lookup only (never iterated)
    std::set<objects::ObjectId> spawned_;                                      // battle units already announced
    std::map<objects::ObjectId, bool> moving_;                                 // ascending id
    std::unordered_set<uint64_t> inaudible_;  // lookup only (never iterated): hidden / stealthed / fogged for the local player
    size_t upgrades_seen_ = 0;                // TacticalBattle::station_upgrades() announced so far
    MusicScene scene_ = MusicScene::None;
    uint32_t last_serial_ = 0;
    bool have_serial_ = false;
    int32_t retreating_player_ = -1;  // TacticalBattle::retreating_player() at the last battle_tick
    bool modal_ = false, gui_only_ = false, attract_ = false;  // OI host: flags last applied
    bool pending_played_ = false;  // battle_pending ran since the last scene change
    bool movie_music_on_ = false, movie_halted_ = false;  // a movie's mode 14 / 17 music is the current one / it halted the music
    GameAudioStats stats_;
};

}  // namespace alamo::game
