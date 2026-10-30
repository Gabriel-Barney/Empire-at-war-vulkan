#include "alamo/commands/events.hpp"

#include <algorithm>

namespace alamo::commands {

std::string_view reject_reason_name(RejectReason r) noexcept {
    switch (r) {
        case RejectReason::None: return "accepted";
        case RejectReason::InvalidPlayer: return "invalid player";
        case RejectReason::Late: return "late";
        case RejectReason::Duplicate: return "duplicate";
        case RejectReason::Malformed: return "malformed";
        case RejectReason::NotOwner: return "not owner";
        case RejectReason::InvalidUnit: return "invalid unit";
        case RejectReason::InvalidTarget: return "invalid target";
        case RejectReason::InvalidPosition: return "invalid position";
        case RejectReason::UnknownAbility: return "unknown ability";
        case RejectReason::AbilityNotReady: return "ability not ready";
        case RejectReason::CannotAfford: return "cannot afford";
        case RejectReason::PrerequisitesUnmet: return "prerequisites unmet";
        case RejectReason::QueueFull: return "queue full";
        case RejectReason::NotAllowed: return "not allowed";
        case RejectReason::Unhandled: return "unhandled";
        case RejectReason::NotScheduled: return "not scheduled";
        case RejectReason::FirstCustom: break;
    }
    return static_cast<uint16_t>(r) >= static_cast<uint16_t>(RejectReason::FirstCustom) ? "custom" : "unknown";
}

EventBus::SubscriberId EventBus::subscribe(Subscriber fn) {
    const SubscriberId id = next_id_++;
    subs_.push_back(Sub{id, std::move(fn)});
    return id;
}

void EventBus::unsubscribe(SubscriberId id) {
    subs_.erase(std::remove_if(subs_.begin(), subs_.end(), [id](const Sub& s) { return s.id == id; }), subs_.end());
}

void EventBus::open_lanes(size_t n) {
    lanes_.clear();
    lanes_.resize(n);
}

void EventBus::merge_lanes() {
    for (auto& l : lanes_) {
        for (auto& e : l.events_) queue_.events_.push_back(std::move(e));
    }
    lanes_.clear();
}

// N1 layout (v1): u32 version | u32 n, per event: u8 kind (0 CommandResult, 1 SimEvent), then
//   CommandResult: u64 tick | u8 player | u8 source | u32 sequence | u64 issue_tick | u64 scheduled_tick
//                  | u16 type | u16 reason
//   SimEvent:      u64 tick | u32 kind | u64 subject | u64 other | i64 value
void EventBus::write(ByteWriter& w) const {
    const auto evs = queue_.events();
    w.u32(kSaveVersion).u32(static_cast<uint32_t>(evs.size()));
    for (const Event& e : evs) {
        if (const auto* c = std::get_if<CommandResult>(&e)) {
            w.u8(0).u64(c->tick).u8(c->header.player).u8(static_cast<uint8_t>(c->header.source));
            w.u32(c->header.sequence).u64(c->header.issue_tick).u64(c->header.scheduled_tick);
            w.u16(c->type).u16(static_cast<uint16_t>(c->reason));
        } else {
            const SimEvent& s = std::get<SimEvent>(e);
            w.u8(1).u64(s.tick).u32(s.kind).u64(s.subject).u64(s.other).i64(s.value);
        }
    }
}

bool EventBus::read(ByteReader& r) {
    if (r.u32() != kSaveVersion || !r.ok()) return false;
    const uint32_t n = r.u32();
    if (!r.ok() || n > r.remaining() / 33) return false;  // smallest event: 1 + 32 bytes
    std::vector<Event> evs;
    evs.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t kind = r.u8();
        if (kind == 0) {
            CommandResult c;
            c.tick = r.u64();
            c.header.player = r.u8();
            c.header.source = static_cast<CommandSource>(r.u8());  // diagnostic: kept as posted
            c.header.sequence = r.u32();
            c.header.issue_tick = r.u64();
            c.header.scheduled_tick = r.u64();
            c.type = r.u16();
            c.reason = static_cast<RejectReason>(r.u16());
            evs.emplace_back(c);
        } else if (kind == 1) {
            SimEvent s;
            s.tick = r.u64();
            s.kind = r.u32();
            s.subject = r.u64();
            s.other = r.u64();
            s.value = r.i64();
            evs.emplace_back(s);
        } else {
            return false;
        }
        if (!r.ok()) return false;
    }
    queue_.events_ = std::move(evs);
    return true;
}

void EventBus::deliver() {
    // Swap out first: events posted by subscribers go to the next deliver().
    std::vector<Event> batch;
    batch.swap(queue_.events_);
    const std::vector<Sub> subs = subs_;  // stable against (un)subscribe during delivery
    for (const Event& e : batch)
        for (const Sub& s : subs) s.fn(e);
}

}  // namespace alamo::commands
