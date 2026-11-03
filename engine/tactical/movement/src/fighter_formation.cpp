// s78f t02: fighter formations - the move-block stack of a squadron (or of a fighter without one) and the formation
// step that runs it (docs/re/FIGHTER_FLYING.md §4.11 A-I, A1_READINGS §33.4 / §33.4.1, MOVEMENT §6.1 / §6.2). Replaces
// S7 t37's one-block FighterGuard (fighter.hpp FighterFormation, movement_system.hpp "s78f t02").
//
// Port model. One FighterFormation per key (squadron container id, or the solo fighter's id), ascending key; the retail
// coordinator is not modelled: every space move order of a key's fighters is a fresh group move (a new coordinator with
// empty formations), so the order's block becomes the only block (Move_To 0x4f8200 -> Set_Move 0x423c10). The formation's
// members are the squadron container (or the fighter): the port reads the container through its fighters - L = the box
// centre of the fighters (§4.10 B, I 9), T / the owner / the attack range = the first fighter's (D 2) - and "plan" /
// "re-path" (0x41c1a0 / 0x417520) are moves of the fighters to the top's goal point (MovementSystem::move with the
// internal flag: the formation is kept). The order's own move is its plan [port], so a new formation starts in state 1.
// Every function here runs serially on the sim thread; formations in ascending key, members in team index order.
#include <algorithm>
#include <cmath>

#include "alamo/tactical/movement/movement_system.hpp"

