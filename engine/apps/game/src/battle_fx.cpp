#include "alamo/game/battle_fx.hpp"

#include <algorithm>
#include <cmath>

#include "alamo/tactical/abilities/special_weapons.hpp"

namespace alamo::game {

namespace ta = tactical::abilities;

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
            default: continue;
        }
        if (id == ta::kNoObject) continue;
        const objects::ObjectHandle h = store.find(id);
        if (!store.exists(h)) continue;  // the dummy killed this tick is dead but not flushed yet
        c.object = id;
        c.type = store.type(h);
        if (c.kind == TswCue::Kind::PlanetDestroyed) {
            c.position = e.point;
        } else {
            const Vec3 p = store.position(h);
            c.position = {p.x, p.y, p.z};
        }
        out.push_back(c);
    }
    return out;
}

void CameraShake::start(const tactical::Vec3& source, const CameraShakeParams& p) {
    if (!(p.duration_s > 0.0f) || !(p.magnitude > 0.0f)) return;
    if (shakes_.size() >= kMaxShakes) shakes_.erase(shakes_.begin());
    Shake s;
    s.source = source;
    s.p = p;
    shakes_.push_back(s);
}

void CameraShake::update(double dt_seconds) {
    if (!(dt_seconds > 0.0)) return;
    for (Shake& s : shakes_) s.t += dt_seconds;
    std::erase_if(shakes_, [](const Shake& s) { return s.t >= static_cast<double>(s.p.duration_s); });
}

tactical::Vec3 CameraShake::offset(const tactical::Vec3& eye, const tactical::Vec3& target) const {
    tactical::Vec3 sum;
    if (shakes_.empty()) return sum;
    const tactical::Vec3 d = eye - target;
    const float distance = std::sqrt(tactical::dot(d, d));
    constexpr double kTwoPi = 6.283185307179586;
    for (const Shake& s : shakes_) {
        const double k = 1.0 - s.t / static_cast<double>(s.p.duration_s);
        if (k <= 0.0) continue;
        double falloff = 1.0;
        if (s.p.radius > 0.0f) {
            const tactical::Vec3 r = target - s.source;
            falloff = std::max(0.0, 1.0 - static_cast<double>(std::sqrt(tactical::dot(r, r))) / static_cast<double>(s.p.radius));
        }
        const double a = static_cast<double>(s.p.magnitude) * static_cast<double>(distance) * k * k * falloff;
        const double w = kTwoPi * static_cast<double>(s.p.frequency_hz) * s.t;
        // Three incommensurate sines: a jitter that does not trace a visible loop.
        sum.x += static_cast<float>(a * std::sin(w));
        sum.y += static_cast<float>(a * std::sin(1.37 * w + 1.1));
        sum.z += static_cast<float>(0.6 * a * std::sin(0.83 * w + 2.3));
    }
    return sum;
}

}  // namespace alamo::game
