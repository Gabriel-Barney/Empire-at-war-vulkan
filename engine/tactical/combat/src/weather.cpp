#include "alamo/tactical/combat/weather.hpp"

#include <algorithm>

#include "alamo/core/sim_crc.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/objects/type_fields.hpp"

namespace alamo::tactical::combat {

namespace {

std::string upper(std::string_view s) {
    std::string out;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        out.push_back(c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
    }
    return out;
}

bool iequal(std::string_view a, std::string_view b) { return upper(a) == upper(b); }

float number(std::string_view v, float fallback) { return v.empty() ? fallback : static_cast<float>(objects::c_atof(v)); }

}  // namespace

const WeatherScenario* WeatherData::find(std::string_view name) const {
    if (name.empty()) return nullptr;
    for (const WeatherScenario& s : scenarios)
        if (iequal(s.name, name)) return &s;
    return nullptr;
}

WeatherType weather_type_from_name(std::string_view name, bool* known) {
    const std::string u = upper(name);
    WeatherType t = WeatherType::Clear;
    bool ok = true;
    if (u == "CLEAR") t = WeatherType::Clear;
    else if (u == "RAIN") t = WeatherType::Rain;
    else if (u == "SNOW") t = WeatherType::Snow;
    else if (u == "SANDSTORM") t = WeatherType::Sandstorm;
    else if (u == "ASH") t = WeatherType::Ash;
    else ok = false;
    if (known) *known = ok;
    return t;
}

int32_t projectile_category_index(std::string_view name) {
    static const char* const kNames[kProjectileCategoryCount] = {"DEFAULT", "MISSILE", "BOMB", "GRENADE", "ROCKET",
                                                                 "STATIC_BOMB", "MPTL_ROCKET", "LASER", "LIGHT_SABER", "CLUSTER_BOMB"};
    const std::string u = upper(name);
    for (uint32_t i = 0; i < kProjectileCategoryCount; ++i)
        if (u == kNames[i]) return static_cast<int32_t>(i);
    return -1;
}

bool load_weather_data(const FileProvider& files, WeatherData& out, std::vector<std::string>* diag) {
    out = WeatherData{};
    bool ok = true;
    std::vector<uint8_t> bytes;
    if (files.read("Data\\XML\\WeatherScenarios.xml", bytes)) {
        const data::XmlDocument doc = data::XmlDocument::parse(bytes, "WeatherScenarios.xml");
        for (data::XmlNode s : doc.root().children("Weather_Scenario")) {
            WeatherScenario sc;
            sc.name = std::string(s.attribute("name"));
            sc.emitter = upper(s.attribute("emitter_name"));
            sc.randomize_phase_selection = iequal(s.attribute("randomize_phase_selection"), "true") ||
                                           iequal(s.attribute("randomize_phase_selection"), "yes");
            for (data::XmlNode p : s.children("Phase")) {
                WeatherPhase ph;
                bool known = true;
                ph.type = weather_type_from_name(p.attribute("name"), &known);
                if (!known && diag)
                    diag->push_back("weather: Invalid Weather Phase Name " + std::string(p.attribute("name")) + " in " + sc.name +
                                    ", defaulting to CLEAR");
                ph.emitter_intensity = number(p.child_text("Emitter_Intensity"), 0.0f);
                ph.lightning_intensity = number(p.child_text("Lightning_Intensity"), 0.0f);
                ph.fog_alpha = number(p.child_text("Fog_Plane_Interpolation_Alpha"), 0.0f);
                ph.duration = objects::c_atoi(p.child_text("Duration"));
                ph.ease_out = objects::c_atoi(p.child_text("Ease_Out_Duration"));
                sc.phases.push_back(ph);
            }
            out.scenarios.push_back(std::move(sc));
        }
    } else {
        ok = false;
        if (diag) diag->push_back("weather: WeatherScenarios.xml not found (no tactical weather)");
    }
    bytes.clear();
    if (files.read("Data\\XML\\WeatherModifiers.xml", bytes)) {
        const data::XmlDocument doc = data::XmlDocument::parse(bytes, "WeatherModifiers.xml");
        for (data::XmlNode m : doc.root().children("WeatherModifier")) {
            bool known = true;
            const WeatherType t = weather_type_from_name(m.attribute("name"), &known);
            if (!known) continue;
            WeatherTypeModifiers& wm = out.modifiers[static_cast<size_t>(t)];
            for (data::XmlNode e : m.children()) {
                const std::string_view tag = e.name();
                const std::string key = upper(e.attribute("name"));
                const float v = number(e.text_trimmed(), 1.0f);
                if (iequal(tag, "Weapon_Accuracy_Modifier")) {
                    const int32_t c = projectile_category_index(key);
                    if (c >= 0) wm.accuracy[static_cast<size_t>(c)] = v;
                } else if (iequal(tag, "Speed_Modifier")) {
                    wm.speed.emplace_back(key, v);
                } else if (iequal(tag, "Sight_Range_Modifier")) {
                    wm.sight.emplace_back(key, v);
                }
            }
        }
    } else {
        ok = false;
        if (diag) diag->push_back("weather: WeatherModifiers.xml not found (modifiers 1.0)");
    }
    return ok;
}

// ---------------------------------------------------------------- WeatherSystem

void WeatherSystem::start(const WeatherScenario* scenario, const WeatherData* data, uint32_t frame, SimRng& rng) {
    events_.clear();
    data_ = data;
    if (!scenario || scenario->phases.empty()) {  // weather off (audio -> CLEAR, emitter removed)
        scenario_ = nullptr;
        type_ = WeatherType::Clear;
        intensity_ = lightning_ = fog_alpha_ = 0.0f;
        transition_ = false;
        events_.push_back({WeatherEvent::Kind::Off, 0, frame});
        return;
    }
    scenario_ = scenario;
    const uint32_t n = static_cast<uint32_t>(scenario->phases.size());
    index_ = 0;
    current_ = 0;
    next_ = n > 1 ? 1u : 0u;
    type_ = scenario->phases[0].type;
    intensity_ = scenario->phases[0].emitter_intensity;
    transition_ = false;
    events_.push_back({WeatherEvent::Kind::Started, 0, frame});
    start_phase(frame, rng);
}

void WeatherSystem::start_phase(uint32_t frame, SimRng& rng) {
    // 0x468980: timer length = synced float(0.3 x Duration, Duration) x FPS frames.
    const WeatherPhase& p = scenario_->phases[current_];
    const float d = static_cast<float>(p.duration);
    timer_start_ = frame;
    timer_length_ = rng.range_float(0.3f * d, d) * static_cast<float>(kFps);
    lightning_ = p.lightning_intensity;
    fog_alpha_ = p.fog_alpha;
    intensity_ = p.emitter_intensity;
    events_.push_back({WeatherEvent::Kind::PhaseStarted, index_, frame});
}

void WeatherSystem::set_intensity(float v, uint32_t frame) {
    const bool was = intensity_ > 0.5f, now = v > 0.5f;
    intensity_ = v;
    if (was != now) events_.push_back({WeatherEvent::Kind::FowDirty, index_, frame});
}

void WeatherSystem::advance(uint32_t frame, SimRng& rng) {
    const uint32_t n = static_cast<uint32_t>(scenario_->phases.size());
    index_ = (index_ + 1) % n;
    current_ = index_;
    next_ = (index_ + 1) % n;
    const WeatherType old = type_;
    type_ = scenario_->phases[current_].type;
    intensity_ = scenario_->phases[current_].emitter_intensity;
    transition_ = false;
    start_phase(frame, rng);
    if (type_ != old) events_.push_back({WeatherEvent::Kind::FowDirty, index_, frame});
}

void WeatherSystem::service(uint32_t frame, SimRng& rng) {
    if (!scenario_) return;
    const WeatherPhase& cur = scenario_->phases[current_];
    const WeatherPhase& nxt = scenario_->phases[next_];
    const float elapsed = static_cast<float>(frame - timer_start_);
    if (transition_) {
        const float t = timer_length_ > 0.0f ? std::min(1.0f, elapsed / timer_length_) : 1.0f;
        lightning_ = cur.lightning_intensity + (nxt.lightning_intensity - cur.lightning_intensity) * t;
        fog_alpha_ = cur.fog_alpha + (nxt.fog_alpha - cur.fog_alpha) * t;
        set_intensity(cur.emitter_intensity + (nxt.emitter_intensity - cur.emitter_intensity) * t, frame);
    }
    if (elapsed >= timer_length_) {
        if (!transition_) {
            transition_ = true;
            const float e = static_cast<float>(cur.ease_out);
            timer_start_ = frame;
            timer_length_ = rng.range_float(0.3f * e, e) * static_cast<float>(kFps);
            events_.push_back({WeatherEvent::Kind::TransitionStarted, index_, frame});
        } else {
            advance(frame, rng);
        }
    }
}

bool WeatherSystem::force_weather(uint32_t frame, SimRng& rng) {
    if (!scenario_) return false;
    advance(frame, rng);
    return true;
}

const WeatherTypeModifiers* WeatherSystem::mods() const {
    if (!scenario_ || !data_ || !(intensity_ > 0.5f)) return nullptr;  // the CLEAR block while I <= 0.5 [H]
    return &data_->modifiers[static_cast<size_t>(type_)];
}

float WeatherSystem::lookup(const std::vector<std::pair<std::string, float>>& m, std::string_view category) const {
    const std::string key = category.empty() ? std::string("DEFAULT") : upper(category);
    float v = -1.0f;
    for (const auto& [k, x] : m)
        if (k == key) v = x;
    if (v < 0.0f)
        for (const auto& [k, x] : m)
            if (k == "DEFAULT") v = x;
    return (v < 0.0f || v <= 0.0001f) ? 1.0f : v;
}

float WeatherSystem::sight_modifier(std::string_view weather_category) const {
    const WeatherTypeModifiers* m = mods();
    return m ? lookup(m->sight, weather_category) : 1.0f;
}

float WeatherSystem::speed_modifier(std::string_view weather_category) const {
    const WeatherTypeModifiers* m = mods();
    return m ? lookup(m->speed, weather_category) : 1.0f;
}

float WeatherSystem::accuracy_modifier(uint32_t projectile_category) const {
    const WeatherTypeModifiers* m = mods();
    if (!m) return 1.0f;
    return projectile_category < kProjectileCategoryCount ? m->accuracy[projectile_category] : 0.0f;  // out of range -> 0 [H]
}

std::array<float, kProjectileCategoryCount> WeatherSystem::accuracy_row() const {
    std::array<float, kProjectileCategoryCount> r{};
    for (uint32_t i = 0; i < kProjectileCategoryCount; ++i) r[i] = accuracy_modifier(i);
    return r;
}

void WeatherSystem::hash(StateHasher& h) const {
    h.boolean(scenario_ != nullptr);
    if (!scenario_) return;
    h.str(scenario_->name);
    h.u32(index_);
    h.u8(static_cast<uint8_t>(type_));
    h.f32(intensity_);
    h.boolean(transition_);
    h.u32(timer_start_);
    h.f32(timer_length_);
}

}  // namespace alamo::tactical::combat
