// N1 save / load at the host level (contract and file container: alamo/core/save_file.hpp).
//
// A save of a G0 session (SimHost::save) is a SaveFile whose FIRST chunk is the session descriptor ("session"): what a
// loader must rebuild before any state can be read. Loading is two steps, done by the application that knows how to
// build sessions (alamo_headless, alamo_game):
//   1. read_session(save) -> SessionDescriptor; load the game data it names (data_args) and build a fresh session
//      exactly like a new game of that descriptor (same SimConfig, campaign, players, options, attached systems),
//      then SimHost::set_session(descriptor of the rebuilt session, with the CURRENT data fingerprint);
//   2. SimHost::load(save): refuses a save whose descriptor does not match the rebuilt session (other campaign,
//      players, options or game data: SessionDescriptor::mismatch), then reads every chunk over the new-game state.
//      The new-game initialisation still runs in step 1; every chunk's read() replaces what it set up (definitions
//      are rebuilt from the data, state comes from the save).
//
// CHUNKS (SimHost::save order; names = the CrcRegistry system names where there is one):
//   session          SessionDescriptor (this header)
//   frame            global frame, last batch CRC, command counters, the GameModeManager stack bottom -> top (kinds,
//                    mode frames, counters, galactic income / pause state; tactical modes: tactical frame, AI services,
//                    bounds, unit orders, boxes to refit, dying objects)
//   objects          the shared IdCounter + every mode's primary store (ObjectStore::write), bottom -> top
//   spatial          only with a tactical mode on the stack: per mode bottom -> top a tactical flag + the mode's TreeSet
//                    (TreeSet::write; CRC "spatial" = the top tactical mode's set)
//   galactic.state   GalacticSim::write (players: credits, tech, ... live in the GalacticSim player packs)
//   players          per-player selections + control groups
//   rng              GameRandom (synced + free)
//   commands         CommandPipeline::write (scheduled queue, sequence counters)
//   globals          the session GlobalValue table + the process table (built with engine/lua only)
//   story            StorySession: session state, fired / trigger logs, every plot set's StoryModel, and while a
//                    battle runs its tactical plot set (identity, clock base, model, queued notifications)    } with a
//   story.lua        every plot script's LuaVm + its service gate (galactic sets, then the tactical set)      } story
//   story.bridge     the story VMs' ObjectBridge script state                                                } session
//   story.players    the story VMs' LuaPlayerWorld                                                           } only
//   story.presentation  the story VMs' PresentationLog (blocks the scripts wait on, presentation state) (stage 2)
//   <participants>   systems the application attached (add_save_participant), in registration order, e.g.
//                    "galactic.conflicts" + "battles" (CampaignBattles::attach), "ai" (GalacticAiWorld, attached by the
//                    application), "anim" (SimAnimation::attach), and while a battle runs the battle chunks CampaignBattles
//                    registers at the battle start (campaign_battles.hpp "SAVE CHUNKS OF A RUNNING BATTLE"):
//                    battle.state, battle.movement, battle.combat, battle.abilities, battle.garrison, battle.hangar,
//                    battle.fog, battle.damage, battle.ai (tactical AI on)
//
// LOADING A SAVE TAKEN DURING A BATTLE (stage 2). The application rebuilds the session exactly as for any save (step 1:
// new game of the descriptor, galactic map only). SimHost::load sees more modes in the "frame" chunk than the session
// has and runs two passes:
//   pass 1  every expected chunk is read leniently (failures ignored, chunks of systems that do not exist yet skipped);
//           "frame" / "objects" restore the bottom modes only. This puts the galaxy, the story, the RNG and the
//           participants' own state at the save point; CampaignBattles' "battles" read stages the running battle's
//           request (SaveParticipant::rebuild data) and the story's read stages the tactical plot set identity.
//   rebuild every participant's `rebuild` callback (registration order) pushes what it owns like a new battle:
//           CampaignBattles starts the staged battle exactly as at the battle start (start_game, TacticalBattle built
//           from the saved request over the restored galaxy, map loaded through the map loader, setup(), the story's
//           tactical plots re-attached without the BattleStarted dispatch, the tactical AI begun, the battle chunks
//           registered). setup() spawns and draws like a new battle; every object, system and the RNG it touched is
//           replaced in pass 2, while what saves do not hold (definitions, battle tables, hooks, callbacks, the stores'
//           interval configs and mode bindings, pointers between systems) stays as the rebuild made it.
//   pass 2  the mode depth must now equal the saved one; every chunk (host, story, participants incl. the new battle
//           chunks) is read strictly, as for a galactic save.
// WHEN SAVING IS REFUSED (SimHost::save_allowed): no session; a tactical mode on the stack while no participant has a
// rebuild callback (e.g. alamo_headless --map runs a bare tactical mode); a story blocker; a participant blocker (e.g.
// CampaignBattles: a battle that has ended or was stopped for auto-resolve and has not been popped yet).
// A failed load (LoadReport names every failing / missing chunk) leaves a half-loaded session: discard it. Orders after a
// load: SimHost::replay_after_load / record_after_load (commands/recording.hpp "REPLAY FROM A SAVE").
#pragma once
#include <concepts>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "alamo/commands/scheduler.hpp"
#include "alamo/core/bytes.hpp"
#include "alamo/core/save_file.hpp"

