// Descriptor management for the translated-shader interface (docs/shaders/PORTING.md section 1):
//   set 0 binding 0 : VSConstants { float4 c[256]; }   uniform buffer
//   set 0 binding 1 : PSConstants { float4 c[32];  }   uniform buffer
//   set 1 binding 0..15 : combined image samplers s0..s15 (partially bound: unused slots may stay empty)
//   push constants   : emulated fixed-function bits
// DescriptorAllocator hands out sets from growable pools that are reset wholesale (one allocator
// per frame-in-flight per recording thread; not thread-safe by itself).
#pragma once
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

#include "alamo/rhi/vk.hpp"

namespace alamo::rhi {

class Context;

inline constexpr uint32_t kSetConstants = 0;
inline constexpr uint32_t kSetTextures = 1;
inline constexpr uint32_t kBindingVSConstants = 0;
inline constexpr uint32_t kBindingPSConstants = 1;
inline constexpr uint32_t kTextureSlots = 16;
inline constexpr VkDeviceSize kVSConstantsBytes = 256 * 16;
inline constexpr VkDeviceSize kPSConstantsBytes = 32 * 16;
// Optional int/bool register files of translated shaders (engine/shaderxlat translate.hpp):
//   set 0 binding 2 : VSIntConstants { int4 i[16]; uint4 b[4]; }   320 bytes
//   set 0 binding 3 : PSIntConstants { int4 i[16]; uint4 b[4]; }   320 bytes
inline constexpr uint32_t kBindingVSIntConstants = 2;
inline constexpr uint32_t kBindingPSIntConstants = 3;
inline constexpr VkDeviceSize kIntBoolConstantsBytes = 16 * 16 + 4 * 16;
inline constexpr uint32_t kTranslatedPushBytes = 64;  // shaderxlat::TranslatedPush, VS+FS

enum StandardLayoutFlags : uint32_t {
    kLayoutIntBoolConstants = 1u << 0,  // also build the set 0 variant with bindings 2 and 3
};

class StandardLayouts {
public:
    StandardLayouts() = default;
    ~StandardLayouts();
    StandardLayouts(const StandardLayouts&) = delete;
    StandardLayouts& operator=(const StandardLayouts&) = delete;

    bool init(Context& ctx, uint32_t flags = 0);  // StandardLayoutFlags
    void shutdown();
    // int_bool: the set 0 variant with bindings 2/3 (VK_NULL_HANDLE unless kLayoutIntBoolConstants).
    // storage (Q3): the instanced translated form's variant, bindings 0 / 1 storage buffers (per-instance images).
    VkDescriptorSetLayout constants(bool int_bool = false, bool storage = false) const {
        return storage ? (int_bool ? set0_st_ib_ : set0_st_) : (int_bool ? set0_ib_ : set0_);
    }
    VkDescriptorSetLayout textures() const { return set1_; }
    // Pipeline layout {set0, set1} + one push-constant range (bytes may be 0). Caller destroys it.
    // Translated shaders: create_pipeline_layout(kTranslatedPushBytes, VS|FS, iface.int_bool_constants).
    VkPipelineLayout create_pipeline_layout(uint32_t push_bytes,
                                            VkShaderStageFlags push_stages = VK_SHADER_STAGE_VERTEX_BIT |
                                                                             VK_SHADER_STAGE_FRAGMENT_BIT,
                                            bool int_bool = false, bool storage = false) const;

private:
    Context* ctx_ = nullptr;
    VkDescriptorSetLayout set0_ = VK_NULL_HANDLE, set0_ib_ = VK_NULL_HANDLE, set1_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set0_st_ = VK_NULL_HANDLE, set0_st_ib_ = VK_NULL_HANDLE;  // Q3 storage variants
};

// Track A "hot set" interface (engine/materials/shaders/common.hlsli):
//   set 0 binding 0     FrameCB          uniform buffer
//   set 0 binding 1..4  Draws, Materials, Bones, FfDraws   storage buffers
//   set 1 binding 0     Texture2D Textures[]   sampled images, bindless (partially bound,
//                       update-after-bind, update-unused-while-pending, non-uniform indexed)
//   set 1 binding 1     SamplerState Samplers[4]  (EngineSampler slots)
//   push constants      16 bytes { DrawIndex, AlphaFunc, AlphaRef, Flags }, VS+FS
// The texture array has a fixed descriptor count (texture_capacity) rather than
// VARIABLE_DESCRIPTOR_COUNT: Vulkan allows a variable count only on the highest-numbered binding,
// and the sampler array sits after it at binding 1. The shader's runtime array works with either.
// Allocate set 1 once from an update-after-bind DescriptorAllocator (bindless_pool_ratios()) and
// write textures into it while frames are in flight; set 0 comes from the per-frame allocator.
inline constexpr uint32_t kHotStorageBuffers = 4;  // bindings 1..4
inline constexpr uint32_t kHotSamplers = 4;
inline constexpr uint32_t kHotPushBytes = 16;

struct DescriptorPoolRatios {
    uint32_t sets = 256;
    uint32_t uniform_buffers = 512;
    uint32_t combined_image_samplers = 4096;
    uint32_t storage_buffers = 1024;
    uint32_t sampled_images = 0;
    uint32_t samplers = 0;
    bool update_after_bind = false;  // VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT
    // Growth cap of one DescriptorAllocator (0 = unlimited). Pools are kept across reset(), so this
    // bounds the allocator's footprint: allocate() fails (logged once) when every pool is full.
    uint32_t max_pools = 64;
};

class HotSetLayouts {
public:
    HotSetLayouts() = default;
    ~HotSetLayouts();
    HotSetLayouts(const HotSetLayouts&) = delete;
    HotSetLayouts& operator=(const HotSetLayouts&) = delete;

