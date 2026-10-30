// P2 stage 2: world-anchored order feedback (docs/re/UI.md §1.5): waypoint lines, move acknowledgements and the
// space hard-point target reticles. All are 2D HUD quads projected through the gameplay View at a constant pixel
// size, like the brackets (hud_world.hpp), except the move acknowledgement's particle, which is requested from
// the host (render particles, R4) by name.
//
//   * Waypoint lines [port]: the original draws WaypointLineSubClass in the 3D scene; here each segment of a
//     unit's queued path is a row of 3 x 3 px dots every 9 px (layer 0), so it needs no rotated primitive.
//   * Move acknowledgement (GameConstants GUI_*_Command_Ack_Effect, GUI_Move_Acknowledge_Scale_Land 1.0 /
//     _Space 5.0, GUI_*_Click_Radar_Event_Name): MoveAcks::add returns the particle effect + scale for the host and
//     the radar event to ping; while it lives (0.6 s [port]) a shrinking ring of dots marks the point on screen.
//   * Hard-point reticles (GameConstants HardPoint_Target_Reticle_{Enemy,Friendly}[_Tracked|_Repairing|_Disabled
//     [_Tracked]]_Texture "HARD_POINT_<TYPE>, <texture>" pairs; *_Screen_Size 0.03): an atlas quad of the texture,
//     side = screen size x the screen HEIGHT in canvas units [port: "fraction of screen"], layer 1; textures not in
//     the MT_CommandBar atlas draw a [port] four-corner outline in the reticle's colour.
#pragma once
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/ui/hud_canvas.hpp"
#include "alamo/ui/hud_components.hpp"
#include "alamo/ui/hud_draw.hpp"
#include "alamo/ui/hud_world.hpp"
#include "alamo/ui/picking.hpp"

