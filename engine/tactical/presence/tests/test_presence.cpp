// Presence tests (docs/re/ANIM_PARTICLES.md §A.11 / §A.12): IMPOSING_PRESENCE service timing (first-due draw, every
// 30 frames) and its gates on the affected unit, aging / expiry, the move and target clears, the Deploys freeze,
// the winner rule (strictly more frames, allied override, list order, dead keys), default idle (SPREAD_OUT first,
// induced anim lookup, 0), the hash-map list order and growth, update() service order, save round trip / rejects,
// explosion flinch intensity / clamp / filters, and with FoC data the vanilla lists.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/tactical/presence/explosion_flinch.hpp"
#include "alamo/tactical/presence/presence_system.hpp"

#ifdef ALAMO_PRESENCE_TESTS_HAVE_DATA
#include "alamo/anim/anim_types.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_types.hpp"
#include "alamo/tactical/presence/presence_data.hpp"
#include "alamo/vfs/vfs.hpp"
#endif

using namespace alamo;
using namespace alamo::tactical::presence;

namespace {

int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

bool near(float a, float b) { return a - b < 1e-5f && b - a < 1e-5f; }

// Types: 0 trooper (Attention near 2 / 3, Celebrate near 4), 1 other infantry (lists nothing), 2 / 3 Vader-like
// presence types (range 100), 4 a presence type with range 200, 5 deploying trooper (Attention near 2), 6 trooper
// with Allow_Idle_When_Moving.
constexpr TypeId kTrooper = 0, kOther = 1, kVader = 2, kEmperor = 3, kAtat = 4, kDeployer = 5, kWalker = 6;

PresenceTypeTable make_types() {
    PresenceTypeTable t;
    t.resize(7);
    t.at(kTrooper).induced = {InducedAnimation{kAnimAttention, {kEmperor, kVader}}, InducedAnimation{kAnimCelebrate, {kAtat, kVader}}};
    t.at(kVader).presence_range = 100.0f;
    t.at(kEmperor).presence_range = 100.0f;
    t.at(kAtat).presence_range = 200.0f;
    t.at(kDeployer).induced = {InducedAnimation{kAnimAttention, {kVader}}};
    t.at(kDeployer).deploys = true;
    t.at(kWalker).induced = {InducedAnimation{kAnimAttention, {kVader}}};
    t.at(kWalker).allow_idle_when_moving = true;
    return t;
}

// Owners: allied when equal or both < 10 (team A) or both >= 10 (team B).
struct FakeWorld : PresenceWorld {
    std::map<ObjectId, UnitView> units;
    mutable uint32_t queries = 0;
    bool unit(ObjectId id, UnitView& out) const override {
        auto it = units.find(id);
        if (it == units.end()) return false;
        out = it->second;
        return true;
    }
    // A superset (every unit) twice over: the system must re-test the cube and drop duplicates.
    void box_query(float, float, float, float, std::vector<ObjectId>& out) const override {
        ++queries;
        for (const auto& [id, u] : units) out.push_back(id);
        for (auto it = units.rbegin(); it != units.rend(); ++it) out.push_back(it->first);
    }
    bool allied(int32_t a, int32_t b) const override { return a == b || ((a < 10) == (b < 10)); }

