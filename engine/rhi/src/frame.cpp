#include "alamo/rhi/frame.hpp"

#include <algorithm>
#include <vector>

#include "alamo/core/log.hpp"
#include "alamo/rhi/context.hpp"

namespace alamo::rhi {

VkCommandBuffer Frame::next(ThreadPools& t, bool secondary) {
    std::vector<VkCommandBuffer>& list = secondary ? t.secondaries : t.primaries;
    uint32_t& used = secondary ? t.used_secondaries : t.used_primaries;
    if (used == list.size()) {
        // Past the preallocated count (FrameRingDesc::*_per_thread): grow by one, counted (Q3 steady-state check).
        if (!preallocate(t, secondary, 1)) return VK_NULL_HANDLE;
        if (growth_) growth_->fetch_add(1, std::memory_order_relaxed);
    }
    return list[used++];
}

bool Frame::preallocate(ThreadPools& t, bool secondary, uint32_t count) {
    if (count == 0) return true;
    std::vector<VkCommandBuffer>& list = secondary ? t.secondaries : t.primaries;
    auto ai = vk_struct<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
    ai.commandPool = t.pool;
    ai.level = secondary ? VK_COMMAND_BUFFER_LEVEL_SECONDARY : VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = count;
    const size_t at = list.size();
    list.resize(at + count, VK_NULL_HANDLE);
    if (!vk_check(ctx_->vkd().vkAllocateCommandBuffers(ctx_->device(), &ai, list.data() + at), "vkAllocateCommandBuffers")) {
        list.resize(at);
        return false;
    }
    return true;
}

VkCommandBuffer Frame::primary(uint32_t thread) {
    if (thread >= threads_.size()) return VK_NULL_HANDLE;
    VkCommandBuffer cb = next(threads_[thread], false);
    if (!cb) return cb;
    auto bi = vk_struct<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ctx_->vkd().vkBeginCommandBuffer(cb, &bi);
    return cb;
}

VkCommandBuffer Frame::secondary(uint32_t thread, const VkCommandBufferInheritanceRenderingInfo& rendering) {
    if (thread >= threads_.size()) return VK_NULL_HANDLE;
    VkCommandBuffer cb = next(threads_[thread], true);
    if (!cb) return cb;
    auto inh = vk_struct<VkCommandBufferInheritanceInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO);
    inh.pNext = &rendering;
    auto bi = vk_struct<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
    bi.pInheritanceInfo = &inh;
    ctx_->vkd().vkBeginCommandBuffer(cb, &bi);
    return cb;
}

UniformAlloc Frame::alloc_uniform(VkDeviceSize size) { return alloc_aligned(size, uniform_align_); }
UniformAlloc Frame::alloc_storage(VkDeviceSize size) { return alloc_aligned(size, storage_align_); }

UniformAlloc Frame::alloc_aligned(VkDeviceSize size, VkDeviceSize align) {
    UniformAlloc a;
    if (!uniforms_ || !size) return a;
    // Lock-free bump with an aligned start: CAS so concurrent callers never overlap.
    VkDeviceSize cur = uniform_head_.load();
    VkDeviceSize off = 0;
    do {
        off = align_up(cur, align);
        if (size > uniforms_.size || off > uniforms_.size - size) return a;  // no wrap for huge sizes
    } while (!uniform_head_.compare_exchange_weak(cur, off + size));
    a.buffer = uniforms_.handle;
    a.offset = off;
    a.size = size;
    a.ptr = static_cast<uint8_t*>(uniforms_.mapped()) + off;
    return a;
}

// ---------------------------------------------------------------------------------------------

FrameRing::~FrameRing() { shutdown(); }

bool FrameRing::init(Context& ctx, Allocator& alloc, const FrameRingDesc& desc) {
    shutdown();
    ctx_ = &ctx;
    alloc_ = &alloc;
    if (!timeline_.init(ctx, 0, "frame timeline")) return false;
    const uint32_t n = std::clamp(desc.frames_in_flight, 1u, 3u);
    const uint32_t threads = std::max(desc.recording_threads, 1u);
    const DeviceFns& fn = ctx.vkd();
    for (uint32_t i = 0; i < n; ++i) {
        auto f = std::make_unique<Frame>();
        f->ctx_ = &ctx;
        f->growth_ = &growth_;
        f->slot_ = i;
        f->uniform_align_ = std::max<VkDeviceSize>(ctx.caps().min_uniform_buffer_offset_alignment, 16);
        f->storage_align_ = std::max(f->uniform_align_, ctx.caps().min_storage_buffer_offset_alignment);
        f->threads_.resize(threads);
        for (auto& t : f->threads_) {
            auto pci = vk_struct<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
            pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            pci.queueFamilyIndex = ctx.graphics().family;
            if (!vk_check(fn.vkCreateCommandPool(ctx.device(), &pci, nullptr, &t.pool), "frame command pool")) {
                frames_.push_back(std::move(f));
                shutdown();
                return false;
            }
            if (!t.descriptors.init(ctx, desc.descriptors)) {
                frames_.push_back(std::move(f));
                shutdown();
                return false;
            }
            // Q3: the steady-state command buffers up front (one allocation call each), plus list capacity for growth.
            t.primaries.reserve(std::max(desc.primaries_per_thread, 1u) * 2u);
            t.secondaries.reserve(std::max(desc.secondaries_per_thread, 1u) * 2u);
            if (!f->preallocate(t, false, desc.primaries_per_thread) || !f->preallocate(t, true, desc.secondaries_per_thread)) {
                frames_.push_back(std::move(f));
                shutdown();
                return false;
            }
        }
        auto sci = vk_struct<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
        if (!vk_check(fn.vkCreateSemaphore(ctx.device(), &sci, nullptr, &f->acquire_), "acquire semaphore")) {
            frames_.push_back(std::move(f));
            shutdown();
            return false;
        }
        if (desc.uniform_bytes &&
            !alloc.create_buffer(desc.uniform_bytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                 MemoryUsage::Dynamic, f->uniforms_, "frame uniforms")) {
            frames_.push_back(std::move(f));
            shutdown();
            return false;
        }
        frames_.push_back(std::move(f));
    }
    submit_cmds_.reserve(8);
    submit_signals_.reserve(4);
    growth_.store(0, std::memory_order_relaxed);
    return true;
}

