// R3 per-object animation state (docs/re/ANIM_PARTICLES.md §A.3-§A.8). Animation is SIMULATION state:
// behaviours read the current clip / variant / frame / mode / crossfade (idle timing, flinch, weapon gate,
// death removal, move-clip phase, Walk_Transition) and gameplay reads animated bone transforms. Everything
// here is deterministic: fixed float sequences (no FMA, anim_math.hpp), RNG draws only through the SimRng
// the caller passes (synced for primary-store objects, free for secondary-store objects, §A.8), rows kept in
// ascending object id, the per-frame advance is a pure per-row function run with per-index outputs.
//
// AnimWorld = the animation part of ModelClass for every object of one store, SoA by row (rows sorted by
// ObjectId). Three slots per object (§A.3): primary (current clip), previous (fading out; its timer holds the
// crossfade) and overlay (Attack_Animation_Is_Overlay clips).
//
// Mapping to the original:
//   play()     ModelClass play 0x265560 + GameObjectClass::Play_Animation 0x3a92f0 (lock bit, random
//              variant index 0xFFFF on the passed RNG, start frame). The Berserker override (§A.4.1) is the
//              caller's (abilities) job: it rewrites the request before calling play().
//   create()   model created 0x3a4820: default idle, index 0, loop = Loop_Idle_Anim_00, speed =
//              Idle_Anim_00_Rate_Mod, then a random start frame in [0, count - 1] (no draw without a clip).
//   advance()  FUN_140265280 / 0x2650f0 at t = frame / 30 (object Service step 6): loops wrap at count - 1,
//              one-shots stop at max(count - 2, 0) and become finished, timed slots expire, crossfade weight
//              = 1 - remaining / duration.
//   pose       alHModel::vf15 0x12c7c0: bind / one clip / crossfade (+ overlay), nlerp with the retail fast
//              inverse square root, local x parent world in bone order.
//
// PORT CHOICES ([port] = not settled by the RE, picked here and listed in docs/PHASE4.md R3 open items):
//   * sim time is kept in double (frame / 30.0); dt = float(t - last) feeds the float frame advance;
//   * crossfade blend modes 2/3 rescale `remaining` when a fade is running, then duration = blend;
//   * an unbound clip (bone mismatch) poses as the bind pose; as the previous slot it is ignored;
//   * the idle behaviour's first fidget timer is 0 (a fidget may play on the first idle service);
//   * AnimationSFXMaps.txt events fire for the primary AND the overlay slot (primary first, anim_events.hpp);
//   * play_overlay(loop = true) loops the overlay clip (the RE only shows one-shot overlays; for "and similar"
//     callers and tests). The mode byte is already part of the slot state, so save / hash are unchanged.
#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "alamo/anim/anim_events.hpp"
#include "alamo/anim/anim_math.hpp"
#include "alamo/anim/anim_types.hpp"
#include "alamo/anim/clip.hpp"
#include "alamo/core/parallel.hpp"

namespace alamo {
class SimRng;
class StateHasher;
class CrcRegistry;
class ByteWriter;
class ByteReader;
}  // namespace alamo

namespace alamo::anim {

using ObjectId = int32_t;  // = objects::ObjectId (gameplay-visible id; rows are ordered by it)

inline constexpr double kSimTicksPerSecond = 30.0;
inline constexpr uint32_t kRandomVariant = 0xFFFF;  // Play_Animation index meaning "random variant"
inline constexpr float kLuaPlayBlendSeconds = 0.33f; // Lua GameObject.Play_Animation (0x57e6c0)

enum class SlotMode : uint8_t { OneShot = 0, Loop = 1, Finished = 2 };
enum Slot : uint8_t { kPrimary = 0, kPrevious = 1, kOverlay = 2, kSlotCount = 3 };

// Unknown .bss blend-time globals (§A.7, doc §C item 1). [port] 0 = hard cut, as the RE doc recommends
// until a capture settles them (G1 capture paused).
struct AnimTunables {
    float idle_blend_seconds = 0.0f;  // 0x140b39930 (idle behaviour, type change)
    float move_blend_seconds = 0.0f;  // 0x140b16d18 (WalkLocomotor)
};

struct PlayRequest {
    int32_t type = kAnimIdle;          // kNoAnimType clears primary + previous
    uint32_t index = 0;                // variant; kRandomVariant = synced/free random in [0, count - 1]
    bool loop = false;
    float blend_seconds = 0.0f;        // 0 = hard cut
    float speed = 1.0f;
    float start_frame = 0.0f;          // > 0: start there (forced 0 when the clip has < 2 frames)
    uint8_t blend_mode = 0;            // 0 normal; 2 / 3 rescale a running fade (flinch / idle)
    bool lock = false;                 // object +0x3a0 bit 3
    float timed_seconds = 0.0f;        // > 0: slot finishes (speed reset to 1) after this many seconds
};

enum class PlayResult : uint8_t {
    Played = 0,
    Cleared,       // type -1
    Locked,        // ignored: a locked animation runs and the request has no lock
    NoClip,        // index out of range (type has no such variant) - the original returns 0
    NoObject,
};

struct SlotView {
    int32_t type = kNoAnimType;
    uint32_t variant = 0;
    SlotMode mode = SlotMode::Finished;
    float frame = 0.0f;
    float speed = 1.0f;
    bool timed = false;
    float timed_remaining = 0.0f;
    const ClipRef* clip = nullptr;  // null when the slot is empty
};

struct AnimCreateParams {
    int32_t idle_type = kAnimIdle;  // default idle type (71 when crouched, garrison mapping) - the caller's
    bool loop_idle = false;         // Loop_Idle_Anim_00
    float idle_rate = 1.0f;         // Idle_Anim_00_Rate_Mod
};

class AnimWorld {
public:
    explicit AnimWorld(AnimTunables tunables = {}) : tunables_(tunables) {}

