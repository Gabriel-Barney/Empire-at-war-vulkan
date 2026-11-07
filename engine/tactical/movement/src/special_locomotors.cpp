#include "alamo/tactical/movement/special_locomotors.hpp"

#include <algorithm>
#include <cmath>

#include "alamo/objects/game_object_types.hpp"
#include "alamo/tactical/movement/fighter.hpp"

namespace alamo::tactical::movement {

namespace {
bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 'a' + 'A');
        if (y >= 'a' && y <= 'z') y = static_cast<char>(y - 'a' + 'A');
        if (x != y) return false;
    }
    return true;
}
bool is_locomotor_token(std::string_view t) {
    // Any *_LOCOMOTOR token occupies the locomotor slot (the Default ones included).
    constexpr std::string_view suffix = "LOCOMOTOR";
    return t.size() >= suffix.size() && ieq(t.substr(t.size() - suffix.size()), suffix);
}
}  // namespace

LocomotorKind locomotor_kind_from_token(std::string_view t) {
    if (ieq(t, "BIKE_LOCOMOTOR")) return LocomotorKind::Bike;
    if (ieq(t, "JETPACK_LOCOMOTOR")) return LocomotorKind::Jetpack;
    if (ieq(t, "FLYING_LOCOMOTOR")) return LocomotorKind::Flying;
    if (ieq(t, "LANDBOMBER_LOCOMOTOR")) return LocomotorKind::LandBomber;
    if (ieq(t, "FIGHTER_LOCOMOTOR")) return LocomotorKind::Fighter;
    return LocomotorKind::Default;
}

LocomotorKind locomotor_kind_from_type(const objects::GameObjectTypes& types, uint32_t ti, MoveMode mode) {
    for (const std::string_view tag : {mode == MoveMode::Land ? std::string_view("LandBehavior") : std::string_view("SpaceBehavior"),
                                       std::string_view("Behavior")}) {
        for (std::string_view t : types.get_list(ti, tag))
            if (is_locomotor_token(t)) return locomotor_kind_from_token(t);
    }
    return LocomotorKind::Default;
}

TurnAnim jetpack_turn_anim(float err, float r, bool pre) {
    TurnAnim a;
    const float ae = std::fabs(err);
    const bool left = err > 0.0f;
    if (pre) {
        if (ae < r) return a;
        if (ae < 22.5f) {
            a.anim = left ? kAnimTurnLeftQuarter : kAnimTurnRightQuarter;
            a.rate = r * 0.25f * ae / 11.25f;
        } else if (ae < 45.0f) {
            a.anim = left ? kAnimTurnLeftHalf : kAnimTurnRightHalf;
            a.rate = r * 0.5f;
        } else {
            a.anim = left ? kAnimTurnLeft : kAnimTurnRight;
            a.rate = r;
        }
        return a;
    }
    a.anim = left ? kAnimTurnLeft : kAnimTurnRight;
    a.rate = r > 0.0f ? (ae / r) * 0.03333f : 0.0f;
    return a;
}

// ---- s78l t02 (MOVEMENT §3.5.1 "Arrival turn, details")
bool anim_idle_class(int32_t a) { return a == 0 || a == 1 || a == 71 || (a >= 77 && a <= 82); }
bool anim_turn_clip(int32_t a) { return a == kAnimTurnLeft || a == kAnimTurnRight || (a >= kAnimTurnLeftHalf && a <= kAnimTurnRightQuarter); }

float arrival_turn_threshold(float r, bool collision_path) { return collision_path && r <= 10.0f ? r : 10.0f; }

