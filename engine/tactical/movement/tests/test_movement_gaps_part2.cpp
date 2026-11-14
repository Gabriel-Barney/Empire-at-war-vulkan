// Part 2 of test_movement_gaps (bo3 t03: split by position; see test_movement_gaps_common.hpp).
#include "test_movement_gaps_common.hpp"

void test_forced_path() {
    // A wall across x = 15 (the whole height): a normal move stops at the nearest reachable spot on the near side; a
    // flag-0x20 move walks straight through to the destination.
    for (const bool forced : {false, true}) {
        MovementSystem sys{MovementConfig{}};
        PassabilityGrid g = open_grid(40, 40);
        for (int32_t y = 0; y < 40; ++y) g.set_base(15, y, kPassImpassable);
        sys.set_grid(std::move(g));
        CHECK(sys.add_unit(unit(1, cell_center(10, 10), 2, 2.0f, 8.0f, LocomotorKind::Default)));
        SimRng rng(3);
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(20, 12);
        if (forced) o.flags = kMoveFlagSkipValidation;
        sys.move(o, rng, 0);
        bool arrived = false;
        for (uint32_t f = 1; f < 600 && !arrived; ++f) {
            sys.tick(f, rng);
            arrived = sys.view(1).arrived;
        }
        const UnitView v = sys.view(1);
        std::printf("forced path %d: arrived %d at (%.1f, %.1f), failed %d\n", forced ? 1 : 0, arrived ? 1 : 0,
                    static_cast<double>(v.pos.x), static_cast<double>(v.pos.y), v.failed ? 1 : 0);
        if (forced) {
            // s78n t26.4 (MOVEMENT 3.5.1 item 1): arrived where 0x15 stopped it, within 5 before the last line
            CHECK(arrived && distance(v.pos, cell_center(20, 12)) < 5.0f && v.goal == cell_center(20, 12));
        } else {
            CHECK(v.pos.x < 15.0f * kCellSize);  // the wall closes the map: the route ends on the near side
        }
    }
}

// s78k t03 (MOVEMENT §3.4 / §3.7): the team locomotor cap - two team units of different Max_Speed, moved one at a time
// (no formation): each walks at most at min(Max_Speed) x mult x the container factor; leaving the team removes it.
void test_team_cap() {
    MovementSystem sys{MovementConfig{}};
    land_system(sys, 100);
    CHECK(sys.add_unit(unit(1, cell_center(10, 10), 2, 1.0f, 8.0f, LocomotorKind::Default)));
    CHECK(sys.add_unit(unit(2, cell_center(10, 30), 2, 3.0f, 8.0f, LocomotorKind::Default)));
    const float mult = MovementConstants{}.land_speed_multiplier;
    CHECK(sys.set_team(1, 50, 1.5f) && sys.set_team(2, 50, 1.5f) && !sys.set_team(9, 50, 1.0f));
    SimRng rng(3);
    MoveOrder o;
    o.units = {2};
    o.destination = cell_center(60, 30);
    sys.move(o, rng, 100);  // cu8j t01 [M]: after frame 60, so the plan is not gated to frame 61
    float top = 0.0f;
    for (uint32_t f = 101; f < 140; ++f) {
        sys.tick(f, rng);
        top = std::max(top, sys.view(2).speed);
    }
    const float cap = (1.0f * mult) * 1.5f;
    CHECK(sys.view(1).team_speed == cap && sys.view(2).team_speed == cap && top == cap);
    CHECK(sys.set_team(2, -1, 1.0f));
    for (uint32_t f = 140; f < 160; ++f) {
        sys.tick(f, rng);
        top = std::max(top, sys.view(2).speed);
    }
    CHECK(sys.view(2).team_speed > 1.0e30f && top == 3.0f * mult);
    std::printf("team cap: %.3f in the team, %.3f alone\n", static_cast<double>(cap), static_cast<double>(top));
}

// s78l t04 (FIGHTER_FLYING §4.11 item 4, MOVEMENT §3.4, TACTICAL_COMBAT §4.4 "No mode gate"): a space squadron's
// formation speed (F +0x21c, 0x41b2a0) is the container's effective max speed - the minimum raw Max_Speed of its fighters
// x the space multiplier x the container factor set_team gives it (the squad Mod_Multiplier SPEED) - and not the fighters'
// own speed factors. A lone fighter's formation keeps its own effective max speed (own factor included).
void test_squad_team_speed() {
    MovementConfig cfg;
    cfg.mode = MoveMode::Space;
    const float mult = MovementConstants{}.space_speed_multiplier;
    // Guarding a fast carrier (2 x its speed > any fighter speed below, so the follow cap never bites): the formation's
    // speed after `frames` frames with the given container factor / fighter-1 own factor; `squad` false = fighter 1 alone.
    const auto run = [&](bool squad, float team_factor, float own_factor, uint32_t frames) {
        MovementSystem sys(cfg);
        sys.space().set_box({-4000.0f, -4000.0f}, {4000.0f, 4000.0f});
        const float speeds[3] = {10.0f, 8.0f, 12.0f};  // raw Max_Speed; fighter 2 is the slowest
        FighterSquadDesc sq;
        sq.id = 500;
        for (ObjectId i = 1; i <= 3; ++i) {
            const float side = static_cast<float>(i - 2) * 30.0f;
            UnitDesc f = unit(i, {-2000.0f, 1500.0f + side}, -1, speeds[i - 1], 10.0f, LocomotorKind::Fighter);
            f.space_layer = kLayerFighter;
            f.space_layer_type = 0;
            CHECK(sys.add_unit(f));
            sq.members.push_back(i);
            sq.slots.push_back({0.0f, side, 0.0f});
        }
        if (squad) {
            CHECK(sys.set_fighter_squad(sq));
            for (ObjectId i = 1; i <= 3; ++i) CHECK(sys.set_team(i, 500, team_factor));
        }
        CHECK(sys.set_speed_factor(1, own_factor));
        UnitDesc c = unit(50, {-2000.0f, 0.0f}, -1, 20.0f, 60.0f, LocomotorKind::Default);
        c.space_layer_type = 2;
        CHECK(sys.add_unit(c));
        SimRng rng(5);
        MoveOrder g;
        g.units = squad ? std::vector<ObjectId>{1, 2, 3} : std::vector<ObjectId>{1};
        g.destination = {-2000.0f, 0.0f};
        g.flags = kMoveFlagGuardMove;
        g.goal_object = 50;
        sys.move(g, rng, 0);
        MoveOrder mc;
        mc.units = {50};
        mc.destination = {3000.0f, 0.0f};
        sys.move(mc, rng, 0);
        float speed = -1.0f;
        for (uint32_t fr = 0; fr < frames; ++fr) {
            sys.tick(fr, rng);
            if (const FighterFormation* f = sys.fighter_formation(squad ? 500 : 1)) speed = f->speed;
        }
        CHECK(2.0f * sys.view(50).max_speed > 12.0f * mult * 2.0f);  // the follow cap stays above every value below
        return std::pair<float, float>{speed, sys.view(1).max_speed};
    };
    const float slow = 8.0f * mult;
    CHECK(run(true, 1.0f, 1.0f, 40).first == slow);         // squadron, factor 1: the slowest fighter's raw speed x mult
    CHECK(run(true, 1.5f, 1.0f, 40).first == slow * 1.5f);  // container factor 1.5 (squad SPRINT)
    CHECK(run(true, 0.5f, 1.0f, 40).first == slow * 0.5f);  // container factor 0.5
    {  // a fighter's own factor (0.25 on fighter 1, which would make it the slowest) is not read for a squadron
        const auto [s, own] = run(true, 1.0f, 0.25f, 40);
        CHECK(s == slow && own == 10.0f * mult * 0.25f);
    }
    {  // a lone fighter: its own effective max speed, own factor included; set_team is not involved
        const auto [s, own] = run(false, 1.0f, 0.5f, 40);
        CHECK(s == own && own == 10.0f * mult * 0.5f);
    }
    std::printf("squad team speed: slowest %.3f, x1.5 %.3f, x0.5 %.3f\n", static_cast<double>(slow), static_cast<double>(slow * 1.5f),
                static_cast<double>(slow * 0.5f));
}

// s78l t01 (MOVEMENT §3.4 "Team cap, exact" / "Full team formula", TACTICAL_COMBAT §4.4 "Team move speed"): the team
// speed is min(container Max_Speed, min member raw Max_Speed) x mult x the container factor, and a member walks AT it -
// its own speed factor is not multiplied in (no double count); a stunned member (factor 0) stays put.
void test_team_speed_exact() {
    const float mult = MovementConstants{}.land_speed_multiplier;
    // Two members (raw 1.0 and 1.2) of a Stormtrooper_Team-like container (Max_Speed 0.8); member 2 walks alone.
    const auto run = [&](float container_max, float factor, float own2) {
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        CHECK(sys.add_unit(unit(1, cell_center(10, 10), 2, 1.0f, 6.0f, LocomotorKind::Default)));
        CHECK(sys.add_unit(unit(2, cell_center(10, 30), 2, 1.2f, 6.0f, LocomotorKind::Default)));
        CHECK(sys.set_team(1, 50, factor, container_max) && sys.set_team(2, 50, factor, container_max));
        CHECK(sys.set_speed_factor(2, own2));
        SimRng rng(3);
        MoveOrder o;
        o.units = {2};
        o.destination = cell_center(60, 30);
        sys.move(o, rng, 100);  // cu8j t01 [M]: after frame 60, so the plan is not gated to frame 61
        float top = 0.0f;
        for (uint32_t f = 101; f < 140; ++f) {
            sys.tick(f, rng);
            top = std::max(top, sys.view(2).speed);
        }
        return std::pair<float, float>{sys.view(2).team_speed, top};
    };
    {  // the container's 0.8 is below both members: it sets the team speed
        const auto [ts, top] = run(0.8f, 1.0f, 1.0f);
        CHECK(ts == 0.8f * mult && top == ts);
    }
    {  // a container faster than its members (Sandpeople_Team 1.0 over 0.5 members): the slowest member
        const auto [ts, top] = run(1.5f, 1.0f, 1.0f);
        CHECK(ts == 1.0f * mult && top == ts);
    }
    {  // container unknown (-1): the members' minimum alone (the s78k behaviour)
        const auto [ts, top] = run(-1.0f, 1.0f, 1.0f);
        CHECK(ts == 1.0f * mult && top == ts);
    }
    {  // the container factor applies once; the member's own factor 0.5 does not enter, nor caps it
        const auto [ts, top] = run(0.8f, 1.25f, 0.5f);
        CHECK(ts == 0.8f * mult * 1.25f && top == ts);
    }
    {  // a factor above the member's own top speed: the team speed still rules (the container moves the team)
        const auto [ts, top] = run(-1.0f, 1.5f, 1.0f);
        CHECK(ts == 1.0f * mult * 1.5f && top == ts && top > 1.2f * mult);
    }
    {  // a stunned member (own factor 0: its locomotor is suspended) does not move
        const auto [ts, top] = run(0.8f, 1.0f, 0.0f);
        CHECK(ts == 0.8f * mult && top == 0.0f);
    }
    {  // space squadron: the container's Max_Speed is in the formation speed minimum too (0x41b2a0 -> 0x370f00)
        MovementConfig cfg;
        cfg.mode = MoveMode::Space;
        const float sm = MovementConstants{}.space_speed_multiplier;
        MovementSystem sys(cfg);
        sys.space().set_box({-4000.0f, -4000.0f}, {4000.0f, 4000.0f});
        FighterSquadDesc sq;
        sq.id = 500;
        for (ObjectId i = 1; i <= 2; ++i) {
            const float side = static_cast<float>(i) * 30.0f;
            UnitDesc f = unit(i, {-2000.0f, 1500.0f + side}, -1, 10.0f, 10.0f, LocomotorKind::Fighter);
            f.space_layer = kLayerFighter;
            f.space_layer_type = 0;
            CHECK(sys.add_unit(f));
            sq.members.push_back(i);
            sq.slots.push_back({0.0f, side, 0.0f});
        }
        CHECK(sys.set_fighter_squad(sq));
        for (ObjectId i = 1; i <= 2; ++i) CHECK(sys.set_team(i, 500, 1.0f, 6.0f));
        UnitDesc c = unit(50, {-2000.0f, 0.0f}, -1, 20.0f, 60.0f, LocomotorKind::Default);
        c.space_layer_type = 2;
        CHECK(sys.add_unit(c));
        SimRng rng(5);
        MoveOrder g;
        g.units = {1, 2};
        g.destination = {-2000.0f, 0.0f};
        g.flags = kMoveFlagGuardMove;
        g.goal_object = 50;
        sys.move(g, rng, 0);
        MoveOrder mc;
        mc.units = {50};
        mc.destination = {3000.0f, 0.0f};
        sys.move(mc, rng, 0);
        float speed = -1.0f;
        for (uint32_t fr = 0; fr < 40; ++fr) {
            sys.tick(fr, rng);
            if (const FighterFormation* f = sys.fighter_formation(500)) speed = f->speed;
        }
        CHECK(speed == 6.0f * sm);
    }
    std::printf("team speed exact: container 0.8 -> %.3f\n", static_cast<double>(0.8f * mult));
}

// s78l t02 (MOVEMENT §3.5.1 "Arrival turn, details"): the pure rules.
void test_arrival_turn_rules() {
    CHECK(arrival_turn_threshold(6.0f, false) == 10.0f && arrival_turn_threshold(6.0f, true) == 6.0f &&
          arrival_turn_threshold(12.0f, true) == 10.0f);
    CHECK(anim_idle_class(0) && anim_idle_class(1) && anim_idle_class(71) && anim_idle_class(77) && anim_idle_class(82) &&
          !anim_idle_class(2) && !anim_idle_class(83) && !anim_idle_class(kAnimNone));
    CHECK(anim_turn_clip(3) && anim_turn_clip(4) && anim_turn_clip(21) && anim_turn_clip(24) && !anim_turn_clip(12) &&
          !anim_turn_clip(13) && !anim_turn_clip(25));
    {
        const TurnAnim q = arrival_turn_band(15.0f, 3.0f, 10.0f, 0);
        CHECK(q.anim == kAnimTurnLeftQuarter && q.rate == 3.0f * 0.25f * 15.0f / 11.25f);
        const TurnAnim h = arrival_turn_band(-30.0f, 3.0f, 10.0f, 0);
        CHECK(h.anim == kAnimTurnRightHalf && h.rate == 1.5f);
        const TurnAnim f = arrival_turn_band(45.0f, 3.0f, 10.0f, 0);
        CHECK(f.anim == kAnimTurnLeft && f.rate == 3.0f);
        const TurnAnim n = arrival_turn_band(9.5f, 3.0f, 10.0f, kAnimTurnLeftHalf);  // |e| < T: clip unchanged, rate S
        CHECK(n.anim == kAnimTurnLeftHalf && n.rate == 3.0f);
        const TurnAnim t = arrival_turn_band(10.0f, 3.0f, 10.0f, 0);  // e >= T
        CHECK(t.anim == kAnimTurnLeftQuarter);
    }
    CHECK(arrival_turn_end_clip(3) == kAnimTurnLeftEnd && arrival_turn_end_clip(12) == kAnimTurnLeftEnd &&
          arrival_turn_end_clip(22) == kAnimTurnLeftEnd && arrival_turn_end_clip(4) == kAnimTurnRightEnd &&
          arrival_turn_end_clip(14) == kAnimTurnRightEnd && arrival_turn_end_clip(23) == kAnimTurnRightEnd &&
          arrival_turn_end_clip(0) == kAnimNone && arrival_turn_end_clip(2) == kAnimNone);
}

// s78l t02 (b): the Has_Pre_Turn_Anim arrival turn - begin clip holds the yaw, band rates from the frame after the
// pick, the end clip, then the pop with the default idle; without clip data the band still sets the rate and the turn
// ends early (|e| < 10 with an idle-class clip); a unit without Has_Pre_Turn_Anim plays TURNL once and lands exactly.
bool arrival_clips(ObjectId, int32_t anim, uint32_t& n, float& fps) {
    switch (anim) {
        case kAnimTurnLeftBegin:
        case kAnimTurnRightBegin: n = 12; break;  // 12 frames at 15 fps: 0.5 x speed clip frames per sim frame
        case kAnimTurnLeftEnd:
        case kAnimTurnRightEnd: n = 8; break;
        case kAnimTurnLeft:
        case kAnimTurnRight:
        case kAnimTurnLeftHalf:
        case kAnimTurnRightHalf:
        case kAnimTurnLeftQuarter:
        case kAnimTurnRightQuarter: n = 10; break;
        default: return false;
    }
    fps = 15.0f;
    return true;
}

