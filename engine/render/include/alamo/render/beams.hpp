// alamo_render - ability beams (docs/re/ABILITY_VISUALS.md 2, 4, 6): the LineSubClass ribbon renderer (tractor beam,
// energy weapon, super laser, laser-defense zap, leech shields) with its billboard dots and the LightningEffect helix /
// arc, drawn additive (Engine\PrimAdditive) in the Transparent bucket (8).
//
//   * Ribbon (LineSubClass 0x37eaa0): mode 0 = ONE quad stretched over the whole beam, local +-W/2 across, U 1 -> 0 across,
//     V 1 (start) -> 0 (end) along; mode 1 = tiled scrolling ribbon, segments of 200 W with U 0 -> 200 along the length
//     (one repeat per W world units), V 1 -> 0 across; the U offset is frame / frames and the frame advances once per sim
//     tick, so the texture scrolls one repeat every `frames` ticks. The ribbon is NOT a camera billboard: its width axis
//     is horizontal (the beam axis crossed with world +Z) and it faces up [M, probe pending].
//   * Dots (0x37e650): billboard quads of 3.5 x 3.5 on w_galaxy_dot.tga in the ribbon plane, colour 0.7 x beam colour;
//     each moves 1/50 of the beam per tick, wrapped (tractor: 4 dots travelling target -> owner; energy weapon: 10 dots,
//     van der Corput fractions, owner -> target).
//   * Lightning (LightningEffectClass 0x7dcd40): Tractor_Beam = three thin strands spiralling at radius 5 around the axis
//     (3 rev/s); Piet_Energy_Weapon = a flickering jagged arc along the beam. Bolts are made with the RENDER generator
//     (retail 0x140a13e20), never the synced one; the strip geometry (mapping of Detail / Displace) is [M].
// Everything here is presentation: it never touches the sim, nothing is CRC'd.
//
// Data flow: the snapshot's SnapshotBeam records (sim_snapshot.hpp) are resolved to world endpoints per frame
// (sim_beams.hpp, bone lookups) into BeamInstance; BeamAnimator keeps the per-beam animation state across frames (stepped
// once per sim tick); BeamGeometry turns the animated beams into PrimVertex batches per texture; Renderer::set_beams /
// draw_beams (init_beams) draw them.
#pragma once
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/render/camera.hpp"
#include "alamo/render/lasers.hpp"  // PrimVertex