TurnAnim arrival_turn_band(float e, float s, float t, int32_t current) {
    TurnAnim a;
    a.anim = current;
    a.rate = s;
    const float ae = std::fabs(e);
    if (!(e >= t) && !(e <= -t)) return a;  // |e| < T: the clip is unchanged
    const bool left = e > 0.0f;
    if (ae < 22.5f) {
        a.anim = left ? kAnimTurnLeftQuarter : kAnimTurnRightQuarter;
        a.rate = s * 0.25f * ae / 11.25f;
    } else if (ae < 45.0f) {
        a.anim = left ? kAnimTurnLeftHalf : kAnimTurnRightHalf;
        a.rate = s * 0.5f;
    } else {
        a.anim = left ? kAnimTurnLeft : kAnimTurnRight;
    }
    return a;
}

int32_t arrival_turn_end_clip(int32_t a) {
    if (a == kAnimTurnLeft || a == kAnimTurnLeftBegin || a == kAnimTurnLeftEnd || a == kAnimTurnLeftHalf || a == kAnimTurnLeftQuarter)
        return kAnimTurnLeftEnd;
    if (a == kAnimTurnRight || a == kAnimTurnRightBegin || a == kAnimTurnRightEnd || a == kAnimTurnRightHalf || a == kAnimTurnRightQuarter)
        return kAnimTurnRightEnd;
    return kAnimNone;
}

float jetpack_altitude_target(const TrackFn& track, const TerrainFn& terrain, const InsideFn& inside, float hover,
                              float floor_z, float z) {
    const float base = hover;  // slot z offset 0 [port]
    bool any = false;
    float target = 0.0f;
    for (uint32_t i = 0; i < 60; ++i) {
        const V2 p = track(i);
        if (inside && !inside(p)) continue;
        float h = std::max(terrain(p) + hover, floor_z);
        if (h > base) h = (h - base) * (1.0f - static_cast<float>(i) / 60.0f) + base;
        target = any ? std::max(target, h) : h;
        any = true;
    }
    return any ? target : z;
}

float step_altitude(float z, float target, float max_lift, float terrain_here, float floor_z) {
    z += std::clamp(target - z, -max_lift, max_lift);
    return std::max(std::max(z, terrain_here), floor_z);
}

float bike_lean(float lean, int32_t turn_anim, float rate, float bank) {
    if (turn_anim == kAnimTurnLeft) lean -= rate;
    else if (turn_anim == kAnimTurnRight) lean += rate;
    else if (lean > 0.0f) lean = std::max(0.0f, lean - rate);
    else if (lean < 0.0f) lean = std::min(0.0f, lean + rate);
    return std::clamp(lean, -bank, bank);
}

// ---------------------------------------------------------------- FLYING_LOCOMOTOR (FIGHTER_FLYING.md §6)
// The §1.1 / §1.2 primitives (k, the tiny-angle sin / cos, s(x), wrap, angles) are the fighter ones (fighter.hpp).
namespace {
constexpr float kFlyDegToRad = kFighterDegToRad;  // k (§1.1)
float fly_sin(float x) { return fighter_sin(x); }
float fly_cos(float x) { return fighter_cos(x); }
// 0x139800: len = sqrtf((x x + y y) + z z) [M sum order], inv = 1 / len, multiply when len > 0.
FlyVec3 fly_normalise(FlyVec3 v) {
    const float len = std::sqrt((v.x * v.x + v.y * v.y) + v.z * v.z);
    if (!(len > 0.0f)) return v;
    const float inv = 1.0f / len;
    return {v.x * inv, v.y * inv, v.z * inv};
}
// [port] waypoint facing vector from the planar heading (retail stores the vector, waypoint +0x0c).
FlyVec3 fly_facing(float heading_deg) {
    const float a = heading_deg * kFlyDegToRad;
    return {fly_cos(a), fly_sin(a), 0.0f};
}
// Forward x s from (pitch, yaw) as §3.4 / §6.1: ((cy cp) s, (sy cp) s, sp s), cp = cos(-Pi k), sp = sin(-Pi k).
FlyVec3 fly_forward(FlyOrient o, float s) {
    const float a = (-o.pitch) * kFlyDegToRad, y = o.yaw * kFlyDegToRad;
    const float cp = fly_cos(a), sp = fly_sin(a), sy = fly_sin(y), cy = fly_cos(y);
    return {(cy * cp) * s, (sy * cp) * s, sp * s};
}
// Step `v` toward `v + d` by at most `budget` (consumed), as §3.3: budget < |d| -> +-budget, budget = 0.
float fly_budget_step(float v, float d, float& budget) {
    if (budget < std::fabs(d)) {
        v = d > 0.0f ? v + budget : v - budget;
        budget = 0.0f;
    } else {
        v += d;
        budget -= std::fabs(d);
    }
    return v;
}
}  // namespace