void test_arrival_turn_clips() {
    struct Frame {
        float yaw = 0.0f;
        bool turning = false;
        bool ended = false;
        std::vector<MovementSystem::TurnClipEvent> ev;
    };
    struct Run {
        float arrival_yaw = 0.0f;
        std::vector<Frame> fr;  // from the frame after the arrival
    };
    const auto run = [](bool pre, bool clips, float stopped, float ras) {
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        if (clips) sys.set_deploy_clip_fn(arrival_clips);
        UnitDesc d = unit(1, cell_center(10, 10), 2, 2.0f, 8.0f, LocomotorKind::Default);
        d.has_pre_turn_anim = pre;
        d.stopped_turn_rate = stopped;
        d.rotation_animation_speed = ras;
        CHECK(sys.add_unit(d));
        SimRng rng(3);
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(20, 10);
        o.final_facing_deg = 90.0f;
        sys.move(o, rng, 0);
        Run r;
        uint32_t f = 1;
        for (; f < 400 && !sys.view(1).arrived; ++f) sys.tick(f, rng);
        CHECK(sys.take_turn_clip_events().empty());
        r.arrival_yaw = sys.view(1).yaw;
        for (int k = 0; k < 120; ++k, ++f) {
            sys.tick(f, rng);
            Frame x;
            x.yaw = sys.view(1).yaw;
            x.turning = sys.view(1).arrival_turn;
            x.ended = sys.view(1).arrival_turn_ended;
            x.ev = sys.take_turn_clip_events();
            CHECK(x.ev.size() <= 1);
            r.fr.push_back(x);
        }
        return r;
    };
    {  // with clips: begin (yaw held) -> TURNL band clips -> end clip -> pop + idle
        const Run r = run(true, true, 3.0f, 1.5f);
        CHECK(std::fabs(r.arrival_yaw) < 1e-3f);
        CHECK(!r.fr[0].ev.empty() && r.fr[0].ev[0].anim == kAnimTurnLeftBegin && r.fr[0].ev[0].speed == 2.0f &&
              r.fr[0].ev[0].blend < 0.0f && r.fr[0].turning);
        // Begin clip: 12 frames at 15 fps x speed 2 = 1 clip frame per sim frame -> in its last 2 frames at position 10.
        size_t k = 1;
        while (k < r.fr.size() && r.fr[k].yaw == r.arrival_yaw) ++k;
        CHECK(k == 10);  // frames 0..9 hold the yaw; the turn starts on frame 10
        CHECK(r.fr[k].yaw == 3.0f);  // first turn frame at S = Stopped_Rate_Of_Turn
        CHECK(!r.fr[k].ev.empty() && r.fr[k].ev[0].anim == kAnimTurnLeft);  // e = 87 after the step: TURNL at S
        bool end = false, idle = false, half = false, quarter = false;
        size_t pop = 0;
        for (size_t j = k; j < r.fr.size(); ++j) {
            for (const auto& e : r.fr[j].ev) {
                half = half || e.anim == kAnimTurnLeftHalf;
                quarter = quarter || e.anim == kAnimTurnLeftQuarter;
                end = end || e.anim == kAnimTurnLeftEnd;
                if (e.idle) {
                    idle = true;
                    pop = j;
                }
            }
        }
        CHECK(half && quarter && end && idle);
        CHECK(!r.fr[pop].turning && (pop == 0 || r.fr[pop - 1].turning));
        for (size_t j = 0; j < r.fr.size(); ++j) CHECK(r.fr[j].ended == (j == pop));  // only the pop frame
        CHECK(std::fabs(r.fr.back().yaw - 90.0f) <= 10.0f && !r.fr.back().turning);
        std::printf("arrival turn (clips): begin held %zu frames, popped at +%zu, final yaw %.3f\n", k, pop,
                    static_cast<double>(r.fr.back().yaw));
    }
    {  // Has_Pre_Turn_Anim without clip data: band rates, early end with 0 < |e| < 10, no events
        const Run r = run(true, false, 3.0f, 1.5f);
        CHECK(r.fr[0].yaw == 3.0f && r.fr[1].yaw == 6.0f);  // S (first frame), then TURNL band at S
        for (const Frame& x : r.fr) CHECK(x.ev.empty());
        const float left = std::fabs(wrap_deg(90.0f - r.fr.back().yaw));
        CHECK(left > 0.0f && left < 10.0f && !r.fr.back().turning);
        // in the quarter band the step shrinks with the error (S x 0.25 x e / 11.25)
        bool shrinking = false;
        for (size_t j = 2; j < r.fr.size(); ++j) {
            const float a = r.fr[j - 1].yaw - r.fr[j - 2].yaw, b = r.fr[j].yaw - r.fr[j - 1].yaw;
            if (b > 0.0f && a > 0.0f && b < a && b < 1.5f) shrinking = true;
        }
        CHECK(shrinking);
        std::printf("arrival turn (no clip data): stopped %.3f deg short\n", static_cast<double>(left));
    }
    {  // without Has_Pre_Turn_Anim, with clip data: TURNL once (speed 1, blend |e| / r x 0.03333), exact landing
        const Run r = run(false, true, 30.0f, 0.0f);
        CHECK(r.fr[0].yaw == 30.0f && r.fr[1].yaw == 60.0f && r.fr[2].yaw == 90.0f);
        CHECK(r.fr[0].ev.size() == 1 && r.fr[0].ev[0].anim == kAnimTurnLeft && r.fr[0].ev[0].speed == 1.0f &&
              r.fr[0].ev[0].blend == 90.0f / 30.0f * 0.03333f);
        size_t plays = 0;
        for (const Frame& x : r.fr)
            for (const auto& e : x.ev) plays += e.idle ? 0u : 1u;
        CHECK(plays == 1);
        size_t ends = 0;
        for (const Frame& x : r.fr) ends += x.ended ? 1u : 0u;
        CHECK(ends == 1);
    }
    {  // TURNL blend cap: |e0| / r x 0.03333 = 90 / 10 x 0.03333 = 0.3 -> kArrivalTurnBlendCapPort
        const Run r = run(false, true, 10.0f, 0.0f);
        CHECK(r.fr[0].ev.size() == 1 && r.fr[0].ev[0].anim == kAnimTurnLeft && r.fr[0].ev[0].blend == kArrivalTurnBlendCapPort);
        // TURNL (10 frames at 0.5 per sim frame) holds the state until its last 2 frames: the pop waits for the clip.
        CHECK(r.fr.back().yaw == 90.0f && !r.fr.back().turning);
    }
}

// s78n t26.4 (MOVEMENT 3.5.1 "Arrival turn, details" item 1, 3.5.2 C.1; replaces the s78l t02 arrival-close [port]): End
// Move at v <= a (0x15) sets the speed to 0; heading for the last waypoint within 5 before its line the index steps on and
// the unit has arrived where it stands; further out the move restarts from state 0 (no arrival). A speed override of 0 is
// vmax_u = 0: the move step does nothing at all (C.1) - the unit neither moves nor arrives while it holds.
void test_arrival_close() {
    LocomotorParams p;
    p.max_speed = 1.0f;
    p.accel = 0.1f;
    p.decel = 0.02f;
    p.turn_rate = 10.0f;
    const auto at_stop = [&](float short_by) {
        LandMotion m;
        m.path = {{0.0f, 0.0f}, {100.0f, 0.0f}};
        m.next = 1;
        m.pos = {100.0f - short_by, 0.0f};
        m.speed = 0.015f;  // <= a
        m.state = LocoState::WalkEndMove;
        const WalkResult r = walk_step(m, p, 3.0e38f, nullptr);
        return std::pair<WalkResult, LandMotion>{r, m};
    };
    {
        const auto [r, m] = at_stop(3.784f);
        CHECK(r.arrived && !r.restart && m.speed == 0.0f && m.state == LocoState::WalkStopped && m.next == 2);
        CHECK(m.pos == (V2{100.0f - 3.784f, 0.0f}));  // where it stands
        std::printf("arrival close: arrived %.3f units short of the last point\n", 3.784);
    }
    {
        const auto [r, m] = at_stop(9.0f);
        CHECK(!r.arrived && r.restart && m.next == 1 && m.state != LocoState::WalkStopped);
    }
    {  // the speed override 0 holds the unit in place, unarrived
        MovementSystem sys{MovementConfig{}};
        PassabilityGrid g = open_grid(60, 60);
        sys.set_grid(std::move(g));
        UnitDesc d = unit(1, cell_center(10, 10), 2, 1.0f, 6.0f, LocomotorKind::Default);
        d.decel = 0.02f;
        CHECK(sys.add_unit(d));
        SimRng rng(3);
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(20, 10);
        sys.move(o, rng, 0);
        bool held = false;
        V2 at{};
        uint32_t f = 1;
        for (; f < 400 && !held; ++f) {
            sys.tick(f, rng);
            if (sys.view(1).has_goal && sys.view(1).goal.x - sys.view(1).pos.x < 4.0f) {
                CHECK(sys.set_speed_override(1, 0.0f));
                held = true;
            }
        }
        at = sys.view(1).pos;
        for (uint32_t n = 0; n < 100; ++n) sys.tick(f++, rng);
        CHECK(held && !sys.view(1).arrived && sys.view(1).pos == at);
    }
}

// s78l t02 (c, MOVEMENT §6.1 "Forced-path destination check and fallback"): a flag-0x20 destination in a blocked cell is
// placed by the ring search (the first passable sample from the unit's yaw, counter-clockwise); a destination off the grid
// or with no passable sample in 20 rings gives no path and no move.
void test_forced_destination() {
    const auto run = [](V2 dest, bool block_ring) {
        MovementSystem sys{MovementConfig{}};
        PassabilityGrid g = open_grid(40, 40);
        g.set_base(20, 12, kPassImpassable);  // the destination's cell
        if (block_ring)
            for (int32_t y = 0; y < 40; ++y)
                for (int32_t x = 12; x < 40; ++x) g.set_base(x, y, kPassImpassable);
        sys.set_grid(std::move(g));
        CHECK(sys.add_unit(unit(1, cell_center(5, 12), 2, 2.0f, 8.0f, LocomotorKind::Default)));
        SimRng rng(3);
        MoveOrder o;
        o.units = {1};
        o.destination = dest;
        o.flags = kMoveFlagSkipValidation;
        sys.move(o, rng, 0);
        bool arrived = false;
        for (uint32_t f = 1; f < 600 && !arrived && !sys.view(1).failed; ++f) {
            sys.tick(f, rng);
            arrived = sys.view(1).arrived;
        }
        return sys.view(1);
    };
    const V2 d = cell_center(20, 12);
    {  // blocked cell: ring search from yaw 0 (east); the first passable sample leaves the cell [390, 410) x [230, 250)
        const UnitView v = run(d, false);
        CHECK(v.arrived && !v.failed);
        CHECK(world_to_cell(v.pos.x) != 20 || world_to_cell(v.pos.y) != 12);
        CHECK(distance(v.pos, d) > 10.0f && distance(v.pos, d) < 30.0f);
        // first ring that leaves the cell: R = 13 (0, 1, 2.8, 5.4, 8.8, 13), sample 0 at yaw 0 -> (d.x + 13, d.y); s78n
        // t26.4 (MOVEMENT 3.5.1 item 1): the unit stops within 5 before it (no snap)
        CHECK(std::fabs(v.goal.x - (d.x + 13.0f)) < 1e-3f && std::fabs(v.goal.y - d.y) < 1e-3f);
        CHECK(distance(v.pos, v.goal) < 5.0f);
        std::printf("forced destination: placed at (%.2f, %.2f), %.2f from the blocked point\n", static_cast<double>(v.pos.x),
                    static_cast<double>(v.pos.y), static_cast<double>(distance(v.pos, d)));
    }
    {  // off the grid: no passable sample -> no path, the unit stays
        const UnitView v = run({-2000.0f, -2000.0f}, false);
        CHECK(v.failed && !v.arrived && v.pos.x == cell_center(5, 12).x && v.pos.y == cell_center(5, 12).y);
    }
    {  // everything within the 20 rings blocked -> no path
        const UnitView v = run(cell_center(30, 20), true);
        CHECK(v.failed && !v.arrived && v.pos.x == cell_center(5, 12).x);
    }
}

// s78n t03 (TACTICAL_COMBAT §5.4.4 item 2, MOVEMENT §3.7, FIGHTER_FLYING §7): a land squad container walks its own path
// at the team speed; its members steer to their slots around it (0x62d620). The members stand on one side of the
// container, so their centroid is off the container's straight path; a member behind its slot catches up faster than the
// container (up to its own max speed), one ahead of it waits; an idle team's members recover to their slots (> 4.0 away).
void test_land_squad() {
    struct Run {
        uint64_t crc = 0;
        float max_off_path = 0.0f, min_centroid_gap = 1.0e9f;
        float lead_speed = 0.0f, lag_speed = 1.0e9f, cont_speed = 0.0f, team = 0.0f;
        bool path = false, arrived = false, member_view = false;
        float final_err = 1.0e9f;
    };
    const float mult = MovementConstants{}.land_speed_multiplier;
    const V2 start = cell_center(10, 20);
    const auto run = [&](const ParallelFor& pf) {
        Run r;
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        UnitDesc c = unit(100, start, 2, 5.0f, 12.0f, LocomotorKind::Default);
        c.team_container = true;
        CHECK(sys.add_unit(c));
        // slots (along, side): all on the +side, so the centroid sits 25 off the container
        const V2 slots[3] = {{0.0f, 15.0f}, {0.0f, 25.0f}, {0.0f, 35.0f}};
        // member 1 starts 20 behind its slot, member 3 20 ahead of it, member 2 on it
        const float along0[3] = {-20.0f, 0.0f, 20.0f};
        std::vector<ObjectId> ids;
        for (ObjectId id = 1; id <= 3; ++id) {
            const V2 sl = slots[id - 1];
            // member 1 is faster (3.0) than the team speed (the others' 2.0), so it can catch up
            CHECK(sys.add_unit(unit(id, {start.x + sl.x + along0[id - 1], start.y + sl.y}, 2, id == 1 ? 3.0f : 2.0f, 3.0f,
                                    LocomotorKind::Default)));
            ids.push_back(id);
        }
        CHECK(!sys.set_land_team(1, ids, slots));  // not a team container
        CHECK(sys.set_land_team(100, ids, slots));
        ObjectId tc = -1;
        V2 ts;
        CHECK(sys.team_slot(3, tc, ts) && tc == 100 && ts.y == 35.0f && sys.land_team_slots(100).size() == 3);
        for (ObjectId id : {ObjectId{100}, ObjectId{1}, ObjectId{2}, ObjectId{3}}) CHECK(sys.set_team(id, 100, 1.0f));
        r.team = 2.0f * mult;
        SimRng rng(9);
        MoveOrder o;
        o.units = {2};  // a member's order moves its container
        o.destination = cell_center(35, 20);
        sys.move(o, rng, 100);
        uint32_t first_move = 0;
        for (uint32_t f = 101; f < 400; ++f) {
            sys.tick(f, rng, pf);
            const UnitView k = sys.view(100);
            if (f == 102) r.path = k.has_goal && !sys.view(1).arrived && sys.view(1).has_goal;  // the member reports its team's order
            r.max_off_path = std::max(r.max_off_path, std::fabs(k.pos.y - start.y));
            V2 cen{0.0f, 0.0f};
            for (ObjectId id = 1; id <= 3; ++id) cen = cen + sys.view(id).pos;
            cen = cen * (1.0f / 3.0f);
            if (k.has_goal) r.min_centroid_gap = std::min(r.min_centroid_gap, length(cen - k.pos));
            r.cont_speed = std::max(r.cont_speed, k.speed);
            if (k.speed > 0.0f && first_move == 0) first_move = f;
            if (first_move != 0 && f < first_move + 6) {  // the container is under way; members 1 / 3 still off their slots
                r.lead_speed = std::max(r.lead_speed, sys.view(1).speed);
                r.lag_speed = std::min(r.lag_speed, sys.view(3).speed);
            }
        }
        r.arrived = sys.view(100).arrived && !sys.view(100).has_goal;
        r.member_view = sys.view(1).arrived && sys.view(1).goal == sys.view(100).goal;
        // every member ends near its slot point (recovery leaves it on the point; a member within 4.0 stays)
        const UnitView k = sys.view(100);
        const V2 cs = dir_from_deg(k.yaw);
        r.final_err = 0.0f;
        for (ObjectId id = 1; id <= 3; ++id) {
            const UnitView m = sys.view(id);
            const V2 off = slots[id - 1] + m.jiggle;
            const V2 tgt{k.pos.x + cs.x * off.x - cs.y * off.y, k.pos.y + cs.y * off.x + cs.x * off.y};
            r.final_err = std::max(r.final_err, length(tgt - m.pos));
        }
        // s78n t14 (FIGHTER_FLYING §7.3 item 6): this container has no ContainerArrangement (None), so it is never laid out
        // and a member knocked 30 off its slot while the team is idle does not recover (recovery: test_land_squad_walk_states)
        const V2 m2 = sys.view(2).pos;
        CHECK(sys.teleport(2, {m2.x - 30.0f, m2.y}, 0.0f, 90.0f, 400));
        bool recovering = false;
        for (uint32_t f = 400; f < 520; ++f) {
            sys.tick(f, rng, pf);
            recovering = recovering || sys.view(2).has_goal;
        }
        CHECK((!recovering && sys.view(2).pos == V2{m2.x - 30.0f, m2.y}));
        StateHasher h;
        sys.hash_units(h);
        r.crc = h.digest();
        // removing the container releases its members: their own orders move them again
        CHECK(sys.remove_unit(100) && !sys.team_slot(1, tc, ts));
        MoveOrder own;
        own.units = {1};
        own.destination = cell_center(70, 40);
        sys.move(own, rng, 520);
        for (uint32_t f = 521; f < 530; ++f) sys.tick(f, rng, pf);
        CHECK(sys.view(1).has_goal && sys.view(1).speed > 0.0f);
        return r;
    };
    const Run a = run(serial_for);
    const Run b = run(make_thread_pf(4));
    CHECK(a.crc == b.crc);
    CHECK(a.path && a.arrived && a.member_view);
    CHECK(a.max_off_path < 0.01f);          // the container walks its own straight path, never re-centred on the members
    CHECK(a.min_centroid_gap > 15.0f);      // ... which stay 25 to the side
    // at the team speed (the slowest member's Max_Speed x mult); s78n t16: the straggler cap 0x5ab640 lets it close in on
    // that speed from below while a member that cannot go faster trails it (the cap ends 2 behind the slot box)
    CHECK(a.cont_speed <= a.team && a.cont_speed > 0.9f * a.team);
    CHECK(a.lead_speed > a.team);           // the member behind its slot catches up above the team speed
    CHECK(a.lag_speed < a.team);            // the member ahead of its slot slows down
    CHECK(a.final_err < 25.0f);  // s78n t14: no recovery for a container that is never laid out (ContainerArrangement None)
    std::printf("land squad: off path %.4f, centroid gap >= %.2f, team %.3f, catch-up %.3f, wait %.3f, final slot error %.3f\n",
                static_cast<double>(a.max_off_path), static_cast<double>(a.min_centroid_gap), static_cast<double>(a.team),
                static_cast<double>(a.lead_speed), static_cast<double>(a.lag_speed), static_cast<double>(a.final_err));
}

// s78n t14 (FIGHTER_FLYING §7.2): the reference Dartboard scatter of the spec (radical inverse, rejection 5.0 + r, <= 1000
// tries, sort-matching with the retail grouping quirk), written out independently of land_team.cpp. Returns false when
// fewer than n points were accepted (the port then falls back to Rows).
V2 t14v(float x, float y) { return V2{x, y}; }

float t14_radinv(uint32_t i, uint32_t b) {
    float f = 0.0f, bk = static_cast<float>(b);
    while (i != 0) {
        f += static_cast<float>(i % b) / bk;
        i /= b;
        bk *= static_cast<float>(b);
    }
    return f;
}

bool t14_dartboard(uint32_t n, float hx, float hy, float g, uint32_t seed, float spread, std::vector<V2>& pts) {
    uint32_t k = static_cast<uint32_t>(std::sqrt(static_cast<float>(n)));
    if (k * k != n) ++k;
    const uint32_t m = n / k + (n % k != 0 ? 1u : 0u);
    float r = std::sqrt(hx * hx + hy * hy);
    float L = static_cast<float>(k - 1) * g * 20.0f + static_cast<float>(k) * hx;
    float W = static_cast<float>(m - 1) * g * 20.0f + static_cast<float>(m) * hy;
    L *= spread;
    W *= spread;
    r *= spread;
    pts.clear();
    for (uint32_t t = 0; t < 1000 && pts.size() < n; ++t) {
        const float x = (t14_radinv(seed + t, 2) - 0.5f) * L, y = (t14_radinv(seed + t, 3) - 0.5f) * W;
        bool ok = true;
        for (const V2& p : pts) ok = ok && !(std::sqrt((y - p.y) * (y - p.y) + (x - p.x) * (x - p.x)) < 5.0f + r);
        if (ok) pts.push_back({x, y});
    }
    return pts.size() == n;
}

void t14_squad(MovementSystem& sys, V2 at, int32_t arrangement, const std::vector<T14Member>& mem, float speed) {
    UnitDesc c = unit(100, at, 2, speed, 12.0f, LocomotorKind::Default);
    c.team_container = true;
    c.container_arrangement = arrangement;
    CHECK(sys.add_unit(c));
    std::vector<ObjectId> ids;
    std::vector<V2> slots;
    for (size_t i = 0; i < mem.size(); ++i) {
        UnitDesc d = unit(static_cast<ObjectId>(i + 1), at + mem[i].rel, 2, speed, 3.0f, LocomotorKind::Default);
        d.formation_spacing = mem[i].spacing;
        CHECK(sys.add_unit(d));
        CHECK(sys.set_team_member_shape(d.id, mem[i].hx, mem[i].hy, mem[i].hx, mem[i].hy));
        ids.push_back(d.id);
        slots.push_back(mem[i].rel);
    }
    CHECK(sys.set_land_team(100, ids, slots));
    CHECK(sys.set_team(100, 100, 1.0f));
    for (ObjectId id : ids) CHECK(sys.set_team(id, 100, 1.0f));
}

