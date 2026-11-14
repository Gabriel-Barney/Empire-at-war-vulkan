// G0-3 command scheduler: orders are queued and handed to the sim at a scheduled tick (the original's
// ScheduledEventQueue, docs/re/COMMANDS.md §4).
//
// TICK MODEL. drain(T) is called exactly once per sim tick, at the start of tick T - right after the
// frame increment, before the game-mode update (MAIN_LOOP T2; see pipeline.hpp) - with T = first_tick,
// first_tick+1, ... (no gaps). Between drain(T) and drain(T+1) the scheduler's "now" is T. Before the
// first drain, now = first_tick.
//
// STAMPING (submit(): local human orders; thread-safe, one issuer thread per player): scheduled =
// now + delay (delay 1 = the original's single player: issued between tick N and N+1, applied at the
// start of N+1). The original also runs orders given while paused immediately; here they apply at the
// start of the next tick, which is equivalent for the sim because nothing ticks while paused (a UI may
// preview e.g. the selection).
// Only scheduled event types are accepted; non-scheduled ones (FrameInfo, Chat, PerformanceMetrics,
// Taunt) are executed on receipt by the session layer and never enter the sim -> NotScheduled.
//
// INSERT (replays): insert() keeps the full header. Then:
//   (player, sequence) already queued or drained                 -> Duplicate
//   scheduled_tick already drained (< next drain tick)            -> Late
// Accepted inserts raise the player's next sequence above the inserted one.
//
// The original's network / lockstep scheduling (COMMANDS.md §4.3-§4.4: receive from peers, command
// turns, MaxAhead, FrameSync) is not ported: multiplayer is out of scope (docs/DECISIONS.md, CU7). The
// .acmd header and the saves still carry its config bytes, as the constants below (written as is,
// checked on read), so the stream and save layouts are unchanged.
//
// DRAIN ORDER: scheduled tick, then player id (PlayerList index) ascending, then per-player
// sequence (= issue / arrival order). This is the original executor's order (§4.5).
#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

#include "alamo/commands/command.hpp"
#include "alamo/commands/events.hpp"
#include "alamo/core/bytes.hpp"

namespace alamo::commands {

// Retired lockstep / late-policy config bytes: always these values (single player, reject late).
inline constexpr uint8_t kSchedLateByte = 0;        // late policy: reject
inline constexpr uint8_t kSchedModeByte = 0;        // mode: single player
inline constexpr uint32_t kSchedTurn = 1;           // command-turn length
inline constexpr uint32_t kSchedMaxAhead = 2;       // MaxAhead

struct SchedulerConfig {
    uint32_t delay = 1;            // ticks between issue and application (>= 1)
    uint8_t max_players = 8;       // valid ids 0..max_players-1 (<= kMaxPlayerSlots)
    uint64_t first_tick = 0;       // first tick that will be drained

    static SchedulerConfig single_player() { return {}; }
    friend bool operator==(const SchedulerConfig&, const SchedulerConfig&) = default;
};

struct SubmitResult {
    RejectReason status = RejectReason::None;  // None, InvalidPlayer, NotScheduled, Late, Duplicate, Malformed
    uint64_t scheduled_tick = 0;
    uint32_t sequence = 0;
    bool ok() const noexcept { return status == RejectReason::None; }
};

// One tick's drained commands in canonical order.
struct CommandBatch {
    uint64_t tick = 0;
    std::vector<Command> commands;
    friend bool operator==(const CommandBatch&, const CommandBatch&) = default;
};
// Canonical batch bytes: u64 tick | u32 count | command[count] (command.hpp canonical format).
// This is what the CRC hook hashes and what the recording stores.
void encode_batch(const CommandBatch& b, std::vector<uint8_t>& out);
DecodeError decode_batch(std::span<const uint8_t> in, size_t& pos, CommandBatch& out);

class CommandScheduler {
public:
    explicit CommandScheduler(SchedulerConfig cfg = {});  // throws std::invalid_argument on bad config

    const SchedulerConfig& config() const noexcept { return cfg_; }

    SubmitResult submit(uint8_t player, CommandSource source, CommandPayload payload);
    SubmitResult insert(Command cmd);  // replay: full header kept

    // Removes and returns every command scheduled for `tick`, in canonical order.
    // `tick` must equal next_tick() (throws std::logic_error otherwise).
    CommandBatch drain(uint64_t tick);

    uint64_t now() const;        // tick local submissions are stamped with
    uint64_t next_tick() const;  // next tick drain() expects
    size_t pending() const;      // queued, not yet drained
    // An identical command (header and payload) is queued for its scheduled tick (replay from a save:
    // the save's queue already holds commands the stream recorded when they were drained).
    bool is_queued(const Command& c) const;

    // N1 save / load (alamo/core/save_file.hpp contract; no r.done() check, the caller does that).
    // Saves the whole scheduler: config, now, next
    // drain tick, the queue (tick ascending, each tick's commands in arrival order, canonical command
    // bytes), per player the next sequence and the drained-sequence set. read() restores all of it,
    // config included (max_players may differ from the constructor's); false and nothing changed on a
    // bad version / config / command or an inconsistent queue.
    static constexpr uint32_t kSaveVersion = 1;
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    SubmitResult check_and_queue(Command&& cmd, bool local);
    bool was_drained(uint8_t player, uint32_t seq) const;
    void mark_drained(uint8_t player, uint32_t seq);

    mutable std::mutex mu_;
    SchedulerConfig cfg_;
    uint64_t now_ = 0;
    uint64_t next_tick_ = 0;
    std::map<uint64_t, std::vector<Command>> queued_;  // scheduled tick -> commands (any order)
    std::vector<uint32_t> next_seq_;                   // per player: next sequence
    // Drained sequences per player: every seq < drained_low_ plus the sorted drained_above_.
    std::vector<uint32_t> drained_low_;
    std::vector<std::vector<uint32_t>> drained_above_;
    std::vector<std::vector<uint32_t>> queued_seq_;    // per player: sorted queued sequences
};

}  // namespace alamo::commands
