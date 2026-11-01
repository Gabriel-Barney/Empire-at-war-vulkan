// alamo_rhi tests (no framework; non-zero exit on failure).
//   alamo_rhi_tests cpu  - format tables, ring allocator, sampler keys, queue-family choice, cache header
//   alamo_rhi_tests gpu  - headless device: memory wrapper, uploads (transfer queue + single queue,
//                          multi-threaded), offscreen clear + triangle + textured draws read back,
//                          frame ring pacing, pipeline cache round trip. Prints SKIP and exits 0
//                          when no Vulkan 1.3 device exists. When VK_LAYER_PATH holds the Khronos
//                          validation layer it is required, and any validation message fails the run.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/log.hpp"
#include "alamo/core/test_support.hpp"
#include "alamo/rhi/rhi.hpp"

#ifdef ALAMO_RHI_HAVE_SHADERS
#include "bl_ps.h"
#include "cs_main.h"
#include "cube_ps.h"
#include "vol_ps.h"
#include "discard_ps.h"
#include "fs_ps.h"
#include "fs_vs.h"
#include "tri_ps.h"
#include "tri_vs.h"
#endif

using namespace alamo;
using namespace alamo::rhi;

namespace {

// =============================================================================================
// CPU tests
// =============================================================================================

void test_formats() {
    CHECK(format_info(Format::BC1).bytes_per_block == 8);
    CHECK(format_info(Format::BC3).bytes_per_block == 16);
    CHECK(to_vk(Format::BC1) == VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
    CHECK(to_vk(Format::BC2) == VK_FORMAT_BC2_UNORM_BLOCK);
    CHECK(to_vk(Format::BC3) == VK_FORMAT_BC3_UNORM_BLOCK);
    CHECK(to_vk(Format::BGRA8) == VK_FORMAT_B8G8R8A8_UNORM);
    CHECK(to_vk(Format::R8G8) == VK_FORMAT_R8G8_UNORM);
    CHECK(to_vk(static_cast<Format>(200)) == VK_FORMAT_UNDEFINED);
    for (uint32_t i = 1; i < static_cast<uint32_t>(Format::Count); ++i)
        CHECK(from_vk(to_vk(static_cast<Format>(i))) == static_cast<Format>(i));
    CHECK(from_vk(VK_FORMAT_R64_SFLOAT) == Format::Undefined);

    CHECK(subresource_size(Format::BC1, 4, 4) == 8);
    CHECK(subresource_size(Format::BC1, 8, 8) == 32);
    CHECK(subresource_size(Format::BC1, 2, 2) == 8);   // partial block still costs a block
    CHECK(subresource_size(Format::BC1, 1, 1) == 8);
    CHECK(subresource_size(Format::BC3, 5, 9) == 2 * 3 * 16);
    CHECK(subresource_size(Format::RGBA8, 3, 5) == 60);
    CHECK(subresource_size(Format::RGBA32F, 3, 5) == 240 && to_vk(Format::RGBA32F) == VK_FORMAT_R32G32B32A32_SFLOAT);
    CHECK(subresource_size(Format::RG32F, 2, 2) == 32 && to_vk(Format::RG32F) == VK_FORMAT_R32G32_SFLOAT);
    CHECK(subresource_size(Format::RGBA16F, 2, 2) == 32);
    CHECK(subresource_size(Format::B4G4R4A4, 3, 3) == 18 && to_vk(Format::B4G4R4A4) == VK_FORMAT_B4G4R4A4_UNORM_PACK16);
    CHECK(to_vk(Format::R4G4B4A4) == VK_FORMAT_R4G4B4A4_UNORM_PACK16 && subresource_size(Format::R4G4B4A4, 2, 1) == 4);
    CHECK(volume_mip_size(Format::RGBA8, 4, 4, 4, 0) == 256 && volume_mip_size(Format::RGBA8, 4, 4, 4, 1) == 32);
    CHECK(volume_chain_size(Format::RGBA8, 4, 4, 4, 3) == 256 + 32 + 4);
    CHECK(volume_chain_size(Format::BC1, 8, 8, 2, 2) == 2 * 32 + 8);
    CHECK(max_mip_count_3d(4, 4, 64) == 7 && max_mip_count_3d(16, 2, 1) == 5);
    CHECK(subresource_size(Format::R8, 7, 3) == 21);
    CHECK(row_pitch(Format::R8G8, 7) == 14);
    CHECK(row_pitch(Format::BC2, 9) == 3 * 16);
    CHECK(block_rows(Format::BC1, 9) == 3);
    CHECK(block_rows(Format::RGBA8, 9) == 9);

    CHECK(max_mip_count(1, 1) == 1);
    CHECK(max_mip_count(256, 128) == 9);
    CHECK(max_mip_count(300, 7) == 9);
    CHECK(mip_extent(5, 1) == 2);
    CHECK(mip_extent(1, 3) == 1);
    CHECK(mip_extent(1024, 40) == 1);

    // 256x256 BC1 full chain: 32768 + 8192 + 2048 + 512 + 128 + 32 + 8 + 8 + 8 = 43704
    CHECK(mip_chain_size(Format::BC1, 256, 256, 9) == 43704);
    CHECK(mip_chain_size(Format::RGBA8, 4, 4, 3) == 64 + 16 + 4);
    CHECK(mip_chain_size(Format::RGBA8, 4, 4, 3, 6) == 6 * 84);

    CHECK(aspect_mask(Format::D24S8) == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));
    CHECK(aspect_mask(Format::D32F) == VK_IMAGE_ASPECT_DEPTH_BIT);
    CHECK(aspect_mask(Format::RGBA8) == VK_IMAGE_ASPECT_COLOR_BIT);
    CHECK(align_up(13, 16) == 16 && align_up(32, 16) == 32 && align_up(0, 256) == 0);

    // Swizzles for the D3D9 single/dual-channel formats.
    const VkComponentMapping l8 = component_mapping(Swizzle::RRR1);
    CHECK(l8.r == VK_COMPONENT_SWIZZLE_R && l8.g == VK_COMPONENT_SWIZZLE_R && l8.b == VK_COMPONENT_SWIZZLE_R &&
          l8.a == VK_COMPONENT_SWIZZLE_ONE);
    const VkComponentMapping al = component_mapping(Swizzle::RRRG);
    CHECK(al.r == VK_COMPONENT_SWIZZLE_R && al.b == VK_COMPONENT_SWIZZLE_R && al.a == VK_COMPONENT_SWIZZLE_G);
    const VkComponentMapping a8 = component_mapping(Swizzle::A8);
    CHECK(a8.r == VK_COMPONENT_SWIZZLE_ZERO && a8.g == VK_COMPONENT_SWIZZLE_ZERO && a8.a == VK_COMPONENT_SWIZZLE_R);
    const VkComponentMapping x8 = component_mapping(Swizzle::X8);
    CHECK(x8.r == VK_COMPONENT_SWIZZLE_IDENTITY && x8.a == VK_COMPONENT_SWIZZLE_ONE);
    const VkComponentMapping a4 = component_mapping(Swizzle::ARGB4_IN_BGRA4);
    CHECK(a4.r == VK_COMPONENT_SWIZZLE_G && a4.g == VK_COMPONENT_SWIZZLE_R && a4.b == VK_COMPONENT_SWIZZLE_A &&
          a4.a == VK_COMPONENT_SWIZZLE_B);
    const VkComponentMapping id = component_mapping(Swizzle::Identity);
    CHECK(id.r == VK_COMPONENT_SWIZZLE_IDENTITY && id.a == VK_COMPONENT_SWIZZLE_IDENTITY);

    // D3D9 depth bias -> Vulkan constant factor (units of r).
    const DepthBias d24 = depth_bias_from_d3d9(-0.0001f, 1.5f, Format::D24S8);
    CHECK(d24.constant_factor == -0.0001f * 16777216.0f && d24.slope_factor == 1.5f && d24.clamp == 0.0f);
    CHECK(depth_bias_from_d3d9(1.0f / 65536.0f, 0, Format::D16).constant_factor == 1.0f);
    CHECK(depth_bias_from_d3d9(1.0f / 16777216.0f, 0, Format::D32F).constant_factor == 1.0f);
    CHECK(depth_bias_from_d3d9(0, 0, Format::D32FS8).constant_factor == 0.0f);
}

void test_ring() {
    RingAllocator r(100);
    auto a = r.allocate(40);
    CHECK(a && *a == 0);
    const uint64_t m1 = r.marker();
    auto b = r.allocate(40);
    CHECK(b && *b == 40);
    const uint64_t m2 = r.marker();
    CHECK(!r.allocate(30));  // only 20 left
    CHECK(r.used() == 80);
    r.retire(m1);
    CHECK(r.used() == 40);
    auto c = r.allocate(30);  // does not fit in [80,100): wraps to 0, tail gap counts as used
    CHECK(c && *c == 0);
    CHECK(r.used() == 40 + 20 + 30);
    CHECK(!r.allocate(20));
    r.retire(m2);
    auto d = r.allocate(16, 16);  // phys 30 -> aligned 32
    CHECK(d && *d == 32);
    CHECK(!r.allocate(101));
    r.retire(r.marker());
    CHECK(r.empty());
    // Full-capacity allocation after draining.
    RingAllocator full(64);
    CHECK(full.allocate(64) && full.used() == 64);
    CHECK(!full.allocate(1));
    full.retire(full.marker());
    CHECK(full.allocate(64));

    // Empty ring with the head mid-ring: a large request restarts at 0 instead of failing.
    RingAllocator e(100);
    CHECK(e.allocate(60));
    e.retire(e.marker());
    auto big = e.allocate(80);
    CHECK(big && *big == 0 && e.used() == 80);
    const uint64_t mk = e.marker();
    CHECK(mk > 60);  // positions stay monotonic
    e.retire(mk);
    CHECK(e.empty());
    // Randomized FIFO check: live regions never overlap.
    RingAllocator q(1000);
    struct Live {
        uint64_t off, size, marker;
    };
    std::vector<Live> live;
    uint64_t seed = 12345;
    for (int i = 0; i < 5000; ++i) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        const uint64_t size = 1 + (seed >> 33) % 200;
        const uint64_t align = 1ull << ((seed >> 20) % 5);
        if (auto o = q.allocate(size, align)) {
            CHECK(*o % align == 0 && *o + size <= 1000);
            for (const Live& l : live) CHECK(*o + size <= l.off || l.off + l.size <= *o);
            live.push_back({*o, size, q.marker()});
        } else {
            CHECK(!live.empty() || size > 1000);
            const size_t n = std::max<size_t>(1, live.size() / 2);
            q.retire(live[n - 1].marker);
            live.erase(live.begin(), live.begin() + static_cast<std::ptrdiff_t>(n));
        }
    }
}

void test_sampler_keys() {
    SamplerKey a;
    SamplerKey b;
    CHECK(a == b && a.hash() == b.hash());
    b.lod_bias = 0.5f;
    CHECK(!(a == b));
    SamplerKey c = b;
    c.lod_bias = 0.5001f;  // quantized to 1/256
    CHECK(b == c);
    SamplerKey an0, an1;
    an0.max_anisotropy = 0;
    an1.max_anisotropy = 1;
    CHECK(an0 == an1);  // both "off"
    SamplerKey cmp_off;
    cmp_off.compare_op = VK_COMPARE_OP_GREATER;  // ignored while compare is disabled
    CHECK(cmp_off == SamplerKey{});
    CHECK(SamplerKey::point_clamp().pack() != SamplerKey::linear_clamp().pack());
    CHECK(SamplerKey::point_wrap().pack() != SamplerKey::linear_wrap().pack());

    // Round trip of every field.
    SamplerKey k;
    k.min_filter = VK_FILTER_NEAREST;
    k.mag_filter = VK_FILTER_LINEAR;
    k.mip_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    k.address_u = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    k.address_v = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    k.address_w = VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    k.max_anisotropy = 8;
    k.compare_enable = true;
    k.compare_op = VK_COMPARE_OP_GREATER_OR_EQUAL;
    k.border = VK_BORDER_COLOR_INT_OPAQUE_WHITE;
    k.lod_bias = -1.25f;
    k.min_lod = 2.0f;
    k.max_lod = 7.5f;
    const SamplerKey u = SamplerKey::unpack(k.pack());
    CHECK(u.min_filter == k.min_filter && u.mag_filter == k.mag_filter && u.mip_mode == k.mip_mode);
    CHECK(u.address_u == k.address_u && u.address_v == k.address_v && u.address_w == k.address_w);
    CHECK(u.max_anisotropy == 8 && u.compare_enable && u.compare_op == k.compare_op && u.border == k.border);
    CHECK(u.lod_bias == -1.25f && u.min_lod == 2.0f && u.max_lod == 7.5f);
    CHECK(u.pack() == k.pack());
    CHECK(SamplerKey::unpack(SamplerKey{}.pack()).max_lod == VK_LOD_CLAMP_NONE);

    // Distinct states -> distinct packs and (here) distinct hashes.
    std::set<uint64_t> packs, hashes;
    for (int f = 0; f < 2; ++f)
        for (int m = 0; m < 2; ++m)
            for (int au = 0; au < 5; ++au)
                for (int av = 0; av < 5; ++av)
                    for (int an = 0; an <= 16; an += 4)
                        for (int lb = -2; lb <= 2; ++lb) {
                            SamplerKey s;
                            s.min_filter = s.mag_filter = f ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
                            s.mip_mode = m ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
                            s.address_u = static_cast<VkSamplerAddressMode>(au);
                            s.address_v = static_cast<VkSamplerAddressMode>(av);
                            s.max_anisotropy = static_cast<uint8_t>(an);
                            s.lod_bias = static_cast<float>(lb) * 0.25f;
                            packs.insert(s.pack());
                            hashes.insert(s.hash());
                        }
    CHECK(packs.size() == 2u * 2 * 5 * 5 * 5 * 5);
    CHECK(hashes.size() == packs.size());
}

VkQueueFamilyProperties fam(VkQueueFlags flags, uint32_t count = 1, VkExtent3D gran = {1, 1, 1}) {
    VkQueueFamilyProperties p{};
    p.queueFlags = flags;
    p.queueCount = count;
    p.minImageTransferGranularity = gran;
    return p;
}