V2 t14_slot(const MovementSystem& sys, ObjectId id) {
    ObjectId c = -1;
    V2 s{-1.0e9f, -1.0e9f};
    CHECK(sys.team_slot(id, c, s) && c == 100);
    return s;
}

// s78n t14 (FIGHTER_FLYING §7.2): Rows 0x5ac970 and Dartboard 0x5ac2a0 slot geometry, the 0.99 re-lay rule, SPREAD_OUT.
void test_land_squad_layout() {
    const V2 at = cell_center(20, 20);
    {  // Rows: 5 members (k = 3): row 0 = the three front-most, ordered from the most negative side; mixed shapes
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        std::vector<T14Member> mem(5);
        mem[0].rel = {30.0f, 10.0f};
        mem[1].rel = {30.0f, -10.0f};
        mem[2].rel = {0.0f, 5.0f};
        mem[3].rel = {0.0f, -5.0f};
        mem[4].rel = {-30.0f, 0.0f};
        for (T14Member& m : mem) {
            m.hx = 4.0f;
            m.hy = 3.0f;
        }
        mem[4].hx = 6.0f;
        mem[4].hy = 5.0f;
        mem[4].spacing = 1.0f;
        t14_squad(sys, at, 0, mem);
        bool lo = true;
        V2 ld;
        CHECK(sys.land_team_layout(100, lo, ld) && !lo);
        CHECK(sys.lay_out_land_team(100, {2.0f, 0.0f}, 1));  // dir is normalised
        // hand-computed (spec §7.2 Rows): row 0 W = 11 + 16 + 11, row 1 (HX 6, G 20) along -25, D = 35, shift 13.5
        CHECK(t14_slot(sys, 1) == t14v(13.5f, 16.0f));
        CHECK(t14_slot(sys, 2) == t14v(13.5f, -16.0f));
        CHECK(t14_slot(sys, 3) == t14v(13.5f, 0.0f));
        CHECK(t14_slot(sys, 4) == t14v(-11.5f, -12.5f));
        CHECK(t14_slot(sys, 5) == t14v(-11.5f, 10.5f));
        CHECK(sys.land_team_layout(100, lo, ld) && lo && ld == t14v(1.0f, 0.0f));
        const auto team = sys.land_team(100);  // the index the host's joint box reads is refreshed
        CHECK(team.size() == 5 && team[0].slot == t14v(13.5f, 16.0f) && team[4].slot == t14v(-11.5f, 10.5f));
        // 0x5ac120: a turn of 8 degrees keeps the slots (dot > 0.99), 9 degrees re-lays
        const V2 d8 = dir_from_deg(8.0f), d9 = dir_from_deg(9.0f);
        CHECK(!sys.lay_out_land_team(100, d8, 2) && sys.land_team_layout(100, lo, ld) && ld == t14v(1.0f, 0.0f));
        CHECK(sys.lay_out_land_team(100, d9, 3) && sys.land_team_layout(100, lo, ld) && std::fabs(ld.y - d9.y) < 1e-6f);
        // a re-lay with the same dir needs the flag cleared: SPREAD_OUT (x 1.8 spacing) re-lays at the next tick
        CHECK(!sys.lay_out_land_team(100, d9, 4));
        CHECK(sys.set_team_spread_out(100, true) && sys.land_team_layout(100, lo, ld) && !lo);
        SimRng rng(3);
        sys.tick(5, rng, serial_for);
        CHECK(sys.land_team_layout(100, lo, ld) && lo && ld == t14v(1.0f, 0.0f));  // the container's facing (yaw 0)
        // row 0 with g = 10 x 1.8 = 18: W = 15 + 24 + 15, sides -24, 0, 24 (the members did not move: the tick's recovery
        // decision read the cleared flag)
        CHECK(t14_slot(sys, 1).y == 24.0f && t14_slot(sys, 2).y == -24.0f && t14_slot(sys, 3).y == 0.0f);
        // None (-1): never laid out
        MovementSystem none{MovementConfig{}};
        land_system(none, 100);
        t14_squad(none, at, -1, mem);
        CHECK(!none.lay_out_land_team(100, {1.0f, 0.0f}, 1));
        none.tick(1, rng, serial_for);
        CHECK(none.land_team_layout(100, lo, ld) && !lo && t14_slot(none, 1) == mem[0].rel);
        std::printf("land squad layout: Rows 5 members (13.5, 16) (13.5, -16) (13.5, 0) (-11.5, -12.5) (-11.5, 10.5); spread row 0 "
                    "sides +-24\n");
    }
    {  // Dartboard: the Halton points of container id 100 x frame 7, assigned front to back / side to side
        CHECK(team_radical_inverse(0, 2) == 0.0f && team_radical_inverse(1, 2) == 0.5f && team_radical_inverse(2, 2) == 0.25f &&
              team_radical_inverse(3, 2) == 0.75f && team_radical_inverse(1, 3) == 1.0f / 3.0f &&
              team_radical_inverse(4, 3) == 1.0f / 3.0f + 1.0f / 9.0f);
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        std::vector<T14Member> mem(4);
        mem[0].rel = {20.0f, 0.0f};
        mem[1].rel = {0.0f, 20.0f};
        mem[2].rel = {0.0f, -20.0f};
        mem[3].rel = {-20.0f, 0.0f};
        for (T14Member& m : mem) m.spacing = 1.0f;
        t14_squad(sys, at, 1, mem);
        CHECK(sys.lay_out_land_team(100, {1.0f, 0.0f}, 7));
        std::vector<V2> pts;
        CHECK(t14_dartboard(4, 3.0f, 3.0f, 1.0f, 700u, 1.0f, pts));  // L = W = 26, r = sqrt(18)
        // the spec's matching for this case, by hand: points by x descending; members by projection (1 front, 4 back,
        // 2 / 3 tied at 0 -> index order); groups of x within W / m = 13 sorted by side / y
        std::vector<V2> by_x = pts;
        std::stable_sort(by_x.begin(), by_x.end(), [](V2 a, V2 b) { return a.x > b.x; });
        std::vector<ObjectId> order = {1, 2, 3, 4};
        const auto side_of = [&](ObjectId id) { return mem[static_cast<size_t>(id - 1)].rel.y; };
        uint32_t i = 0;
        do {
            uint32_t j = i;
            while (j < 4 && by_x[i].x - by_x[j].x <= 13.0f) ++j;
            std::stable_sort(order.begin() + i, order.begin() + j, [&](ObjectId a, ObjectId b) { return side_of(a) > side_of(b); });
            std::stable_sort(by_x.begin() + i, by_x.begin() + j, [](V2 a, V2 b) { return a.y > b.y; });
            i = j + 1;
        } while (i < 4);
        bool same = true;
        for (size_t q = 0; q < 4; ++q) same = same && t14_slot(sys, order[q]) == by_x[q];
        CHECK(same);
        std::printf("land squad layout: Dartboard id 100 x frame 7 points (%.3f, %.3f) (%.3f, %.3f) (%.3f, %.3f) (%.3f, %.3f), "
                    "matched %d\n",
                    static_cast<double>(pts[0].x), static_cast<double>(pts[0].y), static_cast<double>(pts[1].x),
                    static_cast<double>(pts[1].y), static_cast<double>(pts[2].x), static_cast<double>(pts[2].y),
                    static_cast<double>(pts[3].x), static_cast<double>(pts[3].y), same ? 1 : 0);
        // another frame is another scatter
        CHECK(sys.set_team_spread_out(100, false));
        std::vector<V2> pts2;
        CHECK(t14_dartboard(4, 3.0f, 3.0f, 1.0f, 800u, 1.0f, pts2) && pts2 != pts);
        // too few points (the rejection radius larger than the area): Rows with the same dir
        MovementSystem tight{MovementConfig{}};
        land_system(tight, 100);
        for (T14Member& m : mem) {
            m.spacing = 0.0f;
            m.hx = m.hy = 2.0f;
        }
        t14_squad(tight, at, 1, mem);
        CHECK(!t14_dartboard(4, 2.0f, 2.0f, 0.0f, 700u, 1.0f, pts));
        CHECK(tight.lay_out_land_team(100, {1.0f, 0.0f}, 7));
        // Rows of 2 x 2, g = 0, hx = hy = 2: front row 1 (side 0 -> -2), 2 (side 20 -> +2); back row 3 (-20 -> -2), 4
        CHECK(t14_slot(tight, 1) == t14v(2.0f, -2.0f) && t14_slot(tight, 2) == t14v(2.0f, 2.0f) &&
              t14_slot(tight, 3) == t14v(-2.0f, -2.0f) && t14_slot(tight, 4) == t14v(-2.0f, 2.0f));
    }
}

// s78n t14 (FIGHTER_FLYING §7.2 callers / §7.3): re-lay at the move start and at waypoints, start delays (drawn in team
// index order, counted down while the container turns or moves), the recovery rules, worker-count invariance.
void test_land_squad_walk_states() {
    const V2 at = cell_center(20, 20);
    std::vector<T14Member> mem(4);
    mem[0].rel = {10.0f, 10.0f};
    mem[1].rel = {10.0f, -10.0f};
    mem[2].rel = {-10.0f, 10.0f};
    mem[3].rel = {-10.0f, -10.0f};
    {  // start delays: drawn at the order (speed 0), in team index order; members stand while > 0
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        t14_squad(sys, at, 0, mem);
        SimRng rng(77);
        sys.tick(1, rng, serial_for);  // the first layout (the container's facing)
        SimRng expect = rng;
        uint32_t want[4];
        for (uint32_t& w : want) w = static_cast<uint32_t>(expect.range_int(0, MovementConstants{}.max_move_frame_delay));
        MoveOrder o;
        o.units = {3};
        o.destination = cell_center(20, 60);  // 90 degrees left: the container turns in place first
        sys.move(o, rng, 2);
        bool drawn = true;
        for (ObjectId id = 1; id <= 4; ++id) drawn = drawn && sys.team_start_delay(id) == want[id - 1];
        CHECK(drawn && rng.state() == expect.state());
        V2 p0[4];
        for (ObjectId id = 1; id <= 4; ++id) p0[id - 1] = sys.view(id).pos;
        bool stood = true, counted = true, relaid_at_start = false;
        uint32_t left[4] = {want[0], want[1], want[2], want[3]};
        uint32_t go = 0;  // the container's own order stagger (OI tactical; set when the order is composed at the tick)
        for (uint32_t f = 2; f < 130; ++f) {
            sys.tick(f, rng, serial_for);
            if (go == 0) go = sys.view(100).start_frame;
            for (ObjectId id = 1; id <= 4; ++id) {
                const uint32_t d = sys.team_start_delay(id);
                if (f < go) counted = counted && d == want[id - 1];  // not counted down while the container waits
                counted = counted && (d == left[id - 1] || d + 1 == left[id - 1]);  // at most one per tick, never up
                if (f >= go && d > 0) stood = stood && sys.view(id).pos == p0[id - 1];  // stands while it counts
                p0[id - 1] = sys.view(id).pos;
                left[id - 1] = d;
            }
            bool lo = false;
            V2 ld;
            if (sys.land_team_layout(100, lo, ld) && lo && std::fabs(ld.y - 1.0f) < 1e-5f) relaid_at_start = true;
        }
        counted = counted && left[0] == 0 && left[1] == 0 && left[2] == 0 && left[3] == 0;
        CHECK(go > 2 && go < 100 && stood && counted && relaid_at_start);
        // a second order while the container moves: no draw
        CHECK(sys.view(100).speed > 0.0f);
        const uint32_t st = rng.state();
        o.destination = cell_center(30, 60);
        sys.move(o, rng, 130);
        CHECK(rng.state() == st);
        std::printf("land squad walk: start delays %u %u %u %u (team index order), stood while counting %d\n", want[0], want[1],
                    want[2], want[3], stood ? 1 : 0);
    }
    {  // recovery rules (0x62bbf0)
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        t14_squad(sys, at, 0, mem);
        SimRng rng(5);
        for (uint32_t f = 1; f < 60; ++f) sys.tick(f, rng, serial_for);  // laid out; everyone on its slot
        const V2 s1 = t14_slot(sys, 1);
        const V2 home{at.x + s1.x, at.y + s1.y};  // yaw 0, no jiggle (the team never moved)
        CHECK(length(sys.view(1).pos - home) < 4.0f && !sys.view(1).has_goal);
        // 30 behind, facing away: turn first (speed 0), then walk
        CHECK(sys.teleport(1, {home.x - 30.0f, home.y}, 0.0f, 180.0f, 60));
        sys.tick(60, rng, serial_for);
        const UnitView t1 = sys.view(1);
        CHECK(t1.has_goal && t1.speed == 0.0f && t1.pos == t14v(home.x - 30.0f, home.y) && std::fabs(t1.yaw) < 180.0f &&
              std::fabs(t1.yaw) > 160.0f);
        // facing it within 0.5 x rate: walk at once
        CHECK(sys.teleport(1, {home.x - 30.0f, home.y}, 0.0f, 1.0f, 61));
        sys.tick(61, rng, serial_for);
        CHECK(sys.view(1).speed > 0.0f && sys.view(1).pos.x > home.x - 30.0f);
        uint32_t f = 62;
        for (; f < 200 && sys.view(1).has_goal; ++f) sys.tick(f, rng, serial_for);
        CHECK(!sys.view(1).has_goal && length(sys.view(1).pos - home) < 4.0f && sys.view(1).speed == 0.0f);
        // a container with a target: no recovery
        CHECK(sys.set_team_target(100, true));
        CHECK(sys.teleport(1, {home.x - 30.0f, home.y}, 0.0f, 0.0f, f));
        for (uint32_t g = f; g < f + 20; ++g) sys.tick(g, rng, serial_for);
        CHECK(!sys.view(1).has_goal && sys.view(1).pos == t14v(home.x - 30.0f, home.y));
        CHECK(sys.set_team_target(100, false));
        sys.tick(f + 20, rng, serial_for);
        CHECK(sys.view(1).has_goal);
        std::printf("land squad walk: recovery turn first, walk when facing, none with a target; settled at %u\n", f);
    }
    {  // a Dartboard squad walking a path with corners: re-lays at waypoints; worker-count invariant
        const auto run = [&](const ParallelFor& pf, uint32_t& relays) {
            MovementSystem sys{MovementConfig{}};
            PassabilityGrid g = open_grid(100, 100);
            for (int32_t y = 10; y <= 40; ++y) g.set_base(30, y, kPassImpassable);  // a wall: the path bends around it
            sys.set_grid(std::move(g));
            sys.set_anim_length_fn(anim_len);
            std::vector<T14Member> m9(9);
            for (size_t i = 0; i < 9; ++i) {
                m9[i].rel = {static_cast<float>(static_cast<int32_t>(i % 3) - 1) * 12.0f, static_cast<float>(static_cast<int32_t>(i / 3) - 1) * 12.0f};
                m9[i].spacing = 0.6f;
            }
            t14_squad(sys, at, 1, m9);
            SimRng rng(11);
            MoveOrder o;
            o.units = {5};
            o.destination = cell_center(45, 25);
            sys.move(o, rng, 1);
            V2 last{};
            relays = 0;
            for (uint32_t f = 1; f < 900; ++f) {
                sys.tick(f, rng, pf);
                bool lo = false;
                V2 ld;
                if (sys.land_team_layout(100, lo, ld) && lo && !(ld == last)) {
                    ++relays;
                    last = ld;
                }
            }
            CHECK(sys.view(100).arrived);
            StateHasher h;
            sys.hash_units(h);
            return h.digest();
        };
        uint32_t ra = 0, rb = 0;
        const uint64_t a = run(serial_for, ra), b = run(make_thread_pf(4), rb);
        CHECK(a == b && ra == rb && ra >= 3);  // the first layout, the move start, >= 1 corner
        std::printf("land squad walk: Dartboard march re-laid %u times, serial = 4 workers %d\n", ra, a == b ? 1 : 0);
    }
}

