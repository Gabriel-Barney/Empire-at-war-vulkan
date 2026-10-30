// GPU timestamps per frame in flight. One query pool per frame slot; scopes are begin/end pairs.
// Results of a slot are read without stalling once that slot's frame has completed, which
// FrameRing::begin_frame already guarantees when it hands the slot out again.
//
//   Frame* f = ring.begin_frame();
//   timer.begin_frame(f->cmd(), f->slot());      // reads the slot's previous results, resets the pool
//   uint32_t s = timer.begin_scope(f->cmd(), "opaque");
//   ... draws ...
//   timer.end_scope(f->cmd(), s);
//   for (auto& r : timer.results()) ...           // results of the last completed use of a slot
//
// Not thread-safe: record scopes from the thread that owns the command buffer passed in.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "alamo/rhi/vk.hpp"

namespace alamo::rhi {

class Context;

struct GpuTimerResult {
    std::string name;
    uint64_t begin_ticks = 0, end_ticks = 0;
    double ms = 0.0;
};

class GpuTimer {
public:
    GpuTimer() = default;
    ~GpuTimer();
    GpuTimer(const GpuTimer&) = delete;
    GpuTimer& operator=(const GpuTimer&) = delete;

    // False (and the timer stays inert: scopes are no-ops) when the graphics queue has no timestamps.
    bool init(Context& ctx, uint32_t frames_in_flight, uint32_t max_scopes = 64);
    void shutdown();
    bool enabled() const { return !slots_.empty(); }

    // Start of the slot's first command buffer, outside rendering. Collects the slot's previous
    // results first; the caller guarantees that frame has completed (FrameRing::begin_frame does).
    void begin_frame(VkCommandBuffer cmd, uint32_t slot);
    uint32_t begin_scope(VkCommandBuffer cmd, const char* name,
                         VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
    void end_scope(VkCommandBuffer cmd, uint32_t scope,
                   VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    // Reads the slot now (non-blocking). False if the queries are not available yet.
    bool collect(uint32_t slot);

    const std::vector<GpuTimerResult>& results() const { return results_; }

private:
    struct Slot {
        VkQueryPool pool = VK_NULL_HANDLE;
        std::vector<std::string> names;  // one per scope recorded this use
        bool pending = false;
    };
    Context* ctx_ = nullptr;
    std::vector<Slot> slots_;
    uint32_t max_scopes_ = 0;
    uint32_t current_ = UINT32_MAX;
    uint64_t mask_ = ~0ull;
    double ns_per_tick_ = 1.0;
    std::vector<GpuTimerResult> results_;
};

}  // namespace alamo::rhi
