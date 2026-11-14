// P2 stage 2: the original 153-slot hotkey table (docs/re/UI.md §4.3 [H] table, [M] slot -> label mapping) and
// its dispatch into commands (COMMANDS.md §2.4 ControlGroup / SelectAll / StopMovement / Retreat) and host
// intents (targeting modes, camera, interface).
//
// Profile keys (UI.md §4.3): KeySettingsCount (<= 153), KeySettingKeycode<n> (Win32 VK, -1 = unbound),
// KeySettingModifier<n> (0 none, 1 Shift, 2 Ctrl, 3 Alt). Binding a key removes it from every other slot; slots
// 0..9 drive their three derived slots (assign 123+n = Ctrl, add 133+n = Shift, third 143+n = Alt [M]).
// Matching (P1_CAMERA_INPUT.md §6, `0x2127f0`): an unmodified slot fires only with Shift, Ctrl and Alt all up; a Shift /
// Ctrl / Alt slot fires when that modifier is held (others ignored); per key the first matching slot in index order wins
// (Ctrl+Shift+1 = assign group 1, slot 123 before 133). So H (stop), Shift+H (hunt), Ctrl+H and Alt+H coexist.
//
// Control groups (UI.md §4.4): recall n = ControlGroup{n, assign 0}; Ctrl+n = ControlGroup{n, assign 1, add 0};
// Shift+n = ControlGroup{n, assign 1, add 1} (the wire's add bit exists only with assign). Group number = the
// digit ([port]: slot 0 = key '1' = group 1 ... slot 9 = key '0' = group 0, the number st_control_group draws).
// A second recall of the same group within 1 s also asks the host to jump the camera (local only).
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/commands/command.hpp"
#include "alamo/ui/camera.hpp"
#include "alamo/ui/input.hpp"
#include "alamo/ui/orders.hpp"