// s78n review (FIGHTER_FLYING §7, MOVEMENT §3.7):
//  * a unit crossing a marching squad takes no collision object with a member (a member is neither a stopped obstacle
//    nor pathing: before, the moving checker treated it as stopped - MovingVsStopped, path ends, nudges) and arrives;
//  * a member's step has no passability test (FIGHTER_FLYING §7.3 "Recovery in full" item 7): a member whose slot point
//    lies in impassable cells walks onto it and stands there (before: a [port] wall test and a give-up);
//  * park_team_member / rejoin_land_team (the garrison entry and leave) keep the member's container, slot and team index.
void test_land_squad_review() {
    const auto squad = [](MovementSystem& sys, V2 at, const V2 (&slots)[3], const V2 (&starts)[3], int32_t arrangement = -1) {
        UnitDesc c = unit(100, at, 2, 2.0f, 12.0f, LocomotorKind::Default);
        c.team_container = true;
        c.container_arrangement = arrangement;  // s78n t14
        CHECK(sys.add_unit(c));
        std::vector<ObjectId> ids;
        for (ObjectId id = 1; id <= 3; ++id) {
            CHECK(sys.add_unit(unit(id, starts[id - 1], 2, 2.0f, 6.0f, LocomotorKind::Default)));
            ids.push_back(id);
        }
        CHECK(sys.set_land_team(100, ids, slots));
        for (ObjectId id : {ObjectId{100}, ObjectId{1}, ObjectId{2}, ObjectId{3}}) CHECK(sys.set_team(id, 100, 1.0f));
    };
    {  // a slow unit crossing the squad's line while the squad marches through
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        const V2 at = cell_center(10, 20);
        const V2 slots[3] = {{0.0f, 15.0f}, {0.0f, 0.0f}, {0.0f, -15.0f}};
        const V2 starts[3] = {at + slots[0], at + slots[1], at + slots[2]};
        squad(sys, at, slots, starts);
        UnitDesc x = unit(50, cell_center(30, 16), 2, 0.4f, 6.0f, LocomotorKind::Default);
        CHECK(sys.add_unit(x));
        SimRng rng(5);
        MoveOrder o;
        o.units = {2};
        o.destination = cell_center(50, 20);
        sys.move(o, rng, 1);
        MoveOrder xo;
        xo.units = {50};
        xo.destination = cell_center(30, 24);
        sys.move(xo, rng, 1);
        uint32_t member_collisions = 0, crossings = 0;
        for (uint32_t f = 2; f < 700; ++f) {
            sys.tick(f, rng, serial_for);
            for (const CollisionObject& co : sys.collisions().list())
                if ((co.a >= 1 && co.a <= 3) || (co.b >= 1 && co.b <= 3)) ++member_collisions;
            const UnitView k = sys.view(100);
            if (k.has_goal && std::fabs(k.pos.x - sys.view(50).pos.x) < 20.0f) ++crossings;
        }
        const UnitView xv = sys.view(50);
        CHECK(crossings > 0);  // the squad passed the crossing unit while it walked
        CHECK(member_collisions == 0);
        CHECK(xv.arrived && !xv.failed && length(xv.pos - cell_center(30, 24)) < 1.0f);
        CHECK(sys.view(100).arrived);
        std::printf("land squad review: crossing frames %u, member collisions %u, crosser arrived %d\n", crossings,
                    member_collisions, xv.arrived ? 1 : 0);
    }
    {  // a slot in a wall: the recovery walks straight at it, through the impassable cells, and ends on it
        MovementSystem sys{MovementConfig{}};
        // s78n t14: a Rows container (recovery needs the laid-out flag): members 2 / 3 in front (+x), member 1 the back-most,
        // alone in the back row; half-extents 20 -> member 1's slot (-20, 0), its slot point (180, 400) in the wall
        PassabilityGrid g2 = open_grid(100, 100);
        for (int32_t y = 19; y <= 21; ++y)
            for (int32_t x = 7; x <= 9; ++x) g2.set_base(x, y, kPassImpassable);
        sys.set_grid(std::move(g2));
        sys.set_anim_length_fn(anim_len);
        const V2 at = cell_center(10, 20);
        const V2 slots[3] = {{-100.0f, 0.0f}, {20.0f, -20.0f}, {20.0f, 20.0f}};
        const V2 starts[3] = {at + slots[0], at + slots[1], at + slots[2]};
        squad(sys, at, slots, starts, 0);
        for (ObjectId id = 1; id <= 3; ++id) CHECK(sys.set_team_member_shape(id, 20.0f, 20.0f, 20.0f, 20.0f));
        const uint32_t mask = MovementConfig{}.classes.mask(2);
        SimRng rng(6);
        uint32_t goal_frames = 0, wall_frames = 0;
        for (uint32_t f = 1; f < 200; ++f) {
            sys.tick(f, rng, serial_for);
            if (sys.view(1).has_goal) ++goal_frames;
            if (!sys.grid().passable_world(sys.view(1).pos, mask)) ++wall_frames;
        }
        ObjectId tc1 = -1;
        V2 ts1;
        CHECK((sys.team_slot(1, tc1, ts1) && ts1 == V2{-20.0f, 0.0f} && sys.team_slot(2, tc1, ts1) && ts1 == V2{20.0f, -20.0f}));
        const V2 sp0{at.x - 20.0f, at.y};
        CHECK(goal_frames > 10 && wall_frames > 0);  // it walked, on impassable cells too
        CHECK((!sys.view(100).has_goal && !sys.view(1).has_goal && length(sys.view(1).pos - sp0) <= 4.0f));  // and is on its slot
        CHECK(!sys.grid().passable_world(sp0, mask));
        // the container is teleported to open ground: s78n t18 (0x3abb80) the member goes with it, at its offset - it
        // stands on its slot there and has nothing to recover
        const V2 there = cell_center(20, 30);
        const V2 off1 = sys.view(1).pos - sys.view(100).pos;
        CHECK(sys.teleport(100, there, 0.0f, 0.0f, 200));
        CHECK(sys.view(1).pos == there + off1);
        bool again = false;
        for (uint32_t f = 200; f < 700; ++f) {
            sys.tick(f, rng, serial_for);
            again = again || sys.view(1).has_goal;
        }
        const V2 sp{there.x - 20.0f, there.y};  // slot (-20, 0) at yaw 0, no jiggle (the team never moved)
        CHECK(!again && !sys.view(1).has_goal && length(sys.view(1).pos - sp) <= 4.0f);
        std::printf("land squad review: slot in a wall - recovering %u frames, on impassable cells %u frames\n", goal_frames,
                    wall_frames);
    }
    {  // park / rejoin (garrison entry and leave)
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        const V2 at = cell_center(10, 20);
        const V2 slots[3] = {{0.0f, 15.0f}, {0.0f, 0.0f}, {0.0f, -15.0f}};
        const V2 starts[3] = {at + slots[0], at + slots[1], at + slots[2]};
        squad(sys, at, slots, starts);
        CHECK(sys.is_land_team_container(100) && !sys.is_land_team_container(2));
        CHECK(!sys.park_team_member(100) && sys.has_unit(100));  // not a steering member: nothing done
        CHECK(sys.park_team_member(2) && !sys.has_unit(2) && sys.team_member_parked(2));
        CHECK(sys.land_team(100).size() == 2 && sys.land_team(100)[0].member == 1 && sys.land_team(100)[1].member == 3);
        CHECK(!sys.rejoin_land_team(2));  // not added again yet: the record is dropped
        CHECK(sys.park_team_member(1) && !sys.team_member_parked(2));
        CHECK(sys.add_unit(unit(1, cell_center(14, 24), 2, 2.0f, 6.0f, LocomotorKind::Default)));
        CHECK(sys.rejoin_land_team(1) && !sys.team_member_parked(1));
        ObjectId tc = -1;
        V2 ts;
        CHECK(sys.team_slot(1, tc, ts) && tc == 100 && ts == slots[0]);
        const auto team = sys.land_team(100);
        CHECK(team.size() == 2 && team[0].member == 1 && team[0].tindex == 0 && team[1].member == 3 && team[1].tindex == 2);
        // an order on the rejoined member moves the squad
        SimRng rng(7);
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(30, 20);
        sys.move(o, rng, 1);
        CHECK(sys.view(100).has_goal && sys.view(1).has_goal);
        // a parked member removed for good (it died inside) and a removed container drop their records
        CHECK(sys.park_team_member(3) && sys.team_member_parked(3) && !sys.remove_unit(3) && !sys.team_member_parked(3));
        CHECK(sys.park_team_member(1) && sys.remove_unit(100) && !sys.team_member_parked(1) && sys.land_team(100).empty());
    }
    {  // s78n t06 review: suspend / rejoin (a squad-member hero in a clash): orders act on the member itself meanwhile
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        const V2 at = cell_center(10, 20);
        const V2 slots[3] = {{0.0f, 15.0f}, {0.0f, 0.0f}, {0.0f, -15.0f}};
        const V2 starts[3] = {at + slots[0], at + slots[1], at + slots[2]};
        squad(sys, at, slots, starts, 0);  // s78n t14: Rows (the recovery below needs a laid-out container)
        ObjectId tc = -1;
        V2 ts;
        CHECK(sys.team_slot(2, tc, ts) && tc == 100);
        CHECK(!sys.suspend_team_member(100));  // not a steering member: nothing done
        CHECK(sys.suspend_team_member(2) && sys.has_unit(2) && sys.team_member_parked(2));
        CHECK(!sys.team_slot(2, tc, ts) && sys.land_team(100).size() == 2);
        SimRng rng(8);
        MoveOrder o;
        o.units = {2};
        o.destination = cell_center(10, 40);
        sys.move(o, rng, 1);
        CHECK(sys.view(2).has_goal && !sys.view(100).has_goal);  // the member walks, its squad stays
        for (uint32_t f = 2; f < 400; ++f) sys.tick(f, rng, serial_for);
        CHECK(sys.view(2).arrived && length(sys.view(2).pos - cell_center(10, 40)) < 2.0f && length(sys.view(100).pos - at) < 0.5f);
        const ObjectId stop_ids[1] = {2};
        sys.stop(stop_ids);
        CHECK(!sys.view(100).has_goal);
        CHECK(sys.rejoin_land_team(2) && !sys.team_member_parked(2));
        CHECK(sys.team_slot(2, tc, ts) && tc == 100 && ts == slots[1] && sys.land_team(100).size() == 3);
        for (uint32_t f = 400; f < 900; ++f) sys.tick(f, rng, serial_for);
        CHECK(length(sys.view(2).pos - (at + slots[1])) <= 4.0f);  // recovered to its slot
        std::printf("land squad review: suspended member walked alone and recovered to its slot\n");
    }
}

void test_determinism() {
    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    const CrcLog a = mixed_run(serial_for);
    const CrcLog b = mixed_run(make_thread_pf(4));
    const CrcLog c = mixed_run(make_thread_pf(hw));
    CHECK(!a.records.empty() && !first_divergence(a, b).diverged && !first_divergence(a, c).diverged);
    const uint64_t g = a.records.back().combined;
    std::printf("determinism: mixed %zu ticks final %016llx (1/4/%u threads identical)\n", a.records.size(),
                static_cast<unsigned long long>(g), hw);
    if (kGoldenMixed != 0) CHECK(g == kGoldenMixed);
}

// S8 t11 / t12 (MOVEMENT §3.2 "Deploy states", ABILITIES_HARDPOINTS §4.13): the Walk deploy window. Clip 0x19 / 0x1A
// with 30 frames at 30 fps: step = 1 x Deployment_Anim_Rate, end at position >= 30 - 2 - step.
void test_deploy() {
    using DR = MovementSystem::DeployResult;
    MovementSystem sys{MovementConfig{}};
    land_system(sys, 100);
    CHECK(sys.add_unit(trooper(1, cell_center(10, 10))));
    UnitDesc fast = trooper(2, cell_center(10, 30));
    fast.deployment_anim_rate = 2.0f;
    CHECK(sys.add_unit(fast));
    CHECK(sys.add_unit(unit(3, cell_center(10, 50), 0, 2.0f, 6.0f, LocomotorKind::Jetpack)));
    sys.set_deploy_clip_fn([](ObjectId, int32_t anim, uint32_t& n, float& fps) {
        if (anim != 0x19 && anim != 0x1A) return false;
        n = 30;
        fps = 30.0f;
        return true;
    });
    bool turret_rest = true;
    sys.set_turret_rest_fn([&turret_rest](ObjectId id) { return id != 1 || turret_rest; });
    SimRng rng(7);
    auto finished_at = [&](ObjectId id, LocoState to, uint32_t from, uint32_t limit) {
        for (uint32_t f = from; f < limit; ++f) {
            sys.tick(f, rng);
            for (const MovementSystem::DeployEvent& e : sys.take_deploy_events())
                if (e.id == id && e.to == to && e.finished) return f;
        }
        return 0xFFFFFFFFu;
    };
    // Only Walk deploys; unknown units are NoWalk too.
    CHECK(sys.deploy(3, true, 0) == DR::NoWalk && sys.deploy(99, true, 0) == DR::NoWalk && sys.deploy(1, false, 0) == DR::Ignored);
    // A moving unit stops; Deploy(true) at frame 10 -> 0x18 now, 0x19 when the clip ends (10 + 27 at rate 1).
    MoveOrder o;
    o.units = {1, 2};
    o.destination = cell_center(60, 10);
    sys.move(o, rng, 0);
    for (uint32_t f = 0; f < 10; ++f) sys.tick(f, rng);
    CHECK(sys.deploy(1, true, 10) == DR::Deploying && sys.deploy(2, true, 10) == DR::Deploying);
    CHECK(sys.deploy(1, true, 10) == DR::Ignored);  // nothing in 0x18
    {
        const std::vector<MovementSystem::DeployEvent> ev = sys.take_deploy_events();
        CHECK(ev.size() == 2 && ev[0].id == 1 && ev[0].from == LocoState::None && ev[0].to == LocoState::WalkDeploying &&
              !ev[0].finished);
    }
    CHECK(sys.deploy_state(1) == LocoState::WalkDeploying && sys.view(1).deploy_state == LocoState::WalkDeploying);
    const V2 held = sys.view(1).pos;
    uint32_t f2 = 0xFFFFFFFFu, f1 = 0xFFFFFFFFu;
    for (uint32_t f = 10; f < 80 && f1 == 0xFFFFFFFFu; ++f) {
        sys.tick(f, rng);
        for (const MovementSystem::DeployEvent& e : sys.take_deploy_events()) {
            if (e.finished && e.to == LocoState::WalkDeployed && e.id == 1) f1 = f;
            if (e.finished && e.to == LocoState::WalkDeployed && e.id == 2) f2 = f;
        }
        CHECK(sys.view(1).pos == held);  // a deploying unit does not move
    }
    CHECK(f1 == 37);  // pos = f - 10 >= 27
    CHECK(f2 == 23);  // rate 2: 2 (f - 10) >= 26
    CHECK(sys.deploy_state(1) == LocoState::WalkDeployed && sys.deploy(1, true, 40) == DR::Ignored);
    // An order while deployed waits: the unit stays put.
    sys.move(o, rng, 40);
    for (uint32_t f = 40; f < 60; ++f) sys.tick(f, rng);
    CHECK(sys.view(1).pos == held);
    // Undeploy: 0x1A, the clip from 0, back to the walk 27 frames later (state None, signal).
    CHECK(sys.deploy(1, false, 60) == DR::Undeploying && sys.deploy_state(1) == LocoState::WalkUndeploying);
    const uint32_t undeployed = finished_at(1, LocoState::None, 60, 200);
    CHECK(undeployed == 87 && sys.deploy_state(1) == LocoState::None);

    // S8 t24 (MOVEMENT §3.2 "Where it gates"): Deploy(true) from the walk starts its clip at once, the turret off rest or
    // not (frame 100 -> ends 127).
    turret_rest = false;
    CHECK(sys.deploy(1, true, 100) == DR::Deploying);
    CHECK(finished_at(1, LocoState::WalkDeployed, 100, 300) == 127);
    // Blocked reverse: 0x1A with the turret off rest (no clip yet); Deploy(true) -> 0x19 at once with the signal.
    turret_rest = false;
    CHECK(sys.deploy(1, false, 150) == DR::Undeploying);
    sys.tick(150, rng);
    (void)sys.take_deploy_events();
    CHECK(sys.deploy(1, true, 151) == DR::ReverseBlocked && sys.deploy_state(1) == LocoState::WalkDeployed);
    {
        const std::vector<MovementSystem::DeployEvent> ev = sys.take_deploy_events();
        CHECK(ev.size() == 1 && ev[0].from == LocoState::WalkUndeploying && ev[0].to == LocoState::WalkDeployed && ev[0].finished);
    }
    turret_rest = true;
    CHECK(finished_at(1, LocoState::None, 160, 200) == 0xFFFFFFFFu);  // nothing pending
    CHECK(sys.deploy(1, false, 200) == DR::Undeploying);
    CHECK(finished_at(1, LocoState::None, 200, 300) == 227);
    // Reversal: Undeploy (frame 250) out of a Deploy clip at position 10 starts the Undeploy clip at 30 - 10 - 1 - 1 = 18:
    // it ends at 27, 9 frames later.
    CHECK(sys.deploy(1, true, 240) == DR::Deploying);
    for (uint32_t f = 240; f < 250; ++f) sys.tick(f, rng);
    CHECK(sys.deploy(1, false, 250) == DR::Undeploying);
    CHECK(finished_at(1, LocoState::None, 250, 300) == 259);

    // Save round trip mid-clip: identical continuation.
    CHECK(sys.deploy(1, true, 300) == DR::Deploying);
    for (uint32_t f = 300; f < 305; ++f) sys.tick(f, rng);
    ByteWriter w;
    sys.write(w);
    MovementSystem copy{MovementConfig{}};
    land_system(copy, 100);
    copy.set_deploy_clip_fn([](ObjectId, int32_t, uint32_t& n, float& fps) {
        n = 30;
        fps = 30.0f;
        return true;
    });
    {
        ByteReader r(w.bytes());
        CHECK(copy.read(r) && r.done());
    }
    StateHasher ha, hb;
    sys.hash_units(ha);
    copy.hash_units(hb);
    CHECK(ha.digest() == hb.digest() && copy.deploy_state(1) == LocoState::WalkDeploying);
    SimRng r2 = rng;
    uint32_t end_a = 0, end_b = 0;
    for (uint32_t f = 305; f < 340; ++f) {
        sys.tick(f, rng);
        copy.tick(f, r2);
        for (const MovementSystem::DeployEvent& e : sys.take_deploy_events())
            if (e.id == 1 && e.finished) end_a = f;
        for (const MovementSystem::DeployEvent& e : copy.take_deploy_events())
            if (e.id == 1 && e.finished) end_b = f;
    }
    CHECK(end_a == 327 && end_b == 327);

    // S8 t12 (e): a locomotor swap undeploys without a signal; JetPack cannot deploy; the Walk rebuilt on landing starts
    // undeployed.
    (void)sys.take_deploy_events();
    CHECK(sys.deploy_state(1) == LocoState::WalkDeployed && sys.use_secondary_locomotor(1, true));
    {
        const std::vector<MovementSystem::DeployEvent> ev = sys.take_deploy_events();
        CHECK(ev.size() == 1 && ev[0].from == LocoState::WalkDeployed && ev[0].to == LocoState::None && !ev[0].finished);
    }
    CHECK(sys.deploy_state(1) == LocoState::None && sys.deploy(1, true, 400) == DR::NoWalk);
    CHECK(sys.use_secondary_locomotor(1, false) && sys.deploy_state(1) == LocoState::None);
    CHECK(sys.deploy(1, true, 401) == DR::Deploying);
    // snap_yaw: the facing at once, position kept.
    const V2 p = sys.view(2).pos;
    CHECK(sys.snap_yaw(2, 90.0f) && near(sys.view(2).yaw, 90.0f, 1e-4f) && sys.view(2).pos == p && !sys.snap_yaw(99, 0.0f));
    std::printf("deploy: window timing, turret wait, blocked reverse, reversal, save, locomotor swap ok\n");
}