    const AnimTunables& tunables() const { return tunables_; }
    void set_tunables(const AnimTunables& t) { tunables_ = t; }

    // ---- membership ----
    // Model created (§A.7 first row). `model` must outlive the world (or the row). `rng`: synced for
    // primary-store objects, free for secondary ones; null = no start-frame draw (frame 0). `frame` = the
    // global frame at creation (sets the last-advance time). Replaces an existing row of the same id.
    void create(ObjectId id, const AnimModel* model, const AnimCreateParams& p, SimRng* rng, uint64_t frame);
    // Row without starting a clip (e.g. a model with no idle); play() later.
    void add(ObjectId id, const AnimModel* model, uint64_t frame);
    bool remove(ObjectId id);
    bool contains(ObjectId id) const { return row(id) != kNoRow; }
    size_t size() const { return id_.size(); }
    std::span<const ObjectId> ids() const { return id_; }  // ascending
    const AnimModel* model(ObjectId id) const;

    // ---- commands (sim thread, service order) ----
    PlayResult play(ObjectId id, const PlayRequest& req, SimRng* rng, uint64_t frame);
    // Overlay slot (Attack_Animation_Is_Overlay, 0x3a8ed0): no crossfade; timed when timed_seconds > 0; one-shot
    // unless `loop` ([port], see above).
    PlayResult play_overlay(ObjectId id, int32_t type, uint32_t index, float speed, float timed_seconds, SimRng* rng, uint64_t frame,
                            bool loop = false);
    void clear_overlay(ObjectId id);
    // WalkLocomotor speed refresh (0x3a7b70): primary speed.
    void set_speed(ObjectId id, float speed);
    // Move <-> Walk switch keeping the phase (§A.7 walk row): frame = old frame * new count / old count.
    PlayResult switch_preserving_phase(ObjectId id, int32_t type, uint32_t index, bool loop, float blend, float speed, uint64_t frame);
    void set_frozen(ObjectId id, bool frozen);  // object +0x3a7: advance skipped [L]
    bool frozen(ObjectId id) const;
    // Berserker ability active (§A.4 item 1): play() rewrites requests other than Attack / Attack Idle / Idle to a
    // looping SpinMove and leaves a running spin alone until it is within 2 frames of its end. The caller (the
    // battle, from AbilitySystem::is_active(BERSERKER)) keeps the flag current.
    void set_berserker(ObjectId id, bool active);
    bool berserker(ObjectId id) const;

    // ---- per-frame advance (object Service step 6) ----
    // Advances every unfrozen row to t = frame / 30. Pure per row: run through `pf` with per-index outputs.
    void advance(uint64_t frame, const ParallelFor& pf = serial_for, uint32_t grain = 256);
    // One row (the Service path when objects are serviced one by one).
    void advance_one(ObjectId id, uint64_t frame);

    // ---- queries (behaviours) ----
    SlotView slot(ObjectId id, Slot s = kPrimary) const;
    int32_t current_type(ObjectId id) const { return slot(id).type; }
    SlotMode current_mode(ObjectId id) const { return slot(id).mode; }  // 0x395660
    bool crossfading(ObjectId id) const;
    float crossfade_weight(ObjectId id) const;  // 1 when no fade
    bool frame_changed(ObjectId id) const;      // primary frame moved in the last advance (pose dirty)
    bool near_end(ObjectId id) const;           // object +0x74 bit 3 is CLEAR once frame >= count - 2.5
    bool locked(ObjectId id) const;
    // Frame count / fps of the current clip (0 when none).
    uint32_t current_frame_count(ObjectId id) const;