namespace alamo::ui {

// The original table has 153 slots (profile compatible); [port: Gabriel] slots 153..161 follow (Pause, camera yaw /
// pitch / roll, keyboard zoom in / out on = / -). Defaults moved for W A S D movement [port: Gabriel]: Attack A -> X, Stop S -> H, PreviousUnit D -> V,
// Waypoint W -> L; Scroll Up / Down / Left / Right arrows -> W / S / A / D (the arrows now yaw / pitch the camera).
// W A S D are for camera movement only, with any modifier: the modified defaults moved too - Ctrl+A select all -> Ctrl+Y,
// Ctrl+W super weapon -> Ctrl+;, Ctrl+D deploy -> Ctrl+,, Ctrl+S stim pack -> Ctrl+., Shift+W spoiler lock -> Shift+Q,
// Shift+D destroy planet -> Shift+R, Shift+A super laser -> Shift+U, Shift+S space self destruct -> Shift+[, Alt+W replenish
// wingmen -> Alt+', Alt+S force sight -> Alt+=, Alt+D drain life -> Alt+\, Alt+A FoC full salvo -> Alt+. An original-format
// profile is migrated on load (HotkeyTable::load).
inline constexpr uint16_t kOriginalHotkeySlots = 153;
inline constexpr uint16_t kHotkeySlots = 166;

enum class HotkeyMod : uint8_t { None = 0, Shift = 1, Ctrl = 2, Alt = 3 };  // profile encoding

struct HotkeyBinding {
    int32_t key = -1;  // Win32 VK, -1 = unbound
    HotkeyMod mod = HotkeyMod::None;
    friend bool operator==(const HotkeyBinding&, const HotkeyBinding&) = default;
};

enum class HotkeyKind : uint8_t {
    SelectGroup,   // 0..9
    AssignGroup,   // 123..132
    AddToGroup,    // 133..142
    GroupThird,    // 143..152 [M: camera jump to the group]
    Order,         // 10..23, 66: selection / unit orders
    Ability,       // 24..65, 90..122
    Camera,        // 69..79
    Interface,     // 67, 68, 80..83, 87..89
    Taunt,         // 84..86
};

struct HotkeySlotInfo {
    const char* name;  // port identifier ("SelectControlGroup1", "Stop", "Ability_JetPack", ...)
    HotkeyKind kind;
    HotkeyBinding def;
};
// The default table (index = slot).
std::span<const HotkeySlotInfo, kHotkeySlots> hotkey_slots();
// Slot by name (case-sensitive), -1 if none.
int32_t hotkey_slot(std::string_view name);
// Named slots used by the dispatcher.
namespace hk {
inline constexpr uint16_t kSelectAll = 10, kSelectLike = 11, kNextUnit = 12, kPrevUnit = 13, kGuard = 14, kAttack = 15,
                          kStop = 16, kMove = 17, kWaypoint = 18, kRetreat = 19, kReinforce = 20, kBeacon = 21,
                          kSuperWeapon = 22, kAttackMove = 23, kDeploySquad = 66, kDroidAdvisor = 67, kChat = 68,
                          kScrollUp = 69, kScrollDown = 70, kScrollLeft = 71, kScrollRight = 72, kCameraReset = 73,
                          kCameraCentre = 74, kCameraHome = 75, kTacticalOverview = 76, kCameraTether = 77,
                          kMovieMode = 78, kCinematicView = 79, kScreenshot = 80, kQuickSave = 81, kQuickLoad = 82,
                          kHolocron = 83, kGiveCredits = 87, kToggleInterface = 88, kTogglePlayerList = 89,
                          kAssignBase = 123, kAddBase = 133, kThirdBase = 143,
                          // [port: Gabriel]
                          kPause = 153, kYawLeft = 154, kYawRight = 155, kPitchUp = 156, kPitchDown = 157, kRollLeft = 158,
                          kRollRight = 159, kZoomIn = 160, kZoomOut = 161,
                          kFleetCohesion = 162,  // [port: Gabriel 2026-11-13] fleet cohesion toggle (O)
                          // [port] fleet groups (docs/DECISIONS.md "Fleet groups: exception to the current focus")
                          kFleetCreate = 163, kFleetUngroup = 164, kFleetLock = 165;
}  // namespace hk

// Profile values (name -> integer), e.g. a registry / ini abstraction owned by the host.
using HotkeyProfile = std::map<std::string, int32_t>;

class HotkeyTable {
public:
    HotkeyTable() { reset_defaults(); }
    void reset_defaults();
    const HotkeyBinding& binding(uint16_t slot) const { return slots_[slot < kHotkeySlots ? slot : 0]; }
    // Binds `slot` (removing the same key + modifier from every other slot); slots 0..9 also rebind their derived
    // slots. key -1 unbinds.
    void bind(uint16_t slot, HotkeyBinding b);
    // UI5 t05: the other slots bind(slot, b) would unbind (the same key + modifier, also for the derived slots of 0..9),
    // ascending; empty for key -1.
    std::vector<uint16_t> conflicts(uint16_t slot, HotkeyBinding b) const;
    // Profile I/O: save writes all 153 slots; load reads min(KeySettingsCount, 153) slots (missing entries keep
    // the current binding, out-of-range modifiers -> none). False if KeySettingsCount is absent.
    void save(HotkeyProfile& out) const;
    bool load(const HotkeyProfile& in);
    // Slots whose binding fired this frame (ascending).
    void pressed(const InputState& in, std::vector<uint16_t>& out) const;
    bool pressed(const InputState& in, uint16_t slot) const;
    // P1 Bindings sync: scroll (69..72) and camera reset (73) become held/pressed actions with kModAny; SelectAll
    // (10), Stop (16), ControlGroup0..9 (slots 9, 0..8), the free camera axes, Pause and zoom (160 / 161) with their
    // exact modifier. Only the camera, pause and zoom actions have a reader; SelectAll / Stop / ControlGroupN stay in the
    // Bindings table (key-config UI, clash checks) but this dispatcher is what issues those orders. Every camera action then has exactly one key, and no key drives two of them (End = reset only,
    // Home = CameraHome only).
    void apply_to_bindings(Bindings& b) const;

private:
    std::array<HotkeyBinding, kHotkeySlots> slots_{};
};

// A fired slot the HUD / host acts on (targeting modes, camera, menus) - everything that is not a sim command.
struct HotkeyEvent {
    uint16_t slot = 0;
    HotkeyKind kind = HotkeyKind::Order;
    int8_t group = -1;        // control-group slots: 0..9
    bool camera_jump = false; // second recall of `group` within 1 s
};

class HotkeyDispatcher {
public:
    // Pressed slots -> commands (control groups, select all, stop, retreat) + events (every fired slot that is
    // not a pure command, plus camera-jump requests). `selection` = synced local selection (Stop needs one).
    void update(const InputState& in, const HotkeyTable& table, std::span<const uint32_t> selection, double now,
                std::vector<commands::EventData>& commands, std::vector<HotkeyEvent>& events);
    static constexpr double kDoubleRecallSeconds = 1.0;  // UI.md §4.4
    // [port: Gabriel] the Pause slot emits pause_event(!paused) (orders.hpp) and flips this; the host keeps it in sync
    // with the sim (e.g. after a HUD pause button).
    void set_paused(bool p) { paused_ = p; }
    bool paused() const { return paused_; }
    // [port] fleet groups: while `on`, FleetGroupCreate / Ungroup / Lock send fleet group commands (commands/fleet_group.hpp)
    // for a non-empty selection; Lock sends SetLock(!selection_locked). Off (the default): those slots do nothing. The host
    // sets it every frame (`selection_locked` = every fleet group of the selection is locked).
    void set_fleet_groups(bool on, bool selection_locked) {
        fleet_on_ = on;
        fleet_locked_ = selection_locked;
    }

private:
    bool paused_ = false;
    bool fleet_on_ = false, fleet_locked_ = false;
    int8_t last_group_ = -1;
    double last_recall_ = -1e30;
    std::vector<uint16_t> fired_;
};

// ui3 t02: the tactical ability (ABILITIES_HARDPOINTS 2.1 type name, e.g. "SPREAD_OUT") an ability slot (24..65, 90..122)
// or DeploySquad (66) fires; "" for other slots and for the two ability slots no FoC type matches (SystemControl,
// DestroyPlanet). [port] matched by meaning: the keyboard dialog's edit-box names (UI.md §4.3) against the ability types the
// FoC data uses; the original's slot -> ability lookup in the hotkey dispatch (0x63f00) is not traced. Slots of one
// meaning share a type (SelfDestruct / SpaceSelfDestruct, LaserDefense / SpaceLaserDefense, FullSalvo / FoCFullSalvo,
// SensorJamming / FoCSensorJamming, MinorDrainLife / DrainLife).
std::string_view hotkey_ability_name(uint16_t slot);

// Group number (0..9) of a group slot (0..9, 123..152), -1 otherwise.
int8_t hotkey_group(uint16_t slot);

// P1: camera hotkeys onto the tactical camera (local presentation only). Host callbacks give world points:
//   camera_jump (double recall / Alt+n)  -> jump_to(group(n))        (UI.md §4.4)
//   kTacticalOverview (Insert)           -> toggle_overview()
//   kCameraReset (End)                   -> reset()
//   kCameraCentre (PgUp)                 -> jump_to(selection())     [port: centre on the selection]
//   kCameraHome (Home)                   -> jump_to(home())          [port: the local player's start / base]
// Returns the slots handled (ascending event order); the rest stay for the host.
struct CameraFocus {
    std::function<std::optional<Vec3>(int8_t group)> group;
    std::function<std::optional<Vec3>()> selection;
    std::function<std::optional<Vec3>()> home;
};
std::vector<uint16_t> apply_camera_hotkeys(std::span<const HotkeyEvent> events, TacticalCameraController& camera,
                                           const CameraFocus& focus);

}  // namespace alamo::ui