// S8 t24 (MOVEMENT §3.2 "Turret recentre around deploy"): the TURRET model of Deploys units. Unit 1 AT-AT-like (3 deg
// per frame, +-55), unit 2 SPMAT-like (3 deg per frame, 360, deployed rest elevation -45). Clips 30 frames at 30 fps.
void test_turret() {
    using DR = MovementSystem::DeployResult;
    MovementSystem sys{MovementConfig{}};
    land_system(sys, 100);
    UnitDesc a = trooper(1, cell_center(10, 10));
    a.turret = true;
    a.turret_speed = 3.0f;
    a.turret_extent = a.turret_deployed_extent = 55.0f;
    UnitDesc s = trooper(2, cell_center(10, 40));
    s.turret = true;
    s.turret_speed = 3.0f;
    s.turret_extent = s.turret_deployed_extent = 360.0f;
    s.turret_deployed_rest[1] = -45.0f;
    CHECK(sys.add_unit(a) && sys.add_unit(s));
    sys.set_deploy_clip_fn([](ObjectId, int32_t, uint32_t& n, float& fps) {
        n = 30;
        fps = 30.0f;
        return true;
    });
    bool aim = false;
    const V2 p1 = sys.view(1).pos;
    sys.set_turret_aim_fn([&](ObjectId id, float& x, float& y, float& z) {
        if (id != 1 || !aim) return false;
        x = p1.x;
        y = p1.y + 100.0f;  // 90 degrees left of the body (yaw 0)
        z = 0.0f;
        return true;
    });
    SimRng rng(3);
    uint32_t f = 0;
    auto run_to = [&](uint32_t end) {
        for (; f < end; ++f) sys.tick(f, rng);
    };
    // Aiming: 3 degrees per frame, clamped to the 55-degree extent.
    aim = true;
    run_to(10);
    CHECK(near(sys.view(1).turret_yaw, 30.0f, 1e-4f));
    run_to(20);
    CHECK(sys.view(1).turret_yaw == 55.0f && sys.view(2).turret_yaw == 0.0f);
    // Idle: held for Turret_Rotate_To_Rest_Delay_Sec (2 s = 60 frames), then back to rest at 3 per frame.
    aim = false;
    run_to(79);
    CHECK(sys.view(1).turret_yaw == 55.0f);
    run_to(90);
    CHECK(sys.view(1).turret_yaw < 55.0f && sys.view(1).turret_yaw > 0.0f);
    run_to(110);
    CHECK(sys.view(1).turret_yaw == 0.0f);
    // Undeploy waits for the rest angle: aim while deployed (yaw 55), then Deploy(false) - the forced recentre skips the
    // delay (one held frame, then 19 frames back), the Undeploy clip starts only at rest and ends 27 frames later.
    CHECK(sys.deploy(1, true, f) == DR::Deploying);
    run_to(f + 40);
    CHECK(sys.deploy_state(1) == LocoState::WalkDeployed);
    aim = true;
    run_to(f + 20);
    CHECK(sys.view(1).turret_yaw == 55.0f);
    aim = false;
    (void)sys.take_deploy_events();
    const uint32_t u0 = f;
    CHECK(sys.deploy(1, false, u0) == DR::Undeploying);
    uint32_t clip_at = 0, done_at = 0;
    for (; f < u0 + 120 && done_at == 0; ++f) {
        sys.tick(f, rng);
        if (clip_at == 0 && sys.view(1).deploy_clip == 0x1A) {
            clip_at = f;
            CHECK(sys.view(1).turret_yaw == 0.0f);  // at rest when the clip starts
        }
        for (const MovementSystem::DeployEvent& e : sys.take_deploy_events())
            if (e.id == 1 && e.to == LocoState::None && e.finished) done_at = f;
    }
    CHECK(clip_at >= u0 + 19 && clip_at <= u0 + 21 && done_at == clip_at + 27);
    // Reversing out of 0x1A while off rest: straight to 0x19 with the signal.
    aim = true;
    run_to(f + 20);  // walk state: yaw 55 again
    CHECK(sys.deploy(1, true, f) == DR::Deploying);
    run_to(f + 40);
    CHECK(sys.deploy(1, false, f) == DR::Undeploying);
    aim = false;
    sys.tick(f, rng);
    ++f;
    CHECK(!near(sys.view(1).turret_yaw, 0.0f, 1e-4f) && sys.deploy(1, true, f) == DR::ReverseBlocked);
    CHECK(sys.deploy_state(1) == LocoState::WalkDeployed);
    // SPMAT: deployed, the idle return goes to the deployed rest (elevation -45); the undeploy recentres to the
    // undeployed rest (0) before its clip.
    CHECK(sys.deploy(2, true, f) == DR::Deploying);
    run_to(f + 120);
    CHECK(sys.deploy_state(2) == LocoState::WalkDeployed && sys.view(2).turret_elevation == -45.0f);
    CHECK(sys.deploy(2, false, f) == DR::Undeploying);
    (void)sys.take_deploy_events();
    const uint32_t s0 = f;
    uint32_t s_done = 0;
    for (; f < s0 + 120 && s_done == 0; ++f) {
        sys.tick(f, rng);
        for (const MovementSystem::DeployEvent& e : sys.take_deploy_events())
            if (e.id == 2 && e.to == LocoState::None && e.finished) s_done = f;
    }
    CHECK(s_done >= s0 + 15 + 27 && s_done <= s0 + 17 + 27 && sys.view(2).turret_elevation == 0.0f);
    // Save round trip with the turret mid-turn: identical continuation.
    aim = true;
    run_to(f + 5);
    ByteWriter w;
    sys.write(w);
    MovementSystem copy{MovementConfig{}};
    land_system(copy, 100);
    {
        ByteReader r(w.bytes());
        CHECK(copy.read(r) && r.done());
    }
    StateHasher ha, hb;
    sys.hash_units(ha);
    copy.hash_units(hb);
    CHECK(ha.digest() == hb.digest() && copy.view(1).turret_yaw == sys.view(1).turret_yaw);
    std::printf("turret: aim + extent, idle hold + return, undeploy wait %u frames, blocked reverse, SPMAT deployed rest, save ok\n",
                clip_at - u0);
}

// s78n t14 part B (FIGHTER_FLYING §7.1 / §7.5): a Rows squad - container 100 (Is_Squashable, Infantry, radius 12,
// MovementPredictionInterval 4) and members 1..4 at (+-10, +-10) - of `owner` at `at`, facing +x.
void t14b_squad(MovementSystem& sys, V2 at, int32_t owner, bool avoidance_disabled) {
    UnitDesc c = unit(100, at, 2, 2.0f, 12.0f, LocomotorKind::Default);
    c.team_container = true;
    c.container_arrangement = 0;
    c.squashable = true;
    c.owner = owner;
    c.movement_prediction_interval = 4.0f;
    CHECK(sys.add_unit(c));
    const V2 rel[4] = {{10.0f, 10.0f}, {10.0f, -10.0f}, {-10.0f, 10.0f}, {-10.0f, -10.0f}};
    std::vector<ObjectId> ids;
    std::vector<V2> slots;
    for (ObjectId id = 1; id <= 4; ++id) {
        UnitDesc d = unit(id, at + rel[id - 1], 2, 2.0f, 3.0f, LocomotorKind::Default);
        d.owner = owner;
        d.formation_spacing = 0.5f;
        d.avoidance_disabled = avoidance_disabled;
        CHECK(sys.add_unit(d));
        CHECK(sys.set_team_member_shape(id, 3.0f, 3.0f, 3.0f, 3.0f));
        ids.push_back(id);
        slots.push_back(rel[id - 1]);
    }
    CHECK(sys.set_land_team(100, ids, slots));
    for (ObjectId id : {ObjectId{100}, ObjectId{1}, ObjectId{2}, ObjectId{3}, ObjectId{4}}) CHECK(sys.set_team(id, 100, 1.0f));
}

// The world point of member `id`'s slot (container position + R(yaw) (slot + jiggle)).
V2 t14b_slot_point(const MovementSystem& sys, ObjectId id) {
    ObjectId c = -1;
    V2 s;
    if (!sys.team_slot(id, c, s)) return V2{-1.0e9f, -1.0e9f};
    const UnitView k = sys.view(c), m = sys.view(id);
    const V2 cs = dir_from_deg(k.yaw);
    const V2 off = s + m.jiggle;
    return {k.pos.x + cs.x * off.x - cs.y * off.y, k.pos.y + cs.y * off.x + cs.x * off.y};
}

// s78n t14 part B (FIGHTER_FLYING §7.1 / §7.5, MOVEMENT §5.4):
//  * the squad container is the collision checker / partner: a Giant vehicle marching through a stopped squad takes its
//    collision object with the container and pushes the container aside; no member is ever a participant or pushed;
//  * a crusher (SQUASH) approaching a stopped hostile squad pushes its container on a crush-flee path: the squad flees with
//    Fleeing_Infantry_Speed_Bonus (2) on the container's team speed and the members' max speed, then stops fleeing when
//    the push path ends and the members recover to their slots; worker-count invariant;
//  * the member steering vector 0x509810: a stopped unit put right beside a marching squad gives the near member a side
//    step away from it; members stay near their slots on a curved march; worker-count invariant.
void test_land_squad_part_b() {
    const V2 at = cell_center(30, 20);
    {  // A: a Giant vehicle vs a stopped squad
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        t14b_squad(sys, at, 1);
        SimRng rng(21);
        for (uint32_t f = 1; f < 20; ++f) sys.tick(f, rng, serial_for);
        UnitDesc g = unit(50, cell_center(10, 20), 2, 1.0f, 20.0f, LocomotorKind::Default);
        g.collision_class = kUccGiantVehicle;
        g.owner = 2;
        CHECK(sys.add_unit(g));
        MoveOrder o;
        o.units = {50};
        o.destination = cell_center(50, 20);
        sys.move(o, rng, 20);
        // (the Giant's MovingVsStopped object with the container is resolved by the push in the pass that creates it, so
        // the live list rarely shows it; the push is the evidence)
        uint32_t member_objects = 0;
        bool cont_pushed = false, member_pushed = false, fled = false;
        for (uint32_t f = 20; f < 900; ++f) {
            sys.tick(f, rng, serial_for);
            for (const CollisionObject& co : sys.collisions().list())
                if ((co.a >= 1 && co.a <= 4) || (co.b >= 1 && co.b <= 4)) ++member_objects;
            cont_pushed = cont_pushed || sys.view(100).pushed;
            for (ObjectId id = 1; id <= 4; ++id) member_pushed = member_pushed || sys.view(id).pushed;
            fled = fled || sys.team_fleeing(100);
        }
        const UnitView gv = sys.view(50);
        CHECK(cont_pushed && !member_pushed && member_objects == 0 && !fled);
        CHECK(gv.arrived && !gv.failed);
        CHECK(sys.stats().pushes >= 1);
        std::printf("land squad part B: Giant vs squad - pushes %u, member objects %u, container pushed %d, giant arrived %d\n",
                    sys.stats().pushes, member_objects, cont_pushed ? 1 : 0, gv.arrived ? 1 : 0);
    }
    {  // B: a crusher vs a stopped hostile squad: crush-flee, 2x, recovery
        struct Res {
            uint64_t crc = 0;
            uint32_t flee_frames = 0;
            float team = 0.0f, cmax = 0.0f, mmax = 0.0f, final_err = 1.0e9f;
            bool ended = false, crusher_arrived = false;
        };
        const auto run = [&](const ParallelFor& pf) {
            Res r;
            MovementSystem sys{MovementConfig{}};
            land_system(sys, 100);
            t14b_squad(sys, at, 1);
            SimRng rng(22);
            for (uint32_t f = 1; f < 20; ++f) sys.tick(f, rng, pf);
            r.team = sys.view(100).team_speed;
            UnitDesc cr = unit(60, cell_center(8, 20), 2, 1.0f, 10.0f, LocomotorKind::Default);
            cr.collision_class = kUccVehicle;
            cr.crusher = true;
            cr.owner = 2;
            CHECK(sys.add_unit(cr));
            MoveOrder o;
            o.units = {60};
            o.destination = cell_center(60, 20);
            sys.move(o, rng, 20);
            uint32_t last_flee = 0;
            for (uint32_t f = 20; f < 1100; ++f) {
                sys.tick(f, rng, pf);
                if (!sys.team_fleeing(100)) continue;
                ++r.flee_frames;
                last_flee = f;
                r.cmax = std::max(r.cmax, sys.view(100).speed);
                for (ObjectId id = 1; id <= 4; ++id) r.mmax = std::max(r.mmax, sys.view(id).speed);
            }
            r.ended = last_flee != 0 && last_flee < 1000 && !sys.team_fleeing(100) && !sys.view(100).pushed;
            r.crusher_arrived = sys.view(60).arrived;
            r.final_err = 0.0f;
            for (ObjectId id = 1; id <= 4; ++id) r.final_err = std::max(r.final_err, length(t14b_slot_point(sys, id) - sys.view(id).pos));
            StateHasher h;
            sys.hash_units(h);
            r.crc = h.digest();
            return r;
        };
        const Res a = run(serial_for);
        const Res b = run(make_thread_pf(4));
        CHECK(a.crc == b.crc && a.flee_frames == b.flee_frames);
        CHECK(a.flee_frames > 0 && a.ended && a.crusher_arrived);
        // the container's team speed and the members' max speed (= the team speed here) x 2 while fleeing
        // (Fleeing_Infantry_Speed_Bonus is exactly 2: this fixture reaches its target speed in one frame). s78n t20 batch 2
        // (FIGHTER_FLYING §7.7 item 7): the flee now starts with the container's turn in place (B x 2); it then leaves its
        // members on the old slots and the straggler cap (0x5ab640, factor 0.90 .. 0.95 here) keeps it under 2x through
        // the short flee - above the team speed, which only the bonus allows.
        // Why a range and not an exact value (s78n t20 batch 2 review): each flee frame the container's target is
        // T = team x 2 (the bonus) x the straggler factor 1 - clamp((edge - p) / 10, 0, 1), p = the most-lagging member's
        // offset along the container's facing, edge = -HX - 2. p comes out of the members' slot steering through the
        // re-lay after the turn (collision vectors and jiggle included), so the factor is not a closed form of the
        // fixture; the speed step reaches T in one frame (rate >= T here), hence cmax = 2 x team x the largest factor
        // over the 11 flee frames. Bounds: factor <= 1 -> cmax <= 2 x team; cmax > team needs a frame with factor > 0.5,
        // i.e. every member within 5 behind the edge. Measured (MSVC = clang, serial = 4 workers): 5.945 = 2 x 3.120 x
        // 0.9527.
        CHECK(a.cmax > a.team && a.cmax <= 2.0f * a.team);
        CHECK(a.mmax == 2.0f * a.team);
        CHECK(a.final_err <= 5.0f);  // recovered (a member stops within the 4.0 recovery distance)
        std::printf("land squad part B: crusher - fled %u frames, team %.3f, container max %.3f, member max %.3f, final slot "
                    "error %.3f\n",
                    a.flee_frames, static_cast<double>(a.team), static_cast<double>(a.cmax), static_cast<double>(a.mmax),
                    static_cast<double>(a.final_err));
    }
    {  // C: the member steering vector on a curved march
        struct Res {
            uint64_t crc = 0;
            float max_err = 0.0f, best_side = 0.0f;
            uint32_t vec_frames = 0;
            bool arrived = false;
        };
        const auto run = [&](const ParallelFor& pf, bool avoidance_disabled) {
            Res r;
            MovementSystem sys{MovementConfig{}};
            PassabilityGrid g = open_grid(100, 100);
            for (int32_t y = 10; y <= 40; ++y) g.set_base(30, y, kPassImpassable);  // a wall: the path bends around it
            sys.set_grid(std::move(g));
            sys.set_anim_length_fn(anim_len);
            const V2 start = cell_center(20, 20);
            t14b_squad(sys, start, 1, avoidance_disabled);
            SimRng rng(23);
            MoveOrder o;
            o.units = {1};
            o.destination = cell_center(45, 25);
            sys.move(o, rng, 1);
            ObjectId next_obstacle = 70;
            for (uint32_t f = 1; f < 1200; ++f) {
                // every 90 frames of the march a stopped unit appears just ahead and right of the container
                const UnitView k = sys.view(100);
                if (k.has_goal && k.speed > 0.0f && f % 90 == 0 && next_obstacle < 74) {
                    const V2 fw = dir_from_deg(k.yaw), right{fw.y, -fw.x};
                    UnitDesc x = unit(next_obstacle++, k.pos + fw * 18.0f + right * 14.0f, 2, 1.0f, 6.0f, LocomotorKind::Default);
                    x.owner = 1;
                    CHECK(sys.add_unit(x));
                }
                sys.tick(f, rng, pf);
                const UnitView kv = sys.view(100);
                const V2 fw = dir_from_deg(kv.yaw), right{fw.y, -fw.x};
                for (ObjectId id = 1; id <= 4; ++id) {
                    if (kv.has_goal && f > 40) r.max_err = std::max(r.max_err, length(t14b_slot_point(sys, id) - sys.view(id).pos));
                    const V2 v = sys.team_member_avoid(id, fw);
                    if (length_sq(v) > 0.0f) {
                        ++r.vec_frames;
                        if (std::fabs(dot(v, right)) > std::fabs(r.best_side)) r.best_side = dot(v, right);
                    }
                }
            }
            r.arrived = sys.view(100).arrived;
            StateHasher h;
            sys.hash_units(h);
            r.crc = h.digest();
            return r;
        };
        const Res a = run(serial_for, false);
        const Res b = run(make_thread_pf(4), false);
        const Res off = run(serial_for, true);  // Avoidance_Disabled members: no vector (0x5c6ca0)
        CHECK(a.crc == b.crc && a.vec_frames == b.vec_frames);
        CHECK(a.arrived && off.arrived);
        CHECK(a.vec_frames > 0 && a.best_side < 0.0f);  // a side step to the left: away from the unit on the right
        CHECK(off.vec_frames == 0);
        // the march's slot error comes from the walk states (start delays, turns around the wall: part A); the vector's
        // side steps stay small next to it
        CHECK(a.max_err < 60.0f && a.max_err <= off.max_err + 5.0f);
        std::printf("land squad part B: steering vector - frames with a vector %u, strongest side component %.3f, max slot "
                    "error on the march %.3f (Avoidance_Disabled %.3f), arrived %d\n",
                    a.vec_frames, static_cast<double>(a.best_side), static_cast<double>(a.max_err),
                    static_cast<double>(off.max_err), a.arrived ? 1 : 0);
    }
    {  // the 0x509810 rules on a hand-placed case: no objects -> 0; not a member -> 0
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        t14b_squad(sys, at, 1);
        CHECK(sys.team_member_avoid(1, {1.0f, 0.0f}) == V2{});
        CHECK(sys.team_member_avoid(100, {1.0f, 0.0f}) == V2{});
        CHECK(!sys.team_fleeing(1) && !sys.team_fleeing(100));
    }
}

void t16_squad(MovementSystem& sys, V2 at, int32_t arrangement, float cmax, float cjiggle, const std::vector<T16Member>& mem,
                      int32_t owner, bool squashable) {
    UnitDesc c = unit(100, at, 2, cmax, 12.0f, LocomotorKind::Default);
    c.team_container = true;
    c.container_arrangement = arrangement;
    c.max_jiggle_distance = cjiggle;
    c.accel = 0.1f;  // the container type's own tags are not read (0x5ab540)
    c.decel = 0.1f;
    c.owner = owner;
    c.squashable = squashable;
    CHECK(sys.add_unit(c));
    std::vector<ObjectId> ids;
    std::vector<V2> slots;
    for (size_t i = 0; i < mem.size(); ++i) {
        UnitDesc d = unit(static_cast<ObjectId>(i + 1), at + mem[i].rel + V2{-mem[i].back, 0.0f}, 2, mem[i].max_speed, 3.0f, LocomotorKind::Default);
        d.accel = mem[i].decel;
        d.decel = mem[i].decel;
        d.owner = owner;
        d.formation_spacing = mem[i].spacing;
        CHECK(sys.add_unit(d));
        CHECK(sys.set_team_member_shape(d.id, 3.0f, 3.0f, 3.0f, 3.0f));
        ids.push_back(d.id);
        slots.push_back(mem[i].rel);
    }
    CHECK(sys.set_land_team(100, ids, slots));
    CHECK(sys.set_team(100, 100, 1.0f, cmax));
    for (ObjectId id : ids) CHECK(sys.set_team(id, 100, 1.0f, cmax));
}

