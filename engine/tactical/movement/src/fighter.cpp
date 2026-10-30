#include "alamo/tactical/movement/fighter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "alamo/core/dmath.hpp"
#include "alamo/objects/game_object_types.hpp"

namespace alamo::tactical::movement {

namespace {
float sgn1(float x) { return x >= 0.0f ? 1.0f : -1.0f; }  // §3.2 s(x)
bool tiny(float x) { return (std::bit_cast<uint32_t>(x) & 0x7F800000u) < 0x1D000001u; }
// float -> int truncation toward zero; [port] guard against out-of-range / NaN (UB in C++).
int32_t trunc_i(float f) {
    if (!(f > -2.0e9f)) return f != f ? 0 : -2000000000;
    if (!(f < 2.0e9f)) return 2000000000;
    return static_cast<int32_t>(f);
}
}  // namespace

// ---------------------------------------------------------------- §1 helpers
float fighter_wrap180(float a) {
    if (!std::isfinite(a)) return a;
    if (std::fabs(a) > 1.0e6f) a = std::fmod(a, 360.0f);  // [port] guard
    while (a > 180.0f) a -= 360.0f;
    while (a <= -180.0f) a += 360.0f;
    return a;
}

float fighter_wrap360(float a) {
    if (!std::isfinite(a)) return a;
    if (std::fabs(a) > 1.0e6f) a = std::fmod(a, 360.0f);  // [port] guard
    while (a >= 360.0f) a -= 360.0f;
    while (a < 0.0f) a += 360.0f;
    return a;
}

float fighter_sin(float x) { return tiny(x) ? x : dmath::sin(x); }
float fighter_cos(float x) { return tiny(x) ? 1.0f : dmath::cos(x); }

FAngles fighter_vector_angles(F3 a, F3 b) {
    FAngles r;
    if (!(a.x == b.x && a.y == b.y)) {
        r.yaw = dmath::atan2(b.y - a.y, b.x - a.x) * kFighterRadToDeg;
        if (r.yaw < 0.0f) r.yaw += 360.0f;
    }
    if (a.z != b.z || a.x != b.x) {
        const float dy = b.y - a.y, dx = b.x - a.x;
        r.pitch = -(dmath::atan2(b.z - a.z, std::sqrt(dy * dy + dx * dx)) * kFighterRadToDeg);
    }
    return r;
}

F3 fighter_fast_normalise(F3 v) {
    const float s = (v.x * v.x + v.y * v.y) + v.z * v.z;
    if (!(s > 0.0f)) return v;
    const float r = dmath::fast_rsqrt_retail(s);  // h = s x 0.5; y = (1.5 - (h y) y) y, twice (§1.3)
    return {v.x * r, v.y * r, v.z * r};
}

// ---------------------------------------------------------------- §3 primitives
RelAngles fighter_body_angles(F3 d, FAttitude o) {
    // (dis) a = -(k Pi), b = -(k Y); rows r0 = (ca cb, -(ca sb), sa), r1 = (sb, cb, 0), r2 = (-(sa cb), sa sb, ca);
    // l_i = (((r_i.x d.x + r_i.y d.y) + r_i.z d.z) + 0).
    const float a = -(kFighterDegToRad * o.pitch);
    const float bb = -(kFighterDegToRad * o.yaw);
    const float sa = fighter_sin(a), ca = fighter_cos(a), sb = fighter_sin(bb), cb = fighter_cos(bb);
    const F3 r0{ca * cb, -(ca * sb), sa};
    const F3 r1{sb, cb, 0.0f};
    const F3 r2{-(sa * cb), sa * sb, ca};
    const F3 l{((r0.x * d.x + r0.y * d.y) + r0.z * d.z) + 0.0f, ((r1.x * d.x + r1.y * d.y) + r1.z * d.z) + 0.0f,
               ((r2.x * d.x + r2.y * d.y) + r2.z * d.z) + 0.0f};
    const FAngles ang = fighter_vector_angles(F3{}, l);
    return {fighter_wrap180(ang.yaw), fighter_wrap180(ang.pitch)};
}

void fighter_yaw_bank_step(FAttitude& o, const FighterKin& k, float dy, float& budget) {
    dy = fighter_wrap180(dy);
    const float bank = std::clamp(dy, -k.bank, k.bank);
    float r = fighter_wrap180(o.roll);
    float y = o.yaw;
    const float e = fighter_wrap180(-bank - r);  // the target roll is -bank
    float rb = k.roll * (budget / k.turn);       // (dis)
    if (rb < 0.0f) rb = 0.0f;
    if (rb > 180.0f) rb = 180.0f;
    if (rb < std::fabs(e)) {
        if (e > 0.0f) r += rb;
        else if (e < 0.0f) r -= rb;
    } else {
        r += e;
    }
    if (r != 0.0f && sgn1(-bank) != sgn1(r)) {
        budget = 0.0f;  // still banked the wrong way: no yaw
    } else if (budget < std::fabs(dy)) {
        if (dy <= 0.0f) y -= budget;
        else y += budget;
        budget = 0.0f;
    } else {
        y += dy;
        budget -= std::fabs(dy);
    }
    o.roll = fighter_wrap360(r);
    o.yaw = fighter_wrap360(y);
}

void fighter_pitch_step(FAttitude& o, float dp, float& budget) {
    float p = fighter_wrap180(o.pitch);
    if (budget < std::fabs(dp)) {
        if (dp > 0.0f) p += budget;
        else if (dp < 0.0f) p -= budget;
        budget = 0.0f;
    } else {
        p += dp;
        budget -= std::fabs(dp);
    }
    o.pitch = fighter_wrap360(p);
}

void fighter_speed_step(FighterBody& b, float v, float& budget) {
    float s = f3_length(b.v);
    const float e = v - s;
    if (budget <= std::fabs(e)) {  // note <=
        if (e >= 0.0f) s += budget;
        else s -= budget;
        budget = 0.0f;
    } else {
        s += e;
        budget -= std::fabs(e);
    }
    const float cp = fighter_cos((-b.o.pitch) * kFighterDegToRad), sp = fighter_sin((-b.o.pitch) * kFighterDegToRad);
    const float cy = fighter_cos(b.o.yaw * kFighterDegToRad), sy = fighter_sin(b.o.yaw * kFighterDegToRad);
    b.v = {(cy * cp) * s, (sy * cp) * s, sp * s};
}

void fighter_steer_to(FighterBody& b, const FighterKin& k, F3 aim, float v, float& tb, float& thb, float& lb, bool idle) {
    if (thb <= 0.0f || tb <= 0.0f) return;
    const RelAngles rel = fighter_body_angles(f3_sub(aim, b.p), b.o);
    float ry = rel.yaw, rp = rel.pitch;
    // §3.1 step 4: the loop (Immelmann) flag.
    bool loop = false;
    if (std::fabs(ry) < 90.0f) b.loop = false;
    else if (b.loop) loop = true;
    if (!loop && !idle && std::fabs(ry) > 170.0f && k.lift > 0.0f) loop = true;
    if (loop) {
        rp = rp > 0.0f ? 180.0f : -180.0f;
        ry = 0.0f;
        b.loop = true;
    }
    // §3.1 step 5: roll anticipation.
    const float r = fighter_wrap180(b.o.roll);
    if (180.0f < std::fabs(k.turn * (r / k.roll) + ry)) ry -= sgn1(ry) * 360.0f;
    fighter_yaw_bank_step(b.o, k, ry, tb);
    fighter_pitch_step(b.o, rp, lb);
    fighter_speed_step(b, v, thb);
}

bool fighter_cone(F3 a, F3 from, FAttitude o, float yaw_limit, float pitch_limit, float r2, RelAngles& rel) {
    const F3 d = f3_sub(a, from);
    if ((d.x * d.x + d.y * d.y) + d.z * d.z > r2) return false;
    rel = fighter_body_angles(d, o);
    return std::fabs(rel.yaw) <= yaw_limit && std::fabs(rel.pitch) <= pitch_limit;
}

FAttitude fighter_fold_pitch360(FAttitude o, float lift) {
    const float p = fighter_wrap180(o.pitch);
    if (std::fabs(p) >= 90.0f) {
        o.roll = fighter_wrap360(o.roll + 180.0f);
        o.pitch = fighter_wrap360(((180.0f - std::fabs(p)) - lift) * (p >= 0.0f ? 1.0f : -1.0f));
        o.yaw = fighter_wrap360(o.yaw + 180.0f);
    }
    return o;
}

float fighter_turn_radius(float turn, float la) { return ((360.0f / turn) * la) / kFighterTwoPi; }

F3 fighter_lookahead(std::span<const SpaceWaypoint> path, uint32_t idx, F3 base, F3 dir, bool solo, float la, float turn) {
    F3 d;
    if (path.empty()) {
        d = fighter_fast_normalise(dir);
        d = f3_scale(d, solo ? std::max(la * 1.1f, 5.0f) : fighter_turn_radius(turn, la));
        return f3_add(base, d);
    }
    const size_t n = path.size();
    d = dir;
    if (static_cast<size_t>(idx) + 1 < n) {
        const V2 a = path[idx].pos, b = path[idx + 1].pos;
        d = {b.x - a.x, b.y - a.y, 0.0f};  // 0x5c5850
    }
    d = fighter_fast_normalise(d);
    if (solo && static_cast<size_t>(idx) + 2 == n) return {path[idx + 1].pos.x, path[idx + 1].pos.y, base.z};  // 0x5c54c0
    const float f = fighter_turn_radius(turn, la);
    const float o = 0.0f;  // pack +0x15c lateral offset [L] -> 0 [port]
    d = {d.x * f + (-d.y) * o, d.y * f + d.x * o, d.z * f + o * 0.0f};
    return f3_add(base, d);
}

float fighter_glide_z(float z_goal, F3 a, F3 p, float pitch, float lb, float la) {
    if (lb > 0.0f) {
        const float q = (std::fabs(fighter_wrap180(pitch)) / lb) * la;
        if (q > 0.0f) {
            const float dy = a.y - p.y, dx = a.x - p.x;
            const float h = std::sqrt(dy * dy + dx * dx);
            if (h < q) z_goal = (h * (z_goal - p.z)) / q + p.z;
        }
    }
    return z_goal;
}

float fighter_station_speed(const StationInput& in) {
    const float lo = std::min(in.lo, in.hi), hi = std::max(in.lo, in.hi);
    const float sl = f3_length(in.leader_v);
    F3 u{};
    if (sl > 0.0f) u = {in.leader_v.x / sl, in.leader_v.y / sl, in.leader_v.z / sl};
    // Along-track slot error (positive = ahead of the slot).
    const float ty = (((in.p.y - in.leader_p.y) + in.slot_leader.y) - in.slot_self.y) * u.y;
    const float tx = u.x * ((in.slot_leader.x + (in.p.x - in.leader_p.x)) - in.slot_self.x);
    const float tz = (((in.p.z - in.leader_p.z) + in.slot_leader.z) - in.slot_self.z) * u.z;
    const float e = (ty + tx) + tz;
    const float a = std::min(lo, in.speed), b = std::max(hi, in.speed);
    const float accel_dist = (b * b - in.v * in.v) / (in.thrust * 2.0f);
    const float decel_dist = (in.v * in.v - a * a) / (in.thrust * 2.0f);
    if (in.tolerance < std::fabs(in.error)) {
        if (e > in.tolerance) {
            if (decel_dist > 0.0f) {
                const float t = std::clamp(e / decel_dist, 0.0f, 1.0f);
                return (1.0f - t) * sl + t * lo;
            }
            return lo;
        }
        if (e < -in.tolerance) {
            if (accel_dist > 0.0f) {
                const float t = std::max(-e / accel_dist, 0.0f);  // no upper clamp
                return (1.0f - t) * sl + t * hi;
            }
            return hi;
        }
    }
    return in.v;
}

bool fighter_waypoint_passed(std::span<const SpaceWaypoint> path, uint32_t idx, F3 p) {
    const size_t n = path.size();
    if (static_cast<size_t>(idx) + 1 >= n) return false;
    const V2 w0 = path[idx].pos, wp = path[idx + 1].pos;
    const F3 in = fighter_fast_normalise({wp.x - w0.x, wp.y - w0.y, 0.0f});
    F3 w = in;
    if (static_cast<size_t>(idx) + 2 < n) {
        const V2 w2 = path[idx + 2].pos;
        const F3 out = fighter_fast_normalise({w2.x - wp.x, w2.y - wp.y, 0.0f});
        w = fighter_fast_normalise({in.x + out.x, in.y + out.y, 0.0f});
    }
    return w.x * (p.x - wp.x) + w.y * (p.y - wp.y) >= -0.001f;
}

float fighter_squad_idle(F3 p, F3 lp, F3& d, bool leader, float avoid_speed) {
    float ux = p.x - d.x, uy = p.y - d.y, uz = 0.0f;
    if (ux == 0.0f && uy == 0.0f) {
        ux = 1.0f;
        uy = 0.0f;
    }
    const float r = std::sqrt((ux * ux + uy * uy) + 0.0f);
    if (r > 0.0f) {
        const float inv = 1.0f / r;
        ux = ux * inv;
        uy = uy * inv;
        uz = inv * 0.0f;
    }
    const float ldx = lp.x - d.x, ldy = lp.y - d.y;
    const float rl = std::max(1.0f, std::sqrt(ldx * ldx + ldy * ldy));
    float v = avoid_speed;
    if (rl < 90.0f) {
        v = (r / rl) * 0.5f;
        if (leader) {  // orbit: 60 units out at an angle 10 / r ahead around D
            const float a = 10.0f / r;
            const float s = fighter_sin(a), c = fighter_cos(a);
            d.x += (((c * 0.0f - s) * uy + (s * 0.0f + c) * ux) + uz * 0.0f) * 60.0f;
            d.y += (((c - s * 0.0f) * uy + (c * 0.0f + s) * ux) + uz * 0.0f) * 60.0f;
        }
    }
    return v;
}

// ---------------------------------------------------------------- XML
FighterParams fighter_params_from_type(const objects::GameObjectTypes& t, uint32_t ti) {
    FighterParams p;
    const float thrust = t.get_float(ti, "Max_Thrust");
    if (thrust > 0.0f) p.max_thrust = thrust;
    const float mfd = t.get_float(ti, "Minimum_Follow_Distance");
    if (mfd > 0.0f) p.min_follow_distance = mfd;
    const float range = t.get_float(ti, "Targeting_Max_Attack_Distance");
    if (range > 0.0f) p.attack_range = range;
    return p;
}

float squadron_tolerance_from_type(const objects::GameObjectTypes& t, uint32_t ti) {
    const float tol = t.get_float(ti, "Squadron_Formation_Error_Tolerance");
    return tol > 0.0f ? tol : 25.0f;
}

// ---------------------------------------------------------------- §5 grids
void FighterCellGrid::build(F3 c, F3 h, float size, uint32_t capacity, uint32_t extra, bool by_position) {
    cx_ = c.x;
    cy_ = c.y;
    cz_ = c.z;
    hx_ = h.x;
    hy_ = h.y;
    hz_ = h.z;
    size_ = size;
    capacity_ = capacity;
    extra_ = extra;
    flag_ = by_position;
    nx_ = size > 0.0f ? std::max(0, trunc_i(((hx_ + cx_) - (cx_ - hx_)) / size)) : 0;
    ny_ = size > 0.0f ? std::max(0, trunc_i(((hy_ + cy_) - (cy_ - hy_)) / size)) : 0;
    if (static_cast<int64_t>(nx_) * ny_ > 4000000) nx_ = ny_ = 0;  // [port] guard against absurd boxes
    cells_.assign(static_cast<size_t>(nx_) * static_cast<size_t>(ny_), {});
}

void FighterCellGrid::clear() {
    for (auto& c : cells_) c.clear();
}

int32_t FighterCellGrid::index(FighterCell c) const {
    if (c.i < 0 || c.j < 0) return -1;
    const int64_t k = static_cast<int64_t>(c.j) * nx_ + c.i;
    if (k >= static_cast<int64_t>(nx_) * ny_) return -1;
    return static_cast<int32_t>(k);
}

F3 FighterCellGrid::centre(FighterCell c, float layer_z) const {
    const float half = size_ / 2.0f;
    float x = ((cx_ - hx_) + static_cast<float>(c.i) * size_) + half;
    if (c.j & 1) x += half;  // staggered rows
    const float y = ((cy_ - hy_) + static_cast<float>(c.j) * size_) + half;
    return {x, y, layer_z};
}

bool FighterCellGrid::join(ObjectId id, int32_t owner, FighterCell c, FighterCell& stored, const FighterAlliedFn& allied) {
    const int32_t k = index(c);
    if (k < 0) return false;
    std::vector<Entry>& cell = cells_[static_cast<size_t>(k)];
    if (!flag_) {
        if (static_cast<size_t>(extra_) + capacity_ <= cell.size()) return false;
        uint32_t allies = 0;
        for (const Entry& e : cell)
            if (allied(owner, e.owner)) ++allies;
        if (allies >= capacity_) return false;
    }
    for (const Entry& e : cell)
        if (e.id == id) return false;  // duplicates refused
    cell.push_back({id, owner});
    stored = c;
    return true;
}

void FighterCellGrid::leave(ObjectId id, FighterCell& stored) {
    const int32_t k = index(stored);
    if (k < 0) return;
    std::vector<Entry>& cell = cells_[static_cast<size_t>(k)];
    for (size_t n = 0; n < cell.size(); ++n)
        if (cell[n].id == id) {
            cell.erase(cell.begin() + static_cast<std::ptrdiff_t>(n));
            break;
        }
    stored = FighterCell{};
}

FighterCell FighterCellGrid::home(F3 ref) const {
    if (!built()) return {};
    const float half = size_ / 2.0f;
    const int32_t j = trunc_i((ref.y - (cy_ - hy_)) / size_);
    float x = (j & 1) ? ref.x - half : ref.x;
    x = std::max(x, cx_ - hx_);
    const int32_t i = trunc_i((x - (cx_ - hx_)) / size_);
    return {std::clamp(i, 0, nx_ - 1), std::clamp(j, 0, ny_ - 1)};
}

FighterCellGrid::Lookup FighterCellGrid::lookup(ObjectId id, int32_t owner, F3 ref, float layer_z, FighterCell& stored,
                                                bool reg, const FighterAlliedFn& allied) {
    Lookup out;
    if (!built()) return out;
    const FighterCell h = home(ref);
    if (index(stored) >= 0) {  // already assigned
        out.cell = stored;
        out.set_dest = !flag_;
        return out;
    }
    if (!flag_ && reg && join(id, owner, h, stored, allied)) {
        out.cell = h;
        out.set_dest = true;
        return out;
    }
    FighterCell best;
    float best_key = 3.0e38f;
    uint32_t evaluated = 0;
    const int32_t max_r = std::max(nx_, ny_) + 1;
    for (int32_t r = 1; r <= max_r && evaluated < 64; ++r) {
        for (int32_t jj = h.j - r; jj <= h.j + r && evaluated < 64; ++jj) {
            for (int32_t ii = h.i - r; ii <= h.i + r && evaluated < 64; ++ii) {
                if (r > 1 && std::max(std::abs(ii - h.i), std::abs(jj - h.j)) != r) continue;  // border only
                const FighterCell c{ii, jj};
                if (ii >= nx_ || jj >= ny_ || index(c) < 0) continue;
                ++evaluated;
                const std::vector<Entry>& cell = cells_[static_cast<size_t>(index(c))];
                bool candidate = flag_;
                if (!candidate) {
                    uint32_t allies = 0;
                    bool foreign = false;
                    for (const Entry& e : cell) {
                        if (allied(owner, e.owner)) ++allies;
                        else foreign = true;
                    }
                    candidate = allies < capacity_ && !foreign;
                }
                if (!candidate) continue;
                const F3 cc = centre(c, layer_z);
                const float dy = cc.y - ref.y, dx = cc.x - ref.x, dz = cc.z - ref.z;
                float key = (dy * dy + dx * dx) + dz * dz;
                if (flag_ && !cell.empty()) key = 0.0f;  // snap to an existing fight
                if (key < best_key) {
                    best_key = key;
                    best = c;
                }
            }
        }
        if (best.valid()) break;  // after each completed ring
    }
    if (!best.valid()) return out;
    if (reg && !join(id, owner, best, stored, allied)) return out;
    out.cell = best;
    out.set_dest = !flag_;
    return out;
}

std::span<const FighterCellGrid::Entry> FighterCellGrid::entries(FighterCell c) const {
    const int32_t k = index(c);
    if (k < 0) return {};
    return cells_[static_cast<size_t>(k)];
}

void FighterCellGrid::hash(StateHasher& h) const {
    h.i32(nx_);
    h.i32(ny_);
    for (size_t k = 0; k < cells_.size(); ++k) {
        if (cells_[k].empty()) continue;
        h.u32(static_cast<uint32_t>(k));
        h.u32(static_cast<uint32_t>(cells_[k].size()));
        for (const Entry& e : cells_[k]) {
            h.i32(e.id);
            h.i32(e.owner);
        }
    }
    h.u32(0xFFFFFFFFu);
}

void FighterCellGrids::build(V2 mn, V2 mx) {
    const F3 c{(mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, 0.0f};
    const F3 h{(mx.x - mn.x) * 0.5f, (mx.y - mn.y) * 0.5f, 0.0f};
    dogfight.build(c, h, kDogfightCellSize, 512, 512, true);
    parking.build(c, h, kParkingCellSize, 1, 0, false);
    built = true;
}

void FighterCellGrids::hash(StateHasher& h) const {
    h.boolean(built);
    dogfight.hash(h);
    parking.hash(h);
}

}  // namespace alamo::tactical::movement
