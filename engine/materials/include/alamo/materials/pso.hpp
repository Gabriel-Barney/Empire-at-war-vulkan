// Pipeline-state keys (PORTING.md 3.2 / 3.3 / 3.4), API-neutral.
//
// The enum values of the key fields are chosen to equal the corresponding Vulkan enum values
// (VkBlendFactor, VkBlendOp, VkCompareOp, VkStencilOp, VkCullModeFlagBits, VkPolygonMode,
// VkFilter, VkSamplerMipmapMode, VkSamplerAddressMode) so the rhi side can static_cast; this
// header does not include Vulkan.
//
// Winding / cull convention (fixed for every key): the pipeline uses
//   frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, with a NEGATIVE-HEIGHT viewport (Y flip),
// so Vulkan's winding equals the on-screen winding D3D9 saw (y up in NDC, image upright).
// Evidence for which on-screen winding is the outside of a mesh:
//   - ALO triangles wind counter-clockwise around their normals (assets SPEC 2.6) and the game
//     uses a right-handed view + D3DXMatrixPerspectiveFovRH (docs/re/RENDER.md 3.2), so outward
//     faces appear COUNTER-CLOCKWISE on screen;
//   - the z-fail shadow volumes confirm it: t0_2pass p0 sets CULLMODE = CCW with STENCILZFAIL =
//     INCR (z-fail increments on back faces, so the surviving clockwise faces are back faces),
//     and the two-sided pass increments on STENCIL* (clockwise) and decrements on CCW_STENCIL*.
// Therefore: D3DCULL_CCW culls the outward (Vulkan front) faces -> CullMode::Front,
// D3DCULL_CW -> CullMode::Back, D3DCULL_NONE -> None; D3D9's two-sided STENCIL* ops (clockwise
// faces) are the Vulkan BACK face ops and CCW_STENCIL* the FRONT face ops. Because outward faces
// are CCW, the engine's device baseline must have been D3DRS_CULLMODE = D3DCULL_CW (no vanilla
// effect sets it for ordinary meshes): engine_device_defaults() applies that, giving cull BACK.
// (kFrontFaceCounterClockwise below records this convention.)
//
// Hashing is FNV-1a 64 over the key bytes (no padding, no pointers, no seeds): stable across
// runs, builds and machines, so it can key the on-disk pipeline cache.
#pragma once
#include <cstdint>
#include <cstring>
#include <functional>
#include <type_traits>

#include "alamo/materials/d3d9_state.hpp"