namespace alamo::objects {
class GameObjectTypes;
}
namespace alamo::galactic {
struct GalacticRules;
struct GalacticSetup;
}  // namespace alamo::galactic

namespace alamo::sim {

// One player of the session as the loader must set it up again (player id = index).
struct SessionPlayer {
    std::string faction;     // faction name ("" = none)
    bool human = false;
    bool ai = false;
    std::string ai_type;     // AI player type (AI_Player_Control / Basic_AI); "" = none
    std::string difficulty;  // DifficultyAdjustments name; "" = none
    friend bool operator==(const SessionPlayer&, const SessionPlayer&) = default;
};

struct SessionDescriptor {
    static constexpr uint32_t kVersion = 1;

    // What to build: "galactic" = a data-driven campaign / galactic conquest; hosts may define their own kinds
    // (alamo_headless "synthetic").
    std::string kind;
    std::string campaign;                 // campaign / scenario name
    std::string data_root;                // where the writer found the game data (informational, not compared)
    std::vector<std::string> data_args;   // VFS command line (MODPATH=.., FALLBACKPATH=..), as given
    // SimConfig of the session (SimHost::save fills these from the host).
    uint32_t seed = 0;
    uint32_t players = 0;
    commands::SchedulerConfig scheduler;
    bool story_crc = false;
    std::vector<SessionPlayer> setup;     // player list
    std::string difficulty;               // session difficulty name ("" = per player)
    // Host options that change the simulation (story on, galactic AI on, battle mode, ...), any order (compared as a
    // sorted list).
    std::vector<std::pair<std::string, std::string>> options;
    // Fingerprint of the loaded definitions (fingerprint_types / fingerprint_galactic, combined by the host): a save
    // from different game data is refused.
    uint64_t data_fingerprint = 0;
    std::string writer;                   // engine build that wrote the save (informational)

    std::string option(std::string_view key, std::string_view fallback = {}) const;
    void set_option(std::string key, std::string value);  // replaces an existing key

