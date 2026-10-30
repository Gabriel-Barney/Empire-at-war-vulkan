// MoviePlayer: playback clock, frame pacing, decode-ahead and the thread-safe audio FIFO.
#include <algorithm>
#include <cstring>

#include "alamo/video/video.hpp"

namespace alamo::video {
namespace {
constexpr size_t kMaxAhead = 8;           // decoded video frames held ahead of the clock
constexpr double kAudioLead_s = 0.5;      // keep at least this much audio queued while frames allow
constexpr double kAudioWait_s = 0.5;      // Audio clock: wait this long for the device before using dt
}  // namespace

MoviePlayer::MoviePlayer(std::unique_ptr<BinkDecoder> decoder, Clock clock) : dec_(std::move(decoder)), clock_(clock) {
    if (!dec_) {
        finished_ = decoder_done_ = true;
        return;
    }
    channels_ = dec_->info().sample_rate > 0 ? dec_->info().channels : 0;
    if (channels_ <= 0) clock_ = Clock::Wall;
    fill_();
}

MoviePlayer::~MoviePlayer() = default;

double MoviePlayer::due_s_(int64_t global_index) const {
    const VideoInfo& vi = dec_->info();
    return static_cast<double>(global_index) * vi.fps_den / static_cast<double>(vi.fps_num);
}

void MoviePlayer::pump_audio_() {
    audio_tmp_.clear();
    if (dec_->take_audio(audio_tmp_) == 0) return;
    std::lock_guard<std::mutex> lk(audio_mx_);
    if (audio_head_ > 0 && audio_head_ * 2 >= audio_q_.size()) {
        audio_q_.erase(audio_q_.begin(), audio_q_.begin() + static_cast<std::ptrdiff_t>(audio_head_));
        audio_head_ = 0;
    }
    audio_q_.insert(audio_q_.end(), audio_tmp_.begin(), audio_tmp_.end());
}

bool MoviePlayer::decode_one_() {
    VideoFrame f;
    if (!pool_.empty()) {
        f = std::move(pool_.back());
        pool_.pop_back();
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (dec_->next_frame(f)) {
            pump_audio_();
            const int64_t global = pass_base_ + f.index;
            pass_frames_ = std::max(pass_frames_, f.index + 1);
            f.pts_s = due_s_(global);
            last_global_ = global;
            pending_.push_back(std::move(f));
            return true;
        }
        pump_audio_();
        if (!loop_ || pass_frames_ == 0 || !dec_->rewind()) break;
        pass_base_ += pass_frames_;
        pass_frames_ = 0;
    }
    pool_.push_back(std::move(f));
    decoder_done_ = true;
    return false;
}

size_t MoviePlayer::audio_queued_frames() const {
    const int ch = channels_;
    if (ch <= 0) return 0;
    std::lock_guard<std::mutex> lk(audio_mx_);
    return (audio_q_.size() - audio_head_) / static_cast<size_t>(ch);
}

void MoviePlayer::fill_() {
    const VideoInfo& vi = dec_->info();
    const size_t audio_target =
        vi.sample_rate > 0 ? static_cast<size_t>(kAudioLead_s * vi.sample_rate) : 0;
    while (!decoder_done_) {
        // Always hold one decoded frame; decode further ahead only to keep the audio fed.
        const bool need_due = pending_.empty();
        const bool audio_low = audio_target > 0 && audio_queued_frames() < audio_target;
        if (!need_due && !(audio_low && pending_.size() < kMaxAhead)) break;
        decode_one_();
    }
}

bool MoviePlayer::update(double dt_s) {
    if (!dec_ || finished_ || paused_) return false;
    const VideoInfo& vi = dec_->info();
    if (dt_s < 0.0) dt_s = 0.0;
    if (clock_ == Clock::Audio) {
        const int64_t consumed = audio_consumed_.load(std::memory_order_acquire);
        const bool audio_left = !decoder_done_ || audio_queued_frames() > 0;
        if (consumed > 0 && audio_left && !audio_fallback_) {
            time_ = std::max(time_, static_cast<double>(consumed) / vi.sample_rate - latency_s_);  // OI host: minus latency
        } else if (consumed == 0 && !audio_fallback_) {
            waited_s_ += dt_s;  // device not pulling yet: hold the first frame
            if (waited_s_ >= kAudioWait_s) audio_fallback_ = true;
        } else {
            time_ += dt_s;
        }
    } else {
        time_ += dt_s;
    }
    bool changed = false;
    for (fill_(); !pending_.empty() && pending_.front().pts_s <= time_; fill_()) {
        if (changed) ++dropped_;  // the previous candidate was never shown
        if (have_frame_) pool_.push_back(std::move(cur_));
        cur_ = std::move(pending_.front());
        pending_.pop_front();
        have_frame_ = true;
        changed = true;
    }
    if (changed) ++serial_;
    if (decoder_done_ && pending_.empty() && have_frame_ && time_ >= due_s_(last_global_ + 1)) finished_ = true;
    if (decoder_done_ && !have_frame_ && pending_.empty()) finished_ = true;
    return changed;
}

const VideoFrame* MoviePlayer::current_frame() const { return have_frame_ ? &cur_ : nullptr; }
uint64_t MoviePlayer::frame_serial() const { return serial_; }
double MoviePlayer::time_s() const { return time_; }
bool MoviePlayer::finished() const { return finished_; }

void MoviePlayer::set_paused(bool paused) {
    paused_ = paused;
    audio_paused_.store(paused, std::memory_order_release);
}

bool MoviePlayer::paused() const { return paused_; }
void MoviePlayer::set_loop(bool loop) { loop_ = loop; }

void MoviePlayer::stop() {
    finished_ = true;
    std::lock_guard<std::mutex> lk(audio_mx_);
    audio_q_.clear();
    audio_head_ = 0;
}

const VideoInfo& MoviePlayer::info() const {
    static const VideoInfo kEmpty{};
    return dec_ ? dec_->info() : kEmpty;
}

const BinkDecoder& MoviePlayer::decoder() const { return *dec_; }
int64_t MoviePlayer::frames_dropped() const { return dropped_; }

size_t MoviePlayer::read_audio(float* out, size_t frames) {
    if (audio_paused_.load(std::memory_order_acquire)) return 0;
    const int ch = channels_;
    if (ch <= 0) return 0;
    std::lock_guard<std::mutex> lk(audio_mx_);
    const size_t avail = (audio_q_.size() - audio_head_) / static_cast<size_t>(ch);
    const size_t n = std::min(avail, frames);
    if (n == 0) return 0;
    std::memcpy(out, audio_q_.data() + audio_head_, n * static_cast<size_t>(ch) * sizeof(float));
    audio_head_ += n * static_cast<size_t>(ch);
    audio_consumed_.fetch_add(static_cast<int64_t>(n), std::memory_order_acq_rel);
    return n;
}

}  // namespace alamo::video