void test_queue_selection() {
    constexpr VkQueueFlags G = VK_QUEUE_GRAPHICS_BIT, C = VK_QUEUE_COMPUTE_BIT, T = VK_QUEUE_TRANSFER_BIT;
    {  // AMD-like: universal, async compute, SDMA
        const VkQueueFamilyProperties f[] = {fam(G | C | T), fam(C | T, 4), fam(T, 2)};
        const QueueFamilies q = select_queue_families(f, {});
        CHECK(q.graphics == 0 && q.compute == 1 && q.transfer == 2);
        const QueueFamilies q2 = select_queue_families(f, {}, false, false);
        CHECK(q2.graphics == 0 && q2.compute == 0 && q2.transfer == 0);
    }
    {  // single universal family
        const VkQueueFamilyProperties f[] = {fam(G | C | T, 16)};
        const QueueFamilies q = select_queue_families(f, {});
        CHECK(q.graphics == 0 && q.compute == 0 && q.transfer == 0);
    }
    {  // transfer family with coarse granularity is not used; graphics-only family loses to universal
        const VkQueueFamilyProperties f[] = {fam(G | T), fam(G | C | T), fam(T, 1, {8, 8, 1})};
        const QueueFamilies q = select_queue_families(f, {});
        CHECK(q.graphics == 1 && q.transfer == 1 && q.compute == 1);
    }
    {  // presentation restricts the graphics family
        const VkQueueFamilyProperties f[] = {fam(G | C | T), fam(G | C | T)};
        const bool present[] = {false, true};
        CHECK(select_queue_families(f, present).graphics == 1);
        const bool none[] = {false, false};
        CHECK(!select_queue_families(f, none).valid());
    }
    {  // no graphics at all
        const VkQueueFamilyProperties f[] = {fam(C | T)};
        CHECK(!select_queue_families(f, {}).valid());
    }
}

void test_cache_header() {
    uint8_t uuid[VK_UUID_SIZE];
    for (uint32_t i = 0; i < VK_UUID_SIZE; ++i) uuid[i] = static_cast<uint8_t>(i * 7 + 1);
    std::vector<uint8_t> blob(64, 0);
    auto put = [&](size_t off, uint32_t v) {
        for (int i = 0; i < 4; ++i) blob[off + static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (8 * i));
    };
    put(0, 32);
    put(4, VK_PIPELINE_CACHE_HEADER_VERSION_ONE);
    put(8, 0x1002);
    put(12, 0x7550);
    std::memcpy(blob.data() + 16, uuid, VK_UUID_SIZE);
    CHECK(pipeline_cache_header_matches(blob, 0x1002, 0x7550, uuid));
    CHECK(!pipeline_cache_header_matches(blob, 0x10DE, 0x7550, uuid));
    CHECK(!pipeline_cache_header_matches(blob, 0x1002, 0x7551, uuid));
    uint8_t other[VK_UUID_SIZE] = {};
    CHECK(!pipeline_cache_header_matches(blob, 0x1002, 0x7550, other));
    CHECK(!pipeline_cache_header_matches(std::span<const uint8_t>(blob.data(), 20), 0x1002, 0x7550, uuid));
    put(0, 200);  // header larger than blob
    CHECK(!pipeline_cache_header_matches(blob, 0x1002, 0x7550, uuid));
}

// On-disk pipeline cache wrapper: length + checksum around the driver blob.
void test_cache_file_wrap() {
    std::vector<uint8_t> blob(300);
    for (size_t i = 0; i < blob.size(); ++i) blob[i] = static_cast<uint8_t>(i * 13 + 5);
    const std::vector<uint8_t> file = pipeline_cache_wrap(blob);
    CHECK(file.size() == blob.size() + kPipelineCacheFileHeader);
    std::span<const uint8_t> out;
    CHECK(pipeline_cache_unwrap(file, out));
    CHECK(out.size() == blob.size() && std::memcmp(out.data(), blob.data(), blob.size()) == 0);
    // Truncated anywhere (header or body), trailing garbage, flipped body/header bytes: rejected.
    for (size_t n : {size_t{0}, size_t{3}, kPipelineCacheFileHeader - 1, kPipelineCacheFileHeader, file.size() - 1}) {
        std::span<const uint8_t> o2;
        CHECK(!pipeline_cache_unwrap(std::span<const uint8_t>(file.data(), n), o2));
        CHECK(o2.empty());
    }
    std::vector<uint8_t> longer = file;
    longer.push_back(0);
    CHECK(!pipeline_cache_unwrap(longer, out));
    for (size_t pos : {size_t{0}, size_t{5}, size_t{9}, size_t{17}, kPipelineCacheFileHeader + 100}) {
        std::vector<uint8_t> bad = file;
        bad[pos] ^= 0x40;
        std::span<const uint8_t> o3;
        CHECK(!pipeline_cache_unwrap(bad, o3));
    }
    // A size field that would overflow size arithmetic is rejected, not wrapped.
    std::vector<uint8_t> huge = file;
    for (size_t i = 8; i < 16; ++i) huge[i] = 0xFF;
    CHECK(!pipeline_cache_unwrap(huge, out));
    // An empty driver blob round-trips.
    const std::vector<uint8_t> empty = pipeline_cache_wrap({});
    std::span<const uint8_t> e;
    CHECK(pipeline_cache_unwrap(empty, e) && e.empty());
}

// Deferred slot reuse: a released slot only comes back after collect() sees its retire value.
void test_bindless_slots() {
    BindlessSlotAllocator a;
    a.init(5, 1);  // slot 0 reserved (white)
    CHECK(a.capacity() == 5 && a.available() == 4 && a.high_water() == 1 && a.live() == 0);
    CHECK(a.allocate() == 1 && a.allocate() == 2 && a.allocate() == 3 && a.allocate() == 4);
    CHECK(a.allocate() == kInvalidBindlessSlot);  // exhausted
    CHECK(a.live() == 4 && a.available() == 0 && a.high_water() == 5);
    std::printf("  (4 bindless release errors are expected next)\n");
    std::fflush(stdout);
    CHECK(!a.release(0, 1));  // reserved
    CHECK(!a.release(9, 1));  // out of range
    CHECK(a.release(2, 10));  // in use by frames up to 10
    CHECK(!a.release(2, 10));  // double release
    CHECK(a.pending() == 1 && a.live() == 3);
    CHECK(a.allocate() == kInvalidBindlessSlot);  // pending slots are not reusable: still exhausted
    CHECK(a.collect(9) == 0 && a.allocate() == kInvalidBindlessSlot);
    CHECK(a.release(4, 11));
    CHECK(a.release(1, 10));
    std::vector<uint32_t> retired;
    CHECK(a.collect(10, &retired) == 2);
    CHECK(retired.size() == 2 && retired[0] == 2 && retired[1] == 1);  // release order
    CHECK(a.pending() == 1 && a.available() == 2);
    CHECK(a.allocate() == 2 && a.allocate() == 1 && a.allocate() == kInvalidBindlessSlot);
    CHECK(a.collect(11) == 1 && a.allocate() == 4);
    CHECK(a.live() == 4 && a.pending() == 0 && a.available() == 0);
    CHECK(!a.release(3 + 3, 0));  // out of range again (errors counted above: 0, 9, 2, 6)
    // Release before any frame (value 0) retires on the first collect.
    CHECK(a.release(3, 0) && a.collect(0) == 1 && a.allocate() == 3);
    // Retired slots are reused before fresh ones (oldest retirement first).
    BindlessSlotAllocator b;
    b.init(4, 0);
    CHECK(b.allocate() == 0 && b.allocate() == 1 && b.release(1, 1) && b.release(0, 1) && b.collect(1) == 2);
    CHECK(b.high_water() == 2 && b.available() == 4);
    CHECK(b.allocate() == 1 && b.allocate() == 0 && b.allocate() == 2 && b.allocate() == 3);
    CHECK(b.allocate() == kInvalidBindlessSlot && b.high_water() == 4);
    BindlessSlotAllocator empty;
    CHECK(empty.allocate() == kInvalidBindlessSlot && empty.collect(100) == 0);
}

// Steady streaming (1 release + 1 allocate per frame, 2 frames in flight) for 1e6 frames: storage
// and the high-water mark stay bounded by the live + pending peak, never creeping to capacity.
void test_bindless_slots_long_run() {
    constexpr uint32_t kCapacity = 64, kReserved = 1, kLive = 5, kInFlight = 2;
    BindlessSlotAllocator a;
    a.init(kCapacity, kReserved);
    std::vector<uint32_t> live;  // FIFO of live slots
    for (uint32_t i = 0; i < kLive; ++i) live.push_back(a.allocate());
    const size_t bytes0 = a.storage_bytes();
    bool ok = true;
    size_t head = 0;
    for (uint64_t frame = 1; frame <= 1000000 && ok; ++frame) {
        if (!a.release(live[head], frame)) ok = false;
        a.collect(frame > kInFlight ? frame - kInFlight : 0);
        const uint32_t s = a.allocate();
        if (s == kInvalidBindlessSlot || s < kReserved) ok = false;
        live[head] = s;
        head = (head + 1) % live.size();
        if (a.storage_bytes() != bytes0 || a.high_water() > kReserved + kLive + kInFlight) ok = false;
    }
    CHECK(ok);
    CHECK(a.storage_bytes() == bytes0 && bytes0 <= 2 * kCapacity * (1 + 4 + 16));
    CHECK(a.high_water() <= kReserved + kLive + kInFlight && a.live() == kLive && a.pending() <= kInFlight);
}

void test_surface_platforms() {
    CHECK(std::strcmp(platform_surface_extension(), "VK_KHR_win32_surface") == 0);
}

// Mouse state through the window procedure entry (hidden window; message ids from WinUser.h).
void test_window_mouse() {
    Window w;
    WindowDesc wd;
    wd.visible = false;
    wd.width = 320;
    wd.height = 200;
    CHECK(w.create(wd));
    if (!w.native_handle()) return;
    constexpr uint32_t kMove = 0x0200, kLDown = 0x0201, kLUp = 0x0202, kRDown = 0x0204, kRUp = 0x0205, kWheel = 0x020A;
    w.handle_message(kMove, 0, (40 & 0xFFFF) | (25 << 16));
    CHECK(w.mouse_x() == 40 && w.mouse_y() == 25);
    w.handle_message(kMove, 0, 0xFFFF | (7 << 16));  // x = -1 (outside, while captured)
    CHECK(w.mouse_x() == -1 && w.mouse_y() == 7);
    w.handle_message(kLDown, 1, 0);
    CHECK(w.mouse_down(MouseButton::Left) && w.mouse_pressed(MouseButton::Left) && !w.mouse_pressed(MouseButton::Left));
    w.handle_message(kRDown, 2, 0);
    w.handle_message(kLUp, 0, 0);
    CHECK(!w.mouse_down(MouseButton::Left) && w.mouse_released(MouseButton::Left) && w.mouse_down(MouseButton::Right));
    w.handle_message(kRUp, 0, 0);
    CHECK(!w.mouse_down(MouseButton::Right) && w.mouse_released(MouseButton::Right));
    w.handle_message(kWheel, static_cast<uintptr_t>(120u << 16), 0);
    w.handle_message(kWheel, static_cast<uintptr_t>(240u << 16), 0);
    w.handle_message(kWheel, static_cast<uintptr_t>(0xFF88u << 16), 0);  // -120
    CHECK(w.take_wheel() == 2.0f && w.take_wheel() == 0.0f);
    w.destroy();
}

// Integration fixes: WM_CHAR / WM_UNICHAR text input (UTF-16 pairs joined, lone surrogates dropped, UTF-8 view, cap).
void test_window_text() {
    Window w;  // handle_message needs no native window for text
    constexpr uint32_t kChar = 0x0102, kUniChar = 0x0109;
    for (const uint32_t u : {0x41u, 0xE9u, 0x08u, 0xD83Du, 0xDE00u, 0xDC00u /* lone low */, 0xD800u /* lone high */, 0x5Au})
        CHECK(w.handle_message(kChar, u, 0) == 0);
    CHECK(w.handle_message(kUniChar, 0xFFFF /* UNICODE_NOCHAR */, 0) == 1);
    CHECK(w.handle_message(kUniChar, 0x1F680, 0) == 0);
    const std::u32string t = w.take_text();
    CHECK(t == std::u32string({U'A', U'é', U'\b', U'\U0001F600', U'Z', U'\U0001F680'}));
    CHECK(w.take_text().empty());
    w.handle_message(kChar, 0x41, 0);
    w.handle_message(kChar, 0xE9, 0);
    w.handle_message(kChar, 0xD83D, 0);
    w.handle_message(kChar, 0xDE00, 0);
    CHECK(w.take_text_utf8() == "A\xC3\xA9\xF0\x9F\x98\x80");
    for (uint32_t i = 0; i < Window::kMaxTextPerFrame + 10; ++i) w.handle_message(kChar, 0x30u + (i % 10), 0);
    const std::u32string capped = w.take_text();
    CHECK(capped.size() == Window::kMaxTextPerFrame && capped.front() == U'0');  // the 10 oldest dropped
}

