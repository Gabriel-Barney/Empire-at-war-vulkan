// s78n t06 - hero clashes (docs/re/HERO_CLASH.md §1-§6; TACTICAL_COMBAT §5.4.3 item 3). Land mode only.
//   * HEROCLASH.XML (§1): one entry per <Hero_Clash>, file order, duplicate names kept (the loader 0x4814d0 does not
//     check them). Action_Driven entries are parsed and never started (§4 [port]: shipped Clash_Range 0.0, so retail never
//     starts them either).
//   * HeroClashManager (§2, §3): the land mode's manager 0x56ce20, ticked in land tick step 4 (after weather, before the
//     common object tick). Detection 0x56c790 only when frame % 15 == 0 (no draws), then every running clash's update in
//     start order. Melee clash 0x6ac780 (start) -> approach (1) -> snap (2) -> fight (3) with one synced float draw per
//     exchange (outcome weights First / Second / draw, the previous outcome removed) and the exchange damage at 80 % of
//     hero 1's clip; it ends only when the exchange loser is dead or a hero is gone.
//   * The pairing rule 0x39a540 (§5.2) lives in CombatSystem::clash_allows (the +0x3b4 byte is kUnitHeroClash); the
//     manager hands its running pairs to the host (pairs()).
// The manager owns no object state: the host (HeroClashHost) answers queries and applies the effects (flag, movement,
// damage). Serial, deterministic (ids, never pointers).
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/tactical/combat/combat_system.hpp"

namespace alamo {
class ByteWriter;
class ByteReader;
class StateHasher;
}  // namespace alamo

