// Tactical ambience: the weather audio manager (weather loops + weather intermittents + "clear weather"
// map intermittents, WeatherAudio.xml) and the map ambient beds (Audio.xml Ambient_Map_Sounds ->
// AmbientMapSounds.xml). Everything plays through AudioSystem as ordinary SFX events (AUDIO.md §4: "There
// is no ambient bus: ambient beds, weather and Is_Ambient_VO are ordinary SFX events").
//
// Rules (AUDIO.md §6.2, §7; [inferred]/[port] where the spec is silent):
//   * Weather loop: the row of WeatherAudioTable::loop_for(weather, intensity). When the selected event
//     changes, the playing loop is stopped with its event's Loop_Fade_Out_Seconds and the new one started
//     2D with its Loop_Fade_In_Seconds (Fade_Out_Active_Loop / Fade_In_New_Loop; the presets comment
//     "Crossfading from light, to medium, to heavy"). A selected loop that is not live (refused by a gate,
//     killed by stop_all_sfx on a mode change) is retried every AmbienceConfig::retry_seconds [port].
//   * Intermittents (weather rows, and the map-environment row of Ambient_SFXEvent_Intermittent while the
//     selected weather intermittent is None = "when weather is clear" [inferred]): when the selected event
//     changes the countdown is armed with float(min, max) seconds; on expiry the event is started at
//     intermittent_distance (50) units from the listener at a free-RNG angle float(0, 2π) in the
//     listener's horizontal plane, then re-armed with float(min, max) (AUDIO.md §6.2).
//     §7 [port]: the countdown runs on wall time (the original counts 1 per update, armed in 1/30 s
//     units: the same duration at 30 updates/s).
//   * Map beds [inferred from the AmbientMapSounds.xml comment "SFXEvent name, closest surround dist,
//     farthest surround dist, min second delay, max second delay"]: each entry has its own countdown,
//     armed with float(min_s, max_s); on expiry the event plays at float(closest, farthest) units from the
//     listener (distance drawn before the angle), then re-arms. Vanilla only defines a stub for
//     Ambient_Map_Sounds_Swamp whose events do not exist; entries naming no SFX event are dropped [port].
//     AmbienceConfig::map_beds turns the beds off.
//   * Weather_Audio_Pause (Lua) pauses the WeatherAudio.xml part: the loop fades out, countdowns stop;
//     unpausing re-selects the loop and re-arms the countdowns. Map beds are not paused [inferred].
//   * Free RNG only; never reads or writes sim state (the host passes weather type/intensity read from
//     the sim). Draw order per update: weather intermittent, map intermittent, beds in data order.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/audio/audio_system.hpp"
#include "alamo/audio/game_audio_data.hpp"

namespace alamo::audio {

struct AmbienceConfig {
    float intermittent_distance = 50.0f;  // §6.2
    float retry_seconds = 1.0f;           // [port] retry of a selected loop that is not live
    bool map_beds = true;
};

class AmbienceManager {
public:
    // All references must outlive the manager. free_rng = GameRandom::free().
    AmbienceManager(AudioSystem& audio, const GameAudioData& data, SimRng& free_rng, AmbienceConfig cfg = {});

    // Tactical start: the map environment (Temperate, Arctic, Desert, Forest, Swamp, Volcanic, Urban; "Space"
    // for space maps, which have no weather or map ambience in the shipped data). Weather starts CLEAR, 0.
    void begin(std::string_view environment);
    // Tactical end: loops fade out (fade = false: stopped at once), countdowns stop.
    void end(bool fade = true);
    bool active() const { return active_; }

    // Current weather from the sim (type CLEAR/RAIN/SNOW/SANDSTORM/ASH, intensity 0..1). Applied on update.
    void set_weather(std::string_view weather, float intensity);
    // Lua Weather_Audio_Pause(bool).
    void set_paused(bool paused);
    bool paused() const { return paused_; }

    // Once per audio step, after AudioSystem::update (AUDIO.md §6.1 order). Wall seconds.
    void update(double dt_seconds);

    // Introspection (tests, debug overlay).
    const std::string& loop_event() const { return loop_event_; }
    uint32_t loop_handle() const { return loop_handle_ == kRefused ? 0u : loop_handle_; }  // 0 = none
    const std::string& weather_intermittent_event() const { return weather_timer_.event; }
    const std::string& map_intermittent_event() const { return map_timer_.event; }
    double weather_intermittent_remaining() const { return weather_timer_.remaining; }
    size_t bed_count() const { return beds_.size(); }

private:
    static constexpr uint32_t kRefused = 0xFFFFFFFFu;  // loop start refused: retry later (never a live id)
    struct Timer {
        std::string event;
        float min_s = 0.0f, max_s = 0.0f;
        float min_d = 0.0f, max_d = 0.0f;  // beds: distance range; intermittents: both = distance
        double remaining = 0.0;
        bool armed = false;
    };
    void select_timer(Timer& t, const std::string& event, float min_s, float max_s, float min_d, float max_d);
    void run_timer(Timer& t, double dt);
    void fade_out_loop(bool fade);
    Vec3 around_listener(float distance, float angle) const;

    AudioSystem* audio_;
    const GameAudioData* data_;
    SimRng* rng_;
    AmbienceConfig cfg_;
    bool active_ = false, paused_ = false;
    std::string environment_;
    std::string weather_ = "CLEAR";
    float intensity_ = 0.0f;
    std::string loop_event_;
    uint32_t loop_handle_ = 0;
    double retry_ = 0.0;
    Timer weather_timer_, map_timer_;
    std::vector<Timer> beds_;
};

}  // namespace alamo::audio
