#pragma once
// Split out of test_movement_gaps.cpp (bo3 t03): shared includes, types and helpers; the tests live in test_movement_gaps_partN.cpp.
// alamo_tactical_movement_gaps_tests (no framework; non-zero exit on failure).
// docs/re/REMAINING_GAPS.md §4.1-4.7: corridor squeeze, threat mode (space ops 25-32, land region cost), and the
// jetpack, flying, bike, land-bomber and fighter locomotors; determinism of a mixed scenario across 1 / 4 / N threads
// with a golden CRC.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/host_fs.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/tactical/movement/corridor.hpp"
#include "alamo/tactical/movement/movement_system.hpp"
#include "alamo/tactical/movement/space_avoid.hpp"
#include "alamo/tactical/movement/special_locomotors.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_types.hpp"
#include "alamo/test/thread_parallel_for.hpp"
#include "alamo/test/support.hpp"

using namespace alamo;
using namespace alamo::tactical::movement;

using ::alamo::test::g_fail;
ParallelFor make_thread_pf(unsigned threads);
bool near(float a, float b, float eps);
PassabilityGrid open_grid(uint32_t w, uint32_t h);
UnitDesc unit(ObjectId id, V2 pos, int32_t cls, float speed, float radius, LocomotorKind kind);
void test_lanes();
void test_corridor();
bool compass_aligned(V2 d);
void test_threat();
void test_rules();
void test_flying_rules();
uint32_t anim_len(ObjectId, int32_t a);
void land_system(MovementSystem& sys, uint32_t cells);
void test_jetpack();
void test_flying();
void test_flying_clearance();
UnitDesc trooper(ObjectId id, V2 pos);
void test_multi_locomotor();
void test_jetpack_alternates();
void test_jetpack_accel_fallback();
void test_jetpack_no_repeated_pose();
uint32_t band_anim_len(ObjectId id, int32_t a);
void test_jetpack_turn_bands();
void test_jetpack_orders();
void test_multi_locomotor_types();
void test_bike();
void test_bomber();
void test_fighter_space();
void test_fighter_guard();
void test_fighter_formation_stack();
CrcLog mixed_run(const ParallelFor& pf);

// Golden digest of the final record (MSVC and clang must both produce it). S7 collision layer (2026-10-29): moved (the
// land hash appends the collision objects; priority-ordered resolution). FIGHTER_FLYING §6 flying (2026-10-29): moved
// (Hermite follower, attitude / altitude steps, D + dir x L loiter; flying units hash pitch, V, path index, target).
// S7 t15 (2026-11-02): moved (the Has_Pre_Turn_Anim jetpack's take-off turn: band rates, one-shot clips, clip-end exit).
// cu8i t02 (2026-11-08): moved (land formations draw the FormationClass ctor int(1, 30); start now + acc + r - 30).
// cu8j t01 / t02 (2026-11-08): compositions at the next coordinator update (draws, start frames, the jetpack swap's draw)
// and the hash's pending composition / adopt state.
// s78n t20 F5 (2026-11-10): moved (checked orders take no cell snap; the single-member shortcut's exact goal is hashed).
inline constexpr uint64_t kGoldenMixed = 0xc3b17af7a5258abeull;  // bkm1: lane fallback / stop point, shield-free class, recruitment, stored z, secondary facing; s78p: formation path and lanes; s78n t26.4: the move step C (no snap); s78n t26.3: the corner pass; s78n t26.2: the speed controller, the start turn (s78n t20: the path stack top, formation identity, the destination check; F5)
AvoidModel wall_model();
void test_space_avoid();
void test_speed_override();
void test_arrival_turn();
void test_forced_path();
void test_team_cap();
void test_squad_team_speed();
void test_team_speed_exact();
void test_arrival_turn_rules();
bool arrival_clips(ObjectId, int32_t anim, uint32_t& n, float& fps);
void test_arrival_turn_clips();
void test_arrival_close();
void test_forced_destination();
void test_land_squad();
V2 t14v(float x, float y);
float t14_radinv(uint32_t i, uint32_t b);
bool t14_dartboard(uint32_t n, float hx, float hy, float g, uint32_t seed, float spread, std::vector<V2>& pts);

// Members 1..n of a container 100 at `at` (yaw 0); member i at at + rel[i - 1], shapes (hx, hy, type hx, type hy) and
// FormationSpacing per member.
struct T14Member {
    V2 rel;
    float hx = 3.0f, hy = 3.0f, spacing = 0.5f;
};
void t14_squad(MovementSystem& sys, V2 at, int32_t arrangement, const std::vector<T14Member>& mem, float speed = 2.0f);
V2 t14_slot(const MovementSystem& sys, ObjectId id);
void test_land_squad_layout();
void test_land_squad_walk_states();
void test_land_squad_review();
void test_determinism();
void test_deploy();
void test_turret();
void t14b_squad(MovementSystem& sys, V2 at, int32_t owner, bool avoidance_disabled = false);
V2 t14b_slot_point(const MovementSystem& sys, ObjectId id);
void test_land_squad_part_b();

// s78n t16 (FIGHTER_FLYING §7.7): a squad - container 100 (raw Max_Speed `cmax`, MaxJiggleDistance `cjiggle`) and members
// 1..n at `at` + rel[i] (raw Max_Speed mmax[i], OverrideAcceleration = OverrideDeceleration mdecel[i], half-extents 3).
struct T16Member {
    V2 rel;
    float max_speed = 1.0f, decel = 0.0f, spacing = 0.3f;
    float back = 0.0f;  // the member starts this far behind its slot (-x)
};
void t16_squad(MovementSystem& sys, V2 at, int32_t arrangement, float cmax, float cjiggle, const std::vector<T16Member>& mem,
                      int32_t owner = -1, bool squashable = false);
void test_land_squad_speed_step();
V2 t14b_avoid_ref(const MovementSystem& sys, ObjectId member, V2 dir, float vmax, uint32_t& added, uint32_t& dropped);
void test_land_squad_review_fixes();
void test_land_squad_teleport();
std::vector<T16Member> t18_storm();
uint32_t t18_brake_steps(float vmax, float a);
void test_land_squad_footprint();
void t18_squad(MovementSystem& sys, ObjectId cid, V2 at, float yaw, int32_t owner, float member_speed = 1.0f);
void test_land_squad_repulsion();
void test_land_squad_speed_order();
void test_land_squad_gates();
void test_land_squad_t18_review();
void t20_squad(MovementSystem& sys, ObjectId cid, V2 at, int32_t owner, float cmax = 0.8f);
UnitDesc t20_vehicle(ObjectId id, V2 pos, int32_t owner, float hx = 10.0f, float hy = 6.0f, float speed = 1.0f);
void test_t20_destination_check();
void test_t20_detour_start_inside();
void test_t20_unsnapped_goal();
void test_t20_path_around();
void test_t20_early_arrival();
void test_t20_decision_leftovers();
void test_t20_stopped_vs_stopped();
UnitDesc t20_clone(ObjectId id, V2 pos, int32_t cls, float hx, float hy);
void test_t20_death_clone();

// Test functions (defined in the part files)
