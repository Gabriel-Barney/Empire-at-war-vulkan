// Conversation sequencer (see alamo/audio/conversation.hpp).
#include "alamo/audio/conversation.hpp"

#include <algorithm>

namespace alamo::audio {

void ConversationPlayer::insert(Entry e) {
    // Waiting entries stay sorted by (priority, id); the playing front entry is never moved here.
    const size_t first_waiting = started_ ? 1u : 0u;
    auto it = std::find_if(queue_.begin() + static_cast<std::ptrdiff_t>(first_waiting), queue_.end(),
                           [&](const Entry& o) { return o.c.priority > e.c.priority; });
    queue_.insert(it, std::move(e));
}

uint32_t ConversationPlayer::start(Conversation c) {
    Entry e;
    e.id = next_id_++;
    if (next_id_ == 0) next_id_ = 1;
    e.c = std::move(c);
    const uint32_t id = e.id;
    if (started_ && !queue_.empty() && e.c.priority < queue_.front().c.priority) {
        // Interrupt: the playing conversation ends unfinished, the new one plays next update.
        stop_line();
        finish_front(false);
        queue_.insert(queue_.begin(), std::move(e));
        return id;
    }
    insert(std::move(e));
    return id;
}

void ConversationPlayer::stop_line() {
    if (speech_ticket_ != 0) audio_->cancel_speech(speech_ticket_);
    if (sfx_handle_ != 0) audio_->stop_sfx(sfx_handle_, 0.0f);
    speech_ticket_ = 0;
    sfx_handle_ = 0;
}

void ConversationPlayer::finish_front(bool completed) {
    if (queue_.empty()) return;
    const uint32_t id = queue_.front().id;
    queue_.erase(queue_.begin());
    started_ = false;
    line_ = 0;
    line_state_ = 0;
    pause_ = 0.0;
    for (auto& cb : finished_) cb(id, completed);
}

void ConversationPlayer::cancel(uint32_t id) {
    for (size_t i = 0; i < queue_.size(); ++i) {
        if (queue_[i].id != id) continue;
        if (i == 0 && started_) {
            stop_line();
            finish_front(false);
        } else {
            queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(i));
            for (auto& cb : finished_) cb(id, false);
        }
        return;
    }
}

void ConversationPlayer::cancel_all() {
    while (!queue_.empty()) cancel(queue_.front().id);
}

bool ConversationPlayer::active(uint32_t id) const {
    for (const Entry& e : queue_)
        if (e.id == id) return true;
    return false;
}

void ConversationPlayer::update(double dt_seconds) {
    for (int guard = 0; guard < 64 && !queue_.empty(); ++guard) {
        started_ = true;
        Entry& e = queue_.front();
        if (line_ >= e.c.lines.size()) {
            finish_front(true);
            continue;
        }
        const ConversationLine& l = e.c.lines[line_];
        if (line_state_ == 0) {
            if (l.kind == ConversationLine::Kind::Speech) {
                speech_ticket_ = audio_->queue_speech(l.event);
            } else {
                SfxPlay p;
                if (l.owner != 0) {
                    p.kind = SfxPlay::Kind::Object;
                    p.owner = l.owner;
                    p.position = l.position;
                }
                sfx_handle_ = audio_->play_sfx(l.event, p);
            }
            line_state_ = 1;
            return;  // the line starts playing in this frame's audio update; check it next update
        }
        if (line_state_ == 1) {
            if (speech_ticket_ != 0 && audio_->speech_pending(speech_ticket_)) return;
            if (sfx_handle_ != 0 && audio_->sfx_live(sfx_handle_)) return;
            speech_ticket_ = 0;
            sfx_handle_ = 0;
            pause_ = static_cast<double>(l.pause_after_seconds);
            line_state_ = 2;
            if (pause_ > 0.0) return;  // the pause counts from the next update
        }
        if (line_state_ == 2) {
            pause_ -= dt_seconds;
            if (pause_ > 0.0) return;
            ++line_;
            line_state_ = 0;
        }
    }
}

}  // namespace alamo::audio