    // texture_capacity is clamped to Context::caps().max_bindless_sampled_images.
    bool init(Context& ctx, uint32_t texture_capacity = 16384);
    void shutdown();
    VkDescriptorSetLayout buffers() const { return set0_; }   // set 0
    VkDescriptorSetLayout textures() const { return set1_; }  // set 1 (update-after-bind)
    uint32_t texture_capacity() const { return capacity_; }
    VkPipelineLayout create_pipeline_layout(uint32_t push_bytes = kHotPushBytes) const;  // caller destroys
    // Pool ratios for an allocator that holds sets bindless texture sets.
    DescriptorPoolRatios bindless_pool_ratios(uint32_t sets = 1) const;

private:
    Context* ctx_ = nullptr;
    VkDescriptorSetLayout set0_ = VK_NULL_HANDLE, set1_ = VK_NULL_HANDLE;
    uint32_t capacity_ = 0;
};

class DescriptorAllocator {
public:
    DescriptorAllocator() = default;
    ~DescriptorAllocator();
    DescriptorAllocator(DescriptorAllocator&& o) noexcept;
    DescriptorAllocator& operator=(DescriptorAllocator&& o) noexcept;
    DescriptorAllocator(const DescriptorAllocator&) = delete;
    DescriptorAllocator& operator=(const DescriptorAllocator&) = delete;

