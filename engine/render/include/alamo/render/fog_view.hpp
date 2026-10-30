// alamo_render - R5 fog of war, renderer side (docs/re/FOG_OF_WAR.md §7 "renderer contract", DECORATIONS.md §2.3-§2.7).
//
// Input: one player's FOW grid as the sim keeps it (engine/tactical/fog FogRenderView: per cell a state byte and a fade
// byte, the grid box / size, the mode frame F, the grid's last-decay frame and its regrow step, the FOW-active / render
// option / setup-phase flags). FogFrameInput is a plain COPY of that view, owned by the immutable sim snapshot
// (SimSnapshot::fog, filled between ticks by apply_battle_fog), so the render thread never reads sim memory and never
// writes the grid.
//
// Per frame the renderer converts it (DECORATIONS.md §2.3):
//   1. state -> intensity per cell: f = clamp(F - last_decay - 1, 0, 15) sim frames since the grid's last decay
//      (FOG_OF_WAR §7), r = regrow_step / 16 (integer [M]); v < 239: v' = max(0, v - f r) (fog regrowing between
//      decays); else v' = min(255, v - fade + f) (fading in); intensity = table[v'] (terrain::fow_state_to_intensity);
//   2. border cells fogged, 1-2-1 x 1-2-1 blur, palette (terrain::FowGrid::build_texels);
//   3. palette: land = (white, LandFOWColor), land setup phase = (white, SetupPhaseFOWColor), space = (transparent,
//      SpaceFOWColor) (fow_palette);
//   4. upload when the texels changed (one W x H RGBA8 texture, WorldRenderer::set_fog_of_war) and enable the world's
//      FOW passes (terrain TerrainFogOfWar / FOW_TEXTURE users on land, the SpaceFogOfWar plane in space) only while
//      FOW-active and the render option are both on (FOG_OF_WAR §6.5 Disable_Rendering, ENABLE_FOW reward).
// The grid geometry is the sim's (box, W, H), so texels line up with sim cells exactly (row 0 = max-y edge, the V flip of
// FOW_TEX_V). Everything here is integer work in index order: identical texels on every machine and thread count.
// [port] The original uploads only the camera-visible rectangle (+2 cells); this port re-converts the whole grid
// (<= 512 x 512) and uploads only when the result differs from the last upload.
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "alamo/terrain/fow.hpp"

namespace alamo::render {

class WorldRenderer;

// Copy of tactical::fog::FogRenderView for one (local) player.
struct FogFrameInput {
    bool valid = false;
    int32_t player = -1;
    uint32_t width = 0, height = 0;          // cells (= texels)
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;    // grid box (x0 left, y0 bottom, x1 right, y1 top)
    float cell_w = 0, cell_h = 0;            // actual cell size (extent / count)
    std::vector<uint8_t> state, fade;        // W x H, row 0 = the max-y edge
    uint32_t frame = 0;                      // mode frame F of the view
    uint32_t last_decay = 0;                 // grid's last-decay frame (0 = never)
    int32_t regrow_step = -1;                // grid +0x4c (-1 until the first decay)
    bool fow_active = true, render_option = true, setup_phase = false, reveal_all = false;
    // R5 space reinforcement overlay (TACTICAL_GAPS §5.5, SpaceReinforceFOWSampler 0x4d92d0): per cell 0 inside a
    // non-allied Reinforcement_Prevention_Radius, else 0xFF (TacticalBattle::fog_reinforce_overlay; same grid, row 0 =
    // max-y). Empty = not sampled. Used instead of the player's own grid while the overlay is on.
    std::vector<uint8_t> reinforce;