int run_cpu() {
    test_window_mouse();
    test_window_text();
    test_formats();
    test_ring();
    test_sampler_keys();
    test_queue_selection();
    test_cache_header();
    test_cache_file_wrap();
    test_bindless_slots();
    test_bindless_slots_long_run();
    test_surface_platforms();
    if (g_fail) {
        std::printf("alamo_rhi_tests cpu: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("alamo_rhi_tests cpu: OK\n");
    return 0;
}

// =============================================================================================
// GPU tests
// =============================================================================================

#ifdef ALAMO_RHI_HAVE_SHADERS

constexpr uint32_t kSize = 64;

struct Rgba {
    uint8_t r, g, b, a;
};
bool close_to(Rgba p, Rgba q) {
    auto d = [](uint8_t x, uint8_t y) { return x > y ? x - y : y - x; };
    return d(p.r, q.r) <= 2 && d(p.g, q.g) <= 2 && d(p.b, q.b) <= 2 && d(p.a, q.a) <= 2;
}

struct Gpu {
    Context ctx;
    Allocator alloc;
    StandardLayouts layouts;
    SamplerCache samplers;
    PipelineCache cache;
    DescriptorAllocator descriptors;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline tri = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    Image target;
    Buffer readback;
    std::vector<uint32_t> tri_vs, tri_ps, fs_vs, fs_ps;

    Rgba pixel(uint32_t x, uint32_t y) const {
        const auto* p = static_cast<const uint8_t*>(readback.mapped()) + (static_cast<size_t>(y) * kSize + x) * 4;
        return {p[0], p[1], p[2], p[3]};
    }
};

void record_begin(Gpu& g, VkCommandBuffer cmd, float r, float gr, float b) {
    const DeviceFns& fn = g.ctx.vkd();
    cmd_barrier(fn, cmd, transition(g.target.handle, ImageState::Undefined, ImageState::ColorAttachment));
    auto ca = vk_struct<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
    ca.imageView = g.target.view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color = {{r, gr, b, 1.0f}};
    auto ri = vk_struct<VkRenderingInfo>(VK_STRUCTURE_TYPE_RENDERING_INFO);
    ri.renderArea = {{0, 0}, {kSize, kSize}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    fn.vkCmdBeginRendering(cmd, &ri);
    const VkViewport vp{0, 0, static_cast<float>(kSize), static_cast<float>(kSize), 0, 1};
    const VkRect2D sc{{0, 0}, {kSize, kSize}};
    fn.vkCmdSetViewport(cmd, 0, 1, &vp);
    fn.vkCmdSetScissor(cmd, 0, 1, &sc);
}

void record_end_readback(Gpu& g, VkCommandBuffer cmd) {
    const DeviceFns& fn = g.ctx.vkd();
    fn.vkCmdEndRendering(cmd);
    cmd_barrier(fn, cmd, transition(g.target.handle, ImageState::ColorAttachment, ImageState::TransferSrc));
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {kSize, kSize, 1};
    fn.vkCmdCopyImageToBuffer(cmd, g.target.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.readback.handle, 1, &copy);
    const VkMemoryBarrier2 mb = memory_barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                               VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    cmd_barrier(fn, cmd, {}, {}, std::span<const VkMemoryBarrier2>(&mb, 1));
}

struct PushData {
    float color[4];
    float lod;
};

void record_triangle(Gpu& g, VkCommandBuffer cmd, const float color[4]) {
    const DeviceFns& fn = g.ctx.vkd();
    fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.tri);
    PushData pd{{color[0], color[1], color[2], color[3]}, 0.0f};
    fn.vkCmdPushConstants(cmd, g.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pd), &pd);
    fn.vkCmdDraw(cmd, 3, 1, 0, 0);
}

void record_textured(Gpu& g, VkCommandBuffer cmd, const Image& tex, float lod) {
    const DeviceFns& fn = g.ctx.vkd();
    VkDescriptorSet set = g.descriptors.allocate(g.layouts.textures());
    CHECK(set != VK_NULL_HANDLE);
    const TextureBinding tb{0, tex.view, g.samplers.get(SamplerKey::point_clamp())};
    write_textures(g.ctx, set, {&tb, 1});
    fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.fs);
    fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, kSetTextures, 1, &set, 0, nullptr);
    PushData pd{{1, 1, 1, 1}, lod};
    fn.vkCmdPushConstants(cmd, g.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pd), &pd);
    fn.vkCmdDraw(cmd, 3, 1, 0, 0);
}

// Draw the textured full-screen triangle at `lod` and read the target back (blocking).
bool draw_textured(Gpu& g, const Image& tex, float lod) {
    g.descriptors.reset();
    return g.ctx.immediate_submit([&](VkCommandBuffer cmd) {
        record_begin(g, cmd, 0, 0, 0);
        record_textured(g, cmd, tex, lod);
        record_end_readback(g, cmd);
    });
}

bool make_pipelines(Gpu& g) {
    g.tri_vs = spirv_from_bytes({g_tri_vs, sizeof(g_tri_vs)});
    g.tri_ps = spirv_from_bytes({g_tri_ps, sizeof(g_tri_ps)});
    g.fs_vs = spirv_from_bytes({g_fs_vs, sizeof(g_fs_vs)});
    g.fs_ps = spirv_from_bytes({g_fs_ps, sizeof(g_fs_ps)});
    g.layout = g.layouts.create_pipeline_layout(sizeof(PushData));
    if (!g.layout) return false;
    const VkFormat color = VK_FORMAT_R8G8B8A8_UNORM;
    GraphicsPipelineDesc d;
    d.vs_spirv = g.tri_vs;
    d.fs_spirv = g.tri_ps;
    d.vs_entry = "tri_vs";
    d.fs_entry = "tri_ps";
    d.raster.cull = VK_CULL_MODE_NONE;
    d.color_formats = {&color, 1};
    d.layout = g.layout;
    d.name = "test triangle";
    g.tri = g.cache.create_graphics(d);
    d.vs_spirv = g.fs_vs;
    d.fs_spirv = g.fs_ps;
    d.vs_entry = "fs_vs";
    d.fs_entry = "fs_ps";
    d.name = "test fullscreen";
    g.fs = g.cache.create_graphics(d);
    return g.tri && g.fs;
}

void test_memory(Gpu& g) {
    const MemoryStats before = g.alloc.stats();
    Buffer b[4];
    const MemoryUsage usages[4] = {MemoryUsage::GpuOnly, MemoryUsage::Upload, MemoryUsage::Dynamic, MemoryUsage::Readback};
    for (int i = 0; i < 4; ++i) {
        CHECK(g.alloc.create_buffer(4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, usages[i], b[i]));
        CHECK(b[i].alloc.size >= 4096);
    }
    CHECK(b[1].mapped() && b[2].mapped() && b[3].mapped());
    // Dynamic (frame uniform ring) must be HOST_COHERENT: it is written without flushes.
    CHECK(g.ctx.memory_properties().memoryTypes[b[2].alloc.memory_type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(g.alloc.stats().allocation_count == before.allocation_count + 4);
    // Map/unmap through the wrapper (ref-counted, also fine on persistently mapped memory).
    void* p = g.alloc.map(b[1].alloc);
    CHECK(p == b[1].mapped());
    std::memset(p, 0xAB, 4096);
    CHECK(g.alloc.flush(b[1].alloc));
    g.alloc.unmap(b[1].alloc);
    // Raw allocation for a caller-bound resource.
    VkMemoryRequirements reqs{65536, 256, 0xFFFFFFFFu};
    Allocation raw;
    CHECK(g.alloc.allocate(reqs, MemoryUsage::Readback, raw));
    CHECK(raw && raw.mapped && raw.memory && raw.size >= 65536);
    CHECK(g.alloc.invalidate(raw));
    g.alloc.free(raw);
    CHECK(!raw);
    // Caller-bound buffer: raw allocation + vkBindBufferMemory from the table.
    {
        const DeviceFns& fn = g.ctx.vkd();
        auto bci = vk_struct<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
        bci.size = 1024;
        bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        VkBuffer vb = VK_NULL_HANDLE;
        CHECK(fn.vkCreateBuffer(g.ctx.device(), &bci, nullptr, &vb) == VK_SUCCESS);
        VkMemoryRequirements mr{};
        fn.vkGetBufferMemoryRequirements(g.ctx.device(), vb, &mr);
        Allocation ba;
        CHECK(g.alloc.allocate(mr, MemoryUsage::Upload, ba));
        CHECK(fn.vkBindBufferMemory(g.ctx.device(), vb, ba.memory, ba.offset) == VK_SUCCESS);
        fn.vkDestroyBuffer(g.ctx.device(), vb, nullptr);
        g.alloc.free(ba);
    }
    for (auto& x : b) g.alloc.destroy(x);
    CHECK(g.alloc.stats().allocation_count == before.allocation_count);
}

void test_triangle(Gpu& g) {
    const float green[4] = {0, 1, 0, 1};
    CHECK(g.ctx.immediate_submit([&](VkCommandBuffer cmd) {
        record_begin(g, cmd, 0, 0, 1);
        record_triangle(g, cmd, green);
        record_end_readback(g, cmd);
    }));
    const Rgba G{0, 255, 0, 255}, B{0, 0, 255, 255};
    CHECK(close_to(g.pixel(2, 2), B));
    CHECK(close_to(g.pixel(61, 61), B));
    CHECK(close_to(g.pixel(32, 32), G));
    CHECK(close_to(g.pixel(32, 20), G));
    CHECK(close_to(g.pixel(20, 20), B));
    CHECK(close_to(g.pixel(22, 40), G));
    CHECK(close_to(g.pixel(10, 40), B));
    CHECK(close_to(g.pixel(32, 60), B));
    uint32_t greens = 0;
    for (uint32_t y = 0; y < kSize; ++y)
        for (uint32_t x = 0; x < kSize; ++x) greens += close_to(g.pixel(x, y), G) ? 1u : 0u;
    // Area: base 32 px, height 32 px -> 512 px (edge rules shift a few).
    CHECK(greens > 480 && greens < 544);
    std::printf("  triangle: %u green pixels\n", greens);
}

std::vector<uint8_t> rgba_mips() {
    // mip0 4x4 quadrants red/green/blue/white, mip1 2x2 yellow, mip2 1x1 magenta.
    std::vector<uint8_t> d;
    for (uint32_t y = 0; y < 4; ++y)
        for (uint32_t x = 0; x < 4; ++x) {
            const bool right = x >= 2, bottom = y >= 2;
            Rgba c = !bottom ? (!right ? Rgba{255, 0, 0, 255} : Rgba{0, 255, 0, 255})
                             : (!right ? Rgba{0, 0, 255, 255} : Rgba{255, 255, 255, 255});
            d.insert(d.end(), {c.r, c.g, c.b, c.a});
        }
    for (int i = 0; i < 4; ++i) d.insert(d.end(), {255, 255, 0, 255});
    d.insert(d.end(), {255, 0, 255, 255});
    return d;
}

void check_rgba_texture(Gpu& g, const Image& tex) {
    CHECK(draw_textured(g, tex, 0.0f));
    CHECK(close_to(g.pixel(16, 16), Rgba{255, 0, 0, 255}));
    CHECK(close_to(g.pixel(48, 16), Rgba{0, 255, 0, 255}));
    CHECK(close_to(g.pixel(16, 48), Rgba{0, 0, 255, 255}));
    CHECK(close_to(g.pixel(48, 48), Rgba{255, 255, 255, 255}));
    CHECK(draw_textured(g, tex, 1.0f));
    CHECK(close_to(g.pixel(5, 50), Rgba{255, 255, 0, 255}));
    CHECK(draw_textured(g, tex, 2.0f));
    CHECK(close_to(g.pixel(40, 9), Rgba{255, 0, 255, 255}));
}

// Uploads through the FrameRing path: acquires recorded in the frame command buffer and the frame
// submit waiting on the upload timeline.
void test_upload_frame_path(Gpu& g, bool use_transfer_queue) {
    Uploader up;
    UploaderDesc ud;
    ud.use_transfer_queue = use_transfer_queue;
    CHECK(up.init(g.ctx, g.alloc, ud));
    FrameRing ring;
    FrameRingDesc fd;
    fd.frames_in_flight = 2;
    fd.recording_threads = 2;
    CHECK(ring.init(g.ctx, g.alloc, fd));

    ImageDesc id;
    id.format = Format::RGBA8;
    id.width = id.height = 4;
    id.mips = 3;
    id.name = "test rgba";
    Image tex;
    CHECK(g.alloc.create_image(id, tex));
    const std::vector<uint8_t> data = rgba_mips();
    const UploadTicket t = up.upload_image_chain(tex, data);
    CHECK(t);
    CHECK(up.wait(t));
    CHECK(up.is_complete(t));
    CHECK(!use_transfer_queue || !g.ctx.has_transfer_queue() || !up.ready(t));  // needs the acquire first

    // Frame 1: acquire + triangle; frame 2: secondary thread primary; frame 3: textured draw.
    for (int frame = 0; frame < 3; ++frame) {
        Frame* f = ring.begin_frame();
        CHECK(f != nullptr);
        if (!f) break;
        CHECK(f->number() == static_cast<uint64_t>(frame + 1));
        const AcquireResult acq = up.record_acquires(f->cmd());
        if (up.dedicated_queue()) CHECK(frame != 0 || acq.barriers == tex.mips);  // one per uploaded subresource
        UniformAlloc ua = f->alloc_uniform(100);
        CHECK(ua && ua.offset % g.ctx.caps().min_uniform_buffer_offset_alignment == 0);
        CHECK(f->alloc_uniform(100).offset >= ua.offset + 100);
        CHECK(!f->alloc_uniform(~VkDeviceSize{0} - 4));  // off + size would wrap: rejected
        CHECK(!f->alloc_storage(~VkDeviceSize{0}));
        const UniformAlloc sa = f->alloc_storage(48);
        CHECK(sa && sa.offset % g.ctx.caps().min_storage_buffer_offset_alignment == 0 &&
              sa.offset % g.ctx.caps().min_uniform_buffer_offset_alignment == 0);
        std::vector<VkSemaphoreSubmitInfo> waits;
        if (acq.wait_value)
            waits.push_back(semaphore_info(up.timeline(), acq.wait_value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT));
        VkCommandBuffer extra = VK_NULL_HANDLE;
        if (frame < 2) {
            const float c[4] = {1, 0, 0, 1};
            record_begin(g, f->cmd(), 0, 0, 0);
            record_triangle(g, f->cmd(), c);
            if (frame == 1) {  // read back from a second thread's primary (recorded on another thread)
                g.ctx.vkd().vkCmdEndRendering(f->cmd());
                std::thread th([&] {
                    extra = f->primary(1);
                    cmd_barrier(g.ctx.vkd(), extra,
                                transition(g.target.handle, ImageState::ColorAttachment, ImageState::ColorAttachment));
                    auto ca = vk_struct<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
                    ca.imageView = g.target.view;
                    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    auto ri = vk_struct<VkRenderingInfo>(VK_STRUCTURE_TYPE_RENDERING_INFO);
                    ri.renderArea = {{0, 0}, {kSize, kSize}};
                    ri.layerCount = 1;
                    ri.colorAttachmentCount = 1;
                    ri.pColorAttachments = &ca;
                    g.ctx.vkd().vkCmdBeginRendering(extra, &ri);
                    record_end_readback(g, extra);
                    g.ctx.vkd().vkEndCommandBuffer(extra);
                });
                th.join();
            } else {
                record_end_readback(g, f->cmd());
            }
        } else {
            CHECK(up.ready(t));
            record_begin(g, f->cmd(), 0, 0, 0);
            VkDescriptorSet set = f->descriptors(0).allocate(g.layouts.textures());
            CHECK(set != VK_NULL_HANDLE);
            const TextureBinding tb{0, tex.view, g.samplers.get(SamplerKey::point_clamp())};
            write_textures(g.ctx, set, {&tb, 1});
            g.ctx.vkd().vkCmdBindPipeline(f->cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS, g.fs);
            g.ctx.vkd().vkCmdBindDescriptorSets(f->cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, kSetTextures, 1,
                                                &set, 0, nullptr);
            PushData pd{{1, 1, 1, 1}, 0.0f};
            g.ctx.vkd().vkCmdPushConstants(f->cmd(), g.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                           0, sizeof(pd), &pd);
            g.ctx.vkd().vkCmdDraw(f->cmd(), 3, 1, 0, 0);
            record_end_readback(g, f->cmd());
        }
        FrameSubmit fs;
        fs.waits = waits;
        if (extra) fs.extra = {&extra, 1};
        CHECK(ring.end_frame(*f, fs));
        CHECK(ring.wait_idle());
        if (frame < 2) {
            CHECK(close_to(g.pixel(32, 32), Rgba{255, 0, 0, 255}));
            CHECK(close_to(g.pixel(2, 2), Rgba{0, 0, 0, 255}));
        } else {
            CHECK(close_to(g.pixel(16, 16), Rgba{255, 0, 0, 255}));
            CHECK(close_to(g.pixel(48, 48), Rgba{255, 255, 255, 255}));
        }
    }
    CHECK(ring.completed_value() == 3);
    ring.shutdown();
    // Also through the blocking helper + all mips.
    check_rgba_texture(g, tex);
    g.alloc.destroy(tex);
    up.shutdown();
}

void test_upload_bc1(Gpu& g, Uploader& up) {
    if (!g.ctx.caps().texture_bc) {
        std::printf("  BC1: textureCompressionBC unsupported, skipped\n");
        return;
    }
    ImageDesc id;
    id.format = Format::BC1;
    id.width = id.height = 8;
    id.mips = 0;  // full chain: 8x8, 4x4, 2x2, 1x1
    Image tex;
    CHECK(g.alloc.create_image(id, tex));
    CHECK(tex.mips == 4);
    auto block = [](uint16_t c) {
        return std::vector<uint8_t>{static_cast<uint8_t>(c), static_cast<uint8_t>(c >> 8), static_cast<uint8_t>(c),
                                    static_cast<uint8_t>(c >> 8), 0, 0, 0, 0};
    };
    std::vector<uint8_t> mip0, mip1 = block(0x07E0), mip2 = block(0x001F), mip3 = block(0xFFFF);
    for (int i = 0; i < 4; ++i) {
        const auto b = block(0xF800);
        mip0.insert(mip0.end(), b.begin(), b.end());
    }
    const SubresourceData subs[] = {{0, 0, mip0}, {1, 0, mip1}, {2, 0, mip2}, {3, 0, mip3}};
    const UploadTicket t = up.upload_image(tex, subs);
    CHECK(t && up.finish(t) && up.ready(t));
    CHECK(draw_textured(g, tex, 0.0f));
    CHECK(close_to(g.pixel(30, 30), Rgba{255, 0, 0, 255}));
    CHECK(draw_textured(g, tex, 1.0f));
    CHECK(close_to(g.pixel(30, 30), Rgba{0, 255, 0, 255}));
    CHECK(draw_textured(g, tex, 2.0f));
    CHECK(close_to(g.pixel(30, 30), Rgba{0, 0, 255, 255}));
    CHECK(draw_textured(g, tex, 3.0f));
    CHECK(close_to(g.pixel(30, 30), Rgba{255, 255, 255, 255}));
    // Wrong size is rejected.
    std::printf("  (an upload_image size error is expected next)\n");
    std::fflush(stdout);
    const SubresourceData bad[] = {{0, 0, mip1}};
    CHECK(!up.upload_image(tex, bad));
    g.alloc.destroy(tex);
}

// Several threads stream buffers through a small ring (forces chunking, wrap-around and waits).
void test_upload_threads(Gpu& g, bool use_transfer_queue) {
    Uploader up;
    UploaderDesc ud;
    ud.ring_bytes = 1ull << 20;
    ud.use_transfer_queue = use_transfer_queue;
    CHECK(up.init(g.ctx, g.alloc, ud));
    constexpr int kThreads = 4, kPerThread = 6;
    constexpr size_t kBytes = 300 * 1024 + 12;
    std::vector<Buffer> bufs(kThreads * kPerThread);
    for (auto& b : bufs)
        CHECK(g.alloc.create_buffer(kBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                    MemoryUsage::GpuOnly, b));
    std::vector<UploadTicket> tickets(bufs.size());
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&, t] {
            std::vector<uint8_t> data(kBytes);
            for (int i = 0; i < kPerThread; ++i) {
                const size_t idx = static_cast<size_t>(t * kPerThread + i);
                for (size_t k = 0; k < kBytes; ++k) data[k] = static_cast<uint8_t>((k * 31 + idx * 7) >> 2);
                tickets[idx] = up.upload_buffer(bufs[idx], 0, data, VK_PIPELINE_STAGE_2_COPY_BIT,
                                                VK_ACCESS_2_TRANSFER_READ_BIT);
            }
        });
    for (auto& th : threads) th.join();
    uint64_t last = 0;
    for (const auto& tk : tickets) {
        CHECK(tk);
        last = std::max(last, tk.value);
    }
    CHECK(up.finish(UploadTicket{last}));
    for (const auto& tk : tickets) CHECK(up.ready(tk));

    Buffer rb;
    CHECK(g.alloc.create_buffer(kBytes * bufs.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::Readback, rb));
    CHECK(g.ctx.immediate_submit([&](VkCommandBuffer cmd) {
        for (size_t i = 0; i < bufs.size(); ++i) {
            const VkBufferCopy c{0, i * kBytes, kBytes};
            g.ctx.vkd().vkCmdCopyBuffer(cmd, bufs[i].handle, rb.handle, 1, &c);
        }
        const VkMemoryBarrier2 mb = memory_barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                                   VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
        cmd_barrier(g.ctx.vkd(), cmd, {}, {}, std::span<const VkMemoryBarrier2>(&mb, 1));
    }));
    g.alloc.invalidate(rb.alloc);
    const auto* p = static_cast<const uint8_t*>(rb.mapped());
    size_t bad = 0;
    for (size_t i = 0; i < bufs.size(); ++i)
        for (size_t k = 0; k < kBytes; ++k)
            if (p[i * kBytes + k] != static_cast<uint8_t>((k * 31 + i * 7) >> 2)) ++bad;
    CHECK(bad == 0);
    std::printf("  threaded upload (%s): %zu buffers x %zu bytes, last ticket %llu, %zu bad bytes\n",
                up.dedicated_queue() ? "transfer queue" : "graphics queue", bufs.size(), kBytes,
                static_cast<unsigned long long>(last), bad);
    g.alloc.destroy(rb);
    for (auto& b : bufs) g.alloc.destroy(b);
    up.shutdown();
}