namespace alamo::tactical::movement {

namespace {

// The block equality 0x40dc90: kind, flags, the push class, the bytes, and the kind's own fields.
bool same_block(const FighterBlock& a, const FighterBlock& b) {
    if (a.kind != b.kind || a.flags != b.flags || a.pushed != b.pushed || a.own_speed != b.own_speed || a.loop != b.loop)
        return false;
    if (a.kind == kFighterBlockObject) return a.object == b.object && a.hard_point == b.hard_point;
    return a.point.x == b.point.x && a.point.y == b.point.y && a.facing == b.facing;
}

float dist2d(V2 a, V2 b) { return std::sqrt(length_sq(a - b)); }

}  // namespace

// ---------------------------------------------------------------- lookups
FighterFormation* MovementSystem::fform_ptr(ObjectId key) {
    auto it = std::lower_bound(fforms_.begin(), fforms_.end(), key, [](const FighterFormation& f, ObjectId v) { return f.key < v; });
    return it != fforms_.end() && it->key == key ? &*it : nullptr;
}

const FighterFormation* MovementSystem::fighter_formation(ObjectId key) const {
    auto it = std::lower_bound(fforms_.begin(), fforms_.end(), key, [](const FighterFormation& f, ObjectId v) { return f.key < v; });
    return it != fforms_.end() && it->key == key ? &*it : nullptr;
}

std::vector<ObjectId> MovementSystem::fform_members(ObjectId key) {
    std::vector<ObjectId> out;
    if (const FighterSquad* c = squad_ptr(key)) {
        for (const ObjectId m : c->members)
            if (unit_ptr(m)) out.push_back(m);
    } else if (const Unit* u = unit_ptr(key); u && is_fighter(*u) && u->fs.squad < 0) {
        out.push_back(key);
    }
    return out;
}

// D 1: Set_Move, Move_To, Push and the waypoint append refuse a block naming a fighter inside a squadron (space: the
// goal is always the container) and a kind-3 block without an object.
bool MovementSystem::fform_block_ok(const FighterBlock& b) {
    if (b.kind != kFighterBlockObject) return true;
    if (b.object < 0) return false;
    const Unit* o = unit_ptr(b.object);
    return !(o && is_fighter(*o) && o->fs.squad >= 0);
}

// A block object's position (+0x78..+0x80): a mover of this system (a fighter's body position), a squadron container
// (the box centre of its fighters, §4.10 B [port: the fighters' positions, no box extents]), else the host hook.
bool MovementSystem::fobject_pos(ObjectId id, F3& p) {
    if (id < 0) return false;
    if (const Unit* x = unit_ptr(id)) {
        p = is_fighter(*x) ? x->fs.b.p : F3{x->m.pos.x, x->m.pos.y, x->z};
        return true;
    }
    if (squad_ptr(id)) {
        const std::vector<ObjectId> mem = fform_members(id);
        if (mem.empty()) return false;
        p = fform_centre(mem);
        return true;
    }
    return fhooks_.position && fhooks_.position(id, p);
}

// Goal point 0x583680 / 0x583150 of a block (D 7): a position block's point; an object block's object position [port:
// the hard point's position and the land team-member rule are not used by fighter formations]; clamped to the playable
// box (z is not kept: the 2-D tests read x / y only).
bool MovementSystem::fblock_goal(const FighterBlock& b, V2& out) {
    if (b.kind == kFighterBlockObject) {
        F3 p;
        if (!fobject_pos(b.object, p)) return false;
        out = {p.x, p.y};
    } else {
        out = b.point;
    }
    if (space_.has_box()) {
        out.x = std::clamp(out.x, space_.box_min().x, space_.box_max().x);
        out.y = std::clamp(out.y, space_.box_min().y, space_.box_max().y);
    }
    return true;
}

// FOW_Object_Hidden(mode, lead owner, object, force 1) (0x35f470). [port] Stealth (0x39b140 without Target_Stealth_Units)
// is not separate: the host's hook may fold it in.
bool MovementSystem::fform_hidden(ObjectId key, ObjectId object) {
    if (!fhooks_.hidden || object < 0) return false;
    const std::vector<ObjectId> mem = fform_members(key);
    const Unit* l = mem.empty() ? nullptr : unit_ptr(mem.front());
    return l && fhooks_.hidden(l->d.owner, object);
}

// 0x41b2a0: the slowest member's speed [port: a squadron container's 0x370f00(company type) is read as its slowest
// fighter's max speed]. A top with flag 0x10 (SpaceIdleMovementSpeed) is never built [port].
float MovementSystem::fform_unit_speed(ObjectId key) {
    float s = -1.0f;
    for (const ObjectId m : fform_members(key))
        if (const Unit* u = unit_ptr(m)) s = s < 0.0f ? u->lp.max_speed : std::min(s, u->lp.max_speed);
    return s;
}

// L (0x41a9b0, G 3 / I 9): one member -> its position; a squadron formation's member is the container, whose position is
// the box centre of its fighters (§4.10 B): (min + max) / 2 per axis over the fighters in team index order.
F3 MovementSystem::fform_centre(const std::vector<ObjectId>& members) {
    F3 lo{}, hi{};
    bool first = true;
    for (const ObjectId m : members) {
        const Unit* u = unit_ptr(m);
        if (!u) continue;
        const F3 p = u->fs.b.p;
        if (first) {
            lo = hi = p;
            first = false;
        } else {
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
    }
    return {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
}

// "G moving" (§4.11 B 2): a locomotor with a current path that is not finished (0x5569b0, path state != 5). A ship: a
// goal it has not reached; a fighter / squadron: its (leader's) path is open; anything else (no pack): not moving.
bool MovementSystem::fobject_moving(ObjectId id) {
    if (Unit* x = unit_ptr(id)) return is_fighter(*x) ? fpath_open(*x) : (x->has_goal && !x->arrived);
    if (const FighterSquad* c = squad_ptr(id); c && !c->members.empty())
        if (Unit* l = unit_ptr(c->members.front())) return fpath_open(*l);
    return false;
}

// The dogfight cell of a formation's lead or of a block object (pack +0x1c4 / +0x1c8; §5): a squadron container's, a
// solo fighter's own, none for other objects.
FighterCell MovementSystem::fobject_cell(ObjectId id) {
    if (const FighterSquad* c = squad_ptr(id)) return c->dog;
    if (Unit* x = unit_ptr(id); x && is_fighter(*x)) {
        if (const FighterSquad* c = fsquad(*x)) return c->dog;
        return x->fs.dog;
    }
    return {};
}

// ---------------------------------------------------------------- orders
// Coordinator::Move_To (0x4f8200) for each fighter formation of a (non-waypoint) order: a fresh coordinator, so the
// order's block is the only block (Set_Move into an empty stack). Recorded goal = the order's destination, clamped as
// move_space clamps it; first 0x4192b0 FPS frames later [M]. Set_Move refuses a block naming a squadron's fighter (D 1):
// that formation then has none. F speed := -1 (unset); the order's own move is the plan, so the state is 1 [port].
void MovementSystem::fform_order(const MoveOrder& order, const std::vector<uint32_t>& idx, uint32_t frame) {
    if (cfg_.mode != MoveMode::Space) return;
    std::vector<ObjectId> keys;
    for (uint32_t i : idx)
        if (is_fighter(units_[i])) keys.push_back(fkey(units_[i]));
    if (keys.empty()) return;
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    FighterBlock b;
    b.kind = order.goal_object >= 0 ? kFighterBlockObject : kFighterBlockPosition;
    b.object = order.goal_object;
    b.point = order.destination;
    if (space_.has_box()) {
        b.point.x = std::clamp(b.point.x, space_.box_min().x, space_.box_max().x);
        b.point.y = std::clamp(b.point.y, space_.box_min().y, space_.box_max().y);
    }
    b.facing = order.facing_deg;
    b.threat = order.threat_threshold;
    b.flags = order.flags;
    const bool ok = fform_block_ok(b);
    for (const ObjectId k : keys) {
        std::erase_if(fforms_, [k](const FighterFormation& f) { return f.key == k; });
        if (!ok) continue;
        FighterFormation f;
        f.key = k;
        f.blocks.push_back(b);
        f.state = kFormationMoving;
        f.goal = b.point;
        f.next = frame + kFighterGuardInterval;
        if (const FighterSquad* c = squad_ptr(k)) {
            f.follow = c->guard_follow;
        } else if (const Unit* u = unit_ptr(k)) {
            f.follow = u->d.space_layer_type == 0;
        }
        auto it = std::lower_bound(fforms_.begin(), fforms_.end(), k, [](const FighterFormation& x, ObjectId v) { return x.key < v; });
        fforms_.insert(it, std::move(f));
    }
}

void MovementSystem::fighter_new_order(const MoveOrder& order, uint32_t frame) {
    if (cfg_.mode != MoveMode::Space) return;
    std::vector<uint32_t> idx;
    for (const ObjectId id : order.units) {
        const int32_t i = find(id);
        if (i < 0 || std::find(idx.begin(), idx.end(), static_cast<uint32_t>(i)) != idx.end()) continue;
        idx.push_back(static_cast<uint32_t>(i));
    }
    fform_order(order, idx, frame);
}

// The waypoint-queue move (MOVEMENT §6.2, 0x2a8240): a fighter whose formation exists hands the block to it (once per
// formation, first-seen order; coordinator vf +0x30 -> 0x420f70) and does not move now; the others stay in `idx`.
void MovementSystem::fform_waypoint(const MoveOrder& order, std::vector<uint32_t>& idx) {
    if (!order.waypoint || cfg_.mode != MoveMode::Space) return;
    std::vector<ObjectId> done;
    std::vector<uint32_t> rest;
    for (const uint32_t i : idx) {
        if (!is_fighter(units_[i])) {
            rest.push_back(i);
            continue;
        }
        const ObjectId k = fkey(units_[i]);
        FighterFormation* f = fform_ptr(k);
        if (!f) {
            rest.push_back(i);
            continue;
        }
        if (std::find(done.begin(), done.end(), k) != done.end()) continue;
        done.push_back(k);
        FighterBlock b;
        b.kind = order.goal_object >= 0 ? kFighterBlockObject : kFighterBlockPosition;
        b.object = order.goal_object;
        b.point = order.destination;
        if (space_.has_box()) {
            b.point.x = std::clamp(b.point.x, space_.box_min().x, space_.box_max().x);
            b.point.y = std::clamp(b.point.y, space_.box_min().y, space_.box_max().y);
        }
        b.facing = order.facing_deg;
        b.threat = order.threat_threshold;
        b.flags = order.flags;
        if (fform_block_ok(b)) fform_append(*f, b);
    }
    idx = std::move(rest);
}

// 0x420f70 (B 1): MaxWaypointsPerPath queued (0x08) blocks at most; b gets 0x04; empty -> the only block; equal to
// blocks[0] -> ignored; an earlier player waypoint (0x04, index 1..n-1) whose goal is within 80 (space; 20 on land) of
// b's closes a patrol loop (blocks[0..i] +0x5e := 1, b not added); else b goes to the BOTTOM (runs after everything
// queued). Then, when n was 1 or the formation had arrived (state 3), state := 0.
void MovementSystem::fform_append(FighterFormation& f, FighterBlock b) {
    uint32_t queued = 0;
    for (const FighterBlock& x : f.blocks) queued += (x.flags & kMoveFlagQueue) != 0 ? 1u : 0u;
    if (queued >= cfg_.constants.max_waypoints_per_path) return;
    b.flags = static_cast<uint8_t>(b.flags | kMoveFlagPlayerOrder);
    const size_t n = f.blocks.size();
    if (n == 0) {
        f.blocks.push_back(b);
    } else {
        if (same_block(b, f.blocks[0])) return;
        V2 gb;
        const bool have_gb = fblock_goal(b, gb);
        const float loop_r = cfg_.mode == MoveMode::Land ? 20.0f : 80.0f;
        size_t loop_at = 0;
        for (size_t i = 1; i < n && have_gb && loop_at == 0; ++i) {
            V2 gi;
            if ((f.blocks[i].flags & kMoveFlagPlayerOrder) != 0 && fblock_goal(f.blocks[i], gi) && dist2d(gi, gb) <= loop_r) loop_at = i;
        }
        if (loop_at > 0) {
            for (size_t i = 0; i <= loop_at; ++i) f.blocks[i].loop = true;
        } else {
            f.blocks.insert(f.blocks.begin(), b);
        }
    }
    if (n == 1 || f.state == kFormationArrived) f.state = kFormationPlan;
}

// Pop 0x4105a0 (B 1), n > 1 only: the top T goes; a patrol T (+0x5e) gets 0x08 and is re-inserted at the bottom; the
// recorded goal := goal(new top); state := 5 when T named an object, else 0. [port] Not modelled: the listener leave and
// the path-progress object F +0x230 (no such objects), signal 0x2a (flag 0x40, no reader), 0x3a8710 on Space_Layer
// members and unit ability 5 (no fighter has it active through a block).
void MovementSystem::fform_pop(FighterFormation& f) {
    if (f.blocks.size() <= 1) return;
    FighterBlock t = f.blocks.back();
    f.blocks.pop_back();
    if (t.loop) {
        t.flags = static_cast<uint8_t>(t.flags | kMoveFlagQueue);
        f.blocks.insert(f.blocks.begin(), t);
    }
    V2 g;
    if (fblock_goal(f.blocks.back(), g)) f.goal = g;
    f.state = t.kind == kFighterBlockObject ? kFormationPopped : kFormationPlan;
}

bool MovementSystem::fighter_pop(ObjectId key) {
    FighterFormation* f = fform_ptr(key);
    if (!f || f->blocks.size() <= 1) return false;
    fform_pop(*f);
    return true;
}

// Push 0x41fd30 (B 1) of an auto attack block (0x42d0f0 -> 0x41cf70: {kind 3, object, hard point, flags 0, +0x50 = the
// auto class, +0x5d = 1}). Refused (D 1) for a squadron's fighter or an unknown object [port: the host passes live,
// hostile objects; the dying / order-lock refusals are the host's]. Pre-pop while n > 1: the top goes unless it is a
// guard, a patrol block (+0x5e) or of another push class. Then appended on top; recorded goal := goal(b); speed := -1;
// state := 0. [port] Not modelled: unit ability 5 switched off on the members, the land staging (F +0x228 = 1 only).
bool MovementSystem::fighter_push(ObjectId key, ObjectId object, int32_t hard_point) {
    if (cfg_.mode != MoveMode::Space) return false;
    FighterFormation* f = fform_ptr(key);
    if (!f) return false;
    FighterBlock b;
    b.kind = kFighterBlockObject;
    b.object = object;
    b.hard_point = hard_point;
    b.pushed = true;
    b.own_speed = true;
    V2 g;
    if (!fform_block_ok(b) || !fblock_goal(b, g)) return false;
    while (f->blocks.size() > 1) {
        const FighterBlock& t = f->blocks.back();
        if ((t.flags & kMoveFlagGuardMove) != 0 || t.loop || t.pushed != b.pushed) break;
        fform_pop(*f);
    }
    f->blocks.push_back(b);
    f->goal = g;
    f->speed = -1.0f;
    f->state = kFormationPlan;
    return true;
}

void MovementSystem::fform_stop(std::span<const ObjectId> ids) {
    if (fforms_.empty()) return;
    for (const ObjectId id : ids)
        if (const Unit* u = unit_ptr(id); u && is_fighter(*u)) {
            const ObjectId k = fkey(*u);
            std::erase_if(fforms_, [k](const FighterFormation& f) { return f.key == k; });
        }
}

// A unit or squadron leaves: its own formation goes. Blocks naming it: the bottom block (the coordinator's, listener
// 0x4f77e0) becomes a position block (fform_lost); an upper block (Push subscribed F's own listener F +0x258 to the
// object; its handler was not read [M]) is removed, and when it was the top the formation re-plans its new top as after
// a pop of an object block (state 5).
void MovementSystem::fform_removed(ObjectId id) {
    if (fforms_.empty()) return;
    std::erase_if(fforms_, [id](const FighterFormation& f) { return f.key == id; });
    for (FighterFormation& f : fforms_) {
        if (f.blocks.empty()) continue;
        const size_t n = f.blocks.size();
        const bool top_named = f.blocks.back().kind == kFighterBlockObject && f.blocks.back().object == id;
        for (size_t i = n; i-- > 1;)
            if (f.blocks[i].kind == kFighterBlockObject && f.blocks[i].object == id) f.blocks.erase(f.blocks.begin() + static_cast<std::ptrdiff_t>(i));
        if (f.blocks[0].kind == kFighterBlockObject && f.blocks[0].object == id) fform_lost(f, 0);
        if (top_named && n > 1 && f.blocks.size() < n) f.state = kFormationPopped;
    }
}

// Listener 0x4f77e0 -> 0x41a580 (§4.10 E): kind 1, object null, position = the last waypoint of the formation's path
// (its lead fighter's), else the recorded goal; flags kept (a guard becomes a position guard).
void MovementSystem::fform_lost(FighterFormation& f, size_t block) {
    if (block >= f.blocks.size()) return;
    FighterBlock& b = f.blocks[block];
    b.kind = kFighterBlockPosition;
    b.object = -1;
    b.hard_point = -1;
    const std::vector<ObjectId> mem = fform_members(f.key);
    const Unit* l = mem.empty() ? nullptr : unit_ptr(mem.front());
    b.point = l && !l->wps.empty() ? l->wps.back().pos : f.goal;
}

// ---------------------------------------------------------------- the formation step (0x422b70)
void MovementSystem::fform_tick(uint32_t frame, SimRng& rng) {
    for (size_t fi = 0; fi < fforms_.size(); ++fi) {
        FighterFormation& f = fforms_[fi];
        const std::vector<ObjectId> mem = fform_members(f.key);
        if (mem.empty() || f.blocks.empty()) continue;
        // [port] an object block whose object left this system without a removal counts as gone.
        for (size_t i = f.blocks.size(); i-- > 0;) {
            F3 p;
            if (f.blocks[i].kind != kFighterBlockObject || fobject_pos(f.blocks[i].object, p)) continue;
            if (i == 0) {
                fform_lost(f, 0);
            } else {
                if (i + 1 == f.blocks.size()) f.state = kFormationPopped;
                f.blocks.erase(f.blocks.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }
        if (f.state == kFormationPlan || f.state == kFormationPopped) {
            fform_plan(f, frame, rng);
            continue;
        }
        if (f.state == kFormationMoving) {
            // Arrival 0x423760: every member done (no path left: arrived, enter_idle or the attack's path reset).
            bool done = true;
            for (const ObjectId m : mem)
                if (const Unit* u = unit_ptr(m); u && (u->has_goal || u->need_path)) done = false;
            if (done) {
                const size_t n = f.blocks.size();
                const FighterBlock& t = f.blocks.back();
                if (n == 1 || t.kind == kFighterBlockObject) {
                    f.state = kFormationArrived;  // [port] signal 10: the members' own arrival is the host's reading
                } else if ((t.flags & kMoveFlagPlayerOrder) != 0) {
                    if ((f.blocks[n - 2].flags & kMoveFlagQueue) != 0) fform_pop(f);
                    else f.state = kFormationArrived;
                } else {
                    fform_pop(f);
                }
                continue;
            }
            if (frame >= f.next) fform_update(f, frame, rng);
            continue;
        }
        if (f.state == kFormationArrived) {
            // States 3 / 4 (0x423070 / 0x4234d0): with one block +0x240 := 1 (flag 0x20 cleared); a hidden object goal
            // (C 3): d = |L - P| (2-D), P = the object's position [M: the block's stored point +0x18 is never written for
            // object blocks]; d >= 40 -> re-plan toward it; else Set_Move(position at L, flags kept, clear) -> state 0
            // [M: the members' 0x38d730(null) attack clear is not modelled]. Otherwise (and with n >= 2) 0x410a60.
            if (f.blocks.size() == 1) {
                f.single_arrived = true;
                f.blocks[0].flags = static_cast<uint8_t>(f.blocks[0].flags & ~kMoveFlagSkipValidation);
                const FighterBlock t = f.blocks[0];
                if (t.kind == kFighterBlockObject && fform_hidden(f.key, t.object)) {
                    const F3 l = fform_centre(mem);
                    F3 p;
                    fobject_pos(t.object, p);
                    if (dist2d({l.x, l.y}, {p.x, p.y}) >= 40.0f) {
                        f.state = kFormationPlan;
                    } else {
                        FighterBlock nb = t;
                        nb.kind = kFighterBlockPosition;
                        nb.object = -1;
                        nb.hard_point = -1;
                        nb.point = {l.x, l.y};
                        f.blocks.assign(1, nb);
                        f.goal = nb.point;
                        f.speed = -1.0f;
                        f.state = kFormationPlan;
                    }
                    continue;
                }
            }
            if (frame >= f.next) fform_recheck(f, frame, rng);
        }
    }
}

// Plan (states 0 / 2 / 5 -> 0x41c1a0 -> state 1): the members move to the top's goal point; the recorded goal follows
// [M: the plan's own goal write]; +0x240 is cleared [M].
void MovementSystem::fform_plan(FighterFormation& f, uint32_t frame, SimRng& rng) {
    V2 dest;
    if (!fblock_goal(f.blocks.back(), dest)) return;
    f.goal = dest;
    f.single_arrived = false;
    fform_repath(f, dest, frame, rng, false);
    f.state = kFormationMoving;
}

// A move of the formation's fighters to `dest` (0x417520 / the plan's issue). `keep_in_range` (G 5: a non-guard object
// top): a member already in its max range of the object, not inside its min range and not moving keeps its own path
// [port: the squadron container is read through its first fighter, so the whole squadron keeps it].
void MovementSystem::fform_repath(FighterFormation& f, V2 dest, uint32_t frame, SimRng& rng, bool keep_in_range) {
    const std::vector<ObjectId> mem = fform_members(f.key);
    if (mem.empty()) return;
    const FighterBlock& t = f.blocks.back();
    if (keep_in_range) {
        const Unit* l = unit_ptr(mem.front());
        F3 op;
        const bool in = l && fobject_pos(t.object, op) &&
                        (fhooks_.in_range ? fhooks_.in_range(l->d.id, t.object) : f3_length(f3_sub(l->fs.b.p, op)) <= l->fp.attack_range);
        if (in && !l->has_goal) return;
    }
    MoveOrder mo;
    mo.units = mem;
    mo.destination = dest;
    mo.facing_deg = t.kind == kFighterBlockPosition ? t.facing : kNoFacing;
    mo.threat_threshold = t.threat;
    mo.flags = static_cast<uint8_t>(t.flags & ~kMoveFlagQueue);  // no start-delay draws for a formation's own moves
    const bool was = fform_internal_;
    fform_internal_ = true;
    move(mo, rng, frame);
    fform_internal_ = was;
}

// 0x4192b0 (A1_READINGS §33.4.1 as corrected by FIGHTER_FLYING §4.11): state 1 only, at most once per FPS frames.
//   1. gates (members, the timer; next := frame + FPS before the lead gates); L = fform_centre; the lead / T / owner =
//      the first fighter [port: the container's type is read through it, D 2].
//   2. R = Get_Base_Attack_Range(T, lead) / 2 [port: FighterParams::attack_range]; a guard top: min(R, Space_Guard_Range).
//   3. a guard top with an object and T layer 0 (follow): G not moving -> with n > 1 the top is dropped without the pop's
//      side effects, state 0, step 4 runs on the new top with this R and the old recorded goal. G moving -> F speed :=
//      0x41b2a0; |L - G| < R (3-D, strict) -> cap = min(F speed, 2 S) (S = G's formation speed while it moves in one, else
//      its effective max speed); F's path already G's -> F speed := cap, return; else copy G's path at G's index, F speed :=
//      cap, a one-node copy -> state 3; return. No fog test here (C 1). [port] The footprint check 0x40eb90 is not
//      modelled.
//   4. (and when blocks[n-2] is a guard with an object, only while 0x4165f0 agrees) the re-path: an object top hidden
//      for the lead's owner -> step 5; the space vetoes (D 3: a solo fighter whose squadron object is attacking this
//      formation; the same dogfight cell) -> return; D = |recorded goal - goal(top)| (2-D); D <= R -> no re-path unless
//      a non-guard object top, not every member can hit it (0x40edd0) and the lead is stopping [port: fighter packs are
//      never 0x14 / 0x15; blocks[0] attack-move and the lead without a path counts]; the land stop (F +0x228 = 1) never
//      applies to fighter formations. Then 0x4165f0 false -> step 5; [port: the UNIT_AI veto 0x4f5780 is not modelled:
//      Keep_Moving_In_Battle is unset in FoC, obj +0x108 +0x38 [L]]; re-path: recorded goal := goal(top), the members
//      move there (G 5 keep rule for a non-guard object top).
//   5. a top without 0x04: pop (0x4105a0) when T has layer 0, else 0x4220d0 (a member byte [L]: not modelled, no pop).
void MovementSystem::fform_update(FighterFormation& f, uint32_t frame, SimRng& rng) {
    f.next = frame + kFighterGuardInterval;
    const std::vector<ObjectId> mem = fform_members(f.key);
    const Unit* lp = mem.empty() ? nullptr : unit_ptr(mem.front());
    if (!lp) return;
    const Unit& lead = *lp;
    const F3 l = fform_centre(mem);
    float rr = 0.5f * lead.fp.attack_range;
    if ((f.blocks.back().flags & kMoveFlagGuardMove) != 0) rr = std::min(rr, cfg_.constants.space_guard_range);
    // Step 3.
    {
        const FighterBlock t = f.blocks.back();
        if ((t.flags & kMoveFlagGuardMove) != 0 && t.kind == kFighterBlockObject && f.follow) {
            if (!fobject_moving(t.object)) {
                if (f.blocks.size() > 1) {
                    f.blocks.pop_back();  // F +0x100 -= 0x68 (no patrol re-queue, no goal update, no 0x2a)
                    f.state = kFormationPlan;
                }
            } else {
                f.speed = fform_unit_speed(f.key);
                F3 gp;
                fobject_pos(t.object, gp);
                if (f3_length(f3_sub(l, gp)) < rr) {
                    float s = -1.0f;
                    if (const FighterFormation* gf = fform_ptr(t.object); gf && gf->speed >= 0.0f) {
                        s = gf->speed;
                    } else if (const Unit* o = unit_ptr(t.object)) {
                        s = o->formation_speed < 1.0e38f ? o->formation_speed : loco_max_speed(*o);
                    }
                    const float cap = s >= 0.0f ? std::min(f.speed, 2.0f * s) : f.speed;
                    const Unit* o = unit_ptr(t.object);
                    bool equal = o && o->wps.size() == lead.wps.size() && !o->wps.empty();
                    for (size_t i = 0; equal && i < o->wps.size(); ++i) {
                        const SpaceWaypoint& a = o->wps[i];
                        const SpaceWaypoint& b = lead.wps[i];
                        equal = a.pos.x == b.pos.x && a.pos.y == b.pos.y && a.heading == b.heading && a.speed == b.speed && a.time == b.time;
                    }
                    if (equal) {  // 0x40ded0: the paths are equal -> only the cap
                        f.speed = cap;
                        return;
                    }
                    if (fighter_copy_path(f.key, t.object, frame)) {
                        f.speed = cap;
                        if (o && o->wps.size() == 1) f.state = kFormationArrived;
                        return;
                    }
                    f.speed = cap;  // [port] nothing to copy (G follows no space path): step 4
                }
            }
        }
    }
    const FighterBlock t = f.blocks.back();
    const size_t n = f.blocks.size();
    bool step4 = true;
    if (n >= 2) {
        const FighterBlock& below = f.blocks[n - 2];
        if ((below.flags & kMoveFlagGuardMove) != 0 && below.kind == kFighterBlockObject && !fform_leash(f)) step4 = false;
    }
    if (step4) {
        bool ok = true;
        if (t.kind == kFighterBlockObject) {
            if (fform_hidden(f.key, t.object)) {
                ok = false;
            } else {
                // D 3 vetoes (space, kind 3).
                const bool lead_team = squad_ptr(f.key) != nullptr;
                if (!lead_team && squad_ptr(t.object) && fhooks_.attack_target) {
                    const ObjectId at = fhooks_.attack_target(t.object);
                    if (at >= 0 && (at == f.key || std::find(mem.begin(), mem.end(), at) != mem.end())) return;
                }
                const FighterCell a = fobject_cell(f.key), b = fobject_cell(t.object);
                if (a.i != -1 && a == b) return;
            }
        }
        if (ok) {
            V2 g;
            if (!fblock_goal(t, g)) return;
            const float d = dist2d(f.goal, g);
            const bool b7 = t.kind == kFighterBlockObject && (t.flags & kMoveFlagGuardMove) == 0;
            if (d <= rr) {
                const bool stopping = (f.blocks[0].flags & kMoveFlagAttackMove) != 0 && !lead.has_goal;
                if (!(b7 && stopping && !fform_can_hit(f))) return;
            }
            if (fform_leash(f)) {
                f.goal = g;
                fform_repath(f, g, frame, rng, b7);
                return;
            }
        }
    }
    // Step 5.
    if ((f.blocks.back().flags & kMoveFlagPlayerOrder) == 0 && f.follow) fform_pop(f);
}

// 0x410a60 (A), state 3, the same timer: a = 0x4165f0, c = 0x40edd0. !a && !c -> pop; !a && c && n >= 2 with an object
// below -> pop; a && !c -> F speed := -1, state 0 (re-plan); else nothing.
// [port] (s78f review, B 3): a guard top with blocks below it whose object is not moving is dropped here as in 0x4192b0
// step 3 (state 0, the next block is planned). Retail reaches that pop in state 1 within one check ("a guard followed by
// shift waypoints lasts only while G moves"); the port's members can finish the plan move to a still G before the timer,
// so the formation arrives (state 3: an object top never pops on arrival) with the guard still on top.
void MovementSystem::fform_recheck(FighterFormation& f, uint32_t frame, SimRng& rng) {
    (void)rng;
    f.next = frame + kFighterGuardInterval;
    if (const FighterBlock& t = f.blocks.back(); f.blocks.size() > 1 && (t.flags & kMoveFlagGuardMove) != 0 &&
                                                 t.kind == kFighterBlockObject && f.follow && !fobject_moving(t.object)) {
        f.blocks.pop_back();  // as step 3: no patrol re-queue, no goal update
        f.state = kFormationPlan;
        return;
    }
    const bool a = fform_leash(f), c = fform_can_hit(f);
    const size_t n = f.blocks.size();
    if (!a && !c) {
        fform_pop(f);
    } else if (!a && c && n >= 2 && f.blocks[n - 2].kind == kFighterBlockObject) {
        fform_pop(f);
    } else if (a && !c) {
        f.speed = -1.0f;
        f.state = kFormationPlan;
    }
}

// 0x4165f0 (E): a guard top -> true; an object top hidden for members[0]'s owner -> false; n = 1 -> T with a Space_Layer:
// false, else blocks[0] is kind 3. n >= 2 -> the leash: (the top has no object or blocks[0] lacks 0x01) and blocks[n-2]
// +0x5e clear -> Rr = blocks[0] guard ? Guard_Chase_Range : Idle_Chase_Range around goal(blocks[n-2]); else
// Attack_Move_Response_Range around F's current path point (the lead fighter's next waypoint, else its last; no path: L).
// The top's object attacking the guarded object (blocks[n-2]'s, a guard) or this formation adds
// Autonomous_Move_Extension_Vs_Attacker; a moving guarded object answers only its attackers. False when |L - anchor|
// (2-D) >= Rr; with the object farther than Rr from the anchor, [M: 0x5672b0 "attackable from the point Rr toward it"]
// read as: within Rr + the lead's attack range; else true. Ranges: the squadron type's (a solo fighter: its own).
bool MovementSystem::fform_leash(FighterFormation& f) {
    const FighterBlock& t = f.blocks.back();
    const size_t n = f.blocks.size();
    if ((t.flags & kMoveFlagGuardMove) != 0) return true;
    if (t.kind == kFighterBlockObject && fform_hidden(f.key, t.object)) return false;
    if (n == 1) return f.follow && f.blocks[0].kind == kFighterBlockObject;
    const std::vector<ObjectId> mem = fform_members(f.key);
    Unit* lead = mem.empty() ? nullptr : unit_ptr(mem.front());
    if (!lead) return false;
    const FighterSquad* sq = squad_ptr(f.key);
    const FighterChaseRanges& cr = sq ? sq->chase : lead->fp.chase;
    const FighterBlock& below = f.blocks[n - 2];
    const F3 l3 = fform_centre(mem);
    const V2 l{l3.x, l3.y};
    float rr = 0.0f;
    V2 anchor = l;
    if ((t.kind != kFighterBlockObject || (f.blocks[0].flags & kMoveFlagAttackMove) == 0) && !below.loop) {
        rr = (f.blocks[0].flags & kMoveFlagGuardMove) != 0 ? cr.guard : cr.idle;
        if (!fblock_goal(below, anchor)) anchor = l;
    } else {
        rr = cr.attack_move;
        if (!lead->wps.empty()) anchor = lead->wps[std::min<size_t>(static_cast<size_t>(lead->fs.idx) + 1, lead->wps.size() - 1)].pos;
    }
    if (rr < 0.0f) rr = 0.0f;
    const ObjectId g = (below.flags & kMoveFlagGuardMove) != 0 && below.kind == kFighterBlockObject ? below.object : -1;
    if (t.kind == kFighterBlockObject) {
        const ObjectId at = fhooks_.attack_target ? fhooks_.attack_target(t.object) : -1;
        const bool attacker = at >= 0 && (at == g || at == f.key || std::find(mem.begin(), mem.end(), at) != mem.end());
        if (attacker) rr += cr.extension;
        else if (g >= 0 && fobject_moving(g)) return false;
    }
    if (dist2d(l, anchor) >= rr) return false;
    if (t.kind == kFighterBlockObject) {
        F3 cp;
        if (!fobject_pos(t.object, cp)) return false;
        const float dc = dist2d({cp.x, cp.y}, anchor);
        if (dc > rr) return dc <= rr + lead->fp.attack_range;
    }
    return true;
}

// 0x40edd0 (E, D 5): true without a top object; false when it is hidden for members[0]'s owner [port: stealth folded into
// the hook]; else false when a tested member cannot hit it from where it stands (in_range hook; default 3-D distance <=
// its attack range). Tested: a solo fighter itself; a squadron formation's member is the container - skipped once
// +0x240 is set (space), else tested through its first team member [M: the container's vf +0x230 is false].
bool MovementSystem::fform_can_hit(FighterFormation& f) {
    const FighterBlock& t = f.blocks.back();
    if (t.kind != kFighterBlockObject || t.object < 0) return true;
    if (fform_hidden(f.key, t.object)) return false;
    if (squad_ptr(f.key) && f.single_arrived) return true;
    const std::vector<ObjectId> mem = fform_members(f.key);
    const Unit* m = mem.empty() ? nullptr : unit_ptr(mem.front());
    if (!m) return true;
    if (fhooks_.in_range) return fhooks_.in_range(m->d.id, t.object);
    F3 p;
    if (!fobject_pos(t.object, p)) return false;
    return f3_length(f3_sub(m->fs.b.p, p)) <= m->fp.attack_range;
}

// formation_speed_or_max 0x5ca390 (F): the pack = the container's when the fighter is in a squadron, else its own; with
// a formation, a current path (`need_path`; the leader's for a squadron) and a guard top with an object -> F +0x21c;
// else -1 (the caller uses maxspeed). [port] An unset F speed (-1) also reads as maxspeed (G 6 [M]).
float MovementSystem::fform_speed_read(Unit& u, bool need_path) {
    if (fforms_.empty()) return -1.0f;
    const FighterFormation* f = fform_ptr(fkey(u));
    if (!f || f->blocks.empty()) return -1.0f;
    const FighterBlock& t = f->blocks.back();
    if ((t.flags & kMoveFlagGuardMove) == 0 || t.kind != kFighterBlockObject) return -1.0f;
    if (need_path) {
        const Unit& p = fsquad(u) ? fleader(u) : u;
        if (p.wps.empty()) return -1.0f;
    }
    return f->speed;
}

float MovementSystem::fformation_speed(Unit& u) {
    const float s = fform_speed_read(u, true);
    return s >= 0.0f ? s : u.lp.max_speed;
}

// avoid_speed 0x5ca430 (F): min(minspeed, 0.5 x F +0x21c) under the same top test (no path test).
float MovementSystem::favoid_speed(Unit& u) {
    const float s = fform_speed_read(u, false);
    return s >= 0.0f ? std::min(u.sp.min_speed, 0.5f * s) : u.sp.min_speed;
}

}  // namespace alamo::tactical::movement