namespace alamo::render {

// Unit-ability types that draw a beam (ability_types.hpp 30 / 31 / 55 / 56 / 58).
inline constexpr uint8_t kBeamTractor = 30, kBeamEnergyWeapon = 31, kBeamSuperLaser = 55, kBeamLaserZap = 56, kBeamLeech = 58;
inline constexpr float kBeamDotSize = 3.5f, kBeamDotColorScale = 0.7f;  // 0x37e650 / 0x1f9860
inline constexpr uint32_t kBeamDotPeriod = 50;                           // counter period: 1/50 of the beam per tick
inline constexpr float kBeamDefaultWidth = 10.0f;                        // [M] width <= 0 (0x271cd0 default untraced)
inline constexpr const char* kBeamDotTexture = "w_galaxy_dot.tga";
inline constexpr const char* kBeamLightningTexture = "lightning_default.tga";
inline constexpr int32_t kBeamTractorDots = 4, kBeamEnergyDots = 10;
inline constexpr float kBeamTractorDotStart[4] = {0.45f, 0.70f, 0.50f, 0.89f};  // 2: fractions from the owner end

// GameConstants.xml Tractor_Beam_* / Energy_Beam_* (vanilla values) - the fallbacks of the per-ability beam fields.
struct BeamConstants {
    struct Set {
        std::string texture;
        float width = 10.0f;
        int32_t frames = 10;
        uint8_t color[4] = {255, 255, 255, 255};
    };
    Set tractor{"tractor_beam00.tga", 10.0f, 10, {0, 75, 0, 255}};
    Set energy{"tractor_beam00.tga", 30.0f, 10, {255, 75, 0, 255}};
};
// Reads Tractor_Beam_Width / _Frames / _Texture / _Color and Energy_Beam_* from GameConstants.xml bytes (missing tags keep
// `out`'s values). False: not parseable.
bool parse_beam_constants(std::span<const uint8_t> xml, BeamConstants& out);

// One resolved beam style (the ability's beam fields over the GC fallbacks, 4.2).
struct BeamStyle {
    std::string texture;              // ribbon texture
    float width = kBeamDefaultWidth;  // W
    uint8_t color[4] = {255, 255, 255, 255};
    int32_t frames = 10;              // animation frames (mode 1: scroll period in ticks)
    uint8_t mode = 0;                 // 0 stretched ribbon (+ dots), 1 tiled scrolling ribbon
    uint16_t dots = 0;                // dot count
    int8_t dot_dir = 1;               // +1 owner -> target, -1 target -> owner
    std::string lightning;            // LightningEffect name ("" none)
    std::string start_bone;           // owner bone the beam starts at ("" = the owner position)
    std::vector<std::string> bones;   // zap: Bone_Names (the one nearest the far end starts the beam)
    int32_t lifetime = 0;             // zap: ticks the beam lasts (Laser_Beam_Frames); 0 = while the ability lists it
    int32_t continuation = 0;         // super laser: ticks kept after the target is lost (Fire_Continuation x FPS)
    float radius = 0.0f;              // zap: Protection_Radius (snapshot matching only, 0 = unknown)
};
// The GC-only style of a beam kind: tractor = Tractor_Beam_* (4 dots, Tractor_Beam helix); energy weapon = Energy_Beam_* (10
// dots, Piet_Energy_Weapon arc); super laser = mode 1, width 35, green (0,237,119), tractor_beam01.tga, 5 frames, no dots;
// laser defense = mode 0, no dots, 4 wide, 2 ticks; leech = the energy fallbacks without dots. HP_trac_bone starts tractor
// and energy weapon.
BeamStyle default_beam_style(uint8_t kind, const BeamConstants& gc);

// ---- LightningEffectTypes.xml
struct LightningStyle {
    std::string name;
    std::string texture = kBeamLightningTexture;
    float width_min = 1.0f, width_max = 1.0f;
    float displace[3] = {0.1f, 0.1f, 0.1f};
    float detail = 1.0f;
    uint8_t color_start[4] = {255, 255, 255, 255}, color_end[4] = {255, 255, 255, 255};
    float fade_min = 1.0f, fade_max = 1.0f;  // Fadeout_Time_Min / Max: a bolt's life in seconds
    int32_t bolts = 1;                       // Number_Bolts
    float interval_min = 0.0f, interval_max = 0.1f;  // Bolt_Creation_Interval_Min / Max (seconds)
    bool update_all = false;                 // Update_All: every bolt is reshaped each tick
    float cycles_per_distance = 0.0f, rotations_per_second = 0.0f, radius = 0.0f;
    bool helix() const { return radius > 0.0f; }
};
class LightningLibrary {
public:
    // The shipped Default / Piet_Energy_Weapon / Tractor_Beam records (tests, runs without game data).
    static LightningLibrary builtin();
    // Adds or replaces (by name, case-insensitive) every <LightningEffect name="..."> of LightningEffectTypes.xml bytes.
    // False: not parseable.
    bool load(std::span<const uint8_t> xml);
    const LightningStyle* find(std::string_view name) const;
    size_t size() const { return styles_.size(); }
    void add(LightningStyle s);

private:
    std::vector<LightningStyle> styles_;
};

// ---- per frame
// One beam with world endpoints (sim_beams.hpp resolves them from SnapshotBeam records).
struct BeamInstance {
    uint64_t key = 0;  // stable identity of the beam across frames (state is kept per key)
    uint8_t kind = kBeamTractor;
    Float3 start{}, end{};
    BeamStyle style;
    bool continuous = true;  // false: a one-shot (zap) - kept for style.lifetime ticks after it first appears
};

// Render generator (retail 0x140a13e20 is an LCG; the constants are not traced [port: Numerical Recipes LCG]).
struct RenderLcg {
    uint32_t state = 1;
    uint32_t next() {
        state = state * 1664525u + 1013904223u;
        return state;
    }
    float unit() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }  // [0, 1)
    float range(float lo, float hi) { return lo + (hi - lo) * unit(); }
};

struct BoltDraw {
    uint32_t seed = 0;   // shape seed (re-rolled every tick with Update_All)
    float age = 0.0f, life = 1.0f;  // seconds
    float width = 1.0f;
    float phase = 0.0f;  // helix phase offset (turns)
};
// One beam of the frame, animated.
struct BeamDraw {
    BeamInstance beam;
    float u_offset = 0.0f;            // mode 1 scroll (frame / frames)
    std::vector<float> dots;          // fractions along start -> end, [0, 1)
    float time = 0.0f;                // seconds the beam has been shown (helix rotation)
    const LightningStyle* lightning = nullptr;
    std::vector<BoltDraw> bolts;      // live bolts
};