float fly_wrap180(float a) { return fighter_wrap180(a); }

float fly_wrap360(float a) { return fighter_wrap360(a); }

FlyOrient fly_angles(FlyVec3 a, FlyVec3 b) {  // 0x20acd0
    const FAngles r = fighter_vector_angles(F3{a.x, a.y, a.z}, F3{b.x, b.y, b.z});
    FlyOrient o;
    o.pitch = r.pitch;
    o.yaw = r.yaw;
    return o;
}

FlyFollow flying_follow(std::span<const SpaceWaypoint> wps, uint32_t& idx, float T, const FlyBody& b, const FlyRates& r) {
    FlyFollow out;
    const size_t n = wps.size();
    auto extrapolate = [&]() {  // step 2: P1 = P + forward x s0, O1 = O, s1 = s0
        const FlyVec3 f = fly_forward(b.o, b.speed);
        out.pos = {f.x + b.pos.x, f.y + b.pos.y, f.z + b.pos.z};
        out.o = b.o;
        out.speed = b.speed;
        out.complete = static_cast<size_t>(idx) + 1 >= n;
        return out;
    };
    if (static_cast<size_t>(idx) + 1 >= n) return extrapolate();
    // Step 3: STRICT bracketing (SimpleSpace 0x625990 advances while t <= T).
    while (wps[idx + 1].time < T) {
        ++idx;  // 0x5c4920
        if (static_cast<size_t>(idx) + 1 >= n) return extrapolate();
    }
    const SpaceWaypoint& w0 = wps[idx];
    const SpaceWaypoint& w1 = wps[idx + 1];
    const float dt = w1.time - w0.time;
    if (dt <= 0.0f) {  // step 4
        ++idx;
        return extrapolate();
    }
    const float u = std::clamp((T - w0.time) / dt, 0.0f, 1.0f);
    const FlyVec3 f0 = fly_facing(w0.heading), f1 = fly_facing(w1.heading);
    out.complete = false;
    if (w0.pos == w1.pos) {  // step 6: zero-length segment, yaw interpolated (0x20b100 / 0x20b070 [M wraps])
        out.pos = b.pos;
        out.speed = 0.0f;
        out.o = b.o;  // [M] roll and pitch unchanged
        const float y0 = fly_angles({}, f0).yaw, y1 = fly_angles({}, f1).yaw;
        out.o.yaw = fly_wrap360(y0 + fly_wrap180(y1 - y0) * u);
        return out;
    }
    // Step 7: Hermite with the inverted basis of LocomotorCommon (exact in float32).
    static constexpr float kB[4][4] = {{1.0f, 0.0f, 0.0f, 0.0f},
                                       {0.0f, 0.0f, 1.0f, 0.0f},
                                       {-3.0f, 3.0f, -2.0f, -1.0f},
                                       {2.0f, -2.0f, 1.0f, 1.0f}};
    const FlyVec3 n0 = fly_normalise(f0), n1 = fly_normalise(f1);
    const float p0[3] = {w0.pos.x, w0.pos.y, 0.0f}, p1[3] = {w1.pos.x, w1.pos.y, 0.0f};  // planar path: z = 0 [port]
    const float m0[3] = {(w0.speed * n0.x) * dt, (w0.speed * n0.y) * dt, (w0.speed * n0.z) * dt};
    const float m1[3] = {(w1.speed * n1.x) * dt, (w1.speed * n1.y) * dt, (w1.speed * n1.z) * dt};
    const float u2 = u * u, u3 = u * u2;
    float P[3], D[3];
    for (int ax = 0; ax < 3; ++ax) {
        float a[4];
        for (int row = 0; row < 4; ++row)
            a[row] = ((p0[ax] * kB[row][0] + p1[ax] * kB[row][1]) + m0[ax] * kB[row][2]) + m1[ax] * kB[row][3];
        P[ax] = ((a[1] * u + a[0]) + a[2] * u2) + a[3] * u3;
        D[ax] = (((a[2] * 2.0f) * u + a[1]) + (a[3] * 3.0f) * u2) / dt;
    }
    const float len = std::sqrt((D[1] * D[1] + D[0] * D[0]) + D[2] * D[2]);
    const float yaw1 = len <= 0.0001f ? b.o.yaw : fly_angles({}, {D[0], D[1], D[2]}).yaw;
    // Bank from the yaw change (the decay branch also runs when O1 = O [M]).
    const float dy = fly_wrap180(yaw1 - b.o.yaw);
    const float g = r.turn > 0.0f ? std::clamp(std::fabs(dy / r.turn), 0.0f, 1.0f) : 1.0f;  // [port] turn 0 guard
    float R = fly_wrap180(b.o.roll);
    const float rr = r.roll;
    if (std::fabs(dy) <= 0.01f) {
        if (R > 0.0f) R = std::max(R - rr, 0.0f);
        else if (R < 0.0f) R = std::min(R + rr, 0.0f);
    } else {
        R = dy > 0.0f ? R - g * rr : R + g * rr;
    }
    out.o = {std::clamp(R, -r.bank, r.bank), b.o.pitch, yaw1};
    out.pos = {P[0], P[1], b.pos.z};  // z belongs to §6.3
    out.speed = len;
    return out;
}

