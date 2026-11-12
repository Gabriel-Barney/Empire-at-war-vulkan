#include "alamo/tactical/movement/locomotor.hpp"

#include <algorithm>
#include <cmath>
#include <span>

#include "alamo/tactical/movement/formation.hpp"
#include "alamo/tactical/movement/land_path.hpp"

namespace alamo::tactical::movement {

float remaining_length(const LandMotion& m) {
    if (m.next >= m.path.size()) return 0.0f;
    float l = distance(m.pos, m.path[m.next]);
    for (size_t i = m.next + 1; i < m.path.size(); ++i) l += distance(m.path[i - 1], m.path[i]);
    return l;
}

WalkSpeedOut walk_speed(const WalkSpeedIn& in) {
    WalkSpeedOut o;
    const float v = in.v;
    float t = in.vmax;  // B.1 / B.2
    if (in.formation < 3.0e38f) {
        t = in.formation;
        if (!(t > 0.0f)) o.end_move = true;
    }
    if (!in.started) t = in.a;  // B.3
    const bool start_move = in.state == LocoState::WalkStartMove;
    // B.4 corner cap: 0x20afe0 is |wrap(yaw - wanted)| < |R| (strict)
    if (!in.turn_done && !(in.yaw_err < std::fabs(in.rate)) && !start_move) {
        t = std::min(t, in.frac_cur * in.vmax);
        if (!(t > 0.0f)) o.end_move = true;
    }
    // B.5 brake-ahead toward line(idx + 1), the comparison in double
    if (!start_move) {
        const float vn = in.frac_next * in.vmax;
        if (vn < v && in.a > 0.0f) {
            const double bd = (static_cast<double>(v) * static_cast<double>(v) - static_cast<double>(vn) * static_cast<double>(vn)) /
                              (2.0 * static_cast<double>(in.a));
            if (static_cast<double>(in.line_d) <= bd) {
                t = std::min(t, vn);
                if (!(t > 0.0f)) o.end_move = true;
            }
        }
        t = std::min(t, in.cap);  // B.6
    }
    // B.7: End Move T = a; a Walk_Transition unit holds T = v while its current clip is MOVE / WALKMOVE
    if (o.end_move || in.state == LocoState::WalkEndMove) t = in.hold ? v : in.a;
    float nv = v;  // B.8
    if (t > nv) nv += std::min(in.acc, t - nv);
    else if (t < nv) nv = in.now && !start_move ? t : nv - std::min(in.a, nv - t);
    nv = std::min(nv, in.vmax);
    if (!(nv > 0.0f)) nv = 0.0f;
    o.v = nv;
    return o;
}

float walk_transition_accel(const WalkTransition& wt, float vmax) {
    const float n = wt.count[1];
    if (!(n > 0.0f) || !(wt.s > 0.0f)) return vmax;
    const float r = vmax / wt.s;
    return ((wt.s / n) * r) * r;
}

float walk_transition_decel(const WalkTransition& wt, float vmax, float v, float f0) {
    const float v0 = f0 > 0.0f ? f0 : (v > 0.0f ? v : vmax);
    const float u = std::max(0.75f * vmax, v0);
    const float m = wt.count[2];
    if (m == 0.0f || !(wt.s > 0.0f)) return u;
    const float r = u / wt.s;
    return ((wt.s / (m - 1.0f)) * r) * r;
}

float line_distance_along(V2 pos, V2 d, V2 l0, V2 n) {
    const float dn = dot(d, n);
    if (dn == 0.0f) return 3.0e38f;
    const float mu = cross(l0 - pos, d) / dn;
    const V2 xp = l0 + perp_left(n) * mu;
    return length(pos - xp);
}

namespace {
// MOVEMENT §3.5.2 notation: line(i) = the waypoint line 0x5c4ec0 through path[i] (the port's waypoint i is retail idx + 1),
// normal = the bisector of its two segments; the last waypoint: its segment's direction (fast normalise).
void waypoint_line(const std::vector<V2>& path, size_t i, V2& l0, V2& n) {
    l0 = path[i];
    const V2 din = normalize_retail(path[i] - path[i - 1]);
    n = i + 1 < path.size() ? normalize_retail(din + normalize_retail(path[i + 1] - path[i])) : din;
}
}  // namespace

bool walk_wanted_dir(std::span<const V2> path, uint32_t next, V2 pos, const WalkExtras* x, V2& dir) {
    if (next == 0 || next >= path.size()) return false;  // on the last waypoint (idx + 1 >= count): no direction
    const V2 a = path[next - 1];
    const V2 sd = normalize_retail(path[next] - a);
    float k = 0.0f;
    if (x && x->lane) {
        // 0x41dbb0: e = (the slot's side offset + the jiggle's side on a formation path) - the unit's signed side
        // (s78p [port]: the slot's side offset is 0 - a formation member's path is its lane, already offset by its slot)
        // offset from its segment; |e| <= 0.1 -> 0, else (e -+ 0.1) / 30
        float e = -cross(sd, pos - a);
        if (x->formation_path) e += cross(sd, x->jiggle);
        k = formation_side_correction(e);
    }
    const V2 base = normalize_retail(sd + perp_left(sd) * k);  // item 2, renormalised (fast normalise)
    dir = base;
    if (x && x->avoid && length_sq(base) > 0.0f) dir = base + x->avoid(x->avoid_ctx, base);  // item 3: + 0x509810
    return true;
}

bool walk_wanted_dir(const LandMotion& m, const WalkExtras* x, V2& dir) {
    if (!m.started) return false;
    return walk_wanted_dir(std::span<const V2>(m.path), m.next, m.pos, x, dir);
}

namespace {
// s78n t26 closing review (MOVEMENT §3.5.2 F): the tracked clip's table index (-1: none / not a transition clip).
int32_t wt_index(int32_t clip) {
    for (int32_t i = 0; i < 6; ++i)
        if (kWtClips[i] == clip) return i;
    return -1;
}
// The clip's frame step this frame (fps / 30 x its rate).
float wt_step(const LandMotion& m, const WalkTransition& wt) {
    const int32_t i = wt_index(m.wt_clip);
    return i < 0 ? 0.0f : wt.step[i] * m.wt_rate;
}
// 0x39a180 (as the deploy clips): a one-shot clip has ended when its frame >= count - 2 - step.
bool wt_ended(const LandMotion& m, const WalkTransition& wt) {
    const int32_t i = wt_index(m.wt_clip);
    return i >= 0 && m.wt_pos >= wt.count[i] - 2.0f - wt_step(m, wt);
}
// The anim service of the frame before: the current clip runs on by its step (MOVE loops over its frames).
void wt_advance(LandMotion& m, const WalkTransition& wt) {
    const int32_t i = wt_index(m.wt_clip);
    if (i < 0) return;
    m.wt_pos += wt_step(m, wt);
    // [port] the MOVE loop wraps at its frame count (s78o b0087: one fmod, exact and equal to the subtraction for a step
    // below the count, where a huge position no longer loops; a non-finite one restarts the clip at frame 0)
    if (i == 0 && wt.count[0] > 0.0f && m.wt_pos >= wt.count[0]) m.wt_pos = std::fmod(m.wt_pos, wt.count[0]);
    if (!std::isfinite(m.wt_pos)) m.wt_pos = 0.0f;
}
// 0x372ae0: the transition clips' rate V / S (1.0 when S = 0).
float wt_vs(const WalkTransition& wt, float vmax) { return wt.s > 0.0f ? vmax / wt.s : 1.0f; }
// F 0x14 (0x61f510) before the move step: true -> the end clip has ended (0x15, no speed test).
bool wt_end_move(LandMotion& m, const WalkTransition& wt, float vmax) {
    int32_t k = 0;  // the present end clips 17..20 (0x61d220)
    for (int32_t i = 2; i < 6; ++i) k += wt.count[i] > 0.0f ? 1 : 0;
    const auto play_end = [&](int32_t clip) {
        // played at rate V / S from frame +0xf4 ([M] the start frame "rate + +0xf4" read as +0xf4), +0xf0 := +0xec latched
        m.wt_clip = clip;
        m.wt_pos = m.wt_f4;
        m.wt_rate = wt_vs(wt, vmax);
        m.wt_f0 = m.speed;
    };
    if (m.wt_clip >= 17 && m.wt_clip <= 20) return wt_ended(m, wt);
    if (m.wt_clip == 2 && wt.count[0] > 0.0f && k > 0) {
        // the end clip = 17 + (int(cur / (n_move / k)) % k), switched only when the phase <= the clip step, +0xf4 := the phase
        const float seg = wt.count[0] / static_cast<float>(k);
        float q = std::floor(m.wt_pos / seg);
        if (!(q >= 0.0f) || q > static_cast<float>(k)) q = 0.0f;  // s78o (b0087): never a clip below 17 or an int overflow
        const float phase = m.wt_pos - q * seg;
        if (phase <= wt_step(m, wt)) {
            int32_t e = 17 + static_cast<int32_t>(q) % k;
            if (!(wt.count[e - 15] > 0.0f)) e = 17;  // [port] the end clips are taken as numbered from 17
            m.wt_f4 = phase;
            play_end(e);
        }
        return false;  // B.7 holds T = v while MOVE plays
    }
    play_end(17);  // the current clip 16 -> 17 (any other clip likewise)
    return false;
}
}  // namespace

uint32_t resume_index(const LandMotion& q, uint8_t kind, uint32_t resume) {
    uint32_t next = q.next;
    if (kind == 2) next = std::max(next, std::min(resume, static_cast<uint32_t>(q.path.size())));
    return next == 0 ? 1u : next;
}

WalkResult walk_step(LandMotion& m, const LocomotorParams& p, float speed_cap, const PassabilityGrid* grid, const WalkExtras* x) {
    WalkResult r;
    if (m.next == 0 && !m.path.empty()) m.next = 1;  // path[0] is the start point (retail idx = next - 1)
    const float vmax = p.max_speed;
    const float vmax_u = x && x->vmax_u > 0.0f ? x->vmax_u : vmax;
    // s78n t26 closing review (MOVEMENT §3.4 "Units with Walk_Transition"): a transition unit's acceleration and
    // deceleration come from its MOVESTART / MOVE_ENDONE frame counts (V = vmax_u); the deceleration's u reads the speed,
    // so it is taken afresh where it is used
    const WalkTransition* wt = x && x->wt && x->wt->on ? x->wt : nullptr;
    const bool wts = wt && wt->states;
    const float acc = wt ? walk_transition_accel(*wt, vmax_u) : (p.accel > 0.0f ? p.accel : vmax);
    const float dec_p = p.decel > 0.0f ? p.decel : vmax;
    const auto dec_of = [&]() { return wt ? walk_transition_decel(*wt, vmax_u, m.speed, m.wt_f0) : dec_p; };
    if (wts) wt_advance(m, *wt);
    LandMotion* below = x ? x->below : nullptr;
    bool forced = x && x->forced;
    const auto yaw_of = [&](const LandMotion& k) {
        V2 wd;
        return walk_wanted_dir(k, x, wd) && length_sq(wd) > 0.0f ? heading_deg(wd) : k.yaw;
    };
    // §3.5.1 "Arrival turn, details" item 1 (state 0x15): End Move at a speed at or below one frame's deceleration - speed
    // 0; with a started path, heading for the last waypoint and dot(pos - W, n) > -5 (n = the last segment's direction) the
    // index steps on; on the last waypoint the unit has arrived where it stands (the caller runs the arrival turn / pop),
    // else the move restarts from state 0 (the start rule below, in the same frame). No started path: idle 0x16.
    // s78n t26 closing review (§3.5.2 F 0x14): a Walk_Transition unit with end clips enters 0x15 when its end clip has
    // ended (no speed test); before that the end clip starts at a MOVE phase boundary ([port] no end clip: the speed test)
    bool to15 = false;
    if (m.state == LocoState::WalkEndMove) {
        const bool clips = wts && (wt->count[2] > 0.0f || wt->count[3] > 0.0f || wt->count[4] > 0.0f || wt->count[5] > 0.0f);
        to15 = clips ? wt_end_move(m, *wt, vmax_u) : !(m.speed > dec_of());
    }
    if (to15) {
        m.speed = 0.0f;
        m.wt_f0 = 0.0f;   // 0x15 entry: +0xec and +0xf0 := 0
        m.wt_clip = -1;   // the idle / arrival clips (a restart's 0x12 plays MOVESTART again)
        const size_t n = m.path.size();
        if (!m.started || n == 0) {
            m.state = LocoState::WalkStopped;
            return r;
        }
        if (n >= 2 && static_cast<size_t>(m.next) + 1 == n &&
            dot(m.pos - m.path[n - 1], normalize_retail(m.path[n - 1] - m.path[n - 2])) > -5.0f) {
            m.next = static_cast<uint32_t>(n);
            m.turn_done = false;
        }
        if (m.next >= n) {
            m.state = LocoState::WalkStopped;
            r.arrived = true;
            return r;
        }
        m.state = LocoState::None;
        r.restart = true;
    }
    // §3.5.2 A: the start rule in state 0 (also idle 0x16, and a turn in place the caller dropped): no top path -> the frame
    // ends; an unstarted path is started (the 0x16 handler); |delta| <= R, or |delta| <= CE with the current waypoint's
    // speed fraction > 0 -> Start Move (0x12 -> 0x13 and the move step in the same call); else the turn in place 0xF / 0x10
    // / 0x11 (the caller runs it, WalkResult::turn). Start Move (0x12, a deployed walk's start) goes to 0x13 with no start
    // test. A started path on its last waypoint: 0x12 -> 0x15, arrived.
    const LocoState s0 = m.state;
    if (s0 == LocoState::WalkStopped || s0 == LocoState::None || s0 == LocoState::WalkStartTurn ||
        s0 == LocoState::WalkTurning || s0 == LocoState::WalkEndTurn) {
        m.wt_clip = -1;  // idle / turn clips: no transition clip current
        if (m.path.empty()) {
            m.speed = 0.0f;
            m.state = LocoState::WalkStopped;
            return r;
        }
        m.started = true;
        if (m.next >= m.path.size()) {
            m.speed = 0.0f;
            m.state = LocoState::WalkStopped;
            r.arrived = true;
            return r;
        }
        const float desired = yaw_of(m);
        const float err = std::fabs(wrap_deg(desired - m.yaw));
        const float cur = wp_frac(m, m.next - 1);
        if (err <= p.turn_rate || (err <= p.close_enough_angle && cur > 0.0f)) {
            m.state = wts ? LocoState::WalkStartMove : LocoState::WalkMoving;
        } else {
            m.state = LocoState::WalkStartTurn;
            m.speed = 0.0f;
            r.turn = true;
            r.want = desired;
            return r;
        }
    } else if (s0 == LocoState::WalkStartMove && !wts) {
        m.state = LocoState::WalkMoving;
    }
    // s78n t26 closing review (§3.5.2 A / F 0x12, 0x61ff40): with Walk_Transition the MOVESTART clip plays (speed 0 on its
    // first frame) and the move step runs in 0x12 until the clip has ended, then 0x13 in the same frame (+0xf4 := the frames
    // run past count - 2). A path not started -> 0x14 when MOVESTART is current, else 0x15. [port] a model without MOVESTART
    // goes on as a unit without the flag (0x13).
    if (wts && m.state == LocoState::WalkStartMove) {
        if (!(wt->count[1] > 0.0f)) {
            m.state = LocoState::WalkMoving;
        } else if (!m.path.empty() && !m.started) {
            if (m.wt_clip == 16) {
                m.state = LocoState::WalkEndMove;
            } else {
                m.speed = 0.0f;  // 0x15 with no started path: idle 0x16, the frame ends
                m.wt_f0 = 0.0f;
                m.wt_clip = -1;
                m.state = LocoState::WalkStopped;
                return r;
            }
        } else if (m.wt_clip != 16) {
            m.wt_clip = 16;
            m.wt_pos = 0.0f;
            m.wt_rate = wt_vs(*wt, vmax_u);
            m.speed = 0.0f;
        } else {
            m.wt_f4 = std::max(0.0f, m.wt_pos + wt_step(m, *wt) - (wt->count[1] - 2.0f));
            if (wt_ended(m, *wt)) m.state = LocoState::WalkMoving;
        }
    }
    // 0x13 (0x61f840): a transition unit starts the MOVE clip at frame int(+0xf4)
    if (wts && m.state == LocoState::WalkMoving && m.wt_clip != 2) {
        m.wt_clip = 2;
        m.wt_pos = std::floor(m.wt_f4);
        m.wt_rate = 0.0f;
    }
    // §3.5.2 C.1: vmax_u = 0 -> nothing; P not started -> End Move; the speed controller B.
    if (!(vmax_u > 0.0f)) return r;
    const bool have_p = !m.path.empty();
    if (have_p && !m.started) m.state = LocoState::WalkEndMove;
    WalkSpeedIn si;
    si.v = m.speed;
    si.vmax = vmax;
    si.acc = acc;
    si.a = dec_of();
    si.hold = wts && (m.wt_clip == 2 || m.wt_clip == 0x42);
    if (x) si.formation = x->formation;
    si.cap = speed_cap;
    si.now = x && x->cap_now;
    si.started = have_p && m.started;
    si.turn_done = m.turn_done;
    si.rate = p.turn_rate;
    si.state = m.state;
    if (si.started && m.next < m.path.size()) {
        si.yaw_err = std::fabs(wrap_deg(yaw_of(m) - m.yaw));
        si.frac_cur = wp_frac(m, m.next - 1);
        si.frac_next = wp_frac(m, m.next);
        if (si.frac_next * vmax < m.speed) {  // B.5: D along d to line(idx + 1)
            V2 l0, n;
            waypoint_line(m.path, m.next, l0, n);
            const V2 d = normalize_retail(m.path[m.next] - m.path[m.next - 1]);
            if (length_sq(d) > 0.0f && length_sq(n) > 0.0f) si.line_d = line_distance_along(m.pos, d, l0, n);
        }
    }
    const WalkSpeedOut so = walk_speed(si);
    m.speed = so.v;
    if (so.end_move) m.state = LocoState::WalkEndMove;
    const float v = m.speed;
    // C.1: the MOVE clip's rate f x V / S (f <= 0: 1.0 in 0x14, else 0) - a transition unit's tracked MOVE clip runs at it
    if (wts && m.wt_clip == 2) {
        const float f = v / vmax_u;
        m.wt_rate = f > 0.0f ? f * wt_vs(*wt, vmax_u) : (m.state == LocoState::WalkEndMove ? 1.0f : 0.0f);
    }
    // C.2: the turn budget B = R, x f = v / vmax_u in Start Move (0x12: only a Walk_Transition unit runs the step there) and
    // End Move; s78n t26.5 (E): a land team container's budget is R with no f scaling. C.3: s = v.
    const bool team = x && x->container;
    float budget = p.turn_rate;
    if ((m.state == LocoState::WalkEndMove || m.state == LocoState::WalkStartMove) && !team) budget *= v / vmax_u;
    float s = v;
    if (!(v > 0.0f)) return r;
    // bkm1 t08 (b0050, MOVEMENT §3.5.3 item 2a): B2 once per call before the loop - delta2 = |wrap(w0 - Y2)| with w0 the
    // primary wanted yaw; B2 = clamp(delta2 / SecondaryTurnAngle x f, Min, Max) x R (f = v / vmax_u, not x f again in
    // 0x12 / 0x14)
    float budget2 = 0.0f;
    if (p.secondary) {
        const float d2 = std::fabs(wrap_deg(yaw_of(m) - m.yaw2));
        const float q = p.sec_angle != 0.0f ? d2 / p.sec_angle * (v / vmax_u) : 0.0f;
        budget2 = std::clamp(q, p.sec_min, std::max(p.sec_min, p.sec_max)) * p.turn_rate;
    }
    // w1 (0x61d890): within SecondaryTurnLookaheadDistance of the current segment's end - the next segment's wanted yaw
    // (its own side correction), or on the last segment the started path below's (from its index, stepped on while the
    // unit is past its waypoint lines); else w0
    const auto yaw2_want = [&]() {
        V2 wd;
        const size_t nx = m.next;
        if (nx >= 1 && nx < m.path.size()) {
            const V2 a = m.path[nx - 1];
            const V2 sg = m.path[nx] - a;
            const float l = length(sg);
            const V2 d = normalize_retail(sg);
            if (dot(m.pos - a, d) + p.sec_lookahead > l) {
                if (nx + 1 < m.path.size()) {
                    if (walk_wanted_dir(std::span<const V2>(m.path), static_cast<uint32_t>(nx + 1), m.pos, x, wd) && length_sq(wd) > 0.0f)
                        return heading_deg(wd);
                } else if (below && below->started && below->next >= 1 && below->next < below->path.size()) {
                    uint32_t k = below->next;
                    for (; static_cast<size_t>(k) + 1 < below->path.size(); ++k) {
                        V2 ql0, qn;
                        waypoint_line(below->path, k, ql0, qn);
                        if (!(dot(qn, m.pos - ql0) >= -0.001f)) break;
                    }
                    if (walk_wanted_dir(std::span<const V2>(below->path), k, m.pos, x, wd) && length_sq(wd) > 0.0f) return heading_deg(wd);
                }
            }
        }
        return yaw_of(m);
    };
    bool no_turn = false;
    const int32_t dirs = x ? x->directions : 1;
    for (size_t guard = 0; guard < 4 * m.path.size() + 16; ++guard) {
        // C.4: nothing to walk (no path, on the last waypoint, not started): pop into a started path below (not in End
        // Move; no more turning in this call), else End Move and the coast along the last frame's displacement (a zero
        // vector stays zero: the point is still tested and committed). s78n t26.5 (E): a container coasts along its facing.
        if (m.path.empty() || m.next >= m.path.size() || !m.started) {
            if (!below || !below->started || m.state == LocoState::WalkEndMove) {
                m.state = LocoState::WalkEndMove;
                r.coast = true;
                const V2 np = m.pos + (team ? dir_from_deg(m.yaw) : normalize_retail(m.last_dir)) * s;
                if (!forced && grid && !grid->passable_world(np, p.mask)) {
                    r.blocked = true;
                    return r;
                }
                if (!(np == m.pos)) r.moved = true;
                m.pos = np;
                return r;
            }
            std::swap(m.path, below->path);
            std::swap(m.next, below->next);
            std::swap(m.frac, below->frac);
            std::swap(m.turn_done, below->turn_done);
            std::swap(m.started, below->started);
            m.next = resume_index(m, x->below_kind, x->below_resume);
            forced = x->below_forced;
            below = nullptr;
            r.popped = true;
            no_turn = true;
            // s78n t26.5 (FIGHTER_FLYING §7.2 caller 3): the slots are re-laid along the path below's segment
            if (x->relay && m.next >= 1 && m.next < m.path.size()) x->relay->push_back(m.path[m.next] - m.path[m.next - 1]);
            continue;
        }
        // C.5: n . (pos - L0) >= -0.001 -> the waypoint is passed (0x5c4920: the turn-done byte cleared)
        V2 l0, n;
        waypoint_line(m.path, m.next, l0, n);
        if (dot(n, m.pos - l0) >= -0.001f) {
            ++m.next;
            m.turn_done = false;
            continue;
        }
        // C.6: turn toward the wanted yaw by min(budget, |delta|) (the step that covers the rest lands exactly); the leftover
        // carries over to the next waypoint of this call; leftover > 0 -> the turn-done byte
        if (!no_turn && budget > 0.0f) {
            const float w = yaw_of(m);
            const float e = std::fabs(wrap_deg(w - m.yaw));
            m.yaw = turn_toward(m.yaw, w, budget);
            budget = e > budget ? 0.0f : budget - e;
            if (budget > 0.0f) m.turn_done = true;
        }
        // bkm1 t08: Y2 steps with B2 toward w1 under the same conditions, its leftover carried to the next waypoint of the call
        if (p.secondary && !no_turn && budget2 > 0.0f) {
            const float w2 = yaw2_want();
            const float e2 = std::fabs(wrap_deg(w2 - m.yaw2));
            m.yaw2 = turn_toward(m.yaw2, w2, budget2);
            budget2 = e2 > budget2 ? 0.0f : budget2 - e2;
        }
        // C.7: the travel direction vf +0x288 (0x61e0f0): Movement_Directions_Count 1 = the facing; N > 1 = of the N
        // directions spaced 360 / N from the facing the one with the largest dot product with the wanted direction (the
        // first on ties); <= 0 = the wanted direction
        // s78n t26.5 (E): a container travels along its facing (cos yaw, sin yaw)
        V2 t = team ? dir_from_deg(m.yaw) : normalize_retail(dir_from_deg(m.yaw));
        if (dirs != 1 && !team) {
            V2 wd;
            if (walk_wanted_dir(m, x, wd) && length_sq(wd) > 0.0f) {
                if (dirs <= 0) {
                    t = normalize_retail(wd);
                } else {
                    float best = dot(t, wd);
                    for (int32_t k = 1; k < dirs; ++k) {
                        const V2 c = normalize_retail(dir_from_deg(m.yaw + 360.0f * static_cast<float>(k) / static_cast<float>(dirs)));
                        if (dot(c, wd) > best) {
                            best = dot(c, wd);
                            t = c;
                        }
                    }
                }
            }
        }
        // C.8: a degenerate segment (with d normalised exactly: along the line, or perpendicular to its normal) is skipped
        const V2 seg = m.path[m.next] - m.path[m.next - 1];
        const float sl = length(seg);
        const V2 de = sl > 0.0f ? V2{seg.x / sl, seg.y / sl} : V2{};
        if (std::fabs(n.x * de.y - n.y * de.x) == 1.0f || dot(de, n) == 0.0f) {
            ++m.next;
            m.turn_done = false;
            if (r.degen_skips < 255) ++r.degen_skips;
            continue;
        }
        // C.9: D = |pos - X|, X the point of line(idx + 1) on the line through pos along d (fast normalise); tau = t . d,
        // tau = 0 -> the waypoint is skipped (quirk); s' = |D / tau|
        const V2 d = normalize_retail(seg);
        const float dn = dot(d, n);
        const float tau = dot(t, d);
        if (tau == 0.0f || dn == 0.0f) {
            ++m.next;
            m.turn_done = false;
            if (tau == 0.0f && r.tau_skips < 255) ++r.tau_skips;
            continue;
        }
        const float mu = cross(l0 - m.pos, d) / dn;
        const V2 xp = l0 + perp_left(n) * mu;
        const float sp = std::fabs(length(m.pos - xp) / tau);
        // C.10: pos += t min(s, s'); not forced (path +0x5c, or the planner's escape leg) and the point fails 0x50e280 ->
        // blocked, no commit
        const V2 np = m.pos + t * std::min(s, sp);
        if (!(forced || m.next < m.escape_until) && grid && !grid->passable_world(np, p.mask)) {
            r.blocked = true;
            return r;
        }
        m.pos = np;
        r.moved = true;
        if (s <= sp) {
            // C.11: the budget ends before the line; rest = (s' - s) + the 3-D lengths after path[next] (0x54f700; bkm1 t06:
            // the stored waypoint z is 0 except a slot-leg path's last, LandMotion::end_z); v^2 / (2a) > rest -> End Move
            // unless a started path lies below
            float rest = sp - s;
            for (size_t i = static_cast<size_t>(m.next) + 1; i < m.path.size(); ++i) {
                float l = distance(m.path[i - 1], m.path[i]);
                if (i + 1 == m.path.size() && m.end_z != 0.0f && m.path[i] == m.end_at) l = std::sqrt(l * l + m.end_z * m.end_z);
                rest += l;
            }
            if ((v * v) / (2.0f * dec_of()) > rest && !(below && below->started)) m.state = LocoState::WalkEndMove;
            return r;
        }
        ++m.next;  // s > s': on the line - the next waypoint with the rest of the budget
        m.turn_done = false;
        s -= sp;
        // s78n t26.5 (E, FIGHTER_FLYING §7.2 caller 3): a container re-lays its slots along the new segment
        if (x && x->relay && m.next < m.path.size()) x->relay->push_back(m.path[m.next] - m.path[m.next - 1]);
    }
    return r;
}

}  // namespace alamo::tactical::movement
