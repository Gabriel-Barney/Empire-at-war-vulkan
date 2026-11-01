#include "alamo/commands/recording.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include "alamo/core/bytes.hpp"
#include "wire.hpp"

namespace alamo::commands {
namespace {

bool fail(std::string* err, std::string msg) {
    if (err) *err = std::move(msg);
    return false;
}

void put_str(std::vector<uint8_t>& out, const std::string& s) {
    wire::put(out, static_cast<uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

bool get_str(wire::Reader& r, std::string& s) {
    const uint32_t n = r.get<uint32_t>();
    if (!r.ok() || r.remaining() < n) return false;
    const auto d = r.data().subspan(r.pos(), n);
    s.assign(d.begin(), d.end());
    return r.skip(n);
}

}  // namespace

uint64_t save_digest(std::span<const uint8_t> encoded_save) noexcept {
    if (encoded_save.size() < 8) return 0;
    wire::Reader r(encoded_save.last(8));
    return r.get<uint64_t>();
}

void CommandRecording::append(const CommandBatch& b) {
    if (b.commands.empty()) return;
    if (!batches.empty() && b.tick <= batches.back().tick)
        throw std::invalid_argument("CommandRecording::append: ticks must increase");
    batches.push_back(b);
}

const CommandBatch* CommandRecording::find(uint64_t tick) const noexcept {
    const auto it = std::lower_bound(batches.begin(), batches.end(), tick,
                                     [](const CommandBatch& b, uint64_t t) { return b.tick < t; });
    return (it != batches.end() && it->tick == tick) ? &*it : nullptr;
}

size_t CommandRecording::command_count() const noexcept {
    size_t n = 0;
    for (const auto& b : batches) n += b.commands.size();
    return n;
}

std::vector<uint8_t> CommandRecording::to_bytes() const {
    std::vector<uint8_t> out = {'A', 'C', 'M', 'D'};
    wire::put(out, kWireVersion);
    wire::put(out, from_save ? kStreamFromSave : uint16_t{0});
    wire::put(out, config.delay);
    wire::put(out, config.max_players);
    wire::put(out, static_cast<uint8_t>(config.late));
    wire::put(out, static_cast<uint8_t>(config.mode));
    wire::put(out, uint8_t{0});
    wire::put(out, config.turn);
    wire::put(out, config.max_ahead);
    wire::put(out, seed);
    wire::put(out, config.first_tick);
    wire::put(out, end_tick);
    if (from_save) {
        wire::put(out, from_save->tick);
        wire::put(out, from_save->digest);
    }
    wire::put(out, static_cast<uint32_t>(meta.size()));
    for (const auto& [k, v] : meta) {
        put_str(out, k);
        put_str(out, v);
    }
    wire::put(out, static_cast<uint32_t>(batches.size()));
    for (const auto& b : batches) encode_batch(b, out);
    return out;
}

bool CommandRecording::from_bytes(std::span<const uint8_t> bytes, CommandRecording& out, std::string* err) {
    CommandRecording rec;
    wire::Reader r(bytes);
    if (bytes.size() < 4 || bytes[0] != 'A' || bytes[1] != 'C' || bytes[2] != 'M' || bytes[3] != 'D')
        return fail(err, "not a command stream (magic)");
    r.skip(4);
    const uint16_t version = r.get<uint16_t>();
    const uint16_t flags = r.get<uint16_t>();
    if (!r.ok()) return fail(err, "truncated header");
    if (version != kWireVersion) return fail(err, "unsupported command stream version " + std::to_string(version));
    if ((flags & ~kStreamFromSave) != 0) return fail(err, "unknown stream flags");
    rec.config.delay = r.get<uint32_t>();
    rec.config.max_players = r.get<uint8_t>();
    const uint8_t late = r.get<uint8_t>();
    const uint8_t mode = r.get<uint8_t>();
    r.get<uint8_t>();
    rec.config.turn = r.get<uint32_t>();
    rec.config.max_ahead = r.get<uint32_t>();
    rec.seed = r.get<uint64_t>();
    rec.config.first_tick = r.get<uint64_t>();
    rec.end_tick = r.get<uint64_t>();
    if (flags & kStreamFromSave) {
        SavePoint sp;
        sp.tick = r.get<uint64_t>();
        sp.digest = r.get<uint64_t>();
        rec.from_save = sp;
    }
    const uint32_t nmeta = r.get<uint32_t>();
    if (!r.ok()) return fail(err, "truncated header");
    if (rec.config.delay < 1 || rec.config.max_players < 1 || rec.config.max_players > kMaxPlayerSlots || late > 1 ||
        mode > 1 || rec.config.turn < 1)
        return fail(err, "bad scheduler config in header");
    rec.config.late = static_cast<LatePolicy>(late);
    rec.config.mode = static_cast<ScheduleMode>(mode);
    for (uint32_t i = 0; i < nmeta; ++i) {
        std::string k, v;
        if (!get_str(r, k) || !get_str(r, v)) return fail(err, "truncated meta");
        rec.meta.emplace_back(std::move(k), std::move(v));
    }
    const uint32_t nb = r.get<uint32_t>();
    if (!r.ok()) return fail(err, "truncated batch count");
    size_t pos = r.pos();
    for (uint32_t i = 0; i < nb; ++i) {
        CommandBatch b;
        const DecodeError e = decode_batch(bytes, pos, b);
        if (e != DecodeError::None)
            return fail(err, "batch " + std::to_string(i) + ": " + std::string(decode_error_name(e)));
        if (b.commands.empty() || (!rec.batches.empty() && b.tick <= rec.batches.back().tick))
            return fail(err, "batch " + std::to_string(i) + ": empty or tick not increasing");
        for (size_t j = 1; j < b.commands.size(); ++j)
            if (!drain_order_less(b.commands[j - 1].header, b.commands[j].header))
                return fail(err, "batch " + std::to_string(i) + ": commands not in drain order");
        rec.batches.push_back(std::move(b));
    }
    if (pos != bytes.size()) return fail(err, "trailing bytes after last batch");
    out = std::move(rec);
    return true;
}

bool CommandRecording::save(const std::string& path, std::string* err) const {
    const auto bytes = to_bytes();
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return fail(err, "cannot write " + path);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!f) return fail(err, "write failed: " + path);
    return true;
}

bool CommandRecording::load(const std::string& path, CommandRecording& out, std::string* err) {
    std::vector<uint8_t> bytes;
    if (!read_file_bytes(path, bytes, err)) return false;
    return from_bytes(bytes, out, err);
}

size_t CommandReplayer::feed(CommandScheduler& sched, uint64_t tick) {
    const auto& bs = rec_->batches;
    while (next_ < bs.size() && bs[next_].tick < tick) ++next_;  // skipped ticks: nothing to feed
    if (next_ >= bs.size() || bs[next_].tick != tick) return 0;
    size_t n = 0;
    for (const Command& c : bs[next_].commands) {
        const SubmitResult res = sched.insert(c);
        if (res.ok())
            ++n;
        else if (res.status == RejectReason::Duplicate && sched.is_queued(c))
            ++already_queued_;
        else
            ++failures_;
    }
    ++next_;
    return n;
}

}  // namespace alamo::commands