// Per-beam animation state across frames. step() is called once per render frame with the frame's beams and the sim tick
// the frame shows; the state advances once per sim tick (at most kMaxCatchUp ticks per call).
class BeamAnimator {
public:
    static constexpr uint64_t kMaxCatchUp = 30;
    explicit BeamAnimator(uint32_t seed = 0x5EEDBEA1u) : rng_{seed} {}
    // Lightning effect source (must outlive the animator and not change while beams are listed); null = no lightning. Resets
    // the animator (running beams hold pointers into the library).
    void set_lightning(const LightningLibrary* lib) {
        lib_ = lib;
        clear();
    }
    // `beams` = the beams listed for this tick. Continuous beams keep their state per key; a one-shot (continuous = false) is
    // taken the first time its key is seen and then lives style.lifetime ticks on its own; a super laser that disappears
    // continues for style.continuation ticks to start + 2 x (end - start) (4.3).
    void step(uint64_t tick, std::span<const BeamInstance> beams);
    std::span<const BeamDraw> draws() const { return draws_; }
    size_t size() const { return states_.size(); }
    void clear();

private:
    struct State {
        BeamDraw draw;
        bool seen = false;
        bool oneshot = false;
        int32_t remaining = 0;        // one-shots: ticks left
        int32_t frame = 0;            // mode 1 animation frame
        float next_create = 0.0f;     // seconds until the next lightning bolt may be made
        BeamInstance last;            // last listed instance (continuation source)
        bool had_beam = false;
    };
    void advance(State& s);
    void advance_lightning(State& s);
    void init_state(State& s, const BeamInstance& b);
    uint64_t last_tick_ = 0;
    bool started_ = false;
    const LightningLibrary* lib_ = nullptr;
    RenderLcg rng_;
    std::map<uint64_t, State> states_;  // ordered by key
    std::vector<BeamDraw> draws_;
};

// ---- geometry
// van der Corput base-2 radical inverse of i >= 1 (the energy weapon's dot fractions, 0x241830): 0.5, 0.25, 0.75, 0.125 ...
float van_der_corput(uint32_t i);

// Frame of a beam: unit direction, length, and the horizontal width axis (direction x world +Z; direction (anti)parallel to
// +Z uses +X). False: start == end.
bool beam_frame(const Float3& a, const Float3& b, Float3& dir, float& length, Float3& side);
// Ribbon vertices / indices appended to `v` / `idx` (mode 0: 4 vertices, 6 indices; mode 1: 4 per segment). `u_offset` is the
// scroll of mode 1 (subtracted from U: the pattern moves start -> end [M]). False: degenerate beam.
bool beam_ribbon(const BeamInstance& b, float u_offset, std::vector<PrimVertex>& v, std::vector<uint32_t>& idx);
// One dot quad centred at `fraction` along the beam (in the ribbon plane), colour 0.7 x beam colour.
bool beam_dot(const BeamInstance& b, float fraction, std::vector<PrimVertex>& v, std::vector<uint32_t>& idx);
// Poly-line of one lightning bolt from a to b. Helix effects (radius > 0): the point at distance s is rotated by 2 pi
// (rotations_per_second x time + cycles_per_distance x s + phase) at `radius` around the axis; others: midpoint displacement
// with Detail + 3 levels [M]. Deterministic in (seed, time).
std::vector<Float3> lightning_points(const LightningStyle& st, const Float3& a, const Float3& b, uint32_t seed, float time, float phase);
// Camera-facing strip of a poly-line (width `w` world units, colour), appended.
void lightning_strip(const std::vector<Float3>& pts, const Float3& eye, float w, const uint8_t (&rgba)[4], std::vector<PrimVertex>& v,
                     std::vector<uint32_t>& idx);

struct BeamGeometry {
    struct Batch {
        std::string texture;
        uint32_t first_index = 0, index_count = 0;
    };
    std::vector<PrimVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Batch> batches;  // per texture (ribbons of that texture, dots, bolts)
    uint32_t ribbons = 0, dots = 0, bolts = 0, skipped = 0;
    void build(std::span<const BeamDraw> draws, const ViewMatrices& view);
};

}  // namespace alamo::render