    size_t cells() const { return static_cast<size_t>(width) * height; }
    // Well-formed: valid flag, non-empty grid, both arrays W x H, a finite non-empty box.
    bool usable() const;
    // The fog is drawn: usable() && FOW-active && render option.
    bool drawn() const { return usable() && fow_active && render_option; }
};

// f = clamp(F - last_decay - 1, 0, 15) (FOG_OF_WAR §7, wrapping-safe: F < last_decay + 1 -> 0).
uint32_t fog_frames_since_decay(uint32_t frame, uint32_t last_decay);
// r = regrow_step / 16 (integer [M]); 0 for a negative step (no decay yet).
uint32_t fog_regrow_per_frame(int32_t regrow_step);
// Step 1 for every cell, index order. False (out emptied) when !in.usable().
bool fog_intensity(const FogFrameInput& in, std::vector<uint8_t>& out);
// The terrain-side grid description of the input (box, W, H, cell size taken as given).
terrain::FowGridDesc fog_grid_desc(const FogFrameInput& in);
// FOW_Point_Test on the copy (FOG_OF_WAR §1.2 / §5.1 without the AI gate): visible iff the point lies inside the box and
// its cell's state is not 0 (cell of the 0.001-inset clamped point, index clamped). An unusable input or the fog not
// active (FOW-active off) -> visible. Presentation only (ghost removal), never fed back to the sim.
bool fog_point_visible(const FogFrameInput& in, float x, float y);

// GameConstants colours ("c1,c2,c3,c4" packed with terrain::pack_fow_color). Defaults = the shipped values.
struct FogColors {
    uint32_t land_fog = terrain::kLandFowColor;              // LandFOWColor
    uint32_t setup_fog = terrain::kSetupPhaseFowColor;       // SetupPhaseFOWColor
    uint32_t space_fog = terrain::kSpaceFowColor;            // SpaceFOWColor
    uint32_t space_reinforce_fog = terrain::kSpaceReinforceFowColor;  // SpaceReinforceFOWColor (palette 3: reinforcement overlay)
};
// Reads the four colours from GameConstants.xml text (missing / malformed tags keep `out`'s values). False without a root.
bool parse_fog_colors(std::span<const uint8_t> game_constants_xml, FogColors& out);
// The palette the renderer uses for a frame: space -> (kSpaceFowVisible, space_fog), or with the reinforcement overlay
// palette 3 (kSpaceFowVisible, space_reinforce_fog); land setup phase -> (kLandFowVisible, setup_fog); land ->
// (kLandFowVisible, land_fog).
std::array<uint32_t, 256> fog_palette_for(const FogColors& c, bool space, bool setup_phase, bool reinforce = false);

struct FogRendererStats {
    uint64_t updates = 0, uploads = 0, skipped_same = 0, disabled = 0, invalid = 0;
    uint32_t width = 0, height = 0;
    uint32_t fogged_cells = 0, clear_cells = 0;  // final per-cell values (after border / blur): 0 / 255
    bool reinforce_sampled = false;              // the last build used FogFrameInput::reinforce
};

// Drives a WorldRenderer's fog of war from FogFrameInputs (frame thread, outside Renderer builds).
class FogRenderer {
public:
    void set_colors(const FogColors& c) { colors_ = c; }
    const FogColors& colors() const { return colors_; }
    void set_blur(bool on) { blur_ = on; }  // DECORATIONS §2.3 step 3 (renderer +0x14, default on)
    // Space reinforcement overlay (DECORATIONS §2.1 / §2.3 palette 3, W_Space_Reinforce_FOW_Grid.tga): while on, space
    // frames use palette 3 and the reinforcement grid texture (WorldRenderer::set_space_fow_reinforce). [L] when the
    // original shows it: while the local player places space reinforcements (the HUD's reinforcement mode). The cells are
    // the space reinforcement sampler's (FogFrameInput::reinforce, TACTICAL_GAPS §5.5: 0 / 0xFF, no blur change), or the
    // player's own grid when the input carries none.
    void set_space_reinforce(bool on) { reinforce_ = on; }
    bool space_reinforce() const { return reinforce_; }
    // Converts `in` and hands the result to `world` (uploads only on change). `space` = the world is a space map.
    // An unusable input disables the world's fog. Returns true when an upload was issued this call.
    bool update(WorldRenderer& world, const FogFrameInput& in, bool space);
    // CPU part of update() alone (tests / tools): builds grid() from `in`; false when !in.usable().
    bool build(const FogFrameInput& in, bool space);
    const terrain::FowGrid& grid() const { return grid_; }
    const std::vector<uint8_t>& processed() const { return processed_; }  // per cell after border / blur (0..255)
    // Final per-cell value at a world point of the last build (0 = fogged .. 255 = clear); 255 outside the grid or
    // before any build (render-side presentation only, never fed back to the sim).
    uint8_t value_at(float x, float y) const;
    const FogRendererStats& stats() const { return stats_; }
    bool active() const { return active_; }  // the world's fog was enabled by the last update()

private:
    FogColors colors_;
    bool blur_ = true, active_ = false, reinforce_ = false;
    terrain::FowGrid grid_;
    std::vector<uint8_t> intensity_, processed_, uploaded_;
    uint32_t uploaded_w_ = 0, uploaded_h_ = 0;
    bool have_upload_ = false;
    FogRendererStats stats_;
};

}  // namespace alamo::render
