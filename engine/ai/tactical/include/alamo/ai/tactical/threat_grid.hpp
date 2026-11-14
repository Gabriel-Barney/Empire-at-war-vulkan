// A1 stage 1b - TacticalPerceptionGridClass, the tactical threat grid
// (docs/re/AI_TACTICAL_PERCEPTION.md section 4; consumers 3.3, 4.5, 4.6, 5).
//
// Geometry [H]: nFow = ceil(extent / DesiredFOWCellSize) clamped 0..512 per axis (0x22f9f0 rounds away from
// zero: ceil for x >= 0, floor below; A1_READINGS 13 [H]);
// nx = ceil(nFowX / k), ny = ceil(nFowY / k), k = AI_FogCellsPerThreatCell (4);
// cellW = (right - left) / nx, cellH = (top - bottom) / ny, half diagonal sqrt(cellW^2 + cellH^2) / 2.
// Cell index = row * nx + col, ROW 0 AT THE TOP: row = trunc((top - y) / cellH), col = trunc((x - left) / cellW).
//
// Entries [H]: an object with UNIT_AI gets one entry per hard point of type 5..10 with grid_flag set,
// else one whole-object entry. Radius: whole object = max attack range, hard point = weapon range
// (turret: the hard point's position); mobile objects add speed x look-ahead time; then
// r = max(r, half diagonal), r = min(r, range cap). A destroyed destroyable hard point gets r = -1 and is
// not projected. Power: whole object = AI_Combat_Power, hard point = its value.
//
// Projection [H]: the square of half-side r around the entry, v = power x cellW x cellH / (4 r^2) per
// covered cell, added to threat[p][cell] for every player p the object is hostile to (relation 1), and
// (objects without TEAM / HIDE_WHEN_FOGGED) SUBTRACTED from snapshot[p][cell] (0x656a10, ATP open item 3b,
// A1_READINGS 11 [H]: add does not clamp, removal subtracts again and clamps at 0). Loop order: rows outer,
// columns inner; per cell the players in player-list order; then the live-list append [H].
// Re-projection (0x4f6070, A1_READINGS 14 [H]): the UnitAI data pack (0x550ed0) stamps the next check in the
// past at object creation (two synced draws, draw_unit_ai_stamps); the service runs the check when
// stamp <= F, so the first check is at the object's first service; every check then does stamp += 300
// without re-basing on F (a stamp that is behind catches up one step per service). At a check, if the 3x4
// transform differs from the stored copy every entry is removed, refreshed and re-added, else nothing.
// No decay. Stamps are wrapping u32 frames compared by signed difference (a stamp before frame 0 is fine).
//
// Fog memory [H]: per (object, player) a "fogged" byte, updated on every service while the global FOW
// flag is set and the object has TEAM / HIDE_WHEN_FOGGED (else 0); the test is false for a non-human
// player while AIUsesFogOfWar{Land,Space} is False (shipped), so AI players never have fogged objects.
// not fogged -> fogged copies the entry's radius / position / area / centre cell into the player's
// snapshot; fogged -> not fogged removes the entry from the player's memory lists over that snapshot's
// footprint. Refog event (FOW decay, one of p's FOW cells became fully fogged): for the threat cell:
// lastSeen = F, memory list cleared and refilled with the live list, snapshot layer = threat layer.
// Destroyed objects [port] (A1_READINGS 15): the original (0x6585f0) removes the memory entries only for players
// that currently see the object fogged (over that player's snapshot rect); other players keep stale entries
// until the cell refogs and then read the dead object's data. The port removes the entries from every
// player's memory lists, because the dead object's data (power, owner, type) is not in PerceptionWorld after
// destruction and a kept entry slot would dangle. With shipped data (AIUsesFogOfWar False) an AI player never
// has fogged objects, so the difference reaches only the human player's memory lists.
//
// Cell force (0x6545d0) [H]: sum over the cell's live entries, then Self's memory entries, of
// power / area x cellW x cellH x (1 - (1 - health) x A) (power and health read NOW, area = current
// 4 r^2), filters: not excluded, faction filter, category, owned by a non-neutral-faction player,
// relation(owner -> Self) == 0 (ally) / != 0 (enemy), live entries skipped when fogged for Self.
// MEMORY ENTRIES [H] (ATP open item 3a, A1_READINGS 10): the memory branch skips an entry only when it has
// UNIT_AI and is fogged for Self (the same test as for live entries), so with shipped data an AI player's
// memory entries are counted a SECOND time. This is the only rule (the concentration / bitfield queries differ:
// they count memory entries only while fogged).
//
// Determinism: every list is a vector in insertion order (append on add, ordered erase on remove);
// per-player loops are in player list order; sums are float in that order. Projection / service /
// refog are serial (sim thread); queries are const and may run concurrently between them.
#pragma once
#include <cstdint>
#include <map>
#include <span>
#include <string_view>
#include <vector>

