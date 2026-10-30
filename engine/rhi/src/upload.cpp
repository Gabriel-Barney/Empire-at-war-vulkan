#include "alamo/rhi/upload.hpp"

#include <algorithm>
#include <cstring>

#include "alamo/core/log.hpp"
#include "alamo/rhi/context.hpp"

namespace alamo::rhi {

namespace {
// Satisfies every bufferOffset rule we hit: texel/block size (1..16) and the transfer-queue 4-byte rule.
constexpr uint64_t kStagingAlign = 16;
}  // namespace

Uploader::~Uploader() { shutdown(); }

bool Uploader::init(Context& ctx, Allocator& alloc, const UploaderDesc& desc) {
    shutdown();
    ctx_ = &ctx;
    alloc_ = &alloc;
    dedicated_ = desc.use_transfer_queue && ctx.has_transfer_queue();
    src_family_ = dedicated_ ? ctx.transfer().family : ctx.graphics().family;
    dst_family_ = ctx.graphics().family;
    const uint64_t cap = std::max<uint64_t>(desc.ring_bytes, 1ull << 20);
    if (!alloc.create_buffer(cap, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::Upload, ring_buffer_, "upload ring"))
        return false;
    ring_ptr_ = static_cast<uint8_t*>(ring_buffer_.mapped());
    if (!ring_ptr_) {
        log::errorf("rhi", "upload ring is not host mapped");
        shutdown();
        return false;
    }
    ring_.reset(cap);
    chunk_limit_ = cap / 4;
    // Starts at 1 so a failed submit can hand its ring records an already-signalled value.
    next_value_ = 1;
    if (!timeline_.init(ctx, 1, "upload timeline")) {
        shutdown();
        return false;
    }
    auto pci = vk_struct<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = src_family_;
    if (!vk_check(ctx.vkd().vkCreateCommandPool(ctx.device(), &pci, nullptr, &pool_), "vkCreateCommandPool(upload)")) {
        pool_ = VK_NULL_HANDLE;
        shutdown();
        return false;
    }
    return true;
}

void Uploader::shutdown() {
    if (!ctx_) return;
    if (timeline_.handle() && next_value_ > 1) timeline_.wait(next_value_);
    if (pool_) ctx_->vkd().vkDestroyCommandPool(ctx_->device(), pool_, nullptr);
    pool_ = VK_NULL_HANDLE;
    cmds_.clear();
    pending_.clear();
    records_.clear();
    last_use_.clear();
    graphics_owned_.clear();
    fault_submits_ = 0;
    fault_waits_ = false;
    timeline_.shutdown();
    if (ring_buffer_) alloc_->destroy(ring_buffer_);
    ring_ptr_ = nullptr;
    ring_.reset(0);
    next_value_ = acquired_through_ = 0;
    ctx_ = nullptr;
    alloc_ = nullptr;
}

void Uploader::retire_locked() {
    // Injected device loss: the timeline looks stuck, so nothing retires.
    const uint64_t done = fault_waits_ ? 0 : timeline_.value();
    while (!records_.empty() && records_.front().value && records_.front().value <= done) {
        ring_.retire(records_.front().end);
        records_.pop_front();
    }
}

void Uploader::inject_faults_for_testing(uint32_t fail_submits, bool fail_waits) {
    std::lock_guard<std::mutex> lock(mutex_);
    fault_submits_ = fail_submits;
    fault_waits_ = fail_waits;
}

bool Uploader::reserve(Job& job, uint64_t size, uint64_t& offset) {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        retire_locked();
        if (auto o = ring_.allocate(size, kStagingAlign)) {
            records_.push_back(Record{ring_.marker(), 0});
            job.records.push_back(&records_.back());
            offset = *o;
            return true;
        }
        if (size + kStagingAlign > ring_.capacity()) return false;
        if (!job.records.empty()) {  // free our own reservations first, or we could wait on ourselves
            // A failed partial submit dropped recorded copies (and the image's UNDEFINED ->
            // TRANSFER_DST barrier): the whole upload fails rather than finishing with holes.
            if (!submit_locked(job)) {
                log::errorf("rhi", "upload: partial submit failed; upload abandoned");
                return false;
            }
            continue;
        }
        if (records_.empty()) return false;
        const uint64_t v = records_.front().value;
        if (v) {
            const bool fail = fault_waits_;
            lock.unlock();
            const bool ok = !fail && timeline_.wait(v);
            lock.lock();
            if (!ok) {  // device lost (or the wait failed otherwise): the ring will never drain
                log::errorf("rhi", "upload: waiting for ring space failed (device lost?); upload abandoned");
                return false;
            }
        } else {
            submitted_cv_.wait(lock);  // another thread is still filling the oldest reservation
        }
    }
}

