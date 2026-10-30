// Conversations: scripted sequences of speech and SFX lines played one after another (hero clash
// exchanges, advisor briefings, unit-to-unit chatter scripted by the game). AUDIO.md §8 lists the
// original's SFX/speech conversation managers as not read, so the sequencing rules here are [port],
// built only from the documented primitives:
//   * a Speech line goes through the speech FIFO (AudioSystem::queue_speech, §3.1) and is finished when
//     its queue entry is gone; while it plays the usual speech rules apply (SFX capped at 0.3, HUD VO
//     refused, §3.2);
//   * an Sfx line starts its SFX event (2D UI, or attached to `owner`) through the normal gates (§1.2) and
//     is finished when the instance ends; a refused line is skipped;
//   * pause_after_seconds of wall time follow each line;
//   * one conversation plays at a time; the others wait, most important first (lower priority number,
//     like SFX Priority), then in start order; a conversation whose priority is strictly more important
//     than the playing one interrupts it (its current speech line is cancelled / SFX line stopped).
// Free of RNG and sim state. Not thread-safe (audio thread).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "alamo/audio/audio_system.hpp"

namespace alamo::audio {

struct ConversationLine {
    enum class Kind : uint8_t { Speech, Sfx };
    Kind kind = Kind::Speech;
    std::string event;               // SpeechEvent or SFXEvent name
    uint64_t owner = 0;              // Sfx: object to attach to (0 = 2D UI event)
    Vec3 position{};                 // Sfx with owner: initial position
    float pause_after_seconds = 0.0f;
};

struct Conversation {
    std::vector<ConversationLine> lines;
    int32_t priority = 3;  // 1 = most important
};

class ConversationPlayer {
public:
    explicit ConversationPlayer(AudioSystem& audio) : audio_(&audio) {}

    // Returns the conversation id (non-zero). An empty conversation finishes on the next update.
    uint32_t start(Conversation c);
    void cancel(uint32_t id);  // finished with completed = false
    void cancel_all();
    void update(double dt_seconds);  // after AudioSystem::update

    bool active(uint32_t id) const;   // playing or waiting
    uint32_t playing() const { return queue_.empty() || !started_ ? 0u : queue_.front().id; }
    size_t waiting() const { return queue_.empty() ? 0 : queue_.size() - (started_ ? 1u : 0u); }
    size_t current_line() const { return line_; }

    // (id, completed): completed = every line was reached (refused/unknown lines count as reached).
    void on_finished(std::function<void(uint32_t id, bool completed)> cb) { finished_.push_back(std::move(cb)); }

private:
    struct Entry {
        uint32_t id = 0;
        Conversation c;
    };
    void stop_line();
    void finish_front(bool completed);
    void insert(Entry e);

    AudioSystem* audio_;
    std::vector<Entry> queue_;  // [0] = playing (when started_), rest sorted by priority then id
    bool started_ = false;
    size_t line_ = 0;
    int line_state_ = 0;  // 0 start line, 1 waiting for it, 2 pause
    uint32_t speech_ticket_ = 0, sfx_handle_ = 0;
    double pause_ = 0.0;
    uint32_t next_id_ = 1;
    std::vector<std::function<void(uint32_t, bool)>> finished_;
};

}  // namespace alamo::audio
