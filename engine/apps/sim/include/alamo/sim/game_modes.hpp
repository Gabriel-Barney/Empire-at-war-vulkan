// S5 game modes and the mode stack (docs/re/MAIN_LOOP.md §2-§3, docs/re/OBJECT_RUNTIME.md §5).
//
// GameModeManager keeps a stack whose TOP IS ELEMENT 0. Only the top mode is ticked; a galactic
// mode under a tactical battle stays alive (suspended) and neither its mode frame nor its objects
// change during the battle. There is no menu mode: an empty stack means "simulation stopped".
//
// Every mode owns a primary store (sim objects; ids from the session's shared IdCounter) and a
// secondary store (local / cosmetic objects, own counter, kept out of the sim CRC). Tactical modes
// (land, space) also own the spatial TreeSet (engine/tactical, keyed by gameplay object id).
//
// Tick (vtable +0xb0), real order with stubs where the owning systems do not exist yet:
//   galactic: common tick FIRST, then cinematic step, income at the income frame, per-planet RNG
//             value [stub: no planets in this store yet], victory events.
//   land/space: cinematic step [stub], per-AI-player tactical AI data [stub], movement scratch
//             reset [stub], land: weather + hero clash [stub], tactical frame += 1, victory event
//             0x11 [stub], common tick LAST.
//   common (§3.3): FOW for players with id = F (mod 16) [stub counter], GOM service of the primary
//             then the secondary store (OBJECT_RUNTIME.md §5.3: refit spatial trees, serviced
//             objects newest-first, delayed creations last-to-first, deferred-delete flush),
//             retreat coordinator + victory monitor [stub], bombing/bombard managers [stub].
// The mode frame (+0x10) is advanced by GameModeManager::post_tick() unless the mode is paused.
//
// PROVISIONAL stand-ins until G2 (movement / combat) lands, clearly separate from the real order:
//   * Orders (move / move-to-object / attack / stop) are recorded per object (UnitOrder, keyed by
//     object id in an ordered map).
//   * StubLocomotor: an object with the LOCOMOTOR behaviour and a move-type order advances in a
//     straight line by its Max_Speed per frame (MOVEMENT.md: per-frame units) toward the goal, read
//     from START-OF-TICK positions (computed in parallel, per index; applied in the object's service
//     slot). Attack orders close to Targeting_Max_Attack_Distance and hold. No combat, no damage.
//   * Spatial bounds: a cube of half-extent max(Custom_Hard_X/YExtent) when set, else 40 (space) /
//     10 (land) x Scale_Factor. Real model bounds come with the asset-backed object setup.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/core/parallel.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/objects/object_store.hpp"
#include "alamo/tactical/tree_set.hpp"

namespace alamo::galactic {
class GalacticSim;
}

namespace alamo::sim {

using objects::ModeKind;
using objects::ObjectHandle;
using objects::ObjectId;
using objects::kNoObjectId;

inline constexpr uint32_t kMaxPlayers = 16;  // = commands::kMaxPlayerSlots

// Spatial category bits (TreeSet masks).
inline constexpr uint32_t kCatUnit = 0x1;       // has a locomotor
inline constexpr uint32_t kCatStructure = 0x2;  // selectable, no locomotor
inline constexpr uint32_t kCatProp = 0x4;       // everything else (props, markers, asteroids)

// What a mode tick may use from the host. Trace (optional) receives step names for tests.
struct ModeTickContext {
    uint64_t global_frame = 0;
    GameRandom* random = nullptr;
    const ParallelFor* pf = nullptr;
    uint32_t players = 8;
    std::vector<std::string>* trace = nullptr;
    double sim_time() const { return static_cast<double>(global_frame) / static_cast<double>(30); }
};

enum class OrderKind : uint8_t { Idle = 0, Move = 1, MoveToObject = 2, Attack = 3 };

struct UnitOrder {
    OrderKind kind = OrderKind::Idle;
    uint8_t player = 0;          // issuer
    uint8_t in_range = 0;        // attack: target within Targeting_Max_Attack_Distance (holding)
    uint64_t issued_frame = 0;   // global frame the order was applied in
    Vec3 goal{};                 // Move
    ObjectId target = kNoObjectId;  // MoveToObject / Attack
};

class GameMode {
public:
    GameMode(ModeKind kind, const objects::GameObjectTypes& types, objects::IdCounter& ids);
    virtual ~GameMode() = default;
    GameMode(const GameMode&) = delete;
    GameMode& operator=(const GameMode&) = delete;

    ModeKind kind() const { return kind_; }
    bool tactical() const { return kind_ != ModeKind::Galactic; }
    const char* name() const;

    uint64_t frame() const { return frame_; }  // mode frame (+0x10)
    uint64_t ticks() const { return ticks_; }  // logic ticks run (diagnostic)
    bool suspended() const { return suspended_; }

    objects::ObjectStore& primary() { return primary_; }
    const objects::ObjectStore& primary() const { return primary_; }
    objects::ObjectStore& secondary() { return secondary_; }
    const objects::ObjectStore& secondary() const { return secondary_; }
    const objects::GameObjectTypes& types() const { return *types_; }

