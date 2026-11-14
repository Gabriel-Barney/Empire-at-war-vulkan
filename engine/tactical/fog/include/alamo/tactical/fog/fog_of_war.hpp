// Fog of war: the per-mode system (docs/re/FOG_OF_WAR.md). One FogOfWar per tactical (land / space) battle.
//
//   * start(): one FogGrid per player with `grid` set (the host skips the first player of a neutral faction),
//     all with the mode's geometry (Desired{Land,Space}FOWCellSize 15 / 100); saved Reveal_All re-applied.
//     Land sets the FOW-active flag and the render option to 1; space keeps the values passed in (the
//     original's global flag survives from the previous battle, FOG_OF_WAR §8 item 2).
//   * set_dense_obstacles(): the shared dense mask (nebula / asteroid / ion storm SPACE_OBSTACLE circles).
//   * decay_step(F) = common tick step 4: for each player (list order) whose id = F (mod 16), Decay; skipped
//     while the land setup phase is active. Refog callback for every nonzero -> 0 transition, cell order.
//   * Reveal sources (REVEAL behaviour 0x18, §3.2): source_service() once per frame per object in the
//     object-service step; allies share vision; re-reveal only when dirty or moved >= one average cell
//     (squared 2D test). source_change_owner(), source_remove() (death / leaving the map), source_fired()
//     (REVEAL vf26: every shot flashes the shooter's footprint on the target owner's grid, TACTICAL_GAPS §5.1).
//   * Player FOW reveal-range list (+0x1f8, TACTICAL_GAPS §5.2): Battlefield_Modifier entries (multiplier - 1)
//     reduced MostExtreme within a stacking category, summed over categories; sight x (1 + sum).
//   * Battle scale (TACTICAL_GAPS §5.3): land scales the defender (+8) by the attacker's Modifies_Reveal_Range
//     objects, space the attacker (+4) by the defender's (battle_scale_sides()).
//   * Space reinforcement FOW sampler (TACTICAL_GAPS §5.5): reinforce_sample / reinforce_overlay (render-only
//     palette-3 overlay) and reinforce_point_ok (SpaceMode vf80's fog + prevention part, sim).
//   * Queries (§5): point_fogged() / object_hidden(); a non-human player is never fogged while
//     AIUsesFogOfWar{Land,Space} is False (shipped) unless force = true.
//   * Lua (§6): reveal_object / reveal_position return an undoable FogCells snapshot, undo_reveal empties it;
//     reveal_all returns nothing; temporary_reveal flashes; disable_rendering(b) sets FOW-active = !b, which
//     turns the fog off for game logic too.
//   * Renderer (R5, §7): render_view(player) is a read-only view of one grid (state, fade, last decay,
//     regrow step); the renderer never writes the grid.
// Determinism: no RNG; results are independent of the order in which sources are serviced within a frame
// (counts commute, counted cells always have state >= 239). Not in the frame CRC; hash() is an opt-in digest for
// tests. N1 stage 2: SAVED in full by write() / read() (see there), NOT rebuilt on load: a recount cannot restore the
// fade timing (state bytes 1..238 regrowing at the decay cadence, fade bytes, last decay frames) nor the quirks
// (allies' undo of the last player's list, setup-phase binarise), so the loaded fog continues bit-identically.
// Threading: serial (sim thread); const queries may run concurrently between mutations.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "alamo/core/parallel.hpp"
#include "alamo/tactical/fog/fog_grid.hpp"

namespace alamo {
class StateHasher;
}

