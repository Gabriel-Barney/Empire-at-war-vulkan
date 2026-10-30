// D3D9 shader bytecode (d3dbc) for SM1-SM2: vs_1_0/1_1, vs_2_0/2_x, ps_1_0..1_4, ps_2_0/2_x.
// The parser is bounds-checked and validates every register reference against the shader model,
// so the emitter and the interpreter can index fixed-size register files without further checks.
// It parses untrusted mod FXO contents: malformed or truncated input returns false + an error
// message, never undefined behaviour. SM3 bytecode parses structurally (so tools can report it)
// but the translator rejects it.
//
// Token format (public D3D9 DDK documentation):
//   version      0xFFFE0000 | major<<8 | minor (VS), 0xFFFF0000 | ... (PS)
//   instruction  bits 0-15 opcode, 16-23 opcode-specific controls (comparison, texld project/bias),
//                24-27 length in DWORDs (SM2+ only; 0 in SM1), 28 predicated, 30 co-issue (ps_1_x)
//   comment      opcode 0xFFFE, bits 16-30 length in DWORDs (CTAB, PRES, ... skipped / read)
//   end          0x0000FFFF
//   operands     bit 31 set; register number bits 0-10; type bits 28-30 | (bits 11-12) << 3;
//                bit 13 relative addressing; destination: 16-19 write mask, 20-23 result
//                modifier (1 saturate, 2 partial precision, 4 centroid), 24-27 shift (signed);
//                source: 16-23 swizzle (2 bits per component), 24-27 source modifier.
//   vs_1_x relative addressing has no extra token (always a0.x); SM2+ adds one address token.
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace alamo::shaderxlat {

enum class ShaderType : uint8_t { Vertex, Pixel };

// D3DSHADER_PARAM_REGISTER_TYPE.
namespace reg {
constexpr uint8_t kTemp = 0, kInput = 1, kConst = 2, kAddr = 3, kTexture = 3, kRastOut = 4, kAttrOut = 5,
                  kTexCrdOut = 6, kOutput = 6, kConstInt = 7, kColorOut = 8, kDepthOut = 9, kSampler = 10,
                  kConst2 = 11, kConst3 = 12, kConst4 = 13, kConstBool = 14, kLoop = 15, kTempFloat16 = 16,
                  kMiscType = 17, kLabel = 18, kPredicate = 19;
}

// D3DSHADER_INSTRUCTION_OPCODE_TYPE.
enum class Op : uint16_t {
    Nop = 0, Mov, Add, Sub, Mad, Mul, Rcp, Rsq, Dp3, Dp4, Min, Max, Slt, Sge, Exp, Log, Lit, Dst, Lrp, Frc,
    M4x4, M4x3, M3x4, M3x3, M3x2, Call, CallNz, Loop, Ret, EndLoop, Label, Dcl, Pow, Crs, Sgn, Abs, Nrm,
    SinCos, Rep, EndRep, If, Ifc, Else, EndIf, Break, Breakc, Mova, Defb, Defi,
    TexCoord = 64, TexKill, Tex, TexBem, TexBemL, TexReg2Ar, TexReg2Gb, TexM3x2Pad, TexM3x2Tex, TexM3x3Pad,
    TexM3x3Tex, Reserved0, TexM3x3Spec, TexM3x3VSpec, ExpP, LogP, Cnd, Def, TexReg2Rgb, TexDp3Tex, TexM3x2Depth,
    TexDp3, TexM3x3, TexDepth, Cmp, Bem, Dp2Add, Dsx, Dsy, TexLdd, Setp, TexLdl, Breakp,
    Phase = 0xFFFD, Comment = 0xFFFE, End = 0xFFFF
};
const char* op_name(Op op);

// D3DSHADER_PARAM_SRCMOD_TYPE.
enum class SrcMod : uint8_t { None = 0, Neg, Bias, BiasNeg, Sign, SignNeg, Comp, X2, X2Neg, Dz, Dw, Abs, AbsNeg, Not };

// Comparison for ifc / breakc / setp (opcode-specific bits 16-18).
enum class Cmp : uint8_t { Never = 0, Gt = 1, Eq = 2, Ge = 3, Lt = 4, Ne = 5, Le = 6, Always = 7 };

// D3DDECLUSAGE.
enum class Usage : uint8_t { Position = 0, BlendWeight, BlendIndices, Normal, PSize, Texcoord, Tangent, Binormal,
                             TessFactor, PositionT, Color, Fog, Depth, Sample };
const char* usage_name(Usage u);

// D3DSAMPLER_TEXTURE_TYPE (dcl_2d/cube/volume) and the CTAB sampler types, normalised.
enum class TextureDim : uint8_t { Unknown = 0, Tex2D, Cube, Volume };

struct Src {
    uint8_t type = 0;
    uint16_t index = 0;
    std::array<uint8_t, 4> swz{0, 1, 2, 3};
    SrcMod mod = SrcMod::None;
    bool relative = false;
    uint8_t rel_type = reg::kAddr;   // kAddr (a0) or kLoop (aL)
    uint8_t rel_comp = 0;            // component of the address register
};

struct Dst {
    uint8_t type = 0;
    uint16_t index = 0;
    uint8_t mask = 0xF;              // bit0 x .. bit3 w
    bool saturate = false;
    bool partial_precision = false;
    bool centroid = false;
    int8_t shift = 0;                // +1 x2, +2 x4, +3 x8, -1 d2, -2 d4, -3 d8
};

struct Instr {
    Op op = Op::Nop;
    uint8_t specific = 0;            // bits 16-23 of the instruction token
    bool coissue = false;            // '+' (ps_1_x): executes in parallel with the previous instruction
    bool predicated = false;
    Src pred;                        // predicate register operand (type kPredicate) when predicated
    bool has_dst = false;
    Dst dst;
    uint8_t nsrc = 0;
    std::array<Src, 4> src{};
    // dcl
    Usage usage = Usage::Position;
    uint8_t usage_index = 0;
    TextureDim tex_dim = TextureDim::Unknown;
    // def / defi / defb: raw 32-bit words (float bits, ints, bool)
    std::array<uint32_t, 4> literal{};
    uint32_t token = 0;              // DWORD offset of the instruction token (diagnostics)
};

// Texld flavours (opcode-specific bits for Tex in SM2+).
constexpr uint8_t kTexldProject = 0x01, kTexldBias = 0x02;

struct CtabEntry {
    std::string name;
    uint16_t register_set = 0;       // 0 bool, 1 int4, 2 float4, 3 sampler
    uint16_t register_index = 0, register_count = 0;
    uint16_t cls = 0, type = 0;      // D3DXPARAMETER_CLASS / D3DXPARAMETER_TYPE (10..14 = samplers)
};

struct Program {
    ShaderType type = ShaderType::Vertex;
    uint8_t major = 0, minor = 0;
    std::vector<Instr> instrs;       // excluding comments and END
    std::vector<CtabEntry> ctab;
    bool has_ctab = false;
    bool has_preshader = false;      // PRES comment (D3DX preshader, evaluated on the CPU by materials)
    uint32_t token_count = 0;        // DWORDs up to and including END

    bool is_ps() const { return type == ShaderType::Pixel; }
    bool is_vs() const { return type == ShaderType::Vertex; }
    // ps_1_0 .. ps_1_4
    bool ps1() const { return is_ps() && major == 1; }
    uint32_t version_token() const;
    std::string profile() const;     // "vs_1_1", "ps_2_0", "vs_2_x" ...
};

// Limits applied while parsing (untrusted input).
constexpr size_t kMaxBytecodeBytes = 1u << 20;     // 1 MiB: vanilla blobs are < 8 KiB
constexpr size_t kMaxInstructions = 16384;

// Parses `bytes` (a whole shader object as stored in an FXO). Returns false and *err on malformed,
// truncated or out-of-range input; trailing bytes after END are allowed (FXO objects are padded).
bool parse(std::span<const uint8_t> bytes, Program& out, std::string* err = nullptr);

// One instruction in D3D9 asm syntax ("mul_x2 r0.xyz, t0, r0"), for diagnostics and tests.
std::string disassemble(const Program& p, const Instr& in);
std::string disassemble(const Program& p);

}  // namespace alamo::shaderxlat