void Uploader::abandon(Job& job) {
    if (job.ops.empty() && job.records.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    // Nothing reads these ring bytes: hand them the last really submitted value (already signalled
    // or pending), exactly as a failed submit does.
    for (Record* r : job.records) r->value = next_value_;
    job.ops.clear();
    job.records.clear();
    submitted_cv_.notify_all();
}

uint64_t Uploader::submit(Job& job) {
    std::lock_guard<std::mutex> lock(mutex_);
    return submit_locked(job);
}

uint64_t Uploader::submit_locked(Job& job) {
    if (job.ops.empty() && job.records.empty()) return 0;
    const DeviceFns& fn = ctx_->vkd();
    VkDevice dev = ctx_->device();

    const uint64_t done = timeline_.value();
    CmdSlot* slot = nullptr;
    for (auto& s : cmds_)
        if (s.value <= done) {
            slot = &s;
            break;
        }
    if (!slot) {
        auto ai = vk_struct<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
        ai.commandPool = pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        CmdSlot s;
        if (!vk_check(fn.vkAllocateCommandBuffers(dev, &ai, &s.cmd), "vkAllocateCommandBuffers(upload)")) return 0;
        cmds_.push_back(s);
        slot = &cmds_.back();
    }

    VkCommandBuffer cmd = slot->cmd;
    auto bi = vk_struct<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    fn.vkBeginCommandBuffer(cmd, &bi);
    const VkBuffer staging = ring_buffer_.handle;
    const uint64_t value = next_value_ + 1;
    std::vector<PendingAcquire> fresh;  // committed only if the submit succeeds
    for (const Op& op : job.ops) {
        switch (op.kind) {
            case OpKind::ImagePre: {
                const VkImageMemoryBarrier2 b = image_barrier(
                    op.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE,
                    VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, op.range);
                cmd_barrier(fn, cmd, b);
                break;
            }
            case OpKind::ImageCopy:
                fn.vkCmdCopyBufferToImage(cmd, staging, op.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &op.image_copy);
                break;
            case OpKind::ImagePost: {
                const StateInfo fin = state_info(op.final_state);
                if (dedicated_) {
                    VkImageMemoryBarrier2 rel = image_barrier(op.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, fin.layout,
                                                              VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                                              VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, op.range);
                    rel.srcQueueFamilyIndex = src_family_;
                    rel.dstQueueFamilyIndex = dst_family_;
                    cmd_barrier(fn, cmd, rel);
                    VkImageMemoryBarrier2 acq = rel;
                    acq.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
                    acq.srcAccessMask = VK_ACCESS_2_NONE;
                    acq.dstStageMask = fin.stages ? fin.stages : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                    acq.dstAccessMask = fin.access;
                    fresh.push_back(PendingAcquire{value, true, acq, {}});
                } else {
                    const VkImageMemoryBarrier2 b = image_barrier(
                        op.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, fin.layout, VK_PIPELINE_STAGE_2_COPY_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT, fin.stages ? fin.stages : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        fin.access, op.range);
                    cmd_barrier(fn, cmd, b);
                }
                break;
            }
            case OpKind::BufferCopy: fn.vkCmdCopyBuffer(cmd, staging, op.buffer, 1, &op.buffer_copy); break;
            case OpKind::BufferPost: {
                if (dedicated_) {
                    VkBufferMemoryBarrier2 rel = buffer_barrier(op.buffer, VK_PIPELINE_STAGE_2_COPY_BIT,
                                                                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE,
                                                                VK_ACCESS_2_NONE, op.buffer_copy.dstOffset,
                                                                op.buffer_copy.size);
                    rel.srcQueueFamilyIndex = src_family_;
                    rel.dstQueueFamilyIndex = dst_family_;
                    cmd_barrier(fn, cmd, {}, std::span<const VkBufferMemoryBarrier2>(&rel, 1));
                    VkBufferMemoryBarrier2 acq = rel;
                    acq.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
                    acq.srcAccessMask = VK_ACCESS_2_NONE;
                    acq.dstStageMask = op.dst_stages;
                    acq.dstAccessMask = op.dst_access;
                    fresh.push_back(PendingAcquire{value, false, {}, acq});
                } else {
                    const VkBufferMemoryBarrier2 b =
                        buffer_barrier(op.buffer, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                       op.dst_stages, op.dst_access, op.buffer_copy.dstOffset, op.buffer_copy.size);
                    cmd_barrier(fn, cmd, {}, std::span<const VkBufferMemoryBarrier2>(&b, 1));
                }
                break;
            }
        }
    }
    fn.vkEndCommandBuffer(cmd);

    auto cbi = vk_struct<VkCommandBufferSubmitInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO);
    cbi.commandBuffer = cmd;
    const VkSemaphoreSubmitInfo sig = semaphore_info(timeline_.handle(), value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    auto si = vk_struct<VkSubmitInfo2>(VK_STRUCTURE_TYPE_SUBMIT_INFO_2);
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &sig;
    const Queue& q = dedicated_ ? ctx_->transfer() : ctx_->graphics();
    bool ok = false;
    if (fault_submits_) {
        --fault_submits_;
        log::errorf("rhi", "upload submit: injected failure (test)");
    } else {
        ok = vk_check(q.submit({&si, 1}), "upload submit");
    }
    // On failure the value is not consumed (host-signalling it could overtake pending lower values);
    // the ring records and the command buffer get the last value that was really submitted, which
    // is already signalled or will be, and the upload is reported as failed (ticket 0).
    if (ok) {
        next_value_ = value;
        pending_.insert(pending_.end(), fresh.begin(), fresh.end());
        for (const Op& op : job.ops) {
            const uint64_t h = op.image ? reinterpret_cast<uint64_t>(op.image) : reinterpret_cast<uint64_t>(op.buffer);
            last_use_[h] = value;
            // Dedicated queue: the release to graphics is now submitted, so the transfer family no
            // longer owns the buffer, whether or not record_acquires has run yet.
            if (dedicated_ && op.kind == OpKind::BufferPost) graphics_owned_.insert(h);
        }
    }
    slot->value = next_value_;
    for (Record* r : job.records) r->value = next_value_;
    job.ops.clear();
    job.records.clear();
    submitted_cv_.notify_all();
    return ok ? value : 0;
}

UploadTicket Uploader::upload_buffer(const Buffer& dst, VkDeviceSize dst_offset, std::span<const uint8_t> data,
                                     VkPipelineStageFlags2 dst_stages, VkAccessFlags2 dst_access) {
    // Overflow-safe form of dst_offset + size > dst.size.
    if (!ctx_ || !dst || data.empty() || data.size() > dst.size || dst_offset > dst.size - data.size()) {
        log::errorf("rhi", "upload_buffer: bad arguments");
        return {};
    }
    if (dedicated_ && (dst_offset != 0 || data.size() != dst.size)) {
        // A partial update on the transfer family would need a graphics-side release first to keep
        // the untouched bytes valid; that is only safe if graphics never owned the buffer.
        std::lock_guard<std::mutex> lock(mutex_);
        if (graphics_owned_.count(reinterpret_cast<uint64_t>(dst.handle))) {
            log::errorf("rhi", "upload_buffer: partial re-upload of a graphics-owned buffer through the transfer "
                               "queue is not supported (overwrite it whole, or use an Uploader with "
                               "use_transfer_queue = false)");
            return {};
        }
    }
    Job job;
    bool ok = true;
    for (uint64_t pos = 0; pos < data.size();) {
        const uint64_t n = std::min<uint64_t>(chunk_limit_, data.size() - pos);
        uint64_t off = 0;
        if (!reserve(job, n, off)) {
            ok = false;
            break;
        }
        std::memcpy(ring_ptr_ + off, data.data() + pos, static_cast<size_t>(n));
        alloc_->flush(ring_buffer_.alloc, off, n);
        Op op;
        op.kind = OpKind::BufferCopy;
        op.buffer = dst.handle;
        op.buffer_copy = {off, dst_offset + pos, n};
        job.ops.push_back(op);
        pos += n;
    }
    if (ok) {
        Op post;
        post.kind = OpKind::BufferPost;
        post.buffer = dst.handle;
        post.buffer_copy = {0, dst_offset, data.size()};
        post.dst_stages = dst_stages;
        post.dst_access = dst_access;
        job.ops.push_back(post);
    }
    if (!ok) {
        abandon(job);
        return {};
    }
    return UploadTicket{submit(job)};
}

UploadTicket Uploader::upload_image(const Image& dst, std::span<const SubresourceData> subs, ImageState final_state) {
    if (!ctx_ || !dst) return {};
    const FormatInfo& fi = format_info(dst.format);
    Job job;
    bool ok = true;
    for (const SubresourceData& sub : subs) {
        if (sub.mip >= dst.mips || sub.layer >= dst.layers) {
            log::errorf("rhi", "upload_image: subresource mip %u layer %u out of range", sub.mip, sub.layer);
            ok = false;
            break;
        }
        const uint32_t w = mip_extent(dst.width, sub.mip);
        const uint32_t h = mip_extent(dst.height, sub.mip);
        const uint32_t dz = mip_extent(dst.depth, sub.mip);  // 3D: slices of this mip (1 for 2D)
        const uint64_t slice_bytes = subresource_size(dst.format, w, h);
        const uint64_t expect = slice_bytes * dz;
        if (sub.bytes.size() != expect) {
            log::errorf("rhi", "upload_image: mip %u has %zu bytes, expected %llu", sub.mip, sub.bytes.size(),
                        static_cast<unsigned long long>(expect));
            ok = false;
            break;
        }
        Op pre;
        pre.kind = OpKind::ImagePre;
        pre.image = dst.handle;
        pre.range = {dst.aspect, sub.mip, 1, sub.layer, 1};
        job.ops.push_back(pre);

        const uint64_t pitch = row_pitch(dst.format, w);
        const uint32_t rows = block_rows(dst.format, h);
        const uint32_t rows_per = static_cast<uint32_t>(std::max<uint64_t>(1, chunk_limit_ / pitch));
        for (uint32_t z = 0; z < dz && ok; ++z) {
          for (uint32_t r0 = 0; r0 < rows && ok;) {
            const uint32_t n = std::min(rows_per, rows - r0);
            const uint64_t bytes = pitch * n;
            uint64_t off = 0;
            if (!reserve(job, bytes, off)) {
                ok = false;
                break;
            }
            std::memcpy(ring_ptr_ + off, sub.bytes.data() + slice_bytes * z + pitch * r0, static_cast<size_t>(bytes));
            alloc_->flush(ring_buffer_.alloc, off, bytes);
            Op cp;
            cp.kind = OpKind::ImageCopy;
            cp.image = dst.handle;
            const uint32_t y = r0 * fi.block_h;
            cp.image_copy.bufferOffset = off;
            cp.image_copy.imageSubresource = {dst.aspect, sub.mip, sub.layer, 1};
            cp.image_copy.imageOffset = {0, static_cast<int32_t>(y), static_cast<int32_t>(z)};
            cp.image_copy.imageExtent = {w, std::min(n * static_cast<uint32_t>(fi.block_h), h - y), 1};
            job.ops.push_back(cp);
            r0 += n;
          }
        }
        if (!ok) break;
        Op post;
        post.kind = OpKind::ImagePost;
        post.image = dst.handle;
        post.range = pre.range;
        post.final_state = final_state;
        job.ops.push_back(post);
    }
    if (!ok) {
        abandon(job);
        return {};
    }
    return UploadTicket{submit(job)};
}

UploadTicket Uploader::upload_image_chain(const Image& dst, std::span<const uint8_t> data, ImageState final_state) {
    const uint64_t total = dst.depth > 1 ? volume_chain_size(dst.format, dst.width, dst.height, dst.depth, dst.mips)
                                         : mip_chain_size(dst.format, dst.width, dst.height, dst.mips, dst.layers);
    if (data.size() != total) {
        log::errorf("rhi", "upload_image_chain: %zu bytes, expected %llu", data.size(),
                    static_cast<unsigned long long>(total));
        return {};
    }
    std::vector<SubresourceData> subs;
    subs.reserve(static_cast<size_t>(dst.mips) * dst.layers);
    uint64_t pos = 0;
    for (uint32_t l = 0; l < dst.layers; ++l)
        for (uint32_t m = 0; m < dst.mips; ++m) {
            const uint64_t n = volume_mip_size(dst.format, dst.width, dst.height, dst.depth, m);  // depth 1 = 2D
            subs.push_back({m, l, data.subspan(static_cast<size_t>(pos), static_cast<size_t>(n))});
            pos += n;
        }
    return upload_image(dst, subs, final_state);
}

bool Uploader::is_complete(UploadTicket t) const { return t.value && timeline_.value() >= t.value; }

bool Uploader::wait(UploadTicket t, uint64_t timeout_ns) const { return t.value && timeline_.wait(t.value, timeout_ns); }

bool Uploader::ready(UploadTicket t) const {
    if (!t.value) return false;
    if (!dedicated_) return true;
    std::lock_guard<std::mutex> lock(mutex_);
    return t.value <= acquired_through_;
}

AcquireResult Uploader::record_acquires(VkCommandBuffer graphics_cmd) {
    AcquireResult res;
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t done = timeline_.value();
    for (auto it = last_use_.begin(); it != last_use_.end();) {  // completed uploads need no forget() wait
        if (it->second <= done) it = last_use_.erase(it);
        else ++it;
    }
    if (!dedicated_) return res;
    std::vector<VkImageMemoryBarrier2> images;
    std::vector<VkBufferMemoryBarrier2> buffers;
    auto keep = pending_.begin();
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
        if (it->value <= done) {
            if (it->is_image) {
                images.push_back(it->image);
            } else {
                buffers.push_back(it->buffer);
                graphics_owned_.insert(reinterpret_cast<uint64_t>(it->buffer.buffer));
            }
            res.wait_value = std::max(res.wait_value, it->value);
        } else {
            *keep++ = *it;
        }
    }
    pending_.erase(keep, pending_.end());
    cmd_barrier(ctx_->vkd(), graphics_cmd, images, buffers);
    res.barriers = static_cast<uint32_t>(images.size() + buffers.size());
    acquired_through_ = std::max(acquired_through_, done);
    return res;
}

void Uploader::forget_handle(uint64_t handle, bool is_image) {
    uint64_t wait_value = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = last_use_.find(handle); it != last_use_.end()) {
            wait_value = it->second;
            last_use_.erase(it);
        }
        pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                      [&](const PendingAcquire& p) {
                                          const uint64_t h = p.is_image ? reinterpret_cast<uint64_t>(p.image.image)
                                                                        : reinterpret_cast<uint64_t>(p.buffer.buffer);
                                          return p.is_image == is_image && h == handle;
                                      }),
                       pending_.end());
        if (!is_image) graphics_owned_.erase(handle);
    }
    if (wait_value) timeline_.wait(wait_value);
}

void Uploader::forget(VkBuffer buffer) { forget_handle(reinterpret_cast<uint64_t>(buffer), false); }
void Uploader::forget(VkImage image) { forget_handle(reinterpret_cast<uint64_t>(image), true); }

bool Uploader::finish(UploadTicket t) {
    if (!wait(t)) return false;
    if (!dedicated_) return true;
    // The timeline value was observed on the host before this submit, which orders the release
    // before the acquire.
    return ctx_->immediate_submit([this](VkCommandBuffer cmd) { record_acquires(cmd); });
}

}  // namespace alamo::rhi
