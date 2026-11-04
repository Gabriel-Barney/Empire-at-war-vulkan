// refixf t08 (alamo_game): the host side of the tactical world pick 0x36600 (docs/re/HUD_GAPS.md §2.2; engine/ui
// picking.hpp pick_world). Presentation-side input: const reads of the top tactical mode and the battle between ticks
// (P1 picking contract); nothing here writes sim state or feeds the per-tick CRC. The orders built from a pick are sim
// input as before.
//   precise  the precise model test 0x38f900 (SPACE_AVOIDANCE §5) through movement::avoid_precise_segment on the object's
//            committed pose (TacticalMode::obstacle_of, the refixf t07 model cache, no resolve). [port] Query mask 1
//            (hull): HUD_GAPS does not name the pick's mask; mask 1 is the avoidance query's, so a shielded unit's
//            SHIELD bubble (mesh mask 2 / 0) is not clickable and an unshielded type's SHIELD mesh (mask 1) is. The
//            segment is the whole pick ray (eye -> far plane). A type without model data (no model source, model not
//            loaded): NotModelled, its tree box stands in [port].
//   sphere   Mouse_Collide_Override_Sphere_Radius (type +0x4e0) at the object position (obj +0x78) [M: "the object
//            centre"].
//   skip     garrisoned objects (TacticalBattle::contained_by; HUD_GAPS §2.2 "objects with flag +0x3a7 skipped"). Their
//            host is a tree member and is picked by its own model.
//   resolve  identity [port]: TEAM containers have no tree entry and the port selects squadron members one by one
//            (HUD_GAPS "a container child resolves to its container" is not applied), garrisoned children are skipped.
// [port] Fog / stealth are not filtered (as before t08): the candidates are the tree entries in the view frustum.
#pragma once
#ifdef ALAMO_GAME_HAVE_UI
#include <functional>
#include <span>
#include <vector>

#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_store.hpp"
#include "alamo/ui/picking.hpp"

namespace alamo::sim {
class TacticalMode;
class TacticalBattle;
}  // namespace alamo::sim

namespace alamo::game {

// Mouse_Collide_Override_Sphere_Radius per type (0 when unset / not positive / not finite).
std::vector<float> mouse_sphere_radii(const objects::GameObjectTypes& types);

// The pick tests over `mode` (and `battle` for the garrison skip; may be null). The callbacks keep the pointers and the
// span: valid while the mode, the battle and `radii` live and no tick runs.
// q6 t19 (OBJECT_SCRIPTS §10.3): `script_hidden` (optional, GameSession::script_hidden) - Lua-hidden objects are skipped
// too (the model scene queries 0x266d10 / 0x267680 skip a hidden model, point and box pick). With the pick skipping it,
// a hidden enemy never resolves as the object under the cursor, so it gets no attack cursor and no Attack order (the
// cursor resolver 0x2ad280 refuses it as well).
ui::WorldPickTests tactical_pick_tests(const sim::TacticalMode& mode, const sim::TacticalBattle* battle,
                                       std::span<const float> radii,
                                       std::function<bool(objects::ObjectId)> script_hidden = {});

}  // namespace alamo::game
#endif
