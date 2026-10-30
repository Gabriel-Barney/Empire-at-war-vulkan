// Audio event player, following docs/re/AUDIO.md (section numbers in comments). See audio_system.hpp.
#include "alamo/audio/audio_system.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "alamo/core/hash.hpp"

namespace alamo::audio {

// ---------------------------------------------------------------- NullBackend

bool NullBackend::start(uint32_t voice, const SampleRef& sample, const VoiceParams& p) {
    std::optional<double> len = duration_ ? duration_(sample) : std::optional<double>{};
    if (!len) return false;
    Voice& v = voices_[voice];
    v.sample = sample;
    v.params = p;
    v.length = *len;
    v.position = 0.0;
    v.active = true;
    return true;
}

void NullBackend::set(uint32_t voice, const VoiceParams& p) {
    auto it = voices_.find(voice);
    if (it != voices_.end()) it->second.params = p;
    auto st = streams_.find(voice);
    if (st != streams_.end()) st->second.params = p;
}

void NullBackend::stop(uint32_t voice) {
    voices_.erase(voice);
    streams_.erase(voice);
}

bool NullBackend::playing(uint32_t voice) const {
    if (streams_.count(voice) != 0) return true;
    auto it = voices_.find(voice);
    return it != voices_.end() && it->second.active;
}

bool NullBackend::start_stream(uint32_t voice, const PcmStreamFormat& format, PcmPullFn pull, const VoiceParams& p) {
    voices_.erase(voice);
    StreamVoice s;
    s.resampler = std::make_unique<StreamResampler>(format, device_.sample_rate, device_.channels, std::move(pull));
    s.params = p;
    streams_[voice] = std::move(s);
    return true;
}

const NullBackend::StreamVoice* NullBackend::stream(uint32_t id) const {
    auto it = streams_.find(id);
    return it == streams_.end() ? nullptr : &it->second;
}

void NullBackend::advance(double seconds) {
    // Streams: the device consumes round(seconds x rate) frames (fraction carried to the next call).
    if (seconds > 0.0) {
        frame_carry_ += seconds * static_cast<double>(device_.sample_rate);
        const double whole = std::floor(frame_carry_ + 1e-9);
        frame_carry_ = std::max(0.0, frame_carry_ - whole);
        const size_t frames = static_cast<size_t>(whole);
        if (frames > 0) {
            std::vector<float> block(frames * device_.channels);
            for (auto& kv : streams_) {
                StreamVoice& s = kv.second;
                s.resampler->read(block.data(), frames);
                s.device_frames += frames;
                for (float x : block) {
                    if (s.captured.size() >= device_.capture_limit) break;
                    s.captured.push_back(x * s.params.gain);
                }
            }
        }
    }
    for (auto& kv : voices_) {
        Voice& v = kv.second;
        if (!v.active) continue;
        v.position += seconds * static_cast<double>(v.params.pitch);
        if (v.position >= v.length) {
            if (v.params.loop && v.length > 0.0) v.position = std::fmod(v.position, v.length);
            else v.active = false;
        }
    }
}

const NullBackend::Voice* NullBackend::voice(uint32_t id) const {
    auto it = voices_.find(id);
    return it == voices_.end() ? nullptr : &it->second;
}

uint32_t NullBackend::active_count() const {
    uint32_t n = 0;
    for (const auto& kv : voices_) n += kv.second.active ? 1u : 0u;
    return n;
}

// ---------------------------------------------------------------- helpers

namespace {

constexpr double kUsPerSecond = 1e6;

float length(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
Vec3 sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

bool ends_with_ci(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && iequals(s.substr(s.size() - suffix.size()), suffix);
}

// Moves `level` toward `target` by rate x seconds (rate 0 = jump).
void ramp_toward(float& level, float target, float rate, double seconds) {
    if (rate <= 0.0f) {
        level = target;
        return;
    }
    const float step = static_cast<float>(static_cast<double>(rate) * seconds);
    level = level < target ? std::min(target, level + step) : std::max(target, level - step);
}

}  // namespace

std::string_view language_suffix(std::string_view language) {
    static const std::pair<std::string_view, std::string_view> table[] = {
        {"German", "_GER"},  {"French", "_FRE"},  {"Spanish", "_SPA"}, {"Italian", "_ITA"}, {"Japanese", "_JAP"},
        {"Korean", "_KOR"},  {"Chinese", "_CHI"}, {"Russian", "_RUS"}, {"Polish", "_POL"},  {"Thai", "_THA"},
    };
    for (const auto& [name, suffix] : table)
        if (iequals(name, language)) return suffix;
    return {};
}

std::string localize_file(std::string_view file, std::string_view language) {
    const std::string_view suffix = language_suffix(language);
    if (suffix.empty()) return std::string(file);
    for (std::string_view ext : {std::string_view("_ENG.WAV"), std::string_view("_ENG.MP3")}) {
        if (ends_with_ci(file, ext)) {
            return std::string(file.substr(0, file.size() - ext.size())) + std::string(suffix) +
                   std::string(file.substr(file.size() - 4));
        }
    }
    return std::string(file);
}

Listener listener_from_camera(const Vec3& camera_position, const Vec3& look_at, const Vec3& forward,
                              const Vec3& up, float pullback, bool cinematic) {
    Listener l;
    l.forward = forward;
    l.up = up;
    if (cinematic) {
        l.position = camera_position;
        return l;
    }
    const float fl = length(forward);
    const float k = fl > 0.0f ? pullback / fl : 0.0f;
    l.position = {look_at.x - forward.x * k, look_at.y - forward.y * k, look_at.z - forward.z * k};
    return l;
}

// ---------------------------------------------------------------- log

const char* log_op_name(LogOp op) {
    switch (op) {
        case LogOp::SfxStart: return "sfx_start";
        case LogOp::SfxReject: return "sfx_reject";
        case LogOp::SfxCycle: return "sfx_cycle";
        case LogOp::SfxVoice: return "sfx_voice";
        case LogOp::SfxNoVoice: return "sfx_no_voice";
        case LogOp::SfxSteal: return "sfx_steal";
        case LogOp::SfxStop: return "sfx_stop";
        case LogOp::SfxEnd: return "sfx_end";
        case LogOp::SfxChain: return "sfx_chain";
        case LogOp::MusicPlay: return "music_play";
        case LogOp::MusicRefuse: return "music_refuse";
        case LogOp::MusicTrack: return "music_track";
        case LogOp::MusicStop: return "music_stop";
        case LogOp::SpeechQueue: return "speech_queue";
        case LogOp::SpeechStart: return "speech_start";
        case LogOp::SpeechEnd: return "speech_end";
        case LogOp::SpeechKill: return "speech_kill";
        case LogOp::StreamOpen: return "stream_open";
        case LogOp::StreamStop: return "stream_stop";
    }
    return "?";
}

std::string LogEntry::to_string() const {
    char buf[200];
    std::snprintf(buf, sizeof buf, "%10lld %-13s h%u v%u o%llu", static_cast<long long>(time_us), log_op_name(op),
                  handle, voice, static_cast<unsigned long long>(owner));
    std::string s = buf;
    s += " " + event;
    if (!sample.empty()) s += " sample=" + sample;
    if (op == LogOp::SfxCycle) {
        std::snprintf(buf, sizeof buf, " vol=%d pan=%d pitch=%d pre=%d post=%d", volume, pan, pitch, predelay_ms,
                      postdelay_ms);
        s += buf;
    }
    if (op == LogOp::SfxVoice || op == LogOp::MusicTrack || op == LogOp::SpeechStart || op == LogOp::StreamOpen) {
        std::snprintf(buf, sizeof buf, " gain=%.4f", static_cast<double>(gain));
        s += buf;
    }
    if (!reason.empty()) s += " (" + reason + ")";
    return s;
}

// ---------------------------------------------------------------- AudioSystem: common

AudioSystem::AudioSystem(const AudioDatabase& db, VoiceBackend& backend, SimRng& free_rng, AudioConfig cfg)
    : db_(&db), backend_(&backend), rng_(&free_rng), cfg_(std::move(cfg)) {
    for (BusState& b : buses_) b.level = b.target = cfg_.default_bus_level;
    saved_master_ = cfg_.default_bus_level;
}

AudioSystem::~AudioSystem() {
    for (Instance& in : instances_)
        if (in.voice) backend_->stop(in.voice);
    if (music_current_.voice) backend_->stop(music_current_.voice);
    if (music_previous_.voice) backend_->stop(music_previous_.voice);
    if (speech_voice_) backend_->stop(speech_voice_);
    for (const PcmStreamState& s : streams_) backend_->stop(s.voice);
}

// ---------------------------------------------------------------- streams

bool AudioSystem::ducking_active() const {
    if (speech_playing()) return true;
    for (const PcmStreamState& s : streams_)
        if (s.params.ducks_sfx) return true;
    return false;
}

float AudioSystem::stream_voice_gain(const PcmStreamState& s) const {
    float g = bus_gain(s.params.bus) * s.level;
    if (s.params.bus == AudioBus::Sfx && speech_playing() && !modal_dialog_) g = std::min(g, cfg_.speech_duck_cap);
    return g;
}

uint32_t AudioSystem::open_stream(const StreamParams& params, PcmPullFn pull) {
    LogEntry le;
    le.op = LogOp::StreamOpen;
    if (params.format.sample_rate == 0 || params.format.channels == 0 || !pull) {
        le.reason = "bad_format";
        push(std::move(le));
        return 0;
    }
    PcmStreamState s;
    s.params = params;
    s.level = s.target = std::clamp(params.volume, 0.0f, 1.0f);
    s.voice = next_voice_++;
    VoiceParams vp;
    vp.gain = stream_voice_gain(s);
    if (!backend_->start_stream(s.voice, params.format, std::move(pull), vp)) {
        le.reason = "unsupported";
        push(std::move(le));
        return 0;
    }
    s.handle = next_stream_++;
    if (next_stream_ == 0) next_stream_ = 1;
    le.handle = s.handle;
    le.voice = s.voice;
    le.gain = vp.gain;
    le.volume = static_cast<int32_t>(params.format.sample_rate);
    le.pan = static_cast<int32_t>(params.format.channels);
    push(std::move(le));
    streams_.push_back(s);
    return s.handle;
}

void AudioSystem::stop_stream(uint32_t handle) {
    for (auto it = streams_.begin(); it != streams_.end(); ++it) {
        if (it->handle != handle) continue;
        backend_->stop(it->voice);
        LogEntry le;
        le.op = LogOp::StreamStop;
        le.handle = handle;
        le.voice = it->voice;
        push(std::move(le));
        streams_.erase(it);
        return;
    }
}

void AudioSystem::set_stream_volume(uint32_t handle, float volume, float ramp_seconds) {
    for (PcmStreamState& s : streams_) {
        if (s.handle != handle) continue;
        s.target = std::clamp(volume, 0.0f, 1.0f);
        if (ramp_seconds <= 0.0f) {
            s.level = s.target;
            s.rate = 0.0f;
        } else {
            s.rate = std::fabs(s.target - s.level) / ramp_seconds;
        }
        backend_->set(s.voice, VoiceParams{stream_voice_gain(s), 0.0f, 1.0f, false});
        return;
    }
}

bool AudioSystem::stream_open(uint32_t handle) const {
    for (const PcmStreamState& s : streams_)
        if (s.handle == handle) return true;
    return false;
}

float AudioSystem::stream_gain(uint32_t handle) const {
    for (const PcmStreamState& s : streams_)
        if (s.handle == handle) return stream_voice_gain(s);
    return 0.0f;
}

void AudioSystem::push(LogEntry e) {
    e.time_us = now_us_;
    log_.push_back(std::move(e));
}

std::string AudioSystem::log_text() const {
    std::string s;
    for (const LogEntry& e : log_) s += e.to_string() + "\n";
    return s;
}

void AudioSystem::set_bus_level(AudioBus bus, float level, float ramp_seconds) {
    BusState& b = buses_[static_cast<int>(bus)];
    b.target = std::clamp(level, 0.0f, 1.0f);
    if (ramp_seconds <= 0.0f) {
        b.level = b.target;
        b.rate = 0.0f;
    } else {
        b.rate = std::fabs(b.target - b.level) / ramp_seconds;
    }
}

float AudioSystem::bus_gain(AudioBus bus) const {
    const float master = buses_[0].level;
    return bus == AudioBus::Master ? master : buses_[static_cast<int>(bus)].level * master;
}

float AudioSystem::distance_to_listener(const Vec3& p) const { return length(sub(p, listener_.position)); }

float AudioSystem::attenuation(float distance, float event_saturation) const {
    const AudioConstants& c = db_->constants();
    const int m = static_cast<int>(mode_);
    const float min_d = std::max(1.0f, event_saturation * c.saturation_distance_mod[m]);
    if (distance > cfg_.max_distance) return 0.0f;  // nothing beyond max distance
    if (distance <= min_d) return 1.0f;
    return min_d / (min_d + c.rolloff_distance_mod[m] * (distance - min_d));  // [M] DS3D inverse distance
}

float AudioSystem::pan_for(const Vec3& pos) const {
    // [port] Miles pans 3D voices itself; here pan = cosine between the source direction and the
    // listener's right vector, right = forward x up.
    const Vec3 d = sub(pos, listener_.position);
    const float dl = length(d);
    const Vec3 right = cross(listener_.forward, listener_.up);
    const float rl = length(right);
    if (dl <= 0.0f || rl <= 0.0f) return 0.0f;
    const float dot = (d.x * right.x + d.y * right.y + d.z * right.z) / (dl * rl);
    return std::clamp(dot, -1.0f, 1.0f);
}

uint32_t AudioSystem::active_voices(Bus pool) const {
    if (pool == Bus::Music) return (music_current_.voice ? 1u : 0u) + (music_previous_.voice ? 1u : 0u);
    if (pool == Bus::Speech) return speech_voice_ ? 1u : 0u;
    uint32_t n = 0;
    for (const Instance& in : instances_) n += (in.voice && (in.is3d() == (pool == Bus::Sfx3D))) ? 1u : 0u;
    return n;
}

size_t AudioSystem::live_instances() const {
    size_t n = 0;
    for (const Instance& in : instances_) n += in.state != 0 ? 1u : 0u;
    return n;
}

void AudioSystem::update(double dt_seconds) {
    const int64_t dt = std::max<int64_t>(0, static_cast<int64_t>(std::llround(dt_seconds * kUsPerSecond)));
    const double secs = static_cast<double>(dt) / kUsPerSecond;
    backend_->advance(secs);
    now_us_ += dt;
    for (BusState& b : buses_) ramp_toward(b.level, b.target, b.rate, secs);  // §4 bus ramps
    update_sfx(dt);
    update_music(dt);
    update_speech();
    for (PcmStreamState& s : streams_) {
        ramp_toward(s.level, s.target, s.rate, secs);
        backend_->set(s.voice, VoiceParams{stream_voice_gain(s), 0.0f, 1.0f, false});
    }
}

// ---------------------------------------------------------------- SFX: start (§1.2)

void AudioSystem::reject(uint32_t event, std::string_view name, const SfxPlay& p, const char* reason) {
    LogEntry e;
    e.op = LogOp::SfxReject;
    e.event = event == kNoEvent ? std::string(name) : db_->sfx()[event].name;
    e.owner = p.owner;
    e.reason = reason;
    push(std::move(e));
}

uint32_t AudioSystem::play_sfx(std::string_view event, const SfxPlay& p) {
    const uint32_t idx = db_->find_sfx(event);
    if (idx == kNoEvent) {
        reject(kNoEvent, event, p, "unknown_event");
        return 0;
    }
    return play_sfx(idx, p);
}

uint32_t AudioSystem::play_sfx(uint32_t event_index, const SfxPlay& p) {
    if (event_index >= db_->sfx().size()) return 0;
    const SfxEvent& e = db_->sfx()[event_index];
    auto fail = [&](const char* why) {
        reject(event_index, {}, p, why);
        return 0u;
    };
    // 1.
    if (e.is_preset) return fail("preset");
    if (e.play_count == 0) return fail("play_count_0");
    // 2.
    const bool negative = iequals(e.name, db_->constants().sfx_gui_negative_feedback) &&
                          !db_->constants().sfx_gui_negative_feedback.empty();
    if (negative && cinematic_) return fail("negative_feedback_in_cinematic");
    // 3. permission flags
    if (e.localize && !allow_localized_) return fail("localized_off");
    if (e.is_unit_response_vo && !allow_unit_response_vo_) return fail("unit_response_vo_off");
    if (e.is_hud_vo && !allow_hud_vo_) return fail("hud_vo_off");
    if (e.is_ambient_vo && !allow_ambient_vo_) return fail("ambient_vo_off");
    if (!p.force) {
        // 4. GUI-only mode
        if (gui_only_ && (!e.is_gui || negative)) return fail("gui_only");
        // 5. HUD VO vs speech / tutorial
        if (e.is_hud_vo && (speech_playing() || hud_vo_blocked_)) return fail("hud_vo_blocked");
        // 6. Max_Instances (2D here; 3D at voice start, §1.7). Instances fading out are ignored by both the
        // "already attached" loop test and the count (STATION_HANGARS_SOUND §3.5 item 4: 0x2d4470 counts only
        // non-fading ones), so a loop restarted during its fade-out starts a second instance that fades in
        // while the old one fades out.
        if (e.max_instances <= 0) return fail("max_instances_0");
        int32_t count = 0;
        const Instance* first = nullptr;
        for (const Instance& in : instances_) {
            if (in.state == 0 || in.fading || in.event != event_index) continue;
            if (p.kind == SfxPlay::Kind::Object && in.owner == p.owner && e.play_count == -1)
                return fail("loop_already_attached");
            ++count;
            if (!first) first = &in;
        }
        if (count >= e.max_instances && first && !first->is3d()) return fail("max_instances");
        // 7. Overlap_Test
        if (!e.overlap_test.empty()) {
            for (const Instance& in : instances_)
                if (in.state != 0 && iequals(ev(in).overlap_test, e.overlap_test)) return fail("overlap");
        }
        // 8. Probability: int(1, 100), fail when greater
        if (e.probability < 100 && rng_->range_int(1, 100) > e.probability) return fail("probability");
    }
    // 9. 3D-positional calls under the fog of war
    if (p.kind == SfxPlay::Kind::Position && !cinematic_ && position_fogged_ && position_fogged_(p.position))
        return fail("fogged");

    // [port] Kills_Previous_Object_SFX is applied once the start is accepted (§1.5 says "before starting").
    if (e.kills_previous_object_sfx && p.kind == SfxPlay::Kind::Object && p.owner != 0) {
        for (Instance& in : instances_)
            if (in.state != 0 && in.owner == p.owner) {
                LogEntry le;
                le.op = LogOp::SfxStop;
                le.event = ev(in).name;
                le.handle = in.handle;
                le.owner = in.owner;
                le.reason = "killed_by_new";
                push(std::move(le));
                kill(in, "killed_by_new");
            }
    }

    Instance in;
    in.handle = next_handle_++;
    if (next_handle_ == 0xFFFFFFFFu) next_handle_ = 1;  // ids skip -1 (and 0 = refused here)
    in.event = event_index;
    switch (p.kind) {
        case SfxPlay::Kind::Ui: in.type = 0; break;
        case SfxPlay::Kind::Object: in.type = e.is_3d ? 3 : 1; break;
        case SfxPlay::Kind::Position: in.type = 2; break;
    }
    in.owner = p.kind == SfxPlay::Kind::Object ? p.owner : 0;
    in.pos = p.position;
    in.language = cfg_.language;
    in.fade_in_seconds = p.fade_in_seconds;
    in.state = 1;
    // §1.3: align the sequential pre/post counters with the primary one.
    if (e.play_sequentially) {
        SeqCounters& c = sequential_[event_index];
        if (e.pre_samples.size() == e.samples.size()) c.pre = c.primary;
        if (e.post_samples.size() == e.samples.size()) c.post = c.primary;
    }
    LogEntry le;
    le.op = LogOp::SfxStart;
    le.event = e.name;
    le.handle = in.handle;
    le.owner = p.owner;
    push(std::move(le));
    const uint32_t handle = in.handle;
    if (in.is3d()) instances_.push_back(std::move(in));
    else instances_.insert(instances_.begin(), std::move(in));
    for (uint32_t i = 0; i < p.pump; ++i) update_sfx(0);
    return handle;
}

// ---------------------------------------------------------------- SFX: stop (§1.5)

void AudioSystem::kill(Instance& in, const char*) {
    if (in.voice) backend_->stop(in.voice);
    in.voice = 0;
    in.state = 0;
}

void AudioSystem::stop_sfx(uint32_t handle, float fade_seconds) {
    for (Instance& in : instances_) {
        if (in.handle != handle || in.state == 0) continue;
        LogEntry le;
        le.op = LogOp::SfxStop;
        le.event = ev(in).name;
        le.handle = in.handle;
        le.voice = in.voice;
        le.owner = in.owner;
        le.reason = fade_seconds > 0.0f ? "fade" : "stop";
        push(std::move(le));
        if (fade_seconds > 0.0f) {
            if (!in.fading) {
                in.fading = true;
                in.fade_level = 1.0f;
                in.fade_seconds = fade_seconds;
            }
        } else {
            kill(in, "stop");
        }
    }
}

void AudioSystem::stop_object_sfx(uint64_t owner, std::string_view event, float fade_seconds) {
    const uint32_t idx = db_->find_sfx(event);
    std::vector<uint32_t> handles;
    // A faded stop leaves instances that are already fading out alone (their fade keeps its pace); a stop
    // without fade cuts them too.
    for (const Instance& in : instances_)
        if (in.state != 0 && in.owner == owner && in.event == idx && !(in.fading && fade_seconds > 0.0f))
            handles.push_back(in.handle);
    for (uint32_t h : handles) stop_sfx(h, fade_seconds);
}

void AudioSystem::object_removed(uint64_t owner) {
    // §1.5: death / deletion kill every attached instance at once, fading ones included; never the fade path.
    for (Instance& in : instances_) {
        if (in.state == 0 || in.owner != owner) continue;
        LogEntry le;
        le.op = LogOp::SfxStop;
        le.event = ev(in).name;
        le.handle = in.handle;
        le.voice = in.voice;
        le.owner = in.owner;
        le.reason = "object_removed";
        push(std::move(le));
        kill(in, "object_removed");
    }
}

void AudioSystem::stop_all_sfx() {
    for (Instance& in : instances_)
        if (in.state != 0) kill(in, "stop_all");
    LogEntry le;
    le.op = LogOp::SfxStop;
    le.reason = "stop_all";
    push(std::move(le));
}

bool AudioSystem::sfx_live(uint32_t handle) const {
    for (const Instance& in : instances_)
        if (in.handle == handle) return in.state != 0;
    return false;
}

// ---------------------------------------------------------------- SFX: voices (§1.6, §1.7)

VoiceParams AudioSystem::voice_params(const Instance& in) const {
    const SfxEvent& e = ev(in);
    VoiceParams vp;
    float g = bus_gain(e.localize ? AudioBus::Speech : AudioBus::Sfx);  // §4: localized -> speech bus
    g *= static_cast<float>(in.volume) / 100.0f;
    g *= in.fading ? in.fade_level : 1.0f;
    g *= in.fade_in_level;
    if (in.type == 3 && !in.audible) g = 0.0f;  // visibility factor
    if (in.is3d()) {
        g *= scale_3d_ * (attract_ ? 0.0f : 1.0f);
        g *= attenuation(distance_to_listener(in.pos), e.volume_saturation_distance);
        vp.pan = pan_for(in.pos);
    } else {
        vp.pan = std::clamp(static_cast<float>(in.pan) / 100.0f * 2.0f - 1.0f, -1.0f, 1.0f);
    }
    if (ducking_active() && !modal_dialog_) g = std::min(g, cfg_.speech_duck_cap);  // §3.2 ducking (+ movie streams)
    vp.gain = g;
    vp.pitch = static_cast<float>(in.pitch) / 100.0f;
    vp.loop = in.voice_loop;
    return vp;
}

bool AudioSystem::get_voice(Instance& in) {
    const int32_t prio = ev(in).priority;
    auto steal = [&](Instance& victim, const char* why) {
        LogEntry le;
        le.op = LogOp::SfxSteal;
        le.event = ev(victim).name;
        le.handle = victim.handle;
        le.voice = victim.voice;
        le.owner = victim.owner;
        le.reason = std::string(why) + " by " + ev(in).name;
        push(std::move(le));
        backend_->stop(victim.voice);
        victim.voice = 0;  // the victim's wait state sees its voice stopped
    };
    if (!in.is3d()) {
        uint32_t used = 0;
        Instance* victim = nullptr;
        for (Instance& o : instances_) {
            if (!o.voice || o.is3d()) continue;
            ++used;
            const int32_t op = ev(o).priority;
            if (!victim || op > ev(*victim).priority ||
                (op == ev(*victim).priority && o.voice_start_us < victim->voice_start_us))
                victim = &o;
        }
        if (used >= cfg_.max_voices_2d) {
            if (!victim || prio > ev(*victim).priority) return false;
            steal(*victim, "priority");
        }
    } else {
        const float d = distance_to_listener(in.pos);
        // Per-event Max_Instances for 3D (0x2d3f80). Fading-out instances are not counted (and so never chosen
        // here) [port: STATION_HANGARS_SOUND §3.5 item 4 applies the non-fading count to Max_Instances].
        const int32_t maxi = ev(in).max_instances;
        int32_t holders = 0;
        Instance* farthest = nullptr;
        float far_d = -1.0f;
        for (Instance& o : instances_) {
            if (&o == &in || !o.voice || !o.is3d() || o.fading || o.event != in.event) continue;
            ++holders;
            const float od = distance_to_listener(o.pos);
            if (od > far_d) {
                far_d = od;
                farthest = &o;
            }
        }
        if (holders >= maxi && farthest) {
            if (d > far_d) return false;
            steal(*farthest, "max_instances");
        }
        uint32_t used = 0;
        Instance* victim = nullptr;
        float victim_d = 0.0f;
        for (Instance& o : instances_) {
            if (!o.voice || !o.is3d()) continue;
            ++used;
            const int32_t op = ev(o).priority;
            const float od = distance_to_listener(o.pos);
            if (!victim || op > ev(*victim).priority || (op == ev(*victim).priority && od > victim_d)) {
                victim = &o;
                victim_d = od;
            }
        }
        if (used >= cfg_.max_voices_3d) {
            if (!victim || prio > ev(*victim).priority || !(d < victim_d)) return false;
            steal(*victim, "priority");
        }
    }
    in.voice = next_voice_++;
    in.voice_start_us = now_us_;
    return true;
}

std::string AudioSystem::pick_sample(Instance& in, int stage) {
    const SfxEvent& e = ev(in);
    const std::vector<std::string>& list = stage == 3 ? e.pre_samples : stage == 7 ? e.post_samples : e.samples;
    const int32_t n = static_cast<int32_t>(list.size());
    int32_t idx = 0;
    if (e.play_sequentially) {
        SeqCounters& c = sequential_[in.event];
        uint32_t& counter = stage == 3 ? c.pre : stage == 7 ? c.post : c.primary;
        idx = static_cast<int32_t>(counter % static_cast<uint32_t>(n));
        counter = static_cast<uint32_t>(idx + 1) % static_cast<uint32_t>(n);
    } else {
        idx = rng_->range_int(0, n - 1);  // repeats allowed; n = 1 draws nothing
    }
    const std::string& name = list[static_cast<size_t>(idx)];
    return e.localize ? localize_file(name, in.language) : name;
}

// Starts stage 3 (pre), 5 (primary) or 7 (post). false = no voice (the cycle's rest is skipped).
bool AudioSystem::start_stage(Instance& in, int stage) {
    const std::string sample = pick_sample(in, stage);
    if (!get_voice(in)) {
        LogEntry le;
        le.op = LogOp::SfxNoVoice;
        le.event = ev(in).name;
        le.sample = sample;
        le.handle = in.handle;
        le.owner = in.owner;
        push(std::move(le));
        return false;
    }
    in.voice_loop = stage == 5 && ev(in).play_count == -1;
    in.fade_in_level = in.fade_in_seconds > 0.0f ? 0.0f : 1.0f;
    const VoiceParams vp = voice_params(in);
    const bool ok = backend_->start(in.voice, SampleRef{in.is3d() ? Bus::Sfx3D : Bus::Sfx2D, sample}, vp);
    LogEntry le;
    le.op = LogOp::SfxVoice;
    le.event = ev(in).name;
    le.sample = sample;
    le.handle = in.handle;
    le.voice = in.voice;
    le.owner = in.owner;
    le.gain = vp.gain;
    if (!ok) le.reason = "sample_missing";  // stage skipped (the wait sees the voice stopped)
    push(std::move(le));
    return true;
}

// ---------------------------------------------------------------- SFX: state machine (§1.4)

void AudioSystem::run_instance(Instance& in, std::vector<std::pair<uint32_t, SfxPlay>>& chains) {
    const SfxEvent& e = ev(in);
    auto voice_done = [&]() {
        if (in.voice && backend_->playing(in.voice)) return false;
        if (in.voice) backend_->stop(in.voice);
        in.voice = 0;
        return true;
    };
    for (int guard = 0; guard < 32; ++guard) {
        switch (in.state) {
            case 0: return;
            case 1: {  // draws in this order: volume, pan, pitch, predelay, postdelay
                in.volume = rng_->range_int(e.min_volume, e.max_volume);
                in.pan = rng_->range_int(e.min_pan2d, e.max_pan2d);
                in.pitch = rng_->range_int(e.min_pitch, e.max_pitch);
                in.predelay_ms = rng_->range_int(e.min_predelay, e.max_predelay);
                in.postdelay_ms = rng_->range_int(e.min_postdelay, e.max_postdelay);
                LogEntry le;
                le.op = LogOp::SfxCycle;
                le.event = e.name;
                le.handle = in.handle;
                le.owner = in.owner;
                le.volume = in.volume;
                le.pan = in.pan;
                le.pitch = in.pitch;
                le.predelay_ms = in.predelay_ms;
                le.postdelay_ms = in.postdelay_ms;
                push(std::move(le));
                if (in.predelay_ms > 0) {
                    in.delay_us = static_cast<int64_t>(in.predelay_ms) * 1000;
                    in.state = 2;
                    return;  // [M] the countdown starts with the next update
                }
                in.state = e.pre_samples.empty() ? 5 : 3;
                continue;
            }
            case 2:
                in.delay_us -= frame_dt_us_;
                if (in.delay_us > 0) return;
                in.state = e.pre_samples.empty() ? 5 : 3;
                continue;
            case 3:
                if (!start_stage(in, 3)) {
                    in.state = 11;
                    continue;
                }
                in.state = 4;
                return;
            case 4:
                if (!voice_done()) return;
                in.state = 5;
                continue;
            case 5:
                if (e.samples.empty()) {  // "has no primary samples defined": skip to post
                    in.state = 7;
                    continue;
                }
                if (!start_stage(in, 5)) {
                    in.state = 11;
                    continue;
                }
                in.state = 6;
                return;
            case 6:
                if (!voice_done()) return;
                in.state = 7;
                continue;
            case 7:
                if (e.post_samples.empty()) {
                    in.state = 9;
                    continue;
                }
                if (!start_stage(in, 7)) {
                    in.state = 11;
                    continue;
                }
                in.state = 8;
                return;
            case 8:
                if (!voice_done()) return;
                in.state = 9;
                continue;
            case 9:
                if (in.postdelay_ms > 0) {
                    in.delay_us = static_cast<int64_t>(in.postdelay_ms) * 1000;
                    in.state = 10;
                    return;
                }
                in.state = 11;
                continue;
            case 10:
                in.delay_us -= frame_dt_us_;
                if (in.delay_us > 0) return;
                in.state = 11;
                continue;
            case 11: {
                if (in.voice) backend_->stop(in.voice);
                in.voice = 0;
                ++in.cycle;
                if (e.play_count == -1 || in.cycle < e.play_count) {
                    in.state = 1;
                    continue;
                }
                LogEntry le;
                le.op = LogOp::SfxEnd;
                le.event = e.name;
                le.handle = in.handle;
                le.owner = in.owner;
                push(std::move(le));
                // Chain: 2D instances only, and only to a 2D event.
                if (in.type <= 1 && e.chained_index != kNoEvent && !db_->sfx()[e.chained_index].is_3d) {
                    SfxPlay cp;
                    cp.kind = in.type == 1 ? SfxPlay::Kind::Object : SfxPlay::Kind::Ui;
                    cp.owner = in.owner;
                    cp.position = in.pos;
                    chains.emplace_back(e.chained_index, cp);
                }
                in.state = 0;
                return;
            }
            default: in.state = 0; return;
        }
    }
}

void AudioSystem::update_sfx(int64_t dt_us) {
    frame_dt_us_ = dt_us;
    const double secs = static_cast<double>(dt_us) / kUsPerSecond;
    // Attached instances follow their object (§1.4) and take its visibility (§1.6).
    if (object_query_) {
        for (Instance& in : instances_) {
            if (in.state == 0 || in.owner == 0) continue;
            if (auto o = object_query_(in.owner)) {
                in.pos = o->position;
                in.audible = o->audible || cinematic_;
            }
        }
    }
    // Sort: 2D first, then 3D nearest to the listener first. [port] stable (the original's qsort is not).
    std::stable_sort(instances_.begin(), instances_.end(), [&](const Instance& a, const Instance& b) {
        if (a.is3d() != b.is3d()) return !a.is3d();
        if (!a.is3d()) return false;
        return distance_to_listener(a.pos) < distance_to_listener(b.pos);
    });
    std::vector<std::pair<uint32_t, SfxPlay>> chains;
    std::vector<uint32_t> order;
    order.reserve(instances_.size());
    for (const Instance& in : instances_) order.push_back(in.handle);
    for (uint32_t h : order) {
        // get_voice may touch other instances but never reallocates the vector.
        auto it = std::find_if(instances_.begin(), instances_.end(), [&](const Instance& i) { return i.handle == h; });
        if (it == instances_.end() || it->state == 0) continue;
        Instance& in = *it;
        if (in.fading) {
            // §1.5 / §7 [port]: wall-time fade (the original steps (1/30)/fade per update).
            in.fade_level -= static_cast<float>(secs / static_cast<double>(in.fade_seconds));
            if (in.fade_level <= 0.0f) {
                kill(in, "faded");
                continue;
            }
        }
        if (in.voice && in.fade_in_level < 1.0f && in.fade_in_seconds > 0.0f)
            in.fade_in_level = std::min(1.0f, in.fade_in_level + static_cast<float>(secs / in.fade_in_seconds));
        if (!in.fading) run_instance(in, chains);
    }
    for (Instance& in : instances_)
        if (in.voice && in.state != 0) backend_->set(in.voice, voice_params(in));
    instances_.erase(std::remove_if(instances_.begin(), instances_.end(), [](const Instance& i) { return i.state == 0; }),
                     instances_.end());
    for (const auto& [event, cp] : chains) {
        LogEntry le;
        le.op = LogOp::SfxChain;
        le.event = db_->sfx()[event].name;
        le.owner = cp.owner;
        push(std::move(le));
        play_sfx(event, cp);
    }
}

// ---------------------------------------------------------------- music (§2)

bool AudioSystem::play_music(std::string_view event, uint8_t mode, bool resume_previous, bool force) {
    const uint32_t idx = db_->find_music(event);
    auto refuse = [&](const char* why) {
        LogEntry le;
        le.op = LogOp::MusicRefuse;
        le.event = std::string(event);
        le.reason = why;
        push(std::move(le));
        return false;
    };
    if (idx == kNoEvent) return refuse("unknown_event");
    if (!music_enabled_) return refuse("disabled");
    const uint32_t current = music_queue_.empty() ? kNoEvent : music_queue_.front().event;
    if (!force && current == idx) return refuse("already_current");
    // §2.3 / STRATEGIC_GAPS §7.4: a front-end (mode 13) request is refused while the attract lock is set.
    if (!force && mode == music_mode::kFrontEnd && music_lock_) return refuse("locked");
    MusicEntry e;
    e.event = idx;
    e.mode = mode;
    e.resume_previous = resume_previous;
    if (!music_queue_.empty()) {
        e.prev_event = music_queue_.front().event;
        e.prev_mode = music_queue_.front().mode;
    }
    music_queue_.clear();
    music_queue_.push_back(e);
    music_mode_ = mode;
    // A free-RNG draw is made when an event is queued; its value is not used for the file choice.
    const size_t n = db_->music()[idx].files.size();
    if (n > 0) (void)rng_->range_int(0, static_cast<int32_t>(n) - 1);
    LogEntry le;
    le.op = LogOp::MusicPlay;
    le.event = db_->music()[idx].name;
    le.volume = mode;
    push(std::move(le));
    return true;
}

void AudioSystem::close_stream(Stream& s) {
    if (s.voice) backend_->stop(s.voice);
    s = Stream{};
}

void AudioSystem::stop_all_music() {
    close_stream(music_current_);
    close_stream(music_previous_);
    music_queue_.clear();
    LogEntry le;
    le.op = LogOp::MusicStop;
    le.reason = "stop_all";
    push(std::move(le));
}

bool AudioSystem::end_music_mode(uint8_t mode) {
    if (music_queue_.empty() || music_queue_.front().mode != mode) return false;
    const MusicEntry done = music_queue_.front();
    music_queue_.erase(music_queue_.begin());
    close_stream(music_current_);
    LogEntry le;
    le.op = LogOp::MusicStop;
    le.event = db_->music()[done.event].name;
    le.reason = "mode_end";
    push(std::move(le));
    if (done.prev_event != kNoEvent) play_music(db_->music()[done.prev_event].name, done.prev_mode, false, true);
    return true;
}

std::string AudioSystem::current_music() const {
    return music_queue_.empty() ? std::string() : db_->music()[music_queue_.front().event].name;
}

void AudioSystem::start_music_file(MusicEntry& e) {
    const MusicEvent& m = db_->music()[e.event];
    if (m.files.empty()) {
        e.state = 3;
        return;
    }
    uint32_t& counter = music_file_counter_[e.event];  // sequential, session-wide
    const uint32_t idx = counter % static_cast<uint32_t>(m.files.size());
    counter = (idx + 1) % static_cast<uint32_t>(m.files.size());
    // Crossfade: the older previous closes now; the current fades out over the new event's
    // Fade_Out_Previous_Seconds; the new one ramps up over Fade_In_Seconds.
    close_stream(music_previous_);
    if (music_current_.voice && backend_->playing(music_current_.voice) && m.fade_out_previous_seconds > 0.0f) {
        music_previous_ = music_current_;
        music_previous_.target = 0.0f;
        music_previous_.rate = music_previous_.level / m.fade_out_previous_seconds;
        music_current_ = Stream{};
    } else {
        close_stream(music_current_);
    }
    Stream& s = music_current_;
    s.voice = next_voice_++;
    s.event = e.event;
    s.level = m.fade_in_seconds > 0.0f ? 0.0f : 1.0f;
    s.target = 1.0f;
    s.rate = m.fade_in_seconds > 0.0f ? 1.0f / m.fade_in_seconds : 0.0f;
    s.internal_loop = m.mss_internal_loop;
    s.file = m.files[idx];
    VoiceParams vp;
    vp.gain = bus_gain(AudioBus::Music) * static_cast<float>(m.volume_percent) / 100.0f * s.level;
    vp.loop = s.internal_loop;
    const bool ok = backend_->start(s.voice, SampleRef{Bus::Music, s.file}, vp);
    LogEntry le;
    le.op = LogOp::MusicTrack;
    le.event = m.name;
    le.sample = s.file;
    le.voice = s.voice;
    le.gain = vp.gain;
    if (!ok) le.reason = "sample_missing";
    push(std::move(le));
    ++e.files_played;
    e.state = 2;
}

void AudioSystem::update_music(int64_t dt_us) {
    const double secs = static_cast<double>(dt_us) / kUsPerSecond;
    // Playlist manager (§2.2): 1 start file -> 2 playing -> 3 ended.
    for (int guard = 0; guard < 4 && !music_queue_.empty(); ++guard) {
        MusicEntry& e = music_queue_.front();
        const MusicEvent& m = db_->music()[e.event];
        if (e.state == 1) {
            start_music_file(e);
            break;
        }
        if (e.state == 2) {
            if (music_current_.voice && backend_->playing(music_current_.voice)) break;
            e.state = 3;
        }
        if (e.state == 3) {
            const bool one_pass_done = e.mode == music_mode::kOnePass && e.files_played >= m.files.size();
            if (m.loop && !one_pass_done) {
                e.state = 1;
                continue;
            }
            const MusicEntry done = e;
            music_queue_.erase(music_queue_.begin());
            close_stream(music_current_);
            LogEntry le;
            le.op = LogOp::MusicStop;
            le.event = m.name;
            le.reason = "ended";
            push(std::move(le));
            if ((done.resume_previous || done.mode == music_mode::kOnePass) && done.prev_event != kNoEvent)
                play_music(db_->music()[done.prev_event].name, done.prev_mode, false, true);
            break;
        }
    }
    // Ramps (per wall time) and gains.
    for (Stream* s : {&music_previous_, &music_current_}) {
        if (!s->voice) continue;
        ramp_toward(s->level, s->target, s->rate, secs);
        if (s == &music_previous_ && s->level <= 0.0f) {
            close_stream(*s);
            continue;
        }
        VoiceParams vp;
        vp.gain = bus_gain(AudioBus::Music) * static_cast<float>(db_->music()[s->event].volume_percent) / 100.0f *
                  s->level;
        vp.loop = s->internal_loop;
        backend_->set(s->voice, vp);
    }
}

void AudioSystem::resume_mode_based_music(bool force) {
    if (!force && (music_mode_ == music_mode::kSuperWeapon || music_mode_ == music_mode::kSplash ||
                   music_mode_ == music_mode::kLua || attract_))
        return;
    if (!music_resolver_) return;
    const std::string name = music_resolver_(base_music_mode_, *rng_);
    if (!name.empty()) play_music(name, base_music_mode_, false, force);
}

void AudioSystem::notify_combat(bool involves_local_player) {
    if (!involves_local_player) return;
    peace_ticks_ = 0;
    if (music_mode_ == music_mode::kTacticalBattle || !music_resolver_) return;
    const std::string name = music_resolver_(music_mode::kTacticalBattle, *rng_);
    if (!name.empty()) play_music(name, music_mode::kTacticalBattle);
}

void AudioSystem::music_sim_tick() {
    ++peace_ticks_;
    if (music_mode_ != music_mode::kTacticalBattle) return;
    const AudioConstants& c = db_->constants();
    const float peace = mode_ == AudioMode::Space ? c.music_space_battle_to_ambient_peace_seconds
                                                  : c.music_land_battle_to_ambient_peace_seconds;
    if (static_cast<double>(peace_ticks_) < 30.0 * static_cast<double>(peace)) return;
    if (!music_resolver_) return;
    const std::string name = music_resolver_(music_mode::kTacticalAmbient, *rng_);
    if (!name.empty()) play_music(name, music_mode::kTacticalAmbient);
}

// ---------------------------------------------------------------- speech (§3)

uint32_t AudioSystem::queue_speech(std::string_view event) {
    const uint32_t idx = db_->find_speech(event);
    LogEntry le;
    le.op = LogOp::SpeechQueue;
    le.event = std::string(event);
    if (idx == kNoEvent) {
        le.reason = "unknown_event";
        push(std::move(le));
        return 0;
    }
    le.event = db_->speech()[idx].name;
    const uint32_t ticket = next_speech_ticket_++;
    if (next_speech_ticket_ == 0) next_speech_ticket_ = 1;
    le.handle = ticket;
    push(std::move(le));
    speech_queue_.push_back(SpeechEntry{idx, ticket});
    return ticket;
}

bool AudioSystem::speech_pending(uint32_t ticket) const {
    for (const SpeechEntry& e : speech_queue_)
        if (e.ticket == ticket) return true;
    return false;
}

void AudioSystem::cancel_speech(uint32_t ticket) {
    for (auto it = speech_queue_.begin(); it != speech_queue_.end(); ++it) {
        if (it->ticket != ticket) continue;
        const bool head = it == speech_queue_.begin();
        if (head && speech_voice_) {
            backend_->stop(speech_voice_);
            speech_voice_ = 0;
        }
        const std::string name = db_->speech()[it->event].name;
        speech_queue_.erase(it);
        LogEntry le;
        le.op = LogOp::SpeechKill;
        le.event = name;
        le.handle = ticket;
        le.reason = "cancel";
        push(std::move(le));
        for (auto& cb : speech_done_) cb(name, true);
        return;
    }
}

void AudioSystem::update_speech() {
    if (speech_queue_.empty()) return;
    const SpeechEvent& s = db_->speech()[speech_queue_.front().event];
    if (speech_voice_ == 0) {
        if (s.files.empty()) {
            const std::string name = s.name;
            speech_queue_.pop_front();
            LogEntry le;
            le.op = LogOp::SpeechEnd;
            le.event = name;
            le.reason = "no_files";
            push(std::move(le));
            for (auto& cb : speech_done_) cb(name, false);
            return;
        }
        const int32_t pick = rng_->range_int(0, static_cast<int32_t>(s.files.size()) - 1);
        const std::string file = localize_file(s.files[static_cast<size_t>(pick)], cfg_.language);
        speech_voice_ = next_voice_++;
        VoiceParams vp;
        vp.gain = bus_gain(AudioBus::Speech) * static_cast<float>(s.volume_percent) / 100.0f;
        const bool ok = backend_->start(speech_voice_, SampleRef{Bus::Speech, file}, vp);
        LogEntry le;
        le.op = LogOp::SpeechStart;
        le.event = s.name;
        le.sample = file;
        le.voice = speech_voice_;
        le.handle = speech_queue_.front().ticket;
        le.gain = vp.gain;
        if (!ok) le.reason = "sample_missing";
        push(std::move(le));
        for (auto& cb : speech_start_) cb(s, file);
        return;
    }
    if (backend_->playing(speech_voice_)) {
        VoiceParams vp;
        vp.gain = bus_gain(AudioBus::Speech) * static_cast<float>(s.volume_percent) / 100.0f;
        backend_->set(speech_voice_, vp);
        return;
    }
    backend_->stop(speech_voice_);
    speech_voice_ = 0;
    const std::string name = s.name;
    const uint32_t ticket = speech_queue_.front().ticket;
    speech_queue_.pop_front();
    LogEntry le;
    le.op = LogOp::SpeechEnd;
    le.event = name;
    le.handle = ticket;
    push(std::move(le));
    for (auto& cb : speech_done_) cb(name, false);
    // The next entry starts on the next update.
}

void AudioSystem::stop_all_speech() {
    if (speech_voice_) backend_->stop(speech_voice_);
    speech_voice_ = 0;
    std::vector<std::string> killed;
    for (const SpeechEntry& q : speech_queue_) killed.push_back(db_->speech()[q.event].name);
    speech_queue_.clear();
    for (const std::string& n : killed) {
        LogEntry le;
        le.op = LogOp::SpeechKill;
        le.event = n;
        push(std::move(le));
        for (auto& cb : speech_done_) cb(n, true);
    }
}

std::string AudioSystem::current_speech() const {
    return speech_voice_ && !speech_queue_.empty() ? db_->speech()[speech_queue_.front().event].name : std::string();
}

}  // namespace alamo::audio
