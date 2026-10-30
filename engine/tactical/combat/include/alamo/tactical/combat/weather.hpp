// L1 systems - tactical weather (land only; docs/re/L1_S4_REMAINING.md §3.2). Data: WeatherScenarios.xml (scenario ->
// phases) and WeatherModifiers.xml (per weather type: weapon accuracy by projectile category, speed and sight by
// Weather_Category). WeatherSystem is the global weather state of one land battle (0x140b30b40):
//   * init (0x468a50) with the map environment's scenario: phase 0 current, phase 1 next (phase 0 when alone), type /
//     intensity of phase 0, then START PHASE (0x468980): timer length = synced float(0.3 x Duration, Duration) x FPS
//     frames (one draw), lightning / fog alpha / emitter intensity of the phase. No scenario or 0 phases: weather off.
//   * service (0x46a0c0) per land frame: while transitioning t = min(1, (now - start) / length), lightning, emitter
//     intensity I and fog alpha = lerp(current, next) (I -> the intensity +0x448); I crossing 0.5 either way raises
//     FowDirty (every REVEAL object recomputes its sight). Timer over: not transitioning -> transitioning with timer
//     length = synced float(0.3 x Ease_Out_Duration, Ease_Out_Duration) x FPS (one draw); transitioning -> ADVANCE.
//   * ADVANCE (0x4690c0): index + 1 (wrapping), current / next phases, type, I := current Emitter_Intensity,
//     transition off, start phase (one draw); a type change raises FowDirty. Force_Weather() = one advance.
//   * effects (every modifier 1.0 unless I > 0.5 and the owner is weather-affected [M: every player here]): sight
//     (by Weather_Category, fallback "Default", missing / <= 0.0001 -> 1.0), weapon accuracy m by projectile category
//     (inaccuracy x (2 - m), CombatSystem::set_weather_accuracy), speed (no vanilla effect; exposed).
// randomize_phase_selection is parsed and never read (phases run in file order) [H]. Weather audio / emitter /
// lightning / fog-plane alpha are presentation (exposed, not sim inputs).
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/sim_rng.hpp"

namespace alamo {
class StateHasher;
class ByteWriter;
class ByteReader;
}  // namespace alamo

namespace alamo::tactical::combat {

enum class WeatherType : uint8_t { Clear = 0, Rain = 1, Snow = 2, Sandstorm = 3, Ash = 4 };
inline constexpr uint32_t kWeatherTypeCount = 5;
inline constexpr uint32_t kProjectileCategoryCount = 10;  // DEFAULT .. CLUSTER_BOMB

struct WeatherPhase {
    float emitter_intensity = 0.0f;   // Emitter_Intensity
    float lightning_intensity = 0.0f; // Lightning_Intensity
    float fog_alpha = 0.0f;           // Fog_Plane_Interpolation_Alpha
    WeatherType type = WeatherType::Clear;
    int32_t duration = 0;             // Duration (s)
    int32_t ease_out = 0;             // Ease_Out_Duration (s)
};

struct WeatherScenario {
    std::string name;                 // case-insensitive lookup
    std::string emitter;              // emitter_name (upper-cased)
    bool randomize_phase_selection = false;  // parsed, never read [H]
    std::vector<WeatherPhase> phases;
};

struct WeatherTypeModifiers {
    std::array<float, kProjectileCategoryCount> accuracy{1, 1, 1, 1, 1, 1, 1, 1, 1, 1};  // unset = 1
    std::vector<std::pair<std::string, float>> speed, sight;  // (upper-cased Weather_Category name, value)
};

struct WeatherData {
    std::vector<WeatherScenario> scenarios;
    std::array<WeatherTypeModifiers, kWeatherTypeCount> modifiers;
    const WeatherScenario* find(std::string_view name) const;
};

// WeatherScenarios.xml + WeatherModifiers.xml from Data\XML (missing files: an empty table, diagnostics).
// Unknown phase names -> CLEAR ("Invalid Weather Phase Name ... defaulting to CLEAR").
bool load_weather_data(const FileProvider& files, WeatherData& out, std::vector<std::string>* diagnostics = nullptr);
WeatherType weather_type_from_name(std::string_view name, bool* known = nullptr);
// Projectile category name (DEFAULT, MISSILE, BOMB, GRENADE, ROCKET, STATIC_BOMB, MPTL_ROCKET, LASER, LIGHT_SABER,
// CLUSTER_BOMB) -> index, -1 unknown.
int32_t projectile_category_index(std::string_view name);

struct WeatherEvent {
    enum class Kind : uint8_t { Started, PhaseStarted, TransitionStarted, FowDirty, Off };
    Kind kind = Kind::Started;
    uint32_t phase = 0;
    uint32_t frame = 0;
};

class WeatherSystem {
public:
    static constexpr uint32_t kFps = 30;
    // Starts (or turns off with null / no phases) the weather at `frame` (one synced draw when it starts).
    void start(const WeatherScenario* scenario, const WeatherData* data, uint32_t frame, SimRng& rng);
    void service(uint32_t frame, SimRng& rng);
    bool force_weather(uint32_t frame, SimRng& rng);  // one advance; false when off

    bool active() const { return scenario_ != nullptr; }
    WeatherType type() const { return type_; }
    float intensity() const { return intensity_; }       // +0x448
    float lightning() const { return lightning_; }       // presentation
    float fog_alpha() const { return fog_alpha_; }       // presentation
    uint32_t phase_index() const { return index_; }
    bool transitioning() const { return transition_; }
    float sight_modifier(std::string_view weather_category) const;
    float speed_modifier(std::string_view weather_category) const;
    float accuracy_modifier(uint32_t projectile_category) const;
    // The accuracy row of the current state (all 1 while I <= 0.5 / off), for CombatSystem::set_weather_accuracy.
    std::array<float, kProjectileCategoryCount> accuracy_row() const;
    std::span<const WeatherEvent> events() const { return events_; }
    void clear_events() { events_.clear(); }
    void hash(StateHasher& h) const;
    // N1 stage 2 (save_file.hpp contract, combat_save.cpp). The scenario is written as its index in the WeatherData the
    // system was started with + its name; the loader rebuilds the battle (start() with the same data) first, and read()
    // resolves the scenario in that data (or keeps the current scenario when only the name matches). data_ is not
    // saved (the host's). False (unchanged) when the scenario cannot be resolved or a phase index is out of range.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    void start_phase(uint32_t frame, SimRng& rng);
    void advance(uint32_t frame, SimRng& rng);
    void set_intensity(float v, uint32_t frame);
    const WeatherTypeModifiers* mods() const;
    float lookup(const std::vector<std::pair<std::string, float>>& m, std::string_view category) const;

    const WeatherScenario* scenario_ = nullptr;
    const WeatherData* data_ = nullptr;
    uint32_t index_ = 0, current_ = 0, next_ = 0;
    WeatherType type_ = WeatherType::Clear;
    float intensity_ = 0.0f, lightning_ = 0.0f, fog_alpha_ = 0.0f;
    bool transition_ = false;
    uint32_t timer_start_ = 0;
    float timer_length_ = 0.0f;  // frames
    std::vector<WeatherEvent> events_;
};

}  // namespace alamo::tactical::combat