bool flying_track_linear(std::span<const SpaceWaypoint> wps, float t, float z, FlyVec3& pos, V2& facing) {
    if (wps.empty()) return false;
    if (wps.size() == 1 || t <= wps.front().time || t >= wps.back().time) {
        const SpaceWaypoint& w = (wps.size() == 1 || t <= wps.front().time) ? wps.front() : wps.back();
        pos = {w.pos.x, w.pos.y, z};
        facing = dir_from_deg(w.heading);
        return true;
    }
    size_t k = 0;
    while (k + 2 < wps.size() && wps[k + 1].time <= t) ++k;
    const SpaceWaypoint& a = wps[k];
    const SpaceWaypoint& b = wps[k + 1];
    const float dt = b.time - a.time;
    const float s = dt > 0.0f ? (t - a.time) / dt : 1.0f;
    const V2 p = a.pos + (b.pos - a.pos) * s;
    pos = {p.x, p.y, z};
    const FlyVec3 d = fly_normalise({b.pos.x - a.pos.x, b.pos.y - a.pos.y, 0.0f});
    facing = (d.x == 0.0f && d.y == 0.0f) ? dir_from_deg(a.heading) : V2{d.x, d.y};
    return true;
}

FlyOrient flying_attitude_step(FlyOrient cur, FlyOrient want, float& by, float& bp, bool gate, float rr, float bank) {
    const float dyaw = want.yaw - cur.yaw, dpitch = want.pitch - cur.pitch;  // d = O_d - O_c (0x20b100)
    const float dy = fly_wrap180(dyaw);
    float Y = fly_wrap180(cur.yaw);
    float R = fly_wrap180(cur.roll);
    // Yaw: with the gate only once the roll is on the turning side (a left turn needs R < 0).
    if (!gate || fighter_sign(R) == -fighter_sign(dy)) Y = fly_budget_step(Y, dy, by);
    // Roll (always): target -5 x dy (the yaw error of this frame, before the yaw step [M]).
    const float e = (-dy * 5.0f) - R;
    if (std::fabs(e) >= rr) R = e > 0.0f ? R + rr : R - rr;
    else R += e;
    R = std::clamp(R, -bank, bank);
    // Pitch: as §3.3.
    const float dp = fly_wrap180(dpitch);
    const float p = fly_budget_step(fly_wrap180(cur.pitch), dp, bp);
    return {R, fly_wrap180(p), fly_wrap180(Y)};
}

