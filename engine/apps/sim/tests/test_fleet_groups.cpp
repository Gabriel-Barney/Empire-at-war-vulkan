// [port] Fleet groups, phase 1 (sim/fleet_groups.hpp; docs/DECISIONS.md "Fleet groups: exception to the current focus").
//   * the command payloads round-trip and malformed ones are refused;
//   * FleetGroups on a fake world: create / lock / slots, the locked-move plan, the slowest-member cap and the straggler
//     wait, interrupt, prune (deaths, owner change), remove / dissolve / rename rules, the group cap, save round trip;
//   * a space battle (the S10 fixture): with the switch off every fleet command is refused and moves pass through
//     unchanged; with it on, the human's ships made into a locked group move as a block at the slowest ship's speed and
//     end on their slots. The per-tick battle digests are the worker-count check (--fleet mode, one ctest entry per count).
#include "battle_test_fixture.hpp"

#include "alamo/commands/fleet_group.hpp"
#include "alamo/core/host_fs.hpp"
#include "alamo/sim/fleet_groups.hpp"

namespace mv = alamo::tactical::movement;
namespace cmd = alamo::commands;

namespace {

using R = cmd::RejectReason;
using Op = cmd::FleetGroupOp;

cmd::FleetGroupCommand op(Op o, bool locked = false) {
    cmd::FleetGroupCommand c;
    c.op = o;
    c.locked = locked;
    return c;
}

bool near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }

void test_commands() {
    for (const Op o : {Op::Create, Op::Dissolve, Op::RemoveUnits}) {
        const cmd::EventData e = cmd::fleet_group_event(op(o));
        CHECK(e.type() == cmd::EventType::PortExtension && e.u("subtype") == cmd::kFleetGroupSubtype);
        const auto p = cmd::parse_fleet_group(e.bytes_view("data"));
        CHECK(p && p->op == o);
    }
    for (const bool l : {false, true}) {
        const auto p = cmd::parse_fleet_group(cmd::fleet_group_event(op(Op::SetLock, l)).bytes_view("data"));
        CHECK(p && p->op == Op::SetLock && p->locked == l);
    }
    cmd::FleetGroupCommand rn = op(Op::Rename);
    rn.group = 0x1234;
    rn.name = "Red Squadron";
    const auto p = cmd::parse_fleet_group(cmd::fleet_group_event(rn).bytes_view("data"));
    CHECK(p && p->op == Op::Rename && p->group == 0x1234 && p->name == "Red Squadron");
    rn.name = std::string(50, 'x');  // cut to kFleetGroupNameMax when encoded
    const auto cut = cmd::parse_fleet_group(cmd::fleet_group_event(rn).bytes_view("data"));
    CHECK(cut && cut->name.size() == cmd::kFleetGroupNameMax);
    const uint8_t bad1[] = {9}, bad2[] = {0, 1}, bad3[] = {3, 2}, bad4[] = {4, 1, 0, 5, 'a'};
    CHECK(!cmd::parse_fleet_group({}) && !cmd::parse_fleet_group(bad1) && !cmd::parse_fleet_group(bad2) &&
          !cmd::parse_fleet_group(bad3) && !cmd::parse_fleet_group(bad4));
}

struct World {
    std::map<objects::ObjectId, FleetUnitInfo> units;
    void add(objects::ObjectId id, float x, float y, float speed, int32_t owner = 0) {
        FleetUnitInfo u;
        u.valid = true;
        u.owner = owner;
        u.pos = {x, y};
        u.max_speed = speed;
        units[id] = u;
    }
    FleetGroups::InfoFn fn() {
        return [this](objects::ObjectId id) {
            const auto it = units.find(id);
            return it == units.end() ? FleetUnitInfo{} : it->second;
        };
    }
};

