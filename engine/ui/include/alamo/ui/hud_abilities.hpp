// P2: HudProviders::abilities fed from S9's ability state (engine/tactical/abilities AbilitySystem), read-only queries
// only (has_ability / is_active / can_activate / is_autofire / record / countdown_*): the HUD never changes the sim;
// clicks leave as commands 43 / 40 / 57 / 58 (tactical_hud.hpp, targeting.hpp). Built into alamo_ui_hud when the
// alamo_tactical_abilities target exists (ALAMO_UI_HAVE_ABILITIES).
//
// Per selected group (one type): every unit-ability type 1..75 that some member has, ascending:
//   recharge  = max over members of (recharging ? 1 - counter / maximum : 1) (Countdown phase 0 with frames left)
//   active    = some member has it active (Is_Unit_Ability_Active)
//   enabled   = some member passes Can_Activate without the recharge check (abilities-off, nebula, black market, ...)
//               or has it active (so it can be turned off)
//   autofire  = some member has autofire on; targeted / target_kind = ABILITIES_HARDPOINTS §2.9 (ability_target_kind)
//   icon      = the record's Alternate_Icon_Name (t42), else ability_icon(type): the i_sa_*.tga button art of MT_CommandBar.
//               Not st_ability_icon: that component's i_button_sa_* names are in no atlas, so special_button_NN showed only
//               its lavender blank (QA D5).
// The groups' types merge into one button per ability type in TacticalHud (users merged, best recharge).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "alamo/ui/hud_components.hpp"
#include "alamo/ui/tactical_hud.hpp"

namespace alamo::tactical::abilities {
class AbilitySystem;
}

namespace alamo::ui {

// Icon of an ability type (see above); empty for type 0.
std::string ability_icon(uint8_t ability_type);

#ifdef ALAMO_UI_HAVE_ABILITIES
// One group's abilities at sim frame `frame` (call between ticks: const reads of the sim, like the P1 picking contract).
void abilities_from_s9(const tactical::abilities::AbilitySystem& system, const SelectedGroup& group, uint32_t frame,
                       std::vector<HudAbility>& out);
// HudProviders::abilities over `system` at the frame `frame()` returns. The references must outlive the provider.
std::function<void(const SelectedGroup&, std::vector<HudAbility>&)> s9_ability_provider(
    const tactical::abilities::AbilitySystem& system, std::function<uint32_t()> frame);
#endif

}  // namespace alamo::ui
