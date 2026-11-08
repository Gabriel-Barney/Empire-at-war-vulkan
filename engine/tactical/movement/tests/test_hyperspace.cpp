// L1 systems tests: hyperspace departures, engine control and teleport (hyperspace.hpp, MovementSystem::teleport).
// Hand-computed frames and positions; the departure timer draws are checked against a reference generator.
#include <cstdio>
#include <vector>

#include "alamo/core/sim_crc.hpp"
#include "alamo/tactical/movement/hyperspace.hpp"
#include "alamo/tactical/movement/movement_system.hpp"
#include "alamo/test/support.hpp"

using namespace alamo;
using namespace alamo::tactical;
using namespace alamo::tactical::movement;

namespace {

using ::alamo::test::g_fail;
#define CHECK_EQ(a, b)                                                                                  \
    do {                                                                                                \
        const double va_ = static_cast<double>(a), vb_ = static_cast<double>(b);                        \
        if (!(va_ == vb_)) {                                                                            \
            std::printf("FAIL %s:%d: %s == %s (%.9g vs %.9g)\n", __FILE__, __LINE__, #a, #b, va_, vb_); \
            ++g_fail;                                                                                   \
        }                                                                                               \
    } while (0)

void test_departures(StateHasher* out) {
    HyperspaceDepartures h;
    // L1_S4_REMAINING §3.4: timer = delay + 90; 8.125 per frame while > 90, then the faction increment; killed at 0.
    CHECK(h.schedule(7, 0, false, {70, 0, 5}, {0, 2, 0}, 500.0f));  // cinematic retreat form: parent link cleared
    CHECK(h.schedule(3, 10, true, {30, 0, 5}, {0, 0, 0}, 400.0f));  // Hyperspace_Away(): zero facing -> +x
    CHECK(!h.schedule(3, 9, true, {}, {1, 0, 0}, 1.0f));            // first departure kept
    CHECK_EQ(h.timer(7), 90);
    CHECK_EQ(h.timer(3), 100);
    uint32_t jumped7 = 0, jumped3 = 0, killed7 = 0, killed3 = 0;
    bool keep7 = true, keep3 = false;
    Vec3 p3_before_jump{};
    for (uint32_t f = 1; f <= 200; ++f) {
        if (h.timer(3) == 90) p3_before_jump = h.position(3);
        h.tick(f);
        for (const HyperspaceEvent& e : h.events()) {
            if (e.kind == HyperspaceEvent::Kind::Jumped) (e.id == 7 ? jumped7 : jumped3) = f;
            if (e.kind == HyperspaceEvent::Kind::Killed) {
                (e.id == 7 ? killed7 : killed3) = f;
                (e.id == 7 ? keep7 : keep3) = e.keep_parent_link;
            }
        }
        if (f == 1) CHECK_EQ(h.position(7).y, 500.0f);  // timer 90 at once: the faction increment from the first frame
        if (f == 10) CHECK_EQ(h.position(3).x, 30.0f + 10.0f * 8.125f);
    }
    CHECK_EQ(jumped7, 1);
    CHECK_EQ(killed7, 90);
    CHECK_EQ(jumped3, 11);
    CHECK_EQ(killed3, 100);
    CHECK(!keep7 && keep3);
    CHECK_EQ(p3_before_jump.x, 30.0f + 10.0f * 8.125f);
    CHECK(h.empty() && !h.departing(7) && h.timer(7) == -1);
    HyperspaceDepartures g;
    CHECK(g.schedule(1, 5, true, {0, 0, 0}, {1, 0, 0}, 100.0f));
    g.tick(1);
    if (out) g.hash(*out);
    g.remove(1);
    CHECK(g.empty());
}
void test_engines() {
    EngineControl e;
    CHECK(e.online(1));
    e.disable(1, 2.0f, 10);  // until 10 + 60
    CHECK(!e.online(1));
    CHECK_EQ(e.reenable_frame(1), 70);
    e.disable(1, 1.0f, 20);  // max(70, 50): unchanged
    CHECK_EQ(e.reenable_frame(1), 70);
    e.disable(1, 3.0f, 20);  // max(70, 110)
    CHECK_EQ(e.reenable_frame(1), 110);
    CHECK_EQ(e.events().size(), 1);  // one Off event (it was online once)
    e.clear_events();
    e.tick(109);
    CHECK(!e.online(1));
    e.tick(110);
    CHECK(e.online(1));
    CHECK_EQ(e.events().size(), 1);
    CHECK(e.events()[0].kind == EngineEvent::Kind::On);
    e.clear_events();
    e.disable(2, -1.0f, 0);  // indefinitely
    e.tick(100000);
    CHECK(!e.online(2));
    e.enable(2, 5);
    CHECK(e.online(2));
    e.destroy(3, 0);  // engine hard point destroyed: permanent
    e.disable(3, 1.0f, 0);
    e.tick(1000);
    e.enable(3, 1000);
    CHECK(!e.online(3) && e.destroyed(3));
    e.remove(3);
    CHECK(e.online(3));
    // HARDPOINT_DISABLED.md §9.1. Repair online (0x393980): off -> on, permanent cleared, timer reset, one On event.
    e.clear_events();
    CHECK(!e.repair_online(9, 1));  // unknown (no entry): nothing
    e.disable(4, -1.0f, 0);  // upgrade-copy disable (destroyed = 0, t < 0): not permanent
    CHECK(!e.online(4) && !e.destroyed(4));
    // Edge case: already off indefinitely -> destroying the last engine hard point does NOT set permanent.
    e.destroy(4, 1);
    CHECK(!e.destroyed(4));
    e.disable(4, 2.0f, 1);  // entry test fails too: no timer
    CHECK_EQ(e.reenable_frame(4), 0);
    e.clear_events();
    CHECK(e.repair_online(4, 2) && e.online(4));
    CHECK_EQ(e.events().size(), 1);
    CHECK(e.events()[0].kind == EngineEvent::Kind::On);
    CHECK(!e.repair_online(4, 3));  // already on: no signal
    CHECK_EQ(e.events().size(), 1);
    // A timed disable is pending -> destroy acts: permanent, timer cleared, tick does not bring it back.
    e.disable(5, 1.0f, 0);
    e.destroy(5, 1);
    CHECK(e.destroyed(5) && e.reenable_frame(5) == 0);
    e.tick(100);
    CHECK(!e.online(5));
    // The repair path clears permanent (the script enable does not).
    e.enable(5, 100);
    CHECK(!e.online(5));
    CHECK(e.repair_online(5, 101) && e.online(5) && !e.destroyed(5));
}

void test_teleport() {
    MovementConfig cfg;
    cfg.mode = MoveMode::Space;
    MovementSystem m(cfg);
    UnitDesc d;
    d.id = 4;
    d.pos = {0.0f, 0.0f};
    d.max_speed = 2.0f;
    d.radius = 20.0f;
    CHECK(m.add_unit(d));
    SimRng rng(9u);
    MoveOrder o;
    o.units = {4};
    o.destination = {3000.0f, 0.0f};
    m.move(o, rng, 1);
    for (uint32_t f = 1; f < 40; ++f) m.tick(f, rng);
    CHECK(m.view(4).has_goal);
    CHECK(m.teleport(4, {-500.0f, 250.0f}, 12.0f, 270.0f, 40));
    const UnitView v = m.view(4);
    CHECK(!v.has_goal && v.speed == 0.0f);
    CHECK_EQ(v.pos.x, -500.0f);
    CHECK_EQ(v.pos.y, 250.0f);
    CHECK_EQ(v.z, 12.0f);
    CHECK(!m.teleport(99, {0, 0}, 0, 0, 40));
    for (uint32_t f = 40; f < 60; ++f) m.tick(f, rng);
    CHECK_EQ(m.view(4).pos.x, -500.0f);  // stays put (no order)
    // Land: the collision tree entry follows.
    MovementConfig lc;
    lc.mode = MoveMode::Land;
    MovementSystem l(lc);
    UnitDesc ld;
    ld.id = 1;
    ld.pos = {100.0f, 100.0f};
    CHECK(l.add_unit(ld));
    CHECK(l.teleport(1, {300.0f, 40.0f}, 3.0f, 90.0f, 5));
    CHECK_EQ(l.view(1).pos.x, 300.0f);
    CHECK_EQ(l.view(1).yaw, 90.0f);
}

}  // namespace

int main() {
    StateHasher a, b;
    test_departures(&a);
    test_departures(&b);
    CHECK(a.digest() == b.digest());
    test_engines();
    test_teleport();
    if (g_fail) {
        std::printf("alamo_tactical_hyperspace_tests: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("alamo_tactical_hyperspace_tests: OK\n");
    return 0;
}
