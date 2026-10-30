#include "alamo/anim/anim_types.hpp"

#include <array>

namespace alamo::anim {

namespace {

struct TypeEntry {
    std::string_view name;
    std::string_view suffix;
};

// docs/re/ANIM_PARTICLES.md §A.1, in id order.
constexpr std::array<TypeEntry, kAnimTypeCount> kTypes{{
    {"Idle", "IDLE"},
    {"Space Idle", "SPACE_IDLE"},
    {"Move", "MOVE"},
    {"Turn Left", "TURNL"},
    {"Turn Right", "TURNR"},
    {"Attack", "ATTACK"},
    {"Attack Idle", "ATTACKIDLE"},
    {"Die", "DIE"},
    {"Rotate", "ROTATE"},
    {"Special A", "SPECIAL_A"},
    {"Special B", "SPECIAL_B"},  // 10
    {"Special C", "SPECIAL_C"},
    {"Transition To Left Turn", "TURNL_BEGIN"},
    {"Transition From Left Turn", "TURNL_END"},
    {"Transition To Right Turn", "TURNR_BEGIN"},
    {"Transition From Right Turn", "TURNR_END"},
    {"Transition To Move", "MOVESTART"},
    {"Transition From Move 0", "MOVE_ENDONE"},
    {"Transition From Move 40", "MOVE_ENDTWO"},
    {"Transition From Move 80", "MOVE_ENDTHREE"},
    {"Transition From Move 120", "MOVE_ENDFOUR"},  // 20
    {"Turn Left Half", "TURNL_HALF"},
    {"Turn Left Quarter", "TURNL_QUARTER"},
    {"Turn Right Half", "TURNR_HALF"},
    {"Turn Right Quarter", "TURNR_QUARTER"},
    {"Deploy", "DEPLOY"},
    {"Undeploy", "UNDEPLOY"},
    {"Cinematic", "CINEMATIC"},
    {"Block_Blaster", "BLOCK_BLASTER"},
    {"Redirect_Blaster", "REDIRECT_BLASTER"},
    {"Idle_Blockblaster", "IDLE_BLOCKBLASTER"},  // 30
    {"FW_Attack", "FW_ATTACK"},
    {"FW_Die", "FW_DIE"},
    {"FTK_Attack", "FTK_ATTACK"},
    {"FTK_Hold", "FTK_HOLD"},
    {"FTK_Release", "FTK_RELEASE"},
    {"FTK_Die", "FTK_DIE"},
    {"FB_Attack", "FB_ATTACK"},
    {"FB_Hold", "FB_HOLD"},
    {"FB_Release", "FB_RELEASE"},
    {"FL_Attack", "FL_ATTACK"},  // 40
    {"FL_Die", "FL_DIE"},
    {"Force_Run", "FORCE_RUN"},
    {"Land", "LAND"},
    {"Takeoff", "TAKEOFF"},
    {"Flame_Attack", "FLAME_ATTACK"},
    {"Demolition", "DEMOLITION"},
    {"Bombtoss", "BOMBTOSS"},
    {"Jump", "JUMP"},
    {"FlyIdle", "FLYIDLE"},
    {"FlyLand", "FLYLAND"},  // 50
    {"FlyLandIdle", "FLYLANDIDLE"},
    {"FlyLandDrop", "FLYLANDDROP"},
    {"HC_WIN", "HC_WIN"},
    {"HC_LOSE", "HC_LOSE"},
    {"HC_DRAW", "HC_DRAW"},
    {"SHIELD_ON", "SHIELD_ON"},
    {"SHIELD_OFF", "SHIELD_OFF"},
    {"CA_Die", "CA_DIE"},
    {"Deployed_CA_Die", "DEPLOYED_CA_DIE"},
    {"Deployed_Die", "DEPLOYED_DIE"},  // 60
    {"FIRE_MOVE", "FIRE_MOVE"},
    {"Fire_Die", "FIRE_DIE"},
    {"Pound_Attack", "POUND_ATTACK"},
    {"Eat_Attack", "EAT_ATTACK"},
    {"Eaten_Die", "EATEN_DIE"},
    {"Move by walking", "WALKMOVE"},
    {"Move while crouched", "CROUCHMOVE"},
    {"Structure Open", "OPEN"},
    {"Structure Hold", "HOLD"},
    {"Structure Close", "CLOSE"},  // 70
    {"Idle Crouch", "CROUCHIDLE"},
    {"Turn Left Crouch", "CROUCHTURNL"},
    {"Turn Right Crouch", "CROUCHTURNR"},
    {"Build", "BUILD"},
    {"Transition_Ownership", "TRANS"},
    {"Self_Destruct", "SELF_DESTRUCT"},
    {"Attention", "ATTENTION"},
    {"Celebrate", "CELEBRATE"},
    {"Flinch_Left", "FLINCHL"},
    {"Flinch_Right", "FLINCHR"},  // 80
    {"Flinch_Front", "FLINCHF"},
    {"Flinch_Back", "FLINCHB"},
    {"Attack_Flinch_Left", "ATTACKFLINCHL"},
    {"Attack_Flinch_Right", "ATTACKFLINCHR"},
    {"Attack_Flinch_Front", "ATTACKFLINCHF"},
    {"Attack_Flinch_Back", "ATTACKFLINCHB"},
    {"Talk", "TALK"},
    {"Talk_Gesture", "TALKGESTURE"},
    {"Talk_Question", "TALKQUESTION"},
    {"", "HACKING"},  // 90: suffix only
    {"", "REPAIRING"},  // 91: suffix only
    {"Choke", "CHOKE"},
    {"Choke_Die", "CHOKEDEATH"},
    {"Deploy Troopers", "TROOPDROP"},
    {"Rope Slide", "ROPESLIDE"},
    {"Rope Land", "ROPELAND"},
    {"Drop Rope", "ROPE_DROP"},
    {"Lift Rope", "ROPE_LIFT"},
    {"Alarm", "ALARM"},
    {"Warning", "WARNING"},  // 100
    {"Crushed", "CRUSHED"},
    {"Power_Down", "POWERDOWN"},
    {"Power_Up", "POWERUP"},
    {"SpinMove", "SPINMOVE"},
    {"Force_Revel_Begin", "FORCE_REVEAL_BEGIN"},  // (sic) XML name
    {"Force_Revel_Loop", "FORCE_REVEAL_LOOP"},
    {"Force_Revel_End", "FORCE_REVEAL_END"},
    {"Saber_Throw", "SWORD_THROW"},
    {"Saber_Control", "SWORD_CONTROL"},
    {"Saber_Catch", "SWORD_CATCH"},  // 110
    {"SwordSpin", "SWORDSPIN"},
    {"Contaminate_Attack", "CONTAMINATE_ATTACK"},
    {"Contaminate_Loop", "CONTAIMINATE_LOOP"},  // (sic) the shipped file uses this spelling
    {"Contaminate_Release", "CONTAMINATE_RELEASE"},
    {"Deployed_Walk", "WALK"},
    {"Pad_Build", "PAD_BUILD"},
    {"Pad_Sell", "PAD_SELL"},
    {"Heal", "HEAL"},  // 118
}};

char upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (upper(a[i]) != upper(b[i])) return false;
    return true;
}

bool valid(int32_t id) { return id >= 0 && id < static_cast<int32_t>(kAnimTypeCount); }

}  // namespace