namespace alamo::tactical::fog {

enum class FogMode : uint8_t { Land = 1, Space = 2 };

// GameConstants (shipped values).
struct FogConstants {
    float desired_land_cell_size = 15.0f;   // DesiredLandFOWCellSize
    float desired_space_cell_size = 100.0f; // DesiredSpaceFOWCellSize
    float land_regrow_time = 6.0f;          // LandFOWRegrowTime
    float space_regrow_time = 6.0f;         // SpaceFOWRegrowTime
    float fps = 30.0f;
    bool ai_uses_fog_land = false;          // AIUsesFogOfWarLand
    bool ai_uses_fog_space = false;         // AIUsesFogOfWarSpace
    float tactical_overrun_multiple = 3.0f; // Tactical_Overrun_Multiple
    float min_overrun_seconds = 30.0f;      // Minimum_Tactical_Overrun_Time_In_Secs
    bool setup_phase_enabled = false;       // SetupPhaseEnabled (GameConstants +0x2ef; False in FoC and TR)
    float setup_countdown_seconds = 5.0f;   // SetupPhaseCountdownSeconds (GameConstants +0x11c8)
};

struct FogPlayer {
    int32_t id = -1;
    bool grid = true;    // false for the first player of a neutral faction (0x331cc0)
    bool human = false;  // player +0x62
};

struct FogSetup {
    FogMode mode = FogMode::Space;
    FogBox box;  // map extents (mode slot +0x1f8)
    FogConstants constants;
    std::vector<FogPlayer> players;  // player-list order
    bool fow_active = true;          // carried global FOW-active flag (space keeps it, land sets 1)
    bool render_option = true;       // carried render option (same rule)
    std::vector<int32_t> saved_reveal_all;  // save chunk 0x17: Reveal_All re-applied at start
};

// One object as the fog sees it (filled by the host from the store / type data / combat modifiers).
struct FogObject {
    int32_t owner = -1;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    // Sight base: land = Land_FOW_Reveal_Range x weather factor (weather_sight_factor), space =
    // Space_FOW_Reveal_Range. Default 0 (the radius then floors at 10).
    float sight_range = 0.0f;
    float dense_multiplier = 0.5f;  // Dense_FOW_Reveal_Range_Multiplier (clamped [0, 1] at load)
    float object_modifier = 0.0f;   // sum of the object's COMBAT_MOD_FOW_REVEAL_RANGE (player + object merged) [M]
    // Model bounding box: centre offset from the position (world XY) and half extents; no model -> radius 10.
    bool has_model = false;
    float box_cx = 0.0f, box_cy = 0.0f, box_hx = 0.0f, box_hy = 0.0f;
};

// Inputs of FOW_Object_Hidden (§5.2).
struct FogHideQuery {
    int32_t owner = -1;
    bool team_or_hide_when_fogged = false;  // has TEAM (0x16) or HIDE_WHEN_FOGGED (0x19)
    bool has_reveal = false;                // has REVEAL (0x18)
    float x = 0.0f, y = 0.0f, z = 0.0f;
    bool multisample = false;               // type Multisample_FOW_Check
    std::array<float, 16> corners{};        // with multisample: 8 bounding-box corners (x, y) in world space
};

// LuaFOWCellsClass: player id + the counted cell list of one Reveal.
struct FogCells {
    int32_t player = -1;
    CellList cells;
};

// Read-only renderer input for one player's grid (R5 contract).
struct FogRenderView {
    bool valid = false;
    int32_t player = -1;
    int32_t width = 0, height = 0;
    FogBox box;
    float cell_w = 0.0f, cell_h = 0.0f;
    std::span<const uint8_t> state, fade;
    uint32_t frame = 0;       // current mode frame F
    uint32_t last_decay = 0;  // f = clamp(F - last_decay - 1, 0, 15)
    int32_t regrow_step = -1;
    bool fow_active = true, render_option = true, setup_phase = false, reveal_all = false;
};

// Land setup phase object (§3.4).
struct FogSetupObject {
    FogObject obj;
    bool reveal_during_setup = false;  // Reveal_During_Setup_Phase or Reveal_During_Setup_Phase_Only
    bool reveal_for_attacker = false;  // Reveal_For_Attacker (+0x5a)
    bool reveal_for_defender = false;  // Reveal_For_Defender (+0x5b)
    bool has_locomotor = false, has_reveal = false;
    float reinforcement_prevention_radius = 0.0f;
    bool has_powered = false;  // POWERED behaviour (obj +0x341 != -1): the prevention circle then needs base power
};

// Base power of a player (0x527520 on the player's tactical data, TACTICAL_GAPS §7.3): true while an allied
// Base_Power_Ability source lives; an empty list is false.
using BasePowerFn = std::function<bool(int32_t player)>;

// §3.1.1 weather factor: intensity > 0.5 and the owner is weather-affected -> the Sight_Range_Modifier
// (missing -> 1, <= 0.0001 -> 1); otherwise 1.
float weather_sight_factor(float intensity, bool owner_affected, bool has_modifier, float modifier);
// §3.1 battle scale (mode +0x14): 1 + sum of Reveal_Range_Modifier over the other side's objects whose type
// has Modifies_Reveal_Range, clamped to [0.25, 1]; summed in the given order.
float battle_reveal_scale(std::span<const float> reveal_range_modifiers);
// TACTICAL_GAPS §5.3 (record +4 = attacker, +8 = defender): land sums the ATTACKER's objects and scales the
// DEFENDER's reveals; space sums the defender's objects and scales the attacker's.
struct BattleScaleSides {
    int32_t scaled = -1;  // the player whose radii are multiplied (set_battle_scale)
    int32_t summed = -1;  // the player whose Modifies_Reveal_Range objects are summed
};
BattleScaleSides battle_scale_sides(FogMode mode, int32_t attacker, int32_t defender);

// An object's Reinforcement_Prevention_Radius circle (GOM list +0x2f0 entry) for the reinforcement sampler.
struct PreventionCircle {
    int32_t owner = -1;
    float x = 0.0f, y = 0.0f, radius = 0.0f;
};

class FogOfWar {
public:
    void start(const FogSetup& setup);
    bool started() const { return started_; }
    FogMode mode() const { return mode_; }
    const FogConstants& constants() const { return k_; }

