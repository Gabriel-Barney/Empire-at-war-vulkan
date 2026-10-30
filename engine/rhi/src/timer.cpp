#include "alamo/rhi/timer.hpp"

#include "alamo/rhi/context.hpp"

namespace alamo::rhi {

GpuTimer::~GpuTimer() { shutdown(); }

bool GpuTimer::init(Context& ctx, uint32_t frames_in_flight, uint32_t max_scopes) {
    shutdown();
    ctx_ = &ctx;
    const uint32_t bits = ctx.caps().timestamp_valid_bits;
    if (!bits || !frames_in_flight || !max_scopes) return false;
    mask_ = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
    ns_per_tick_ = static_cast<double>(ctx.caps().timestamp_period_ns);
    max_scopes_ = max_scopes;
    slots_.resize(frames_in_flight);
    for (Slot& s : slots_) {
        auto ci = vk_struct<VkQueryPoolCreateInfo>(VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);
        ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        ci.queryCount = max_scopes * 2;
        if (!vk_check(ctx.vkd().vkCreateQueryPool(ctx.device(), &ci, nullptr, &s.pool), "vkCreateQueryPool")) {
            s.pool = VK_NULL_HANDLE;
            shutdown();
            return false;
        }
    }
    return true;
}

void GpuTimer::shutdown() {
    if (ctx_)
        for (Slot& s : slots_)
            if (s.pool) ctx_->vkd().vkDestroyQueryPool(ctx_->device(), s.pool, nullptr);
    slots_.clear();
    results_.clear();
    current_ = UINT32_MAX;
}

bool GpuTimer::collect(uint32_t slot) {
    if (slot >= slots_.size()) return false;
    Slot& s = slots_[slot];
    if (!s.pending) return true;
    const uint32_t n = static_cast<uint32_t>(s.names.size());
    if (n == 0) {
        s.pending = false;
        results_.clear();
        return true;
    }
    std::vector<uint64_t> ticks(static_cast<size_t>(n) * 2);
    const VkResult r = ctx_->vkd().vkGetQueryPoolResults(ctx_->device(), s.pool, 0, n * 2, ticks.size() * 8,
                                                         ticks.data(), 8, VK_QUERY_RESULT_64_BIT);
    if (r != VK_SUCCESS) return false;  // VK_NOT_READY: frame not finished yet
    results_.clear();
    for (uint32_t i = 0; i < n; ++i) {
        GpuTimerResult res;
        res.name = s.names[i];
        res.begin_ticks = ticks[i * 2] & mask_;
        res.end_ticks = ticks[i * 2 + 1] & mask_;
        const uint64_t d = (res.end_ticks - res.begin_ticks) & mask_;  // wraps within valid bits
        res.ms = static_cast<double>(d) * ns_per_tick_ * 1e-6;
        results_.push_back(std::move(res));
    }
    s.pending = false;
    return true;
}

void GpuTimer::begin_frame(VkCommandBuffer cmd, uint32_t slot) {
    current_ = UINT32_MAX;
    if (slot >= slots_.size()) return;
    collect(slot);
    Slot& s = slots_[slot];
    s.names.clear();
    s.pending = false;
    ctx_->vkd().vkCmdResetQueryPool(cmd, s.pool, 0, max_scopes_ * 2);
    current_ = slot;
}

uint32_t GpuTimer::begin_scope(VkCommandBuffer cmd, const char* name, VkPipelineStageFlags2 stage) {
    if (current_ == UINT32_MAX) return UINT32_MAX;
    Slot& s = slots_[current_];
    if (s.names.size() >= max_scopes_) return UINT32_MAX;
    const auto id = static_cast<uint32_t>(s.names.size());
    s.names.emplace_back(name ? name : "");
    s.pending = true;
    ctx_->vkd().vkCmdWriteTimestamp2(cmd, stage, s.pool, id * 2);
    return id;
}

void GpuTimer::end_scope(VkCommandBuffer cmd, uint32_t scope, VkPipelineStageFlags2 stage) {
    if (current_ == UINT32_MAX || scope == UINT32_MAX) return;
    ctx_->vkd().vkCmdWriteTimestamp2(cmd, stage, slots_[current_].pool, scope * 2 + 1);
}

}  // namespace alamo::rhi