Image make_rgba_texture(Gpu& g, Uploader& up) {
    ImageDesc id;
    id.format = Format::RGBA8;
    id.width = id.height = 4;
    id.mips = 3;
    Image tex;
    CHECK(g.alloc.create_image(id, tex));
    const std::vector<uint8_t> data = rgba_mips();
    const UploadTicket t = up.upload_image_chain(tex, data);
    CHECK(t && up.finish(t));
    return tex;
}

// Track A hot-set layouts: bindless Texture2D[] (update-after-bind) + Samplers[4] + 16-byte push.
void test_hot_set(Gpu& g) {
    Uploader up;
    CHECK(up.init(g.ctx, g.alloc));
    Image tex = make_rgba_texture(g, up);
    HotSetLayouts hot;
    CHECK(hot.init(g.ctx, 1024));
    CHECK(hot.texture_capacity() == 1024);
    VkPipelineLayout layout = hot.create_pipeline_layout();
    const VkFormat color = VK_FORMAT_R8G8B8A8_UNORM;
    const std::vector<uint32_t> bl_ps = spirv_from_bytes({g_bl_ps, sizeof(g_bl_ps)});
    GraphicsPipelineDesc d;
    d.vs_spirv = g.fs_vs;
    d.fs_spirv = bl_ps;
    d.vs_entry = "fs_vs";
    d.fs_entry = "bl_ps";
    d.raster.cull = VK_CULL_MODE_NONE;
    d.color_formats = {&color, 1};
    d.layout = layout;
    VkPipeline pipe = layout ? g.cache.create_graphics(d) : VK_NULL_HANDLE;
    CHECK(pipe != VK_NULL_HANDLE);

    DescriptorAllocator bindless;
    CHECK(bindless.init(g.ctx, hot.bindless_pool_ratios()));
    VkDescriptorSet set1 = bindless.allocate(hot.textures());
    CHECK(set1 != VK_NULL_HANDLE);
    DescriptorAllocator per_frame;
    CHECK(per_frame.init(g.ctx));
    CHECK(per_frame.allocate(hot.buffers()) != VK_NULL_HANDLE);  // set 0 comes from the frame allocator
    const VkSampler smp[4] = {g.samplers.get(SamplerKey::point_clamp()), g.samplers.get(SamplerKey::point_wrap()),
                              g.samplers.get(SamplerKey::linear_clamp()), g.samplers.get(SamplerKey::linear_wrap())};
    write_bindless_samplers(g.ctx, set1, smp);
    write_bindless_texture(g.ctx, set1, 7, tex.view);

    auto draw = [&](uint32_t index, bool write_after_bind) {
        return g.ctx.immediate_submit([&](VkCommandBuffer cmd) {
            const DeviceFns& fn = g.ctx.vkd();
            record_begin(g, cmd, 0, 0, 0);
            fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &set1, 0, nullptr);
            const uint32_t pc[4] = {index, 7, 0, 0};  // DrawIndex, AlphaFunc (always), AlphaRef, Flags (sampler 0)
            fn.vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, pc);
            fn.vkCmdDraw(cmd, 3, 1, 0, 0);
            record_end_readback(g, cmd);
            // Update-after-bind: the slot is written after the set was bound, before submit.
            if (write_after_bind) write_bindless_texture(g.ctx, set1, index, tex.view);
        });
    };
    if (pipe) {
        CHECK(draw(7, false));
        CHECK(close_to(g.pixel(16, 16), Rgba{255, 0, 0, 255}));
        CHECK(close_to(g.pixel(48, 48), Rgba{255, 255, 255, 255}));
        CHECK(draw(1000, true));
        CHECK(close_to(g.pixel(48, 16), Rgba{0, 255, 0, 255}));
        CHECK(close_to(g.pixel(16, 48), Rgba{0, 0, 255, 255}));
    }
    g.ctx.wait_idle();
    per_frame.shutdown();
    bindless.shutdown();
    if (pipe) g.ctx.vkd().vkDestroyPipeline(g.ctx.device(), pipe, nullptr);
    if (layout) g.ctx.vkd().vkDestroyPipelineLayout(g.ctx.device(), layout, nullptr);
    hot.shutdown();
    up.forget(tex.handle);
    g.alloc.destroy(tex);
    up.shutdown();
}

