// G0-3 command pipeline: scheduler + apply handler + event bus + recorder/replayer + CRC hook.
// The one path for all orders (player UI, AI, script, network, replay).
//
// begin_tick(T) is THE command point of a sim tick: call it first in tick T, before any system
// update (provisional until docs/re/MAIN_LOOP.md / COMMANDS.md say where the original applies them).
// It runs, in this order:
//   1. replay mode: insert the recorded batch of T into the scheduler (live submissions are refused);
//   2. drain the batch of T (canonical order: tick, player slot, per-player sequence);
//   3. CRC hook: crc_hook(bytes, size) over the canonical batch bytes (encode_batch), every tick,
//      also for empty batches - Q1 registers this with its per-tick CRC;
//   4. append the batch to the recording (if recording);
//   5. serially, in batch order: FrameSync first retunes the scheduler's MaxAhead (§4.3), then
//      handler(cmd) resolves ids and applies; a CommandResult (diagnostic only, see events.hpp) is
//      posted to the event bus in the same order.
// The handler must follow the original's rules (COMMANDS.md §6.2): resolve ids at execution, treat
// missing/dying targets as a silent no-op, act on the issuer's current selection where the event says
// so (selection is synced sim state set by Select/SelectAll/ControlGroup), and depend only on the
// event fields + sim state (never on the local player).
//
// AI and Lua do not use the queue (COMMANDS.md §5): they build the same EventData and call
// execute_direct() inside their own tick slot. That runs the handler at once - no delay, no network, no
// recording, no batch CRC (their effects are covered by the state CRC; they replay by re-running).
// Events are not delivered here; the host calls events().deliver() at its chosen point.
#pragma once
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "alamo/commands/events.hpp"
#include "alamo/core/bytes.hpp"
#include "alamo/commands/recording.hpp"
#include "alamo/commands/scheduler.hpp"

namespace alamo::commands {

// Validates and applies one command to the sim; returns None when applied, else the reason.
using CommandHandler = std::function<RejectReason(uint64_t tick, const Command& cmd)>;
// Receives the canonical bytes of each tick's drained batch.
using BatchCrcHook = std::function<void(const void* data, size_t size)>;

class CommandPipeline {
public:
    explicit CommandPipeline(SchedulerConfig cfg = {}) : sched_(cfg) {}

    CommandScheduler& scheduler() noexcept { return sched_; }
    EventBus& events() noexcept { return bus_; }

    void set_handler(CommandHandler h) { handler_ = std::move(h); }
    void set_crc_hook(BatchCrcHook h) { crc_hook_ = std::move(h); }

    // Starts recording drained batches into `rec` (config taken from the scheduler, first_tick = its
    // next_tick()); nullptr stops. `from_save` (N1): the pipeline was just loaded from that save, the
    // stream continues it (stored as rec->from_save; recording.hpp "REPLAY FROM A SAVE").
    void record_to(CommandRecording* rec, std::optional<SavePoint> from_save = std::nullopt);
    // Replays `rec` (nullptr ends replay). The pipeline must have been built with rec->config.
    void replay_from(const CommandRecording* rec);
    // N1 replay from a save: call after read() restored this pipeline from the save `loaded` (instead
    // of building a pipeline from rec->config). Checks that `rec` continues this state, then replays it
    // as replay_from(rec) does:
    //   * same scheduler mode, turn, max_players and late policy as the loaded scheduler (they decide
    //     how inserts are aligned / accepted; delay and max_ahead only stamp live orders);
    //   * a stream with from_save: from_save == loaded and config.first_tick == scheduler().next_tick();
    //   * a stream without (a full recording of the run): config.first_tick <= next_tick(); its batches
    //     before next_tick() are skipped.
    // Recorded commands already in the loaded queue are not failures (CommandReplayer::already_queued).
    // False + *err and nothing changed when `rec` does not fit.
    bool replay_from_save(const CommandRecording* rec, const SavePoint& loaded, std::string* err = nullptr);
    bool replaying() const noexcept { return replay_ != nullptr; }
    size_t replay_failures() const noexcept { return replayer_ ? replayer_->failures() : 0; }
    size_t replay_already_queued() const noexcept { return replayer_ ? replayer_->already_queued() : 0; }

    // Local human orders (and tools that record streams). Refused with NotAllowed while replaying.
    SubmitResult submit(uint8_t player, CommandSource source, CommandPayload payload);
    // Network peers (arrival order). Refused with NotAllowed while replaying.
    SubmitResult receive(Command cmd);
    // AI / Lua direct path: runs the handler now for 	ick (the current tick). Returns the handler's
    // reason; non-scheduled types return NotScheduled without running.
    RejectReason execute_direct(uint64_t tick, uint8_t player, CommandSource source, const EventData& event);

    // Steps 1-5 above. Returns the drained batch (valid until the next begin_tick).
    const CommandBatch& begin_tick(uint64_t tick);

    // N1 save / load (contract: alamo/core/save_file.hpp). Saved: the scheduler (CommandScheduler::write:
    // config as retuned, now / next drain tick, queued commands for future ticks, per-player sequence and
    // drained-sequence state) and the event bus's undelivered events (EventBus::write). Not saved (host
    // wiring): handler, CRC hook, recording / replay attachment, the last drained batch (valid only until
    // the next begin_tick), event subscribers. Layout: u32 version | scheduler | event bus.
    // read() restores both or nothing (false, unchanged); like SimRng it does not require r.done().
    // After a load the next begin_tick must be scheduler().next_tick().
    static constexpr uint32_t kSaveVersion = 1;
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    CommandScheduler sched_;
    EventBus bus_;
    CommandHandler handler_;
    BatchCrcHook crc_hook_;
    CommandRecording* record_ = nullptr;
    const CommandRecording* replay_ = nullptr;
    std::unique_ptr<CommandReplayer> replayer_;
    CommandBatch last_;
    std::vector<uint8_t> scratch_;
};

}  // namespace alamo::commands
