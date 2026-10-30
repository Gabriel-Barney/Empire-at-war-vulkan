// G0-3 command scheduler: orders are queued and handed to the sim at a scheduled tick (the original's
// ScheduledEventQueue, docs/re/COMMANDS.md §4).
//
// TICK MODEL. drain(T) is called exactly once per sim tick, at the start of tick T - right after the
// frame increment, before the game-mode update (MAIN_LOOP T2; see pipeline.hpp) - with T = first_tick,
// first_tick+1, ... (no gaps). Between drain(T) and drain(T+1) the scheduler's "now" is T. Before the
// first drain, now = first_tick.
//
// STAMPING (submit(): local human orders; thread-safe, one issuer thread per player):
//   SinglePlayer: scheduled = now + delay (delay 1 = the original: issued between tick N and N+1,
//     applied at the start of N+1). The original also runs orders given while paused immediately;
//     here they apply at the start of the next tick, which is equivalent for the sim because nothing
//     ticks while paused (a UI may preview e.g. the selection).
//   Lockstep: scheduled = floor((now + 1 + MaxAhead + max(T-1, 1)) / T) * T, or now + 1 if MaxAhead = 0
//     and that equals now (§4.3). Presets: lan() T=1 A=2 (+4 frames), internet() T=5 A=10 (+11..15),
//     internet_fast() T=2 A=4. FrameSync retunes MaxAhead (set_max_ahead) at a synced frame.
// Only scheduled event types are accepted; non-scheduled ones (FrameInfo, Chat, PerformanceMetrics,
// Taunt) are executed on receipt by the session layer and never enter the sim -> NotScheduled.
//
// RECEIVE (network peers): receive() keeps the wire's player + execute frame, stamps the next arrival
// sequence of that player (the original has no sequence; per-sender arrival order plays that role).
// INSERT (replays): insert() keeps the full header. For both, in Lockstep the frame is first rounded up
// to a multiple of T (execution-time re-alignment, §4.3; the deferral window FS+0x2c/+0x30 is not
// modelled). Then:
//   (player, sequence) already queued or drained                 -> Duplicate (insert only)
//   scheduled_tick already drained (< next drain tick)            -> LatePolicy::Reject: Late;
//      LatePolicy::Bump: moved to the next undrained tick. In lockstep the gate (§4.4) makes late
//      commands impossible, so Late means a protocol bug / desync.
// Accepted inserts raise the player's next sequence above the inserted one.
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

enum class LatePolicy : uint8_t { Reject = 0, Bump = 1 };
enum class ScheduleMode : uint8_t { SinglePlayer = 0, Lockstep = 1 };

struct SchedulerConfig {
    uint32_t delay = 1;            // SinglePlayer: ticks between issue and application (>= 1)
    uint8_t max_players = 8;       // valid ids 0..max_players-1 (<= kMaxPlayerSlots)
    LatePolicy late = LatePolicy::Reject;
    uint64_t first_tick = 0;       // first tick that will be drained
    ScheduleMode mode = ScheduleMode::SinglePlayer;
    uint32_t turn = 1;             // Lockstep: command-turn length T (frames, >= 1)
    uint32_t max_ahead = 2;        // Lockstep: MaxAhead (frames)

    static SchedulerConfig single_player() { return {}; }
    static SchedulerConfig lan() { return lockstep(1, 2); }
    static SchedulerConfig internet() { return lockstep(5, 10); }
    static SchedulerConfig internet_fast() { return lockstep(2, 4); }
    static SchedulerConfig lockstep(uint32_t t, uint32_t a) {
        SchedulerConfig c;
        c.mode = ScheduleMode::Lockstep;
        c.turn = t;
        c.max_ahead = a;
        return c;
    }
};

// The §4.3 execute frame for a command issued at `current`.
uint64_t lockstep_execute_frame(uint64_t current, uint32_t turn, uint32_t max_ahead) noexcept;

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
    // Later submissions only; queued commands keep their tick.
    void set_delay(uint32_t delay);
    void set_max_ahead(uint32_t max_ahead);  // FrameSync
    void set_turn(uint32_t turn);

    SubmitResult submit(uint8_t player, CommandSource source, CommandPayload payload);
    SubmitResult receive(Command cmd);  // network: arrival sequence assigned here
    SubmitResult insert(Command cmd);   // replay: full header kept

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
    // Saves the whole scheduler: config (delay / max_ahead / turn as retuned at run time), now, next
    // drain tick, the queue (tick ascending, each tick's commands in arrival order, canonical command
    // bytes), per player the next sequence and the drained-sequence set. read() restores all of it,
    // config included (max_players may differ from the constructor's); false and nothing changed on a
    // bad version / config / command or an inconsistent queue.
    static constexpr uint32_t kSaveVersion = 1;
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    enum class Arrival : uint8_t { Local, Assign, Explicit };
    SubmitResult check_and_queue(Command&& cmd, Arrival how);
    uint64_t stamp_locked() const noexcept;
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