void flying_altitude_step(FlyVec3& P, FlyOrient& O, float s, bool use_track, const FlyVec3* dest, const FlyRates& r,
                          const FlyWorld& w) {
    // 1. Limits: maxPitch 30; M = the destination height; a dive is limited to the destination's depression angle.
    float max_pitch = 30.0f, M = 0.0f;
    if (dest) {
        FlyVec3 Q = *dest;  // 0x583150 -> 0x583680: clamped to the playable box, z to +-5000 [M]
        if (w.has_box) {
            Q.x = std::clamp(Q.x, w.box_min.x, w.box_max.x);
            Q.y = std::clamp(Q.y, w.box_min.y, w.box_max.y);
        }
        Q.z = std::clamp(Q.z, -5000.0f, 5000.0f);
        const float q = fly_wrap180(fly_angles(P, Q).pitch);
        M = Q.z;
        if (q > 0.0f) max_pitch = std::min(q, 30.0f);
    }
    // 2. The port always has the tracking system (land mode). 3. Terrain look-ahead.
    const float yk = O.yaw * kFlyDegToRad;
    V2 f{fly_cos(yk), fly_sin(yk)};  // 0x41c000
    float x = P.x, y = P.y;
    const float z = P.z;
    FlyVec3 last{x, y, z};
    for (uint32_t i = 0; i < 60; ++i) {
        FlyVec3 p;
        bool sampled = false;
        if (use_track && w.track) {
            FlyVec3 tp;
            V2 tf;
            if (w.track(i, tp, tf)) {
                sampled = true;
                if (tp.x == x && tp.y == y && tp.z == z) {  // track exhausted: stay, extrapolate from now on
                    use_track = false;
                    p = {x, y, z};
                } else {
                    p = tp;
                    f = tf;
                }
            }
        }
        if (!sampled) p = {x + f.x * s, y + f.y * s, z};
        x = p.x;
        y = p.y;
        last = p;
        if (w.inside({p.x, p.y})) {
            float h = std::max(w.height({p.x, p.y}) + r.hover_offset, w.floor);
            if (h > M) h = (h - M) * (1.0f - static_cast<float>(i) / 60.0f) + M;
            M = std::max(M, h);
        }
    }
    // 4. Clearance (0x63b640 [M]): host hook.
    if (w.clearance) M = w.clearance(P, last, M);
    // 5. Pitch target: +L (dive) at dz <= -100 .. -L (climb) at dz >= +100; climbs always limited to 30.
    const float dz = M - P.z;
    const float c = std::clamp(dz, -100.0f, 100.0f);
    const float L = (c <= 0.0f || dz < -100.0f) ? max_pitch : 30.0f;
    const float t = std::clamp(c / 200.0f + 0.5f, 0.0f, 1.0f);
    const float pt = (-L - L) * t + L;
    // 6. Pitch step by at most lift(T) ([M]: the §3.3 form on the unwrapped difference).
    float lb = r.lift;
    const float p = fly_budget_step(fly_wrap180(O.pitch), pt - fly_wrap180(O.pitch), lb);
    O.pitch = p;
    // 7. Height: z += (-s) sin(p k) / cos(p k).
    const float pk = p * kFlyDegToRad;
    P.z = ((-s) * fly_sin(pk)) / fly_cos(pk) + P.z;
    // 8. Clamp to the bare terrain and the floor.
    P.z = std::max(P.z, w.height({P.x, P.y}));
    P.z = std::max(P.z, w.floor);
}