void test_logic() {
    World w;
    w.add(10, 0.0f, 0.0f, 3.0f);
    w.add(11, 90.0f, 0.0f, 5.0f);
    w.add(12, 0.0f, 90.0f, 5.0f);
    w.add(13, 300.0f, 300.0f, 5.0f);  // the player's, never grouped
    w.add(20, 500.0f, 500.0f, 5.0f, 1);
    const FleetGroups::InfoFn info = w.fn();
    FleetGroups fg;
    CHECK(!fg.used() && fg.groups().empty());
    const objects::ObjectId none[] = {0};
    CHECK(fg.apply(0, op(Op::Create), std::span<const objects::ObjectId>(none, 0), info) == R::InvalidUnit);
    const objects::ObjectId sel[] = {12, 10, 11, 10};
    CHECK(fg.apply(0, op(Op::Create), sel, info) == R::None);
    CHECK(fg.used() && fg.groups().size() == 1);
    const FleetGroup& g = fg.groups()[0];
    CHECK(g.id == 1 && g.owner == 0 && !g.locked && g.members.size() == 3 && g.members[0].id == 10 && g.members[2].id == 12);
    // centre (30, 30), frame forward = member 10's yaw (0): slot = (x - 30, y - 30)
    CHECK(near(g.members[0].slot.x, -30.0f) && near(g.members[0].slot.y, -30.0f) && near(g.members[1].slot.x, 60.0f) &&
          near(g.members[2].slot.y, 60.0f));
    CHECK(fg.group_of(11) == &g && fg.group_of(13) == nullptr);

    // Unlocked: a move plan passes everything through.
    std::vector<FleetGroups::LockedMove> moves;
    std::vector<objects::ObjectId> rest;
    const objects::ObjectId mv_sel[] = {13, 10, 11, 12, 20};
    fg.plan_move(0, mv_sel, {1000.0f, 0.0f}, mv::kNoFacing, info, moves, rest);
    CHECK(moves.empty() && rest.size() == 5);

    const objects::ObjectId one[] = {11};
    CHECK(fg.apply(0, op(Op::SetLock, true), one, info) == R::None && fg.groups()[0].locked);
    CHECK(fg.apply(1, op(Op::SetLock, true), one, info) == R::InvalidUnit);  // not player 1's group

    // Locked, facing 90 (forward +Y, left -X): goal = dest + (0, along) + (-side, 0), mean slot 0.
    fg.plan_move(0, mv_sel, {1000.0f, 0.0f}, 90.0f, info, moves, rest);
    CHECK(moves.size() == 1 && moves[0].goals.size() == 3 && near(moves[0].facing_deg, 90.0f));
    CHECK(rest.size() == 2 && rest[0] == 13 && rest[1] == 20);  // selection order kept
    if (moves.size() == 1 && moves[0].goals.size() == 3) {
        const mv::V2 g11 = moves[0].goals[1].goal;  // slot (60, -30): (1000 + 30, 60)
        CHECK(moves[0].goals[1].id == 11 && near(g11.x, 1030.0f, 0.05f) && near(g11.y, 60.0f, 0.05f));
    }
    // Without a facing: the frame turns to the direction from the members' centroid to the destination.
    fg.plan_move(0, mv_sel, {30.0f, 1030.0f}, mv::kNoFacing, info, moves, rest);
    CHECK(moves.size() == 1 && near(moves[0].facing_deg, 90.0f, 0.05f));
    // Only part of the group selected: the selected members move as a sub-block around their own mean slot.
    const objects::ObjectId part[] = {10, 11};
    fg.plan_move(0, part, {500.0f, 500.0f}, 0.0f, info, moves, rest);
    CHECK(moves.size() == 1 && moves[0].goals.size() == 2 && rest.empty());
    if (moves.size() == 1 && moves[0].goals.size() == 2)  // mean slot (15, -30): member 10 at (500 - 45, 500)
        CHECK(near(moves[0].goals[0].goal.x, 455.0f, 0.05f) && near(moves[0].goals[0].goal.y, 500.0f, 0.05f));

    // Speed: every member at the slowest's speed (3 / 5 for 11 and 12), stragglers waited for.
    fg.plan_move(0, mv_sel, {1000.0f, 0.0f}, 0.0f, info, moves, rest);
    CHECK(moves.size() == 1);
    fg.start_move(moves[0], info);
    CHECK(fg.any_moving() && fg.groups()[0].members[0].start_dist > 900.0f);
    for (auto& [id, u] : w.units) u.has_goal = true;
    std::map<objects::ObjectId, float> scales;
    const auto take = [&scales](objects::ObjectId id, float s) { scales[id] = s; };
    fg.regulate(info, take);
    CHECK(scales.size() == 2 && near(scales[11], 0.6f) && near(scales[12], 0.6f) && !scales.count(10));
    // 11 runs 400 ahead of the straggler 10 (lead 400 > 150): scale 0.6 x max(0.25, 1 - 250 / 150) = 0.15
    {
        const FleetMember& m11 = fg.groups()[0].members[1];
        const mv::V2 dir = (m11.goal - w.units[11].pos) * (1.0f / mv::length(m11.goal - w.units[11].pos));
        w.units[11].pos = w.units[11].pos + dir * 400.0f;
    }
    scales.clear();
    fg.regulate(info, take);
    CHECK(near(scales[11], 0.15f) && near(scales[12], 0.6f));
    // 11 only 200 ahead: 0.6 x (1 - 50 / 150) = 0.4
    {
        const FleetMember& m11 = fg.groups()[0].members[1];
        const mv::V2 dir = (m11.goal - w.units[11].pos) * (1.0f / mv::length(m11.goal - w.units[11].pos));
        w.units[11].pos = w.units[11].pos - dir * 200.0f;
    }
    scales.clear();
    fg.regulate(info, take);
    CHECK(near(scales[11], 0.4f, 0.02f));
    // arrived (no goals): the move ends, slots unchanged
    const mv::V2 slot_before = fg.groups()[0].members[1].slot;
    for (auto& [id, u] : w.units) u.has_goal = false;
    scales.clear();
    fg.regulate(info, take);
    CHECK(!fg.any_moving() && scales.empty() && fg.groups()[0].members[1].slot == slot_before);
    // another order interrupts a running move
    fg.start_move(moves[0], info);
    const objects::ObjectId i12[] = {12};
    fg.interrupt(i12);
    CHECK(!fg.any_moving());

    // save round trip: same hash, bad data refused (state unchanged)
    {
        ByteWriter bw;
        fg.write(bw);
        FleetGroups copy;
        ByteReader br(bw.bytes());
        CHECK(copy.read(br) && br.done());
        StateHasher h1, h2;
        fg.hash(h1);
        copy.hash(h2);
        CHECK(h1.digest() == h2.digest() && copy.used() && copy.groups().size() == 1);
        std::vector<uint8_t> cut(bw.bytes().begin(), bw.bytes().end() - 3);
        ByteReader bc(cut);
        FleetGroups other;
        CHECK(!other.read(bc) && other.groups().empty());
    }

    // rename: own group only, cut to the cap
    cmd::FleetGroupCommand rn = op(Op::Rename);
    rn.group = 1;
    rn.name = "Home Fleet";
    CHECK(fg.apply(1, rn, {}, info) == R::InvalidTarget && fg.apply(0, rn, {}, info) == R::None && fg.groups()[0].name == "Home Fleet");
    rn.group = 7;
    CHECK(fg.apply(0, rn, {}, info) == R::InvalidTarget);

    // prune: a dead member leaves, a member that changed owner leaves, an empty group goes
    w.units[11].valid = false;
    fg.prune(info);
    CHECK(fg.groups()[0].members.size() == 2);
    w.units[12].owner = 1;
    fg.prune(info);
    CHECK(fg.groups()[0].members.size() == 1 && fg.groups()[0].members[0].id == 10);
    w.units[11].valid = true;
    w.units[12].owner = 0;

    // a unit is in one group: a new group takes it out of the old one
    const objects::ObjectId s2[] = {11, 12};
    CHECK(fg.apply(0, op(Op::Create), s2, info) == R::None && fg.groups().size() == 2 && fg.groups()[1].id == 2);
    const objects::ObjectId s3[] = {10, 11};
    CHECK(fg.apply(0, op(Op::Create), s3, info) == R::None);
    CHECK(fg.groups().size() == 2 && fg.group_of(12)->id == 2 && fg.group_of(12)->members.size() == 1 && fg.group_of(10)->id == 3);
    const objects::ObjectId r12[] = {12};
    CHECK(fg.apply(0, op(Op::RemoveUnits), r12, info) == R::None && fg.groups().size() == 1);
    CHECK(fg.apply(0, op(Op::RemoveUnits), r12, info) == R::InvalidUnit);
    CHECK(fg.apply(0, op(Op::Dissolve), s3, info) == R::None && fg.groups().empty() && fg.used());

    // the group cap per player
    for (objects::ObjectId id = 100; id < 100 + static_cast<objects::ObjectId>(FleetGroups::kMaxGroupsPerPlayer) + 1; ++id)
        w.add(id, static_cast<float>(id), 0.0f, 4.0f);
    for (size_t k = 0; k < FleetGroups::kMaxGroupsPerPlayer; ++k) {
        const objects::ObjectId u[] = {static_cast<objects::ObjectId>(100 + k)};
        CHECK(fg.apply(0, op(Op::Create), u, info) == R::None);
    }
    const objects::ObjectId last[] = {static_cast<objects::ObjectId>(100 + FleetGroups::kMaxGroupsPerPlayer)};
    CHECK(fg.apply(0, op(Op::Create), last, info) == R::QueueFull);
    const objects::ObjectId again[] = {100};  // regrouping a group's only unit replaces that group: allowed at the cap
    CHECK(fg.apply(0, op(Op::Create), again, info) == R::None && fg.groups().size() == FleetGroups::kMaxGroupsPerPlayer);
    CHECK(fg.apply(1, op(Op::Create), std::span<const objects::ObjectId>(), info) == R::InvalidUnit);
}