// s78n t16 (FIGHTER_FLYING §7.7, S78N_T14_RUN.md):
//  * 0x5ab540: the container accelerates and brakes at the minimum over its members of OverrideDeceleration x (the
//    members' team-capped speed / member Max_Speed) - FoC Stormtrooper squad 0.05 x 1.56 = 0.078 per frame (team speed
//    0.8 x 1.56), Rebel squad 0.07 x 1.56 = 0.1092 (team speed 1.0 x 1.56); the smallest member value wins; a member type
//    without the tag = full speed in one frame; the container's own acceleration is not read;
//  * 0x5ab640: a member more than HX + 2 behind the container slows its target speed linearly to 0 at HX + 12 (HX = the
//    joint slot box half-length + the container's MaxJiggleDistance); [port] not when a wall is between it and its slot.
void test_land_squad_speed_step() {
    const float mult = 1.56f;  // Object_Max_Speed_Multiplier_Land (the test config's default)
    struct Ramp {
        std::vector<float> v;  // the container's speed after each tick from the order on
        float team = 0.0f;
        bool arrived = false;
    };
    const auto march = [&](int32_t arrangement, float cmax, const std::vector<T16Member>& mem, uint32_t seed) {
        Ramp r;
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        t16_squad(sys, cell_center(10, 20), arrangement, cmax, 0.0f, mem);
        SimRng rng(seed);
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(30, 20);
        sys.move(o, rng, 1);
        for (uint32_t f = 1; f < 900 && !r.arrived; ++f) {
            sys.tick(f, rng, serial_for);
            const UnitView k = sys.view(100);
            r.team = k.team_speed;
            r.arrived = k.arrived;
            r.v.push_back(k.speed);
        }
        return r;
    };
    // `steps` exact start steps of `a`; no frame changes the speed by more than `a` (the stop on the last waypoint aside);
    // the cruise is the team speed; the braking before the stop takes about team / a frames.
    const auto check_ramp = [&](const char* name, const Ramp& r, float a, float team, uint32_t steps) {
        CHECK(r.arrived && near(r.team, team, 1e-5f));
        size_t f0 = 0;
        while (f0 < r.v.size() && !(r.v[f0] > 0.0f)) ++f0;
        bool exact = f0 + steps < r.v.size();
        for (uint32_t i = 0; exact && i < steps; ++i) exact = near(r.v[f0 + i], a * static_cast<float>(i + 1), 1e-4f);
        float top = 0.0f, max_step = 0.0f;
        size_t last_cruise = 0;
        for (size_t i = f0; i + 1 < r.v.size(); ++i) {  // the last entry is the arrival frame (speed 0)
            top = std::max(top, r.v[i]);
            if (near(r.v[i], team, 1e-4f)) last_cruise = i;
            if (i > f0) max_step = std::max(max_step, std::fabs(r.v[i] - r.v[i - 1]));
        }
        const size_t brake = r.v.size() - 1 - last_cruise;
        CHECK(exact);
        CHECK(near(top, team, 1e-4f));
        CHECK(max_step <= a + 1e-4f);
        // (the walk stops on the last waypoint from a few steps of speed, so somewhat fewer than team / a frames)
        CHECK(static_cast<float>(brake) >= 0.75f * team / a && static_cast<float>(brake) <= team / a + 6.0f);
        std::printf("land squad speed step: %s - team %.4f, start steps %.4f %.4f %.4f %.4f, largest change %.4f, braking "
                    "frames %zu\n",
                    name, static_cast<double>(r.team), static_cast<double>(r.v[f0]), static_cast<double>(r.v[f0 + 1]),
                    static_cast<double>(r.v[f0 + 2]), static_cast<double>(r.v[f0 + 3]), static_cast<double>(max_step), brake);
    };
    {  // FoC Stormtrooper squad: Rows, 9 x (Max_Speed 1.0, 0.05), container Max_Speed 0.8
        std::vector<T16Member> mem;
        for (int32_t ix = 1; ix >= -1; --ix)
            for (int32_t iy = 1; iy >= -1; --iy)
                mem.push_back({V2{12.0f * static_cast<float>(ix), 12.0f * static_cast<float>(iy)}, 1.0f, 0.05f, 0.3f, 0.0f});
        check_ramp("Stormtrooper squad", march(0, 0.8f, mem, 31), 0.05f * mult, 0.8f * mult, 8);
    }
    {  // FoC Rebel squad: Dartboard, 5 x (Max_Speed 1.0, 0.07), container Max_Speed 1.1
        std::vector<T16Member> mem;
        const V2 rel[5] = {{10.0f, 6.0f}, {10.0f, -6.0f}, {0.0f, 0.0f}, {-10.0f, 6.0f}, {-10.0f, -6.0f}};
        for (const V2& p : rel) mem.push_back({p, 1.0f, 0.07f, 0.5f, 0.0f});
        check_ramp("Rebel squad", march(1, 1.1f, mem, 32), 0.07f * mult, 1.0f * mult, 8);
    }
    {  // mixed members: the smallest 0x373080 wins - 0.06 x (min(2.0, 1.0) x 1.56) / 2.0 = 0.0468 < 0.05 x 1.56
        std::vector<T16Member> mem = {{V2{8.0f, 0.0f}, 1.0f, 0.05f, 0.3f, 0.0f}, {V2{-8.0f, 0.0f}, 2.0f, 0.06f, 0.3f, 0.0f}};
        const Ramp r = march(-1, 3.0f, mem, 33);
        check_ramp("mixed members", r, 0.06f * (1.0f * mult) / 2.0f, 1.0f * mult, 8);
    }
    {  // a member type without OverrideDeceleration gives the full speed (MOVEMENT §3.4), so the tagged member's value wins
        std::vector<T16Member> mem = {{V2{8.0f, 0.0f}, 1.0f, 0.05f, 0.3f, 0.0f}, {V2{-8.0f, 0.0f}, 1.0f, 0.0f, 0.3f, 0.0f}};
        check_ramp("one untagged member", march(-1, 0.8f, mem, 34), 0.05f * mult, 0.8f * mult, 8);
    }
    {  // no member has the tag: the minimum is the full speed -> the team speed in one frame
        std::vector<T16Member> mem = {{V2{8.0f, 0.0f}, 1.0f, 0.0f, 0.3f, 0.0f}, {V2{-8.0f, 0.0f}, 1.0f, 0.0f, 0.3f, 0.0f}};
        const Ramp r = march(-1, 0.8f, mem, 36);
        size_t f0 = 0;
        while (f0 < r.v.size() && !(r.v[f0] > 0.0f)) ++f0;
        CHECK(r.arrived && f0 < r.v.size() && near(r.v[f0], 0.8f * mult, 1e-5f));
    }
    // ---- the straggler cap 0x5ab640: slots (10, 0) and (-10, 0), half-extent 3 -> box [-13, 13]; MaxJiggleDistance 1.5:
    // HX = 14.5. Member 2 is held (speed factor 0) behind the container; the members have no deceleration tag, so the
    // container's speed is its target speed in one frame.
    const float hx = 13.0f + 1.5f;
    const auto straggler = [&](float behind, bool release) {
        struct Out {
            bool formula = true, arrived = false;
            float first = 0.0f, last = 0.0f, gap = 0.0f, team = 0.0f;
        } out;
        MovementSystem sys{MovementConfig{}};
        // (a wall across member 2's way: nothing about passability enters the cap or the member's step)
        PassabilityGrid g = open_grid(100, 100);
        for (int32_t y = 15; y <= 25; ++y) g.set_base(28, y, kPassImpassable);
        sys.set_grid(std::move(g));
        sys.set_anim_length_fn(anim_len);
        const V2 at = cell_center(30, 20);
        const std::vector<T16Member> mem = {{V2{10.0f, 0.0f}, 2.0f, 0.0f, 0.3f, 0.0f},
                                            {V2{-10.0f, 0.0f}, 2.0f, 0.0f, 0.3f, behind - 10.0f}};
        t16_squad(sys, at, -1, 2.0f, 1.5f, mem);
        SimRng rng(35);
        CHECK(sys.set_speed_factor(2, 0.0f));
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(60, 20);
        sys.move(o, rng, 1);
        for (uint32_t f = 1; f < 180; ++f) {
            // the cap from the positions at the start of the tick
            const UnitView k = sys.view(100);
            const V2 fw = dir_from_deg(k.yaw);
            float fac = 1.0f;
            for (ObjectId id = 1; id <= 2; ++id) {
                const V2 d = sys.view(id).pos - k.pos;
                const float p = fw.x * d.x + fw.y * d.y;
                if (p < -hx - 2.0f) fac = std::min(fac, 1.0f - std::clamp(((-hx - 2.0f) - p) / 10.0f, 0.0f, 1.0f));
            }
            sys.tick(f, rng, serial_for);
            const UnitView kv = sys.view(100);
            out.team = kv.team_speed;
            if (f < kv.start_frame) continue;  // the order's start delay
            if (kv.has_goal) out.formula = out.formula && near(kv.speed, fac * kv.team_speed, 1e-4f);
            if (f == kv.start_frame) out.first = kv.speed;
            out.last = kv.speed;
            out.gap = kv.pos.x - sys.view(2).pos.x;
        }
        if (release) {  // the held member walks again: it closes up and the squad arrives
            CHECK(sys.set_speed_factor(2, 1.0f));
            for (uint32_t f = 180; f < 900 && !out.arrived; ++f) {
                sys.tick(f, rng, serial_for);
                out.arrived = sys.view(100).arrived;
            }
        }
        return out;
    };
    {
        const auto half = straggler(hx + 2.0f + 5.0f, true);  // 5 past HX + 2: t = 0.5 on the first moving frame
        CHECK(half.formula && near(half.first, 0.5f * half.team, 1e-4f));
        CHECK(half.last < 1e-3f && near(half.gap, hx + 12.0f, 1e-2f));  // the container waits HX + 12 ahead of it
        CHECK(half.arrived);
        const auto inside = straggler(hx + 1.5f, false);  // inside HX + 2: the full team speed at first
        CHECK(inside.formula && near(inside.first, inside.team, 1e-5f));
        const auto far = straggler(75.0f, true);  // beyond HX + 12 and behind the wall: the container does not start
        CHECK(far.formula && far.first == 0.0f && far.last == 0.0f);
        CHECK(far.arrived);  // released: the member walks through the wall and the squad arrives
    }
}

// The member steering vector 0x509810 (FIGHTER_FLYING §7.5 items 2-4) recomputed from the public state, for a member of
// a squad whose container's collision partners are plain units (no other squad): per record of the container in creation
// order (live + resolved this tick) the weight (MovingVsMoving 1, MovingVsStopped 2, StoppedVsStopped 3) and the vector
// (MovingVsStopped only: the side step 0x5c71a0 away from the other participant x clamp(speed / max speed)); best = the
// largest weight, a tie the longer vector; + every other vector with dot(best, v) >= -0.5. Open ground: every keep probe
// passes. `added` / `dropped` count the non-zero other vectors the dot rule took / left out.
V2 t14b_avoid_ref(const MovementSystem& sys, ObjectId member, V2 dir, float vmax, uint32_t& added, uint32_t& dropped) {
    ObjectId k = -1;
    V2 slot;
    if (!sys.team_slot(member, k, slot)) return {};
    std::vector<CollisionObject> recs;
    for (const CollisionObject& c : sys.collisions().list())
        if (c.a == k || c.b == k) recs.push_back(c);
    std::sort(recs.begin(), recs.end(), [](const CollisionObject& x, const CollisionObject& y) { return x.seq < y.seq; });
    const UnitView a = sys.view(member);
    const float ratio = vmax > 0.0f ? std::clamp(a.speed / vmax, 0.0f, 1.0f) : 0.0f;
    const float ra = 3.0f;  // s78n t18: the member's soft footprint radius (0x397780 of a unit that is no container)
    std::vector<V2> vecs;
    std::vector<float> weights;
    for (const CollisionObject& c : recs) {
        const ObjectId other = c.a == k ? c.b : c.a;
        CHECK(!sys.is_land_team_container(other));
        V2 v{};
        // s78n t18: MovingVsStopped - the obstacle side always, the mover side only in Repulsion; MovingVsMoving - Repulse
        const bool gives = c.kind == CollisionKind::MovingVsStopped   ? c.b == k || c.state == CollisionState::Repulsion
                           : c.kind == CollisionKind::MovingVsMoving ? c.state == CollisionState::Repulsion
                                                                     : false;
        if (gives) {
            const UnitView b = sys.view(other);
            const float reach = (ra + 12.0f) * 1.3f;  // the obstacles of this test have radius 12 and no hard extents
            const float t = std::clamp(1.0f - length(a.pos - b.pos) / reach, 0.0f, 1.0f);
            if (t > 0.0f) v = (cross(dir, b.pos - a.pos) < 0.0f ? V2{-dir.y, dir.x} : V2{dir.y, -dir.x}) * t * ratio;
        }
        vecs.push_back(v);
        weights.push_back(c.kind == CollisionKind::MovingVsMoving ? 1.0f : c.kind == CollisionKind::MovingVsStopped ? 2.0f : 3.0f);
    }
    if (vecs.empty()) return {};
    size_t best = 0;
    for (size_t i = 1; i < vecs.size(); ++i)
        if (weights[i] > weights[best] || (weights[i] == weights[best] && length_sq(vecs[i]) > length_sq(vecs[best]))) best = i;
    V2 out = vecs[best];
    for (size_t j = 0; j < vecs.size(); ++j) {
        if (j == best) continue;
        const bool take = dot(vecs[best], vecs[j]) >= -0.5f;
        if (take) out = out + vecs[j];
        if (length_sq(vecs[j]) > 0.0f) ++(take ? added : dropped);
    }
    return out;
}

// s78n review of t14 part B / t16:
//  * fleeing: Fleeing_Infantry_Speed_Bonus (2) on the container's acceleration and turn rate and on the members'
//    acceleration and turn rate (the x 2 on the speeds is pinned in test_land_squad_part_b);
//  * the MovingVsMoving crush pair flees only when the squad's owner scatters (set_scatters_fn), else it is pushed;
//  * (the garrison exclusion: test_land_squad_gates, s78n t18);
//  * the steering vector's weights and dot >= -0.5 rule against a recomputation;
//  * a pushed walk cut short on a container with no order leaves it stopped: its members recover to their slots.
void test_land_squad_review_fixes() {
    const float mult = 1.56f;
    const auto grid_squad = [](MovementSystem& sys, V2 at, float decel, int32_t owner) {  // 2 x 2, Rows, Max_Speed 2
        std::vector<T16Member> mem;
        for (float x : {8.0f, -8.0f})
            for (float y : {8.0f, -8.0f}) mem.push_back({V2{x, y}, 2.0f, decel, 0.5f, 0.0f});
        t16_squad(sys, at, 0, 2.0f, 0.0f, mem, owner, true);
    };
    {  // fleeing x 2 on acceleration and turn rate
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        const V2 at = cell_center(30, 20);
        grid_squad(sys, at, 0.1f, 1);
        SimRng rng(41);
        for (uint32_t f = 1; f < 20; ++f) sys.tick(f, rng, serial_for);
        UnitDesc cr = unit(60, cell_center(8, 20), 2, 1.0f, 10.0f, LocomotorKind::Default);
        cr.collision_class = kUccVehicle;
        cr.crusher = true;
        cr.owner = 2;
        CHECK(sys.add_unit(cr));
        MoveOrder o;
        o.units = {60};
        o.destination = cell_center(60, 20);
        sys.move(o, rng, 20);
        const float a = 0.1f * (2.0f * mult) / 2.0f;  // 0x5ab540 and each member's own acceleration: 0.156
        const float rate = 6.0f * mult;               // the fixture's Max_Rate_Of_Turn x the mode multiplier
        float k_dv = 0.0f, k_dyaw = 0.0f, m_dv = 0.0f, m_dyaw = 0.0f;
        uint32_t flee_frames = 0;
        for (uint32_t f = 20; f < 700; ++f) {
            UnitView before[5];
            before[0] = sys.view(100);
            for (ObjectId id = 1; id <= 4; ++id) before[id] = sys.view(id);
            const bool was = sys.team_fleeing(100);
            sys.tick(f, rng, serial_for);
            if (!was || !sys.team_fleeing(100)) continue;  // frames walked wholly on the crush-flee path
            ++flee_frames;
            const UnitView k = sys.view(100);
            k_dv = std::max(k_dv, k.speed - before[0].speed);
            k_dyaw = std::max(k_dyaw, std::fabs(wrap_deg(k.yaw - before[0].yaw)));
            for (ObjectId id = 1; id <= 4; ++id) {
                const UnitView m = sys.view(id);
                m_dv = std::max(m_dv, m.speed - before[id].speed);
                m_dyaw = std::max(m_dyaw, std::fabs(wrap_deg(m.yaw - before[id].yaw)));
            }
        }
        CHECK(flee_frames > 3);
        CHECK(near(k_dv, 2.0f * a, 1e-4f));        // the container's acceleration x 2
        CHECK(near(k_dyaw, 2.0f * rate, 1e-3f));   // its turn rate x 2
        CHECK(near(m_dv, 2.0f * a, 1e-4f));        // a member's acceleration x 2
        CHECK(m_dyaw > rate + 1e-3f && m_dyaw <= 2.0f * rate + 1e-3f);  // and its turn rate
        std::printf("land squad review fixes: flee %u frames - container step %.4f (2 x %.4f), turn %.3f (2 x %.3f); member step "
                    "%.4f, turn %.3f\n",
                    flee_frames, static_cast<double>(k_dv), static_cast<double>(a), static_cast<double>(k_dyaw),
                    static_cast<double>(rate), static_cast<double>(m_dv), static_cast<double>(m_dyaw));
    }
    {  // MovingVsMoving crush pair: crush-flee only when the owner's faction scatters
        const auto run = [&](bool scatters) {
            std::pair<uint32_t, uint32_t> r{0, 0};  // pushed frames, fleeing frames
            MovementSystem sys{MovementConfig{}};
            land_system(sys, 100);
            sys.set_scatters_fn([scatters](int32_t) { return scatters; });
            grid_squad(sys, cell_center(15, 20), 0.0f, 1);
            UnitDesc cr = unit(60, cell_center(60, 20), 2, 1.0f, 10.0f, LocomotorKind::Default);
            cr.collision_class = kUccVehicle;
            cr.crusher = true;
            cr.owner = 2;
            cr.yaw_deg = 180.0f;
            CHECK(sys.add_unit(cr));
            SimRng rng(42);
            MoveOrder o;
            o.units = {1};  // the squad marches +x, the crusher -x on the same line: both moving when they meet
            o.destination = cell_center(60, 20);
            sys.move(o, rng, 1);
            MoveOrder co;
            co.units = {60};
            co.destination = cell_center(15, 20);
            sys.move(co, rng, 1);
            for (uint32_t f = 1; f < 600; ++f) {
                sys.tick(f, rng, serial_for);
                if (sys.view(100).pushed) ++r.first;
                if (sys.team_fleeing(100)) ++r.second;
            }
            return r;
        };
        const auto on = run(true), off = run(false);
        CHECK(on.first > 0 && on.second > 0);  // Scatters_From_Crushers: a crush-flee detour (state 1, kind 7)
        // s78n t23 (MOVEMENT §5.4.6 chooser, "4 (head-on)"): a crusher against a non-scattering victim -> state 7, both
        // run on (no detour, no push)
        CHECK(off.first == 0 && off.second == 0);
        std::printf("land squad review fixes: moving crush pair - scatters: pushed %u fleeing %u; not: pushed %u fleeing %u\n",
                    on.first, on.second, off.first, off.second);
    }
    {  // the steering vector against the recomputation: weights, the longer-vector tie, dot >= -0.5
        struct Out {
            uint32_t added = 0, dropped = 0, frames = 0;
            bool placed = false, same = true;
            float worst = 0.0f;
        };
        // Once the squad cruises, two stopped units (radius 12) appear 5 ahead of member 1, `side` to its left and right
        // (it walks between them): two MovingVsStopped objects of the container with opposed side steps for member 1.
        const auto run = [&](float side) {
            Out r;
            MovementSystem sys{MovementConfig{}};
            land_system(sys, 100);
            grid_squad(sys, cell_center(15, 20), 0.0f, 1);
            SimRng rng(44);
            MoveOrder o;
            o.units = {1};
            o.destination = cell_center(70, 20);
            sys.move(o, rng, 1);
            uint32_t placed_at = 0;
            for (uint32_t f = 1; f < 400; ++f) {
                const UnitView k0 = sys.view(100);
                if (placed_at == 0 && k0.has_goal && near(k0.speed, k0.team_speed, 1e-4f) &&
                    near(sys.view(1).speed, k0.speed, 1e-3f) && k0.pos.x > cell_center(22, 20).x) {
                    const V2 fw0 = dir_from_deg(k0.yaw), right{fw0.y, -fw0.x};
                    const V2 p = sys.view(1).pos;
                    for (int32_t sgn : {1, -1}) {
                        UnitDesc x = unit(sgn > 0 ? 70 : 71, p + fw0 * 5.0f + right * (side * static_cast<float>(sgn)), 2, 1.0f, 12.0f,
                                          LocomotorKind::Default);
                        x.owner = 1;
                        CHECK(sys.add_unit(x));
                    }
                    placed_at = f;
                }
                sys.tick(f, rng, serial_for);
                if (placed_at == 0 || f > placed_at + 40) continue;
                const V2 fw = dir_from_deg(sys.view(100).yaw);
                for (ObjectId id = 1; id <= 4; ++id) {
                    const V2 ref = t14b_avoid_ref(sys, id, fw, 2.0f * mult, r.added, r.dropped);
                    const V2 got = sys.team_member_avoid(id, fw);
                    const float err = length(ref - got);
                    r.worst = std::max(r.worst, err);
                    r.same = r.same && err < 1e-4f;
                    if (length_sq(got) > 0.0f) ++r.frames;
                }
            }
            r.placed = placed_at != 0;
            return r;
        };
        const Out tight = run(3.0f), wide = run(8.0f);
        CHECK(tight.placed && wide.placed && tight.same && wide.same);
        CHECK(tight.frames > 0 && tight.dropped > 0);  // close on both sides: dot(best, other) < -0.5, only the longer one
        CHECK(wide.frames > 0 && wide.added > 0);      // further apart: the opposed step is added
        std::printf("land squad review fixes: steering vector = recomputation (worst %.6f / %.6f); units 3 to each side: %u "
                    "frames, opposed added %u dropped %u; 8 to each side: %u frames, added %u dropped %u\n",
                    static_cast<double>(tight.worst), static_cast<double>(wide.worst), tight.frames, tight.added, tight.dropped,
                    wide.frames, wide.added, wide.dropped);
    }
    {  // a pushed walk blocked on a container with no order: stopped, so its members recover. s78n t24 (MOVEMENT §5.4.7):
       // the failed step makes a TerrainCollision record, whose Path Around finds no order path (no kind-1 record) - the
       // container is stopped and its push record cleared
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        const V2 at = cell_center(30, 20);
        grid_squad(sys, at, 0.0f, 1);
        SimRng rng(45);
        for (uint32_t f = 1; f < 20; ++f) sys.tick(f, rng, serial_for);
        UnitDesc g = unit(50, cell_center(10, 20), 2, 1.0f, 20.0f, LocomotorKind::Default);
        g.collision_class = kUccGiantVehicle;
        g.owner = 2;
        CHECK(sys.add_unit(g));
        MoveOrder o;
        o.units = {50};
        o.destination = cell_center(50, 20);
        sys.move(o, rng, 20);
        uint32_t f = 20, cut_at = 0;
        for (; f < 900 && !sys.view(100).pushed; ++f) sys.tick(f, rng, serial_for);
        CHECK(sys.view(100).pushed);
        // wall the container in: its pushed walk is blocked from now on. s78n t23: a ring two cells out - its own cell
        // stays passable (standing on an impassable cell, its MovingVsMoving record with the giant would stop it at
        // once, MOVEMENT §5.4.2 validity)
        const V2 kp = sys.view(100).pos;
        const int32_t cx = world_to_cell(kp.x), cy = world_to_cell(kp.y);
        for (int32_t y = cy - 2; y <= cy + 2; ++y)
            for (int32_t x = cx - 2; x <= cx + 2; ++x)
                if (std::abs(x - cx) == 2 || std::abs(y - cy) == 2) sys.grid().set_base(x, y, kPassImpassable);
        const uint32_t blocked0 = sys.stats().blocked_steps;
        for (uint32_t n = 0; n < 40 && sys.view(100).pushed; ++n, ++f) sys.tick(f, rng, serial_for);
        cut_at = f;
        CHECK(!sys.view(100).pushed && sys.stats().blocked_steps >= blocked0 + 1 && sys.stats().terrain_stops_nobase >= 1);
        CHECK(sys.remove_unit(50));  // the pusher goes: the squad stands alone, with no order
        CHECK(!sys.view(100).has_goal && sys.view(100).speed == 0.0f);
        // a member knocked off its slot now recovers (the recovery decision runs only while the container is stopped)
        const V2 m1 = sys.view(1).pos;
        CHECK(sys.teleport(1, {m1.x + 30.0f, m1.y + 30.0f}, 0.0f, 0.0f, f));
        bool recovering = false;
        for (uint32_t n = 0; n < 200; ++n, ++f) {
            sys.tick(f, rng, serial_for);
            recovering = recovering || sys.view(1).has_goal;
        }
        CHECK(recovering && !sys.view(1).has_goal && length(t14b_slot_point(sys, 1) - sys.view(1).pos) <= 4.0f);
        std::printf("land squad review fixes: pushed walk cut at frame %u, member recovered %d\n", cut_at, recovering ? 1 : 0);
    }
}

