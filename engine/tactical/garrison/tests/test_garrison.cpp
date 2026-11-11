// alamo_tactical_garrison_tests (S4, docs/re/L1_S4_REMAINING.md §3.5 / §2.5 / §4.2): GarrisonSystem (masks with the
// zero-mask rule, allied / neutral hosts, Lua Can_Garrison capacity rule with reserved walkers vs the entry rule,
// approach point, entering delay, exit point, eject on host death / owner change, kill for CanContainGarrison-only
// hosts, Leave_Garrison cancelling an approach, Can_Garrison_Fire, hash determinism), CapturePointSystem (start gate,
// cadence, first-candidate control, contested, decay vs sticky, two-step capture of an enemy point, garrison rule b),
// and the descriptors read from type XML (garrison_types.hpp).
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_types.hpp"
#include "alamo/tactical/garrison/capture.hpp"
#include "alamo/tactical/garrison/garrison.hpp"
#include "alamo/tactical/garrison/garrison_types.hpp"
#include "alamo/test/support.hpp"

using namespace alamo;
using namespace alamo::tactical::garrison;
namespace fs = std::filesystem;

namespace {

using ::alamo::test::g_fail;
using ::alamo::test::g_checks;

constexpr uint64_t kInf = 1, kVeh = 2;

struct World {
    std::vector<std::pair<ObjectId, Vec3>> pos;
    bool get(ObjectId id, Vec3& out, Vec3& facing) const {
        for (const auto& p : pos)
            if (p.first == id) {
                out = p.second;
                facing = {1.0f, 0.0f, 0.0f};
                return true;
            }
        return false;
    }
    void set(ObjectId id, Vec3 v) {
        for (auto& p : pos)
            if (p.first == id) {
                p.second = v;
                return;
            }
        pos.emplace_back(id, v);
    }
    void erase(ObjectId id) {
        for (size_t i = 0; i < pos.size(); ++i)
            if (pos[i].first == id) {
                pos.erase(pos.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
    }
};

void test_rules() {
    GarrisonSystem g;
    g.set_neutral_player(9);
    g.set_allied([](int32_t a, int32_t b) { return (a == 1 && b == 3) || (a == 3 && b == 1); });
    HolderDesc bunker;
    bunker.kind = HolderKind::Structure;
    bunker.accepts = kInf;
    bunker.slots = 3;
    bunker.exit_dist = 50.0f;
    bunker.can_fire = true;
    bunker.extent_len = 20.0f;
    bunker.radius = 30.0f;
    HolderDesc jug;
    jug.kind = HolderKind::Vehicle;
    jug.accepts = kInf | kVeh;
    jug.slots = 10;
    jug.radius = 30.0f;
    jug.enter_dist = 15.0f;
    HolderDesc box = bunker;  // CanContainGarrison only
    box.kind = HolderKind::Property;
    box.accepts = 0;          // zero mask matches everything
    box.can_fire = false;
    CHECK(g.add_holder(10, 1, bunker) && !g.add_holder(10, 1, bunker));
    CHECK(g.add_holder(20, 1, jug) && g.add_holder(30, 9, box) && g.add_holder(40, 2, bunker));
    const OccupantDesc trooper{kInf, 1}, dt3{kVeh, 2};
    CHECK(g.add_unit(1, 1, trooper) && g.add_unit(2, 1, trooper) && g.add_unit(3, 1, dt3) && g.add_unit(4, 2, trooper));
    CHECK(g.add_unit(5, 3, trooper) && g.add_unit(6, 1, trooper) && g.add_unit(20, 1, dt3));
    // Masks (zero mask always matches), allied / neutral / hostile hosts, the unit itself.
    CHECK(g.masks_match(1, 10) && !g.masks_match(3, 10) && g.masks_match(3, 20) && g.masks_match(3, 30));
    CHECK(g.can_garrison(1, 10) && g.can_garrison(5, 10) && !g.can_garrison(4, 10) && g.can_garrison(4, 30) && !g.can_garrison(1, 40));
    CHECK(!g.can_garrison(20, 20) && !g.can_garrison(3, 10));
    // s78n t18 review: enter_allowed = order_enter's test without the order (the host refuses before it sends units
    // walking): own / allied host yes, hostile host no, mask mismatch no, unknown ids and the unit itself no; no state.
    CHECK(g.enter_allowed(1, 10) && g.enter_allowed(5, 10) && !g.enter_allowed(4, 10) && g.enter_allowed(4, 30));
    CHECK(!g.enter_allowed(3, 10) && !g.enter_allowed(1, 999) && !g.enter_allowed(999, 10) && !g.enter_allowed(20, 20));
    CHECK(g.pending_holder(1) == kNoObject && g.pending_holder(4) == kNoObject && g.reserved(10) == 0);
    // Lua capacity rule: free - reserved - value > 0. The bunker (3 slots): walkers reserve.
    CHECK(g.order_enter(1, 10) && g.order_enter(2, 10) && g.reserved(10) == 2);
    CHECK(!g.can_garrison(6, 10));  // 3 - 2 - 1 = 0: Can_Garrison false ...
    CHECK(g.enter_allowed(6, 10));  // (no capacity test at order time)
    CHECK(g.order_enter(6, 10));    // ... but the order is accepted (no capacity test at order time)
    CHECK(!g.order_enter(4, 10));   // hostile host
    // s78n t20 batch 2: a host without entry bones - the approach move goes to host + facing x half the host box's
    // half-extent length (no Enter_Dist; 40 -> (20, 0)), the range test reads A = the host position: 3-D |A - P| - r <=
    // Garrison_Radius (30, r 5: 34 enters, 36 does not).
    World w;
    w.set(10, {0, 0, 0});
    w.set(1, {34, 0, 0});
    w.set(2, {36, 0, 0});
    w.set(6, {300, 0, 0});
    int32_t bone = 7;
    const Vec3 ad = g.approach_destination(10, {0, 0, 0}, {2, 0, 0}, {300, 0, 0}, 40.0f, {}, bone);
    CHECK(ad.x == 20.0f && ad.y == 0.0f && bone == -1);
    const Vec3 ap = g.approach_point(10, {0, 0, 0}, {2, 0, 0});
    CHECK(ap.x == 0.0f && ap.y == 0.0f && g.bone_of(1) == -1);
    auto pose = [&w](ObjectId id, Vec3& p, Vec3& f) { return w.get(id, p, f); };
    const ApproachFn r5 = [](ObjectId, ObjectId, ApproachInfo& ai) {
        ai.radius = 5.0f;  // the move still runs (no "finished" signal in this test)
        return true;
    };
    g.service(1, pose, r5);
    CHECK(g.entering(1) && !g.entering(2) && g.pending_holder(2) == 10 && g.holder_of(1) == kNoObject);
    CHECK(g.events().size() == 1 && g.events()[0].kind == GarrisonEventKind::EnterStarted && g.events()[0].unit == 1);
    CHECK(!g.cancel_enter(1) && !g.order_enter(1, 20));  // an entering unit is committed
    CHECK(!g.enter_allowed(1, 20) && g.enter_allowed(2, 20) == g.masks_match(2, 20));  // (a walking unit may be re-ordered)
    g.clear_events();
    g.service(30, pose, r5);
    CHECK(g.holder_of(1) == kNoObject);  // 30 frames of fade: enters at frame 31
    g.service(31, pose, r5);
    CHECK(g.holder_of(1) == 10 && g.events().size() == 1 && g.events()[0].kind == GarrisonEventKind::Entered &&
          g.used_slots(10) == 1 && g.free_slots(10) == 2);
    // Can_Garrison_Fire: holder flag and its occupants.
    CHECK(g.can_garrison_fire(10) && g.can_garrison_fire(1) && !g.can_garrison_fire(20) && !g.can_garrison_fire(2) &&
          !g.can_garrison_fire(30));
    // Leave_Garrison on a walker cancels; on an occupant exits to host + facing x Exit_Dist.
    g.clear_events();
    CHECK(g.order_exit(2, {0, 0, 0}, {1, 0, 0}, 32) && g.pending_holder(2) == kNoObject &&
          g.events()[0].kind == GarrisonEventKind::OrderDropped);
    CHECK(g.order_exit(1, {0, 0, 5}, {0, 3, 0}, 33) && g.holder_of(1) == kNoObject);
    CHECK(g.events()[1].kind == GarrisonEventKind::Exited && g.events()[1].position.x == 0.0f && g.events()[1].position.y == 50.0f &&
          g.events()[1].position.z == 5.0f);
    CHECK(!g.order_exit(1, {}, {}, 33));
    // Exit without Exit_Dist: the full box diagonal.
    const Vec3 ep = g.exit_point(30, {0, 0, 0}, {1, 0, 0});
    CHECK(ep.x == 50.0f);  // box has exit_dist 50 (copied); juggernaut: 2 x half diagonal 0 = 0
    // Capacity: the juggernaut takes 1 + 2 x 2 value; a full host drops the approach.
    w.set(20, {1000, 0, 0});
    w.set(3, {1015, 0, 0});
    CHECK(g.order_enter(3, 20));
    g.clear_events();
    g.service(40, pose, r5);
    CHECK(g.entering(3));
    g.service(70, pose, r5);
    CHECK(g.holder_of(3) == 20 && g.used_slots(20) == 2);
    // The walker 6 to the bunker: hostile owner change of the host drops it.
    g.clear_events();
    g.set_owner(10, 2);
    g.service(71, pose, r5);
    CHECK(g.pending_holder(6) == kNoObject && g.events().size() == 1 && g.events()[0].kind == GarrisonEventKind::OrderDropped);
    // Owner change of an occupied host to a hostile owner ejects (an allied one keeps them, test_owner_change); host
    // death ejects (behaviour) or kills (CanContainGarrison only).
    g.clear_events();
    g.set_owner(20, 2, {1000, 0, 0}, {1, 0, 0}, 72);
    CHECK(g.holder_of(3) == kNoObject && g.events().size() == 1 && g.events()[0].kind == GarrisonEventKind::Ejected &&
          g.events()[0].position.x == 1000.0f);
    g.set_owner(20, 3);  // empty host: no events
    CHECK(g.events().size() == 1);
    w.set(30, {500, 0, 0});
    w.set(4, {520, 0, 0});
    CHECK(g.order_enter(4, 30));
    g.service(80, pose, r5);
    g.service(110, pose, r5);
    CHECK(g.holder_of(4) == 30);
    g.clear_events();
    g.remove(30, {500, 0, 0}, {1, 0, 0}, 111);
    CHECK(!g.is_holder(30) && g.holder_of(4) == kNoObject && g.events().size() == 1 && g.events()[0].kind == GarrisonEventKind::Killed);
    CHECK(g.occupancy().empty());
    // Eject_Garrison.
    w.set(5, {22, 0, 0});
    g.set_owner(10, 3);
    CHECK(g.order_enter(5, 10));
    g.service(120, pose, r5);
    g.service(150, pose, r5);
    CHECK(g.holder_of(5) == 10 && g.occupancy().size() == 1);
    g.clear_events();
    CHECK(g.eject_all(10, {0, 0, 0}, {1, 0, 0}, 151) == 1 && g.events()[0].kind == GarrisonEventKind::Ejected);
    // A dead walker's host disappears: dropped.
    w.set(2, {30, 0, 0});
    CHECK(g.order_enter(2, 40) == false);  // hostile
    CHECK(g.order_enter(6, 20));
    g.clear_events();
    w.erase(20);
    g.service(160, pose, r5);
    CHECK(g.events().size() == 1 && g.events()[0].kind == GarrisonEventKind::OrderDropped);
}

uint64_t scripted_hash() {
    GarrisonSystem g;
    HolderDesc h;
    h.accepts = kInf;
    h.slots = 2;
    h.radius = 12.0f;
    g.add_holder(50, 0, h);
    for (ObjectId i = 1; i <= 4; ++i) g.add_unit(i, 0, OccupantDesc{kInf, 1});
    World w;
    w.set(50, {0, 0, 0});
    for (ObjectId i = 1; i <= 4; ++i) {
        w.set(i, {static_cast<float>(i) * 5.0f, 0, 0});
        g.order_enter(i, 50);
    }
    auto pose = [&w](ObjectId id, Vec3& p, Vec3& f) { return w.get(id, p, f); };
    for (uint32_t f = 1; f <= 40; ++f) g.service(f, pose);
    StateHasher hs;
    g.hash(hs);
    return hs.digest() ^ static_cast<uint64_t>(g.occupants(50).size());
}

// A1 t08: a host captured by its own garrison keeps it. Capture complete (§4.2) hands an occupied host to the first
// occupant's owner (rule b) and the host applies that owner change to the garrison: occupants allied with the new owner
// stay, the others are ejected (entry order, at the exit point); a hostile owner change (bribe / Change_Owner) or a
// neutral one ejects everyone.
void test_owner_change() {
    constexpr int32_t kNeutral = 9;
    auto allied13 = [](int32_t a, int32_t b) { return (a == 1 && b == 3) || (a == 3 && b == 1); };
    GarrisonSystem g;
    g.set_neutral_player(kNeutral);
    g.set_allied(allied13);
    HolderDesc bunker;
    bunker.accepts = kInf;
    bunker.slots = 4;
    bunker.exit_dist = 40.0f;
    CHECK(g.add_holder(10, kNeutral, bunker));
    const OccupantDesc trooper{kInf, 1};
    auto owner_of = [](ObjectId u) -> int32_t { return u == 1 ? 1 : (u == 2 ? 3 : 2); };  // units 1, 2, 3
    for (ObjectId u = 1; u <= 3; ++u) CHECK(g.add_unit(u, owner_of(u), trooper));
    // A neutral bunker is open to every side: owner 1, its ally 3 and the enemy 2 are all inside.
    CHECK(g.can_garrison(1, 10) && g.can_garrison(3, 10));
    CHECK(g.board(1, 10, {0, 0, 0}, 1) && g.board(2, 10, {0, 0, 0}, 1) && g.board(3, 10, {0, 0, 0}, 1));
    CHECK(g.occupants(10).size() == 3);

    // The garrison captures the point: controller = the first occupant's owner (1); capture completes at progress 1.
    CapturePointSystem c(kNeutral, 4, 0);
    c.set_allied(allied13);
    c.set_occupant_owner([&g, &owner_of](ObjectId point) -> int32_t {
        const auto in = g.occupants(point);
        return in.empty() ? -1 : owner_of(in.front());
    });
    CapturePointDesc d;
    d.radius = 100.0f;
    d.transition_seconds = 0.0f;  // instant
    CHECK(c.add_point(10, kNeutral, {0, 0, 0}, d));
    c.service(0, {});
    CHECK(c.events().size() == 1 && c.events()[0].old_owner == kNeutral && c.events()[0].new_owner == 1);
    g.clear_events();
    g.set_owner(10, c.events()[0].new_owner, {0, 0, 0}, {1, 0, 0}, 2);
    c.clear_events();
    // Own side (1) and its ally (3) stay; the enemy (2) is ejected to the exit point.
    CHECK(g.holder_of(1) == 10 && g.holder_of(2) == 10 && g.holder_of(3) == kNoObject);
    CHECK(g.events().size() == 1 && g.events()[0].kind == GarrisonEventKind::Ejected && g.events()[0].unit == 3 &&
          g.events()[0].owner == 2 && g.events()[0].position.x == 40.0f && g.events()[0].frame == 2);
    CHECK(g.occupants(10).size() == 2 && g.occupants(10)[0] == 1 && g.occupants(10)[1] == 2);
    // The garrison keeps control: no further capture while it stays inside.
    for (uint32_t f = 4; f <= 40; f += 4) c.service(f, {});
    CHECK(c.events().empty() && c.owner(10) == 1);
    // Owner change to an ally of the occupants: nobody leaves.
    g.clear_events();
    g.set_owner(10, 3, {0, 0, 0}, {1, 0, 0}, 50);
    CHECK(g.events().empty() && g.occupants(10).size() == 2);
    // The enemy captures the host (bribe / Change_Owner): every occupant hostile to it is ejected, in entry order.
    g.set_owner(10, 2, {100, 0, 0}, {0, 1, 0}, 60);
    CHECK(g.occupants(10).empty() && g.holder_of(1) == kNoObject && g.holder_of(2) == kNoObject);
    CHECK(g.events().size() == 2 && g.events()[0].unit == 1 && g.events()[1].unit == 2 &&
          g.events()[0].kind == GarrisonEventKind::Ejected && g.events()[1].kind == GarrisonEventKind::Ejected &&
          g.events()[0].position.x == 100.0f && g.events()[0].position.y == 40.0f);
    // A neutral new owner is allied with nobody: the enemy's own occupant leaves too.
    CHECK(g.board(3, 10, {100, 0, 0}, 61) && g.holder_of(3) == 10);
    g.clear_events();
    g.set_owner(10, kNeutral, {100, 0, 0}, {1, 0, 0}, 70);
    CHECK(g.holder_of(3) == kNoObject && g.events().size() == 1 && g.events()[0].kind == GarrisonEventKind::Ejected);
    // Same owner again: no change, no events.
    CHECK(g.board(1, 10, {100, 0, 0}, 71));
    g.clear_events();
    g.set_owner(10, kNeutral, {100, 0, 0}, {1, 0, 0}, 72);
    CHECK(g.events().empty() && g.holder_of(1) == 10);
}

void test_capture() {
    constexpr int32_t kNeutral = 9;
    CapturePointSystem c(kNeutral, 4, 8);
    c.set_allied([](int32_t a, int32_t b) { return (a == 1 && b == 3) || (a == 3 && b == 1); });
    CapturePointDesc d;
    d.radius = 100.0f;
    d.transition_seconds = 0.4f;  // step = 1 / 12 x 4 = 1/3
    CHECK(c.add_point(5, kNeutral, {0, 0, 0}, d) && !c.add_point(5, kNeutral, {0, 0, 0}, d));
    CHECK(c.add_point(6, 2, {1000, 0, 0}, d));  // enemy-held, not sticky
    std::vector<Influencer> inf = {{1, 0, {50, 0, 0}}, {2, 1, {1050, 0, 0}}};
    c.service(0, inf);  // neutral point: no gate; the owned point waits for frame 8
    CHECK(c.pending(5) == 0 && c.progress(5) > 0.33f && c.progress(5) < 0.34f && c.progress(6) == 0.0f);
    c.service(2, inf);  // not due (next = 4)
    CHECK(c.progress(5) > 0.33f && c.progress(5) < 0.34f);
    c.service(4, inf);
    c.service(8, inf);
    // 3 x 1/3 rounds below 1 in float: the capture completes at the 4th step (frame 12).
    const bool done8 = c.owner(5) == 0;
    c.service(12, inf);
    CHECK(c.owner(5) == 0 && !c.events().empty() && c.events()[0].point == 5 && c.events()[0].old_owner == kNeutral &&
          c.events()[0].new_owner == 0);
    (void)done8;
    // Point 6 (owner 2) with player 1 inside: controller neutral -> owner goes 2 -> neutral, then 1 controls.
    for (uint32_t f = 16; f <= 64; f += 4) c.service(f, inf);
    bool to_neutral = false, to_one = false;
    for (const CaptureEvent& e : c.events()) {
        to_neutral = to_neutral || (e.point == 6 && e.old_owner == 2 && e.new_owner == kNeutral);
        to_one = to_one || (e.point == 6 && e.old_owner == kNeutral && e.new_owner == 1);
    }
    CHECK(to_neutral && to_one && c.owner(6) == 1);
    c.clear_events();
    // Contested (0 and 1, not allied): the neutral controller pulls point 5 (owner 0) back; allied 1 + 3 keep point 6.
    inf = {{1, 0, {10, 0, 0}}, {2, 1, {20, 0, 0}}, {3, 1, {1000, 10, 0}}, {4, 3, {1000, 20, 0}}};
    c.service(68, inf);
    CHECK(c.pending(5) == kNeutral && c.progress(5) > 0.0f && c.pending(6) == 1 && c.progress(6) == 0.0f);
    // Owner alone again: progress falls back to 0.
    inf = {{1, 0, {10, 0, 0}}};
    c.service(72, inf);
    CHECK(c.progress(5) == 0.0f && c.pending(5) == 0);
    // No candidates: decay to neutral unless sticky.
    CapturePointDesc s = d;
    s.sticky = true;
    CHECK(c.add_point(7, 1, {5000, 0, 0}, s, 76));
    inf.clear();
    for (uint32_t f = 76; f <= 120; f += 4) c.service(f, inf);
    CHECK(c.owner(7) == 1 && c.owner(5) == kNeutral);
    // Rule (b): a garrison host with occupants is controlled by the first occupant's owner.
    c.set_occupant_owner([](ObjectId p) { return p == 7 ? 3 : -1; });
    for (uint32_t f = 124; f <= 160; f += 4) c.service(f, inf);
    CHECK(c.owner(7) == 3);
    c.set_owner(7, 2);
    CHECK(c.owner(7) == 2 && c.progress(7) == 0.0f && c.pending(7) == -1);
    // q6 t19 rule (a) (AI_GALACTIC_TOKENS §9.11, AI_TACTICAL §7.5.1 item 1): an occupied build pad keeps its owner whoever
    // stands on it - here an enemy (0) alone on pad 8 (owner 1, not sticky) changes nothing; emptied, it is contested.
    CHECK(c.add_point(8, 1, {9000, 0, 0}, d, 164));
    bool occupied = true;
    c.set_pad_occupied([&occupied](ObjectId p) { return p == 8 && occupied; });
    inf = {{11, 0, {9010, 0, 0}}};
    for (uint32_t f = 164; f <= 200; f += 4) c.service(f, inf);
    CHECK(c.owner(8) == 1 && c.progress(8) == 0.0f);
    occupied = false;
    for (uint32_t f = 204; f <= 260; f += 4) c.service(f, inf);
    CHECK(c.owner(8) == 0);
    StateHasher a, b;
    c.hash(a);
    c.hash(b);
    CHECK(a.digest() == b.digest());
}

// ---- N1 stage 2: save / load mid-battle (GarrisonSystem / CapturePointSystem write / read) ----

template <class S>
std::vector<uint8_t> bytes_of(const S& s) {
    ByteWriter w;
    s.write(w);
    return w.bytes();
}

template <class S>
uint64_t digest_of(const S& s) {
    StateHasher h;
    s.hash(h);
    return h.digest();
}

// Garrison scene: a 3-slot bunker (allied owners 1 / 3) and a juggernaut; walkers approach, enter, leave.
struct GarrisonScene {
    GarrisonSystem g;
    World w;
    GarrisonScene() {
        g.set_neutral_player(9);
        g.set_allied([](int32_t a, int32_t b) { return (a == 1 && b == 3) || (a == 3 && b == 1); });
        HolderDesc bunker;
        bunker.accepts = kInf;
        bunker.slots = 3;
        bunker.exit_dist = 50.0f;
        bunker.can_fire = true;
        bunker.extent_len = 20.0f;
        bunker.radius = 30.0f;  // s78n t20 batch 2: entering within 30 of the host (A = the host position)
        HolderDesc jug;
        jug.kind = HolderKind::Vehicle;
        jug.accepts = kInf | kVeh;
        jug.slots = 10;
        jug.radius = 30.0f;
        jug.enter_dist = 15.0f;
        g.add_holder(10, 1, bunker);
        g.add_holder(20, 3, jug);
        for (ObjectId i = 1; i <= 5; ++i) g.add_unit(i, i == 5 ? 3 : 1, OccupantDesc{kInf, 1});
        g.add_unit(6, 1, OccupantDesc{kVeh, 2});
        w.set(10, {0, 0, 0});
        w.set(20, {1000, 0, 0});
        for (ObjectId i = 1; i <= 6; ++i) w.set(i, {25.0f + 40.0f * static_cast<float>(i), 0, 0});
        for (ObjectId i = 1; i <= 4; ++i) g.order_enter(i, 10);
        g.order_enter(5, 10);
        g.order_enter(6, 20);
    }
    // Host side: walkers advance toward their host (deterministic in the frame), a unit leaves at frame 60.
    void step(uint32_t f) {
        for (ObjectId i = 1; i <= 6; ++i) {
            Vec3 p, fc;
            if (!w.get(i, p, fc)) continue;
            const float target = i == 6 ? 1015.0f : 20.0f;
            if (g.pending_holder(i) != kNoObject && !g.entering(i)) p.x = p.x > target ? std::max(target, p.x - 6.0f) : std::min(target, p.x + 30.0f);
            w.set(i, p);
        }
        if (f == 60) g.order_exit(2, {0, 0, 0}, {1, 0, 0}, f);
        if (f == 75) g.set_owner(20, 1, {1000, 0, 0}, {1, 0, 0}, f);
        auto pose = [this](ObjectId id, Vec3& p, Vec3& fc) { return w.get(id, p, fc); };
        g.service(f, pose);
    }
};

// s78n t12 review (HERO_CLASH §8.4.3, REMAINING_GAPS §5.1): the on-demand holder of a vehicle a thief boarded.
void test_vehicle_thief_holder() {
    GarrisonSystem g;
    g.set_allied([](int32_t a, int32_t b) { return a == b; });
    HolderDesc car;
    car.kind = HolderKind::VehicleThief;
    car.extent_len = 10.0f;
    OccupantDesc inf;
    inf.category = kInf;
    inf.value = 1;
    CHECK(g.add_holder(50, 1, car));  // the stolen vehicle (already the thief's owner's)
    CHECK(g.add_unit(1, 1, inf) && g.add_unit(2, 1, inf));  // the thief and an allied trooper
    const Vec3 at{100.0f, 50.0f, 0.0f}, face{0.0f, 1.0f, 0.0f};
    // closed to orders: nobody walks in, no free slot, the Lua rule says no
    CHECK(!g.can_garrison(1, 50) && !g.enter_allowed(1, 50) && !g.order_enter(1, 50) && g.pending_holder(1) == kNoObject);
    CHECK(g.capacity(50) == 0 && g.free_slots(50) == 0);
    // the boarding is the only way in
    CHECK(g.board(1, 50, at, 7) && g.holder_of(1) == 50 && g.occupants(50).size() == 1);
    CHECK(g.events().size() == 1 && g.events()[0].kind == GarrisonEventKind::Entered);
    g.clear_events();
    CHECK(!g.can_garrison(2, 50) && !g.order_enter(2, 50) && g.free_slots(50) == 0);
    // no ordinary leave, no Eject_Garrison
    CHECK(!g.order_exit(1, at, face, 8) && g.eject_all(50, at, face, 8) == 0u);
    CHECK(g.holder_of(1) == 50 && g.events().empty());
    // an owner change of the vehicle (even to a hostile owner) keeps the thief
    g.set_owner(50, 2, at, face, 9);
    CHECK(g.holder_of(1) == 50 && g.events().empty());
    // save / load keeps the kind (and so the rules)
    ByteWriter w;
    g.write(w);
    GarrisonSystem g2;
    g2.set_allied([](int32_t a, int32_t b) { return a == b; });
    ByteReader r(w.bytes());
    CHECK(g2.read(r) && bytes_of(g2) == bytes_of(g) && digest_of(g2) == digest_of(g));
    CHECK(g2.holder_desc(50) && g2.holder_desc(50)->kind == HolderKind::VehicleThief && !g2.order_exit(1, at, face, 9));
    {  // the hash tells a thief holder from an ordinary one with the same occupant
        GarrisonSystem p;
        HolderDesc box = car;
        box.kind = HolderKind::Vehicle;
        p.add_holder(50, 2, box);
        p.add_unit(1, 1, inf);
        p.add_unit(2, 1, inf);
        p.board(1, 50, at, 7);
        CHECK(digest_of(p) != digest_of(g));
    }
    // the vehicle dies: the thief is released alive at the vehicle's position (not killed, not at an exit point)
    for (GarrisonSystem* s : {&g, &g2}) {
        s->remove(50, at, face, 10);
        CHECK(!s->is_holder(50) && s->holder_of(1) == kNoObject && s->is_unit(1));
        CHECK(s->events().size() == 1);
        if (s->events().size() == 1) {
            const GarrisonEvent& e = s->events()[0];
            CHECK(e.kind == GarrisonEventKind::Ejected && e.unit == 1 && e.holder == 50 && e.frame == 10);
            CHECK(e.position.x == at.x && e.position.y == at.y);
        }
    }
    {  // the same order is taken by an ordinary garrison vehicle and refused by a thief holder
        GarrisonSystem q;
        q.set_allied([](int32_t a, int32_t b) { return a == b; });
        HolderDesc open = car;
        open.kind = HolderKind::Vehicle;
        open.slots = 4;
        q.add_holder(50, 1, open);
        q.add_holder(60, 1, car);
        q.add_unit(2, 1, inf);
        CHECK(q.order_enter(2, 50) && !q.order_enter(2, 60));
        CHECK(q.pending_holder(2) == 50);
    }
}

void test_garrison_save() {
    GarrisonScene a;
    for (uint32_t f = 1; f <= 40; ++f) {
        a.g.clear_events();
        a.step(f);
    }
    // Mid-way: some units inside, some entering or walking.
    uint32_t inside = 0, pending = 0;
    for (ObjectId i = 1; i <= 6; ++i) {
        inside += a.g.garrisoned(i) ? 1u : 0u;
        pending += a.g.pending_holder(i) != kNoObject ? 1u : 0u;
    }
    CHECK(pending > 0 && inside > 0);
    const std::vector<uint8_t> saved = bytes_of(a.g);
    // The first unit's state byte: version, neutral, holder count, 2 holders (46 bytes + 12 per entry bone (none here) + 4
    // per occupant), unit count, then id, owner, category, value.
    size_t unit0_state = 4 + 4 + 4;
    for (const ObjectId h : {10, 20}) unit0_state += 46 + 4 * a.g.occupants(h).size();
    unit0_state += 4 + 4 + 4 + 8 + 4;

    GarrisonScene b;
    ByteReader r(saved);
    CHECK(b.g.read(r));
    b.w = a.w;  // host-side positions
    CHECK(bytes_of(b.g) == saved);
    CHECK(digest_of(a.g) == digest_of(b.g));
    uint32_t diverged = 0, events = 0;
    for (uint32_t f = 41; f <= 140; ++f) {
        a.g.clear_events();
        b.g.clear_events();
        a.step(f);
        b.step(f);
        events += static_cast<uint32_t>(a.g.events().size());
        bool same = a.g.events().size() == b.g.events().size();
        for (size_t k = 0; same && k < a.g.events().size(); ++k)
            same = a.g.events()[k].kind == b.g.events()[k].kind && a.g.events()[k].unit == b.g.events()[k].unit &&
                   a.g.events()[k].frame == b.g.events()[k].frame;
        if (!same || digest_of(a.g) != digest_of(b.g) || a.g.occupancy() != b.g.occupancy()) ++diverged;
    }
    CHECK(diverged == 0 && events > 0);
    CHECK(bytes_of(a.g) == bytes_of(b.g));

    // Rejects leave the system unchanged.
    GarrisonScene c;
    const std::vector<uint8_t> before = bytes_of(c.g);
    uint32_t accepted = 0;
    for (size_t len = 0; len < saved.size(); ++len) {
        ByteReader t(std::span<const uint8_t>(saved.data(), len));
        if (c.g.read(t)) ++accepted;
    }
    CHECK(accepted == 0);
    std::vector<uint8_t> bad = saved;
    bad.push_back(0);
    ByteReader rb1(bad);
    CHECK(!c.g.read(rb1));
    bad = saved;
    bad[8] = 0x7F;  // holder count far past the data
    ByteReader rb2(bad);
    CHECK(!c.g.read(rb2));
    bad = saved;
    CHECK(bad[unit0_state] == 7 || bad[unit0_state] == 5 || bad[unit0_state] == 2 || bad[unit0_state] == 1);
    bad[unit0_state] = 3;  // not a GarrisonDataPack state
    ByteReader rb3(bad);
    CHECK(!c.g.read(rb3));
    for (const uint8_t v : {uint8_t{2}, uint8_t{4}}) {  // only the current version (3: s78n t20 batch 2) is read
        bad = saved;
        CHECK(bad[0] == 3);
        bad[0] = v;
        ByteReader rb4(bad);
        CHECK(!c.g.read(rb4));
    }
    {  // a holder kind past the last one is refused: the first holder's kind byte follows version, neutral, count, id, owner
        bad = saved;
        const size_t kind0 = 4 + 4 + 4 + 4 + 4;
        CHECK(bad[kind0] <= 4);
        bad[kind0] = 5;
        ByteReader rb5(bad);
        CHECK(!c.g.read(rb5));
    }
    CHECK(bytes_of(c.g) == before);
    ByteReader rok(saved);
    CHECK(c.g.read(rok) && bytes_of(c.g) == saved);
    std::printf("garrison save: %zu bytes at frame 40 (%u inside, %u walking / entering), 100 frames continued identically "
                "(%u events)\n",
                saved.size(), inside, pending, events);
}

struct CaptureScene {
    CapturePointSystem c{9, 4, 8};
    CaptureScene() {
        c.set_allied([](int32_t a, int32_t b) { return (a == 1 && b == 3) || (a == 3 && b == 1); });
        CapturePointDesc d;
        d.radius = 100.0f;
        d.transition_seconds = 2.0f;
        c.add_point(5, 9, {0, 0, 0}, d);
        c.add_point(6, 2, {1000, 0, 0}, d);
        CapturePointDesc s = d;
        s.sticky = true;
        c.add_point(7, 1, {5000, 0, 0}, s, 3);
    }
    void step(uint32_t f) {
        std::vector<Influencer> inf = {{1, 0, {50, 0, 0}}, {2, 1, {1050, 0, 0}}};
        if (f > 100) inf.push_back({3, 2, {20, 0, 0}});  // contested from frame 101
        c.service(f, inf);
    }
};

void test_capture_save() {
    CaptureScene a;
    for (uint32_t f = 0; f <= 40; ++f) a.step(f);
    CHECK(a.c.progress(5) > 0.0f && a.c.progress(5) < 1.0f);  // a capture in progress
    const std::vector<uint8_t> saved = bytes_of(a.c);
    CaptureScene b;
    ByteReader r(saved);
    CHECK(b.c.read(r));
    CHECK(bytes_of(b.c) == saved && digest_of(a.c) == digest_of(b.c));
    uint32_t diverged = 0;
    for (uint32_t f = 41; f <= 300; ++f) {
        a.step(f);
        b.step(f);
        if (digest_of(a.c) != digest_of(b.c) || a.c.events().size() != b.c.events().size()) ++diverged;
    }
    CHECK(diverged == 0 && !a.c.events().empty());
    CHECK(bytes_of(a.c) == bytes_of(b.c));
    // Rejects (truncation, interval 0, trailing data) leave it unchanged.
    CaptureScene c;
    const std::vector<uint8_t> before = bytes_of(c.c);
    uint32_t accepted = 0;
    for (size_t len = 0; len < saved.size(); ++len) {
        ByteReader t(std::span<const uint8_t>(saved.data(), len));
        if (c.c.read(t)) ++accepted;
    }
    CHECK(accepted == 0);
    std::vector<uint8_t> bad = saved;
    for (size_t i = 8; i < 12; ++i) bad[i] = 0;  // interval 0
    ByteReader rb1(bad);
    CHECK(!c.c.read(rb1));
    bad = saved;
    bad.push_back(1);
    ByteReader rb2(bad);
    CHECK(!c.c.read(rb2));
    CHECK(bytes_of(c.c) == before);
    ByteReader rok(saved);
    CHECK(c.c.read(rok) && bytes_of(c.c) == saved);
    std::printf("capture save: %zu bytes at frame 40, 260 frames continued identically (%zu capture events)\n",
                saved.size(), a.c.events().size());
}

using ::alamo::test::write_file;

void test_types() {
    const fs::path root = fs::temp_directory_path() / "alamo_tactical_garrison_tests";
    std::error_code ec;
    remove_all_long(root, ec);
    write_file(root / "Data/XML/GameObjectFiles.xml", "<L><File>G.xml</File></L>");
    write_file(root / "Data/XML/G.xml",
               "<R>\n"
               "<GroundVehicle Name=\"Jug\"><LandBehavior>WALK_LOCOMOTOR, GARRISON_VEHICLE</LandBehavior>"
               "<Garrison_Category> Infantry, Vehicle </Garrison_Category><Num_Garrison_Slots>10</Num_Garrison_Slots>"
               "<Garrison_Exit_Dist>5</Garrison_Exit_Dist><Garrison_Enter_Dist>7</Garrison_Enter_Dist><Garrison_Radius>30</Garrison_Radius>"
               "<Property_Flags>GoodInfantryCrusher | CanContainGarrison</Property_Flags><CategoryMask> Vehicle </CategoryMask></GroundVehicle>\n"
               "<SpecialStructure Name=\"Bunker\"><LandBehavior>REVEAL, GARRISON_STRUCTURE</LandBehavior>"
               "<Garrison_Category> Infantry </Garrison_Category><Garrison_Exit_Dist>50</Garrison_Exit_Dist>"
               "<Property_Flags>GarrisonCanFire</Property_Flags></SpecialStructure>\n"
               "<GroundVehicle Name=\"Transport\"><Property_Flags> CanContainGarrison </Property_Flags>"
               "<Garrison_Category>Infantry</Garrison_Category></GroundVehicle>\n"
               "<GroundVehicle Name=\"DT1\"><LandBehavior>WALK_LOCOMOTOR, GARRISON_UNIT</LandBehavior>"
               "<Garrison_Category> Infantry </Garrison_Category><Garrison_Value>2</Garrison_Value>"
               "<CategoryMask> Vehicle | AntiInfantry</CategoryMask><Influences_Capture_Point>False</Influences_Capture_Point></GroundVehicle>\n"
               "<GroundInfantry Name=\"Plain\"><CategoryMask>Infantry</CategoryMask></GroundInfantry>\n"
               "<SecondaryStructure Name=\"Tower\"><Behavior> CAPTURE_POINT, REVEAL, SELECTABLE</Behavior>"
               "<Capture_Point_Transition_Time_Seconds>10.0</Capture_Point_Transition_Time_Seconds>"
               "<Capture_Point_Radius>200.0</Capture_Point_Radius></SecondaryStructure>\n"
               "<Marker Name=\"RP\"><Behavior> CAPTURE_POINT, REVEAL, REINFORCEMENT_POINT </Behavior>"
               "<Capture_Point_Radius>100.0</Capture_Point_Radius></Marker>\n"
               "</R>");
    DirectoryProvider dp(path_to_utf8(root));
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    CHECK(raw.load(dp) && types.build(raw));
    const objects::TypeIndex jug = raw.find("Jug"), bunker = raw.find("Bunker"), tr = raw.find("Transport"), dt1 = raw.find("DT1"),
                             plain = raw.find("Plain"), tower = raw.find("Tower"), rp = raw.find("RP");
    HolderDesc h;
    CHECK(holder_desc_from_type(types, jug, h) && h.kind == HolderKind::Vehicle && h.slots == 10 && h.enter_dist == 7.0f &&
          h.exit_dist == 5.0f && h.radius == 30.0f && !h.can_fire);
    const uint64_t inf = h.accepts;
    CHECK(inf != 0);
    CHECK(holder_desc_from_type(types, bunker, h) && h.kind == HolderKind::Structure && h.can_fire && h.slots == 0);
    CHECK(holder_desc_from_type(types, tr, h) && h.kind == HolderKind::Property);
    CHECK(!holder_desc_from_type(types, plain, h) && !holder_desc_from_type(types, objects::kNone, h));
    OccupantDesc o;
    CHECK(occupant_desc_from_type(types, dt1, o) && o.value == 2 && o.category != 0 && (o.category & inf) == o.category);
    CHECK(!occupant_desc_from_type(types, plain, o));
    CapturePointDesc cp;
    CHECK(capture_desc_from_type(types, tower, cp) && cp.radius == 200.0f && cp.transition_seconds == 10.0f && !cp.sticky);
    CHECK(capture_desc_from_type(types, rp, cp) && cp.sticky);
    CHECK(!capture_desc_from_type(types, plain, cp));
    CHECK(influences_capture(types, plain) && !influences_capture(types, dt1));
    remove_all_long(root, ec);
}

}  // namespace

// s78n t18 (FIGHTER_FLYING 7.4): a squad garrisons as one object - its team container. Units 11, 12, 13 are members of
// group 100 (value 1, category kInf); unit 20 is on its own.
static void test_groups() {
    GarrisonSystem g;
    g.set_group_fn([](ObjectId unit, GarrisonGroup& out) {
        if (unit < 11 || unit > 13) return false;
        out.id = 100;
        out.value = 1;
        out.category = kInf;
        return true;
    });
    HolderDesc bunker;
    bunker.kind = HolderKind::Structure;
    bunker.accepts = kInf;
    bunker.slots = 2;
    bunker.radius = 30.0f;
    bunker.enter_dist = 40.0f;  // (no entry bones: A is the host position)
    bunker.exit_dist = 50.0f;
    CHECK(g.add_holder(1, 0, bunker));
    OccupantDesc inf;
    inf.category = kVeh;  // the members' own category is not read: the group's (the container's) is
    inf.value = 1;
    for (ObjectId id : {11, 12, 13}) CHECK(g.add_unit(id, 0, inf));
    OccupantDesc solo;
    solo.category = kInf;
    solo.value = 1;
    CHECK(g.add_unit(20, 0, solo));
    CHECK(g.masks_match(11, 1) && g.can_garrison(11, 1));
    for (ObjectId id : {11, 12, 13}) CHECK(g.order_enter(id, 1));
    CHECK(g.reserved(1) == 1);                    // the squad reserves once
    CHECK(!g.can_garrison(20, 1));                // 2 free - 1 reserved - 1 = 0: the strict Lua rule
    World w;
    w.set(1, {0.0f, 0.0f, 0.0f});
    w.set(100, {200.0f, 0.0f, 0.0f});             // the container: its position is what the range test reads
    w.set(11, {5.0f, 0.0f, 0.0f});                // a member standing at the host does not start the entry
    w.set(12, {210.0f, 5.0f, 0.0f});
    w.set(13, {190.0f, -5.0f, 0.0f});
    const PoseFn pose = [&w](ObjectId id, Vec3& p, Vec3& f) { return w.get(id, p, f); };
    float radius = 20.0f;
    bool arrived = false;
    // s78n t20 batch 2: the key of a squad is its container (100); a single unit is its own key
    const ApproachFn reach = [&](ObjectId key, ObjectId host, ApproachInfo& ai) {
        CHECK((key == 100 || key == 20) && host == 1);
        ai.radius = key == 100 ? radius : 0.0f;
        ai.moving = !arrived;
        return true;
    };
    g.service(1, pose, reach);
    CHECK(g.events().empty() && g.pending_holder(11) == 1 && !g.entering(11));
    // 3-D distance from the container to A (the host position, no entry bones) - r <= Garrison_Radius: 51 - 20 > 30...
    w.set(100, {51.0f, 0.0f, 0.0f});
    g.service(2, pose, reach);
    CHECK(g.events().empty());
    w.set(100, {0.0f, 0.0f, 49.0f});              // ...and the distance is 3-D: 49 up - 20 = 29
    g.service(3, pose, reach);
    CHECK(g.events().size() == 3);                // all three start entering in the same pass, ascending id
    for (size_t i = 0; i < g.events().size(); ++i)
        CHECK(g.events()[i].kind == GarrisonEventKind::EnterStarted && g.events()[i].unit == static_cast<ObjectId>(11 + i));
    CHECK(g.entering(11) && g.entering(12) && g.entering(13));
    g.clear_events();
    for (uint32_t f = 4; f < 3 + g.config().enter_frames; ++f) g.service(f, pose, reach);
    CHECK(g.events().empty() && !g.garrisoned(11));
    g.service(3 + g.config().enter_frames, pose, reach);
    CHECK(g.events().size() == 3 && g.garrisoned(11) && g.garrisoned(12) && g.garrisoned(13));  // inside together
    for (const GarrisonEvent& e : g.events()) CHECK(e.kind == GarrisonEventKind::Entered);
    g.clear_events();
    CHECK(g.used_slots(1) == 1 && g.free_slots(1) == 1);  // three members, one slot
    CHECK(g.can_garrison(20, 1) == false);                // 1 free - 0 reserved - 1 = 0 (strict)
    CHECK(g.order_enter(20, 1));                          // the entry rule itself: free >= value
    w.set(20, {20.0f, 0.0f, 0.0f});
    g.service(100, pose, reach);
    for (uint32_t f = 101; f <= 100 + g.config().enter_frames; ++f) g.service(f, pose, reach);
    CHECK(g.garrisoned(20) && g.free_slots(1) == 0);
    // a second squad finds no slot: its members are dropped together
    GarrisonSystem g2;
    g2.set_group_fn([](ObjectId unit, GarrisonGroup& out) {
        out.id = unit < 20 ? 100 : 200;
        out.value = 1;
        out.category = kInf;
        return true;
    });
    bunker.slots = 1;
    CHECK(g2.add_holder(1, 0, bunker));
    for (ObjectId id : {11, 12, 21, 22}) CHECK(g2.add_unit(id, 0, inf));
    for (ObjectId id : {11, 12, 21, 22}) CHECK(g2.order_enter(id, 1));
    CHECK(g2.reserved(1) == 2);
    World w2;
    w2.set(1, {0.0f, 0.0f, 0.0f});
    w2.set(100, {20.0f, 0.0f, 0.0f});
    w2.set(200, {400.0f, 0.0f, 0.0f});
    for (ObjectId id : {11, 12, 21, 22}) w2.set(id, {20.0f, 0.0f, 0.0f});
    const PoseFn pose2 = [&w2](ObjectId id, Vec3& p, Vec3& f) { return w2.get(id, p, f); };
    radius = 0.0f;
    g2.service(1, pose2, {});                             // no hook: radius 0, the moves still run
    CHECK(g2.entering(11) && g2.entering(12) && !g2.entering(21));
    for (uint32_t f = 2; f <= 1 + g2.config().enter_frames; ++f) g2.service(f, pose2, {});
    CHECK(g2.garrisoned(11) && g2.garrisoned(12) && g2.free_slots(1) == 0);
    // squad 200 in the pass that took squad 100 in: free 0 < 1 -> both members dropped together
    CHECK(g2.pending_holder(21) == kNoObject && g2.pending_holder(22) == kNoObject);
    uint32_t dropped = 0;
    for (const GarrisonEvent& e : g2.events())
        if (e.kind == GarrisonEventKind::OrderDropped) {
            CHECK(e.frame == 1 + g2.config().enter_frames && (e.unit == 21 || e.unit == 22));
            ++dropped;
        }
    CHECK(dropped == 2);
    // the approach move ended short of the radius: inside 60.0 (0x140a28bcc) state 2 - the host stops and the container
    // gets a flag-0x20 move of the whole 2-D excess toward A (s78n t20 batch 2), entering on the way
    GarrisonSystem g3;
    g3.set_group_fn([](ObjectId, GarrisonGroup& out) {
        out.id = 100;
        return true;
    });
    bunker.slots = 5;
    bunker.radius = 5.0f;
    CHECK(g3.add_holder(1, 0, bunker));
    CHECK(g3.add_unit(11, 0, inf) && g3.order_enter(11, 1));  // (the default group category 0 matches every host)
    World w3;
    w3.set(1, {0.0f, 0.0f, 0.0f});
    w3.set(100, {55.0f, 0.0f, 0.0f});                     // 55 from A
    w3.set(11, {55.0f, 0.0f, 0.0f});
    const PoseFn pose3 = [&w3](ObjectId id, Vec3& p, Vec3& f) { return w3.get(id, p, f); };
    arrived = false;
    radius = 5.0f;
    g3.service(1, pose3, reach);
    CHECK(!g3.entering(11) && g3.state_of(11) == 1);      // still walking: 55 - 5 > 5
    arrived = true;
    g3.service(2, pose3, reach);                          // its move ended: 55 - 5 <= 60
    CHECK(!g3.entering(11) && g3.state_of(11) == 2 && g3.events().size() == 2);
    if (g3.events().size() == 2) {
        CHECK(g3.events()[0].kind == GarrisonEventKind::HostStop && g3.events()[0].unit == 11 && g3.events()[0].holder == 1);
        const GarrisonEvent& cm = g3.events()[1];
        // the whole 2-D excess e = 55 - 5 = 50 toward A: the container stops r = 5 short of it
        CHECK(cm.kind == GarrisonEventKind::CorrectiveMove && cm.unit == 11 && std::fabs(cm.position.x - 5.0f) < 1e-4f &&
              cm.position.y == 0.0f);
    }
    g3.clear_events();
    arrived = false;                                      // the host issued the corrective move
    w3.set(100, {5.0f, 0.0f, 0.0f});
    g3.service(3, pose3, reach);
    CHECK(g3.entering(11) && g3.events().size() == 1 && g3.events()[0].kind == GarrisonEventKind::EnterStarted);
}

// s78n t20 batch 2 (FIGHTER_FLYING §7.4 "Garrison approach in full"): one key (a single unit), host radius 10.
namespace {
struct ApproachScene {
    GarrisonSystem g;
    World w;
    ApproachInfo info;  // what the host answers for the key
    ApproachScene() {
        HolderDesc h;
        h.kind = HolderKind::Vehicle;
        h.accepts = kInf;
        h.slots = 4;
        h.radius = 10.0f;
        CHECK(g.add_holder(50, 1, h));
        CHECK(g.add_unit(1, 1, OccupantDesc{kInf, 1}));
        w.set(50, {0.0f, 0.0f, 0.0f});
        info.radius = 5.0f;
    }
    void service(uint32_t f) {
        const PoseFn pose = [this](ObjectId id, Vec3& p, Vec3& fc) { return w.get(id, p, fc); };
        g.service(f, pose, [this](ObjectId, ObjectId, ApproachInfo& ai) {
            ai = info;
            return true;
        });
    }
    size_t count(GarrisonEventKind k) const {
        size_t n = 0;
        for (const GarrisonEvent& e : g.events()) n += e.kind == k ? 1u : 0u;
        return n;
    }
};
}  // namespace

static void test_approach() {
    {  // the per-frame range test is 3-D: model-less unit (r = 0.0866), Garrison_Radius 10 (hand: 10.05 - 0.0866 <= 10;
       // 10.1 - 0.0866 > 10; sqrt(9.6^2 + 9) - 0.0866 = 9.971; sqrt(9.7^2 + 9) - 0.0866 = 10.067)
        const float r = std::sqrt(3.0f * 0.05f * 0.05f);
        const Vec3 at[4] = {{10.05f, 0.0f, 0.0f}, {10.1f, 0.0f, 0.0f}, {9.6f, 0.0f, 3.0f}, {9.7f, 0.0f, 3.0f}};
        const bool enters[4] = {true, false, true, false};
        for (int i = 0; i < 4; ++i) {
            ApproachScene s;
            s.info.radius = r;
            CHECK(s.g.order_enter(1, 50) && s.g.state_of(1) == 1);
            s.w.set(1, at[i]);
            s.service(1);
            CHECK(s.g.entering(1) == enters[i]);
        }
    }
    {  // finished at 3-D 40 with r 5: within 60 -> state 2, HostStop, a corrective move of the whole 2-D excess (35)
        ApproachScene s;
        CHECK(s.g.order_enter(1, 50));
        s.w.set(1, {0.0f, 40.0f, 0.0f});
        s.service(1);  // walking: 35 > 10
        CHECK(s.g.events().empty() && s.g.state_of(1) == 1);
        s.info.moving = false;
        s.service(2);
        CHECK(s.g.state_of(1) == 2 && s.g.events().size() == 2 && s.count(GarrisonEventKind::HostStop) == 1);
        if (s.g.events().size() == 2) {
            const GarrisonEvent& m = s.g.events()[1];
            CHECK(m.kind == GarrisonEventKind::CorrectiveMove && m.position.x == 0.0f && std::fabs(m.position.y - 5.0f) < 1e-5f);
            CHECK(std::fabs((40.0f - m.position.y) - 35.0f) < 1e-5f);  // length 35: the unit would stop r short of A
        }
        CHECK(s.g.reserved(50) == 1 && s.g.pending_holder(1) == 50);  // state 2 still reserves
        s.g.clear_events();
        s.info.moving = true;  // the corrective move runs; on the way, 3-D 15 - 5 <= 10 enters
        s.w.set(1, {0.0f, 20.0f, 0.0f});
        s.service(3);
        CHECK(s.g.events().empty());
        s.w.set(1, {0.0f, 15.0f, 0.0f});
        s.service(4);
        CHECK(s.g.entering(1) && s.g.events().size() == 1 && s.g.events()[0].kind == GarrisonEventKind::EnterStarted);
    }
    {  // state 2: the corrective move's end enters unconditionally (it ended short, e.g. blocked)
        ApproachScene s;
        CHECK(s.g.order_enter(1, 50));
        s.w.set(1, {50.0f, 0.0f, 0.0f});
        s.info.moving = false;
        s.service(1);
        CHECK(s.g.state_of(1) == 2 && s.count(GarrisonEventKind::CorrectiveMove) == 1);
        s.g.clear_events();
        s.info.moving = true;
        s.service(2);
        s.info.moving = false;  // ended at 3-D 50 - 5 = 45 > 10
        s.service(3);
        CHECK(s.g.entering(1) && s.count(GarrisonEventKind::EnterStarted) == 1 && s.count(GarrisonEventKind::HostStop) == 0);
    }
    {  // 2-D excess within the radius: HostStop and entering at once (no corrective move)
        ApproachScene s;
        CHECK(s.g.order_enter(1, 50));
        s.w.set(1, {14.0f, 0.0f, 20.0f});  // 3-D 24.4 - 5 > 10, 2-D 14 - 5 <= 10
        s.info.moving = false;
        s.service(1);
        CHECK(s.g.entering(1) && s.g.events().size() == 2 && s.g.events()[0].kind == GarrisonEventKind::HostStop &&
              s.g.events()[1].kind == GarrisonEventKind::EnterStarted);
    }
    for (const bool host_path : {true, false}) {  // finished at 70 (- 5 > 60): cancel with a host path, else re-issue
        ApproachScene s;
        CHECK(s.g.order_enter(1, 50));
        s.w.set(1, {70.0f, 0.0f, 0.0f});
        s.info.moving = false;
        s.info.host_path = host_path;
        s.service(1);
        CHECK(s.g.events().size() == 1);
        if (host_path) {
            CHECK(s.count(GarrisonEventKind::OrderDropped) == 1 && s.g.events()[0].was == 1 && s.g.state_of(1) == 0 &&
                  s.g.reserved(50) == 0);
        } else {
            CHECK(s.count(GarrisonEventKind::Reissue) == 1 && s.g.events()[0].unit == 1 && s.g.state_of(1) == 1);
            s.g.clear_events();
            s.service(2);  // no move in flight until the host re-orders: no second signal
            CHECK(s.g.events().empty());
        }
    }
    {  // eleven finished approaches with an unchanged A: ten Reissue, then OrderDropped; a changed A resets the count
        ApproachScene s;
        s.w.set(50, {0.0f, 1.0f, 0.0f});  // (off the origin: the memo starts at (0, 0, 0), as the behaviour's fields)
        s.w.set(1, {100.0f, 0.0f, 0.0f});
        s.info.moving = false;
        uint32_t reissues = 0, dropped = 0, frame = 1;
        CHECK(s.g.order_enter(1, 50));
        for (int k = 0; k < 11; ++k) {
            s.g.clear_events();
            s.service(frame++);
            reissues += static_cast<uint32_t>(s.count(GarrisonEventKind::Reissue));
            dropped += static_cast<uint32_t>(s.count(GarrisonEventKind::OrderDropped));
            if (s.count(GarrisonEventKind::Reissue) == 1) CHECK(s.g.order_enter(1, 50));  // the host's new approach order
        }
        CHECK(reissues == 10 && dropped == 1 && s.g.state_of(1) == 0 && s.g.retries_of(1) == 10);
        // the count survives the cancel: a new order's first finish with the same A cancels at once
        s.g.clear_events();
        CHECK(s.g.order_enter(1, 50));
        s.service(frame++);
        CHECK(s.count(GarrisonEventKind::OrderDropped) == 1 && s.g.retries_of(1) == 11);
        // A changed (the host moved): the count starts again
        s.g.clear_events();
        s.w.set(50, {0.0f, 2.0f, 0.0f});
        CHECK(s.g.order_enter(1, 50));
        s.service(frame++);
        CHECK(s.count(GarrisonEventKind::Reissue) == 1 && s.g.retries_of(1) == 0);
    }
    {  // entry bones: A = the bone's world point (host yaw 90: local x forward = world +y, local y left = world -x), the
       // nearest reachable bone in 3-D is the destination; none reachable -> host + half the extent length along its yaw
        GarrisonSystem g;
        HolderDesc h;
        h.accepts = kInf;
        h.slots = 4;
        h.radius = 10.0f;
        h.bones = {{10.0f, 0.0f, 2.0f}, {0.0f, 10.0f, 2.0f}, {-10.0f, 0.0f, 2.0f}};
        CHECK(g.add_holder(50, 1, h));
        const Vec3 hp{100.0f, 50.0f, 0.0f}, hf{0.0f, 3.0f, 0.0f};
        const Vec3 a0 = g.approach_point(50, hp, hf, 0), a1 = g.approach_point(50, hp, hf, 1), a2 = g.approach_point(50, hp, hf, 2);
        CHECK(a0.x == 100.0f && a0.y == 60.0f && a0.z == 2.0f && a1.x == 90.0f && a1.y == 50.0f && a2.x == 100.0f && a2.y == 40.0f);
        const Vec3 none = g.approach_point(50, hp, hf, 3);  // a bone the host does not have: the host position
        CHECK(none.x == hp.x && none.y == hp.y && none.z == hp.z);
        int32_t bone = -1;
        Vec3 d = g.approach_destination(50, hp, hf, {100.0f, 0.0f, 0.0f}, 40.0f, {}, bone);
        CHECK(bone == 2 && d.x == 100.0f && d.y == 40.0f);
        d = g.approach_destination(50, hp, hf, {100.0f, 0.0f, 0.0f}, 40.0f, [](Vec3 p) { return p.y != 40.0f; }, bone);
        CHECK(bone == 1 && d.x == 90.0f && d.y == 50.0f);  // 3-D 51.03 < 60.03
        d = g.approach_destination(50, hp, hf, {100.0f, 0.0f, 0.0f}, 40.0f, [](Vec3) { return false; }, bone);
        CHECK(bone == -1 && d.x == 100.0f && d.y == 70.0f && d.z == 0.0f);
        // Garrison_Enter_Dist > 0: the bone point pushed outward (2-D host -> bone, z kept); the fallback uses it as L
        GarrisonSystem g2;
        h.enter_dist = 5.0f;
        CHECK(g2.add_holder(50, 1, h));
        const Vec3 p2 = g2.approach_point(50, hp, hf, 2);
        CHECK(p2.x == 100.0f && p2.y == 35.0f && p2.z == 2.0f);
        d = g2.approach_destination(50, hp, hf, {100.0f, 0.0f, 0.0f}, 40.0f, [](Vec3) { return false; }, bone);
        CHECK(bone == -1 && d.x == 100.0f && d.y == 55.0f);
        // the exit point (FIGHTER_FLYING §7.4 "Exit"): E = the bone's point with yaw host -> bone, else the host position
        // and yaw; + Garrison_Exit_Dist, else the 3-D half-extent length (20); the enter push does not apply
        {
            GarrisonSystem g3, g4;
            HolderDesc he = h;
            he.extent_len = 20.0f;
            CHECK(g3.add_holder(50, 1, he));
            he.exit_dist = 5.0f;
            CHECK(g4.add_holder(50, 1, he));
            const Vec3 e0 = g3.exit_point(50, hp, hf, 0), e1 = g3.exit_point(50, hp, hf, 1), en = g3.exit_point(50, hp, hf);
            const Vec3 e2 = g4.exit_point(50, hp, hf, 2);
            CHECK(e0.x == 100.0f && e0.y == 80.0f && e0.z == 2.0f && e1.x == 70.0f && e1.y == 50.0f && e1.z == 2.0f);
            CHECK(en.x == 100.0f && en.y == 70.0f && en.z == 0.0f && e2.x == 100.0f && e2.y == 35.0f && e2.z == 2.0f);
        }
        // the service reads A from the bone: a unit 3-D 12 from bone 0's point (z 2 above it) - r 5 <= 10 enters
        OccupantDesc o{kInf, 1};
        CHECK(g.add_unit(1, 1, o) && g.order_enter(1, 50, 0) && g.bone_of(1) == 0);
        World w;
        w.set(50, hp);
        w.set(1, {100.0f + std::sqrt(140.0f), 60.0f, 0.0f});  // sqrt(140 + 4) = 12
        const PoseFn pose = [&w](ObjectId id, Vec3& p, Vec3& f) {
            if (!w.get(id, p, f)) return false;
            if (id == 50) f = {0.0f, 3.0f, 0.0f};
            return true;
        };
        g.service(1, pose, [](ObjectId, ObjectId, ApproachInfo& ai) {
            ai.radius = 5.0f;
            return true;
        });
        CHECK(g.entering(1));
    }
    {  // save / load inside state 2 (bone, move in flight, retry memo) - both copies carry on alike
        ApproachScene a;
        a.g.order_enter(1, 50, -1);
        a.w.set(1, {50.0f, 0.0f, 0.0f});
        a.info.moving = false;
        a.service(1);
        CHECK(a.g.state_of(1) == 2);
        const std::vector<uint8_t> saved = bytes_of(a.g);
        ApproachScene b;
        ByteReader r(saved);
        CHECK(b.g.read(r) && bytes_of(b.g) == saved && digest_of(b.g) == digest_of(a.g));
        CHECK(b.g.state_of(1) == 2 && b.g.retries_of(1) == a.g.retries_of(1) && b.g.events().size() == a.g.events().size());
        if (b.g.events().size() == 2) CHECK(b.g.events()[1].kind == GarrisonEventKind::CorrectiveMove);
        for (ApproachScene* x : {&a, &b}) {
            x->g.clear_events();
            x->w = a.w;
            x->info.moving = true;
            x->service(2);
            x->info.moving = false;
            x->service(3);  // the corrective move ended: enter
            CHECK(x->g.entering(1));
        }
        CHECK(digest_of(a.g) == digest_of(b.g));
        ApproachScene d;  // a state-2 unit's save differs from a state-1 one's
        d.g.order_enter(1, 50, -1);
        CHECK(bytes_of(d.g) != saved);
    }
}

int main() {
    test_rules();
    const uint64_t h1 = scripted_hash(), h2 = scripted_hash();
    CHECK(h1 == h2);
    test_capture();
    test_owner_change();  // A1 t08
    test_types();
    test_garrison_save();  // N1 stage 2
    test_vehicle_thief_holder();  // s78n t12 review
    test_groups();  // s78n t18
    test_approach();  // s78n t20 batch 2
    test_capture_save();
    std::printf("alamo_tactical_garrison_tests: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
