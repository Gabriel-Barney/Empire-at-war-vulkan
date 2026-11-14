// G0-3 event bus: results of commands (accepted / rejected with a reason) and a generic sim-event
// channel, delivered to subscribers in a deterministic order.
//
// The original reports nothing back: an invalid command is a silent no-op at execution (COMMANDS.md
// §6.2). CommandResult is a port DIAGNOSTIC channel (UI feedback, logs, tests) - sim code must never
// branch on it, and it is never recorded.
//
// ORDER RULE: events are delivered in post() call order (the command-apply phase is serial, in drain
// order); subscribers are called in subscription order for each event (event-major). post() is not
// thread-safe. Events posted from inside a subscriber are queued and delivered by the next deliver().
#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include "alamo/commands/command.hpp"
#include "alamo/core/bytes.hpp"

namespace alamo::commands {

// Why a command was not applied. Values are part of replays/CRCs: append only.
enum class RejectReason : uint16_t {
    None = 0,            // accepted
    InvalidPlayer = 1,   // player slot out of range / not in the game
    Late = 2,            // scheduled tick already drained (submit-time)
    Duplicate = 3,       // (player, sequence) already queued or drained (submit-time)
    Malformed = 4,       // payload not encodable / unknown type
    NotOwner = 5,        // a unit/producer does not belong to the issuing player
    InvalidUnit = 6,     // handle dead or wrong kind
    InvalidTarget = 7,
    InvalidPosition = 8,
    UnknownAbility = 9,
    AbilityNotReady = 10,
    CannotAfford = 11,
    PrerequisitesUnmet = 12,
    QueueFull = 13,
    NotAllowed = 14,     // rules forbid it now (story lock, wrong game mode ...)
    Unhandled = 15,      // no handler installed for the type
    NotScheduled = 16,   // non-scheduled event type (FrameInfo/Chat/PerformanceMetrics/Taunt) offered to the queue
    FirstCustom = 0x100, // game-specific reasons
};
std::string_view reject_reason_name(RejectReason r) noexcept;

struct CommandResult {
    uint64_t tick = 0;  // tick the command was applied (drained) in
    CommandHeader header;
    uint16_t type = 0;
    RejectReason reason = RejectReason::None;
    bool accepted() const noexcept { return reason == RejectReason::None; }
    friend bool operator==(const CommandResult&, const CommandResult&) = default;
};

// Generic sim event: `kind` is owned by the posting system (e.g. name_crc of an event name).
struct SimEvent {
    uint64_t tick = 0;
    uint32_t kind = 0;
    uint64_t subject = 0;  // object id / handle, owned by the poster
    uint64_t other = 0;
    int64_t value = 0;
    friend bool operator==(const SimEvent&, const SimEvent&) = default;
};

using Event = std::variant<CommandResult, SimEvent>;

class EventQueue {
public:
    void post(Event e) { events_.push_back(std::move(e)); }
    std::span<const Event> events() const noexcept { return events_; }
    void clear() noexcept { events_.clear(); }

private:
    friend class EventBus;
    std::vector<Event> events_;
};

class EventBus {
public:
    using Subscriber = std::function<void(const Event&)>;
    using SubscriberId = uint32_t;

    SubscriberId subscribe(Subscriber fn);
    void unsubscribe(SubscriberId id);

    void post(Event e) { queue_.post(std::move(e)); }

    // Events waiting for deliver(), in delivery order.
    std::span<const Event> pending() const noexcept { return queue_.events(); }
    // Delivers every pending event (event-major, subscription order), then clears them.
    void deliver();

    // N1 save / load (alamo/core/save_file.hpp contract; no r.done() check, the caller does that):
    // the events pending delivery, in order. Subscribers (host wiring) are not saved. read() replaces
    // the pending events; false and nothing changed on a bad version / event kind or truncated data.
    static constexpr uint32_t kSaveVersion = 1;
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    struct Sub {
        SubscriberId id;
        Subscriber fn;
    };
    EventQueue queue_;
    std::vector<Sub> subs_;
    SubscriberId next_id_ = 1;
};

}  // namespace alamo::commands