namespace alamo::tactical::combat {

// Clash_Type (§1.1): Melee_Duel 0, Action_Driven 1, anything else -1 (never starts).
inline constexpr int32_t kClashMelee = 0;
inline constexpr int32_t kClashActionDriven = 1;
// Exchange outcomes (§3.6): 1 first hero wins, 2 second hero wins, 3 draw, 4 conversation; 0 = none yet.
inline constexpr uint8_t kExchangeNone = 0;
inline constexpr uint8_t kExchangeFirstWins = 1;
inline constexpr uint8_t kExchangeSecondWins = 2;
inline constexpr uint8_t kExchangeDraw = 3;
inline constexpr uint8_t kExchangeConversation = 4;
// [port] Exchange clip when the host has no model data (no sim animation, or a model without the clip): the shipped
// HC_WIN / HC_LOSE / HC_DRAW clips of Darth_Vader and Obi_Wan_Kenobi (one variant, 101 frames at 30 fps, §1.2).
inline constexpr uint32_t kClashFallbackClipFrames = 101;
inline constexpr float kClashFallbackClipFps = 30.0f;

struct HeroClashEntry {
    std::string name;
    int32_t clash_type = -1;            // Clash_Type
    float clash_range = 0.0f;           // Clash_Range (3-D start distance)
    bool play_conversation = false;     // Play_Conversation_Events
    float combat_distance = 0.0f;       // Combat_Distance
    float damage_amount = 0.0f;         // Damage_Amount
    float damage_percentage = 0.0f;     // Damage_Percentage
    float first_win = 0.0f;             // First_Hero_Win_Exchange_Chance
    float second_win = 0.0f;            // Second_Hero_Win_Exchange_Chance (draw = 1 - first - second)
    float first_multiplier = 0.0f;      // First_Hero_Damage_Multiplier
    float second_multiplier = 0.0f;     // Second_Hero_Damage_Multiplier
    std::string first_type, second_type;  // First_Hero_Type / Second_Hero_Type (resolved by the host)
    std::vector<std::string> involved;    // Involved_Hero_Types
    // Win / Lose / Draw / Conversation animation type names (First_Hero_*_Anim_Type, Second_Hero_*_Anim_Type).
    std::array<std::string, 4> first_anims, second_anims;
    std::string first_speech, second_speech;  // *_Win_Speech (audio only)
    uint32_t action_count = 0;                // Clash_Actions children (Action_Driven; never serviced, §4)
};

struct HeroClashData {
    std::vector<HeroClashEntry> entries;  // file order, duplicates kept
};

// Parses HEROCLASH.XML bytes (appends to `out`); false when the root is not <Hero_Clashes>. First + Second win chance
// > 1: the loader's error, both set to 0.3 (§1.1).
bool parse_hero_clash_xml(std::span<const uint8_t> bytes, HeroClashData& out, std::vector<std::string>* diagnostics = nullptr);
// Data\XML\HeroClash.xml through the provider; false when the file is missing (no clashes).
bool load_hero_clash_data(const FileProvider& files, HeroClashData& out, std::vector<std::string>* diagnostics = nullptr);

// One exchange outcome (§3.6 "Start a new exchange", the non-conversation branch): weights {1: first, 2: second,
// 3: 1 - first - second} without the previous outcome's key; one synced float draw r = Get_Float(0, total); keys
// ascending, r -= w, the first key with r <= 0 wins; fallback 3.
uint8_t clash_pick_outcome(uint8_t previous, float first_win, float second_win, SimRng& rng);
// The exchange damage (§3.6 step 4): (int)round_half_away((loser start health x Damage_Percentage + Damage_Amount) x
// mult), mult = First_Hero_Damage_Multiplier when the winner's type is First_Hero_Type, Second_... when it is
// Second_Hero_Type, else 0 (0x480ed0).
int32_t clash_exchange_damage(const HeroClashEntry& e, float loser_start_health, bool winner_is_first_type,
                              bool winner_is_second_type);

// Host side of the manager (TacticalBattle in the sim; a stub in the tests). Called from the serial land tick only.
class HeroClashHost {
public:
    virtual ~HeroClashHost() = default;
    // Live objects of exactly type `type` (team containers excluded [port]: a melee start needs the hero objects, so a
    // container candidate always fails in retail), any order.
    virtual void candidates(int32_t type, std::vector<ObjectId>& out) = 0;
    virtual bool alive(ObjectId id) = 0;  // not dying / death clone / removed (+0x3a0 0x40 / 0x80 / 0x02)
    virtual int32_t type_of(ObjectId id) = 0;
    virtual int32_t owner(ObjectId id) = 0;
    virtual bool hero_type(int32_t type) = 0;      // Is_Named_Hero or Is_Generic_Hero (0x39a600)
    virtual bool enemies(int32_t a, int32_t b) = 0;  // relation table [a][b] == 1 (0x2824d0)
    virtual Vec3 position(ObjectId id) = 0;
    virtual float yaw_deg(ObjectId id) = 0;
    virtual float health(ObjectId id) = 0;         // current hull health (+0x5c)
    virtual bool jetpacking(ObjectId id) = 0;      // an active jet-pack locomotor (0x39ae20)
    virtual ObjectId attack_target(ObjectId id) = 0;  // 0x397a40(obj, 0): formation attack target, else the target
    virtual bool walking(ObjectId id) = 0;         // locomotor vf +0x1c0(0): the walk has a path
    virtual bool in_clash(ObjectId id) = 0;        // the +0x3b4 byte
    // +0x3b4 := on. On: TARGETING suspended (slot 9) and the movement orders locked (0x3a8e50(h, 1)); off: unlocked and
    // TARGETING resumed (the release 0x6acb80, states 1 / 3 / 4).
    virtual void set_in_clash(ObjectId id, bool on) = 0;
    // Begin 0x6abde0: leave the hero's coordinators, a new coordinator with a Move_To `spot`.
    virtual void move_to(ObjectId id, Vec3 spot) = 0;
    // Snap 0x6acfb0: position := `pos`, yaw := `yaw_deg` at once (0x3a8f90 / 0x3a8710; no pathing, no collision).
    virtual void place(ObjectId id, Vec3 pos, float yaw_deg) = 0;
    virtual void face(ObjectId id, float yaw_deg) = 0;  // Fight 0x6abcd0: the snap turn toward the opponent
    // Fight: suspend IDLE (slot 7) and the locomotor (slot 1); false: resume them (release in state 3 / 4).
    virtual void hold(ObjectId id, bool on) = 0;
    // Clip of animation `anim` (a ModelAnimType name) on `id`'s model: variant count, frame count and fps of `variant`.
    // False: no model data ([port] the manager then uses the shipped clip, kClashFallbackClip*).
    virtual bool clip(ObjectId id, std::string_view anim, uint32_t variant, uint32_t& variants, uint32_t& frames, float& fps) = 0;
    // The speech manager allows / is done with a conversation (0x3223c0 / 0x3223f0). [port] audio: the host may answer true.
    virtual bool speech_idle() = 0;
    // Take_Damage (0x3a9e30): victim, kind 1, amount, the winner as a non-projectile source, no hardpoint.
    virtual void damage(ObjectId victim, float amount, ObjectId source) = 0;
};

class HeroClashManager {
public:
    enum State : uint8_t { kApproach = 1, kSnap = 2, kFight = 3 };
    struct Clash {
        uint32_t entry = 0;
        ObjectId hero1 = kNoObject, hero2 = kNoObject;
        int32_t type1 = -1, type2 = -1;
        float health1 = 0.0f, health2 = 0.0f;  // +0x38 / +0x3c: health at the clash start
        uint8_t state = kApproach;
        Vec3 spot1, spot2;
        Vec3 snap_pos1, snap_pos2;          // snap start positions
        float snap_yaw1 = 0.0f, snap_yaw2 = 0.0f;  // snap start yaws
        float want_yaw1 = 0.0f, want_yaw2 = 0.0f;  // snap target yaws (own spot -> other spot)
        uint32_t snap_start = 0, snap_end = 0;
        uint8_t outcome = kExchangeNone;   // +0xb8
        uint8_t previous = kExchangeNone;  // +0xbc
        ObjectId ex_winner = kNoObject, ex_loser = kNoObject;
        bool damage_applied = false;       // +0x50
        bool playing = false;              // the exchange clips play (else the next frame starts a new exchange)
        uint32_t ex_start = 0;             // frame the exchange started
        uint32_t ex_frames = 0;            // hero 1's clip frame count
        float ex_fps = 0.0f;               // hero 1's clip fps
        uint32_t exchanges = 0;            // started exchanges (diagnostics; saved, hashed)
    };
    struct EndEvent {
        uint32_t entry = 0;
        ObjectId winner = kNoObject, loser = kNoObject;
        uint32_t frame = 0;
        bool by_removal = false;  // a hero left (§3.8 object-removed handler) instead of dying from the exchange
    };

