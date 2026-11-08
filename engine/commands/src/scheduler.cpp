#include "alamo/commands/scheduler.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>

#include "wire.hpp"

namespace alamo::commands {

void encode_batch(const CommandBatch& b, std::vector<uint8_t>& out) {
    wire::put(out, b.tick);
    wire::put(out, static_cast<uint32_t>(b.commands.size()));
    for (const Command& c : b.commands) encode_command(c, out);
}

DecodeError decode_batch(std::span<const uint8_t> in, size_t& pos, CommandBatch& out) {
    wire::Reader r(in, pos);
    CommandBatch b;
    b.tick = r.get<uint64_t>();
    const uint32_t n = r.get<uint32_t>();
    if (!r.ok()) return DecodeError::Truncated;
    if (n > r.remaining() / kCommandHeaderBytes) return DecodeError::Truncated;
    size_t p = r.pos();
    b.commands.resize(n);
    for (auto& c : b.commands) {
        const DecodeError e = decode_command(in, p, c);
        if (e != DecodeError::None) return e;
        if (c.header.scheduled_tick != b.tick) return DecodeError::BadField;
    }
    pos = p;
    out = std::move(b);
    return DecodeError::None;
}

CommandScheduler::CommandScheduler(SchedulerConfig cfg) : cfg_(cfg) {
    if (cfg_.delay < 1) throw std::invalid_argument("CommandScheduler: delay must be >= 1");
    if (cfg_.max_players < 1 || cfg_.max_players > kMaxPlayerSlots)
        throw std::invalid_argument("CommandScheduler: max_players out of range");
    now_ = cfg_.first_tick;
    next_tick_ = cfg_.first_tick;
    next_seq_.assign(cfg_.max_players, 0);
    drained_low_.assign(cfg_.max_players, 0);
    drained_above_.assign(cfg_.max_players, {});
    queued_seq_.assign(cfg_.max_players, {});
}

SubmitResult CommandScheduler::submit(uint8_t player, CommandSource source, CommandPayload payload) {
    Command c;
    c.header.player = player;
    c.header.source = source;
    c.payload = std::move(payload);
    return check_and_queue(std::move(c), true);
}

SubmitResult CommandScheduler::insert(Command cmd) { return check_and_queue(std::move(cmd), false); }

SubmitResult CommandScheduler::check_and_queue(Command&& c, bool local) {
    SubmitResult res;
    if (c.header.player >= cfg_.max_players || static_cast<uint8_t>(c.header.source) >= kCommandSourceCount) {
        res.status = RejectReason::InvalidPlayer;
        return res;
    }
    if (!c.payload.def().scheduled) {
        res.status = RejectReason::NotScheduled;
        return res;
    }
    if (!c.payload.valid()) {
        res.status = RejectReason::Malformed;
        return res;
    }
    std::lock_guard lock(mu_);
    const uint8_t p = c.header.player;
    auto& qs = queued_seq_[p];
    if (local) {
        c.header.sequence = next_seq_[p]++;
        c.header.issue_tick = now_;
        c.header.scheduled_tick = now_ + cfg_.delay;
    } else {
        const uint32_t seq = c.header.sequence;
        if (was_drained(p, seq) || std::binary_search(qs.begin(), qs.end(), seq)) {
            res.status = RejectReason::Duplicate;
            return res;
        }
        if (c.header.scheduled_tick < next_tick_) {
            res.status = RejectReason::Late;
            return res;
        }
        if (seq >= next_seq_[p]) next_seq_[p] = seq + 1;
    }
    res.sequence = c.header.sequence;
    res.scheduled_tick = c.header.scheduled_tick;
    qs.insert(std::upper_bound(qs.begin(), qs.end(), c.header.sequence), c.header.sequence);
    queued_[c.header.scheduled_tick].push_back(std::move(c));
    return res;
}

CommandBatch CommandScheduler::drain(uint64_t tick) {
    std::lock_guard lock(mu_);
    if (tick != next_tick_) throw std::logic_error("CommandScheduler::drain: ticks must be drained in order, without gaps");
    CommandBatch b;
    b.tick = tick;
    if (auto it = queued_.find(tick); it != queued_.end()) {
        b.commands = std::move(it->second);
        queued_.erase(it);
    }
    // (player, sequence) is unique, so the order is total and std::sort is deterministic.
    std::sort(b.commands.begin(), b.commands.end(),
              [](const Command& a, const Command& c) { return drain_order_less(a.header, c.header); });
    for (const Command& c : b.commands) {
        const uint8_t p = c.header.player;
        auto& qs = queued_seq_[p];
        if (auto q = std::lower_bound(qs.begin(), qs.end(), c.header.sequence); q != qs.end() && *q == c.header.sequence)
            qs.erase(q);
        mark_drained(p, c.header.sequence);
    }
    now_ = tick;
    next_tick_ = tick + 1;
    return b;
}

bool CommandScheduler::was_drained(uint8_t p, uint32_t seq) const {
    const auto& above = drained_above_[p];
    return seq < drained_low_[p] || std::binary_search(above.begin(), above.end(), seq);
}

void CommandScheduler::mark_drained(uint8_t p, uint32_t seq) {
    auto& low = drained_low_[p];
    auto& above = drained_above_[p];
    if (seq < low) return;
    if (seq == low) {
        ++low;
        // Absorb the contiguous run that now follows the watermark.
        size_t k = 0;
        while (k < above.size() && above[k] == low) {
            ++low;
            ++k;
        }
        above.erase(above.begin(), above.begin() + static_cast<std::ptrdiff_t>(k));
        return;
    }
    above.insert(std::upper_bound(above.begin(), above.end(), seq), seq);
}

uint64_t CommandScheduler::now() const {
    std::lock_guard lock(mu_);
    return now_;
}
uint64_t CommandScheduler::next_tick() const {
    std::lock_guard lock(mu_);
    return next_tick_;
}
size_t CommandScheduler::pending() const {
    std::lock_guard lock(mu_);
    size_t n = 0;
    for (const auto& [t, v] : queued_) n += v.size();
    return n;
}

bool CommandScheduler::is_queued(const Command& c) const {
    std::lock_guard lock(mu_);
    const auto it = queued_.find(c.header.scheduled_tick);
    return it != queued_.end() && std::find(it->second.begin(), it->second.end(), c) != it->second.end();
}

// ---- N1 save / load ----------------------------------------------------------------------------------
//
// Layout (v1, ByteWriter):
//   u32 version | u32 delay | u8 max_players | u8 late | u8 mode | u32 turn | u32 max_ahead | u64 first_tick
//   (late / mode / turn / max_ahead: the retired lockstep bytes, always kSchedLateByte ... kSchedMaxAhead)
//   | u64 now | u64 next drain tick
//   per player (max_players): u32 next sequence | u32 drained watermark | u32 n + u32 drained above it
//   u32 queued ticks, per tick (ascending): u64 tick | u32 n, per command: u32 length + canonical bytes
//   (encode_command), in arrival order
// The per-player queued-sequence index is rebuilt from the queue.
void CommandScheduler::write(ByteWriter& w) const {
    std::lock_guard lock(mu_);
    w.u32(kSaveVersion).u32(cfg_.delay).u8(cfg_.max_players).u8(kSchedLateByte);
    w.u8(kSchedModeByte).u32(kSchedTurn).u32(kSchedMaxAhead).u64(cfg_.first_tick);
    w.u64(now_).u64(next_tick_);
    for (size_t p = 0; p < cfg_.max_players; ++p) {
        w.u32(next_seq_[p]).u32(drained_low_[p]).u32(static_cast<uint32_t>(drained_above_[p].size()));
        for (uint32_t q : drained_above_[p]) w.u32(q);
    }
    w.u32(static_cast<uint32_t>(queued_.size()));
    std::vector<uint8_t> bytes;
    for (const auto& [tick, cmds] : queued_) {
        w.u64(tick).u32(static_cast<uint32_t>(cmds.size()));
        for (const Command& c : cmds) {
            bytes.clear();
            encode_command(c, bytes);
            w.u32(static_cast<uint32_t>(bytes.size())).raw(bytes);
        }
    }
}

bool CommandScheduler::read(ByteReader& r) {
    if (r.u32() != kSaveVersion || !r.ok()) return false;
    SchedulerConfig cfg;
    cfg.delay = r.u32();
    cfg.max_players = r.u8();
    const uint8_t late = r.u8(), mode = r.u8();
    const uint32_t turn = r.u32(), max_ahead = r.u32();
    cfg.first_tick = r.u64();
    const uint64_t saved_now = r.u64(), saved_next = r.u64();
    if (!r.ok() || cfg.delay < 1 || cfg.max_players < 1 || cfg.max_players > kMaxPlayerSlots || late != kSchedLateByte ||
        mode != kSchedModeByte || turn != kSchedTurn || max_ahead != kSchedMaxAhead ||
        (saved_next != saved_now && saved_next != saved_now + 1) || saved_next < cfg.first_tick)
        return false;
    const size_t np = cfg.max_players;
    std::vector<uint32_t> next_seq(np), low(np);
    std::vector<std::vector<uint32_t>> above(np), queued_seq(np);
    for (size_t p = 0; p < np; ++p) {
        next_seq[p] = r.u32();
        low[p] = r.u32();
        const uint32_t n = r.u32();
        if (!r.ok() || n > r.remaining() / 4) return false;
        above[p].resize(n);
        for (uint32_t& q : above[p]) q = r.u32();
        // Strictly ascending, above the watermark (the run following it is always absorbed).
        for (size_t k = 0; k < n; ++k)
            if (above[p][k] <= (k ? above[p][k - 1] : low[p])) return false;
    }
    std::map<uint64_t, std::vector<Command>> queued;
    const uint32_t nticks = r.u32();
    if (!r.ok() || nticks > r.remaining() / 12) return false;
    uint64_t prev = 0;
    for (uint32_t i = 0; i < nticks; ++i) {
        const uint64_t tick = r.u64();
        const uint32_t n = r.u32();
        if (!r.ok() || n == 0 || n > r.remaining() / (4 + kCommandHeaderBytes) || tick < saved_next ||
            (i > 0 && tick <= prev))
            return false;
        prev = tick;
        std::vector<Command>& cmds = queued[tick];
        cmds.resize(n);
        for (Command& c : cmds) {
            const uint32_t len = r.u32();
            const std::span<const uint8_t> bytes = r.raw(len);
            size_t pos = 0;
            if (!r.ok() || decode_command(bytes, pos, c) != DecodeError::None || pos != len) return false;
            const uint8_t p = c.header.player;
            const uint32_t seq = c.header.sequence;
            if (c.header.scheduled_tick != tick || p >= np || !c.payload.def().scheduled || !c.payload.valid() ||
                seq >= next_seq[p] || seq < low[p] || std::binary_search(above[p].begin(), above[p].end(), seq))
                return false;
            queued_seq[p].push_back(seq);
        }
    }
    for (auto& qs : queued_seq) {
        std::sort(qs.begin(), qs.end());
        if (std::adjacent_find(qs.begin(), qs.end()) != qs.end()) return false;  // (player, sequence) is unique
    }
    std::lock_guard lock(mu_);
    cfg_ = cfg;
    now_ = saved_now;
    next_tick_ = saved_next;
    queued_ = std::move(queued);
    next_seq_ = std::move(next_seq);
    drained_low_ = std::move(low);
    drained_above_ = std::move(above);
    queued_seq_ = std::move(queued_seq);
    return true;
}

}  // namespace alamo::commands