// ---- the space battle ----

struct FleetRun {
    std::vector<uint64_t> digests;
    bool ordered = false;      // the group was made, locked and moved
    bool refused_off = false;  // switch off: every fleet command refused, the move passed through
    bool arrived = false;      // the move ended with every member near its slot goal
    bool capped = true;        // no member ran faster than the slowest member's max speed during the move
    bool split_speeds = false; // the members had different max speeds (the cap was tested)
    uint32_t members = 0, move_ticks = 0;
    float worst_goal = 0.0f, fastest_over = 0.0f;
};

FleetRun run_fleet_battle(const Data& d, int32_t workers, bool on) {
    FleetRun out;
    jobs::JobSystem js(jobs::JobSystemConfig{workers});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSetup setup = make_setup(d, false);
    // Extra ships for player 0 at the battle planet: the fixture's Space_Tactical_Unit_Cap of 6 (the star base takes one slot)
    // deploys five movable ships, the rest wait as reinforcements; the group takes every deployed ship.
    for (int k = 0; k < 2; ++k) {
        setup.forces.push_back({0, 1, type_of(d, "S10_Frigate")});
        setup.forces.push_back({0, 1, type_of(d, "S10_Corvette")});
    }
    galactic::GalacticSim* gs = host.start_galactic(d.rules, setup);
    CHECK(gs != nullptr);
    if (!gs) return out;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 3000;
    opt.battle.skip_human_units = true;  // the human's ships take only this test's orders
    opt.fleet_groups = on;
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Space_B") return false;
                                m = space_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == R::None);
    std::vector<objects::ObjectId> ids;
    float slowest = 0.0f;
    bool moving = false;
    for (uint64_t t = 0; t < 2400; ++t) {
        host.tick_once();
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm || b->frame() < 1) {
            if (out.ordered) break;
            continue;
        }
        StateHasher hs;
        b->hash(hs);
        b->movement().hash_units(hs);
        out.digests.push_back(hs.digest());
        const objects::ObjectStore& st = tm->primary();
        if (b->frame() == 3 && ids.empty()) {
            for (objects::ObjectHandle h : st.owned_by(0))
                if (st.alive(h) && b->movement().has_unit(st.id(h)) && ids.size() < 6) ids.push_back(st.id(h));
            std::sort(ids.begin(), ids.end());
            CHECK(ids.size() >= 2);
            if (ids.size() < 2) break;
            out.members = static_cast<uint32_t>(ids.size());
            SimRng& rng = host.random().synced();
            // away from the enemy: the members' centroid moved 900 along (own centroid - enemy centroid)
            float ox = 0.0f, oy = 0.0f, ex = 0.0f, ey = 0.0f;
            uint32_t en = 0;
            for (const objects::ObjectId id : ids) {
                const Vec3 p = st.position(st.find(id));
                ox += p.x;
                oy += p.y;
            }
            ox /= static_cast<float>(ids.size());
            oy /= static_cast<float>(ids.size());
            for (objects::ObjectHandle h : st.owned_by(1))
                if (st.alive(h)) {
                    const Vec3 p = st.position(h);
                    ex += p.x;
                    ey += p.y;
                    ++en;
                }
            float dx = 1.0f, dy = 0.0f;
            if (en > 0) {
                dx = ox - ex / static_cast<float>(en);
                dy = oy - ey / static_cast<float>(en);
                const float l = std::sqrt(dx * dx + dy * dy);
                if (l > 1.0f) {
                    dx /= l;
                    dy /= l;
                } else {
                    dx = 1.0f;
                    dy = 0.0f;
                }
            }
            const float tx = ox + 900.0f * dx, ty = oy + 900.0f * dy;
            const auto data = [](const cmd::FleetGroupCommand& c) {
                const cmd::EventData e = cmd::fleet_group_event(c);
                const std::span<const uint8_t> v = e.bytes_view("data");
                return std::vector<uint8_t>(v.begin(), v.end());
            };
            std::vector<objects::ObjectId> rest;
            uint32_t blocks = 0;
            if (!on) {
                out.refused_off = b->fleet_group_command(*tm, 0, data(op(Op::Create)), ids) == R::NotAllowed &&
                                  b->fleet_groups().groups().empty();
                b->fleet_order_move(*tm, 0, ids, tx, ty, rng, mv::kNoFacing, false, false, mv::kMoveFlagPlayerOrder, rest, blocks);
                out.refused_off = out.refused_off && blocks == 0 && rest == ids;
                CHECK(b->order_move(*tm, 0, rest, tx, ty, rng, mv::kNoFacing, false, false, mv::kMoveFlagPlayerOrder) == R::None);
                break;
            }
            CHECK(b->fleet_group_command(*tm, 0, data(op(Op::Create)), ids) == R::None);
            CHECK(b->fleet_group_command(*tm, 0, data(op(Op::SetLock, true)), ids) == R::None);
            CHECK(b->fleet_group_command(*tm, 1, data(op(Op::Create)), ids) == R::InvalidUnit);  // not player 1's ships
            slowest = 3.0e38f;
            float fastest = 0.0f;
            for (const objects::ObjectId id : ids) {
                slowest = std::min(slowest, b->movement().view(id).max_speed);
                fastest = std::max(fastest, b->movement().view(id).max_speed);
            }
            out.split_speeds = fastest > slowest * 1.2f;
            b->fleet_order_move(*tm, 0, ids, tx, ty, rng, mv::kNoFacing, false, false, mv::kMoveFlagPlayerOrder, rest, blocks);
            CHECK(blocks == 1 && rest.empty());
            out.ordered = blocks == 1 && b->fleet_groups().any_moving();
            moving = out.ordered;
            continue;
        }
        if (!on || !moving) continue;
        const FleetGroup* g = b->fleet_groups().groups().empty() ? nullptr : &b->fleet_groups().groups()[0];
        if (!g) break;
        if (g->moving) {
            ++out.move_ticks;
            for (const FleetMember& m : g->members) {
                const mv::UnitView v = b->movement().view(m.id);
                const float over = v.speed - slowest;
                out.fastest_over = std::max(out.fastest_over, over);
                if (over > slowest * 0.02f + 0.01f) out.capped = false;
            }
            continue;
        }
        moving = false;
        out.arrived = true;
        for (const FleetMember& m : g->members) {
            const float dist = mv::distance(b->movement().view(m.id).pos, m.goal);
            out.worst_goal = std::max(out.worst_goal, dist);
            if (dist > 120.0f) out.arrived = false;
        }
        break;
    }
    return out;
}