    bool init(Context& ctx, const DescriptorPoolRatios& ratios = {});
    void shutdown();
    // VK_NULL_HANDLE on failure. A layout that does not fit an *empty* pool of these ratios (a
    // descriptor type with a zero ratio, more descriptors than one pool holds) fails without
    // growing: no new pool is created for it, and it is logged once per allocator. The layout must
    // match the pool kind (update-after-bind layouts need ratios.update_after_bind; that is a
    // Vulkan usage rule the allocator cannot check from the handle). Exhaustion (max_pools full)
    // also fails, logged once until reset().
    VkDescriptorSet allocate(VkDescriptorSetLayout layout);
    void reset();  // all sets from this allocator become invalid
    size_t pool_count() const { return pools_.size(); }
    uint64_t failed_allocations() const { return failures_; }  // since init

private:
    VkDescriptorPool new_pool();
    Context* ctx_ = nullptr;
    DescriptorPoolRatios ratios_{};
    std::vector<VkDescriptorPool> pools_;
    std::vector<uint32_t> pool_sets_;  // sets allocated from each pool since its last reset
    size_t current_ = 0;
    uint64_t failures_ = 0;
    bool logged_never_fit_ = false, logged_exhausted_ = false;
};

struct TextureBinding {
    uint32_t slot = 0;  // s0..s15
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
};

// Writes set 0. A null buffer skips that binding.
void write_constants(const Context& ctx, VkDescriptorSet set, VkBuffer vs, VkDeviceSize vs_offset, VkDeviceSize vs_range,
                     VkBuffer ps, VkDeviceSize ps_offset, VkDeviceSize ps_range);
// Q3: the same for the storage variant (StandardLayouts::constants(ib, true)): bindings 0 / 1 as storage buffers.
void write_constants_storage(const Context& ctx, VkDescriptorSet set, VkBuffer vs, VkDeviceSize vs_offset, VkDeviceSize vs_range,
                             VkBuffer ps, VkDeviceSize ps_offset, VkDeviceSize ps_range);
void write_textures(const Context& ctx, VkDescriptorSet set, std::span<const TextureBinding> textures);
// Writes set 0 bindings 2/3 (int/bool constants) of the kLayoutIntBoolConstants variant. Null skips.
void write_int_bool_constants(const Context& ctx, VkDescriptorSet set, VkBuffer vs, VkDeviceSize vs_offset, VkBuffer ps,
                              VkDeviceSize ps_offset);

// Hot-set writes. Null buffers are skipped; ranges may be VK_WHOLE_SIZE.
struct BufferRange {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize range = VK_WHOLE_SIZE;
};
void write_hot_buffers(const Context& ctx, VkDescriptorSet set, const BufferRange& frame_cb,
                       std::span<const BufferRange> storage /* bindings 1.. */);
// Bindless slot index of set 1 binding 0 (thread-compatible: callers serialise writes to one set).
void write_bindless_texture(const Context& ctx, VkDescriptorSet set, uint32_t index, VkImageView view,
                            VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
void write_bindless_samplers(const Context& ctx, VkDescriptorSet set, std::span<const VkSampler> samplers);

// ---------------------------------------------------------------------------------------------
// Bindless slot recycling with deferred reuse.
//
// A released slot may still be read by frames the GPU has not finished, so it only becomes
// reusable once the frame timeline reaches the value passed to release(). Pass
// FrameRing::retire_value() from any thread (covers a frame begun concurrently with the call), or
// FrameRing::current_value() only from the thread that calls begin_frame (serialised with it); and
// call collect(FrameRing::completed_value()) once per frame (after begin_frame is fine). Slots
// [0, reserved) are never handed out (slot 0 = white = materials::kNoTexture).
// Reuse policy: retired (collected) slots are reused first, oldest retirement first (FIFO, so a
// stale index keeps sampling the default view as long as possible); fresh slots are taken only
// when no retired slot is free, so high_water() tracks the peak of live + pending slots rather than
// creeping up to capacity. Reusing a retired slot is legal: collect() only retires slots whose
// frames the GPU has finished, so no pending command buffer uses that descriptor
// (UPDATE_UNUSED_WHILE_PENDING). Allocation never stalls on pending slots (exhaustion =
// kInvalidBindlessSlot while slots are still pending). Storage is O(capacity): the free list is a
// fixed ring of `capacity` entries (each slot is in it at most once) and the pending list holds at
// most `capacity` entries.
inline constexpr uint32_t kInvalidBindlessSlot = 0xFFFFFFFFu;

// Pure CPU bookkeeping (no Vulkan). Thread-safe.
class BindlessSlotAllocator {
public:
    void init(uint32_t capacity, uint32_t reserved = 0);
    uint32_t allocate();  // kInvalidBindlessSlot when no slot is free (pending ones are not)
    // retire_value: the frame timeline value after which no submitted work reads the slot.
    // False (and logged) for an out-of-range, reserved, or not-live slot (double release).
    bool release(uint32_t slot, uint64_t retire_value);
    // Moves every pending slot with retire_value <= completed_value to the free list; appends
    // them to `retired` (release order) when given. Returns the number retired.
    uint32_t collect(uint64_t completed_value, std::vector<uint32_t>* retired = nullptr);

    uint32_t capacity() const;
    uint32_t live() const;        // allocated and not released
    uint32_t pending() const;     // released, waiting for the GPU
    uint32_t available() const;   // allocatable now (fresh + retired)
    uint32_t high_water() const;  // highest slot ever handed out + 1 (>= reserved)
    size_t storage_bytes() const; // heap reserved by the bookkeeping: O(capacity), never grows with use

private:
    enum : uint8_t { kFree = 0, kLive = 1, kPending = 2 };
    struct Pending {
        uint32_t slot;
        uint64_t value;
    };
    mutable std::mutex mutex_;
    uint32_t capacity_ = 0, reserved_ = 0, next_fresh_ = 0, live_ = 0;
    std::vector<uint8_t> state_;
    std::vector<uint32_t> free_;  // ring of `capacity_` entries: retired slots, FIFO from free_head_
    size_t free_head_ = 0, free_count_ = 0;
    std::vector<Pending> pending_;
};

// BindlessSlotAllocator + the descriptor writes of set 1 binding 0 (HotSetLayouts::textures()).
// allocate() writes the view into the slot; a released slot keeps its old descriptor until it
// retires (frames in flight may still read it; rewriting a descriptor a pending command buffer
// uses is not allowed even with UPDATE_UNUSED_WHILE_PENDING), then collect() writes default_view
// into it so a stale index samples the default texture instead of a destroyed view. The caller
// keeps the released image view alive until collect() has retired its slot. Thread-safe.
class BindlessTextureTable {
public:
    BindlessTextureTable() = default;
    BindlessTextureTable(const BindlessTextureTable&) = delete;
    BindlessTextureTable& operator=(const BindlessTextureTable&) = delete;

    // capacity: HotSetLayouts::texture_capacity(). default_view may be VK_NULL_HANDLE (retired
    // slots then keep their last descriptor; the view must then stay alive until the slot is reused).
    bool init(const Context& ctx, VkDescriptorSet set, uint32_t capacity, VkImageView default_view,
              uint32_t reserved = 1, VkImageLayout default_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    void shutdown();
    uint32_t allocate(VkImageView view, VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    bool release(uint32_t slot, uint64_t retire_value);
    uint32_t collect(uint64_t completed_value);  // returns slots retired (each rewritten to default_view)
    const BindlessSlotAllocator& slots() const { return slots_; }
    VkDescriptorSet set() const { return set_; }

private:
    const Context* ctx_ = nullptr;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkImageView default_view_ = VK_NULL_HANDLE;
    VkImageLayout default_layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    BindlessSlotAllocator slots_;
    std::mutex write_mutex_;  // serialises descriptor writes to set_
    std::vector<uint32_t> scratch_;
};

}  // namespace alamo::rhi