void flying_loiter_move(FlyBody& b, const FlyVec3* dest, const FlyRates& r, const FlyWorld& w) {
    const float yk = b.o.yaw * kFlyDegToRad;  // the yaw before this frame's attitude change
    FlyVec3 P1{b.speed * fly_cos(yk) + b.pos.x, b.speed * fly_sin(yk) + b.pos.y, b.pos.z + 0.0f};
    float tb = r.turn, lb = r.lift;
    FlyOrient O = flying_attitude_step(b.o, {0.0f, b.o.pitch, b.o.yaw}, tb, lb, true, r.roll, r.bank);
    flying_altitude_step(P1, O, b.speed, false, dest, r, w);
    b.o = O;
    if (!(P1 == b.pos)) b.vel = P1 - b.pos;
    b.pos = P1;
}

V2 flying_loiter_reissue(V2 pos, V2 dest, float yaw_deg, float L, SimRng& rng, bool& turn_back) {
    const float dx = pos.x - dest.x, dy = pos.y - dest.y;
    V2 dir;
    if (std::sqrt(dx * dx + dy * dy) >= 0.3f * L) {
        const FlyVec3 n = fly_normalise({dest.x - pos.x, dest.y - pos.y, 0.0f});
        const float a = rng.range_float(-90.0f, 90.0f) * kFlyDegToRad;  // the only synced draw (0x1ffbb0)
        const float ca = fly_cos(a), sa = fly_sin(a);
        dir = {n.x * ca - n.y * sa, n.x * sa + n.y * ca};
    } else {
        const float yk = yaw_deg * kFlyDegToRad;
        dir = {-fly_cos(yk), -fly_sin(yk)};
        turn_back = true;
    }
    return {dest.x + dir.x * L, dest.y + dir.y * L};  // the DESTINATION + dir x L (§9.2)
}

V2 flying_strafe_aim(V2 pos, V2 aim, float yaw_deg, float radius, float S, bool& turn_back) {
    const float dx = aim.x - pos.x, dy = aim.y - pos.y;  // [M] sqrtf of the 2-D squared distance
    if (std::sqrt(dx * dx + dy * dy) < radius + 0.3f * S) {
        const float yk = yaw_deg * kFlyDegToRad;
        aim = {aim.x + fly_cos(yk) * S, aim.y + fly_sin(yk) * S};  // (z += Prepare_Strafe_Height: planar goal)
        turn_back = true;
    }
    return aim;
}

float flying_circle_height(const FlyWorld& w, FlyVec3 P, FlyVec3 V, float target_z, float hover) {
    float H = w.height({P.x + V.x, P.y + V.y});
    H = std::max(H, target_z);
    for (int32_t k = 2; k <= 11; ++k) {
        const float fk = static_cast<float>(k);
        H = std::max(H, w.height({P.x + V.x * fk, P.y + V.y * fk}));
    }
    return H + hover;
}

float flying_circle_dz(float H, float z, float vmax, float psh, float hover) {
    float dz = std::clamp(H - z, -vmax, vmax);
    if (dz < 0.0f) {
        const float span = std::max(psh - hover, 1.0f);
        dz = (-(H - z) / span) * dz;  // no second clamp (§9.3)
    }
    return dz;
}