    // "paused" query (vtable +0x48): freezes the mode frame.
    virtual bool paused() const { return paused_; }
    void set_paused(bool p) { paused_ = p; }

    // Lifecycle (vtable +0x08 init, +0x10 shutdown, +0x50 suspend, +0x58 resume).
    virtual void init() {}
    virtual void shutdown() {}
    virtual void suspend() { suspended_ = true; }
    virtual void resume() { suspended_ = false; }

    // Logic tick (vtable +0xb0).
    virtual void tick(const ModeTickContext& ctx) = 0;
    // GameModeManager::Post_Tick: mode frame += 1 unless paused.
    void post_tick() {
        if (!paused()) ++frame_;
    }
    // Per-frame (render-rate) update (vtable +0xb8); d = 0.03 per tick done this iteration, else 0.
    virtual void per_frame_update(double d) { last_frame_delta_ = d; }

    // Mode-level sim state for the CRC (frames, counters, orders); stores are hashed separately.
    virtual void hash(StateHasher& h) const;
    // N1 save (SimHost "frame" chunk): the mode-level state hash() covers plus the suspended flag. Stores are saved
    // separately ("objects"). read_state: false (unchanged) on a kind mismatch / bad data. Tactical modes are not
    // saved in stage 1 (SimHost::save_allowed refuses while one is on the stack): their write_state writes nothing
    // and read_state fails.
    virtual void write_state(ByteWriter& w) const;
    virtual bool read_state(ByteReader& r);

protected:
    void common_tick(const ModeTickContext& ctx);
    // Store service hooks used by the common tick (tactical modes add tree refit + stub motion).
    virtual void pre_service(objects::ObjectStore&, const ModeTickContext&) {}
    virtual void service_object(objects::ObjectStore&, ObjectHandle, const ModeTickContext&) {}
    virtual void after_store_update(const ModeTickContext&) {}
    static void trace(const ModeTickContext& ctx, const char* step);

    ModeKind kind_;
    const objects::GameObjectTypes* types_;
    objects::ObjectStore primary_;
    objects::ObjectStore secondary_;
    uint64_t frame_ = 0;
    uint64_t ticks_ = 0;
    uint64_t fow_updates_ = 0;
    bool paused_ = false;
    bool suspended_ = false;
    double last_frame_delta_ = 0.0;
};

class GalacticMode final : public GameMode {
public:
    GalacticMode(const objects::GameObjectTypes& types, objects::IdCounter& ids);
    ~GalacticMode() override;
    void tick(const ModeTickContext& ctx) override;
    void hash(StateHasher& h) const override;
    // N1: + income cadence and the pause counter (restored without re-pushing GalacticSim pauses: the sim's own pause
    // state is in its "galactic.state" chunk).
    void write_state(ByteWriter& w) const override;
    bool read_state(ByteReader& r) override;

    // S3 economy (engine/galactic GalacticSim) running on this mode's primary store. When attached,
    // the tick is GalacticSim::update() (it services the store itself: fleets, planets, production,
    // delayed creations + flush, fiscal cycle), then the secondary store.
    void attach_sim(std::unique_ptr<galactic::GalacticSim> sim);
    galactic::GalacticSim* sim() { return sim_.get(); }
    const galactic::GalacticSim* sim() const { return sim_.get(); }
    // Galactic pause counter (GMM +0x2c, MAIN_LOOP.md §1.5): freezes the mode frame and services;
    // mirrored into GalacticSim::push_pause / pop_pause.
    void push_pause();
    void pop_pause();
    bool paused() const override { return paused_ || pause_count_ > 0; }
    // Income: every income_interval mode frames (GALACTIC_ECONOMY.md fiscal cycle; S3 owns the
    // amounts). Counted here so the cadence is in the CRC.
    void set_income_interval(uint32_t frames) { income_interval_ = frames ? frames : 1; }
    uint64_t income_events() const { return income_events_; }

private:
    uint32_t income_interval_ = 1350;
    uint64_t next_income_frame_ = 0;
    uint64_t income_events_ = 0;
    uint32_t pause_count_ = 0;
    std::unique_ptr<galactic::GalacticSim> sim_;
};

// Land and space battles.
class TacticalMode : public GameMode {
public:
    TacticalMode(ModeKind kind, const objects::GameObjectTypes& types, objects::IdCounter& ids, uint32_t players);

    tactical::TreeSet& trees() { return trees_; }
    const tactical::TreeSet& trees() const { return trees_; }
    uint64_t tactical_frame() const { return tactical_frame_; }  // land +0x410 / space +0x3c0

    // Playable bounds (orders outside are rejected). Defaults: +-1e6.
    void set_bounds(const tactical::Aabb& b) { bounds_ = b; }
    const tactical::Aabb& bounds() const { return bounds_; }