// BindlessTextureTable through a FrameRing: release fenced by the frame timeline, the default
// (white) descriptor written into the slot on retirement, then reuse.
void test_bindless_recycle(Gpu& g) {
    Uploader up;
    CHECK(up.init(g.ctx, g.alloc));
    Image tex = make_rgba_texture(g, up);
    Image white;
    {
        ImageDesc id;
        id.format = Format::RGBA8;
        id.width = id.height = 1;
        CHECK(g.alloc.create_image(id, white));
        const uint8_t px[4] = {255, 255, 255, 255};
        const UploadTicket t = up.upload_image_chain(white, px);
        CHECK(t && up.finish(t));
    }
    HotSetLayouts hot;
    CHECK(hot.init(g.ctx, 4));
    VkPipelineLayout layout = hot.create_pipeline_layout();
    const VkFormat color = VK_FORMAT_R8G8B8A8_UNORM;
    const std::vector<uint32_t> bl_ps = spirv_from_bytes({g_bl_ps, sizeof(g_bl_ps)});
    GraphicsPipelineDesc d;
    d.vs_spirv = g.fs_vs;
    d.fs_spirv = bl_ps;
    d.vs_entry = "fs_vs";
    d.fs_entry = "bl_ps";
    d.raster.cull = VK_CULL_MODE_NONE;
    d.color_formats = {&color, 1};
    d.layout = layout;
    VkPipeline pipe = layout ? g.cache.create_graphics(d) : VK_NULL_HANDLE;
    CHECK(pipe != VK_NULL_HANDLE);
    DescriptorAllocator bindless;
    CHECK(bindless.init(g.ctx, hot.bindless_pool_ratios()));
    VkDescriptorSet set1 = bindless.allocate(hot.textures());
    CHECK(set1 != VK_NULL_HANDLE);
    const VkSampler smp[4] = {g.samplers.get(SamplerKey::point_clamp()), g.samplers.get(SamplerKey::point_wrap()),
                              g.samplers.get(SamplerKey::linear_clamp()), g.samplers.get(SamplerKey::linear_wrap())};
    if (set1) {
        write_bindless_samplers(g.ctx, set1, smp);
        write_bindless_texture(g.ctx, set1, 0, white.view);  // reserved slot 0
    }
    BindlessTextureTable table;
    CHECK(table.init(g.ctx, set1, hot.texture_capacity(), white.view, 1));
    FrameRing ring;
    FrameRingDesc fd;
    fd.uniform_bytes = 0;
    CHECK(ring.init(g.ctx, g.alloc, fd));

    auto frame_draw = [&](uint32_t index) {
        Frame* f = ring.begin_frame();
        if (!f) return false;
        table.collect(ring.completed_value());
        const DeviceFns& fn = g.ctx.vkd();
        VkCommandBuffer cmd = f->cmd();
        record_begin(g, cmd, 0, 0, 0);
        fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &set1, 0, nullptr);
        const uint32_t pc[4] = {index, 7, 0, 0};
        fn.vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, pc);
        fn.vkCmdDraw(cmd, 3, 1, 0, 0);
        record_end_readback(g, cmd);
        return ring.end_frame(*f);
    };
    if (pipe && set1 && ring.frames_in_flight()) {
        const uint32_t s = table.allocate(tex.view);
        CHECK(s == 1);
        CHECK(frame_draw(s) && ring.wait_idle());
        CHECK(close_to(g.pixel(16, 16), Rgba{255, 0, 0, 255}) && close_to(g.pixel(48, 16), Rgba{0, 255, 0, 255}));
        // Released while frame current_value() may still read it: not reusable yet.
        CHECK(table.release(s, ring.current_value()));
        CHECK(table.collect(ring.current_value() - 1) == 0 && table.slots().pending() == 1);
        CHECK(table.allocate(tex.view) == 2 && table.allocate(tex.view) == 3);
        CHECK(table.allocate(tex.view) == kInvalidBindlessSlot);  // 4 slots, 0 reserved, 1 pending
        // The next frame's begin_frame/collect retires it (frame 1 completed) and writes white.
        CHECK(frame_draw(s) && ring.wait_idle());
        CHECK(table.slots().pending() == 0 && table.slots().available() == 1);
        CHECK(close_to(g.pixel(16, 16), Rgba{255, 255, 255, 255}) && close_to(g.pixel(48, 16), Rgba{255, 255, 255, 255}));
        CHECK(table.allocate(tex.view) == s);  // reused
        CHECK(frame_draw(s) && ring.wait_idle());
        CHECK(close_to(g.pixel(16, 16), Rgba{255, 0, 0, 255}));

        // A streaming thread releases/allocates its slots (2, 3) with retire_value() while this
        // thread begins and records frames: the tag is never older than a frame being recorded.
        CHECK(ring.retire_value() == ring.current_value() + 1);
        std::atomic<uint64_t> recording{0};
        std::atomic<bool> stop{false}, stream_ok{true};
        std::atomic<uint32_t> cycles{0};
        std::thread streamer([&] {
            uint32_t owned[2] = {2, 3};
            uint64_t last = 0;
            while (!stop.load()) {
                for (uint32_t& o : owned) {
                    const uint64_t rec = recording.load();  // a frame begun (maybe still recording)
                    const uint64_t r = ring.retire_value();
                    if (r <= rec || r < last) stream_ok = false;
                    last = r;
                    if (!table.release(o, r)) stream_ok = false;
                    uint32_t n = kInvalidBindlessSlot;
                    while (n == kInvalidBindlessSlot && !stop.load()) {
                        n = table.allocate(tex.view);
                        if (n == kInvalidBindlessSlot) std::this_thread::yield();
                    }
                    if (n == kInvalidBindlessSlot) return;  // stopping; o stays pending
                    if (n == s) stream_ok = false;           // the main thread's live slot
                    o = n;
                    cycles.fetch_add(1);
                }
            }
        });
        bool frames_ok = true;
        for (int i = 0; i < 40 && frames_ok; ++i) {
            Frame* f = ring.begin_frame();
            if (!f) {
                frames_ok = false;
                break;
            }
            recording.store(f->number());
            table.collect(ring.completed_value());
            const DeviceFns& fn = g.ctx.vkd();
            VkCommandBuffer cmd = f->cmd();
            record_begin(g, cmd, 0, 0, 0);
            fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &set1, 0, nullptr);
            const uint32_t pc[4] = {s, 7, 0, 0};
            fn.vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, pc);
            fn.vkCmdDraw(cmd, 3, 1, 0, 0);
            record_end_readback(g, cmd);
            frames_ok = ring.end_frame(*f);
        }
        stop = true;
        streamer.join();
        CHECK(frames_ok && stream_ok.load() && ring.wait_idle());
        std::printf("  bindless streaming: %u release/allocate cycles over 40 frames\n", cycles.load());
        // Everything the streamer released retires once the GPU is idle (tags <= last begun + 1).
        table.collect(ring.retire_value());
        CHECK(table.slots().pending() == 0 && table.slots().live() + table.slots().available() == 3);
        CHECK(close_to(g.pixel(16, 16), Rgba{255, 0, 0, 255}));
    }
    ring.shutdown();
    table.shutdown();
    g.ctx.wait_idle();
    bindless.shutdown();
    if (pipe) g.ctx.vkd().vkDestroyPipeline(g.ctx.device(), pipe, nullptr);
    if (layout) g.ctx.vkd().vkDestroyPipelineLayout(g.ctx.device(), layout, nullptr);
    hot.shutdown();
    up.forget(tex.handle);
    up.forget(white.handle);
    g.alloc.destroy(tex);
    g.alloc.destroy(white);
    up.shutdown();
}

// Q3: command buffers preallocated per (frame slot, thread): frames within the preallocated counts never allocate
// (command_buffer_growth() stays 0); a frame needing one more secondary grows that slot's list once.
void test_frame_preallocation(Gpu& g) {
    FrameRing ring;
    FrameRingDesc fd;
    fd.frames_in_flight = 2;
    fd.recording_threads = 2;
    fd.primaries_per_thread = 2;
    fd.secondaries_per_thread = 3;
    fd.uniform_bytes = 0;
    CHECK(ring.init(g.ctx, g.alloc, fd));
    const VkFormat color = VK_FORMAT_R8G8B8A8_UNORM;
    auto inh = vk_struct<VkCommandBufferInheritanceRenderingInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO);
    inh.colorAttachmentCount = 1;
    inh.pColorAttachmentFormats = &color;
    inh.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    const DeviceFns& fn = g.ctx.vkd();
    auto frame = [&](uint32_t secondaries_thread1) {
        Frame* f = ring.begin_frame();
        if (!f) return false;
        bool ok = true;
        // Thread 0: its second primary (the first is cmd()); thread 1: one primary + n secondaries.
        for (uint32_t t = 0; t < 2; ++t) {
            VkCommandBuffer p = f->primary(t);
            ok = ok && p != VK_NULL_HANDLE;
            if (p) fn.vkEndCommandBuffer(p);
        }
        for (uint32_t k = 0; k < 3; ++k) {
            VkCommandBuffer s = f->secondary(0, inh);
            ok = ok && s != VK_NULL_HANDLE;
            if (s) fn.vkEndCommandBuffer(s);
        }
        for (uint32_t k = 0; k < secondaries_thread1; ++k) {
            VkCommandBuffer s = f->secondary(1, inh);
            ok = ok && s != VK_NULL_HANDLE;
            if (s) fn.vkEndCommandBuffer(s);
        }
        return ring.end_frame(*f) && ok;
    };
    bool ok = true;
    for (int i = 0; i < 8; ++i) ok = ok && frame(3);
    CHECK(ok && ring.wait_idle());
    CHECK(ring.command_buffer_growth() == 0);
    for (int i = 0; i < 4; ++i) ok = ok && frame(4);  // one secondary over on thread 1: each slot grows once
    CHECK(ok && ring.wait_idle());
    CHECK(ring.command_buffer_growth() == 2);
    std::printf("  frame ring: 8 frames within the preallocated buffers, growth %llu after 4 frames with one more\n",
                static_cast<unsigned long long>(ring.command_buffer_growth()));
    ring.shutdown();
}

// L8 stored as R8 and sampled through an RRR1 view.
void test_swizzle(Gpu& g) {
    Uploader up;
    CHECK(up.init(g.ctx, g.alloc));
    ImageDesc id;
    id.format = Format::R8;
    id.width = id.height = 2;
    id.swizzle = Swizzle::RRR1;
    Image tex;
    CHECK(g.alloc.create_image(id, tex));
    const uint8_t texels[4] = {10, 80, 160, 240};
    const UploadTicket t = up.upload_image_chain(tex, texels);
    CHECK(t && up.finish(t));
    CHECK(draw_textured(g, tex, 0.0f));
    CHECK(close_to(g.pixel(16, 16), Rgba{10, 10, 10, 255}));
    CHECK(close_to(g.pixel(48, 16), Rgba{80, 80, 80, 255}));
    CHECK(close_to(g.pixel(16, 48), Rgba{160, 160, 160, 255}));
    CHECK(close_to(g.pixel(48, 48), Rgba{240, 240, 240, 255}));
    up.forget(tex.handle);
    g.alloc.destroy(tex);
    up.shutdown();
}

void test_compute(Gpu& g) {
    const DeviceFns& fn = g.ctx.vkd();
    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    auto lci = vk_struct<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    lci.bindingCount = 1;
    lci.pBindings = &b;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    CHECK(fn.vkCreateDescriptorSetLayout(g.ctx.device(), &lci, nullptr, &dsl) == VK_SUCCESS);
    VkPipelineLayout layout = create_pipeline_layout(g.ctx, {&dsl, 1}, 0, 0);
    const std::vector<uint32_t> cs = spirv_from_bytes({g_cs_main, sizeof(g_cs_main)});
    ComputePipelineDesc cd;
    cd.cs_spirv = cs;
    cd.entry = "cs_main";
    cd.layout = layout;
    cd.name = "test compute";
    VkPipeline pipe = g.cache.create_compute(cd);
    CHECK(pipe != VK_NULL_HANDLE);
    constexpr uint32_t kCount = 256;
    Buffer out, rb;
    CHECK(g.alloc.create_buffer(kCount * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                MemoryUsage::GpuOnly, out));
    CHECK(g.alloc.create_buffer(kCount * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::Readback, rb));
    g.descriptors.reset();
    VkDescriptorSet set = g.descriptors.allocate(dsl);
    CHECK(set != VK_NULL_HANDLE);
    VkDescriptorBufferInfo bi{out.handle, 0, VK_WHOLE_SIZE};
    auto w = vk_struct<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
    w.dstSet = set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &bi;
    fn.vkUpdateDescriptorSets(g.ctx.device(), 1, &w, 0, nullptr);
    if (pipe) {
        CHECK(g.ctx.immediate_submit([&](VkCommandBuffer cmd) {
            fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
            fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
            fn.vkCmdDispatch(cmd, kCount / 64, 1, 1);
            const VkMemoryBarrier2 m1 = memory_barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                                       VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            cmd_barrier(fn, cmd, {}, {}, std::span<const VkMemoryBarrier2>(&m1, 1));
            const VkBufferCopy c{0, 0, kCount * 4};
            fn.vkCmdCopyBuffer(cmd, out.handle, rb.handle, 1, &c);
            const VkMemoryBarrier2 m2 = memory_barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                                       VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
            cmd_barrier(fn, cmd, {}, {}, std::span<const VkMemoryBarrier2>(&m2, 1));
        }));
        g.alloc.invalidate(rb.alloc);
        const auto* v = static_cast<const uint32_t*>(rb.mapped());
        uint32_t bad = 0;
        for (uint32_t i = 0; i < kCount; ++i) bad += v[i] == i * 3 + 1 ? 0u : 1u;
        CHECK(bad == 0);
    }
    g.alloc.destroy(out);
    g.alloc.destroy(rb);
    if (pipe) fn.vkDestroyPipeline(g.ctx.device(), pipe, nullptr);
    if (layout) fn.vkDestroyPipelineLayout(g.ctx.device(), layout, nullptr);
    fn.vkDestroyDescriptorSetLayout(g.ctx.device(), dsl, nullptr);
}

// HLSL discard -> OpDemoteToHelperInvocation (needs shaderDemoteToHelperInvocation).
void test_discard(Gpu& g) {
    const std::vector<uint32_t> ps = spirv_from_bytes({g_discard_ps, sizeof(g_discard_ps)});
    const VkFormat color = VK_FORMAT_R8G8B8A8_UNORM;
    GraphicsPipelineDesc d;
    d.vs_spirv = g.fs_vs;
    d.fs_spirv = ps;
    d.vs_entry = "fs_vs";
    d.fs_entry = "discard_ps";
    d.raster.cull = VK_CULL_MODE_NONE;
    d.color_formats = {&color, 1};
    d.layout = g.layout;
    VkPipeline pipe = g.cache.create_graphics(d);
    CHECK(pipe != VK_NULL_HANDLE);
    if (!pipe) return;
    CHECK(g.ctx.immediate_submit([&](VkCommandBuffer cmd) {
        const DeviceFns& fn = g.ctx.vkd();
        record_begin(g, cmd, 0, 0, 1);
        fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        PushData pd{{0, 1, 0, 1}, 0.0f};
        fn.vkCmdPushConstants(cmd, g.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pd), &pd);
        fn.vkCmdDraw(cmd, 3, 1, 0, 0);
        record_end_readback(g, cmd);
    }));
    CHECK(close_to(g.pixel(10, 30), Rgba{0, 0, 255, 255}));  // discarded: clear colour
    CHECK(close_to(g.pixel(50, 30), Rgba{0, 255, 0, 255}));
    g.ctx.vkd().vkDestroyPipeline(g.ctx.device(), pipe, nullptr);
}

// Cube image (6 layers, CUBE view) sampled by direction; faces +X,-X,+Y,-Y,+Z,-Z.
void test_cube(Gpu& g) {
    Uploader up;
    CHECK(up.init(g.ctx, g.alloc));
    ImageDesc id;
    id.format = Format::RGBA8;
    id.width = id.height = 2;
    id.layers = 6;
    id.cube = true;
    Image cube;
    CHECK(g.alloc.create_image(id, cube));
    const Rgba faces[6] = {{255, 0, 0, 255},   {0, 255, 0, 255},   {0, 0, 255, 255},
                           {255, 255, 255, 255}, {255, 255, 0, 255}, {255, 0, 255, 255}};
    std::vector<uint8_t> data;
    for (const Rgba& c : faces)
        for (int t = 0; t < 4; ++t) data.insert(data.end(), {c.r, c.g, c.b, c.a});
    const UploadTicket t = up.upload_image_chain(cube, data);
    CHECK(t && up.finish(t));
    ImageDesc bad = id;
    bad.layers = 5;
    Image tmp;
    std::printf("  (a create_image cube error is expected next)\n");
    std::fflush(stdout);
    CHECK(!g.alloc.create_image(bad, tmp));

    const std::vector<uint32_t> ps = spirv_from_bytes({g_cube_ps, sizeof(g_cube_ps)});
    const VkFormat color = VK_FORMAT_R8G8B8A8_UNORM;
    GraphicsPipelineDesc d;
    d.vs_spirv = g.fs_vs;
    d.fs_spirv = ps;
    d.vs_entry = "fs_vs";
    d.fs_entry = "cube_ps";
    d.raster.cull = VK_CULL_MODE_NONE;
    d.color_formats = {&color, 1};
    d.layout = g.layout;
    VkPipeline pipe = g.cache.create_graphics(d);
    CHECK(pipe != VK_NULL_HANDLE);
    const float dirs[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int f = 0; f < 6 && pipe; ++f) {
        g.descriptors.reset();
        CHECK(g.ctx.immediate_submit([&](VkCommandBuffer cmd) {
            const DeviceFns& fn = g.ctx.vkd();
            VkDescriptorSet set = g.descriptors.allocate(g.layouts.textures());
            const TextureBinding tb{0, cube.view, g.samplers.get(SamplerKey::point_clamp()),
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            write_textures(g.ctx, set, {&tb, 1});
            record_begin(g, cmd, 0, 0, 0);
            fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, kSetTextures, 1, &set, 0, nullptr);
            PushData pd{{dirs[f][0], dirs[f][1], dirs[f][2], 0}, 0.0f};
            fn.vkCmdPushConstants(cmd, g.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pd), &pd);
            fn.vkCmdDraw(cmd, 3, 1, 0, 0);
            record_end_readback(g, cmd);
        }));
        CHECK(close_to(g.pixel(32, 32), faces[f]));
    }
    if (pipe) g.ctx.vkd().vkDestroyPipeline(g.ctx.device(), pipe, nullptr);
    up.forget(cube.handle);
    g.alloc.destroy(cube);
    up.shutdown();
}

