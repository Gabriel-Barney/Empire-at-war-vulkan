#include "alamo/rhi/descriptors.hpp"

#include <algorithm>
#include <utility>

#include "alamo/core/log.hpp"
#include "alamo/rhi/context.hpp"

namespace alamo::rhi {

StandardLayouts::~StandardLayouts() { shutdown(); }

bool StandardLayouts::init(Context& ctx, uint32_t flags) {
    shutdown();
    ctx_ = &ctx;
    const DeviceFns& fn = ctx.vkd();
    constexpr VkShaderStageFlags kGraphics = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    // Set 0: float register files (bindings 0/1), plus int/bool files (2/3) in the optional variant.
    VkDescriptorSetLayoutBinding b0[4]{};
    b0[0] = {kBindingVSConstants, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kGraphics, nullptr};
    b0[1] = {kBindingPSConstants, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kGraphics, nullptr};
    b0[2] = {kBindingVSIntConstants, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kGraphics, nullptr};
    b0[3] = {kBindingPSIntConstants, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kGraphics, nullptr};
    const VkDescriptorBindingFlags f0[4] = {VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
                                            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT};
    auto make_set0 = [&](uint32_t count, VkDescriptorSetLayout& out, VkDescriptorType float_type) {
        b0[0].descriptorType = b0[1].descriptorType = float_type;
        auto fi0 = vk_struct<VkDescriptorSetLayoutBindingFlagsCreateInfo>(
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO);
        fi0.bindingCount = count;
        fi0.pBindingFlags = f0;
        auto ci0 = vk_struct<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
        ci0.pNext = &fi0;
        ci0.bindingCount = count;
        ci0.pBindings = b0;
        return vk_check(fn.vkCreateDescriptorSetLayout(ctx.device(), &ci0, nullptr, &out), "set 0 layout");
    };
    if (!make_set0(2, set0_, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)) return false;
    if ((flags & kLayoutIntBoolConstants) && !make_set0(4, set0_ib_, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)) return false;
    // Q3: the instanced translated form - bindings 0 / 1 are storage buffers (per-instance register images).
    if (!make_set0(2, set0_st_, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)) return false;
    if ((flags & kLayoutIntBoolConstants) && !make_set0(4, set0_st_ib_, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)) return false;

    VkDescriptorSetLayoutBinding b1[kTextureSlots]{};
    VkDescriptorBindingFlags f1[kTextureSlots]{};
    for (uint32_t i = 0; i < kTextureSlots; ++i) {
        b1[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kGraphics, nullptr};
        f1[i] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    }
    auto fi1 = vk_struct<VkDescriptorSetLayoutBindingFlagsCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO);
    fi1.bindingCount = kTextureSlots;
    fi1.pBindingFlags = f1;
    auto ci1 = vk_struct<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    ci1.pNext = &fi1;
    ci1.bindingCount = kTextureSlots;
    ci1.pBindings = b1;
    if (!vk_check(fn.vkCreateDescriptorSetLayout(ctx.device(), &ci1, nullptr, &set1_), "set 1 layout")) return false;
    return true;
}

void StandardLayouts::shutdown() {
    if (!ctx_) return;
    const DeviceFns& fn = ctx_->vkd();
    if (set0_) fn.vkDestroyDescriptorSetLayout(ctx_->device(), set0_, nullptr);
    if (set0_ib_) fn.vkDestroyDescriptorSetLayout(ctx_->device(), set0_ib_, nullptr);
    if (set0_st_) fn.vkDestroyDescriptorSetLayout(ctx_->device(), set0_st_, nullptr);
    if (set0_st_ib_) fn.vkDestroyDescriptorSetLayout(ctx_->device(), set0_st_ib_, nullptr);
    set0_st_ = set0_st_ib_ = VK_NULL_HANDLE;
    if (set1_) fn.vkDestroyDescriptorSetLayout(ctx_->device(), set1_, nullptr);
    set0_ = set0_ib_ = set1_ = VK_NULL_HANDLE;
    ctx_ = nullptr;
}