// s78n t18 item 8 (FIGHTER_FLYING §7.4, 0x3abb80): a teleport of the container puts every member at the new place + its
// old offset from the container, with its locomotor idle; member facings are not written; a parked member's stored
// offset is not touched.
void test_land_squad_teleport() {
    MovementSystem sys{MovementConfig{}};
    land_system(sys, 100);
    const V2 at = cell_center(20, 20);
    t14b_squad(sys, at, 1);
    SimRng rng(51);
    MoveOrder o;
    o.units = {1};
    o.destination = cell_center(40, 24);
    sys.move(o, rng, 1);
    for (uint32_t f = 1; f < 100; ++f) sys.tick(f, rng, serial_for);  // marching (an early order starts at frame 61)
    CHECK(sys.view(100).speed > 0.0f && sys.view(1).speed > 0.0f);
    V2 off4;
    CHECK(sys.park_team_member(4) && sys.parked_offset(4, off4));
    const UnitView k0 = sys.view(100);
    UnitView m0[4];
    for (ObjectId id = 1; id <= 3; ++id) m0[id] = sys.view(id);
    const V2 to = cell_center(60, 50);
    CHECK(sys.teleport(100, to, 0.0f, 90.0f, 100));
    const UnitView k1 = sys.view(100);
    CHECK(k1.pos == to && k1.yaw == 90.0f && k1.speed == 0.0f && !k1.has_goal);
    for (ObjectId id = 1; id <= 3; ++id) {
        const UnitView m = sys.view(id);
        const V2 want = to + (m0[id].pos - k0.pos);  // the same expression the system evaluates
        CHECK(m.pos == want && m.speed == 0.0f && m.yaw == m0[id].yaw);
    }
    V2 off4b;
    CHECK(sys.parked_offset(4, off4b) && off4b == off4);
    // a member in recovery (0x2A) has it cleared by the teleport
    for (uint32_t f = 100; f < 110; ++f) sys.tick(f, rng, serial_for);
    const V2 m1 = sys.view(1).pos;
    CHECK(sys.teleport(1, {m1.x + 40.0f, m1.y}, 0.0f, 0.0f, 110));
    bool recovering = false;
    for (uint32_t f = 110; f < 210 && !recovering; ++f) {
        sys.tick(f, rng, serial_for);
        recovering = sys.view(1).has_goal && sys.view(1).speed > 0.0f;
    }
    CHECK(recovering);
    const V2 kp = sys.view(100).pos, mp = sys.view(1).pos;
    const V2 to2 = cell_center(30, 60);
    CHECK(sys.teleport(100, to2, 0.0f, 90.0f, 210));
    CHECK(sys.view(1).pos == to2 + (mp - kp) && sys.view(1).speed == 0.0f && !sys.view(1).has_goal);
    // a suspended member (a hero clash) is a TEAM entry: it moves with the container too
    CHECK(sys.suspend_team_member(2));
    const V2 kp2 = sys.view(100).pos, sp2 = sys.view(2).pos;
    CHECK(sys.teleport(100, at, 0.0f, 0.0f, 211));
    CHECK(sys.view(2).pos == at + (sp2 - kp2));
    std::printf("land squad teleport: members keep their offsets, speeds 0, facings kept, parked offset kept\n");
}

// ---- s78n t18 ------------------------------------------------------------------------------------------------------
// FoC squads of the t16 fixtures: Stormtrooper (Rows 3 x 3, slots at pitch 12, half-extents 3, Max_Speed 1.0 capped by
// the container's 0.8, OverrideDeceleration 0.05: team speed 1.248, rate 0.078) and Rebel (Dartboard, 5 members, rate
// 0.1092, team speed 1.56).
std::vector<T16Member> t18_storm() {
    std::vector<T16Member> mem;
    for (int32_t ix = 1; ix >= -1; --ix)
        for (int32_t iy = 1; iy >= -1; --iy)
            mem.push_back({V2{12.0f * static_cast<float>(ix), 12.0f * static_cast<float>(iy)}, 1.0f, 0.05f, 0.3f, 0.0f});
    return mem;
}

// s78n t18 review: the squad container's braking frames above speed 0 from the team speed `vmax` with the rate `a` - the
// speed step 0x5aaf70 while braking (target = the rate), replayed in floats: v -= min(a, v - a) while v > a.
uint32_t t18_brake_steps(float vmax, float a) {
    uint32_t n = 0;
    for (float v = vmax; v > a && n < 1000; ++n) v -= std::min(a, v - a);
    return n;
}

// s78n t18 items 2 / 6 (MOVEMENT §5.3.1 / §5.3.2, FIGHTER_FLYING §7.8): the squad's footprint is its live joint slot box
// (0x404450) - a rectangle centred on the container and turned by its yaw; members are in no collision or occupancy query,
// the container is in all of them with that rectangle.
void test_land_squad_footprint() {
    const V2 at = cell_center(10, 20);
    {  // the joint box: slots at pitch 12 with half-extents 3 -> (15, 15); SPREAD_OUT x 1.8 on the spacing; a lost row
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        t16_squad(sys, at, 0, 0.8f, 0.0f, t18_storm());
        float hx = 0.0f, hy = 0.0f;
        CHECK(sys.team_joint_box(100, hx, hy) && near(hx, 15.0f, 1e-4f) && near(hy, 15.0f, 1e-4f));
        CHECK(!sys.team_joint_box(1, hx, hy));  // not a container
        SimRng rng(61);
        sys.tick(1, rng, serial_for);  // the first layout (Rows, the container's facing): the same grid
        CHECK(sys.team_joint_box(100, hx, hy) && near(hx, 15.0f, 1e-3f) && near(hy, 15.0f, 1e-3f));
        CHECK(sys.set_team_spread_out(100, true));
        sys.tick(2, rng, serial_for);  // re-laid: pitch 6 + 0.3 x 20 x 1.8 = 16.8 -> 16.8 + 3
        CHECK(sys.team_joint_box(100, hx, hy) && near(hx, 19.8f, 1e-3f) && near(hy, 19.8f, 1e-3f));
        CHECK(sys.set_team_spread_out(100, false));
        sys.tick(3, rng, serial_for);
        CHECK(sys.team_joint_box(100, hx, hy) && near(hx, 15.0f, 1e-3f));
        // the three members of the front row (slot x = +12) go: x from -15 to max(0, 0 + 3) -> HX 9; y unchanged
        uint32_t gone = 0;
        for (const auto& [id, slot] : sys.land_team_slots(100))
            if (slot.x > 6.0f) {
                CHECK(sys.remove_unit(id));
                ++gone;
            }
        CHECK(gone == 3 && sys.team_joint_box(100, hx, hy) && near(hx, 9.0f, 1e-3f) && near(hy, 15.0f, 1e-3f));
    }
    // A container marching +x past a stopped Vehicle-class unit of radius 20 (no hard extents: a 20 x 20 footprint): a
    // collision object only when the unit is nearer the path than 15 + 20 (the circle of the box's radius 21.2 would
    // reach 41.2); with SPREAD_OUT the box is 19.8: 39.8.
    const auto pass = [&](float lateral, bool spread) {
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        t16_squad(sys, at, 0, 0.8f, 0.0f, t18_storm());
        UnitDesc v = unit(70, {at.x + 200.0f, at.y + lateral}, 2, 1.0f, 20.0f, LocomotorKind::Default);
        v.collision_class = kUccVehicle;
        CHECK(sys.add_unit(v));
        SimRng rng(62);
        CHECK(sys.set_team_spread_out(100, spread));
        sys.tick(1, rng, serial_for);
        MoveOrder o;
        o.units = {1};
        o.destination = {at.x + 400.0f, at.y};
        sys.move(o, rng, 2);
        for (uint32_t f = 2; f < 500; ++f) {
            CHECK(sys.set_team_spread_out(100, spread));
            sys.tick(f, rng, serial_for);
        }
        return sys.stats().collisions_created;  // (the only possible pair: the container and the unit)
    };
    const uint32_t far = pass(38.0f, false), near_by = pass(33.0f, false), spread = pass(38.0f, true);
    CHECK(far == 0 && near_by > 0 && spread > 0);
    std::printf("land squad footprint: objects with a unit 38 / 33 beside the path %u / %u, 38 with SPREAD_OUT %u\n", far, near_by,
                spread);
    {  // occupancy (0x538200, the push spot's box query): a squad member is no occupant, a container's box is
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        t16_squad(sys, at, 0, 0.8f, 0.0f, t18_storm());
        UnitDesc j = unit(80, cell_center(30, 30), 0, 2.0f, 6.0f, LocomotorKind::Jetpack);
        CHECK(sys.add_unit(j));
        SimRng rng(63);
        sys.tick(1, rng, serial_for);
        // members of a squad whose container stands far away: a second squad is not needed - move the container off
        CHECK(sys.teleport(100, cell_center(60, 60), 0.0f, 0.0f, 1));  // members go with it (item 8)
        // put member 1 alone at a spot and ask whether the jetpack may land there
        const V2 spot = cell_center(20, 30);
        CHECK(sys.teleport(1, spot, 0.0f, 0.0f, 1));
        CHECK(sys.land_spot_clear(80, spot));                       // only a squad member stands there: free
        CHECK(!sys.land_spot_clear(80, cell_center(60, 60)));       // the container's point: inside its joint box
        const V2 edge{cell_center(60, 60).x + 15.0f + 6.0f - 1.0f, cell_center(60, 60).y};  // box 15 + lander 6, overlapping by 1
        CHECK(!sys.land_spot_clear(80, edge));
        const V2 off{cell_center(60, 60).x + 15.0f + 6.0f + 1.0f, cell_center(60, 60).y};
        CHECK(sys.land_spot_clear(80, off));
    }
}

// A second FoC-sized squad for the s78n t18 tests: container `cid` (Rows, Max_Speed 0.8, Infantry, squashable) facing
// `yaw` with 2 x 2 members cid + 1 .. cid + 4 (Max_Speed 1.0, OverrideDeceleration 0.05, half-extents 3, slots +-6).
void t18_squad(MovementSystem& sys, ObjectId cid, V2 at, float yaw, int32_t owner, float member_speed) {
    const float cspeed = member_speed > 0.0f ? 0.8f : 0.0f;  // (Max_Speed 0: a squad that cannot be pushed)
    UnitDesc c = unit(cid, at, 2, cspeed, 12.0f, LocomotorKind::Default);
    c.team_container = true;
    c.container_arrangement = 0;
    c.yaw_deg = yaw;
    c.owner = owner;
    c.squashable = true;
    CHECK(sys.add_unit(c));
    const V2 f = dir_from_deg(yaw), l{-f.y, f.x};
    std::vector<ObjectId> ids;
    std::vector<V2> slots;
    for (int32_t i = 0; i < 4; ++i) {
        const V2 s{i < 2 ? 6.0f : -6.0f, i % 2 == 0 ? 6.0f : -6.0f};
        UnitDesc d = unit(cid + 1 + i, at + f * s.x + l * s.y, 2, member_speed, 3.0f, LocomotorKind::Default);
        d.accel = d.decel = member_speed > 0.0f ? 0.05f : 0.0f;
        d.yaw_deg = yaw;
        d.owner = owner;
        d.formation_spacing = 0.3f;
        CHECK(sys.add_unit(d));
        CHECK(sys.set_team_member_shape(d.id, 3.0f, 3.0f, 3.0f, 3.0f));
        ids.push_back(d.id);
        slots.push_back(s);
    }
    CHECK(sys.set_land_team(cid, ids, slots));
    CHECK(sys.set_team(cid, cid, 1.0f, cspeed));
    for (ObjectId id : ids) CHECK(sys.set_team(id, cid, 1.0f, cspeed));
}