#include "alamo/ai/tactical/constants.hpp"
#include "alamo/ai/tactical/hint_zones.hpp"
#include "alamo/ai/tactical/perception_world.hpp"

namespace alamo {
class SimRng;
class StateHasher;
class ByteWriter;
class ByteReader;
}  // namespace alamo

namespace alamo::ai::tactical {

inline constexpr uint32_t kReprojectionFrames = 300;  // 10 s gate [H]
inline constexpr int32_t kUnitAiCheckSpread = 300;     // data pack +0x20: F + Get_Int(0, 300) - 300 [H]
inline constexpr int32_t kUnitAiSecondSpread = 150;    // data pack +0x34: F + Get_Int(0, 150) - 150 [H]
inline constexpr int32_t kMaxFowCellsPerAxis = 512;
inline constexpr int32_t kFirstGridHardPointType = 5, kLastGridHardPointType = 10;
// Point-danger scale squares (0x653ff0): 4000^2 space, 800^2 land [H].
inline constexpr float kSpacePointDangerArea = 4000.0f * 4000.0f;
inline constexpr float kLandPointDangerArea = 800.0f * 800.0f;

struct ThreatGridConfig {
    int32_t fog_cells_per_threat_cell = 4;
    float desired_fow_cell_size = 15.0f;
    float threat_range_cap = 500.0f;
    float threat_look_ahead_time = 0.0f;
    bool ai_uses_fog_of_war = false;
    uint32_t reprojection_frames = kReprojectionFrames;
};
ThreatGridConfig threat_grid_config(TacticalMode mode, const TacticalAiConstants& c);

// Filters of the cell force / concentration / bitfield queries.
struct GridQuery {
    uint64_t category = ~0ull;    // Parameter_Category (default all bits)
    int32_t self = kNoPlayerIndex;
    bool ally = true;             // true: relation(owner -> self) == 0; false: != 0
    float attenuator = 0.0f;      // Parameter_Attenuator
    int32_t exclude = -1;         // the location's attached object
    std::string_view faction;     // Parameter_Faction (owner faction, case-insensitive [M]); empty = any
};

// One matcher of the *UnitsBitfield tokens: exact type name, or category intersect.
struct BitfieldMatcher {
    std::string_view type_name;   // non-empty: exact (case-insensitive) type name
    uint64_t category = 0;
};

// The UnitAI data-pack stamps (0x550ed0, A1_READINGS 14 [H]): set when an object with UNIT_AI is created, from
// two synced draws in this order: reprojection_check = F + Get_Int(0, 300) - 300 (data pack +0x20, the threat-grid
// re-projection gate), then second = F + Get_Int(0, 150) - 150 (+0x34; its consumer is outside the grid). Both lie
// in [F - spread, F], so the first re-projection check is at the object's first service. Frames may be < 0.
struct UnitAiStamps {
    int32_t reprojection_check = 0;
    int32_t second = 0;
};
// Makes the two draws (exactly two range_int calls on `rng`, the synchronized RNG, in the order above).
UnitAiStamps draw_unit_ai_stamps(SimRng& rng, uint32_t frame);

class TacticalPerceptionGrid {
public:
    // Builds the grid (tactical mode start). `players` = player list size.
    void init(TacticalMode mode, const Rect2& map, uint32_t players, const ThreatGridConfig& cfg);
    bool valid() const { return nx_ > 0 && ny_ > 0; }
    TacticalMode mode() const { return mode_; }

    // ---- geometry ----
    int32_t fow_x() const { return fow_x_; }
    int32_t fow_y() const { return fow_y_; }
    int32_t nx() const { return nx_; }
    int32_t ny() const { return ny_; }
    uint32_t cell_count() const { return static_cast<uint32_t>(nx_) * static_cast<uint32_t>(ny_); }
    float cell_w() const { return cell_w_; }
    float cell_h() const { return cell_h_; }
    float half_diagonal() const { return half_diag_; }
    float left() const { return left_; }
    float top() const { return top_; }
    float right() const { return right_; }
    float bottom() const { return bottom_; }
    // Cell of a world point: truncation toward 0 then clamp. -1 when the grid is empty.
    int32_t cell_clamped(float x, float y) const;
    // -1 when the point is outside the grid.
    int32_t cell_strict(float x, float y) const;
    // Centre of cell (col, row): (left + (col + 0.5) cellW, top - (row + 0.5) cellH).
    Vec3f cell_center(int32_t col, int32_t row) const;
    // Cells overlapped by a rect (0x654290): rows top -> bottom, columns left -> right, clamped.
    void rect_cells(const Rect2& r, std::vector<uint32_t>& out) const;
    // Threat cells of a hint zone: its packed (x | y << 16) cells as (col, row), list order, cells
    // outside the grid dropped [M: zone +0x90 holds threat-grid cells, AI_TACTICAL 6.4].
    void zone_cells(const HintZone& z, std::vector<uint32_t>& out) const;
    uint64_t category_union() const { return category_union_; }

