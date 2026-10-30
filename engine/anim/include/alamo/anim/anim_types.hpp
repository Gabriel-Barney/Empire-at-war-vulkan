// R3 animation types (docs/re/ANIM_PARTICLES.md §A.1): the original's ModelAnimType ids 0..118, their
// XML / Lua names (Specific_Death_Anim_Type, Attack_Animation, Play_Animation, ...) and their ALA file
// suffixes (<base>_<SUFFIX>_<NN>.ALA). The suffix of id 113 keeps the shipped misspelling
// CONTAIMINATE_LOOP. Ids 90 (HACKING) and 91 (REPAIRING) have a suffix but no XML name.
//
// The type groups are the switch predicates the behaviours test (§A.1 second table).
#pragma once
#include <cstdint>
#include <string_view>

namespace alamo::anim {

inline constexpr uint32_t kAnimTypeCount = 119;
inline constexpr int32_t kNoAnimType = -1;

// Named ids used by the engine's play rules (§A.7). Every id 0..118 is valid.
enum AnimType : int32_t {
    kAnimIdle = 0,
    kAnimSpaceIdle = 1,
    kAnimMove = 2,
    kAnimTurnLeft = 3,
    kAnimTurnRight = 4,
    kAnimAttack = 5,
    kAnimAttackIdle = 6,
    kAnimDie = 7,
    kAnimDeploy = 25,
    kAnimUndeploy = 26,
    kAnimCinematic = 27,
    kAnimFireMove = 61,
    kAnimWalkMove = 66,
    kAnimCrouchMove = 67,
    kAnimIdleCrouch = 71,
    kAnimFlinchLeft = 79,
    kAnimFlinchRight = 80,
    kAnimFlinchFront = 81,
    kAnimFlinchBack = 82,
    kAnimAttackFlinchLeft = 83,
    kAnimAttackFlinchRight = 84,
    kAnimAttackFlinchFront = 85,
    kAnimAttackFlinchBack = 86,
    kAnimSpinMove = 104,
    kAnimContaminateLoop = 113,
    kAnimDeployedWalk = 115,
};

// XML / Lua name of an id ("" for 90, 91 and out-of-range ids).
std::string_view anim_type_name(int32_t id);
// ALA file suffix of an id ("" for out-of-range ids).
std::string_view anim_type_suffix(int32_t id);
// XML name -> id, ASCII case-insensitive (vanilla XML writes DIE / FL_DIE). kNoAnimType if unknown.
int32_t find_anim_type(std::string_view xml_name);

// §A.1 type groups.
bool is_attack_like(int32_t id);     // 5, 25, 26, 31, 33-35, 37-40, 45-47, 63, 64
bool is_idle_like(int32_t id);       // 0, 1, 71, 77, 78, 79-82 (interruptible)
bool is_turn(int32_t id);            // 3, 4, 21-24, 72, 73
bool is_crouch(int32_t id);          // 67, 71, 72, 73
bool is_flinch(int32_t id);          // 79-86
bool is_one_shot_pose(int32_t id);   // 7, 9, 10, 11, 32, 36, 41, 58, 62

}  // namespace alamo::anim