std::string_view anim_type_name(int32_t id) { return valid(id) ? kTypes[static_cast<size_t>(id)].name : std::string_view{}; }
std::string_view anim_type_suffix(int32_t id) { return valid(id) ? kTypes[static_cast<size_t>(id)].suffix : std::string_view{}; }

int32_t find_anim_type(std::string_view xml_name) {
    if (xml_name.empty()) return kNoAnimType;
    for (size_t i = 0; i < kTypes.size(); ++i)
        if (iequals(kTypes[i].name, xml_name)) return static_cast<int32_t>(i);
    return kNoAnimType;
}

bool is_attack_like(int32_t id) {
    switch (id) {
        case 5: case 25: case 26: case 31: case 33: case 34: case 35: case 37: case 38: case 39: case 40:
        case 45: case 46: case 47: case 63: case 64:
            return true;
        default:
            return false;
    }
}

bool is_idle_like(int32_t id) { return id == 0 || id == 1 || id == 71 || id == 77 || id == 78 || (id >= 79 && id <= 82); }
bool is_turn(int32_t id) { return id == 3 || id == 4 || (id >= 21 && id <= 24) || id == 72 || id == 73; }
bool is_crouch(int32_t id) { return id == 67 || (id >= 71 && id <= 73); }
bool is_flinch(int32_t id) { return id >= 79 && id <= 86; }
bool is_one_shot_pose(int32_t id) {
    switch (id) {
        case 7: case 9: case 10: case 11: case 32: case 36: case 41: case 58: case 62:
            return true;
        default:
            return false;
    }
}

}  // namespace alamo::anim
