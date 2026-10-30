// Tactical ambience (see alamo/audio/ambience.hpp).
#include "alamo/audio/ambience.hpp"

#include <cmath>

#include "alamo/core/hash.hpp"

namespace alamo::audio {
namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

Vec3 cross(const Vec3& a, const Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float len(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
Vec3 scaled(const Vec3& v, float k) { return {v.x * k, v.y * k, v.z * k}; }

}  // namespace

AmbienceManager::AmbienceManager(AudioSystem& audio, const GameAudioData& data, SimRng& free_rng, AmbienceConfig cfg)
    : audio_(&audio), data_(&data), rng_(&free_rng), cfg_(cfg) {}

void AmbienceManager::begin(std::string_view environment) {
    end(false);
    active_ = true;
    environment_ = std::string(environment);
    weather_ = "CLEAR";
    intensity_ = 0.0f;
    beds_.clear();
    if (!cfg_.map_beds) return;
    // Audio.xml <Ambient_Map_Sounds> ENVIRONMENT, AmbientMapSound name.
    for (const auto& [env, name] : audio_->database().constants().ambient_map_sounds) {
        if (!iequals(env, environment_)) continue;
        if (const AmbientMapSound* a = data_->find_ambient_map_sound(name)) {
            for (const AmbientMapSoundEntry& e : a->entries) {
                // [port] entries naming no SFX event are dropped (vanilla's only bed is a stub of unknown events).
                if (audio_->database().find_sfx(e.event) == kNoEvent) continue;
                Timer t;
                select_timer(t, e.event, e.min_seconds, e.max_seconds, e.min_distance, e.max_distance);
                beds_.push_back(std::move(t));
            }
        }
        break;
    }
}

void AmbienceManager::end(bool fade) {
    fade_out_loop(fade);
    weather_timer_ = Timer{};
    map_timer_ = Timer{};
    beds_.clear();
    active_ = false;
}

void AmbienceManager::set_weather(std::string_view weather, float intensity) {
    weather_ = std::string(weather);
    intensity_ = intensity;
}

void AmbienceManager::set_paused(bool paused) { paused_ = paused; }

void AmbienceManager::fade_out_loop(bool fade) {
    if (loop_handle_ != 0 && loop_handle_ != kRefused) {
        float seconds = 0.0f;
        const uint32_t idx = audio_->database().find_sfx(loop_event_);
        if (fade && idx != kNoEvent) seconds = audio_->database().sfx()[idx].loop_fade_out_seconds;
        audio_->stop_sfx(loop_handle_, seconds);
    }
    loop_handle_ = 0;
    loop_event_.clear();
    retry_ = 0.0;
}

void AmbienceManager::select_timer(Timer& t, const std::string& event, float min_s, float max_s, float min_d,
                                   float max_d) {
    if (t.armed && t.event == event) return;
    t.event = event;
    t.min_s = min_s;
    t.max_s = max_s;
    t.min_d = min_d;
    t.max_d = max_d;
    t.armed = !event.empty();
    t.remaining = t.armed ? static_cast<double>(rng_->range_float(min_s, max_s)) : 0.0;
}

Vec3 AmbienceManager::around_listener(float distance, float angle) const {
    // [port] the listener's horizontal plane: right = forward x up, ahead = up x right.
    const Listener& l = audio_->listener();
    Vec3 right = cross(l.forward, l.up);
    const float rl = len(right);
    if (rl <= 0.0f) right = Vec3{1.0f, 0.0f, 0.0f};
    else right = scaled(right, 1.0f / rl);
    Vec3 ahead = cross(l.up, right);
    const float al = len(ahead);
    if (al <= 0.0f) ahead = Vec3{0.0f, 0.0f, 1.0f};
    else ahead = scaled(ahead, 1.0f / al);
    const float c = std::cos(angle) * distance, s = std::sin(angle) * distance;
    return {l.position.x + right.x * c + ahead.x * s, l.position.y + right.y * c + ahead.y * s,
            l.position.z + right.z * c + ahead.z * s};
}

void AmbienceManager::run_timer(Timer& t, double dt) {
    if (!t.armed) return;
    t.remaining -= dt;
    if (t.remaining > 0.0) return;
    float distance = t.min_d;
    if (t.max_d != t.min_d) distance = rng_->range_float(t.min_d, t.max_d);
    const float angle = rng_->range_float(0.0f, kTwoPi);
    SfxPlay p;
    p.kind = SfxPlay::Kind::Position;
    p.position = around_listener(distance, angle);
    audio_->play_sfx(t.event, p);
    t.remaining = static_cast<double>(rng_->range_float(t.min_s, t.max_s));
}

void AmbienceManager::update(double dt_seconds) {
    if (!active_) return;
    const WeatherAudioTable& w = data_->weather();
    // Weather loop.
    std::string want;
    if (!paused_) {
        if (const WeatherLoopEntry* e = w.loop_for(weather_, intensity_)) want = e->event;
    }
    if (want != loop_event_) {
        fade_out_loop(true);
        loop_event_ = want;
        retry_ = 0.0;
    }
    if (!loop_event_.empty() && (loop_handle_ == 0 || !audio_->sfx_live(loop_handle_))) {
        retry_ -= dt_seconds;
        if (loop_handle_ == 0 || retry_ <= 0.0) {
            const uint32_t idx = audio_->database().find_sfx(loop_event_);
            SfxPlay p;
            if (idx != kNoEvent) p.fade_in_seconds = audio_->database().sfx()[idx].loop_fade_in_seconds;
            loop_handle_ = audio_->play_sfx(loop_event_, p);
            if (loop_handle_ == 0) loop_handle_ = kRefused;
            retry_ = static_cast<double>(cfg_.retry_seconds);
        }
    }
    // Intermittents (WeatherAudio.xml).
    if (!paused_) {
        const WeatherIntermittentEntry* wi = w.intermittent_for(weather_, intensity_);
        if (wi) select_timer(weather_timer_, wi->event, wi->min_seconds, wi->max_seconds,
                             cfg_.intermittent_distance, cfg_.intermittent_distance);
        else select_timer(weather_timer_, std::string(), 0.0f, 0.0f, 0.0f, 0.0f);
        const AmbientIntermittentEntry* ai = weather_timer_.event.empty() ? w.ambient_for(environment_) : nullptr;
        if (ai) select_timer(map_timer_, ai->event, ai->min_seconds, ai->max_seconds, cfg_.intermittent_distance,
                             cfg_.intermittent_distance);
        else select_timer(map_timer_, std::string(), 0.0f, 0.0f, 0.0f, 0.0f);
        run_timer(weather_timer_, dt_seconds);
        run_timer(map_timer_, dt_seconds);
    } else {
        weather_timer_ = Timer{};
        map_timer_ = Timer{};
    }
    for (Timer& t : beds_) run_timer(t, dt_seconds);
}

}  // namespace alamo::audio