int fleet_worker_check(int argc, char** argv) {
    uint32_t workers = 0;
    std::string out_path, golden_path;
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--workers") workers = static_cast<uint32_t>(std::strtoul(argv[i + 1], nullptr, 10));
        else if (a == "--crc-out") out_path = argv[i + 1];
        else if (a == "--golden") golden_path = argv[i + 1];
    }
    static std::string prefix;  // a fixture folder per worker count (the entries run side by side under ctest -j)
    prefix = "alamo_sim_fleet_groups_w" + std::to_string(workers);
    g_temp_prefix = prefix.c_str();
    auto d = make_data();
    const FleetRun r = run_fleet_battle(*d, static_cast<int32_t>(workers), true);
    CHECK(r.ordered && r.arrived && r.capped && r.members == 5 && !r.digests.empty());
    std::printf("fleet groups on %u workers: %zu ticks, %u members, move %u ticks\n", workers, r.digests.size(), r.members, r.move_ticks);
    if (!out_path.empty()) {
        std::ofstream f(out_path, std::ios::binary | std::ios::trunc);
        for (const uint64_t v : r.digests) f << std::hex << v << "\n";
        CHECK(static_cast<bool>(f));
    }
    if (!golden_path.empty()) {
        std::ifstream f(golden_path, std::ios::binary);
        std::vector<uint64_t> want;
        uint64_t v = 0;
        while (f >> std::hex >> v) want.push_back(v);
        CHECK(!want.empty());
        size_t first = 0;
        while (first < want.size() && first < r.digests.size() && want[first] == r.digests[first]) ++first;
        if (want != r.digests) {
            std::printf("fleet groups on %u workers: digests differ from the 0-worker run at tick %zu (%zu vs %zu ticks)\n", workers,
                        first, r.digests.size(), want.size());
            ++g_fail;
        }
    }
    std::error_code ec;
    remove_all_long(d->root, ec);
    if (g_fail) {
        std::printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    g_temp_prefix = "alamo_sim_fleet_groups_tests";
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--fleet") return fleet_worker_check(argc, argv);
    std::printf("fleet group commands\n");
    test_commands();
    std::printf("fleet groups on a fake world\n");
    test_logic();
    auto d = make_data();
    std::printf("fleet groups in a space battle: switch off\n");
    const FleetRun off = run_fleet_battle(*d, 0, false);
    CHECK(off.refused_off);
    std::printf("fleet groups in a space battle: switch on\n");
    const FleetRun on = run_fleet_battle(*d, 0, true);
    CHECK(on.ordered && on.arrived && on.capped);
    std::printf("  %u members, speeds differ %d, move %u ticks, worst slot distance %.1f, fastest over cap %.3f\n", on.members,
                on.split_speeds ? 1 : 0, on.move_ticks, static_cast<double>(on.worst_goal), static_cast<double>(on.fastest_over));
    CHECK(on.split_speeds && on.members == 5);  // the fixture's frigates (3) and corvettes (5): the cap is exercised
    std::error_code ec;
    remove_all_long(d->root, ec);
    if (g_fail) {
        std::printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    std::printf("all fleet group checks passed\n");
    return 0;
}