    // Shared dense mask from (x, y, radius) circles (position + Space_Obstacle_Offset, Space_Obstacle_Radius)
    // of objects with Is_Asteroid_Field / Is_Impassable_Asteroid / Is_Ion_Storm / Is_Nebula and SPACE_OBSTACLE.
    struct DenseCircle {
        float x = 0.0f, y = 0.0f, radius = 0.0f;
    };
    void set_dense_obstacles(std::span<const DenseCircle> circles);
    const std::vector<uint8_t>& dense_mask() const { return mask_; }

    // Refog hook (AI threat grid slot 3): (player, cell); cell_centre() gives the world point.
    void set_refog(RefogFn fn) { refog_ = std::move(fn); }
    // Relation p -> owner (0 = allied, the default for p == owner; others default to enemy).
    void set_relation(int32_t p, int32_t owner, uint8_t relation);
    bool allied(int32_t p, int32_t owner) const;
    // Owner's tactical player FOW reveal-range list (+0x1f8, TACTICAL_GAPS §2.3.11 / §5.2). Writer: the
    // Battlefield_Modifier apply (per affected player, category = Stacking_Category, source = (ability, user)):
    // add_player_reveal_multiplier stores FOW_Reveal_Range_Multiplier - 1 (skipped at exactly 1.0); the entries of
    // a source are removed when its ability ends. Reduction (0x33fb70 with MostExtreme, TACTICAL_COMBAT §4.2):
    // categories ascending, within one the largest |value| (first kept on ties), summed in category order. Read at
    // reveal time (sources pick it up when they next re-reveal; nothing is marked dirty, FOG_OF_WAR §3.2 [H]).
    void add_player_reveal_modifier(int32_t player, int32_t category, uint64_t source, float value);
    void add_player_reveal_multiplier(int32_t player, int32_t category, uint64_t source, float multiplier);
    void remove_player_reveal_modifier(int32_t player, uint64_t source);
    float player_reveal_bonus(int32_t player) const;
    // Battle scale (mode +0x14) applied to reveals for `player` (battle_scale_sides().scaled: the record's +8
    // defender on land, the +4 attacker in space).
    void set_battle_scale(int32_t player, float scale);
    void set_human(int32_t player, bool human);

