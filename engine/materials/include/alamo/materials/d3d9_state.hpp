// D3D9 device-state model used to resolve effect passes into pipeline keys.
//
// PORTING.md 3.1: effective state = D3D9 defaults <- ENGINE\Phase<phase>.fxo pass 0 <- material
// pass. States are stored by their index in the D3DX effect state table (alamo::fx::stateInfo,
// 0..0xB2), not by D3DRENDERSTATETYPE value, so the FXO state operations apply without a second
// numbering. Enum values below are the d3d9types.h values as stored in FXO state words.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace alamo::fx {
struct Effect;
struct Pass;
struct State;
}  // namespace alamo::fx

namespace alamo::materials {

// ----------------------------------------------------------------- D3DX state-table indices
// Render states: table indices 0..102 (same order as fx2_reader kRender).
enum class RS : uint8_t {
    ZEnable = 0, FillMode, ShadeMode, ZWriteEnable, AlphaTestEnable, LastPixel, SrcBlend, DestBlend,
    CullMode, ZFunc, AlphaRef, AlphaFunc, DitherEnable, AlphaBlendEnable, FogEnable, SpecularEnable,
    FogColor, FogTableMode, FogStart, FogEnd, FogDensity, RangeFogEnable, StencilEnable, StencilFail,
    StencilZFail, StencilPass, StencilFunc, StencilRef, StencilMask, StencilWriteMask, TextureFactor,
    Wrap0, Wrap15 = Wrap0 + 15, Clipping, Lighting, Ambient, FogVertexMode, ColorVertex, LocalViewer,
    NormalizeNormals, DiffuseMaterialSource, SpecularMaterialSource, AmbientMaterialSource,
    EmissiveMaterialSource, VertexBlend, ClipPlaneEnable, PointSize, PointSizeMin, PointSizeMax,
    PointSpriteEnable, PointScaleEnable, PointScaleA, PointScaleB, PointScaleC, MultisampleAntialias,
    MultisampleMask, PatchEdgeStyle, DebugMonitorToken, IndexedVertexBlendEnable, ColorWriteEnable,
    TweenFactor, BlendOp, PositionDegree, NormalDegree, ScissorTestEnable, SlopeScaleDepthBias,
    AntialiasedLineEnable, MinTessellationLevel, MaxTessellationLevel, AdaptiveTessX, AdaptiveTessY,
    AdaptiveTessZ, AdaptiveTessW, EnableAdaptiveTessellation, TwoSidedStencilMode, CcwStencilFail,
    CcwStencilZFail, CcwStencilPass, CcwStencilFunc, ColorWriteEnable1, ColorWriteEnable2,
    ColorWriteEnable3, BlendFactor, SrgbWriteEnable, DepthBias, SeparateAlphaBlendEnable,
    SrcBlendAlpha, DestBlendAlpha, BlendOpAlpha,
    Count
};
static_assert(static_cast<int>(RS::Count) == 103, "render-state table size");

// Texture-stage states: table indices 103..120.
enum class TSS : uint8_t {
    ColorOp = 0, ColorArg0, ColorArg1, ColorArg2, AlphaOp, AlphaArg0, AlphaArg1, AlphaArg2, ResultArg,
    BumpEnvMat00, BumpEnvMat01, BumpEnvMat10, BumpEnvMat11, TexCoordIndex, BumpEnvLScale,
    BumpEnvLOffset, TextureTransformFlags, Constant,
    Count
};
// Sampler states: table indices 165..177 (AddressU .. DMAPOffset).
enum class SS : uint8_t {
    AddressU = 0, AddressV, AddressW, BorderColor, MagFilter, MinFilter, MipFilter, MipMapLodBias,
    MaxMipLevel, MaxAnisotropy, SrgbTexture, ElementIndex, DmapOffset,
    Count
};

namespace op {  // D3DX state-table operation indices used by the resolver
constexpr uint32_t kFirstRender = 0;
constexpr uint32_t kFirstTss = 103;
constexpr uint32_t kTransformProjection = 123, kTransformView = 124, kTransformWorld = 125,
                   kTransformTexture0 = 126;
constexpr uint32_t kMaterialDiffuse = 127, kMaterialAmbient = 128, kMaterialSpecular = 129,
                   kMaterialEmissive = 130, kMaterialPower = 131;
constexpr uint32_t kLightEnable = 145, kVertexShader = 146, kPixelShader = 147;
constexpr uint32_t kTexture = 164, kFirstSampler = 165, kSampler = 178;
constexpr uint32_t kTableSize = 179;
}  // namespace op

// ----------------------------------------------------------------- d3d9types.h enum values
namespace d3d {
enum Blend : uint32_t { kZero = 1, kOne, kSrcColor, kInvSrcColor, kSrcAlpha, kInvSrcAlpha, kDestAlpha,
                        kInvDestAlpha, kDestColor, kInvDestColor, kSrcAlphaSat, kBothSrcAlpha,
                        kBothInvSrcAlpha, kBlendFactor, kInvBlendFactor };
enum Cmp : uint32_t { kNever = 1, kLess, kEqual, kLessEqual, kGreater, kNotEqual, kGreaterEqual, kAlways };
enum StencilOp : uint32_t { kKeep = 1, kSZero, kReplace, kIncrSat, kDecrSat, kInvert, kIncr, kDecr };
enum Cull : uint32_t { kCullNone = 1, kCullCW, kCullCCW };
enum BlendOp : uint32_t { kAdd = 1, kSubtract, kRevSubtract, kMin, kMax };
enum Fill : uint32_t { kPoint = 1, kWireframe, kSolid };
enum TextureOp : uint32_t {
    kDisable = 1, kSelectArg1, kSelectArg2, kModulate, kModulate2x, kModulate4x, kOpAdd, kAddSigned,
    kAddSigned2x, kOpSubtract, kAddSmooth, kBlendDiffuseAlpha, kBlendTextureAlpha, kBlendFactorAlpha,
    kBlendTextureAlphaPM, kBlendCurrentAlpha, kPreModulate, kModulateAlphaAddColor,
    kModulateColorAddAlpha, kModulateInvAlphaAddColor, kModulateInvColorAddAlpha, kBumpEnvMap,
    kBumpEnvMapLuminance, kDotProduct3, kMultiplyAdd, kLerp
};
enum TextureArg : uint32_t { kTaDiffuse = 0, kTaCurrent = 1, kTaTexture = 2, kTaTFactor = 3, kTaSpecular = 4,
                             kTaTemp = 5, kTaConstant = 6, kTaComplement = 0x10, kTaAlphaReplicate = 0x20 };
enum TexFilter : uint32_t { kFilterNone = 0, kFilterPoint = 1, kFilterLinear = 2, kFilterAnisotropic = 3 };
enum TexAddress : uint32_t { kWrap = 1, kMirror, kClamp, kBorder, kMirrorOnce };
enum TexCoordIndexFlags : uint32_t { kTciPassThru = 0, kTciCameraSpaceNormal = 0x10000,
                                     kTciCameraSpacePosition = 0x20000,
                                     kTciCameraSpaceReflectionVector = 0x30000, kTciSphereMap = 0x40000 };
enum TextureTransformFlags : uint32_t { kTtffDisable = 0, kTtffCount1 = 1, kTtffCount4 = 4, kTtffProjected = 256 };
enum MaterialColorSource : uint32_t { kMcsMaterial = 0, kMcsColor1 = 1, kMcsColor2 = 2 };
}  // namespace d3d

// How an entry got its value.
enum class StateOrigin : uint8_t {
    Default = 0,   // D3D9 device default
    Phase,         // phase effect pass 0
    Constant,      // material pass constant
    Expression,    // material pass FXLC expression (value = 0 until evaluated per draw)
    ParameterRef,  // material pass "= <param>" (value resolved by the caller per draw)
};

struct StateSlot {
    uint32_t value = 0;
    StateOrigin origin = StateOrigin::Default;
    int16_t source_state = -1;  // index into the source pass state list (Expression / ParameterRef)
    uint32_t source_pass = 0;   // DeviceState::apply_serial of the apply_pass call that set it
};

constexpr int kMaxTextureStages = 8;
constexpr int kMaxSamplers = 16;

// Texture / sampler binding gathered from Texture[n] / Sampler[n] states and from the shader CTAB.
struct StageBinding {
    std::string texture_param;   // effect parameter name of the texture ("" = none / engine bound)
    std::string sampler_param;   // effect sampler parameter applied by Sampler[n] = <...>
};

struct DeviceState {
    std::array<StateSlot, static_cast<size_t>(RS::Count)> rs{};
    std::array<std::array<StateSlot, static_cast<size_t>(TSS::Count)>, kMaxTextureStages> tss{};
    std::array<std::array<StateSlot, static_cast<size_t>(SS::Count)>, kMaxSamplers> ss{};
    std::array<StageBinding, kMaxSamplers> stage{};
    bool has_vertex_shader = false, has_pixel_shader = false;   // pass set a non-NULL shader
    bool sets_shaders = false;                                  // pass has VertexShader/PixelShader states
    int vertex_shader_state = -1, pixel_shader_state = -1;      // index in the pass state list
    uint32_t vertex_shader_pass = 0, pixel_shader_pass = 0;     // apply_serial those indices refer to
    uint32_t apply_serial = 0;   // incremented by every apply_pass call (0 = none yet)