// Track-B layout for translated shaders: int/bool UBOs at set 0 bindings 2/3, 64-byte push range.
void test_translated_layout(Gpu& g) {
    StandardLayouts sl;
    CHECK(sl.init(g.ctx, kLayoutIntBoolConstants));
    CHECK(sl.constants(true) != VK_NULL_HANDLE && sl.constants(false) != VK_NULL_HANDLE);
    VkPipelineLayout pl = sl.create_pipeline_layout(kTranslatedPushBytes, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, true);
    CHECK(pl != VK_NULL_HANDLE);
    Buffer ub;
    CHECK(g.alloc.create_buffer(4096, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, MemoryUsage::Dynamic, ub));
    g.descriptors.reset();
    VkDescriptorSet set = g.descriptors.allocate(sl.constants(true));
    CHECK(set != VK_NULL_HANDLE);
    write_constants(g.ctx, set, ub.handle, 0, kVSConstantsBytes, ub.handle, 0, kPSConstantsBytes);
    write_int_bool_constants(g.ctx, set, ub.handle, 0, ub.handle, 512);
    StandardLayouts plain;
    CHECK(plain.init(g.ctx));
    CHECK(plain.constants(true) == VK_NULL_HANDLE);
    if (pl) g.ctx.vkd().vkDestroyPipelineLayout(g.ctx.device(), pl, nullptr);
    g.ctx.wait_idle();
    g.alloc.destroy(ub);
    plain.shutdown();
    sl.shutdown();
}

// Volume texture: 4x4x4 with one colour per slice, mip 1 2x2x2 with one colour per slice.
void test_volume(Gpu& g) {
    Uploader up;
    CHECK(up.init(g.ctx, g.alloc));
    ImageDesc id;
    id.format = Format::RGBA8;
    id.width = id.height = id.depth = 4;
    id.mips = 2;
    Image vol;
    CHECK(g.alloc.create_image(id, vol));
    CHECK(vol.depth == 4);
    const Rgba s0[4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 255, 255}};
    const Rgba s1[2] = {{255, 255, 0, 255}, {255, 0, 255, 255}};
    std::vector<uint8_t> data;
    for (const Rgba& c : s0)
        for (int t = 0; t < 16; ++t) data.insert(data.end(), {c.r, c.g, c.b, c.a});
    for (const Rgba& c : s1)
        for (int t = 0; t < 4; ++t) data.insert(data.end(), {c.r, c.g, c.b, c.a});
    const UploadTicket t = up.upload_image_chain(vol, data);
    CHECK(t && up.finish(t));
    const std::vector<uint32_t> ps = spirv_from_bytes({g_vol_ps, sizeof(g_vol_ps)});
    const VkFormat color = VK_FORMAT_R8G8B8A8_UNORM;
    GraphicsPipelineDesc d;
    d.vs_spirv = g.fs_vs;
    d.fs_spirv = ps;
    d.vs_entry = "fs_vs";
    d.fs_entry = "vol_ps";
    d.raster.cull = VK_CULL_MODE_NONE;
    d.color_formats = {&color, 1};
    d.layout = g.layout;
    VkPipeline pipe = g.cache.create_graphics(d);
    CHECK(pipe != VK_NULL_HANDLE);
    auto draw = [&](float w, float lod) {
        g.descriptors.reset();
        return g.ctx.immediate_submit([&](VkCommandBuffer cmd) {
            const DeviceFns& fn = g.ctx.vkd();
            VkDescriptorSet set = g.descriptors.allocate(g.layouts.textures());
            const TextureBinding tb{0, vol.view, g.samplers.get(SamplerKey::point_clamp()),
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            write_textures(g.ctx, set, {&tb, 1});
            record_begin(g, cmd, 0, 0, 0);
            fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, kSetTextures, 1, &set, 0, nullptr);
            PushData pd{{w, 0, 0, 0}, lod};
            fn.vkCmdPushConstants(cmd, g.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pd), &pd);
            fn.vkCmdDraw(cmd, 3, 1, 0, 0);
            record_end_readback(g, cmd);
        });
    };
    for (int z = 0; z < 4 && pipe; ++z) {
        CHECK(draw((static_cast<float>(z) + 0.5f) / 4.0f, 0.0f));
        CHECK(close_to(g.pixel(20, 40), s0[z]));
    }
    if (pipe) {
        CHECK(draw(0.2f, 1.0f));
        CHECK(close_to(g.pixel(20, 40), s1[0]));
        CHECK(draw(0.8f, 1.0f));
        CHECK(close_to(g.pixel(20, 40), s1[1]));
        g.ctx.vkd().vkDestroyPipeline(g.ctx.device(), pipe, nullptr);
    }
    up.forget(vol.handle);
    g.alloc.destroy(vol);
    up.shutdown();
}

// D3D A4R4G4B4 texels stored as B4G4R4A4 and fixed up by the view swizzle.
void test_argb4(Gpu& g) {
    Uploader up;
    CHECK(up.init(g.ctx, g.alloc));
    ImageDesc id;
    id.format = Format::B4G4R4A4;
    id.width = id.height = 2;
    id.swizzle = Swizzle::ARGB4_IN_BGRA4;
    Image tex;
    CHECK(g.alloc.create_image(id, tex));
    // A=F R=F G=0 B=8 | A=8 R=0 G=F B=0 | A=F R=1 G=2 B=3 | A=0 R=F G=F B=F  (little-endian u16)
    const uint16_t texels[4] = {0xFF08, 0x80F0, 0xF123, 0x0FFF};
    uint8_t bytes[8];
    std::memcpy(bytes, texels, 8);
    const UploadTicket t = up.upload_image_chain(tex, bytes);
    CHECK(t && up.finish(t));
    CHECK(draw_textured(g, tex, 0.0f));
    CHECK(close_to(g.pixel(16, 16), Rgba{255, 0, 136, 255}));
    CHECK(close_to(g.pixel(48, 16), Rgba{0, 255, 0, 136}));
    CHECK(close_to(g.pixel(16, 48), Rgba{17, 34, 51, 255}));
    CHECK(close_to(g.pixel(48, 48), Rgba{255, 255, 255, 0}));
    up.forget(tex.handle);
    g.alloc.destroy(tex);
    up.shutdown();
}

// Copies g.target (after a stretch into it) to the readback buffer.
void record_target_readback(Gpu& g, VkCommandBuffer cmd) {
    const DeviceFns& fn = g.ctx.vkd();
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {kSize, kSize, 1};
    fn.vkCmdCopyImageToBuffer(cmd, g.target.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.readback.handle, 1, &copy);
    const VkMemoryBarrier2 mb = memory_barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                               VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    cmd_barrier(fn, cmd, {}, {}, std::span<const VkMemoryBarrier2>(&mb, 1));
}

// RenderTarget (colour + depth-stencil, scaled), begin_rendering, StretchRect copy and blit.
void test_render_target(Gpu& g) {
    const Format ds = pick_depth_stencil_format(g.ctx);
    CHECK(ds == Format::D24S8 || ds == Format::D32FS8);
    std::printf("  depth-stencil format: %s\n", format_info(ds).name);
    RenderTarget scene, quarter;
    RenderTargetDesc sd;
    sd.scale = 0.5f;  // colour only: the test pipelines have no depth format
    sd.name = "test scene RT";
    CHECK(scene.init(g.alloc, sd, {128, 128}));
    CHECK(scene.extent().width == 64 && scene.extent().height == 64 && scene.has_color() && !scene.has_depth());
    RenderTargetDesc qd;
    qd.scale = 0.125f;
    CHECK(quarter.init(g.alloc, qd, {128, 128}));
    CHECK(quarter.extent().width == 16);
    const float red[4] = {1, 0, 0, 1};
    const DeviceFns& fn = g.ctx.vkd();
    // Same size -> vkCmdCopyImage2.
    CHECK(g.ctx.immediate_submit([&](VkCommandBuffer cmd) {
        ClearValues cv;
        cv.color[0] = 0;
        cv.color[1] = 0;
        cv.color[2] = 1;
        scene.begin_rendering(fn, cmd, &cv);
        record_triangle(g, cmd, red);
        scene.end_rendering(fn, cmd);
        CopyImage src{scene.color().handle, scene.extent(), scene.color().vk_format(), VK_IMAGE_ASPECT_COLOR_BIT,
                      scene.color_state(), ImageState::ShaderRead};
        CopyImage dst{g.target.handle, {kSize, kSize}, g.target.vk_format(), VK_IMAGE_ASPECT_COLOR_BIT,
                      ImageState::Undefined, ImageState::TransferSrc};
        cmd_stretch_rect(fn, cmd, src, dst);
        scene.set_color_state(ImageState::ShaderRead);
        record_target_readback(g, cmd);
    }));
    CHECK(close_to(g.pixel(32, 32), Rgba{255, 0, 0, 255}));
    CHECK(close_to(g.pixel(2, 2), Rgba{0, 0, 255, 255}));
    // Downscale (blit, linear) into the 16x16 target, then upscale (blit, nearest) back for readback.
    CHECK(g.ctx.immediate_submit([&](VkCommandBuffer cmd) {
        CopyImage src{scene.color().handle, scene.extent(), scene.color().vk_format(), VK_IMAGE_ASPECT_COLOR_BIT,
                      scene.color_state(), ImageState::ShaderRead};
        quarter.copy_from(fn, cmd, src, VK_FILTER_LINEAR, ImageState::TransferSrc);
        CopyImage q{quarter.color().handle, quarter.extent(), quarter.color().vk_format(), VK_IMAGE_ASPECT_COLOR_BIT,
                    quarter.color_state(), ImageState::ShaderRead};
        CopyImage dst{g.target.handle, {kSize, kSize}, g.target.vk_format(), VK_IMAGE_ASPECT_COLOR_BIT,
                      ImageState::TransferSrc, ImageState::TransferSrc};
        cmd_stretch_rect(fn, cmd, q, dst, VK_FILTER_NEAREST);
        quarter.set_color_state(ImageState::ShaderRead);
        record_target_readback(g, cmd);
    }));
    CHECK(close_to(g.pixel(32, 36), Rgba{255, 0, 0, 255}));
    CHECK(close_to(g.pixel(2, 2), Rgba{0, 0, 255, 255}));
    g.ctx.wait_idle();
    CHECK(scene.resize({256, 256}) && scene.extent().width == 128);
    CHECK(!scene.resize({256, 256}));
    CHECK(scene.color_state() == ImageState::Undefined);
    quarter.shutdown();
    scene.shutdown();
}

// Two-sided stencil with dynamic stencil ops: front faces increment, back faces decrement (wrap).
void test_stencil(Gpu& g) {
    const Format ds = pick_depth_stencil_format(g.ctx);
    if (ds == Format::Undefined) return;
    RenderTarget rt;
    RenderTargetDesc d;
    d.color = Format::Undefined;
    d.depth = ds;
    d.width = d.height = kSize;
    CHECK(rt.init(g.alloc, d, {kSize, kSize}));
    GraphicsPipelineDesc pd;
    pd.vs_spirv = g.tri_vs;
    pd.vs_entry = "tri_vs";
    pd.raster.cull = VK_CULL_MODE_NONE;
    pd.depth.stencil_test = true;
    pd.dynamic_stencil_op = true;
    pd.depth_format = to_vk(ds);
    pd.stencil_format = to_vk(ds);
    pd.layout = g.layout;
    pd.raster.front_face = VK_FRONT_FACE_CLOCKWISE;  // the test triangle is clockwise: front facing
    VkPipeline front = g.cache.create_graphics(pd);
    pd.raster.front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;  // same triangle seen as a back face
    VkPipeline back = g.cache.create_graphics(pd);
    CHECK(front && back);
    Buffer rb;
    CHECK(g.alloc.create_buffer(kSize * kSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::Readback, rb));
    const DeviceFns& fn = g.ctx.vkd();
    CHECK(rt.depth().attachment_view && rt.depth().attachment_view != rt.depth().view);  // depth+stencil view
    if (front && back) {
        CHECK(g.ctx.immediate_submit([&](VkCommandBuffer cmd) {
            ClearValues cv;
            cv.stencil = 0;
            rt.begin_rendering(fn, cmd, &cv);
            fn.vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
            fn.vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
            fn.vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
            fn.vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_FRONT_BIT, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_INCREMENT_AND_WRAP,
                                 VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
            fn.vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_BACK_BIT, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_DECREMENT_AND_WRAP,
                                 VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
            fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, front);
            fn.vkCmdDraw(cmd, 3, 1, 0, 0);  // +1
            fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, back);
            fn.vkCmdDraw(cmd, 3, 1, 0, 0);  // -1
            fn.vkCmdDraw(cmd, 3, 1, 0, 0);  // -1 -> wraps to 255
            rt.end_rendering(fn, cmd);
            rt.transition_depth(fn, cmd, ImageState::TransferSrc);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1};
            copy.imageExtent = {kSize, kSize, 1};
            fn.vkCmdCopyImageToBuffer(cmd, rt.depth().handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.handle, 1, &copy);
            const VkMemoryBarrier2 mb = memory_barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                                       VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
            cmd_barrier(fn, cmd, {}, {}, std::span<const VkMemoryBarrier2>(&mb, 1));
        }));
        g.alloc.invalidate(rb.alloc);
        const auto* st = static_cast<const uint8_t*>(rb.mapped());
        CHECK(st[32 * kSize + 32] == 255);  // +1 (front) -1 -1 (back), wrapped
        CHECK(st[2 * kSize + 2] == 0);
        std::printf("  two-sided stencil: inside %u, outside %u\n", st[32 * kSize + 32], st[2 * kSize + 2]);
    }
    g.ctx.wait_idle();
    if (front) fn.vkDestroyPipeline(g.ctx.device(), front, nullptr);
    if (back) fn.vkDestroyPipeline(g.ctx.device(), back, nullptr);
    g.alloc.destroy(rb);
    rt.shutdown();
}