    // ---- per frame ----------------------------------------------------------------------------------------
    void decay_step(uint32_t frame);  // common tick step 4 (also sets the current frame)
    void set_frame(uint32_t frame) { frame_ = frame; }
    uint32_t frame() const { return frame_; }

    // ---- reveal sources (REVEAL behaviour) -----------------------------------------------------------------
    // Service (vf6): creates the source (dirty) on first call. `contained` = garrisoned / inside a transport;
    // `setup_only` = Reveal_During_Setup_Phase_Only. While the land setup phase is active the REVEAL behaviours
    // are disabled (setup start disables every behaviour but a few, TACTICAL_GAPS §5.4): nothing but the source
    // record is created [port: every source, also those created during setup].
    void source_service(int32_t id, const FogObject& o, bool contained = false, bool setup_only = false);
    void source_change_owner(int32_t id, int32_t old_owner, const FogObject& o);  // vf18
    void source_remove(int32_t id);                                               // vf15
    // SP5 (docs/SIM_PARALLEL_DESIGN.md §4.3): a frame's source services as one batch, the same result as calling
    // source_remove / source_service for `ops` in order. The re-reveals' cell lists (sight radii, circles, dense mask:
    // const) are computed in parallel through `pf`; every grid change (undo, count) is then applied serially in op order,
    // so the clamped undo and the per-cell first-reveal state see exactly the serial sequence. An id appears at most once.
    struct SourceOp {
        int32_t id = -1;
        bool remove = false;  // true: source_remove(id); false: source_service(id, obj, contained, setup_only)
        bool contained = false, setup_only = false;
        FogObject obj;
    };
    void service_sources(std::span<const SourceOp> ops, const ParallelFor& pf);
    // Fired hook (REVEAL vf26 0x537390 from the post-shot hook 0x3a06a0, TACTICAL_GAPS §5.1): every shot flashes
    // the SHOOTER's footprint on the TARGET OWNER's grid only (not its allies); no damage-side caller.
    void source_fired(const FogObject& shooter, int32_t target_owner);
    void mark_sources_dirty();
    bool has_source(int32_t id) const;
    size_t source_count() const { return sources_.size(); }
    float move_threshold_sq() const { return threshold_; }

    // RevealObject (0x35d1b0): footprint for non-allied objects unless force, else sight. Appends the cells.
    void reveal_object(int32_t p, const FogObject& o, CellList& out, bool force);
    void sight_radii(int32_t p, const FogObject& o, bool force, float& cx, float& cy, float& r, float& r_dense) const;

    // ---- other sources ------------------------------------------------------------------------------------
    void reveal_all(int32_t p);       // Force Sight (start), overrun, Lua Reveal_All
    void undo_reveal_all(int32_t p);  // Force Sight end
    std::vector<int32_t> reveal_all_players() const;  // save chunk 0x17
    // Overrun reveal (§3.3, common tick step 3): every 15 frames, game type in {1, 2, 4}, enabled (mode +0x2cf),
    // frame >= 30 x min seconds, min(A, B) x multiple < max(A, B): the stronger record player gets Reveal_All
    // once. Returns the player revealed this call, -1 otherwise.
    int32_t service_overrun(uint32_t frame, int32_t game_type, bool enabled, int32_t p4, float power4, int32_t p8, float power8);
    bool overrun_done(bool p8_side) const { return p8_side ? overrun8_ : overrun4_; }

