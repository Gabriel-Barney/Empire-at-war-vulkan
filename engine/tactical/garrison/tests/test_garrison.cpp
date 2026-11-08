// alamo_tactical_garrison_tests (S4, docs/re/L1_S4_REMAINING.md §3.5 / §2.5 / §4.2): GarrisonSystem (masks with the
// zero-mask rule, allied / neutral hosts, Lua Can_Garrison capacity rule with reserved walkers vs the entry rule,
// approach point, entering delay, exit point, eject on host death / owner change, kill for CanContainGarrison-only
// hosts, Leave_Garrison cancelling an approach, Can_Garrison_Fire, hash determinism), CapturePointSystem (start gate,
// cadence, first-candidate control, contested, decay vs sticky, two-step capture of an enemy point, garrison rule b),
// and the descriptors read from type XML (garrison_types.hpp).
#include <algorithm>
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
    bunker.half_diagonal = 20.0f;
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
    const OccupantDesc trooper{kInf, 1, 5.0f}, dt3{kVeh, 2, 5.0f};
    CHECK(g.add_unit(1, 1, trooper) && g.add_unit(2, 1, trooper) && g.add_unit(3, 1, dt3) && g.add_unit(4, 2, trooper));
    CHECK(g.add_unit(5, 3, trooper) && g.add_unit(6, 1, trooper) && g.add_unit(20, 1, dt3));
    // Masks (zero mask always matches), allied / neutral / hostile hosts, the unit itself.
    CHECK(g.masks_match(1, 10) && !g.masks_match(3, 10) && g.masks_match(3, 20) && g.masks_match(3, 30));
    CHECK(g.can_garrison(1, 10) && g.can_garrison(5, 10) && !g.can_garrison(4, 10) && g.can_garrison(4, 30) && !g.can_garrison(1, 40));
    CHECK(!g.can_garrison(20, 20) && !g.can_garrison(3, 10));
    // Lua capacity rule: free - reserved - value > 0. The bunker (3 slots): walkers reserve.
    CHECK(g.order_enter(1, 10) && g.order_enter(2, 10) && g.reserved(10) == 2);
    CHECK(!g.can_garrison(6, 10));  // 3 - 2 - 1 = 0: Can_Garrison false ...
    CHECK(g.order_enter(6, 10));    // ... but the order is accepted (no capacity test at order time)
    CHECK(!g.order_enter(4, 10));   // hostile host
    // Approach point: host + facing x half diagonal (no Enter_Dist) = (20, 0); reach = max(radius 0, slack 10) + 5.
    World w;
    w.set(10, {0, 0, 0});
    w.set(1, {34, 0, 0});
    w.set(2, {36, 0, 0});
    w.set(6, {300, 0, 0});
    const Vec3 ap = g.approach_point(10, {0, 0, 0}, {2, 0, 0});
    CHECK(ap.x == 20.0f && ap.y == 0.0f);
    auto pose = [&w](ObjectId id, Vec3& p, Vec3& f) { return w.get(id, p, f); };
    g.service(1, pose);
    CHECK(g.entering(1) && !g.entering(2) && g.pending_holder(2) == 10 && g.holder_of(1) == kNoObject);
    CHECK(g.events().size() == 1 && g.events()[0].kind == GarrisonEventKind::EnterStarted && g.events()[0].unit == 1);
    CHECK(!g.cancel_enter(1) && !g.order_enter(1, 20));  // an entering unit is committed
    g.clear_events();
    g.service(30, pose);
    CHECK(g.holder_of(1) == kNoObject);  // 30 frames of fade: enters at frame 31
    g.service(31, pose);
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
    g.service(40, pose);
    CHECK(g.entering(3));
    g.service(70, pose);
    CHECK(g.holder_of(3) == 20 && g.used_slots(20) == 2);
    // The walker 6 to the bunker: hostile owner change of the host drops it.
    g.clear_events();
    g.set_owner(10, 2);
    g.service(71, pose);
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
    g.service(80, pose);
    g.service(110, pose);
    CHECK(g.holder_of(4) == 30);
    g.clear_events();
    g.remove(30, {500, 0, 0}, {1, 0, 0}, 111);
    CHECK(!g.is_holder(30) && g.holder_of(4) == kNoObject && g.events().size() == 1 && g.events()[0].kind == GarrisonEventKind::Killed);
    CHECK(g.occupancy().empty());
    // Eject_Garrison.
    w.set(5, {22, 0, 0});
    g.set_owner(10, 3);
    CHECK(g.order_enter(5, 10));
    g.service(120, pose);
    g.service(150, pose);
    CHECK(g.holder_of(5) == 10 && g.occupancy().size() == 1);
    g.clear_events();
    CHECK(g.eject_all(10, {0, 0, 0}, {1, 0, 0}, 151) == 1 && g.events()[0].kind == GarrisonEventKind::Ejected);
    // A dead walker's host disappears: dropped.
    w.set(2, {30, 0, 0});
    CHECK(g.order_enter(2, 40) == false);  // hostile
    CHECK(g.order_enter(6, 20));
    g.clear_events();
    w.erase(20);
    g.service(160, pose);
    CHECK(g.events().size() == 1 && g.events()[0].kind == GarrisonEventKind::OrderDropped);
}

uint64_t scripted_hash() {
    GarrisonSystem g;
    HolderDesc h;
    h.accepts = kInf;
    h.slots = 2;
    g.add_holder(50, 0, h);
    for (ObjectId i = 1; i <= 4; ++i) g.add_unit(i, 0, OccupantDesc{kInf, 1, 4.0f});
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
    const OccupantDesc trooper{kInf, 1, 5.0f};
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
        bunker.half_diagonal = 20.0f;
        HolderDesc jug;
        jug.kind = HolderKind::Vehicle;
        jug.accepts = kInf | kVeh;
        jug.slots = 10;
        jug.radius = 30.0f;
        jug.enter_dist = 15.0f;
        g.add_holder(10, 1, bunker);
        g.add_holder(20, 3, jug);
        for (ObjectId i = 1; i <= 5; ++i) g.add_unit(i, i == 5 ? 3 : 1, OccupantDesc{kInf, 1, 5.0f});
        g.add_unit(6, 1, OccupantDesc{kVeh, 2, 5.0f});
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
    // The first unit's state byte: version, neutral, holder count, 2 holders (42 bytes + 4 per occupant), unit count,
    // then id, owner, category, value, radius.
    size_t unit0_state = 4 + 4 + 4;
    for (const ObjectId h : {10, 20}) unit0_state += 42 + 4 * a.g.occupants(h).size();
    unit0_state += 4 + 4 + 4 + 8 + 4 + 4;

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
    CHECK(bad[unit0_state] == 7 || bad[unit0_state] == 5 || bad[unit0_state] == 1);
    bad[unit0_state] = 3;  // not a GarrisonDataPack state
    ByteReader rb3(bad);
    CHECK(!c.g.read(rb3));
    bad = saved;
    bad[0] = 2;  // version
    ByteReader rb4(bad);
    CHECK(!c.g.read(rb4));
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

int main() {
    test_rules();
    const uint64_t h1 = scripted_hash(), h2 = scripted_hash();
    CHECK(h1 == h2);
    test_capture();
    test_owner_change();  // A1 t08
    test_types();
    test_garrison_save();  // N1 stage 2
    test_capture_save();
    std::printf("alamo_tactical_garrison_tests: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