    // ---- idle behaviour (0x62e570, the idle part only; §A.7 idle row, §A.11) ----
    // Runs when the current clip is finished / absent, when idle_class_restart(current type, T = idle_type) (an idle or
    // crouch clip of the other crouch class: restart mid-clip, anim_types.hpp), or when crouch_changed forces it. Presence idles (77 / 78) take the same path.
    // No clip of T: the timer draws below still happen, the play fails (false) and the finished clip holds its last
    // frame; the next call retries. When frame >= the idle timer:
    // variant = rng int [1, count - 1] (0 with one clip; no draw then) and timer = frame + rng int [210, 450];
    // otherwise variant 0. Plays the idle type one-shot at speed 1, blend 0 when the type is unchanged, else
    // tunables().idle_blend_seconds. Returns true if it played.
    bool service_idle(ObjectId id, int32_t idle_type, bool crouch_changed, SimRng& rng, uint64_t frame);
    uint64_t idle_timer(ObjectId id) const;

    // ---- animation events (AnimationSFXMaps.txt, firing rule in anim_events.hpp) ----
    // Protocol around advance(): mark_events() for the rows of interest (ascending id), advance(), then
    // crossed_since() / fired_events() per mark in mark order. No play / clear between the mark and advance().
    // Marks are output bookkeeping only: never saved, never hashed.
    struct EventMark {
        ObjectId id = 0;
        Slot slot = kPrimary;            // kPrimary or kOverlay
        const ClipRef* clip = nullptr;
        int32_t type = kNoAnimType;
        uint32_t variant = 0;
        float frame = 0.0f;              // frame before the advance
        bool loop = false;
    };
    struct FiredEvent {
        ObjectId id = 0;
        Slot slot = kPrimary;            // kOverlay: fired by the overlay slot's clip
        const ClipRef* clip = nullptr;   // the clip that fired
        const AnimEventDef* def = nullptr;  // into the AnimEventMap passed to fired_events (kind, name, bone, time)
        uint32_t index = 0;              // index of def in the clip's event list
    };
    // Appends the marks of `id`'s event-capable slots: primary, then overlay, each only when it holds a clip that is
    // not Finished. Returns the number appended (0..2).
    size_t mark_events(ObjectId id, std::vector<EventMark>& out) const;
    // After the advance: appends the indices into `events` (the marked clip's list, AnimEventMap::find) crossed since
    // the mark, in firing order. Nothing when the slot no longer holds the marked clip (cleared / replaced).
    void crossed_since(const EventMark& mark, const std::vector<AnimEventDef>& events, std::vector<uint32_t>& out) const;
    // Every mark in order (primary before overlay per object): crossed_since against map.find(clip name).
    void fired_events(std::span<const EventMark> marks, const AnimEventMap& map, std::vector<FiredEvent>& out) const;

    // ---- pose (model space, §A.6) ----
    // Local transforms of every bone of the object's skeleton (bind pose for bones without a track,
    // unbound clips, or no clip). False when the object has no row or its model has no skeleton.
    bool local_pose(ObjectId id, std::span<Mat34> local, std::span<uint8_t> visible = {}) const;
    // Per-bone visibility only (§A.6: the clip's per-bone visibility bits; in a crossfade visible if either clip
    // says visible; the overlay's bit for non-excluded bones; 1 for bones without a track / unbound clips). Same
    // bits as local_pose's `visible`, without the matrices. visible.size() must be >= the skeleton size.
    bool bone_visibility(ObjectId id, std::span<uint8_t> visible) const;
    // World (model-space) transforms of every bone. world.size() must equal the skeleton size.
    bool world_pose(ObjectId id, std::span<Mat34> world, std::span<uint8_t> visible = {}) const;
    // Gameplay bone read (0x12d2c0 -> vf15): model-space transform of one bone (only its parent chain is
    // evaluated). False for a bad id / bone.
    bool bone_transform(ObjectId id, int32_t bone, Mat34& out) const;
    bool bone_transform(ObjectId id, std::string_view bone_name, Mat34& out) const;
    // World position of a bone (projectile spawn, Lua Get_Bone_Position): object_to_world (position, facing,
    // Scale_Factor; the caller's) applied to the bone's model-space origin.
    bool bone_world_position(ObjectId id, int32_t bone, const Mat34& object_to_world, Float3& out) const;

