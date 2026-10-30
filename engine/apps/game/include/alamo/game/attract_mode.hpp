// OI host (alamo_game): the main-menu attract demo controller - the owner of the audio attract-mode flag (AUDIO.md §1.6:
// 3D voices silent, no battle mode music while the demo runs; the menu theme plays) and of the demo's idle pause.
//
// docs/re/MAIN_MENU_BACKGROUND.md §3 (the original controller 0x22bc0 / 0x22f10 / 0x22c50 / 0x22bf0) [H]:
//   * menu_opened(): every time the main menu opens with Main_Menu_Demo_Attract_Mode set - not running: arm and Start
//     the first map at once; running: the map-cycle check (0x235f0).
//   * the map index advances before every load (index = (index + 1) mod count; the first map shown is entry 1 mod count,
//     entry 0 for a one-map list - as coded).
//   * input never stops the demo (it runs under the main menu and every sub-dialog); input only refreshes the idle
//     clock. Demo_Attract_Start_Timeout_Seconds is the idle PAUSE threshold: idle longer than it -> the tactical pause
//     (idle_paused()), the next input resumes.
//   * map cycling while running and not idle-paused, once now - map start > Demo_Attract_Map_Cycle_Delay_Seconds[index]
//     (120 s when the list is shorter than the map list): with no dialog open at once; with the main menu open after
//     8 s idle, with another dialog open after 20 s idle (and the window active).
//   * stop(): a real game starts, credits, exit, the menu closes.
// Wall time only: never part of the sim (the demo session itself is an ordinary deterministic session).
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace alamo::data {
struct GameConstants;
}

namespace alamo::game {

struct AttractConfig {
    bool enabled = false;                // Main_Menu_Demo_Attract_Mode
    double start_timeout_s = 3600.0;     // Demo_Attract_Start_Timeout_Seconds
    std::vector<std::string> maps;       // Demo_Attract_Maps, ".ted" stripped
    std::vector<double> cycle_delay_s;   // Demo_Attract_Map_Cycle_Delay_Seconds (per map; 120 s past its end)
};
// Reads the four GameConstants fields (comma / whitespace lists). Missing fields keep the defaults above.
AttractConfig load_attract_config(const data::GameConstants& gc);

// EaW data (Demo_Attract_Maps = Front_end_demo_attract.rec, GameData\Recordings; Gabriel 2026-10-28 "EaW main-menu attract
// demo"): a .rec entry is NOT replayed - only its map is read and the same live AI-vs-AI demo runs on it. [port] header
// scan (no layout spec): the first NUL-terminated ASCII run in the first 8 KB that contains ".\Data\Art\Maps\" and ends in
// ".ted" (case-insensitive; the path starts at that prefix - the EaW file has printable header bytes right before it). Returns the map name without folder and extension ("Front_end_demo_attract"), ""
// when there is none.
std::string attract_rec_map(std::span<const uint8_t> bytes);
// True for a Demo_Attract_Maps entry naming a recording (".rec", case-insensitive).
bool is_attract_recording(const std::string& entry);

// --- the demo's camera (presentation only, wall time; never part of the sim) ---
// The original films the demo with the in-game cinematic ("interesting object") camera: a code-driven camera, not data (the
// demo map holds no camera keys, Demo_Attract_Controller.lua only reveals the map and sends the fleets to the story zones;
// the shot logic is untraced, MAIN_MENU_BACKGROUND.md §3.1 step 8). [M] stand-in (Playtest 3 t14, hulls seen nose-on):
// shots of `shot_seconds` on the next live combat unit; the camera keeps a SIDE view of the followed hull - its view
// direction stays within `swing_deg` of perpendicular to the hull's facing (turn rate limited), so a nose-on or tail-on
// view never lasts. Yaw convention of ui::TacticalCameraController: degrees about +Z, 0 = looking north (+Y), positive
// clockwise from above, horizontal view direction (sin yaw, cos yaw); facing = the hull's nose (snapshot facing).
struct DemoCameraTuning {
    double shot_seconds = 8.0;          // [M] kept from the first stand-in
    float pitch_deg = 35.0f;            // [M] below the tactical 50: more profile than plan view
    float swing_deg = 18.0f;            // [M] slow orbit amplitude around the side view
    float swing_period_s = 14.0f;       // [M]
    float max_turn_deg_per_s = 25.0f;   // [M] how fast the yaw follows a turning hull
};
float demo_wrap_deg(float deg);  // to (-180, 180]
// |cos| of the angle between the camera's horizontal view direction and the hull's facing (x, y): 1 = nose-on or tail-on,
// 0 = broadside. 0 for a vertical facing (no horizontal direction).
float demo_view_alignment(float yaw_deg, float facing_x, float facing_y);
// The camera yaw that looks along the hull's broadside (view direction perpendicular to the facing), the one of the two
// sides nearest `from_yaw_deg`. A vertical facing keeps `from_yaw_deg`.
float demo_side_yaw_deg(float facing_x, float facing_y, float from_yaw_deg);
// One frame of a shot: the yaw `dt` seconds after `current_yaw_deg`, heading for the side view of the facing plus the slow
// swing at shot time `shot_t`, at most max_turn_deg_per_s * dt. Pure.
float demo_shot_yaw_deg(float current_yaw_deg, float facing_x, float facing_y, float shot_t, float dt, const DemoCameraTuning& t = {});

class AttractController {
public:
    enum class Request : uint8_t { None, Start, Next };
    // Which dialogs are open this frame (the cycle's idle threshold).
    enum class Dialogs : uint8_t { None, MainMenu, Other };
    explicit AttractController(AttractConfig cfg = {}) : cfg_(std::move(cfg)) {}
    void set_config(AttractConfig cfg) { cfg_ = std::move(cfg); }
    const AttractConfig& config() const { return cfg_; }

    // The main menu opened (0xc58e0 step 3): Start when enabled (or `force`, the --attract option) and not running, else
    // the cycle check. Disabled / no maps: None.
    Request menu_opened(double now, bool force = false);
    // Once per frame while armed: `input` = any key / button / wheel / mouse motion this frame, `window_active` = the
    // window has the focus. Returns Next when the map cycles.
    Request update(double now, bool input, Dialogs dialogs, bool window_active = true);
    // Ends the demo (and disarms).
    void stop();
    bool running() const { return running_; }
    bool armed() const { return armed_; }
    // Idle longer than Start_Timeout: the demo's tactical pause (rendering continues).
    bool idle_paused() const { return paused_; }
    size_t map_index() const { return index_; }
    const std::string& map() const;
    double cycle_delay() const;  // of the current map (120 s past the end of the delay list)
    double map_started() const { return started_at_; }
    uint64_t starts() const { return starts_; }

private:
    Request start(double now);
    bool cycle_due(double now, Dialogs dialogs, bool window_active) const;
    AttractConfig cfg_;
    bool running_ = false, armed_ = false, paused_ = false;
    double last_input_ = 0.0, started_at_ = 0.0;
    size_t index_ = 0;
    uint64_t starts_ = 0;
};

}  // namespace alamo::game