void test_timer(Gpu& g) {
    GpuTimer timer;
    if (!timer.init(g.ctx, 2)) {
        std::printf("  timestamps: graphics queue has no timestampValidBits, skipped\n");
        return;
    }
    const float green[4] = {0, 1, 0, 1};
    for (int run = 0; run < 2; ++run) {
        CHECK(g.ctx.immediate_submit([&](VkCommandBuffer cmd) {
            timer.begin_frame(cmd, 0);
            const uint32_t all = timer.begin_scope(cmd, "frame");
            const uint32_t draw = timer.begin_scope(cmd, "triangle");
            record_begin(g, cmd, 0, 0, 1);
            for (int i = 0; i < 200; ++i) record_triangle(g, cmd, green);
            g.ctx.vkd().vkCmdEndRendering(cmd);
            timer.end_scope(cmd, draw);
            timer.end_scope(cmd, all);
        }));
        CHECK(timer.collect(0));  // the submit has completed: no stall
        const auto& r = timer.results();
        CHECK(r.size() == 2);
        if (r.size() == 2) {
            CHECK(r[0].name == "frame" && r[1].name == "triangle");
            CHECK(r[0].end_ticks > r[0].begin_ticks && r[1].end_ticks > r[1].begin_ticks);
            CHECK(r[1].begin_ticks >= r[0].begin_ticks && r[1].end_ticks <= r[0].end_ticks);
            CHECK(r[0].ms > 0.0 && r[1].ms > 0.0);
            if (run == 1)
                std::printf("  timestamps: frame %.4f ms, 200 triangles %.4f ms (%u valid bits, %.2f ns/tick)\n", r[0].ms,
                            r[1].ms, g.ctx.caps().timestamp_valid_bits, static_cast<double>(g.ctx.caps().timestamp_period_ns));
        }
    }
    timer.shutdown();
}

void test_pipeline_feedback(Gpu& g) {
    PipelineCache fresh;  // empty, in memory only
    CHECK(fresh.init(g.ctx));
    const VkFormat color = VK_FORMAT_R8G8B8A8_UNORM;
    GraphicsPipelineDesc d;
    d.vs_spirv = g.tri_vs;
    d.fs_spirv = g.tri_ps;
    d.vs_entry = "tri_vs";
    d.fs_entry = "tri_ps";
    d.raster.cull = VK_CULL_MODE_NONE;
    d.color_formats = {&color, 1};
    d.layout = g.layout;
    PipelineFeedback f1, f2;
    d.feedback = &f1;
    VkPipeline p1 = fresh.create_graphics(d);
    d.feedback = &f2;
    VkPipeline p2 = fresh.create_graphics(d);
    CHECK(p1 && p2);
    CHECK(f1.stage_count == 2 && f2.stage_count == 2);
    std::printf("  pipeline feedback: first valid=%d hit=%d %.3f ms, second valid=%d hit=%d %.3f ms\n", f1.valid(),
                f1.cache_hit(), f1.duration_ms(), f2.valid(), f2.cache_hit(), f2.duration_ms());
    if (f2.valid()) {
        CHECK(!f1.cache_hit());
        if (!f2.cache_hit()) std::printf("  pipeline feedback: driver does not report APPLICATION_PIPELINE_CACHE_HIT\n");
    } else {
        std::printf("  pipeline feedback: driver leaves feedback invalid\n");
    }
    ComputePipelineDesc cd;
    const std::vector<uint32_t> cs = spirv_from_bytes({g_cs_main, sizeof(g_cs_main)});
    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    auto lci = vk_struct<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    lci.bindingCount = 1;
    lci.pBindings = &b;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    CHECK(g.ctx.vkd().vkCreateDescriptorSetLayout(g.ctx.device(), &lci, nullptr, &dsl) == VK_SUCCESS);
    VkPipelineLayout cl = create_pipeline_layout(g.ctx, {&dsl, 1}, 0, 0);
    PipelineFeedback fc;
    cd.cs_spirv = cs;
    cd.entry = "cs_main";
    cd.layout = cl;
    cd.feedback = &fc;
    VkPipeline pc = fresh.create_compute(cd);
    CHECK(pc && fc.stage_count == 1);
    // Cache-level stats: every creation is counted, also without a per-desc feedback pointer.
    d.feedback = nullptr;
    VkPipeline p3 = fresh.create_graphics(d);
    CHECK(p3 != VK_NULL_HANDLE);
    const PipelineCacheStats st = fresh.stats();
    std::printf("  pipeline cache stats: %llu created, %llu hits, %llu misses, %llu unknown, %.3f ms\n",
                static_cast<unsigned long long>(st.created), static_cast<unsigned long long>(st.hits),
                static_cast<unsigned long long>(st.misses), static_cast<unsigned long long>(st.unknown), st.total_ms);
    CHECK(st.created == 4 && st.failed == 0 && st.hits + st.misses + st.unknown == 4);
    if (f1.valid()) CHECK(st.misses >= 1);
    if (f2.valid() && f2.cache_hit()) CHECK(st.hits >= 1);
    CHECK(st.total_ms >= 0.0);
    fresh.reset_stats();
    CHECK(fresh.stats().created == 0 && fresh.stats().hits == 0);
    CHECK(g.cache.stats().created > 0);  // the shared cache counted make_pipelines etc.
    const DeviceFns& fn = g.ctx.vkd();
    for (VkPipeline p : {p1, p2, pc, p3})
        if (p) fn.vkDestroyPipeline(g.ctx.device(), p, nullptr);
    fn.vkDestroyPipelineLayout(g.ctx.device(), cl, nullptr);
    fn.vkDestroyDescriptorSetLayout(g.ctx.device(), dsl, nullptr);
    fresh.shutdown(false);
}

// forget() before destroy, and the partial re-upload guard on the transfer queue.
void test_upload_lifetime(Gpu& g) {
    Uploader up;
    CHECK(up.init(g.ctx, g.alloc));
    std::vector<uint8_t> data(4096, 0x5A);
    Buffer b;
    CHECK(g.alloc.create_buffer(data.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::GpuOnly, b));
    const UploadTicket t = up.upload_buffer(b, 0, data);
    CHECK(t);
    up.forget(b.handle);  // waits for the copy and drops the pending acquire
    CHECK(up.is_complete(t));
    g.alloc.destroy(b);
    AcquireResult acq{};
    CHECK(g.ctx.immediate_submit([&](VkCommandBuffer cmd) { acq = up.record_acquires(cmd); }));
    CHECK(acq.barriers == 0);

    if (up.dedicated_queue()) {
        // Ownership is tracked at submit: a partial upload right after a whole one (release
        // submitted, record_acquires not run yet) is rejected too.
        Buffer d;
        CHECK(g.alloc.create_buffer(data.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::GpuOnly, d));
        const UploadTicket td = up.upload_buffer(d, 0, data);
        CHECK(td);
        std::printf("  (an upload_buffer partial re-upload error is expected next)\n");
        std::fflush(stdout);
        CHECK(!up.upload_buffer(d, 16, std::span<const uint8_t>(data.data(), 64)));
        CHECK(up.finish(td));
        up.forget(d.handle);
        g.alloc.destroy(d);

        Buffer c;
        CHECK(g.alloc.create_buffer(data.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::GpuOnly, c));
        const UploadTicket t1 = up.upload_buffer(c, 0, data);
        CHECK(t1 && up.finish(t1));  // now owned by graphics
        std::printf("  (an upload_buffer partial re-upload error is expected next)\n");
        std::fflush(stdout);
        CHECK(!up.upload_buffer(c, 16, std::span<const uint8_t>(data.data(), 64)));
        const UploadTicket t2 = up.upload_buffer(c, 0, data);  // whole overwrite is fine
        CHECK(t2 && up.finish(t2));
        up.forget(c.handle);
        g.alloc.destroy(c);
    }
    up.shutdown();
}

// Device-loss-like failures through the injection hook: failed submits (final and partial) fail
// the whole upload, and a stuck timeline makes a ring-space wait return instead of spinning.
void test_upload_faults(Gpu& g) {
    Uploader up;
    UploaderDesc ud;
    ud.ring_bytes = 1ull << 20;  // chunks of 256 KiB: a 2 MiB upload needs partial submits
    CHECK(up.init(g.ctx, g.alloc, ud));
    constexpr size_t kBig = 2u << 20, kSmall = 600u * 1024;
    std::vector<uint8_t> data(kBig);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i * 7 + 3);
    Buffer a, b, c;
    for (Buffer* x : {&a, &c})
        CHECK(g.alloc.create_buffer(kBig, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                    MemoryUsage::GpuOnly, *x));
    CHECK(g.alloc.create_buffer(kSmall, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::GpuOnly, b));
    const std::span<const uint8_t> big(data), small(data.data(), kSmall);
    std::printf("  (upload failure messages are expected next)\n");
    std::fflush(stdout);
    // Overflow-safe bounds: dst_offset + size would wrap.
    CHECK(!up.upload_buffer(b, ~VkDeviceSize{0} - 8, small.first(64)));
    // Final submit fails -> ticket 0.
    up.inject_faults_for_testing(1);
    CHECK(!up.upload_buffer(b, 0, small));
    // First partial submit of a multi-chunk upload fails -> the whole upload fails (ticket 0).
    up.inject_faults_for_testing(1);
    CHECK(!up.upload_buffer(a, 0, big));
    // Recovered: the abandoned ring space retires and uploads work again.
    up.inject_faults_for_testing(0);
    const UploadTicket t0 = up.upload_buffer(a, 0, big);
    CHECK(t0 && up.finish(t0));
    const UploadTicket t1 = up.upload_buffer(b, 0, small);  // holds 600 KiB of the ring
    CHECK(t1);
    // "Device lost": the timeline stops advancing and waits fail. The next upload fills the rest of
    // the ring, submits it, then has to wait for t1's space: it must give up, not spin.
    up.inject_faults_for_testing(0, true);
    CHECK(!up.upload_buffer(c, 0, big));
    up.inject_faults_for_testing(0);
    CHECK(up.finish(t1));
    const UploadTicket t2 = up.upload_buffer(c, 0, big);
    CHECK(t2 && up.finish(t2));
    // Content check of the recovered uploads.
    Buffer rb;
    CHECK(g.alloc.create_buffer(kBig * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::Readback, rb));
    CHECK(g.ctx.immediate_submit([&](VkCommandBuffer cmd) {
        const VkBufferCopy c0{0, 0, kBig};
        const VkBufferCopy c1{0, kBig, kBig};
        g.ctx.vkd().vkCmdCopyBuffer(cmd, a.handle, rb.handle, 1, &c0);
        g.ctx.vkd().vkCmdCopyBuffer(cmd, c.handle, rb.handle, 1, &c1);
        const VkMemoryBarrier2 mb = memory_barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                                   VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
        cmd_barrier(g.ctx.vkd(), cmd, {}, {}, std::span<const VkMemoryBarrier2>(&mb, 1));
    }));
    g.alloc.invalidate(rb.alloc);
    const auto* p = static_cast<const uint8_t*>(rb.mapped());
    CHECK(std::memcmp(p, data.data(), kBig) == 0 && std::memcmp(p + kBig, data.data(), kBig) == 0);
    g.alloc.destroy(rb);
    for (Buffer* x : {&a, &b, &c}) {
        up.forget(x->handle);
        g.alloc.destroy(*x);
    }
    up.shutdown();
}