    UnitView& add(ObjectId id, TypeId type, int32_t owner, float x, float y, float z = 0.0f, bool idle = true) {
        UnitView u;
        u.type = type;
        u.owner = owner;
        u.x = x;
        u.y = y;
        u.z = z;
        u.has_idle = idle;
        u.has_locomotor = true;
        return units[id] = u;
    }
};

uint64_t digest(const PresenceSystem& s) {
    StateHasher h;
    s.hash(h);
    return h.digest();
}

// A system with presence unit `p` due at frame 0 (no draw).
void add_due_now(PresenceSystem& s, ObjectId p) { CHECK(s.add_presence_unit(p, 0, nullptr)); }

// ---- tests ----------------------------------------------------------------------------------------------

void test_service_timing() {
    PresenceSystem s;
    s.set_types(make_types());
    FakeWorld w;
    w.add(100, kVader, 1, 0.0f, 0.0f);
    w.add(5, kTrooper, 1, 50.0f, 0.0f);

    SimRng rng(1234u), mirror(1234u);
    CHECK(s.add_presence_unit(100, 10, &rng));
    const uint32_t due = 10u + static_cast<uint32_t>(mirror.range_int(0, 29));
    CHECK(rng.state() == mirror.state());
    CHECK(s.next_due(100) == due);
    CHECK(!s.add_presence_unit(100, 10, &rng));  // already registered: no second draw
    CHECK(rng.state() == mirror.state());
    CHECK(s.next_due(7) == 0xFFFFFFFFu);

    if (due > 10) CHECK(s.service_presence(100, due - 1, w) == 0);
    CHECK(s.remaining(5, 100) == 0);
    CHECK(s.service_presence(100, due, w) == 1);
    CHECK(s.remaining(5, 100) == 150);
    CHECK(s.next_due(100) == due + 30);
    CHECK(s.service_presence(100, due + 29, w) == 0);
    CHECK(s.service_presence(100, due + 30, w) == 1);
    CHECK(s.next_due(100) == due + 60);
    CHECK(s.service_presence(999, due, w) == 0);  // not a presence unit

    // Relaxation time from GameConstants: int(30 x 2.5) = 75; a late service (frame past due) re-arms from now.
    PresenceConfig cfg;
    cfg.relaxation_seconds = 2.5f;
    CHECK(cfg.relaxation_frames() == 75);
    s.set_config(cfg);
    CHECK(s.service_presence(100, due + 100, w) == 1);
    CHECK(s.remaining(5, 100) == 75);
    CHECK(s.next_due(100) == due + 130);
}

void test_service_gates() {
    PresenceSystem s;
    s.set_types(make_types());
    FakeWorld w;
    w.add(100, kVader, 1, 0.0f, 0.0f, 0.0f, false);
    w.add(1, kTrooper, 1, 100.0f, -100.0f, 100.0f);      // corner of the cube: inclusive
    w.add(2, kTrooper, 12, 10.0f, 0.0f);                  // enemy owner: no owner filter
    w.add(3, kTrooper, 1, 100.5f, 0.0f);                  // outside (x)
    w.add(4, kTrooper, 1, 0.0f, 0.0f, 101.0f);            // outside (z): a cube, not a circle
    w.add(6, kOther, 1, 0.0f, 10.0f);                     // type lists nothing
    w.add(7, kTrooper, 1, 0.0f, 10.0f, 0.0f, false);      // no IDLE
    w.add(8, kTrooper, 1, 0.0f, 10.0f).dying = true;      // dying / removed
    w.add(9, kTrooper, 1, 0.0f, 10.0f).has_target = true; // TARGETING has a target
    w.add(10, kTrooper, 1, 0.0f, 10.0f).moving = true;    // moving without Allow_Idle_When_Moving
    UnitView& stop = w.add(11, kTrooper, 1, 0.0f, 10.0f); // moving flag but no locomotor
    stop.moving = true;
    stop.has_locomotor = false;
    w.add(13, kWalker, 1, 0.0f, 10.0f).moving = true;     // moving, Allow_Idle_When_Moving
    w.add(14, kTrooper, 1, 70.0f, 70.0f);                 // XY distance 99: inside
    add_due_now(s, 100);
    // The presence unit itself is not gated (it has no IDLE and lists nothing here anyway).
    w.units[100].has_target = true;
    w.units[100].moving = true;

    CHECK(s.service_presence(100, 0, w) == 5);
    for (ObjectId id : {1, 2, 11, 13, 14}) CHECK(s.remaining(id, 100) == 150);
    for (ObjectId id : {3, 4, 6, 7, 8, 9, 10}) CHECK(s.remaining(id, 100) == 0);
    CHECK(s.map_count() == 5);

    // Presence type not listed by the trooper: nothing.
    PresenceSystem s2;
    s2.set_types(make_types());
    FakeWorld w2;
    w2.add(50, kOther, 1, 0.0f, 0.0f);
    w2.add(1, kTrooper, 1, 0.0f, 0.0f);
    add_due_now(s2, 50);
    CHECK(s2.service_presence(50, 0, w2) == 0);  // kOther has range 0 and is not listed
    CHECK(s2.map_count() == 0);

    // Range 0: only an object exactly at the position.
    PresenceTypeTable t = make_types();
    t.at(kVader).presence_range = 0.0f;
    PresenceSystem s3;
    s3.set_types(t);
    FakeWorld w3;
    w3.add(100, kVader, 1, 5.0f, 5.0f);
    w3.add(1, kTrooper, 1, 5.0f, 5.0f);
    w3.add(2, kTrooper, 1, 5.0f, 5.001f);
    add_due_now(s3, 100);
    CHECK(s3.service_presence(100, 0, w3) == 1);
    CHECK(s3.remaining(1, 100) == 150 && s3.remaining(2, 100) == 0);

    // A presence unit that is no longer live: the service still re-arms, writes nothing.
    PresenceSystem s4;
    s4.set_types(make_types());
    add_due_now(s4, 100);
    FakeWorld empty;
    CHECK(s4.service_presence(100, 0, empty) == 0);
    CHECK(s4.next_due(100) == 30);
}

void test_countdown_and_clears() {
    PresenceSystem s;
    s.set_types(make_types());
    FakeWorld w;
    w.add(100, kVader, 1, 0.0f, 0.0f);
    w.add(1, kTrooper, 1, 10.0f, 0.0f);
    add_due_now(s, 100);
    CHECK(s.service_presence(100, 0, w) == 1);
    CHECK(s.service_idle(1, w) == IdleMaintenance::Aged);
    CHECK(s.remaining(1, 100) == 149);
    // The presence unit leaves (removed from the mode): the entry ages out 150 frames after the last refresh.
    w.units.erase(100);
    s.remove_object(100);
    for (int i = 0; i < 147; ++i) s.service_idle(1, w);
    CHECK(s.remaining(1, 100) == 2);
    CHECK(s.default_idle(1, false, w) == kNoAnimType);  // entries left, no live key [M]
    s.service_idle(1, w);
    CHECK(s.remaining(1, 100) == 1);
    CHECK(s.service_idle(1, w) == IdleMaintenance::Aged);
    CHECK(s.remaining(1, 100) == 0);
    CHECK(s.entries(1).empty());
    CHECK(s.map_count() == 0);
    CHECK(s.service_idle(1, w) == IdleMaintenance::None);
    CHECK(s.default_idle(1, false, w) == kAnimIdle);
    CHECK(s.stats().expired == 1);

    // Clears: moving (no Allow_Idle_When_Moving) and has-target; moving with the flag only ages.
    w.add(100, kVader, 1, 0.0f, 0.0f);
    w.add(2, kTrooper, 1, 0.0f, 20.0f);
    w.add(3, kWalker, 1, 0.0f, 30.0f);
    add_due_now(s, 100);
    CHECK(s.service_presence(100, 0, w) == 3);
    w.units[1].moving = true;
    w.units[2].has_target = true;
    w.units[3].moving = true;
    CHECK(s.service_idle(1, w) == IdleMaintenance::ClearedMoving);
    CHECK(s.service_idle(2, w) == IdleMaintenance::ClearedTarget);
    CHECK(s.service_idle(3, w) == IdleMaintenance::Aged);
    CHECK(s.entries(1).empty() && s.entries(2).empty());
    CHECK(s.remaining(3, 100) == 149);
    CHECK(s.stats().cleared == 2);

    // Deploys type while deployed / (un)deploying: frozen; otherwise aged. The move / target clears still apply.
    w.add(4, kDeployer, 1, 0.0f, 40.0f);
    CHECK(s.service_presence(100, 30, w) == 2);  // 3 (walker, still moving: allowed) and 4
    w.units[4].deploy_busy = true;
    CHECK(s.service_idle(4, w) == IdleMaintenance::Frozen);
    CHECK(s.remaining(4, 100) == 150);
    w.units[4].deploy_busy = false;
    CHECK(s.service_idle(4, w) == IdleMaintenance::Aged);
    CHECK(s.remaining(4, 100) == 149);
    w.units[4].deploy_busy = true;
    w.units[4].has_target = true;
    CHECK(s.service_idle(4, w) == IdleMaintenance::ClearedTarget);
    // deploy_busy on a non-Deploys type is ignored.
    w.units[3].moving = false;
    w.units[3].deploy_busy = true;
    CHECK(s.service_idle(3, w) == IdleMaintenance::Aged);

    // The affected unit vanished without remove_object: its map is dropped [port].
    w.units.erase(3);
    CHECK(s.service_idle(3, w) == IdleMaintenance::Dropped);
    CHECK(s.entries(3).empty());
}

// Ids with distinct buckets (8 buckets), so their list order is their insertion order.
std::vector<ObjectId> distinct_bucket_ids(size_t n, ObjectId from) {
    std::vector<ObjectId> out;
    std::vector<uint32_t> used;
    for (ObjectId id = from; out.size() < n; ++id) {
        const uint32_t b = PresenceSystem::bucket_of(id, 8);
        bool dup = false;
        for (uint32_t u : used) dup = dup || u == b;
        if (dup) continue;
        used.push_back(b);
        out.push_back(id);
    }
    return out;
}

// A system holding one map for `affected` with `entries` in this list order (through read()).
bool load_map(PresenceSystem& s, ObjectId affected, const std::vector<PresenceEntry>& entries) {
    ByteWriter w;
    w.u32(1).u32(0).u32(1).i32(affected).u32(8).u32(static_cast<uint32_t>(entries.size()));
    for (const PresenceEntry& e : entries) w.i32(e.presence).i32(e.remaining);
    ByteReader r(w.bytes());
    return s.read(r);
}

void test_winner_rule() {
    const std::vector<ObjectId> ids = distinct_bucket_ids(4, 200);
    const ObjectId a = ids[0], b = ids[1], c = ids[2], d = ids[3];
    FakeWorld w;
    w.add(1, kTrooper, 1, 0.0f, 0.0f);  // team A
    w.add(a, kVader, 12, 0.0f, 0.0f);    // enemy
    w.add(b, kEmperor, 2, 0.0f, 0.0f);   // allied (team A)
    w.add(c, kAtat, 13, 0.0f, 0.0f);     // enemy
    w.add(d, kVader, 1, 0.0f, 0.0f);     // own owner (self counts as allied)
    PresenceSystem s;
    s.set_types(make_types());

    // Strictly more frames wins among same-allegiance candidates.
    CHECK(load_map(s, 1, {{a, 100}, {c, 120}}));
    CHECK(s.winner(1, w) == c);
    CHECK(s.default_idle(1, false, w) == kAnimCelebrate);
    CHECK(load_map(s, 1, {{c, 120}, {a, 120}}));  // tie: the first stays
    CHECK(s.winner(1, w) == c);
    // Allied candidate with fewer frames replaces a non-allied best.
    CHECK(load_map(s, 1, {{a, 140}, {b, 100}}));
    CHECK(s.winner(1, w) == b);
    CHECK(s.default_idle(1, false, w) == kAnimAttention);
    // ... and the best's value drops to the candidate's: a later non-allied entry with more frames takes over.
    CHECK(load_map(s, 1, {{a, 140}, {b, 100}, {c, 120}}));
    CHECK(s.winner(1, w) == c);
    // A later non-allied entry with fewer / equal frames does not replace an allied best.
    CHECK(load_map(s, 1, {{b, 100}, {a, 100}}));
    CHECK(s.winner(1, w) == b);
    // Allied after allied: only strictly more frames.
    CHECK(load_map(s, 1, {{b, 100}, {d, 100}}));
    CHECK(s.winner(1, w) == b);
    CHECK(load_map(s, 1, {{b, 100}, {d, 101}}));
    CHECK(s.winner(1, w) == d);
    // Dead keys are skipped but keep their entry.
    CHECK(load_map(s, 1, {{c, 150}, {a, 10}}));
    w.units.erase(c);
    CHECK(s.winner(1, w) == a);
    CHECK(s.remaining(1, c) == 150);
    CHECK(s.default_idle(1, false, w) == kAnimAttention);  // Vader: the first entry listing it (Attention, not Celebrate)
}

void test_default_idle() {
    PresenceSystem s;
    s.set_types(make_types());
    FakeWorld w;
    w.add(100, kAtat, 12, 0.0f, 0.0f);
    w.add(1, kTrooper, 1, 0.0f, 150.0f);
    w.add(2, kOther, 1, 0.0f, 0.0f);
    CHECK(s.default_idle(1, false, w) == kAnimIdle);
    CHECK(s.default_idle(1, true, w) == kAnimIdleCrouch);
    CHECK(s.presence_idle(1, w) == kNoAnimType);
    add_due_now(s, 100);
    CHECK(s.service_presence(100, 0, w) == 1);
    CHECK(s.default_idle(1, false, w) == kAnimCelebrate);  // enemy AT-AT clone: no owner filter
    CHECK(s.default_idle(1, true, w) == kAnimIdleCrouch);   // SPREAD_OUT wins
    CHECK(s.presence_idle(1, w) == kAnimCelebrate);
    CHECK(s.default_idle(2, false, w) == kAnimIdle);
    CHECK(s.default_idle(77, false, w) == kAnimIdle);  // unknown object

    // The type table helpers (0x370630 / 0x373530).
    const PresenceTypeTable t = make_types();
    CHECK(t.lists(kTrooper, kVader) && t.lists(kTrooper, kAtat) && !t.lists(kTrooper, kOther));
    CHECK(!t.lists(kOther, kVader) && !t.lists(kNoType, kVader) && !t.lists(kTrooper, kNoType));
    CHECK(t.induced_anim(kTrooper, kVader) == kAnimAttention);
    CHECK(t.induced_anim(kTrooper, kAtat) == kAnimCelebrate);
    CHECK(t.induced_anim(kTrooper, kOther) == kNoAnimType);
    CHECK(t.induced_anim(99, kVader) == kNoAnimType);
}

// Independent minstd (64-bit modulo) against the Schrage form.
uint32_t ref_bucket(ObjectId id, uint32_t buckets) {
    const uint64_t x = (static_cast<uint32_t>(id) ^ 0xDEADBEEFu) & 0x7FFFFFFFu;
    return static_cast<uint32_t>((x * 16807u) % 0x7FFFFFFFu) & (buckets - 1u);
}

void test_list_order() {
    for (ObjectId id = -5; id < 5000; ++id) {
        CHECK(PresenceSystem::bucket_of(id, 8) == ref_bucket(id, 8));
        CHECK(PresenceSystem::bucket_of(id, 64) == ref_bucket(id, 64));
    }
    CHECK(PresenceSystem::bucket_of(0x3FFFFF, 512) == ref_bucket(0x3FFFFF, 512));

    // Two keys in one bucket inserted high first: the run is ascending. A key in another bucket is appended.
    ObjectId lo = -1, hi = -1, other = -1;
    for (ObjectId id = 1000; hi < 0; ++id)
        if (PresenceSystem::bucket_of(id, 8) == PresenceSystem::bucket_of(1000, 8) && id != 1000) hi = id;
    lo = 1000;
    for (ObjectId id = 1000; other < 0; ++id)
        if (PresenceSystem::bucket_of(id, 8) != PresenceSystem::bucket_of(1000, 8)) other = id;
    PresenceTypeTable t = make_types();
    PresenceSystem s;
    s.set_types(t);
    FakeWorld w;
    w.add(1, kTrooper, 1, 0.0f, 0.0f);
    w.add(hi, kVader, 1, 0.0f, 0.0f);
    w.add(other, kVader, 1, 0.0f, 0.0f);
    w.add(lo, kVader, 1, 0.0f, 0.0f);
    add_due_now(s, hi);
    add_due_now(s, other);
    add_due_now(s, lo);
    s.service_presence(hi, 0, w);
    s.service_presence(other, 0, w);
    s.service_presence(lo, 0, w);
    std::span<const PresenceEntry> e = s.entries(1);
    CHECK(e.size() == 3);
    if (e.size() == 3) CHECK(e[0].presence == lo && e[1].presence == hi && e[2].presence == other);
    // A refresh keeps the position.
    s.service_presence(other, 30, w);
    e = s.entries(1);
    if (e.size() == 3) CHECK(e[2].presence == other);

    // Growth: the 9th key rehashes to 64 buckets; the result keeps each bucket's run contiguous and ascending
    // (checked by read() of the written state) and grows no further below 65 entries.
    PresenceSystem g;
    g.set_types(t);
    FakeWorld gw;
    gw.add(1, kTrooper, 1, 0.0f, 0.0f);
    for (ObjectId p = 300; p < 320; ++p) {
        gw.add(p, kVader, 1, 0.0f, 0.0f);
        add_due_now(g, p);
    }
    for (ObjectId p = 319; p >= 300; --p) g.service_presence(p, 0, gw);
    CHECK(g.entries(1).size() == 20);
    ByteWriter bw;
    g.write(bw);
    PresenceSystem g2;
    ByteReader br(bw.bytes());
    CHECK(g2.read(br));
    CHECK(digest(g2) == digest(g));
    // Bucket count 64 is in the save: after clearing, the map keeps its buckets (MSVC clear()) and stays stored.
    gw.units[1].has_target = true;
    CHECK(g.service_idle(1, gw) == IdleMaintenance::ClearedTarget);
    CHECK(g.map_count() == 1 && g.entries(1).empty());
    ByteWriter bw2;
    g.write(bw2);
    PresenceSystem g3;
    ByteReader br2(bw2.bytes());
    CHECK(g3.read(br2));
    CHECK(digest(g3) == digest(g));
}

// update(): newest first; for a unit with a higher id than its presence unit the refresh comes after its aging
// (150 at the end of the frame), for a lower id before it (149), including a map created in the same walk.
void test_update_order() {
    PresenceSystem s;
    s.set_types(make_types());
    FakeWorld w;
    w.add(100, kVader, 1, 0.0f, 0.0f);
    w.add(50, kTrooper, 1, 10.0f, 0.0f);
    w.add(150, kTrooper, 1, 20.0f, 0.0f);
    add_due_now(s, 100);
    s.update(0, w);
    CHECK(s.remaining(150, 100) == 150);  // serviced before the presence unit: nothing to age yet
    CHECK(s.remaining(50, 100) == 149);   // created during the walk, aged later in the same frame
    for (uint32_t f = 1; f < 30; ++f) s.update(f, w);
    CHECK(s.remaining(150, 100) == 121);
    CHECK(s.remaining(50, 100) == 120);
    s.update(30, w);
    CHECK(s.remaining(150, 100) == 150);
    CHECK(s.remaining(50, 100) == 149);
    CHECK(s.default_idle(50, false, w) == kAnimAttention);
    // The trooper starts moving: cleared at its next IDLE service, not refreshed while moving.
    w.units[50].moving = true;
    s.update(31, w);
    CHECK(s.entries(50).empty());
    for (uint32_t f = 32; f <= 60; ++f) s.update(f, w);
    CHECK(s.entries(50).empty());
    CHECK(s.remaining(150, 100) == 150);
}

void test_save() {
    PresenceSystem s;
    s.set_types(make_types());
    FakeWorld w;
    w.add(100, kVader, 1, 0.0f, 0.0f);
    w.add(101, kAtat, 12, 30.0f, 0.0f);
    for (ObjectId id = 1; id <= 6; ++id) w.add(id, kTrooper, id < 4 ? 1 : 11, static_cast<float>(id) * 12.0f, 5.0f);
    SimRng rng(77u);
    s.add_presence_unit(100, 0, &rng);
    s.add_presence_unit(101, 0, &rng);
    for (uint32_t f = 0; f < 45; ++f) s.update(f, w);
    CHECK(s.map_count() == 6);

    ByteWriter bw;
    s.write(bw);
    PresenceSystem l;
    l.set_types(make_types());
    ByteReader br(bw.bytes());
    CHECK(l.read(br));
    CHECK(digest(l) == digest(s));
    ByteWriter bw2;
    l.write(bw2);
    CHECK(bw2.bytes() == bw.bytes());
    // Continuing both runs gives the same state every frame.
    bool same = true;
    for (uint32_t f = 45; f < 400; ++f) {
        if (f == 200) w.units[2].has_target = true;
        if (f == 260) w.units.erase(100);
        s.update(f, w);
        l.update(f, w);
        same = same && digest(l) == digest(s);
    }
    CHECK(same);
    for (ObjectId id = 1; id <= 6; ++id) CHECK(s.default_idle(id, false, w) == l.default_idle(id, false, w));

    // Rejects leave the system unchanged.
    const uint64_t before = digest(l);
    auto rejects = [&](std::vector<uint8_t> bytes) {
        ByteReader r(bytes);
        const bool ok = l.read(r);
        CHECK(!ok);
        CHECK(digest(l) == before);
    };
    std::vector<uint8_t> bad = bw.bytes();
    bad[0] = 9;  // version
    rejects(bad);
    bad = bw.bytes();
    bad.pop_back();  // truncated
    rejects(bad);
    bad = bw.bytes();
    bad.push_back(0);  // trailing byte
    rejects(bad);
    {
        ByteWriter x;  // remaining < 1
        x.u32(1).u32(0).u32(1).i32(1).u32(8).u32(1).i32(100).i32(0);
        rejects(x.bytes());
    }
    {
        ByteWriter x;  // duplicate key
        x.u32(1).u32(0).u32(1).i32(1).u32(8).u32(2).i32(100).i32(5).i32(100).i32(6);
        rejects(x.bytes());
    }
    {
        ByteWriter x;  // bucket count not a power of two
        x.u32(1).u32(0).u32(1).i32(1).u32(12).u32(1).i32(100).i32(5);
        rejects(x.bytes());
    }
    {
        ByteWriter x;  // presence units not ascending
        x.u32(1).u32(2).i32(5).u32(0).i32(5).u32(0).u32(0);
        rejects(x.bytes());
    }
    {
        // Same bucket, descending: not a list order the hash map can hold.
        ObjectId hi = -1;
        for (ObjectId id = 1001; hi < 0; ++id)
            if (PresenceSystem::bucket_of(id, 8) == PresenceSystem::bucket_of(1000, 8)) hi = id;
        ByteWriter x;
        x.u32(1).u32(0).u32(1).i32(1).u32(8).u32(2).i32(hi).i32(5).i32(1000).i32(6);
        rejects(x.bytes());
    }
    // An empty system round-trips.
    PresenceSystem e;
    ByteWriter ew;
    e.write(ew);
    PresenceSystem e2;
    ByteReader er(ew.bytes());
    CHECK(e2.read(er));
    CHECK(digest(e2) == digest(e));
}

void test_explosion() {
    // Intensity: clamp(I (1 - 0.5 d²/R²), 0.25, 1).
    CHECK(near(explosion_flinch_intensity(1.0f, 0.0f, 100.0f), 1.0f));
    CHECK(near(explosion_flinch_intensity(1.0f, 10000.0f, 100.0f), 0.5f));
    CHECK(near(explosion_flinch_intensity(1.0f, 5000.0f, 100.0f), 0.75f));
    CHECK(near(explosion_flinch_intensity(1.0f, 20000.0f, 100.0f), 0.25f));  // cube corner: 0 -> floor
    CHECK(near(explosion_flinch_intensity(2.0f, 0.0f, 100.0f), 1.0f));       // ceiling
    CHECK(near(explosion_flinch_intensity(2.0f, 10000.0f, 100.0f), 1.0f));
    CHECK(near(explosion_flinch_intensity(0.2f, 0.0f, 100.0f), 0.25f));      // floor
    CHECK(near(explosion_flinch_intensity(0.8f, 2500.0f, 50.0f), 0.4f));
    CHECK(near(explosion_flinch_intensity(1.0f, 0.0f, 0.0f), 0.25f));

    std::vector<FlinchCandidate> c = {
        {9, 60.0f, 80.0f, 0.0f, true},     // d²_xy = 10000 -> 0.5
        {3, 10.0f, 10.0f, 0.0f, true},     // inside -> 0.99
        {4, 0.0f, 0.0f, 0.0f, false},      // no IDLE
        {5, 101.0f, 0.0f, 0.0f, true},     // outside (x)
        {6, 0.0f, 0.0f, 100.0f, true},     // z at the face: inside, d²_xy = 0 -> 1
        {7, 0.0f, 0.0f, 100.5f, true},     // outside (z)
        {3, 10.0f, 10.0f, 0.0f, true},     // duplicate
        {2, 100.0f, -100.0f, -100.0f, true}, // corner -> floor
    };
    const std::vector<ExplosionFlinch> v = explosion_flinches(0.0f, 0.0f, 0.0f, 100.0f, 1.0f, c);
    CHECK(v.size() == 4);
    if (v.size() == 4) {
        CHECK(v[0].victim == 2 && near(v[0].intensity, 0.25f) && near(v[0].dx, 100.0f) && near(v[0].dz, -100.0f));
        CHECK(v[1].victim == 3 && near(v[1].intensity, 0.99f) && near(v[1].dx, 10.0f) && near(v[1].dy, 10.0f));
        CHECK(v[2].victim == 6 && near(v[2].intensity, 1.0f) && near(v[2].dz, 100.0f));
        CHECK(v[3].victim == 9 && near(v[3].intensity, 0.5f));
    }
    // Offset explosion, default intensity 1, range 50.
    const std::vector<ExplosionFlinch> o = explosion_flinches(100.0f, 100.0f, 0.0f, 50.0f, 1.0f, c);
    CHECK(o.size() == 1 && o[0].victim == 9 && near(o[0].dx, -40.0f) && near(o[0].dy, -20.0f) &&
          near(o[0].intensity, 1.0f - 0.5f * 2000.0f / 2500.0f));
    CHECK(explosion_flinches(0.0f, 0.0f, 0.0f, 0.0f, 1.0f, c).empty());

    // Through the world callbacks: no owner / dying / self filter, IDLE required.
    FakeWorld w;
    w.add(20, kOther, 1, 0.0f, 0.0f, 0.0f, false);  // the explosion object itself (no IDLE)
    w.add(21, kTrooper, 12, 30.0f, 40.0f).dying = true;
    w.add(22, kTrooper, 1, 0.0f, 0.0f);
    w.add(23, kTrooper, 1, 0.0f, 300.0f);
    const std::vector<ExplosionFlinch> ww = explosion_flinches(0.0f, 0.0f, 0.0f, kDefaultInducedFlinchRange,
                                                               kDefaultInducedFlinchIntensity, w);
    CHECK(ww.size() == 2);
    if (ww.size() == 2) {
        CHECK(ww[0].victim == 21 && near(ww[0].intensity, 1.0f - 0.5f * 2500.0f / 10000.0f));
        CHECK(ww[1].victim == 22 && near(ww[1].intensity, 1.0f));
    }
}

#ifdef ALAMO_PRESENCE_TESTS_HAVE_DATA
void test_foc_data() {
    // Parser on its own.
    {
        const std::vector<std::string_view> toks = {"Stray", "Attention", "A", "Unknown", "B", "CELEBRATE", "B"};
        auto anim = [](std::string_view s) { return anim::find_anim_type(s); };
        auto type = [](std::string_view s) -> TypeId { return s == "A" ? 1u : s == "B" ? 2u : kNoType; };
        const std::vector<InducedAnimation> e = parse_presence_induced_animations(toks, anim, type);
        CHECK(e.size() == 2);
        if (e.size() == 2) {
            CHECK(e[0].anim_type == kAnimAttention && e[0].types == std::vector<TypeId>({1u, 2u}));
            CHECK(e[1].anim_type == kAnimCelebrate && e[1].types == std::vector<TypeId>({2u}));
        }
        CHECK(presence_config(nullptr).relaxation_frames() == 150);
        data::GameConstants gc;  // empty: the default applies
        CHECK(presence_config(&gc).relaxation_frames() == 150);
    }
    const char* root = std::getenv("ALAMO_FOC_DATA");
    if (!root || !*root) {
        std::printf("foc data: skipped (ALAMO_FOC_DATA unset)\n");
        return;
    }
    vfs::VfsConfig vcfg;
    vcfg.foc_root = root;
    std::string err;
    const std::unique_ptr<vfs::Vfs> v = vfs::Vfs::create(vcfg, &err);
    if (!v) {
        std::printf("FAIL foc data: VFS: %s\n", err.c_str());
        ++g_fail;
        return;
    }
    objects::ObjectTypeDatabase db;
    std::vector<std::string> diag;
    CHECK(db.load(*v, &diag));
    objects::GameObjectTypes types;
    CHECK(types.build(db, v.get(), &diag, &anim::find_anim_type));
    const PresenceTypeTable t = build_presence_types(types, &anim::find_anim_type);
    CHECK(t.size() == types.size());
    auto id = [&](std::string_view n) -> TypeId {
        const objects::TypeIndex i = types.resolve_name(n);
        return i == objects::kNone ? kNoType : static_cast<TypeId>(i);
    };
    const TypeId storm = id("Stormtrooper"), vader = id("Darth_Vader"), emperor = id("Emperor_Palpatine");
    const TypeId rebel = id("Rebel_Trooper"), plex = id("Plex_Soldier"), clone0 = id("AT_AT_Walker_Death_Clone_00");
    const TypeId guard = id("Imperial_Guard_Non_AI"), vader_x = id("Darth_Vader_Expansion");
    CHECK(storm != kNoType && vader != kNoType && emperor != kNoType && rebel != kNoType && clone0 != kNoType);
    if (storm == kNoType || vader == kNoType || emperor == kNoType || rebel == kNoType || clone0 == kNoType) return;
    CHECK(t.induced_anim(storm, vader) == kAnimAttention);
    CHECK(t.induced_anim(storm, emperor) == kAnimAttention);
    CHECK(t.induced_anim(storm, clone0) == kNoAnimType);
    if (guard != kNoType) CHECK(t.induced_anim(guard, vader) == kAnimAttention);
    if (vader_x != kNoType) CHECK(!t.lists(storm, vader_x));  // its own type, not a variant: induces nothing
    CHECK(t.induced_anim(rebel, clone0) == kAnimCelebrate);
    if (plex != kNoType) CHECK(t.induced_anim(plex, clone0) == kAnimCelebrate);
    CHECK(near(t.find(vader)->presence_range, 100.0f));
    CHECK(near(t.find(clone0)->presence_range, 200.0f));
    CHECK(types.has_behavior(vader, objects::BehaviorSet::Land, 0x40));  // IMPOSING_PRESENCE
    uint32_t holders = 0;
    for (TypeId i = 0; i < t.size(); ++i)
        if (!t.find(i)->induced.empty()) ++holders;
    std::printf("foc data: %u types with Presence_Induced_Animations\n", holders);
    CHECK(holders >= 5);
    // EXPLOSION types: no vanilla XML sets Induced_Flinch_*, so the constructor defaults apply.
    const TypeId boom = id("Medium_Explosion_Land");
    CHECK(boom != kNoType);
    if (boom != kNoType) {
        const ExplosionFlinchParams p = explosion_flinch_params(types, boom);
        CHECK(near(p.range, 100.0f) && near(p.intensity, 1.0f));
        // R3f t05: objects' constructor defaults are exactly the presence / anim constants.
        CHECK(p.range == kDefaultInducedFlinchRange && p.intensity == kDefaultInducedFlinchIntensity);
        CHECK(types.induced_flinch_range(boom) == kDefaultInducedFlinchRange &&
              types.induced_flinch_intensity(boom) == kDefaultInducedFlinchIntensity);
    }
}
#endif

}  // namespace

int main() {
    test_service_timing();
    test_service_gates();
    test_countdown_and_clears();
    test_winner_rule();
    test_default_idle();
    test_list_order();
    test_update_order();
    test_save();
    test_explosion();
#ifdef ALAMO_PRESENCE_TESTS_HAVE_DATA
    test_foc_data();
#else
    std::printf("foc data: skipped (no objects / vfs / anim in this build)\n");
#endif
    if (g_fail) {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("alamo_tactical_presence_tests: all passed\n");
    return 0;
}