VkPipelineLayout StandardLayouts::create_pipeline_layout(uint32_t push_bytes, VkShaderStageFlags push_stages,
                                                        bool int_bool, bool storage) const {
    if (int_bool && !set0_ib_) {
        log::errorf("rhi", "StandardLayouts: int/bool variant requested but not created (kLayoutIntBoolConstants)");
        return VK_NULL_HANDLE;
    }
    VkDescriptorSetLayout sets[2] = {constants(int_bool, storage), set1_};
    VkPushConstantRange pc{push_stages, 0, push_bytes};
    auto ci = vk_struct<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    ci.setLayoutCount = 2;
    ci.pSetLayouts = sets;
    ci.pushConstantRangeCount = push_bytes ? 1u : 0u;
    ci.pPushConstantRanges = &pc;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (!vk_check(ctx_->vkd().vkCreatePipelineLayout(ctx_->device(), &ci, nullptr, &layout), "vkCreatePipelineLayout"))
        return VK_NULL_HANDLE;
    return layout;
}

// ---------------------------------------------------------------------------------------------

HotSetLayouts::~HotSetLayouts() { shutdown(); }

bool HotSetLayouts::init(Context& ctx, uint32_t texture_capacity) {
    shutdown();
    ctx_ = &ctx;
    const DeviceFns& fn = ctx.vkd();
    constexpr VkShaderStageFlags kGraphics = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    capacity_ = texture_capacity;
    if (ctx.caps().max_bindless_sampled_images) capacity_ = std::min(capacity_, ctx.caps().max_bindless_sampled_images);
    if (!capacity_) capacity_ = 1;

    VkDescriptorSetLayoutBinding b0[1 + kHotStorageBuffers]{};
    VkDescriptorBindingFlags f0[1 + kHotStorageBuffers]{};
    b0[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kGraphics, nullptr};
    for (uint32_t i = 1; i <= kHotStorageBuffers; ++i) b0[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kGraphics, nullptr};
    for (auto& f : f0) f = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;  // FF and mesh programs use different subsets
    auto fi0 = vk_struct<VkDescriptorSetLayoutBindingFlagsCreateInfo>(
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO);
    fi0.bindingCount = 1 + kHotStorageBuffers;
    fi0.pBindingFlags = f0;
    auto ci0 = vk_struct<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    ci0.pNext = &fi0;
    ci0.bindingCount = 1 + kHotStorageBuffers;
    ci0.pBindings = b0;
    if (!vk_check(fn.vkCreateDescriptorSetLayout(ctx.device(), &ci0, nullptr, &set0_), "hot set 0 layout")) return false;

    VkDescriptorSetLayoutBinding b1[2]{};
    b1[0] = {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, capacity_, kGraphics, nullptr};
    b1[1] = {1, VK_DESCRIPTOR_TYPE_SAMPLER, kHotSamplers, kGraphics, nullptr};
    const VkDescriptorBindingFlags f1[2] = {
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT,
        0};
    auto fi1 = vk_struct<VkDescriptorSetLayoutBindingFlagsCreateInfo>(
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO);
    fi1.bindingCount = 2;
    fi1.pBindingFlags = f1;
    auto ci1 = vk_struct<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    ci1.pNext = &fi1;
    ci1.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    ci1.bindingCount = 2;
    ci1.pBindings = b1;
    if (!vk_check(fn.vkCreateDescriptorSetLayout(ctx.device(), &ci1, nullptr, &set1_), "hot set 1 layout")) return false;
    return true;
}

void HotSetLayouts::shutdown() {
    if (!ctx_) return;
    const DeviceFns& fn = ctx_->vkd();
    if (set0_) fn.vkDestroyDescriptorSetLayout(ctx_->device(), set0_, nullptr);
    if (set1_) fn.vkDestroyDescriptorSetLayout(ctx_->device(), set1_, nullptr);
    set0_ = set1_ = VK_NULL_HANDLE;
    ctx_ = nullptr;
}

VkPipelineLayout HotSetLayouts::create_pipeline_layout(uint32_t push_bytes) const {
    VkDescriptorSetLayout sets[2] = {set0_, set1_};
    VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, push_bytes};
    auto ci = vk_struct<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    ci.setLayoutCount = 2;
    ci.pSetLayouts = sets;
    ci.pushConstantRangeCount = push_bytes ? 1u : 0u;
    ci.pPushConstantRanges = &pc;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (!vk_check(ctx_->vkd().vkCreatePipelineLayout(ctx_->device(), &ci, nullptr, &layout), "vkCreatePipelineLayout"))
        return VK_NULL_HANDLE;
    return layout;
}