namespace alamo::materials {

// Pipeline front face for every key: VK_FRONT_FACE_COUNTER_CLOCKWISE (see the winding note above).
constexpr bool kFrontFaceCounterClockwise = true;

enum class BlendFactor : uint8_t { Zero = 0, One, SrcColor, OneMinusSrcColor, DstColor, OneMinusDstColor,
                                   SrcAlpha, OneMinusSrcAlpha, DstAlpha, OneMinusDstAlpha, ConstantColor,
                                   OneMinusConstantColor, ConstantAlpha, OneMinusConstantAlpha,
                                   SrcAlphaSaturate };
enum class BlendOp : uint8_t { Add = 0, Subtract, ReverseSubtract, Min, Max };
enum class CompareOp : uint8_t { Never = 0, Less, Equal, LessOrEqual, Greater, NotEqual, GreaterOrEqual, Always };
enum class StencilOp : uint8_t { Keep = 0, Zero, Replace, IncrementAndClamp, DecrementAndClamp, Invert,
                                 IncrementAndWrap, DecrementAndWrap };
enum class CullMode : uint8_t { None = 0, Front = 1, Back = 2 };
enum class PolygonMode : uint8_t { Fill = 0, Line = 1, Point = 2 };
enum class Filter : uint8_t { Nearest = 0, Linear = 1 };
enum class MipmapMode : uint8_t { Nearest = 0, Linear = 1 };
enum class AddressMode : uint8_t { Repeat = 0, MirroredRepeat, ClampToEdge, ClampToBorder, MirrorClampToEdge };

// Vertex declarations named by _ALAMO_VERTEX_TYPE (plus engine-side ones). Values are stable.
enum class VertexLayout : uint8_t {
    Unknown = 0, N, NU2, NU2C, NU2U3U3, NU2U3U3C, RSkinNU2, RSkinNU2C, RSkinNU2U3U3, PrimVert,
    Terrain,
    PrimVertLit,  // dynamic FVF XYZ|NORMAL|DIFFUSE|TEX1 (36 B); PrimVert = XYZ|DIFFUSE|TEX1 (24 B), RENDER.md 1.4
    Count
};
VertexLayout vertex_layout_from_name(const char* alamo_vertex_type);  // "alD3dVertNU2" ...
const char* vertex_layout_name(VertexLayout l);
bool vertex_layout_skinned(VertexLayout l);

// PsoKey::vs_id / ps_id namespaces: hand-written / FF programs (programs.hpp ShaderBlob::id)
// have bit 31 clear; ids of FXO bytecode awaiting track-B translation have it set.
constexpr uint32_t kProgramIdTranslatedBit = 0x80000000u;

#pragma pack(push, 1)
struct StencilFace {
    StencilOp fail_op = StencilOp::Keep, pass_op = StencilOp::Keep, depth_fail_op = StencilOp::Keep;
    CompareOp compare = CompareOp::Always;
};

struct PsoKey {
    uint32_t vs_id = 0, ps_id = 0;   // program ids (programs.hpp ShaderBlob::id), 0 = unresolved
    VertexLayout vertex_layout = VertexLayout::Unknown;
    uint8_t render_pass = 0;         // attachment formats + MSAA class, set by the renderer
    uint8_t blend_enable = 0;
    BlendFactor src_color = BlendFactor::One, dst_color = BlendFactor::Zero;
    BlendOp color_op = BlendOp::Add;
    BlendFactor src_alpha = BlendFactor::One, dst_alpha = BlendFactor::Zero;
    BlendOp alpha_op = BlendOp::Add;
    uint8_t color_write_mask = 0xF;  // bit0 R, bit1 G, bit2 B, bit3 A (same as Vk)
    PolygonMode polygon_mode = PolygonMode::Fill;
    CullMode cull = CullMode::Back;  // front face = counter-clockwise (see header)
    uint8_t depth_test = 1, depth_write = 1;
    CompareOp depth_compare = CompareOp::LessOrEqual;
    uint8_t stencil_enable = 0;
    StencilFace front{}, back{};
    uint8_t depth_bias_enable = 0;
    CompareOp alpha_test = CompareOp::Always;  // emulated in the PS (push constants); kept for batching
    uint8_t flags = 0;               // PsoFlags (translated.hpp): translated program, writes depth
    uint8_t reserved[5] = {0, 0, 0, 0, 0};
};
#pragma pack(pop)
static_assert(sizeof(PsoKey) == 40, "PsoKey layout");
static_assert(std::has_unique_object_representations_v<PsoKey>, "PsoKey must hash as bytes");

inline bool operator==(const PsoKey& a, const PsoKey& b) { return std::memcmp(&a, &b, sizeof(PsoKey)) == 0; }
inline bool operator<(const PsoKey& a, const PsoKey& b) { return std::memcmp(&a, &b, sizeof(PsoKey)) < 0; }
uint64_t hash(const PsoKey& k);

#pragma pack(push, 1)
struct SamplerKey {
    Filter mag = Filter::Linear, min = Filter::Linear;
    MipmapMode mip = MipmapMode::Linear;
    uint8_t mip_enable = 1;          // D3DTEXF_NONE mip filter -> 0 (sample level 0 only: maxLod = 0)
    AddressMode u = AddressMode::Repeat, v = AddressMode::Repeat, w = AddressMode::Repeat;
    uint8_t max_anisotropy = 1;      // 1 = off; >1 when D3DTEXF_ANISOTROPIC
    uint32_t border_color = 0;       // D3DCOLOR ARGB (rhi picks the nearest Vk border color)
    uint32_t mip_lod_bias_bits = 0;  // float bits of D3DSAMP_MIPMAPLODBIAS
    uint8_t max_mip_level = 0;       // D3DSAMP_MAXMIPLEVEL (most detailed level allowed) -> minLod
    uint8_t from_effect = 0;         // 1: at least one state came from the effect, 0: engine defaults
    uint8_t reserved[2] = {0, 0};
};
#pragma pack(pop)
static_assert(sizeof(SamplerKey) == 20, "SamplerKey layout");
static_assert(std::has_unique_object_representations_v<SamplerKey>, "SamplerKey must hash as bytes");
inline bool operator==(const SamplerKey& a, const SamplerKey& b) { return std::memcmp(&a, &b, sizeof(SamplerKey)) == 0; }
uint64_t hash(const SamplerKey& k);

// Dynamic pipeline state (not in the key): stencil reference/masks, blend constants, depth bias.
struct DynamicState {
    uint32_t stencil_ref = 0, stencil_read_mask = 0xFFFFFFFFu, stencil_write_mask = 0xFFFFFFFFu;
    uint32_t blend_factor = 0xFFFFFFFFu;  // D3DCOLOR ARGB
    float depth_bias = 0.0f, slope_scaled_depth_bias = 0.0f;  // D3D9 units (bias in depth range)
};

// Push-constant block shared by every hand-written and FF shader (PORTING.md 3.4), 16 bytes.
struct PushConstants {
    uint32_t draw_index = 0;   // base index into the DrawData SSBO (+ SV_InstanceID)
    uint32_t alpha_func = static_cast<uint32_t>(CompareOp::Always);  // CompareOp; Always = no test
    uint32_t alpha_ref = 0;    // D3DRS_ALPHAREF, 0..255 (compared against round(alpha * 255))
    uint32_t flags = 0;        // PushFlags
};
static_assert(sizeof(PushConstants) == 16, "PushConstants layout");
enum PushFlags : uint32_t {
    kPushFog = 1u << 0,        // apply lerp(FogColor, color, fog) in the PS epilogue
    kPushFading = 1u << 1,     // informational: the fading PSO variant is bound
};

// Conversions D3D9 -> key enums (exposed for tests and the translator).
BlendFactor to_blend_factor(uint32_t d3d_blend);
BlendOp to_blend_op(uint32_t d3d_blend_op);
CompareOp to_compare(uint32_t d3d_cmp);
StencilOp to_stencil_op(uint32_t d3d_stencil_op);
CullMode to_cull(uint32_t d3d_cull);

// Resolution of a DeviceState into a key. `alpha_blend_override` (0/1) replaces a dynamic
// ALPHABLENDENABLE (the fade switch); -1 keeps the slot value.
PsoKey resolve_pso(const DeviceState& st, int alpha_blend_override = -1);
DynamicState resolve_dynamic(const DeviceState& st);
PushConstants resolve_push(const DeviceState& st);

// Sampler n of a resolved state. States still at D3D9 defaults are replaced by `engine_default`
// state by state (D3D9 device defaults are POINT/no-mip; the engine overrides them, PORTING 3.3).
SamplerKey resolve_sampler(const DeviceState& st, int n, const SamplerKey& engine_default);

}  // namespace alamo::materials

template <>
struct std::hash<alamo::materials::PsoKey> {
    size_t operator()(const alamo::materials::PsoKey& k) const noexcept {
        return static_cast<size_t>(alamo::materials::hash(k));
    }
};
template <>
struct std::hash<alamo::materials::SamplerKey> {
    size_t operator()(const alamo::materials::SamplerKey& k) const noexcept {
        return static_cast<size_t>(alamo::materials::hash(k));
    }
};