    // Spawns into the primary store with health = fraction x Tactical_Health and links the object
    // into the trees by its object id. fraction is clamped to finite (non-finite -> 1).
    ObjectHandle spawn(const objects::SpawnParams& p, float health_fraction = 1.0f);
    // Destroy_Object + tree dead flag; unlinked from the trees after the store's flush.
    bool destroy(ObjectHandle h);

    tactical::Aabb bounds_of(ObjectHandle h) const;
    uint32_t category_of(ObjectHandle h) const;
    bool movable(ObjectHandle h) const;
    // Map load end (Rebuild_With_Threshold on every tree).
    void finish_load(const ParallelFor& pf = serial_for);

    // Orders (sim state, applied by the command handler).
    void set_order(ObjectId id, const UnitOrder& o);
    void clear_order(ObjectId id) { orders_.erase(id); }
    const UnitOrder* order(ObjectId id) const;
    const std::map<ObjectId, UnitOrder>& orders() const { return orders_; }

    void tick(const ModeTickContext& ctx) override;
    void hash(StateHasher& h) const override;
    // N1 stage 2: GameMode state + tactical frame, AI service count, bounds, the unit orders, the boxes to refit at the
    // next store update and the dying objects not yet flushed. The spatial trees are the host's "spatial" chunk.
    void write_state(ByteWriter& w) const override;
    bool read_state(ByteReader& r) override;

    uint64_t ai_services() const { return ai_services_; }

protected:
    void pre_service(objects::ObjectStore& store, const ModeTickContext& ctx) override;
    void service_object(objects::ObjectStore& store, ObjectHandle h, const ModeTickContext& ctx) override;
    void after_store_update(const ModeTickContext& ctx) override;
    virtual void mode_specific(const ModeTickContext&) {}

private:
    void plan_motion(const ModeTickContext& ctx);

    struct Step {
        ObjectId id = kNoObjectId;
        ObjectHandle h;
        const UnitOrder* order = nullptr;
        Vec3 pos{}, facing{};
        uint8_t moved = 0, arrived = 0, in_range = 0, lost_target = 0;
    };
    void compute_step(Step& s) const;

    std::vector<float> half_extent_;  // per type
    tactical::TreeSet trees_;
    tactical::Aabb bounds_{{-1.0e6f, -1.0e6f, -1.0e6f}, {1.0e6f, 1.0e6f, 1.0e6f}};
    std::map<ObjectId, UnitOrder> orders_;
    uint64_t tactical_frame_ = 0;
    uint64_t ai_services_ = 0;
    // Stub motion scratch (per tick).
    std::vector<Step> steps_;
    std::vector<uint32_t> step_of_slot_;
    std::vector<ObjectId> moved_;       // objects whose box changed; refit at the next store update
    std::vector<ObjectId> dying_;       // destroyed; removed from the trees after the flush
};

class LandMode final : public TacticalMode {
public:
    LandMode(const objects::GameObjectTypes& types, objects::IdCounter& ids, uint32_t players)
        : TacticalMode(ModeKind::Land, types, ids, players) {}

protected:
    void mode_specific(const ModeTickContext& ctx) override;  // weather + hero clash [stubs]
};

class SpaceMode final : public TacticalMode {
public:
    SpaceMode(const objects::GameObjectTypes& types, objects::IdCounter& ids, uint32_t players)
        : TacticalMode(ModeKind::Space, types, ids, players) {}
};

// The mode stack. Session-level effects of Start_Game (FrameSynchronizer reset, RNG seeding) are
// done by the host (SimHost::start_game); this class does the stack part.
class GameModeManager {
public:
    GameModeManager(const objects::GameObjectTypes& types, uint32_t players) : types_(&types), players_(players) {}

    // Start_Game push: suspend the current top, create the mode, insert at index 0, init. A
    // tactical mode pushed over another mode gets the parent-mode death hook (§2.5).
    GameMode& push(ModeKind kind);
    // End_Game_Mode pop: shutdown + destroy the top; the new top is resumed. False if empty.
    bool pop();

    bool empty() const { return stack_.empty(); }
    size_t depth() const { return stack_.size(); }
    GameMode* top() { return stack_.empty() ? nullptr : stack_.front().get(); }
    const GameMode* top() const { return stack_.empty() ? nullptr : stack_.front().get(); }
    GameMode& at(size_t i) { return *stack_.at(i); }  // 0 = top
    const GameMode& at(size_t i) const { return *stack_.at(i); }
    TacticalMode* top_tactical();
    const TacticalMode* top_tactical() const;

    // GameModeManager::Update: scoring service [stub], then ONLY the top mode's tick.
    void update(const ModeTickContext& ctx);
    // Post_Tick: top mode frame += 1 unless paused.
    void post_tick();

    objects::IdCounter& ids() { return ids_; }
    const objects::IdCounter& ids() const { return ids_; }
    void reset_ids() { ids_.reset(); }

private:
    const objects::GameObjectTypes* types_;
    uint32_t players_;
    objects::IdCounter ids_;  // shared by the primary stores of every mode in the session
    std::vector<std::unique_ptr<GameMode>> stack_;
};

}  // namespace alamo::sim