    // ---- CRC (opt-in: nothing registers it by default, so existing goldens are unchanged) ----
    // Ordered digest: rows by id ascending, every sim-visible field (types, variants, modes, frame / speed /
    // timer bit patterns, crossfade, lock, idle timer, last time).
    void hash_state(StateHasher& h) const;
    void register_crc(CrcRegistry& registry, const std::string& name = "anim") const;

    // ---- N1 save / load (contract: alamo/core/save_file.hpp; src/anim_save.cpp) ----
    // Layout: u32 version (1), u32 row count, then per row in ascending id: i32 id, bool has-model, u32
    // name_crc(model->model) (0 without a model), f64 last time, bool frozen / lock / frame-changed / near-end /
    // berserker, u64 idle timer, the three slots (primary, previous, overlay: i32 type, u32 variant, u8 mode, f32
    // frame, f32 speed, bool timed, f32 timed remaining), then the crossfade (bool active, f32 duration, f32
    // remaining, f32 weight). Floats as bit patterns. Not saved: the tunables (configuration) and the model
    // pointers (resolved on read).
    // read(): `model_of(id)` returns the model of each row saved with one (the host's model choice for that object,
    // e.g. SimAnimation::model_for of the store object or of a corpse's object type); a null model, or one whose
    // name CRC differs from the saved one, fails the read. Replaces every row (clip, time and every random-derived
    // field come back as saved: nothing is re-drawn). False and unchanged on bad data; reads its own fields only
    // (the owner checks done()).
    // N1 stage 2: the host does NOT re-create the saved rows after read (SimAnimation restores its max_seen so
    // sync() skips them) and keeps calling set_tunables() as for a new game.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r, const std::function<const AnimModel*(ObjectId id)>& model_of);

private:
    static constexpr uint32_t kNoRow = 0xFFFFFFFFu;
    uint32_t row(ObjectId id) const;
    uint32_t insert_row(ObjectId id);
    void advance_row(uint32_t r, double t);
    PlayResult play_row(uint32_t r, const PlayRequest& req, SimRng* rng, uint64_t frame);
    void clear_slot(uint32_t r, Slot s);
    const ClipRef* slot_clip(uint32_t r, Slot s) const;
    bool sample_local(uint32_t r, size_t bone_count, std::span<Mat34> local, std::span<uint8_t> visible, int32_t only_chain_bone) const;

    AnimTunables tunables_;
    // SoA by row (ascending id).
    std::vector<ObjectId> id_;
    std::vector<const AnimModel*> model_;
    std::vector<uint32_t> model_crc_;  // name_crc(model->model), 0 without a model
    std::vector<double> last_time_;
    std::vector<uint8_t> frozen_, lock_, frame_changed_, near_end_, berserk_;
    std::vector<uint64_t> idle_timer_;
    // Per slot.
    std::vector<int32_t> type_[kSlotCount];
    std::vector<uint32_t> variant_[kSlotCount];
    std::vector<uint8_t> mode_[kSlotCount];
    std::vector<float> frame_[kSlotCount];
    std::vector<float> speed_[kSlotCount];
    std::vector<uint8_t> timed_[kSlotCount];
    std::vector<float> timed_left_[kSlotCount];
    // Crossfade (previous slot timer, ModelClass +0x108..+0x114).
    std::vector<uint8_t> fade_active_;
    std::vector<float> fade_duration_, fade_remaining_, fade_weight_;
};

// Sampling helper (exposed for tests and the render side): keys a, b and fraction t of frame position
// `frame` in a clip of `count` frames - a = floor(frame), b = a + 1, both wrapped into [0, count - 1);
// t = frame - floor(frame). Clips with < 2 frames sample frame 0 (t = 0).
void sample_keys(float frame, uint32_t count, uint32_t& a, uint32_t& b, float& t);

// Full single-clip pose (no state): `clip` sampled at frame position `frame` on `skel` (local -> world).
// Used by the render side for stateless background idles (MapScene). world.size() must be skel.size().
void clip_world_pose(const Skeleton& skel, const ClipRef& clip, float frame, std::span<Mat34> world);

// Loop / one-shot frame advance rules on one value (§A.5), exposed for tests:
//   loop:     frame += step; while frame >= count - 1: frame -= count - 1   (count < 2: frame = 0)
//   one-shot: frame += step; if frame >= count - 2: frame = max(count - 2, 0), finished
float advance_loop(float frame, float step, uint32_t count);
float advance_one_shot(float frame, float step, uint32_t count, bool& finished);

}  // namespace alamo::anim
