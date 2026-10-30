#include "alamo/game/game_controls.hpp"

#ifdef ALAMO_GAME_HAVE_CONTROLS
#include <cstdint>

namespace alamo::game {

void sync_control_bindings(const ui::HotkeyTable& hotkeys, const ui::CameraSettings& camera, ui::Bindings& bindings) {
    hotkeys.apply_to_bindings(bindings);
    bindings.set_queue_mods(camera.queue_mods);
}

std::vector<std::string> check_control_decisions(const ui::HotkeyTable& hotkeys, const ui::Bindings& bindings) {
    std::vector<std::string> out;
    const auto slots = ui::hotkey_slots();
    auto wasd = [](int32_t key) { return key == 'W' || key == 'A' || key == 'S' || key == 'D'; };
    // W A S D: camera movement only, with any modifier (hotkey slots and Bindings actions alike).
    const uint16_t movement[] = {ui::hk::kScrollUp, ui::hk::kScrollLeft, ui::hk::kScrollDown, ui::hk::kScrollRight};
    const char movement_key[] = {'W', 'A', 'S', 'D'};
    for (uint16_t s = 0; s < ui::kHotkeySlots; ++s) {
        bool is_movement = false;
        for (const uint16_t m : movement) is_movement = is_movement || m == s;
        const ui::HotkeyBinding& b = hotkeys.binding(s);
        if (!is_movement && wasd(b.key))
            out.push_back(std::string("hotkey ") + slots[s].name + " is on W/A/S/D (W A S D move the camera only)");
    }
    for (size_t i = 0; i < 4; ++i) {
        const ui::HotkeyBinding& b = hotkeys.binding(movement[i]);
        if (b.key != movement_key[i] || b.mod != ui::HotkeyMod::None)
            out.push_back(std::string("hotkey ") + slots[movement[i]].name + " is not " + movement_key[i]);
    }
    const ui::Action scroll[] = {ui::Action::ScrollUp, ui::Action::ScrollLeft, ui::Action::ScrollDown, ui::Action::ScrollRight};
    for (size_t a = 0; a < ui::kActionCount; ++a) {
        const auto act = static_cast<ui::Action>(a);
        bool is_scroll = false;
        for (const ui::Action m : scroll) is_scroll = is_scroll || m == act;
        for (size_t k = 0; k < ui::Bindings::kSlots; ++k) {
            const ui::KeyBinding& kb = bindings.binding(act, k);
            if (kb.key != 0 && !is_scroll && wasd(kb.key))
                out.push_back(std::string("binding ") + std::string(ui::action_name(act)) + " is on W/A/S/D");
        }
    }
    for (size_t i = 0; i < 4; ++i) {
        bool found = false;
        for (size_t k = 0; k < ui::Bindings::kSlots; ++k) {
            const ui::KeyBinding& kb = bindings.binding(scroll[i], k);
            found = found || (kb.key == static_cast<uint16_t>(movement_key[i]) && (kb.mods & ui::kModAny) != 0);
        }
        if (!found) out.push_back(std::string("binding ") + std::string(ui::action_name(scroll[i])) + " does not move on " + movement_key[i] + " with any modifier");
    }
    // Pause: a hotkey slot (rebindable) and the Bindings action.
    if (hotkeys.binding(ui::hk::kPause).key < 0) out.push_back("hotkey Pause is unbound");
    if (bindings.binding(ui::Action::Pause, 0).key == 0 && bindings.binding(ui::Action::Pause, 1).key == 0)
        out.push_back("binding Pause is unbound");
    // Free 360-degree tactical camera: yaw / pitch / roll keys.
    const ui::Action axes[] = {ui::Action::RotateLeft, ui::Action::RotateRight, ui::Action::PitchUp, ui::Action::PitchDown,
                               ui::Action::RollLeft,   ui::Action::RollRight};
    for (const ui::Action a : axes)
        if (bindings.binding(a, 0).key == 0 && bindings.binding(a, 1).key == 0)
            out.push_back(std::string("binding ") + std::string(ui::action_name(a)) + " is unbound");
    // Keyboard zoom (engine/ui 2026-10-28 defaults: + / -): bound, and off End / Home (camera reset / camera home).
    for (const ui::Action a : {ui::Action::ZoomIn, ui::Action::ZoomOut}) {
        bool bound = false;
        for (size_t k = 0; k < ui::Bindings::kSlots; ++k) {
            const ui::KeyBinding& kb = bindings.binding(a, k);
            bound = bound || kb.key != 0;
            if (kb.key == ui::vk::kEnd || kb.key == ui::vk::kHome)
                out.push_back(std::string("binding ") + std::string(ui::action_name(a)) + " is on End/Home (camera reset / home keys)");
        }
        if (!bound) out.push_back(std::string("binding ") + std::string(ui::action_name(a)) + " is unbound");
    }
    if (bindings.binding(ui::Action::ResetCamera, 0).key == 0 && bindings.binding(ui::Action::ResetCamera, 1).key == 0)
        out.push_back("binding ResetCamera is unbound");
    // Shift+click (by default) queues orders.
    if (bindings.queue_mods() == 0) out.push_back("no queue modifier (queued orders impossible)");
    return out;
}

}  // namespace alamo::game
#endif