void FrameRing::shutdown() {
    if (!ctx_) return;
    wait_idle();
    const DeviceFns& fn = ctx_->vkd();
    for (auto& f : frames_) {
        for (auto& t : f->threads_) {
            t.descriptors.shutdown();
            if (t.pool) fn.vkDestroyCommandPool(ctx_->device(), t.pool, nullptr);
        }
        if (f->acquire_) fn.vkDestroySemaphore(ctx_->device(), f->acquire_, nullptr);
        if (f->uniforms_) alloc_->destroy(f->uniforms_);
    }
    frames_.clear();
    timeline_.shutdown();
    frame_number_.store(0);
    last_submitted_ = 0;
    ctx_ = nullptr;
    alloc_ = nullptr;
}

Frame* FrameRing::begin_frame() {
    if (!ctx_ || frames_.empty()) return nullptr;
    // Published before anything of the new frame is recorded: a concurrent retire_value() that
    // reads the old value still returns this frame's number.
    const uint64_t number = frame_number_.fetch_add(1, std::memory_order_seq_cst) + 1;
    Frame& f = *frames_[static_cast<size_t>(number % frames_.size())];
    if (f.submitted_value_ && !timeline_.wait(f.submitted_value_)) return nullptr;
    const DeviceFns& fn = ctx_->vkd();
    for (auto& t : f.threads_) {
        fn.vkResetCommandPool(ctx_->device(), t.pool, 0);
        t.used_primaries = t.used_secondaries = 0;
        t.descriptors.reset();
    }
    f.uniform_head_ = 0;
    f.number_ = number;
    f.main_ = f.primary(0);
    return f.main_ ? &f : nullptr;
}

bool FrameRing::end_frame(Frame& f, const FrameSubmit& s) {
    const DeviceFns& fn = ctx_->vkd();
    // The uniform ring is HOST_COHERENT (MemoryUsage::Dynamic); the flush is a no-op kept as a guard
    // should the memory type ever change.
    if (f.uniforms_) {
        const VkDeviceSize used = std::min<VkDeviceSize>(f.uniform_head_.load(), f.uniforms_.size);
        if (used) alloc_->flush(f.uniforms_.alloc, 0, used);
    }
    if (!vk_check(fn.vkEndCommandBuffer(f.main_), "vkEndCommandBuffer")) return false;
    // Member scratch (end_frame runs on the frame thread only): no allocation once the capacity covers the counts.
    std::vector<VkCommandBufferSubmitInfo>& cmds = submit_cmds_;
    cmds.clear();
    auto ci = vk_struct<VkCommandBufferSubmitInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO);
    ci.commandBuffer = f.main_;
    cmds.push_back(ci);
    for (VkCommandBuffer cb : s.extra) {
        ci.commandBuffer = cb;
        cmds.push_back(ci);
    }
    std::vector<VkSemaphoreSubmitInfo>& signals = submit_signals_;
    signals.assign(s.signals.begin(), s.signals.end());
    signals.push_back(semaphore_info(timeline_.handle(), f.number_, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT));
    auto si = vk_struct<VkSubmitInfo2>(VK_STRUCTURE_TYPE_SUBMIT_INFO_2);
    si.waitSemaphoreInfoCount = static_cast<uint32_t>(s.waits.size());
    si.pWaitSemaphoreInfos = s.waits.data();
    si.commandBufferInfoCount = static_cast<uint32_t>(cmds.size());
    si.pCommandBufferInfos = cmds.data();
    si.signalSemaphoreInfoCount = static_cast<uint32_t>(signals.size());
    si.pSignalSemaphoreInfos = signals.data();
    if (!vk_check(ctx_->graphics().submit({&si, 1}), "frame submit")) {
        // Not host-signalled (it could overtake pending lower values): the slot falls back to the
        // last value that was really submitted, so begin_frame()/wait_idle() never wait on it.
        f.submitted_value_ = last_submitted_;
        return false;
    }
    f.submitted_value_ = f.number_;
    last_submitted_ = f.number_;
    return true;
}

bool FrameRing::wait_idle() const {
    return last_submitted_ == 0 || timeline_.wait(last_submitted_);
}

}  // namespace alamo::rhi