    // ---- UNIT_AI hooks (serial) ----
    // First UNIT_AI service / GOM add hook: creates the entries and projects them. `check_stamp` is the
    // object's data-pack re-projection stamp (UnitAiStamps::reprojection_check, A1_READINGS 14 [H]): the next
    // service at F >= stamp runs the check. False if the object is unknown to the world, has no UNIT_AI, or is
    // registered already.
    bool register_object(const PerceptionWorld& w, int32_t id, int32_t check_stamp);
    // UNIT_AI service: fog bytes (every call), then the re-projection gate (stamp <= frame: check, stamp += 300).
    // Returns true when the object was re-projected.
    bool service_object(const PerceptionWorld& w, int32_t id, uint32_t frame);
    // Every registered object in registration order.
    void service_all(const PerceptionWorld& w, uint32_t frame);
    // Destroyed signal: removed from the live lists (threat subtracted), the owner list and every
    // player's memory lists [port: the original removes the memory entries only for players that see it
    // fogged, A1_READINGS 15; see the header comment].
    bool on_object_destroyed(int32_t id);
    // Owner change (0x658930): remove, re-owner, re-add; appended to the new owner's list [M].
    bool on_owner_changed(const PerceptionWorld& w, int32_t id);
    // Refog event for player p at threat cell `cell` (grid slot 3).
    void refog(int32_t player, uint32_t cell, uint32_t frame);
    // Refog at a world point (the FOW cell centre); ignored outside the grid.
    void refog_at(int32_t player, float x, float y, uint32_t frame);

    // ---- state ----
    bool is_registered(int32_t id) const { return objects_.count(id) != 0; }
    bool fogged_for(int32_t id, int32_t player) const;
    float threat(int32_t player, uint32_t cell) const;
    float snapshot(int32_t player, uint32_t cell) const;
    int32_t last_seen(int32_t player, uint32_t cell) const;  // frame (0 = never re-fogged)
    std::span<const int32_t> player_objects(int32_t player) const;  // registration order
    // Live / memory entries of a cell: (object id, hard-point index) in list order.
    std::vector<std::pair<int32_t, int32_t>> live_entries(uint32_t cell) const;
    std::vector<std::pair<int32_t, int32_t>> memory_entries(int32_t player, uint32_t cell) const;
    uint32_t entry_count() const;  // live entries (objects x hard points)

    // ---- queries (const) ----
    float cell_force(const PerceptionWorld& w, uint32_t cell, const GridQuery& q) const;
    float cells_force(const PerceptionWorld& w, std::span<const uint32_t> cells, const GridQuery& q) const;
    // FriendlyUnitConcentration: entries whose centre cell is the cell; memory only while fogged. Each entry adds its object's
    // type AI_Combat_Power, with no exclusion of the location's own unit (A1_READINGS §16).
    float cell_concentration(const PerceptionWorld& w, uint32_t cell, const GridQuery& q) const;
    float cells_concentration(const PerceptionWorld& w, std::span<const uint32_t> cells, const GridQuery& q) const;
    // *UnitsBitfield: bit i set if a matching entry is in the cells (memory only while fogged).
    uint64_t cells_bitfield(const PerceptionWorld& w, std::span<const uint32_t> cells, const GridQuery& q,
                            std::span<const BitfieldMatcher> matchers) const;
    // Visible test 0x656fe0.
    bool cell_visible(const PerceptionWorld& w, int32_t player, uint32_t cell) const;
    // Point danger (0x653ff0): layer x S / (cellW x cellH) at the point's cell, 0 outside.
    float point_danger(const PerceptionWorld& w, int32_t player, float x, float y) const;
    // Zone danger (0x653e90): sum of the layer over the cells, unscaled.
    float cells_danger(const PerceptionWorld& w, int32_t player, std::span<const uint32_t> cells) const;

    void hash(StateHasher& h) const;