// s78n t18 items 1 / 7 (MOVEMENT §5.4.1 / §5.4.2, FIGHTER_FLYING §7.8 item 5): what a squad container does in its
// collision records.
//  * moving against a standing Infantry-class unit with a free destination: Repulsion - it walks on along its path at
//    full speed, its members side-step, the obstacle stays; the record ends when the obstacle is behind by the two radii;
//  * with a blocked destination: the early-arrival cut (its box (HX + 5, HY) clear of the standing unit);
//  * two squads on opposed headings: Repulse (both at full speed, no hold, members' vectors); on the same heading: Speed
//    Slave Only (the rear one follows);
//  * against a standing vehicle: Path Around, never a shove; a crusher's one push attempt, then it drives on;
//  * a failed resolution: Try Again Later - speed 0 at the braking rate, re-decided every 30 frames;
//  * a single infantry unit against single infantry: Repulsion at once, the vector bends its own heading.
void test_land_squad_repulsion() {
    const float a = 0.05f * 1.56f, vmax = 0.8f * 1.56f;
    const V2 at = cell_center(10, 20);
    struct Out {
        uint64_t crc = 0;
        float off_line = 0.0f, min_cruise = 3.0e38f, obstacle_moved = 0.0f;
        uint32_t repulsion_frames = 0, vec_frames = 0, records_after = 0, other_states = 0;
        bool arrived = false;
        MovementStats st;
    };
    // obstacle: 0 a standing infantry unit dead ahead, 1 a standing second squad (container 200, members 201..204)
    const auto through = [&](int obstacle, const ParallelFor& pf) {
        Out r;
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        t16_squad(sys, at, 0, 0.8f, 0.0f, t18_storm(), 1);
        const V2 op{at.x + 200.0f, at.y};
        if (obstacle == 0) {
            UnitDesc x = unit(70, op, 2, 1.0f, 6.0f, LocomotorKind::Default);
            x.owner = 1;
            CHECK(sys.add_unit(x));
        } else {
            t18_squad(sys, 200, op, 90.0f, 1);
        }
        const ObjectId oid = obstacle == 0 ? 70 : 200;
        SimRng rng(71);
        sys.tick(1, rng, pf);
        const V2 o0 = sys.view(oid).pos;
        MoveOrder o;
        o.units = {1};
        o.destination = {at.x + 500.0f, at.y};
        sys.move(o, rng, 2);
        bool cruising = false;
        for (uint32_t f = 2; f < 900 && !r.arrived; ++f) {
            sys.tick(f, rng, pf);
            const UnitView k = sys.view(100);
            r.arrived = k.arrived;
            r.off_line = std::max(r.off_line, std::fabs(k.pos.y - at.y));
            if (near(k.speed, vmax, 1e-5f)) cruising = true;
            if (cruising && k.pos.x < at.x + 440.0f) r.min_cruise = std::min(r.min_cruise, k.speed);
            bool rep = false;
            for (const CollisionObject& c : sys.collisions().list())
                if (c.a == 100 && c.b == oid) {
                    if (c.kind == CollisionKind::MovingVsStopped && c.state == CollisionState::Repulsion) rep = true;
                    else ++r.other_states;
                    if (k.pos.x > op.x + 80.0f) ++r.records_after;
                }
            if (rep) ++r.repulsion_frames;
            const V2 fw = dir_from_deg(k.yaw);
            for (ObjectId id = 1; id <= 9; ++id)
                if (length_sq(sys.team_member_avoid(id, fw)) > 0.0f) ++r.vec_frames;
        }
        r.obstacle_moved = length(sys.view(oid).pos - o0);
        r.st = sys.stats();
        StateHasher h;
        sys.hash_units(h);
        r.crc = h.digest();
        return r;
    };
    for (int obstacle = 0; obstacle < 2; ++obstacle) {
        const Out s = through(obstacle, serial_for), p = through(obstacle, make_thread_pf(4));
        CHECK(s.crc == p.crc);
        CHECK(s.arrived && s.off_line < 1e-4f);                 // on its path, straight through the footprint
        CHECK(near(s.min_cruise, vmax, 1e-5f));                 // never slowed
        CHECK(s.repulsion_frames > 0 && s.other_states == 0 && s.records_after == 0);  // Repulsion while passing, then gone
        CHECK(s.vec_frames > 0);                                // the members side-step
        CHECK(s.obstacle_moved == 0.0f);                        // the standing unit / squad stays where it is
        CHECK(s.st.dynamic_repaths == 0 && s.st.pushes == 0 && s.st.early_arrivals == 0);
        std::printf("land squad repulsion: obstacle %d - Repulsion %u frames, member vectors %u, off its line %.5f\n", obstacle,
                    s.repulsion_frames, s.vec_frames, static_cast<double>(s.off_line));
    }
    {  // blocked destination: a standing infantry unit on the spot the squad is ordered to -> the early-arrival cut
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        t16_squad(sys, at, 0, 0.8f, 0.0f, t18_storm(), 1);
        const V2 goal = cell_center(25, 20);
        UnitDesc x = unit(70, goal, 2, 1.0f, 6.0f, LocomotorKind::Default);
        x.owner = 1;
        CHECK(sys.add_unit(x));
        SimRng rng(72);
        sys.tick(1, rng, serial_for);
        MoveOrder o;
        o.units = {1};
        o.destination = goal;
        sys.move(o, rng, 2);
        bool arrived = false;
        for (uint32_t f = 2; f < 900 && !arrived; ++f) {
            sys.tick(f, rng, serial_for);
            arrived = sys.view(100).arrived;
        }
        const UnitView k = sys.view(100);
        // the box (15 + 5, 15) at the stop point clears the unit's circle of 6: the centre at least 26 before it; the
        // search steps back by 5 and the cut takes 3 more, the walk stops within 5 of the cut path's end
        const float gap = goal.x - k.pos.x;
        CHECK(arrived && sys.stats().early_arrivals == 1 && sys.stats().dynamic_repaths == 0 && sys.stats().pushes == 0);
        CHECK(gap >= 26.0f && gap < 26.0f + 5.0f + 3.0f + 5.0f + 1.0f && std::fabs(k.pos.y - goal.y) < 1e-3f);
        CHECK(sys.view(70).pos == goal);
        std::printf("land squad repulsion: blocked destination - stopped %.2f before the standing unit\n", static_cast<double>(gap));
    }
    {  // two squads head-on (MovingVsMoving, opposed headings): Repulse - both on their lines at full speed, both arrive
        struct Res {
            uint64_t crc = 0;
            float off = 0.0f, min_a = 3.0e38f, min_b = 3.0e38f;
            uint32_t repulse = 0, other = 0, vec = 0;
            bool arrived = false;
        };
        const auto run = [&](const ParallelFor& pf) {
            Res r;
            MovementSystem sys{MovementConfig{}};
            land_system(sys, 100);
            const V2 pa = cell_center(10, 20), pb = cell_center(35, 20);
            t18_squad(sys, 100, pa, 0.0f, 1);
            t18_squad(sys, 200, pb, 180.0f, 1);
            SimRng rng(73);
            sys.tick(1, rng, pf);
            MoveOrder oa, ob;
            oa.units = {101};
            oa.destination = pb;
            ob.units = {201};
            ob.destination = pa;
            sys.move(oa, rng, 2);
            sys.move(ob, rng, 2);
            bool ca = false, cb = false;
            for (uint32_t f = 2; f < 900 && !r.arrived; ++f) {
                sys.tick(f, rng, pf);
                const UnitView ka = sys.view(100), kb = sys.view(200);
                r.arrived = ka.arrived && kb.arrived;
                r.off = std::max(r.off, std::max(std::fabs(ka.pos.y - pa.y), std::fabs(kb.pos.y - pa.y)));
                ca = ca || near(ka.speed, vmax, 1e-5f);
                cb = cb || near(kb.speed, vmax, 1e-5f);
                if (ca && ka.pos.x < pb.x - 60.0f) r.min_a = std::min(r.min_a, ka.speed);
                if (cb && kb.pos.x > pa.x + 60.0f) r.min_b = std::min(r.min_b, kb.speed);
                for (const CollisionObject& c : sys.collisions().list()) {
                    if (c.kind == CollisionKind::MovingVsMoving && c.state == CollisionState::Repulsion) ++r.repulse;
                    else ++r.other;
                }
                const V2 fw = dir_from_deg(ka.yaw);
                for (ObjectId id = 101; id <= 104; ++id)
                    if (length_sq(sys.team_member_avoid(id, fw)) > 0.0f) ++r.vec;
            }
            StateHasher h;
            sys.hash_units(h);
            r.crc = h.digest();
            return r;
        };
        const Res s = run(serial_for), p = run(make_thread_pf(4));
        CHECK(s.crc == p.crc);
        CHECK(s.arrived && s.off < 1e-4f && s.repulse > 0 && s.other == 0 && s.vec > 0);
        CHECK(near(s.min_a, vmax, 1e-5f) && near(s.min_b, vmax, 1e-5f));  // neither is slowed or held
        std::printf("land squad repulsion: head-on squads - Repulse %u frames, member vectors %u\n", s.repulse, s.vec);
    }
    {  // two squads on the same heading, the rear one faster. s78n t23 follow-up (MOVEMENT 5.4.6 "State 2 service", RE 2):
       // different formations, so the chooser's Speed Slave Only comes from the relation path, and its service re-run
       // reads the record's own stored answer (0) for p0 and bails to state 0 every frame - the rear squad is never
       // slowed by the record (answered max speed) and walks up to the front one (before: it followed, never overlapping)
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        const V2 pa = cell_center(10, 20), pb = cell_center(14, 20);
        t18_squad(sys, 100, pa, 0.0f, 1);            // rear: team speed 0.8 x 1.56
        t18_squad(sys, 200, pb, 0.0f, 1, 0.4f);      // front: its members cap the team at 0.4 x 1.56
        SimRng rng(74);
        sys.tick(1, rng, serial_for);
        MoveOrder oa, ob;
        oa.units = {101};
        oa.destination = cell_center(45, 20);
        ob.units = {201};
        ob.destination = cell_center(48, 20);
        sys.move(oa, rng, 2);
        sys.move(ob, rng, 2);
        uint32_t slave = 0, records = 0, full = 0;
        float min_gap = 3.0e38f;
        for (uint32_t f = 2; f < 500; ++f) {
            sys.tick(f, rng, serial_for);
            const UnitView ka = sys.view(100), kb = sys.view(200);
            min_gap = std::min(min_gap, kb.pos.x - ka.pos.x);
            for (const CollisionObject& c : sys.collisions().list()) {
                if (c.kind != CollisionKind::MovingVsMoving) continue;
                ++records;
                if (c.state == CollisionState::SpeedSlave) ++slave;
                if (c.state == CollisionState::Active && near(ka.speed, vmax, 1e-5f)) ++full;
            }
        }
        CHECK(records > 0 && slave == 0 && full > 0);
        CHECK(min_gap < 40.0f - 10.0f);  // the rear squad closed up from its start gap of 40
        std::printf("land squad repulsion: following squads - MovingVsMoving record %u frames, Speed Slave Only %u, rear at full "
                    "speed while undecided %u, closest %.2f\n",
                    records, slave, full, static_cast<double>(min_gap));
    }
    {  // against a standing friendly vehicle on the path (destination free): Path Around - the vehicle is never moved, the
       // mover's members get no vector from that record (the mover side gives one only in Repulsion)
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        t16_squad(sys, at, 0, 0.8f, 0.0f, t18_storm(), 1);
        const V2 vp{at.x + 200.0f, at.y};
        UnitDesc v = unit(70, vp, 2, 1.0f, 20.0f, LocomotorKind::Default);
        v.collision_class = kUccVehicle;
        v.owner = 1;
        CHECK(sys.add_unit(v));
        SimRng rng(75);
        sys.tick(1, rng, serial_for);
        MoveOrder o;
        o.units = {1};
        o.destination = {at.x + 500.0f, at.y};
        sys.move(o, rng, 2);
        bool arrived = false, vectors = false, around = false;
        for (uint32_t f = 2; f < 1200 && !arrived; ++f) {
            sys.tick(f, rng, serial_for);
            arrived = sys.view(100).arrived;
            for (const CollisionObject& c : sys.collisions().list()) {
                CHECK(c.state != CollisionState::Repulsion);  // non-infantry pairs never get Repulsion
                around = around || c.state == CollisionState::PathAround;
            }
            const V2 fw = dir_from_deg(sys.view(100).yaw);
            for (ObjectId id = 1; id <= 9; ++id) vectors = vectors || length_sq(sys.team_member_avoid(id, fw)) > 0.0f;
        }
        CHECK(arrived && around && !vectors && sys.stats().dynamic_repaths >= 1 && sys.stats().pushes == 0);
        CHECK(sys.view(70).pos == vp);
    }
    {  // a vehicle (no crusher) against a standing squad: Path Around; the container is never displaced in one step
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        t16_squad(sys, cell_center(20, 20), 0, 0.8f, 0.0f, t18_storm(), 1);
        UnitDesc v = unit(70, cell_center(8, 20), 2, 1.0f, 20.0f, LocomotorKind::Default);
        v.collision_class = kUccVehicle;
        v.owner = 1;
        CHECK(sys.add_unit(v));
        SimRng rng(76);
        sys.tick(1, rng, serial_for);
        MoveOrder o;
        o.units = {70};
        o.destination = cell_center(35, 20);
        sys.move(o, rng, 2);
        float max_step = 0.0f;
        bool arrived = false;
        for (uint32_t f = 2; f < 1200 && !arrived; ++f) {
            const V2 k0 = sys.view(100).pos;
            sys.tick(f, rng, serial_for);
            max_step = std::max(max_step, length(sys.view(100).pos - k0));
            arrived = sys.view(70).arrived;
        }
        CHECK(arrived && max_step <= vmax + 1e-4f);
    }
    {  // a crusher against a hostile squad that cannot be pushed (Max_Speed 0): one push attempt, then it drives on at
       // full speed - no hold, no arrival cut, no repath, no retry
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        t18_squad(sys, 200, cell_center(25, 20), 0.0f, 1, 0.0f);
        UnitDesc cr = unit(60, cell_center(8, 20), 2, 1.0f, 10.0f, LocomotorKind::Default);
        cr.collision_class = kUccVehicle;
        cr.crusher = true;
        cr.owner = 2;
        CHECK(sys.add_unit(cr));
        SimRng rng(77);
        sys.tick(1, rng, serial_for);
        MoveOrder o;
        o.units = {60};
        o.destination = cell_center(45, 20);
        sys.move(o, rng, 2);
        bool arrived = false, cruising = false, crusher_state = false;
        float min_speed = 3.0e38f;
        for (uint32_t f = 2; f < 1200 && !arrived; ++f) {
            sys.tick(f, rng, serial_for);
            const UnitView c = sys.view(60);
            arrived = c.arrived;
            cruising = cruising || near(c.speed, 1.56f, 1e-5f);
            if (cruising && c.pos.x < cell_center(44, 20).x) min_speed = std::min(min_speed, c.speed);
            for (const CollisionObject& co : sys.collisions().list()) crusher_state = crusher_state || co.state == CollisionState::Crusher;
        }
        const MovementStats& st = sys.stats();
        std::printf("land squad repulsion: crusher - arrived %d, state 4 seen %d, min cruise %.3f, push failures %u, pushes %u\n",
                    arrived ? 1 : 0, crusher_state ? 1 : 0, static_cast<double>(min_speed), st.push_failures, st.pushes);
        CHECK(arrived && crusher_state && near(min_speed, 1.56f, 1e-5f));
        CHECK(st.push_failures == 1 && st.pushes == 0 && st.early_arrivals == 0 && st.dynamic_repaths == 0);
        CHECK(length(sys.view(60).pos - cell_center(45, 20)) < 1.0f);
    }
    {  // Try Again Later: a squad on a forced path (flag 0x20) against a vehicle that cannot be pushed (Max_Speed 0) - the
       // speed answer 0 is taken at the braking rate, not at once, and the record is re-decided every 30 frames
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        t16_squad(sys, at, 0, 0.8f, 0.0f, t18_storm(), 1);
        UnitDesc v = unit(70, {at.x + 250.0f, at.y}, 2, 0.0f, 20.0f, LocomotorKind::Default);
        v.collision_class = kUccVehicle;
        v.owner = 1;
        CHECK(sys.add_unit(v));
        SimRng rng(78);
        sys.tick(1, rng, serial_for);
        MoveOrder o;
        o.units = {1};
        o.destination = {at.x + 500.0f, at.y};
        o.flags = kMoveFlagSkipValidation;
        sys.move(o, rng, 2);
        std::vector<float> v_at;
        bool tal = false;
        uint32_t crowd_frames = 0, crowd_other = 0;
        for (uint32_t f = 2; f < 600; ++f) {
            sys.tick(f, rng, serial_for);
            v_at.push_back(sys.view(100).speed);
            for (const CollisionObject& c : sys.collisions().list()) {
                tal = tal || c.state == CollisionState::TryAgainLater;
                // s78n t18 review (MOVEMENT 5.3.2, the crowd box query on layer 0x10): the 9-member squad counts once, as
                // its container, with the vehicle: 5 x 2 (the members are no layer objects: 5 x 11 would be 55)
                if (c.a == 100 && c.b == 70) {
                    if (c.crowd == 10.0f) ++crowd_frames;
                    else ++crowd_other;
                }
            }
        }
        CHECK(crowd_frames > 100 && crowd_other == 0);
        size_t top = 0;
        while (top < v_at.size() && !near(v_at[top], vmax, 1e-5f)) ++top;
        size_t d0 = top;
        while (d0 + 1 < v_at.size() && near(v_at[d0 + 1], vmax, 1e-5f)) ++d0;  // the last cruise frame
        bool steps = d0 + 17 < v_at.size();
        for (size_t i = 1; steps && i <= 16; ++i) steps = near(v_at[d0 + i], vmax - a * static_cast<float>(i), 1e-4f);
        CHECK(tal && steps && v_at.back() == 0.0f && sys.view(100).has_goal);  // 16 steps of the rate down to 0, then held
        CHECK(sys.stats().push_failures >= 3 && sys.stats().push_failures <= 2 + 600 / 30);  // re-decided every 30 frames
        CHECK(sys.view(100).pos.x < at.x + 250.0f - 35.0f);  // it stopped before the vehicle
        CHECK(sys.view(70).pos == (V2{at.x + 250.0f, at.y}));
    }
    {  // single infantry against single infantry: Repulsion at once - no repath, no push; on a leg that is not its last
       // the vector bends the mover's own heading (MOVEMENT 5.4.1 "How Walk combines the vector")
        MovementSystem sys{MovementConfig{}};
        PassabilityGrid g = open_grid(100, 100);
        for (int32_t y = 10; y <= 40; ++y) g.set_base(30, y, kPassImpassable);  // a wall: the path has two legs or more
        sys.set_grid(std::move(g));
        sys.set_anim_length_fn(anim_len);
        CHECK(sys.add_unit(unit(1, cell_center(20, 20), 2, 1.0f, 6.0f, LocomotorKind::Default)));
        SimRng rng(79);
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(45, 25);
        sys.move(o, rng, 1);
        uint32_t f = 1;
        for (; f < 200 && !(sys.view(1).speed > 0.0f); ++f) sys.tick(f, rng, serial_for);
        const std::vector<V2> path(sys.land_path(1).begin(), sys.land_path(1).end());
        CHECK(path.size() >= 3);
        if (path.size() >= 3) {
            const V2 t = normalize_retail(path[1] - path[0]), left{-t.y, t.x};
            const V2 spot = path[0] + (path[1] - path[0]) * 0.5f + left * -2.0f;  // on the first leg, 2 to its right
            CHECK(sys.add_unit(unit(2, spot, 2, 1.0f, 6.0f, LocomotorKind::Default)));
            float bend = 0.0f;
            uint32_t repulsion = 0, other = 0;
            bool arrived = false;
            for (; f < 1500 && !arrived; ++f) {
                sys.tick(f, rng, serial_for);
                const UnitView v = sys.view(1);
                arrived = v.arrived;
                if (dot(v.pos - path[0], t) < length(path[1] - path[0]) * 0.75f) bend = std::max(bend, dot(v.pos - path[0], left));
                for (const CollisionObject& c : sys.collisions().list()) {
                    if (c.a == 1 && c.b == 2 && c.state == CollisionState::Repulsion) ++repulsion;
                    else ++other;
                }
            }
            CHECK(arrived && length(sys.view(1).pos - cell_center(45, 25)) < 1.0f);
            CHECK(repulsion > 0 && other == 0 && bend > 0.5f);  // it side-stepped to the left, away from the unit on its right
            CHECK(sys.view(2).pos == spot && sys.stats().dynamic_repaths == 0 && sys.stats().pushes == 0);
            std::printf("land squad repulsion: single infantry - Repulsion %u frames, side step up to %.2f\n", repulsion,
                        static_cast<double>(bend));
        }
    }
}