DescriptorPoolRatios HotSetLayouts::bindless_pool_ratios(uint32_t sets) const {
    DescriptorPoolRatios r;
    r.sets = sets;
    r.uniform_buffers = 0;
    r.combined_image_samplers = 0;
    r.storage_buffers = 0;
    r.sampled_images = capacity_ * sets;
    r.samplers = kHotSamplers * sets;
    r.update_after_bind = true;
    return r;
}

// ---------------------------------------------------------------------------------------------

DescriptorAllocator::~DescriptorAllocator() { shutdown(); }

DescriptorAllocator::DescriptorAllocator(DescriptorAllocator&& o) noexcept
    : ctx_(o.ctx_),
      ratios_(o.ratios_),
      pools_(std::move(o.pools_)),
      pool_sets_(std::move(o.pool_sets_)),
      current_(o.current_),
      failures_(o.failures_),
      logged_never_fit_(o.logged_never_fit_),
      logged_exhausted_(o.logged_exhausted_) {
    o.ctx_ = nullptr;
    o.pools_.clear();
    o.pool_sets_.clear();
    o.current_ = 0;
    o.failures_ = 0;
}

DescriptorAllocator& DescriptorAllocator::operator=(DescriptorAllocator&& o) noexcept {
    if (this != &o) {
        shutdown();
        ctx_ = o.ctx_;
        ratios_ = o.ratios_;
        pools_ = std::move(o.pools_);
        pool_sets_ = std::move(o.pool_sets_);
        current_ = o.current_;
        failures_ = o.failures_;
        logged_never_fit_ = o.logged_never_fit_;
        logged_exhausted_ = o.logged_exhausted_;
        o.ctx_ = nullptr;
        o.pools_.clear();
        o.pool_sets_.clear();
        o.current_ = 0;
        o.failures_ = 0;
    }
    return *this;
}

bool DescriptorAllocator::init(Context& ctx, const DescriptorPoolRatios& ratios) {
    shutdown();
    ctx_ = &ctx;
    ratios_ = ratios;
    return new_pool() != VK_NULL_HANDLE;
}

void DescriptorAllocator::shutdown() {
    if (ctx_)
        for (VkDescriptorPool p : pools_) ctx_->vkd().vkDestroyDescriptorPool(ctx_->device(), p, nullptr);
    pools_.clear();
    pool_sets_.clear();
    current_ = 0;
    failures_ = 0;
    logged_never_fit_ = logged_exhausted_ = false;
    ctx_ = nullptr;
}

VkDescriptorPool DescriptorAllocator::new_pool() {
    const VkDescriptorPoolSize all[5] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, ratios_.uniform_buffers},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, ratios_.combined_image_samplers},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, ratios_.storage_buffers},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, ratios_.sampled_images},
        {VK_DESCRIPTOR_TYPE_SAMPLER, ratios_.samplers},
    };
    VkDescriptorPoolSize sizes[5];
    uint32_t n = 0;
    for (const auto& p : all)
        if (p.descriptorCount) sizes[n++] = p;  // zero counts are invalid
    auto ci = vk_struct<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
    ci.flags = ratios_.update_after_bind
                   ? static_cast<VkDescriptorPoolCreateFlags>(VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT)
                   : 0u;
    ci.maxSets = ratios_.sets;
    ci.poolSizeCount = n;
    ci.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (!vk_check(ctx_->vkd().vkCreateDescriptorPool(ctx_->device(), &ci, nullptr, &pool), "vkCreateDescriptorPool"))
        return VK_NULL_HANDLE;
    pools_.push_back(pool);
    pool_sets_.push_back(0);
    return pool;
}

