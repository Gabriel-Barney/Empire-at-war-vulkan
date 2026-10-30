// G0-3 command recording: the drained per-tick batches of a run, written to a binary stream file and
// replayed into a scheduler. Together with the initial state + seed this reproduces the run exactly.
//
// The original's replay is the same idea: seed + settings + players + the scheduled command stream
// (COMMANDS.md §7.3). AI/Lua orders are not in it: they run identically on replay.
//
// STREAM FILE v2 (little-endian, no padding; v1 = stage-1 typed orders, no longer readable):
//   u8[4] magic "ACMD" | u16 version (= kWireVersion) | u16 flags (0, or kStreamFromSave)
//   u32 delay | u8 max_players | u8 late_policy | u8 mode | u8 reserved (0) | u32 turn | u32 max_ahead
//   u64 seed | u64 first_tick | u64 end_tick (first tick NOT covered; 0 = unknown)
//   [flags & kStreamFromSave: u64 save_tick | u64 save_digest]           (N1: the stream continues a save)
//   u32 meta_count | meta_count x (u32 key_len | key | u32 value_len | value)
//   u32 batch_count | batch[batch_count]          (scheduler.hpp encode_batch; ticks strictly increasing)
// Only non-empty batches are stored; a tick without a batch had no commands.
//
// REPLAY FROM A SAVE (N1). A stream recorded after loading a save (CommandPipeline::record_to with the
// save's SavePoint) starts at the first tick after the save (config.first_tick = the loaded scheduler's
// next_tick()) and names the save it continues (from_save); its config is the loaded scheduler's. To
// replay it: rebuild the session, load the save (CommandPipeline::read included, NOT a new pipeline from
// rec.config), then CommandPipeline::replay_from_save(rec, save point) and run from next_tick(). A full
// stream (from_save unset, e.g. a tick-0 recording of the same run) can be replayed from a mid-run save
// the same way: its batches before next_tick() are skipped. Streams without a save point are written
// with flags 0, byte-identical to before the extension; older readers reject kStreamFromSave streams
// ("unknown stream flags").
#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "alamo/commands/scheduler.hpp"

namespace alamo::commands {

inline constexpr uint16_t kStreamFromSave = 0x0001;  // header flag: save_tick + save_digest follow end_tick

// The save a stream continues. `tick` is SaveFile::tick; `digest` identifies the save's content - by
// convention save_digest() of the encoded save file (SaveFile::encode() / the file's bytes).
struct SavePoint {
    uint64_t tick = 0;
    uint64_t digest = 0;
    friend bool operator==(const SavePoint&, const SavePoint&) = default;
};
// The trailing u64 of an encoded save file (save_file.hpp: the StateHasher digest of every byte before
// it, so it covers tick, description and every chunk). 0 when `encoded_save` is shorter than 8 bytes.
uint64_t save_digest(std::span<const uint8_t> encoded_save) noexcept;

struct CommandRecording {
    SchedulerConfig config;  // scheduler settings + first_tick of the recorded run (max_ahead: initial)
    uint64_t seed = 0;       // sim seed (owned by the host; stored for convenience)
    uint64_t end_tick = 0;   // first tick not covered by the recording (0 = unknown)
    std::optional<SavePoint> from_save;  // N1: set = the stream starts from this save, not from tick 0
    std::vector<std::pair<std::string, std::string>> meta;
    std::vector<CommandBatch> batches;  // non-empty, tick strictly increasing

    // Appends a drained batch (empty batches are skipped). Throws std::invalid_argument if the tick
    // does not increase.
    void append(const CommandBatch& b);
    // Batch for `tick` or nullptr (binary search).
    const CommandBatch* find(uint64_t tick) const noexcept;
    size_t command_count() const noexcept;

    std::vector<uint8_t> to_bytes() const;
    static bool from_bytes(std::span<const uint8_t> bytes, CommandRecording& out, std::string* err = nullptr);
    bool save(const std::string& path, std::string* err = nullptr) const;
    static bool load(const std::string& path, CommandRecording& out, std::string* err = nullptr);

    friend bool operator==(const CommandRecording& a, const CommandRecording& b) {
        return a.config.delay == b.config.delay && a.config.max_players == b.config.max_players &&
               a.config.late == b.config.late && a.config.first_tick == b.config.first_tick &&
               a.config.mode == b.config.mode && a.config.turn == b.config.turn &&
               a.config.max_ahead == b.config.max_ahead && a.seed == b.seed &&
               a.end_tick == b.end_tick && a.from_save == b.from_save && a.meta == b.meta && a.batches == b.batches;
    }
};

// Feeds a recording into a scheduler, tick by tick. Call feed(sched, T) right before
// sched.drain(T): it inserts the recorded batch of T with its original headers, so the drained batch
// is byte-identical to the recorded one.
class CommandReplayer {
public:
    explicit CommandReplayer(const CommandRecording& rec) noexcept : rec_(&rec) {}
    // Returns the number of commands inserted; any insert failure (which means the scheduler was not
    // set up from rec.config or received live commands) is counted in failures(). A recorded command
    // that is already queued, identical, at its tick (the queue of a loaded save) is not inserted again
    // and not a failure: it is counted in already_queued().
    size_t feed(CommandScheduler& sched, uint64_t tick);
    size_t failures() const noexcept { return failures_; }
    size_t already_queued() const noexcept { return already_queued_; }

private:
    const CommandRecording* rec_;
    size_t next_ = 0;  // cursor into rec_->batches
    size_t failures_ = 0;
    size_t already_queued_ = 0;
};

}  // namespace alamo::commands