    // N1 save / load (contract: alamo/core/save_file.hpp; inside TacticalPerception's section). Layout: u32 version (1),
    // the geometry (mode, players, FOW / threat cell counts, extents, cell size, half diagonal: checked against this
    // grid's own init(), not restored), the category union, every entry slot (free ones included: all fields), the free
    // list, the live lists, per player the threat / snapshot layers and last-seen stamps (sparse: cells whose bit
    // pattern is not 0), the memory lists (sparse), the player object lists, the object records (ascending id) and the
    // registration order. Not saved: the config (init()). read(): false and unchanged on bad data or another geometry;
    // reads its own fields only.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    struct EntrySnap {
        float radius = 0.0f, gx = 0.0f, gy = 0.0f, area = 0.0f;
        int32_t center_cell = -1;
        bool valid = false;
    };
    struct Entry {
        int32_t object = -1;
        int32_t hard_point = -1;
        float radius = 0.0f, gx = 0.0f, gy = 0.0f, area = 0.0f;
        int32_t center_cell = -1;
        // projection as added (removal subtracts the same values)
        bool projected = false;
        int32_t c0 = 0, c1 = -1, r0 = 0, r1 = -1;
        float v = 0.0f;
        bool affects_snapshot = false;
        std::vector<uint8_t> hostile;   // per player
        std::vector<EntrySnap> snap;    // per player (fog transition)
        bool in_use = false;
    };
    struct GridObject {
        int32_t owner = kNoPlayerIndex;
        std::vector<uint32_t> entries;
        uint32_t next_check = 0;        // wrapping frame stamp (data pack +0x20)
        float transform[12] = {};
        std::vector<uint8_t> fogged;    // per player
        bool team_or_hide = false;
    };

    uint32_t alloc_entry();
    void refresh_entry(const PerceptionWorld& w, const PerceptionObject& o, Entry& e, std::span<const PerceptionHardPoint> hps) const;
    void add_entry(const PerceptionWorld& w, const PerceptionObject& o, uint32_t ei, std::span<const PerceptionHardPoint> hps);
    void remove_entry(uint32_t ei);
    float entry_power(const PerceptionObject& o, const Entry& e, std::span<const PerceptionHardPoint> hps) const;
    float entry_power_now(const PerceptionWorld& w, const PerceptionObject& o, const Entry& e) const;
    bool fog_test(const PerceptionWorld& w, int32_t player, int32_t id) const;  // 0x35f470 force = 0
    void footprint(float gx, float gy, float r, int32_t& c0, int32_t& c1, int32_t& r0, int32_t& r1) const;
    bool owner_passes(const PerceptionWorld& w, const PerceptionObject& o, const GridQuery& q) const;
    void remove_from_memory(int32_t player, uint32_t ei, int32_t c0, int32_t c1, int32_t r0, int32_t r1);
    // The query walk of a cell (cell force / concentration / bitfield): fn(entry) for the live entries not fogged for
    // `self`, then (a valid `self`) `self`'s memory entries whose fogged state equals `memory_while_fogged`, each list
    // in its order. Defined in threat_grid.cpp (only used there).
    template <class Fn>
    void visit_cell_entries(uint32_t cell, int32_t self, bool memory_while_fogged, Fn&& fn) const;
    static void erase_value(std::vector<uint32_t>& v, uint32_t x);

    TacticalMode mode_ = TacticalMode::Land;
    ThreatGridConfig cfg_;
    uint32_t players_ = 0;
    int32_t fow_x_ = 0, fow_y_ = 0, nx_ = 0, ny_ = 0;
    float left_ = 0.0f, top_ = 0.0f, right_ = 0.0f, bottom_ = 0.0f;
    float cell_w_ = 0.0f, cell_h_ = 0.0f, half_diag_ = 0.0f;
    uint64_t category_union_ = 0;

    std::vector<Entry> entries_;
    std::vector<uint32_t> free_entries_;              // LIFO reuse
    std::vector<std::vector<uint32_t>> live_;         // cell -> entries
    std::vector<std::vector<float>> threat_;          // player -> cell
    std::vector<std::vector<float>> snapshot_;        // player -> cell
    std::vector<std::vector<int32_t>> last_seen_;     // player -> cell
    std::vector<std::vector<std::vector<uint32_t>>> memory_;  // player -> cell -> entries
    std::vector<std::vector<int32_t>> player_objects_;        // player -> object ids (registration order)
    std::map<int32_t, GridObject> objects_;           // id -> record (lookup; hashed in id order)
    std::vector<int32_t> registration_order_;
};

}  // namespace alamo::ai::tactical