    void write(ByteWriter& w) const;
    bool read(ByteReader& r);  // false (unchanged) on a version mismatch / truncated data
    // "" when a save written with `saved` can be loaded into a session described by `current`, else the first
    // difference (kind, campaign, data args, SimConfig, players, difficulty, options, data fingerprint).
    static std::string mismatch(const SessionDescriptor& saved, const SessionDescriptor& current);
};

// The descriptor of a save, without loading anything (what the application needs to rebuild the session).
bool read_session(const SaveFile& save, SessionDescriptor& out, std::string* err = nullptr);

// Definition fingerprints (StateHasher digests; equal data -> equal value on every build).
//   types:    every object type (names, every field's tag + text) of the typed view's raw database
//   galactic: GalacticRules (factions, difficulties, per-type economy fields) + the campaign setup (planet rows,
//             positions, routes, planet types, players, starting forces)
uint64_t fingerprint_types(const objects::GameObjectTypes& types);
uint64_t fingerprint_galactic(const galactic::GalacticRules& rules, const galactic::GalacticSetup& setup);
uint64_t combine_fingerprints(std::initializer_list<uint64_t> parts);

// A system the application attached to the session (not owned by SimHost) that saves as one chunk. write / read
// follow the save_file.hpp contract; `blocker` (optional) returns why a save is impossible right now ("" = fine).
// `rebuild` (optional, stage 2): called by SimHost::load between its two passes when the save has more modes than the
// session; it pushes the modes / systems the participant owns from what its pass-1 read staged and returns "" or why it
// cannot (the load then fails with that reason). A participant with a rebuild callback is what makes a tactical mode
// on the stack saveable.
struct SaveParticipant {
    std::string name;     // chunk name: non-empty, no whitespace, unique (not a host chunk name)
    uint32_t version = 1;
    std::function<void(ByteWriter&)> write;
    std::function<bool(ByteReader&)> read;
    std::function<std::string()> blocker;
    std::function<std::string()> rebuild;
};

// What SimHost::load found (every chunk is tried, so one load names every failing chunk).
struct LoadReport {
    std::vector<std::string> loaded;      // chunks read successfully
    std::vector<std::string> failed;      // read() returned false / bad version (state of those systems unchanged)
    std::vector<std::string> missing;     // expected by the rebuilt session, absent from the save
    std::vector<std::string> unexpected;  // in the save, but the rebuilt session has no such system
    std::string descriptor_error;         // non-empty: the save does not belong to this session (nothing was read)
    bool ok() const { return descriptor_error.empty() && failed.empty() && missing.empty() && unexpected.empty(); }
    std::string text() const;             // one line
};

// ---- helpers for nested sections inside a chunk ----
// u32 byte length + the bytes `fn` writes (a sub-system's own write), so the outer reader stays aligned even when the
// sub-system writes nothing (stubs).
template <class F>
void write_section(ByteWriter& w, F&& fn) {
    ByteWriter sub;
    fn(sub);
    w.u32(static_cast<uint32_t>(sub.size()));
    w.raw(sub.bytes());
}
// Reads a write_section blob and hands `fn` a reader over exactly those bytes; true when fn succeeds and consumed
// them all.
template <class F>
bool read_section(ByteReader& r, F&& fn) {
    const uint32_t n = r.u32();
    const std::span<const uint8_t> d = r.raw(n);
    if (!r.ok()) return false;
    ByteReader sub(d);
    return fn(sub) && sub.ok() && sub.done();
}

// True for a type with the save_file.hpp system methods (write / read). Used to pick up implementations in other
// modules as they land (sections whose system lacks them are written empty and fail to read: "pending").
template <class T>
concept SaveableSystem = requires(const T& c, T& m, ByteWriter& w, ByteReader& r) {
    c.write(w);
    { m.read(r) } -> std::same_as<bool>;
};
// write / read when T has them; otherwise nothing is written and the read fails (the chunk reports "pending").
template <class T>
void write_if_saveable(const T& system, ByteWriter& w) {
    if constexpr (SaveableSystem<T>) {
        system.write(w);
    } else {
        (void)system;
        (void)w;
    }
}
template <class T>
bool read_if_saveable(T& system, ByteReader& r) {
    if constexpr (SaveableSystem<T>) {
        return system.read(r);
    } else {
        (void)system;
        (void)r;
        return false;
    }
}
template <class T>
constexpr bool is_saveable(const T*) {
    return SaveableSystem<T>;
}

}  // namespace alamo::sim