void flying_circle_step(FlyBody& b, const FlyVec3* C, float rmin, float rmax, const FlyRates& r, const FlyWorld& w) {
    const FlyVec3 P = b.pos;
    if (!C) {  // 1. no target: P += V
        b.pos = {P.x + b.vel.x, P.y + b.vel.y, P.z + b.vel.z};
        return;
    }
    // 2. Direction to the target.
    const float ux = C->x - P.x, uy = C->y - P.y;
    const float dist = std::sqrt(uy * uy + ux * ux);
    float hx = ux, hy = uy;
    if (dist > 0.0f) {
        const float inv = 1.0f / dist;
        hx = ux * inv;
        hy = uy * inv;
    }
    // 3. / 4. Height target and vertical step.
    const float vmax = r.max_speed;
    const float H = flying_circle_height(w, P, b.vel, C->z, r.hover_offset);
    const float dz = flying_circle_dz(H, P.z, vmax, r.prepare_strafe_height, r.hover_offset);
    // 5. Velocity direction (xy, [M] the sqrtf / reciprocal normaliser).
    const FlyVec3 vh = fly_normalise({b.vel.x, b.vel.y, 0.0f});
    // 6. Heading.
    FlyVec3 dir;
    if (dist < rmin) dir = {-hx, -hy, dz};
    else if (dist > rmax) dir = {hx, hy, dz};
    else if (vh.x * hy - vh.y * hx <= 0.0f) dir = {-hy, hx, dz};
    else dir = {hy, -hx, dz};
    const FlyOrient want = fly_angles({}, dir);
    // 7. Attitude without the gate.
    float tb = r.turn, lb = r.lift;
    const FlyOrient O = flying_attitude_step(b.o, want, tb, lb, false, r.roll, r.bank);
    // 8. V from the new pitch and yaw at vmax.
    FlyVec3 V = fly_forward(O, vmax);
    FlyVec3 Q{P.x + V.x, P.y + V.y, P.z + V.z};
    // 9. Up to 3 terrain-clear retries ([port] the 2-D box stands for the 3-D map box).
    for (int32_t i = 0; i < 3; ++i) {
        float g = w.inside({Q.x, Q.y}) ? w.height({Q.x, Q.y}) : 0.0f;
        g += r.hover_offset;
        if (g <= Q.z) break;
        const FlyVec3 nv = fly_normalise({Q.x - P.x, Q.y - P.y, g - P.z});
        V = {nv.x * vmax, nv.y * vmax, nv.z * vmax};
        Q = {P.x + V.x, P.y + V.y, P.z + V.z};
    }
    // 10. Orientation (attitude roll, angles(P -> Q)), position Q, V.
    const FlyOrient a = fly_angles(P, Q);
    b.o = {O.roll, a.pitch, a.yaw};
    b.pos = Q;
    b.vel = V;
}

// ---------------------------------------------------------------- §6.3.1 clearance query (0x63b640)
bool locomotor_airborne_kind(LocomotorKind kind, LocoState state) {
    switch (kind) {
        case LocomotorKind::Flying:
        case LocomotorKind::LandBomber: return true;
        case LocomotorKind::Jetpack:
            return state == LocoState::None ||
                   (static_cast<uint8_t>(state) >= static_cast<uint8_t>(LocoState::WalkStartTurn) &&
                    static_cast<uint8_t>(state) <= static_cast<uint8_t>(LocoState::WalkMoving));
        default: return false;
    }
}

uint32_t object_tracking_layer(bool uses_multiple_locomotors, bool airborne_kind, bool moving, uint32_t space_layer,
                               uint32_t stationary_space_layer) {
    if (uses_multiple_locomotors && airborne_kind) return kLayerCorvette;
    if (moving || stationary_space_layer == 0) return space_layer;
    return stationary_space_layer;
}

V2 clearance_half_extents(float custom_x, float custom_y, float scale, bool has_model, float max_x, float max_y) {
    const float fx = has_model ? std::fabs(max_x) : 0.0f, fy = has_model ? std::fabs(max_y) : 0.0f;
    return {(custom_x > 0.0f ? custom_x : fx) * scale, (custom_y > 0.0f ? custom_y : fy) * scale};
}

bool clearance_candidate(V2 from, V2 to, float qx, float qy, const ClearanceEntry& e) {
    const float g = std::max(qx, qy), h = std::max(e.ex, e.ey);
    const float qx0 = std::min(from.x, to.x) - g, qx1 = std::max(from.x, to.x) + g;
    const float qy0 = std::min(from.y, to.y) - g, qy1 = std::max(from.y, to.y) + g;
    const float ex0 = std::min(e.s.x, e.e.x) - h, ex1 = std::max(e.s.x, e.e.x) + h;
    const float ey0 = std::min(e.s.y, e.e.y) - h, ey1 = std::max(e.s.y, e.e.y) + h;
    return ex0 < qx1 && qx0 < ex1 && ey0 < qy1 && qy0 < ey1;
}