    // The entries and their First / Second hero types resolved by the host (-1 unknown: the entry never starts).
    void set_data(const HeroClashData* data, std::vector<std::pair<int32_t, int32_t>> types);
    const HeroClashData* data() const { return data_; }
    // The land tick step: detection when frame % 15 == 0, then every clash's update (start order).
    void update(uint32_t frame, HeroClashHost& host, SimRng& rng);

    bool empty() const { return clashes_.empty(); }
    const std::vector<Clash>& clashes() const { return clashes_; }
    // The running clashes' hero pairs, start order (CombatSystem::set_hero_clashes).
    std::vector<std::pair<ObjectId, ObjectId>> pairs() const;
    // True when the running clash list changed since the last call (start / end / load).
    bool take_changed() {
        const bool c = changed_;
        changed_ = false;
        return c;
    }
    std::vector<EndEvent> take_ended() { return std::exchange(ended_, {}); }
    uint32_t started_total() const { return started_; }

    void clear();
    void hash(StateHasher& h) const;
    // N1 stage 2: the running clashes (entry index + every field); data_ / types_ are the host's (set_data before read).
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    bool candidate_ok(HeroClashHost& host, ObjectId id, int32_t other_type) const;
    void detect(uint32_t frame, HeroClashHost& host);
    bool start(uint32_t entry, ObjectId a, ObjectId b, HeroClashHost& host, uint32_t frame);
    // Returns false when the clash ended (removed from the list by the caller).
    bool update_clash(Clash& c, uint32_t frame, HeroClashHost& host, SimRng& rng);
    void begin_snap(Clash& c, uint32_t frame, HeroClashHost& host);
    void fight(Clash& c, HeroClashHost& host);
    void new_exchange(Clash& c, uint32_t frame, HeroClashHost& host, SimRng& rng);
    bool apply_damage(Clash& c, uint32_t frame, HeroClashHost& host);  // false: the clash ended
    void release(const Clash& c, ObjectId hero, HeroClashHost& host) const;
    void end(Clash& c, ObjectId winner, uint32_t frame, HeroClashHost& host, bool by_removal);

    const HeroClashData* data_ = nullptr;
    std::vector<std::pair<int32_t, int32_t>> types_;
    std::vector<Clash> clashes_;
    std::vector<EndEvent> ended_;
    uint32_t started_ = 0;
    bool changed_ = false;
};

}  // namespace alamo::tactical::combat