// Pools never grow for a layout that cannot fit an empty pool, and max_pools caps growth.
void test_descriptor_limits(Gpu& g) {
    const DeviceFns& fn = g.ctx.vkd();
    auto make_layout = [&](VkDescriptorType type, uint32_t count) {
        VkDescriptorSetLayoutBinding b{0, type, count, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        auto ci = vk_struct<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
        ci.bindingCount = 1;
        ci.pBindings = &b;
        VkDescriptorSetLayout l = VK_NULL_HANDLE;
        CHECK(fn.vkCreateDescriptorSetLayout(g.ctx.device(), &ci, nullptr, &l) == VK_SUCCESS);
        return l;
    };
    const VkDescriptorSetLayout sampled = make_layout(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1);  // ratio 0
    const VkDescriptorSetLayout wide = make_layout(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024);
    const VkDescriptorSetLayout ubo = make_layout(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1);
    std::printf("  (DescriptorAllocator never-fit / exhaustion errors may follow)\n");
    std::fflush(stdout);
    for (VkDescriptorSetLayout layout : {sampled, wide}) {
        DescriptorPoolRatios r;
        r.sets = 4;
        r.uniform_buffers = 1;
        r.combined_image_samplers = 1;  // far fewer than `wide` needs; no sampled images at all
        r.storage_buffers = 0;
        DescriptorAllocator da;
        CHECK(da.init(g.ctx, r));
        uint32_t got = 0;
        for (int i = 0; i < 100; ++i) got += da.allocate(layout) ? 1u : 0u;
        CHECK(da.failed_allocations() + got == 100);
        if (layout == wide) {
            // Cannot fit an empty pool on any driver: fails every time without adding pools.
            CHECK(got == 0);
            CHECK(da.pool_count() == 1);
        } else {
            // Byte-budget pools (e.g. AMD) may serve a type the ratios omit; growth stays capped.
            CHECK(got != 0 || da.pool_count() == 1);
            CHECK(da.pool_count() <= r.max_pools);
        }
        std::printf("  descriptor never-fit (%s): %u of 100 allocated by the driver, %zu pool(s)\n",
                    layout == sampled ? "zero ratio" : "too wide", got, da.pool_count());
        CHECK(da.allocate(ubo) != VK_NULL_HANDLE);  // a fitting layout still works
        da.shutdown();
    }
    {
        DescriptorPoolRatios r;
        r.sets = 2;
        r.max_pools = 3;
        DescriptorAllocator da;
        CHECK(da.init(g.ctx, r));
        uint32_t got = 0;
        for (int i = 0; i < 20; ++i) got += da.allocate(g.layouts.constants()) ? 1u : 0u;
        CHECK(got >= 6);
        CHECK(da.pool_count() <= 3);
        da.reset();
        CHECK(da.allocate(g.layouts.constants()) != VK_NULL_HANDLE);
        CHECK(da.pool_count() <= 3);
        std::printf("  descriptor exhaustion: %u of 20 allocated, %zu pools (cap 3)\n", got, da.pool_count());
        da.shutdown();
    }
    fn.vkDestroyDescriptorSetLayout(g.ctx.device(), sampled, nullptr);
    fn.vkDestroyDescriptorSetLayout(g.ctx.device(), wide, nullptr);
    fn.vkDestroyDescriptorSetLayout(g.ctx.device(), ubo, nullptr);
}

// PipelineCache::init on truncated / corrupt / headerless files starts empty instead of feeding
// the bytes to the driver; save() leaves no temp files behind. `deep`: the cache lives in a folder
// nested beyond 300 characters (Windows MAX_PATH), created and cleaned up through long_path().
// `prefixed`: PipelineCache gets the "\\?\" form (long_path) instead of the plain long path.
void test_pipeline_cache_files(Gpu& g, bool deep, bool prefixed = false) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = fs::temp_directory_path() / ("alamo_rhi_cache_" + std::to_string(g.ctx.caps().device_id) +
                                                       (deep ? "_deep" : "") + (prefixed ? "_prefixed" : ""));
    fs::path dir = root;
    if (deep)
        while (dir.native().size() <= 320) dir /= "nested_pipeline_cache_level_0123456789";
    remove_all_long(root, ec);
    CHECK(fs::create_directories(long_path(dir), ec) && !ec);
    const fs::path file = prefixed ? long_path(dir / "pipelines.cache") : dir / "pipelines.cache";
    auto read_all = [&]() {
        std::ifstream in(long_path(file), std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    auto write_all = [&](std::span<const uint8_t> bytes) {
        std::ofstream out(long_path(file), std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    };
    auto loads = [&]() {
        PipelineCache pc;
        const bool ok = pc.init(g.ctx, file);
        CHECK(ok);
        const bool loaded = ok && pc.loaded_from_disk();
        pc.shutdown(false);
        return loaded;
    };
    {
        PipelineCache pc;
        CHECK(pc.init(g.ctx, file));
        CHECK(pc.save());
        CHECK(pc.save());  // twice: unique temp names, rename over the existing file
        pc.shutdown(false);
    }
    {
        // Concurrent saves from two caches on the same file: distinct temp files, the last rename wins.
        PipelineCache a, b;
        CHECK(a.init(g.ctx, file) && b.init(g.ctx, file));
        bool ok_a = false, ok_b = false;
        std::thread ta([&] { ok_a = a.save(); });
        std::thread tb([&] { ok_b = b.save(); });
        ta.join();
        tb.join();
        CHECK(ok_a && ok_b);  // MoveFileEx over a closed target does not fail; neither save may
        a.shutdown(false);
        b.shutdown(false);
    }
    const std::vector<uint8_t> good = read_all();
    std::span<const uint8_t> blob;
    CHECK(pipeline_cache_unwrap(good, blob));
    CHECK(loads());
    write_all(std::span<const uint8_t>(good.data(), good.size() / 2));  // truncated
    CHECK(!loads());
    std::vector<uint8_t> bad = good;
    bad.back() ^= 0xFF;  // body corrupted
    write_all(bad);
    CHECK(!loads());
    write_all(blob);  // raw driver blob without the alamo header (old format)
    CHECK(!loads());
    const std::vector<uint8_t> junk(777, 0xA5);
    write_all(junk);
    CHECK(!loads());
    write_all({});
    CHECK(!loads());
    size_t leftovers = 0;
    const std::vector<fs::path> entries = list_directory(dir, ec);
    for (const fs::path& e : entries)
        if (e.extension() == ".tmp") ++leftovers;
    CHECK(!ec && entries.size() == 1 && leftovers == 0);
    remove_all_long(root, ec);
    CHECK(!ec && !fs::exists(long_path(root), ec));
}

// Optional-feature fallbacks: non-solid fill, LOD bias range, MIRROR_CLAMP_TO_EDGE, cube arrays.
void test_caps_fallbacks(Gpu& g) {
    const DeviceCaps& caps = g.ctx.caps();
    std::printf("  caps: fillModeNonSolid %d, samplerMirrorClampToEdge %d, imageCubeArray %d, maxSamplerLodBias %.1f\n",
                caps.fill_mode_non_solid, caps.sampler_mirror_clamp_to_edge, caps.image_cube_array,
                static_cast<double>(caps.max_sampler_lod_bias));
    const VkFormat color = VK_FORMAT_R8G8B8A8_UNORM;
    GraphicsPipelineDesc d;
    d.vs_spirv = g.tri_vs;
    d.fs_spirv = g.tri_ps;
    d.vs_entry = "tri_vs";
    d.fs_entry = "tri_ps";
    d.raster.cull = VK_CULL_MODE_NONE;
    d.raster.polygon = VK_POLYGON_MODE_LINE;  // FILL when fillModeNonSolid is missing
    d.color_formats = {&color, 1};
    d.layout = g.layout;
    VkPipeline p = g.cache.create_graphics(d);
    CHECK(p != VK_NULL_HANDLE);
    if (p) g.ctx.vkd().vkDestroyPipeline(g.ctx.device(), p, nullptr);

    SamplerCache sc;
    sc.init(g.ctx);
    SamplerKey bias;
    bias.lod_bias = 100.0f;  // beyond any maxSamplerLodBias: clamped
    CHECK(sc.get(bias) != VK_NULL_HANDLE);
    bias.lod_bias = -100.0f;
    CHECK(sc.get(bias) != VK_NULL_HANDLE);
    SamplerKey mirror;
    mirror.address_u = mirror.address_v = VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    CHECK(sc.get(mirror) != VK_NULL_HANDLE);
    sc.shutdown();

    ImageDesc id;
    id.format = Format::RGBA8;
    id.width = id.height = 2;
    id.layers = 12;
    id.cube = true;
    Image img;
    if (!caps.image_cube_array) {
        std::printf("  (a create_image cube array error is expected next)\n");
        std::fflush(stdout);
    }
    const bool made = g.alloc.create_image(id, img);
    CHECK(made == caps.image_cube_array);
    if (made) g.alloc.destroy(img);
}

bool layer_path_has_validation() {
    const char* p = std::getenv("VK_LAYER_PATH");
    if (!p || !*p) return false;
    std::error_code ec;
    std::string s(p);
    // VK_LAYER_PATH is a ';'-separated path list.
    const char sep = ';';
    size_t start = 0;
    while (start <= s.size()) {
        const size_t end = std::min(s.find(sep, start), s.size());
        const std::string dir = s.substr(start, end - start);
        if (!dir.empty() && std::filesystem::exists(std::filesystem::path(dir) / "VkLayer_khronos_validation.json", ec))
            return true;
        start = end + 1;
    }
    return false;
}

int run_gpu() {
    const bool strict = layer_path_has_validation();
    {
        Loader probe;
        if (!probe.open()) {
            std::printf("SKIP alamo_rhi gpu: no Vulkan loader\n");
            return strict ? 1 : 0;
        }
    }
    Gpu g;
    ContextDesc cd;
    cd.app_name = "alamo_rhi_tests";
    cd.validation = strict ? Validation::Required : Validation::IfAvailable;
    if (!g.ctx.init(cd)) {
        if (strict) {
            // Distinguish "layer missing" (a failure) from "no device" (a skip).
            Loader l;
            l.open();
            uint32_t n = 0;
            l.global().vkEnumerateInstanceLayerProperties(&n, nullptr);
            std::vector<VkLayerProperties> layers(n);
            l.global().vkEnumerateInstanceLayerProperties(&n, layers.data());
            bool found = false;
            for (uint32_t i = 0; i < n; ++i) found |= std::strcmp(layers[i].layerName, "VK_LAYER_KHRONOS_validation") == 0;
            if (!found) {
                std::printf("FAIL: VK_LAYER_PATH has the validation layer but the loader cannot enable it\n");
                return 1;
            }
        }
        std::printf("SKIP alamo_rhi gpu: no suitable Vulkan 1.3 device\n");
        return 0;
    }
    std::printf("device: %s, api %u.%u.%u, validation %s%s, async compute %s, transfer queue %s, BC %s\n",
                g.ctx.caps().name.c_str(), VK_API_VERSION_MAJOR(g.ctx.caps().api_version),
                VK_API_VERSION_MINOR(g.ctx.caps().api_version), VK_API_VERSION_PATCH(g.ctx.caps().api_version),
                g.ctx.validation_enabled() ? "on" : "off", strict ? " (required)" : "",
                g.ctx.has_async_compute() ? "yes" : "no", g.ctx.has_transfer_queue() ? "yes" : "no",
                g.ctx.caps().texture_bc ? "yes" : "no");
    CHECK(!strict || g.ctx.validation_enabled());
    if (g.ctx.validation_enabled()) {
        // The messenger must count validation messages: inject one through VK_EXT_debug_utils.
        auto submit = reinterpret_cast<PFN_vkSubmitDebugUtilsMessageEXT>(
            g.ctx.loader().global().vkGetInstanceProcAddr(g.ctx.instance(), "vkSubmitDebugUtilsMessageEXT"));
        CHECK(submit != nullptr);
        if (submit) {
            const log::Level old = log::min_level();
            log::set_min_level(log::Level::Error);
            auto cb = vk_struct<VkDebugUtilsMessengerCallbackDataEXT>(VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT);
            cb.pMessage = "alamo_rhi_tests: injected validation warning (expected)";
            submit(g.ctx.instance(), VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
                   VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &cb);
            log::set_min_level(old);
            CHECK(g.ctx.validation_warnings() == 1);
            g.ctx.reset_validation_counts();
        }
    }

    CHECK(g.alloc.init(g.ctx));
    CHECK(g.layouts.init(g.ctx));
    g.samplers.init(g.ctx);
    CHECK(g.descriptors.init(g.ctx));
    const std::filesystem::path cache_file =
        std::filesystem::temp_directory_path() / ("alamo_rhi_test_" + std::to_string(g.ctx.caps().device_id) + ".cache");
    std::error_code ec;
    std::filesystem::remove(cache_file, ec);
    CHECK(g.cache.init(g.ctx, cache_file));
    CHECK(!g.cache.loaded_from_disk());
    CHECK(make_pipelines(g));

    ImageDesc td;
    td.format = Format::RGBA8;
    td.width = td.height = kSize;
    td.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    td.name = "test target";
    CHECK(g.alloc.create_image(td, g.target));
    CHECK(g.alloc.create_buffer(kSize * kSize * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::Readback, g.readback,
                                "test readback"));

    if (g.tri && g.fs && g.target && g.readback) {
        test_memory(g);
        test_triangle(g);
        std::printf("  frame ring + uploads (transfer queue requested)\n");
        test_upload_frame_path(g, true);
        std::printf("  frame ring + uploads (graphics queue)\n");
        test_upload_frame_path(g, false);
        {
            Uploader up;
            CHECK(up.init(g.ctx, g.alloc));
            test_upload_bc1(g, up);
            up.shutdown();
        }
        test_upload_threads(g, true);
        test_upload_threads(g, false);
        test_hot_set(g);
        test_bindless_recycle(g);
        test_frame_preallocation(g);
        test_swizzle(g);
        test_compute(g);
        test_upload_lifetime(g);
        test_discard(g);
        test_cube(g);
        test_translated_layout(g);
        test_volume(g);
        test_argb4(g);
        test_render_target(g);
        test_stencil(g);
        test_timer(g);
        test_pipeline_feedback(g);

        VkSampler s1 = g.samplers.get(SamplerKey::linear_wrap());
        VkSampler s2 = g.samplers.get(SamplerKey{});
        SamplerKey aniso;
        aniso.max_anisotropy = 16;
        CHECK(s1 && s1 == s2 && g.samplers.get(aniso) != s1);
        CHECK(g.samplers.size() == 5);  // point_clamp/point_wrap/linear_clamp (draws, hot set) + linear_wrap + aniso
        test_upload_faults(g);
        test_descriptor_limits(g);
        test_pipeline_cache_files(g, false);
        test_pipeline_cache_files(g, true);
        test_pipeline_cache_files(g, true, true);  // "\\?\" path through init/save/load
        test_caps_fallbacks(g);
    }

    // Pipeline cache round trip.
    CHECK(g.cache.save());
    g.cache.shutdown(false);
    CHECK(std::filesystem::exists(cache_file, ec));
    CHECK(g.cache.init(g.ctx, cache_file));
    CHECK(g.cache.loaded_from_disk());
    g.cache.shutdown(false);
    std::filesystem::remove(cache_file, ec);

    g.ctx.wait_idle();
    const DeviceFns& fn = g.ctx.vkd();
    if (g.tri) fn.vkDestroyPipeline(g.ctx.device(), g.tri, nullptr);
    if (g.fs) fn.vkDestroyPipeline(g.ctx.device(), g.fs, nullptr);
    if (g.layout) fn.vkDestroyPipelineLayout(g.ctx.device(), g.layout, nullptr);
    g.alloc.destroy(g.target);
    g.alloc.destroy(g.readback);
    g.descriptors.shutdown();
    g.samplers.shutdown();
    g.layouts.shutdown();
    CHECK(g.alloc.stats().allocation_count == 0);
    g.alloc.shutdown();
    const uint32_t errors = g.ctx.validation_errors(), warnings = g.ctx.validation_warnings();
    const bool validated = g.ctx.validation_enabled();
    g.ctx.shutdown();
    if (validated || strict) std::printf("  validation: %u errors, %u warnings\n", errors, warnings);
    CHECK(errors == 0);
    CHECK(!strict || warnings == 0);

    if (g_fail) {
        std::printf("alamo_rhi_tests gpu: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("alamo_rhi_tests gpu: OK\n");
    return 0;
}

#else

int run_gpu() {
    std::printf("SKIP alamo_rhi gpu: built without DXC test shaders\n");
    return 0;
}

#endif

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "all";
    if (mode == "cpu") return run_cpu();
    if (mode == "gpu") return run_gpu();
    const int a = run_cpu();
    const int b = run_gpu();
    return a | b;
}