    // Pass-state indices are only meaningful for the pass of the most recent apply_pass call:
    // indices recorded while applying a baseline (phase) pass must never be used against the
    // material pass.
    bool refers_to_last_pass(const StateSlot& s) const {
        return s.source_state >= 0 && apply_serial != 0 && s.source_pass == apply_serial;
    }
    int last_pass_vertex_shader() const { return vertex_shader_pass == apply_serial ? vertex_shader_state : -1; }
    int last_pass_pixel_shader() const { return pixel_shader_pass == apply_serial ? pixel_shader_state : -1; }

    uint32_t get(RS s) const { return rs[static_cast<size_t>(s)].value; }
    const StateSlot& slot(RS s) const { return rs[static_cast<size_t>(s)]; }
    uint32_t get(int stg, TSS s) const { return tss[static_cast<size_t>(stg)][static_cast<size_t>(s)].value; }
};

// D3D9 device defaults (IDirect3DDevice9 documented reset values; ZENABLE = TRUE as with an
// auto depth-stencil surface).
DeviceState d3d9_defaults();
// The engine's device baseline under the phase effects: D3D9 defaults with
// D3DRS_CULLMODE = D3DCULL_CW (outward ALO faces are counter-clockwise on screen, so the game
// must cull clockwise faces; see pso.hpp). Slots keep StateOrigin::Default.
DeviceState engine_device_defaults();

// Statistics/diagnostics from applying a pass.
struct ApplyReport {
    int expressions = 0;
    int parameter_refs = 0;
    int ignored = 0;          // states with no PSO/FF meaning (transforms, lights, shader consts...)
    std::vector<std::string> unknown;
};

// State indices are stored as int16: passes with more states are rejected by the loader, and
// apply_pass ignores (and reports) states beyond this limit.
constexpr size_t kMaxPassStates = 32767;

// Applies the states of `pass` (states of `effect`) onto `st`. `origin` is Phase for the phase
// baseline, Constant for material passes (expressions / parameter refs are tagged automatically).
// Sampler[n] = <samplerParam> expands the sampler parameter's sampler_state block onto sampler n.
void apply_pass(const fx::Effect& effect, const fx::Pass& pass, StateOrigin origin, DeviceState& st,
                ApplyReport* report = nullptr);

// Applies the sampler_state block of an effect sampler parameter onto sampler slot `n`.
void apply_sampler_param(const fx::Effect& effect, const std::string& sampler_param, int n,
                         StateOrigin origin, DeviceState& st);

}  // namespace alamo::materials