VkDescriptorSet DescriptorAllocator::allocate(VkDescriptorSetLayout layout) {
    if (!ctx_ || pools_.empty()) return VK_NULL_HANDLE;
    const DeviceFns& fn = ctx_->vkd();
    // Pools after current_ are always empty (allocation only moves forward until reset()), so this
    // loop ends at the latest on the first empty pool it tries.
    for (;;) {
        auto ai = vk_struct<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
        ai.descriptorPool = pools_[current_];
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        const VkResult r = fn.vkAllocateDescriptorSets(ctx_->device(), &ai, &set);
        if (r == VK_SUCCESS) {
            ++pool_sets_[current_];
            return set;
        }
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) {
            vk_check(r, "vkAllocateDescriptorSets");
            ++failures_;
            return VK_NULL_HANDLE;
        }
        if (pool_sets_[current_] == 0) {
            ++failures_;
            // No live set comes from this pool, so resetting it is free; some drivers (seen on AMD)
            // otherwise keep failing later, fitting allocations from a pool after such a failure.
            fn.vkResetDescriptorPool(ctx_->device(), pools_[current_], 0);
            // An empty pool cannot hold it, so no pool of these ratios ever will: do not grow.
            if (!logged_never_fit_) {
                log::errorf("rhi", "DescriptorAllocator: a set layout does not fit an empty pool (descriptor type "
                                   "missing from the pool ratios, or more descriptors than one pool holds)");
                logged_never_fit_ = true;
            }
            return VK_NULL_HANDLE;
        }
        // Next pool (reuse one kept from an earlier frame, else grow up to max_pools).
        if (current_ + 1 < pools_.size()) {
            ++current_;
        } else if (ratios_.max_pools && pools_.size() >= ratios_.max_pools) {
            ++failures_;
            if (!logged_exhausted_) {
                log::errorf("rhi", "DescriptorAllocator: all %u pools are full (raise max_pools or reset more often)",
                            ratios_.max_pools);
                logged_exhausted_ = true;
            }
            return VK_NULL_HANDLE;
        } else if (new_pool()) {
            current_ = pools_.size() - 1;
        } else {
            ++failures_;
            return VK_NULL_HANDLE;
        }
    }
}

void DescriptorAllocator::reset() {
    if (!ctx_) return;
    for (VkDescriptorPool p : pools_) ctx_->vkd().vkResetDescriptorPool(ctx_->device(), p, 0);
    for (uint32_t& n : pool_sets_) n = 0;
    current_ = 0;
    logged_exhausted_ = false;
}

// ---------------------------------------------------------------------------------------------

void write_constants(const Context& ctx, VkDescriptorSet set, VkBuffer vs, VkDeviceSize vs_offset, VkDeviceSize vs_range,
                     VkBuffer ps, VkDeviceSize ps_offset, VkDeviceSize ps_range) {
    VkDescriptorBufferInfo infos[2] = {{vs, vs_offset, vs_range}, {ps, ps_offset, ps_range}};
    VkWriteDescriptorSet w[2]{};
    uint32_t n = 0;
    for (uint32_t i = 0; i < 2; ++i) {
        if (!infos[i].buffer) continue;
        w[n] = {};
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[n].dstSet = set;
        w[n].dstBinding = i == 0 ? kBindingVSConstants : kBindingPSConstants;
        w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[n].pBufferInfo = &infos[i];
        ++n;
    }
    if (n) ctx.vkd().vkUpdateDescriptorSets(ctx.device(), n, w, 0, nullptr);
}

void write_constants_storage(const Context& ctx, VkDescriptorSet set, VkBuffer vs, VkDeviceSize vs_offset, VkDeviceSize vs_range,
                             VkBuffer ps, VkDeviceSize ps_offset, VkDeviceSize ps_range) {
    VkDescriptorBufferInfo infos[2] = {{vs, vs_offset, vs_range}, {ps, ps_offset, ps_range}};
    VkWriteDescriptorSet w[2]{};
    uint32_t n = 0;
    for (uint32_t i = 0; i < 2; ++i) {
        if (!infos[i].buffer) continue;
        w[n] = {};
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[n].dstSet = set;
        w[n].dstBinding = i == 0 ? kBindingVSConstants : kBindingPSConstants;
        w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[n].pBufferInfo = &infos[i];
        ++n;
    }
    if (n) ctx.vkd().vkUpdateDescriptorSets(ctx.device(), n, w, 0, nullptr);
}

void write_textures(const Context& ctx, VkDescriptorSet set, std::span<const TextureBinding> textures) {
    VkDescriptorImageInfo infos[kTextureSlots]{};
    VkWriteDescriptorSet w[kTextureSlots]{};
    uint32_t n = 0;
    for (const TextureBinding& t : textures) {
        if (t.slot >= kTextureSlots || n >= kTextureSlots) {
            log::errorf("rhi", "write_textures: slot %u out of range", t.slot);
            continue;
        }
        infos[n] = {t.sampler, t.view, t.layout};
        w[n] = {};
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[n].dstSet = set;
        w[n].dstBinding = t.slot;
        w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[n].pImageInfo = &infos[n];
        ++n;
    }
    if (n) ctx.vkd().vkUpdateDescriptorSets(ctx.device(), n, w, 0, nullptr);
}

}  // namespace alamo::rhi

