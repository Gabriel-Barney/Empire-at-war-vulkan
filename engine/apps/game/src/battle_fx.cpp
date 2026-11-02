#include "alamo/game/battle_fx.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/type_fields.hpp"
#include "alamo/tactical/abilities/special_weapons.hpp"

namespace alamo::game {

namespace ta = tactical::abilities;
using tactical::Vec3;

std::vector<TswCue> tsw_cues(std::span<const ta::SpecialWeaponEvent> events, const objects::ObjectStore& store) {
    std::vector<TswCue> out;
    for (const ta::SpecialWeaponEvent& e : events) {
        TswCue c;
        objects::ObjectId id = objects::kNoObjectId;
        switch (e.kind) {
            case ta::SwEventKind::TswFiring:
                c.kind = TswCue::Kind::Firing;
                id = e.id;
                break;
            case ta::SwEventKind::TswAttackAnim:
                c.kind = TswCue::Kind::AttackAnim;
                id = e.id;
                break;
            case ta::SwEventKind::TswPlanetDestroyed:  // §4 step 3: no target dummy -> no explosion, shake or sound
                c.kind = TswCue::Kind::PlanetDestroyed;
                id = e.target;
                break;
            case ta::SwEventKind::LobChargeup:  // S8 t31
                c.kind = TswCue::Kind::LobChargeup;
                id = e.id;
                break;
            case ta::SwEventKind::LobCharged:
                c.kind = TswCue::Kind::LobCharged;
                id = e.id;
                break;
            default: continue;
        }
        if (id == ta::kNoObject) continue;
        const objects::ObjectHandle h = store.find(id);
        if (!store.exists(h)) continue;  // the dummy killed this tick is dead but not flushed yet
        c.object = id;
        c.type = store.type(h);
        if (c.kind == TswCue::Kind::PlanetDestroyed) {
            c.position = e.point;
            c.pick = e.value;
        } else {
            const auto p = store.position(h);
            c.position = {p.x, p.y, p.z};
            c.target = e.target == ta::kNoObject ? objects::kNoObjectId : e.target;
        }
        if (c.kind == TswCue::Kind::LobChargeup || c.kind == TswCue::Kind::LobCharged) {
            c.target = objects::kNoObjectId;
            c.frame = e.frame;
        }
        out.push_back(c);
    }
    return out;
}

void track_lob_chargeups(std::vector<TswCue>& active, std::span<const TswCue> cues) {
    for (const TswCue& c : cues) {
        if (c.kind != TswCue::Kind::LobChargeup && c.kind != TswCue::Kind::LobCharged) continue;
        std::erase_if(active, [&c](const TswCue& a) { return a.object == c.object; });
        if (c.kind == TswCue::Kind::LobChargeup) active.push_back(c);
    }
}

// ---- §11.1 noise and envelopes --------------------------------------------------------------------------------------
namespace {

constexpr std::array<uint8_t, 256> kPerm = {
    151, 160, 137, 91,  90,  15,  131, 13,  201, 95,  96,  53,  194, 233, 7,   225, 140, 36,  103, 30,  69,  142, 8,   99,  37,  240,
    21,  10,  23,  190, 6,   148, 247, 120, 234, 75,  0,   26,  197, 62,  94,  252, 219, 203, 117, 35,  11,  32,  57,  177, 33,  88,
    237, 149, 56,  87,  174, 20,  125, 136, 171, 168, 68,  175, 74,  165, 71,  134, 139, 48,  27,  166, 77,  146, 158, 231, 83,  111,
    229, 122, 60,  211, 133, 230, 220, 105, 92,  41,  55,  46,  245, 40,  244, 102, 143, 54,  65,  25,  63,  161, 1,   216, 80,  73,
    209, 76,  132, 187, 208, 89,  18,  169, 200, 196, 135, 130, 116, 188, 159, 86,  164, 100, 109, 198, 173, 186, 3,   64,  52,  217,
    226, 250, 124, 123, 5,   202, 38,  147, 118, 126, 255, 82,  85,  212, 207, 206, 59,  227, 47,  16,  58,  17,  182, 189, 28,  42,
    223, 183, 170, 213, 119, 248, 152, 2,   44,  154, 163, 70,  221, 153, 101, 155, 167, 43,  172, 9,   129, 22,  39,  253, 19,  98,
    108, 110, 79,  113, 224, 232, 178, 185, 112, 104, 218, 246, 97,  228, 251, 34,  242, 193, 238, 210, 144, 12,  191, 179, 162, 241,
    81,  51,  145, 235, 249, 14,  239, 107, 49,  192, 214, 31,  181, 199, 106, 157, 184, 84,  204, 176, 115, 121, 50,  45,  127, 4,
    150, 254, 138, 236, 205, 93,  222, 114, 67,  29,  24,  72,  243, 141, 128, 195, 78,  66,  215, 61,  156, 180};

int perm(int i) { return kPerm[static_cast<size_t>(i & 255)]; }  // the table stored twice = index mod 256
float fade(float f) { return f * f * (3.0f - 2.0f * f); }        // cubic 3f^2 - 2f^3 (not the 2002 quintic)
float lerp(float t, float a, float b) { return a + t * (b - a); }
float grad(int hash, float x, float y, float z) {
    const int h = hash & 15;
    const float u = h < 8 ? x : y;
    const float v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
    return ((h & 1) != 0 ? -u : u) + ((h & 2) != 0 ? -v : v);
}
float smooth(float u) { return 3.0f * u * u - 2.0f * u * u * u; }
Vec3 normalized(Vec3 v) {
    const float l = std::sqrt(tactical::dot(v, v));
    return l > 0.0f ? v * (1.0f / l) : Vec3{};
}

}  // namespace

std::span<const uint8_t, 256> perlin_permutation() { return kPerm; }

float improved_noise(float x, float y, float z) {
    const float fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    const int X = static_cast<int>(fx) & 255, Y = static_cast<int>(fy) & 255, Z = static_cast<int>(fz) & 255;
    x -= fx;
    y -= fy;
    z -= fz;
    const float u = fade(x), v = fade(y), w = fade(z);
    const int A = perm(X) + Y, AA = perm(A) + Z, AB = perm(A + 1) + Z;
    const int B = perm(X + 1) + Y, BA = perm(B) + Z, BB = perm(B + 1) + Z;
    return lerp(w,
                lerp(v, lerp(u, grad(perm(AA), x, y, z), grad(perm(BA), x - 1.0f, y, z)),
                     lerp(u, grad(perm(AB), x, y - 1.0f, z), grad(perm(BB), x - 1.0f, y - 1.0f, z))),
                lerp(v, lerp(u, grad(perm(AA + 1), x, y, z - 1.0f), grad(perm(BA + 1), x - 1.0f, y, z - 1.0f)),
                     lerp(u, grad(perm(AB + 1), x, y - 1.0f, z - 1.0f), grad(perm(BB + 1), x - 1.0f, y - 1.0f, z - 1.0f))));
}

float noise_fbm(float x, float y, float z) {
    float f = 1.0f, sum = 0.0f;
    for (int i = 0; i < 3; ++i) {
        x *= f;
        y *= f;
        z *= f;
        sum += improved_noise(x, y, z) / f;
        f *= 1.9f;
    }
    return sum;
}

float shake_ease(float t, float ease_in, float ease_out) {
    const float a = std::clamp(ease_in, 0.0f, 1.0f), b = std::clamp(ease_out, 0.0f, 1.0f);
    const float e = 1.0f - b;
    const float lo = std::min(a, e), hi = std::max(a, e);
    if (t < lo) return t <= 0.0f ? 0.0f : smooth(t / lo);
    if (t <= hi) return 1.0f;
    if (t >= 1.0f) return 0.0f;
    return 1.0f - smooth((t - hi) / (1.0f - hi));
}

float shot_accel(float s, float a, float b) {
    const float e = 1.0f - b;
    const float denom = 2.0f - a - b;
    if (!(denom > 0.0f)) return s;  // a = b = 1: no constant phase (not used by the TSW shot) [port]
    const float v = 2.0f / denom;
    if (s < a) return v * s * s / (2.0f * a);
    if (s < e || !(b > 0.0f)) return v * a / 2.0f + (s - a) * v;
    return v * a / 2.0f + (e - a) * v + v * (s - s * s / 2.0f - e + e * e / 2.0f) / b;
}

ShakeEffect explosion_shake(float jitter, float distance, int32_t noise_draw) {
    ShakeEffect e;  // stored straight in: the ranged-parameter limits (amplitude <= 1000, roll <= 90) are not applied
    e.x_amplitude = jitter * 500.0f / distance;
    e.y_amplitude = jitter * 500.0f / distance;
    e.roll_amplitude = jitter * 30.0f / distance;
    e.frequency = static_cast<float>(kCinematicFps) * jitter * 11.0f;
    e.noise_offset = static_cast<float>(noise_draw) / 50.0f;
    e.ease_in = 0.0f;
    e.ease_out = 0.25f;
    return e;
}

float explosion_jitter_factor(const objects::GameObjectTypes& types, objects::TypeIndex explosion) {
    static const uint32_t f = objects::find_type_field("Explosion_Jitter_Factor");
    if (explosion == objects::kNone || f == 0xFFFFFFFFu) return 1.0f;
    const objects::FieldValue* v = types.value(explosion, f);
    return v ? v->f : 1.0f;  // type constructor 0x367250 default
}

objects::TypeIndex tsw_explosion_type(const objects::GameObjectTypes& types, objects::TypeIndex dummy_type, int32_t pick) {
    if (dummy_type == objects::kNone || pick < 0) return objects::kNone;
    const std::vector<objects::TypeIndex> list = types.get_type_refs(dummy_type, "Death_Explosions");
    return static_cast<size_t>(pick) < list.size() ? list[static_cast<size_t>(pick)] : objects::kNone;
}

int32_t UnsyncedRandom::range(int32_t lo, int32_t hi) {
    if (lo >= hi) return lo;
    s_ ^= s_ >> 12;
    s_ ^= s_ << 25;
    s_ ^= s_ >> 27;
    const uint64_t r = s_ * 2685821657736338717ull;
    const uint64_t span = static_cast<uint64_t>(static_cast<int64_t>(hi) - static_cast<int64_t>(lo)) + 1u;
    return static_cast<int32_t>(static_cast<int64_t>(lo) + static_cast<int64_t>((r >> 11) % span));
}

void CameraShake::start(const ShakeEffect& e, int32_t start_clock, int32_t duration_frames) {
    if (duration_frames <= 0) return;
    if (trackers_.size() >= kMaxTrackers) trackers_.erase(trackers_.begin());
    trackers_.push_back({e, start_clock, duration_frames});
}

CameraShake::Sample CameraShake::evaluate(int32_t clock, const Vec3& right, const Vec3& up) const {
    Sample out;
    for (const Tracker& k : trackers_) {
        const float t = static_cast<float>(clock - k.start) / static_cast<float>(k.duration);
        if (t < 0.0f || t > 1.0f) continue;
        const float s = k.e.frequency * t;
        const float env = shake_ease(t, k.e.ease_in, k.e.ease_out);
        const float dx = noise_fbm(s, k.e.noise_offset, 0.0f) * k.e.x_amplitude * env;
        const float dy = noise_fbm(s, k.e.noise_offset, 100.0f) * k.e.y_amplitude * env;
        const float droll = noise_fbm(s, k.e.noise_offset, 303.1416015625f) * k.e.roll_amplitude * env;
        out.offset = out.offset + right * dx + up * dy;
        out.roll_deg += droll;
    }
    return out;
}

bool cinematic_shake(CameraShake& shake, bool cinematic_active, bool battle_mode, float jitter, float distance, int32_t clock,
                     UnsyncedRandom& rng) {
    if (!battle_mode || !cinematic_active) return false;
    shake.start(explosion_shake(jitter, distance, rng.range(0, 100)), clock, kCinematicFps);
    return true;
}

void camera_axes(const Vec3& eye, const Vec3& look, float roll_deg, Vec3& right, Vec3& up) {
    const Vec3 f = normalized(look - eye);
    right = normalized(tactical::cross(f, Vec3{0.0f, 0.0f, 1.0f}));
    if (tactical::dot(right, right) == 0.0f) right = {1.0f, 0.0f, 0.0f};  // looking straight up / down
    up = tactical::cross(right, f);
    if (roll_deg != 0.0f) {  // about the view axis [M: sign of 0x22d600's roll]
        const float r = roll_deg * (3.14159265358979f / 180.0f);
        const float c = std::cos(r), s = std::sin(r);
        const Vec3 r2 = right * c + up * s;
        up = up * c - right * s;
        right = r2;
    }
}

CinematicPose apply_shake(const Vec3& pos, const Vec3& look, float roll_deg, const CameraShake& shake, int32_t clock) {
    CinematicPose p;
    p.eye = pos;
    p.look = look;
    p.roll_deg = roll_deg;
    Vec3 right, up;
    camera_axes(pos, look, roll_deg, right, up);
    if (!shake.empty()) {
        const CameraShake::Sample s = shake.evaluate(clock, right, up);
        const Vec3 d = normalized(look - pos);
        p.eye = pos + s.offset * 0.1f;
        p.look = pos + d * 1000.0f + s.offset;
        p.roll_deg = roll_deg + s.roll_deg;
        camera_axes(p.eye, p.look, p.roll_deg, right, up);
    }
    p.up = up;
    return p;
}

// ---- §11.2 the firing shot ------------------------------------------------------------------------------------------
void TswFiringShot::begin(const TswShotInputs& in, uint32_t fire_frame, int32_t cursor_x, int32_t cursor_y) {
    in_ = in;
    fire_frame_ = fire_frame;
    cursor_x_ = cursor_x;
    cursor_y_ = cursor_y;
    active_ = true;
    last_tsw_ = ObjectFrame{};
    last_tsw_.valid = true;
    last_tsw_.position = in.tsw_position;
    const Vec3 E = in.half_extents;
    using S = ShotKey::Segment;
    eye_.clear();
    look_.clear();
    eye_.push_back({0, S::Spline, true, {2.0f * E.x, -2.0f * E.y, 2.0f * E.z}, 0.5f, 0.0f});                  // P1
    eye_.push_back({kGlideEnd, S::Hold, true, {1.5f * E.x, -1.5f * E.y, 1.5f * E.z}, 0.0f, 0.25f});           // P2
    eye_.push_back({kCut, S::Spline, true, {15.0f * E.x, -5.0f * E.y, 5.0f * E.z}, 0.5f, 0.0f});              // P3
    if (in.have_station) {                                                                                    // P4
        const Vec3 mt = in.station_position - in.dummy_position;
        const float len = std::sqrt(tactical::dot(mt, mt));
        const Vec3 u = normalized(mt);
        eye_.push_back({kLength, S::Spline, false, in.station_position + u * (len + kStationBeyond), 0.5f, 0.0f});
    }
    look_.push_back({0, S::Hold, true, {0.0f, -E.y, 0.15f * E.z}, 0.0f, 0.0f});                               // L1
    look_.push_back({kCut, S::Hold, false, (in.tsw_position + in.dummy_position) * 0.5f, 0.0f, 0.0f});        // L2 = Mid
}

const ObjectFrame& TswFiringShot::frame_of(const ObjectFrame& tsw) {
    if (tsw.valid) last_tsw_ = tsw;
    return last_tsw_;
}

Vec3 TswFiringShot::key_position(const ShotKey& k, const ObjectFrame& tsw) const {
    if (!k.attached) return k.where;
    return tsw.apply(k.where) + Vec3{0.0f, 0.0f, in_.z_adjust};  // 0x49d640: + Ranged_Target_Z_Adjust on z
}

Vec3 TswFiringShot::evaluate(std::span<const ShotKey> keys, int32_t c, const ObjectFrame& tsw) const {
    if (keys.empty()) return {};
    if (c < keys.front().frame) return key_position(keys.front(), tsw);
    size_t k = 0;
    while (k + 1 < keys.size() && keys[k + 1].frame <= c) ++k;
    if (k + 1 >= keys.size()) return key_position(keys[k], tsw);  // after the last key: hold it (re-evaluated if attached)
    const ShotKey& a = keys[k];
    const ShotKey& b = keys[k + 1];
    const float sigma = static_cast<float>(c - a.frame) / static_cast<float>(b.frame - a.frame);
    const float w = shot_accel(sigma, a.ease_out, b.ease_in);
    const Vec3 pa = key_position(a, tsw), pb = key_position(b, tsw);
    switch (a.segment) {
        case ShotKey::Segment::Hold:
        case ShotKey::Segment::Orbit:  // [port] mode 3 (orbit, period +0x4c) is not used by this shot: held
            return pa;
        case ShotKey::Segment::Linear: return pa + (pb - pa) * w;
        case ShotKey::Segment::Spline:
            if (!a.attached && !b.attached) {  // Catmull-Rom Hermite between two world keys (0x499ef0), tangent scale 0.5
                const Vec3 p0 = k > 0 ? key_position(keys[k - 1], tsw) : pa;
                const Vec3 p3 = k + 2 < keys.size() ? key_position(keys[k + 2], tsw) : pb;
                const Vec3 m1 = (pb - p0) * 0.5f, m2 = (p3 - pa) * 0.5f;
                const float w2 = w * w, w3 = w2 * w;
                return pa * (2.0f * w3 - 3.0f * w2 + 1.0f) + m1 * (w3 - 2.0f * w2 + w) + pb * (-2.0f * w3 + 3.0f * w2) + m2 * (w3 - w2);
            }
            return pa + (pb - pa) * w;
    }
    return pa;
}

Vec3 TswFiringShot::eye(int32_t c, const ObjectFrame& tsw) { return evaluate(eye_, c, frame_of(tsw)); }
Vec3 TswFiringShot::look(int32_t c, const ObjectFrame& tsw) { return evaluate(look_, c, frame_of(tsw)); }

bool TswFiringShot::input_cancels(int32_t mouse_x, int32_t mouse_y, uint32_t width, uint32_t height, bool button_or_wheel) const {
    if (button_or_wheel) return true;
    const float dx = static_cast<float>(std::abs(mouse_x - cursor_x_)), dy = static_cast<float>(std::abs(mouse_y - cursor_y_));
    return !(dx < static_cast<float>(width) / 20.0f && dy < static_cast<float>(height) / 20.0f);
}

void Letterbox::update(float dt_seconds) {
    if (!(dt_seconds > 0.0f)) return;
    const float step = dt_seconds / kSlideSeconds;
    level_ = level_ < target_ ? std::min(target_, level_ + step) : std::max(target_, level_ - step);
}

}  // namespace alamo::game