    // ---- land setup phase (§3.4) ---------------------------------------------------------------------------
    void set_setup_phase(bool active) { setup_ = active; }
    bool setup_phase() const { return setup_; }
    // The setup reveal's "powered" test (unset: no player has base power). Host hook, not saved (like the refog hook).
    void set_base_power(BasePowerFn fn) { base_power_ = std::move(fn); }
    // Setup manager (mode +0x1f0, §3.4 "Manager details"). Command 32 SetupPhaseTriggerEnd (0x5aef60): the player's ready
    // byte (player +0x184) := 1; the first call arms the SetupPhaseCountdownSeconds countdown at the current frame
    // ((int)(fps x seconds) frames); all-ready := the AND of the ready bytes of every human player. No phase test (the
    // original runs it whenever the mode has a manager); the armed flag is never cleared.
    void setup_player_ready(int32_t player);
    bool setup_ready(int32_t player) const;
    bool setup_all_ready() const { return setup_all_ready_; }
    bool setup_countdown_armed() const { return setup_armed_; }
    // Common tick step 11 (0x3bfba0): the setup phase is active and (every human is ready, or the countdown is armed
    // and F0 + frames <= frame). The host then calls end_setup.
    bool setup_end_due(uint32_t frame) const;
    // Setup start (land, SetupPhaseEnabled): the setup phase becomes active (decay and REVEAL services stop) and
    // the setup reveal runs (with the binarise). The host services its REVEAL sources once before (behaviour
    // Initialise services at once, then setup start disables them).
    void begin_setup(std::span<const FogSetupObject> objects, int32_t p4, int32_t p8);
    // Setup reveal (0x3c01e0) + binarise while the setup phase continues. p4 / p8 = record players (-1: none).
    void setup_reveal(std::span<const FogSetupObject> objects, int32_t p4, int32_t p8);
    // Setup end (0x3bfba0): behaviours re-enabled, grids Reset(0), setup reveal, grids Reset(1). Nothing marks the
    // REVEAL sources dirty (TACTICAL_GAPS §5.4, quirk kept): they keep their stored lists, so stationary units'
    // cells regrow ~6.4 s after setup end until they move one cell.
    void end_setup(std::span<const FogSetupObject> objects, int32_t p4, int32_t p8);

    // ---- space reinforcement FOW sampler (TACTICAL_GAPS §5.5) ---------------------------------------------
    // SpaceReinforceFOWSampler vf0 0x4d92d0(mode, p, pos): 0 when a NON-ALLIED circle covers pos (2-D, strict <,
    // compared squared), else 0xFF. Render only (palette 3, SpaceReinforceFOWColor).
    uint8_t reinforce_sample(int32_t p, float x, float y, std::span<const PreventionCircle> circles) const;
    // The sampler at every cell centre of p's grid (W x H, row 0 = max-y edge, like state()); false (out
    // emptied) when p has no grid.
    bool reinforce_overlay(int32_t p, std::span<const PreventionCircle> circles, std::vector<uint8_t>& out) const;
    // The fog / prevention part of the space drop validity (SpaceMode vf80, TACTICAL_GAPS §4.2): unless forced,
    // rejected when fogged for p (force 1: AI players too) or inside a non-allied prevention circle. The
    // occupancy / playable-box tests are the host's.
    bool reinforce_point_ok(int32_t p, float x, float y, std::span<const PreventionCircle> circles) const;

    // ---- queries (§5) -------------------------------------------------------------------------------------
    bool point_fogged(int32_t p, float x, float y, bool force) const;
    bool object_hidden(int32_t p, const FogHideQuery& q, bool force) const;

    // ---- Lua / story (§6) ---------------------------------------------------------------------------------
    FogCells lua_reveal_object(int32_t p, const FogObject& o);  // FogOfWar.Reveal(player, object)
    FogCells lua_reveal_position(int32_t p, float x, float y, float radius, float dense_radius);  // battle scale applies
    void undo_reveal(FogCells& cells);                          // cells.Undo_Reveal(): decrement, empty
    void recount(const FogCells& cells);                        // load: re-count every listed cell once
    void temporary_reveal(int32_t p, const FogObject& o, float radius);  // radius 0 -> footprint radius
    void disable_rendering(bool disable);                       // FOW-active = render option = !disable
    void reward_enable_fow(bool has_param, int32_t value);      // story reward ENABLE_FOW
    bool fow_active() const { return fow_active_; }
    bool render_option() const { return render_option_; }
    void set_fow_active(bool on) { fow_active_ = on; }
    void set_flags(bool fow_active, bool render_option) {
        fow_active_ = fow_active;
        render_option_ = render_option;
    }

