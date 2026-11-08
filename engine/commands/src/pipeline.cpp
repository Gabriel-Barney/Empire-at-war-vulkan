#include "alamo/commands/pipeline.hpp"

#include <string>
#include <utility>

namespace alamo::commands {

void CommandPipeline::record_to(CommandRecording* rec, std::optional<SavePoint> from_save) {
    record_ = rec;
    if (rec) {
        rec->config = sched_.config();
        rec->config.first_tick = sched_.next_tick();
        rec->from_save = from_save;
    }
}

void CommandPipeline::replay_from(const CommandRecording* rec) {
    replay_ = rec;
    replayer_ = rec ? std::make_unique<CommandReplayer>(*rec) : nullptr;
}

bool CommandPipeline::replay_from_save(const CommandRecording* rec, const SavePoint& loaded, std::string* err) {
    auto fail = [err](std::string msg) {
        if (err) *err = std::move(msg);
        return false;
    };
    if (!rec) return fail("no recording");
    const SchedulerConfig& c = sched_.config();
    const SchedulerConfig& rc = rec->config;
    if (rc.max_players != c.max_players) return fail("recording scheduler max_players differs from the loaded one");
    const uint64_t next = sched_.next_tick();
    if (rec->from_save) {
        if (!(*rec->from_save == loaded))
            return fail("recording continues save tick " + std::to_string(rec->from_save->tick) +
                        ", not the loaded save (tick " + std::to_string(loaded.tick) + ")");
        if (rc.first_tick != next)
            return fail("recording starts at tick " + std::to_string(rc.first_tick) + ", the loaded state at " +
                        std::to_string(next));
    } else if (rc.first_tick > next) {
        return fail("recording starts at tick " + std::to_string(rc.first_tick) + ", after the loaded state (" +
                    std::to_string(next) + ")");
    }
    replay_from(rec);
    return true;
}

void CommandPipeline::write(ByteWriter& w) const {
    w.u32(kSaveVersion);
    sched_.write(w);
    bus_.write(w);
}

bool CommandPipeline::read(ByteReader& r) {
    // Dry run on scratch copies first, so a bad event-bus part cannot leave a loaded scheduler behind.
    ByteReader probe = r;
    if (probe.u32() != kSaveVersion || !probe.ok()) return false;
    CommandScheduler sched;
    EventBus bus;
    if (!sched.read(probe) || !bus.read(probe) || !probe.ok()) return false;
    r.u32();
    const bool ok = sched_.read(r) && bus_.read(r);  // same bytes: cannot fail now
    return ok && r.ok();
}

SubmitResult CommandPipeline::submit(uint8_t player, CommandSource source, CommandPayload payload) {
    if (replay_) {
        SubmitResult r;
        r.status = RejectReason::NotAllowed;
        return r;
    }
    return sched_.submit(player, source, std::move(payload));
}

RejectReason CommandPipeline::execute_direct(uint64_t tick, uint8_t player, CommandSource source, const EventData& event) {
    Command c;
    c.header.player = player;
    c.header.source = source;
    c.header.issue_tick = tick;
    c.header.scheduled_tick = tick;
    c.payload = event;
    CommandResult r;
    r.tick = tick;
    r.header = c.header;
    r.type = c.payload.type_id();
    if (!event.def().scheduled)
        r.reason = RejectReason::NotScheduled;
    else if (!event.valid())
        r.reason = RejectReason::Malformed;
    else
        r.reason = handler_ ? handler_(tick, c) : RejectReason::Unhandled;
    bus_.post(r);
    return r.reason;
}

const CommandBatch& CommandPipeline::begin_tick(uint64_t tick) {
    if (replayer_) replayer_->feed(sched_, tick);
    last_ = sched_.drain(tick);
    if (crc_hook_) {
        scratch_.clear();
        encode_batch(last_, scratch_);
        crc_hook_(scratch_.data(), scratch_.size());
    }
    if (record_) {
        record_->append(last_);
        record_->end_tick = tick + 1;
    }
    for (const Command& c : last_.commands) {
        CommandResult r;
        r.tick = tick;
        r.header = c.header;
        r.type = c.payload.type_id();
        r.reason = handler_ ? handler_(tick, c) : RejectReason::Unhandled;
        bus_.post(r);
    }
    return last_;
}

}  // namespace alamo::commands