namespace {
struct D2 {
    double x = 0.0, y = 0.0;
};
// normalise with sqrtf; a zero vector stays zero.
D2 clr_unit(V2 v) {
    const float l = std::sqrt(v.x * v.x + v.y * v.y);
    if (!(l > 0.0f)) return {};
    return {static_cast<double>(v.x / l), static_cast<double>(v.y / l)};
}
double clr_lerp(double a, double b, double s) { return (1.0 - s) * a + s * b; }
}  // namespace

bool clearance_hit(V2 from, V2 to, float qx, float qy, const ClearanceEntry& e, float t) {
    // qdir = (to - from).xy / sqrtf(dx^2 + dy^2), divided in double; (0, 0) at length 0.
    const float qdx = to.x - from.x, qdy = to.y - from.y;
    const float ql = std::sqrt(qdx * qdx + qdy * qdy);
    D2 qdir;
    if (ql > 0.0f) qdir = {static_cast<double>(qdx) / static_cast<double>(ql), static_cast<double>(qdy) / static_cast<double>(ql)};
    // A = S + (E - S) a, a = clamp((t - slice start) / (slice end - slice start), 0, 1).
    const double span = static_cast<double>(e.t1) - static_cast<double>(e.t0);
    const double a = span > 0.0 ? std::clamp((static_cast<double>(t) - static_cast<double>(e.t0)) / span, 0.0, 1.0) : 0.0;
    const D2 A{static_cast<double>(e.s.x) + (static_cast<double>(e.e.x) - static_cast<double>(e.s.x)) * a,
               static_cast<double>(e.s.y) + (static_cast<double>(e.e.y) - static_cast<double>(e.s.y)) * a};
    const D2 u0 = clr_unit(e.s - from), u1 = clr_unit(e.e - to);
    const double fx = static_cast<double>(e.facing.x), fy = static_cast<double>(e.facing.y);
    auto R = [&](D2 u) {
        const double cf = u.x * fx + u.y * fy, cq = u.x * qdir.x + u.y * qdir.y;
        return clr_lerp(static_cast<double>(e.ey), static_cast<double>(e.ex), cf * cf) +
               clr_lerp(static_cast<double>(qy), static_cast<double>(qx), cq * cq);
    };
    const double R0 = R(u0), dR = R(u1) - R0;
    const D2 d{static_cast<double>(from.x) - A.x, static_cast<double>(from.y) - A.y};
    const D2 D{static_cast<double>(to.x) - static_cast<double>(from.x), static_cast<double>(to.y) - static_cast<double>(from.y)};
    const double qa = (D.x * D.x + D.y * D.y) - dR * dR;
    const double qb = 2.0 * (d.x * D.x + d.y * D.y) - 2.0 * R0 * dR;
    const double qc = (d.x * d.x + d.y * d.y) - R0 * R0;
    if (qa == 0.0) {
        if (qb == 0.0) return qc <= 0.0;
        const double r = -qc / qb;
        return !((qb < 0.0 && r > 1.0) || (qb > 0.0 && r < 0.0));
    }
    const double disc = qb * qb - 4.0 * qa * qc;
    if (disc <= 0.0) return qa < 0.0;
    const double sd = std::sqrt(disc);
    const double r1 = (sd - qb) / (2.0 * qa), r2 = (-qb - sd) / (2.0 * qa);
    if (qa > 0.0) return !(r1 < 0.0 || r2 > 1.0);
    return !(r1 <= 0.0 && r2 >= 1.0);
}

}  // namespace alamo::tactical::movement