namespace alamo::ui {

struct HudWaypointLine {
    std::vector<Vec3> points;  // unit position first, then the queued waypoints
    Rgba8 color{120, 255, 120, 200};
};
// Returns the number of dots drawn.
uint32_t add_waypoint_lines(HudDrawList& dl, const HudCanvas& canvas, const View& view, std::span<const HudWaypointLine> lines,
                            uint32_t layer = 0);

// [port: Gabriel] queued orders (docs/PHASE4.md "Camera and controls": shift-queued orders). The host fills one
// HudOrderQueue per unit from the sim's read-only queued_orders(unit) query (the current order first, then the queued
// ones); the HUD draws a dotted line from the unit through every step (colour per kind: move green, attack red,
// ability blue, other grey) and a numbered marker (1, 2, ...) on each step point - a 7 x 7 px square plus the number
// (EmpireAtWar-Medium 7) above-right of it. Steps without a point (e.g. a self ability) only number the chain.
struct HudQueuedStep {
    enum class Kind : uint8_t { Move, Attack, Ability, Other };
    Kind kind = Kind::Move;
    Vec3 point{};
    bool has_point = true;
};
struct HudOrderQueue {
    uint32_t unit = 0;
    Vec3 position{};  // the unit's position (chain start)
    std::vector<HudQueuedStep> steps;
};
struct QueueMarkerStats {
    uint32_t dots = 0, markers = 0;
};
Rgba8 queue_step_color(HudQueuedStep::Kind k);
QueueMarkerStats add_queue_markers(HudDrawList& dl, FontCache* fonts, const HudCanvas& canvas, const View& view,
                                   std::span<const HudOrderQueue> queues, uint32_t layer = 1);

enum class MoveAckKind : uint8_t { Move = 0, DoubleClick, AttackMove, Guard };
struct MoveAckConfig {
    std::string effect[4] = {"GUI_Move_Command_Particle", "GUI_Double_Click_Move_Command_Particle",
                             "GUI_Attack_Move_Command_Particle", "GUI_Guard_Move_Command_Particle"};
    std::string radar_event[4] = {"Default_Click", "Default_Double_Click", "Default_Attack_Click", "Default_Click"};
    float scale_land = 1.0f, scale_space = 5.0f;
    float lifetime = 0.6f;  // [port] screen ring
    void parse(const data::XmlNode& game_constants_root);
};
struct MoveAckRequest {  // what the host spawns
    std::string effect;     // particle effect name
    float scale = 1.0f;
    Vec3 position{};
    std::string radar_event;
};
class MoveAcks {
public:
    MoveAckRequest add(const MoveAckConfig& cfg, MoveAckKind kind, Vec3 position, bool land);
    void update(float dt);
    // Shrinking dot ring at each live acknowledgement; returns dots drawn.
    uint32_t draw(HudDrawList& dl, const HudCanvas& canvas, const View& view, uint32_t layer = 0) const;
    size_t size() const { return items_.size(); }

private:
    struct Item {
        Vec3 position;
        MoveAckKind kind;
        float age = 0, lifetime = 0.6f;
    };
    std::vector<Item> items_;
};

// Hard-point reticles (docs/re/HUD_GAPS.md §1): built for the HOVERED ship only (never for the selection), one per
// targetable hard point that is not destroyed (the host skips destroyed / enemy-disabled ones, §1.2). Size: width =
// *_Screen_Size x screen width, height = width x 4/3 as a screen fraction (= size x 4/3 x canvas height), half size on
// the "on" frames of a flash (60 frames from an attack / targeted ability on that hard point, phase toggling every 3
// frames at 30 fps, §1.3). Tint by health fraction (§1.4): disabled grey 128, f >= 0.66 green 32,255,32, >= 0.33 yellow
// 255,255,32, else red 255,32,32. Texture variant (§1.5) = the host's flags: `tracked` = the Tracked list (enemy
// hovered / flashing while the selection can hit hard points; friendly hovered), `repairing` = Friendly_Repairing (the
// host blinks it at 1 Hz), `disabled` = Friendly_Disabled[_Tracked].
struct HudHardPoint {
    Vec3 position{};
    std::string type;  // HardPointType name, e.g. "HARD_POINT_WEAPON_LASER"
    bool enemy = true, tracked = false, repairing = false, disabled = false;
    // Owning ship (object id). 0 = always drawn (callers that pre-filter); non-zero: TacticalHud draws the point only
    // while that ship is hovered (HudUnit::hovered, filter_hardpoints).
    uint32_t parent = 0;
    float health = 1.0f;         // hard point health / max (tint)
    double flash_start = -1.0;   // wall seconds of the attack / ability order on this hard point (< 0: none)
};
// The points TacticalHud draws: parent 0, or a parent that is hovered in `units`.
std::vector<HudHardPoint> filter_hardpoints(std::span<const HudHardPoint> points, std::span<const HudUnit> units);
Rgba8 reticle_tint(const HudHardPoint& hp);
bool reticle_flash_on(const HudHardPoint& hp, double now);
class ReticleTable {
public:
    // GameConstants root: every HardPoint_Target_Reticle_*_Texture pair and the two screen sizes.
    void parse(const data::XmlNode& game_constants_root);
    void set(std::string_view variant_tag, std::string_view hard_point_type, std::string texture);  // tests
    // Texture for a hard point's state (enemy: Enemy[_Tracked]; friendly: Friendly + _Repairing / _Disabled[_Tracked]
    // / _Tracked); "" when unknown.
    std::string texture(const HudHardPoint& hp) const;
    float enemy_screen_size = 0.03f, friendly_screen_size = 0.03f;
    size_t size() const { return map_.size(); }

private:
    std::map<std::string, std::string> map_;  // "VARIANT|TYPE" (upper case) -> texture
};
// Returns the number of reticles drawn. The vanilla I_Hard_Point_Reticle_* textures are stand-alone DDS files (EaW
// Textures.meg), not MT_CommandBar atlas entries: `images` (the host's image-page resolver, the GUI's GuiImageResolver)
// is tried first for a texture missing from the atlas ("<name>.tga" as in the atlas convention, then the bare name);
// only when both fail does the [port] corner outline stand in.
uint32_t add_hardpoint_reticles(HudDrawList& dl, const TextureAtlas& atlas, const HudCanvas& canvas, const View& view,
                                const ReticleTable& table, std::span<const HudHardPoint> points, uint32_t layer = 1,
                                const GuiImageResolver* images = nullptr, double now = 0.0);
// Reticle size in canvas units (screen-size rule + flash).
Vec2f reticle_size(const HudHardPoint& hp, const ReticleTable& table, const HudCanvas& canvas, double now);
// HUD_GAPS §1.6 hit test: index of the first point (list order) whose reticle rectangle contains the cursor pixel, -1
// none. The host uses it to keep the hovered ship = the reticle owner (no flicker) and to attach the hard point to an
// attack / targeted-ability / RepairHardpoint click.
int32_t reticle_at(const HudCanvas& canvas, const View& view, const ReticleTable& table, std::span<const HudHardPoint> points,
                   float px, float py, double now = 0.0);

}  // namespace alamo::ui