namespace alamo::rhi {

void write_hot_buffers(const Context& ctx, VkDescriptorSet set, const BufferRange& frame_cb,
                       std::span<const BufferRange> storage) {
    VkDescriptorBufferInfo infos[1 + kHotStorageBuffers]{};
    VkWriteDescriptorSet w[1 + kHotStorageBuffers]{};
    uint32_t n = 0;
    auto add = [&](uint32_t binding, VkDescriptorType type, const BufferRange& r) {
        if (!r.buffer) return;
        infos[n] = {r.buffer, r.offset, r.range};
        w[n] = vk_struct<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        w[n].dstSet = set;
        w[n].dstBinding = binding;
        w[n].descriptorCount = 1;
        w[n].descriptorType = type;
        w[n].pBufferInfo = &infos[n];
        ++n;
    };
    add(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frame_cb);
    for (size_t i = 0; i < storage.size() && i < kHotStorageBuffers; ++i)
        add(static_cast<uint32_t>(i + 1), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, storage[i]);
    if (n) ctx.vkd().vkUpdateDescriptorSets(ctx.device(), n, w, 0, nullptr);
}

void write_bindless_texture(const Context& ctx, VkDescriptorSet set, uint32_t index, VkImageView view,
                            VkImageLayout layout) {
    VkDescriptorImageInfo info{VK_NULL_HANDLE, view, layout};
    auto w = vk_struct<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
    w.dstSet = set;
    w.dstBinding = 0;
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &info;
    ctx.vkd().vkUpdateDescriptorSets(ctx.device(), 1, &w, 0, nullptr);
}

void write_bindless_samplers(const Context& ctx, VkDescriptorSet set, std::span<const VkSampler> samplers) {
    VkDescriptorImageInfo infos[kHotSamplers]{};
    const uint32_t n = static_cast<uint32_t>(std::min<size_t>(samplers.size(), kHotSamplers));
    if (!n) return;
    for (uint32_t i = 0; i < n; ++i) infos[i] = {samplers[i], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    auto w = vk_struct<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
    w.dstSet = set;
    w.dstBinding = 1;
    w.descriptorCount = n;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w.pImageInfo = infos;
    ctx.vkd().vkUpdateDescriptorSets(ctx.device(), 1, &w, 0, nullptr);
}

}  // namespace alamo::rhi
namespace alamo::rhi {

// --- BindlessSlotAllocator ---------------------------------------------------------------------

void BindlessSlotAllocator::init(uint32_t capacity, uint32_t reserved) {
    std::lock_guard<std::mutex> lock(mutex_);
    capacity_ = capacity;
    reserved_ = std::min(reserved, capacity);
    next_fresh_ = reserved_;
    live_ = 0;
    state_.assign(capacity, kFree);
    free_.assign(capacity, 0u);  // fixed ring: a slot is in the free list at most once
    free_.shrink_to_fit();
    free_head_ = free_count_ = 0;
    pending_.clear();
    pending_.shrink_to_fit();
    pending_.reserve(capacity);  // a slot is pending at most once: never grows past this
}

uint32_t BindlessSlotAllocator::allocate() {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t slot = kInvalidBindlessSlot;
    if (free_count_) {  // retired slots first (oldest first): keeps the high-water mark low
        slot = free_[free_head_];
        free_head_ = (free_head_ + 1 == free_.size()) ? 0 : free_head_ + 1;
        --free_count_;
    } else if (next_fresh_ < capacity_) {
        slot = next_fresh_++;
    } else {
        return kInvalidBindlessSlot;
    }
    state_[slot] = kLive;
    ++live_;
    return slot;
}

bool BindlessSlotAllocator::release(uint32_t slot, uint64_t retire_value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot >= capacity_ || slot < reserved_ || state_[slot] != kLive) {
        log::errorf("rhi", "bindless release: slot %u is not a live slot", slot);
        return false;
    }
    state_[slot] = kPending;
    --live_;
    pending_.push_back({slot, retire_value});
    return true;
}

uint32_t BindlessSlotAllocator::collect(uint64_t completed_value, std::vector<uint32_t>* retired) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t n = 0;
    size_t keep = 0;
    for (size_t i = 0; i < pending_.size(); ++i) {  // stable: retired slots keep their release order
        const Pending p = pending_[i];
        if (p.value <= completed_value) {
            state_[p.slot] = kFree;
            size_t tail = free_head_ + free_count_;
            if (tail >= free_.size()) tail -= free_.size();
            free_[tail] = p.slot;
            ++free_count_;
            if (retired) retired->push_back(p.slot);
            ++n;
        } else {
            pending_[keep++] = p;
        }
    }
    pending_.resize(keep);
    return n;
}