    // ---- grids --------------------------------------------------------------------------------------------
    const FogGrid* grid(int32_t p) const;
    FogGrid* grid(int32_t p);
    std::span<const FogPlayer> players() const { return players_; }
    FogRenderView render_view(int32_t p) const;
    void hash(StateHasher& h) const;  // opt-in (fog is not in the frame CRC)

    // ---- N1 stage 2 save / load (contract: alamo/core/save_file.hpp; src/fog_save.cpp) ------------------------
    // Saves ALL state except the refog and base-power hooks: started flag, mode, constants, players (id, grid, human),
    // every grid (FogGrid::write, by player id, invalid ones included), the relation table, the player FOW reveal-range
    // lists and their reduced sums, the dense mask (PackBits), battle scale, the REVEAL sources (ascending id: owner,
    // stored position, dirty / revealed, cell list), move threshold, frame, regrow step, FOW-active / render option /
    // setup / overrun flags, the setup manager (ready bytes, countdown, armed / all-ready). Layout: u32 version (2), then
    // those fields in that order (grids as u32 length + FogGrid bytes).
    // read() replaces everything, re-links the grids' dense-mask pointers to the loaded mask and keeps the hooks
    // (the host attaches set_refog / set_base_power as at battle start; before or after read). After a load the host must NOT call
    // start(), recount() (the Lua FogCells' counts are in the saved grids), source_service re-reveals or re-apply the
    // saved Reveal_All players: the loaded state already holds them. False (unchanged) on an invalid mode, a mask
    // that does not match the grids, inconsistent list sizes, unordered sources or truncated data.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    struct Source {
        int32_t id = -1;
        int32_t owner = -1;
        float x0 = 0.0f, y0 = 0.0f;
        CellList cells;
        bool dirty = true, revealed = false;
    };
    Source* find_source(int32_t id);
    const Source* find_source(int32_t id) const;
    void reveal_for_allies(Source& s, const FogObject& o);
    void undo_for_allies(const Source& s, int32_t owner);
    bool ai_gate(int32_t p, bool force) const;
    bool is_human(int32_t p) const;

    bool started_ = false;
    FogMode mode_ = FogMode::Space;
    FogConstants k_;
    std::vector<FogPlayer> players_;
    std::vector<FogGrid> grids_;  // by player id (invalid grid = none)
    std::vector<uint8_t> relation_;
    int32_t rel_n_ = 0;
    struct RevealEntry {
        int32_t category = 0;
        uint64_t source = 0;
        float value = 0.0f;
    };
    void reduce_player_list(int32_t player);
    std::vector<std::vector<RevealEntry>> player_list_;  // by player id, insertion order
    std::vector<float> player_bonus_;                    // reduced sums of player_list_
    std::vector<uint8_t> mask_;
    int32_t scale_player_ = -1;
    float scale_ = 1.0f;
    RefogFn refog_;
    BasePowerFn base_power_;
    std::vector<Source> sources_;  // ascending id
    float threshold_ = 100.0f;
    uint32_t frame_ = 0;
    int32_t step_ = 21;
    bool fow_active_ = true, render_option_ = true, setup_ = false;
    bool overrun4_ = false, overrun8_ = false;
    // Setup manager: ready bytes by player id, countdown (start frame, length), armed (+0x35), all ready (+0x36).
    std::vector<uint8_t> setup_ready_;
    uint32_t setup_count_start_ = 0;
    int32_t setup_count_frames_ = 0;
    bool setup_armed_ = false, setup_all_ready_ = false;
};

}  // namespace alamo::tactical::fog