uint32_t BindlessSlotAllocator::capacity() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return capacity_;
}
uint32_t BindlessSlotAllocator::live() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return live_;
}
uint32_t BindlessSlotAllocator::pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<uint32_t>(pending_.size());
}
uint32_t BindlessSlotAllocator::available() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return (capacity_ - next_fresh_) + static_cast<uint32_t>(free_count_);
}
uint32_t BindlessSlotAllocator::high_water() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_fresh_;
}
size_t BindlessSlotAllocator::storage_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.capacity() * sizeof(uint8_t) + free_.capacity() * sizeof(uint32_t) +
           pending_.capacity() * sizeof(Pending);
}

// --- BindlessTextureTable ----------------------------------------------------------------------

bool BindlessTextureTable::init(const Context& ctx, VkDescriptorSet set, uint32_t capacity, VkImageView default_view,
                                uint32_t reserved, VkImageLayout default_layout) {
    if (!set || !capacity) {
        log::errorf("rhi", "BindlessTextureTable: null set or zero capacity");
        return false;
    }
    std::lock_guard<std::mutex> lock(write_mutex_);
    ctx_ = &ctx;
    set_ = set;
    default_view_ = default_view;
    default_layout_ = default_layout;
    slots_.init(capacity, reserved);
    scratch_.clear();
    return true;
}

void BindlessTextureTable::shutdown() {
    std::lock_guard<std::mutex> lock(write_mutex_);
    ctx_ = nullptr;
    set_ = VK_NULL_HANDLE;
    default_view_ = VK_NULL_HANDLE;
    slots_.init(0, 0);
    scratch_.clear();
}

uint32_t BindlessTextureTable::allocate(VkImageView view, VkImageLayout layout) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (!ctx_) return kInvalidBindlessSlot;
    const uint32_t slot = slots_.allocate();
    if (slot != kInvalidBindlessSlot && view) write_bindless_texture(*ctx_, set_, slot, view, layout);
    return slot;
}

bool BindlessTextureTable::release(uint32_t slot, uint64_t retire_value) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (!ctx_) return false;
    return slots_.release(slot, retire_value);
}

uint32_t BindlessTextureTable::collect(uint64_t completed_value) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (!ctx_) return 0;
    scratch_.clear();
    const uint32_t n = slots_.collect(completed_value, &scratch_);
    if (n && default_view_) {
        // The GPU finished every frame that could read these slots: rewriting them is legal
        // (update-after-bind + update-unused-while-pending) and later frames never index them.
        const VkDescriptorImageInfo info{VK_NULL_HANDLE, default_view_, default_layout_};
        std::vector<VkWriteDescriptorSet> w(scratch_.size());
        for (size_t i = 0; i < scratch_.size(); ++i) {
            w[i] = vk_struct<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
            w[i].dstSet = set_;
            w[i].dstBinding = 0;
            w[i].dstArrayElement = scratch_[i];
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            w[i].pImageInfo = &info;
        }
        ctx_->vkd().vkUpdateDescriptorSets(ctx_->device(), static_cast<uint32_t>(w.size()), w.data(), 0, nullptr);
    }
    return n;
}

void write_int_bool_constants(const Context& ctx, VkDescriptorSet set, VkBuffer vs, VkDeviceSize vs_offset, VkBuffer ps,
                              VkDeviceSize ps_offset) {
    VkDescriptorBufferInfo infos[2] = {{vs, vs_offset, kIntBoolConstantsBytes}, {ps, ps_offset, kIntBoolConstantsBytes}};
    VkWriteDescriptorSet w[2]{};
    uint32_t n = 0;
    for (uint32_t i = 0; i < 2; ++i) {
        if (!infos[i].buffer) continue;
        w[n] = vk_struct<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        w[n].dstSet = set;
        w[n].dstBinding = i == 0 ? kBindingVSIntConstants : kBindingPSIntConstants;
        w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[n].pBufferInfo = &infos[i];
        ++n;
    }
    if (n) ctx.vkd().vkUpdateDescriptorSets(ctx.device(), n, w, 0, nullptr);
}

}  // namespace alamo::rhi