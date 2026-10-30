// fx2_reader.hpp - C++17 reader for compiled D3DX9 effects (fx_2_0, tag 0xFEFF0901) and
// D3D9 shader bytecode CTAB tables. Port of tools/shaders/fx2.py / d3d9sm.py (parsing only).
// No dependencies; does not touch D3D. Intended for the Vulkan renderer's effect loader so that
// vanilla and mod .fxo files can be ingested at runtime.
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace alamo::fx {

enum class ParamType : uint32_t {
    Void = 0, Bool, Int, Float, String, Texture, Texture1D, Texture2D, Texture3D, TextureCube,
    Sampler, Sampler1D, Sampler2D, Sampler3D, SamplerCube, PixelShader, VertexShader,
    PixelFragment, VertexFragment, Unsupported
};
enum class ParamClass : uint32_t { Scalar = 0, Vector, MatrixRows, MatrixColumns, Object, Struct };

struct TypeDef {
    ParamType type{};
    ParamClass cls{};
    std::string name, semantic;
    uint32_t elements = 0, rows = 0, columns = 0;
    std::vector<TypeDef> members;   // struct members
};

struct State;

// Value of a parameter / annotation / state (flattened):
//  - numeric: raw 32-bit words (interpret by TypeDef::type)
//  - object:  object ids (strings, textures, shaders), one per element
//  - sampler: list of sampler states, one list per element
struct Value {
    std::vector<uint32_t> words;
    std::vector<uint32_t> objects;
    std::vector<std::vector<State>> samplers;
};

struct Annotation {
    TypeDef type;
    Value value;
};

enum class StateClass { Render, TextureStage, NPatch, FVF, Transform, Material, Light, LightEnable,
                        VertexShader, PixelShader, ShaderConst, Texture, Sampler, SetSampler, Unknown };

struct State {
    uint32_t operation = 0;     // index into the D3DX state table (see stateInfo())
    uint32_t index = 0;         // stage / sampler / light index
    TypeDef type;
    Value value;
    // filled from the resource table:
    enum class Kind { Constant, ParameterRef, Expression, Shader, ArraySelector } kind = Kind::Constant;
    std::string refName;        // ParameterRef / ArraySelector
    std::vector<uint8_t> blob;  // Expression (FXLC preshader stream) or Shader bytecode
};

struct Parameter {
    TypeDef type;
    uint32_t flags = 0;
    std::vector<Annotation> annotations;
    Value value;
};

struct Pass {
    std::string name;
    std::vector<Annotation> annotations;
    std::vector<State> states;
};

struct Technique {
    std::string name;
    std::vector<Annotation> annotations;
    std::vector<Pass> passes;
};

struct Object {
    std::vector<uint8_t> data;    // string bytes (NUL-terminated) or shader bytecode
};

struct Effect {
    std::vector<Parameter> parameters;
    std::vector<Technique> techniques;
    std::map<uint32_t, Object> objects;

    const Parameter* findParameter(const std::string& name) const;
    std::optional<std::string> string(uint32_t objectId) const;
    // "LOD" annotation of a technique ("DX9", "DX8ATI", "DX8", "FIXEDFUNCTION"...), empty if none
    std::string techniqueLod(const Technique& t) const;
};

struct StateInfo { StateClass cls; const char* name; };
const StateInfo& stateInfo(uint32_t operation);

class ParseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Throws ParseError on malformed input. Never reads out of bounds.
Effect parseEffect(const uint8_t* data, size_t size);

// ---------------------------------------------------------------- D3D9 bytecode helpers
struct CtabConstant {
    std::string name;
    uint16_t registerSet = 0;   // 0 bool, 1 int4, 2 float4, 3 sampler
    uint16_t registerIndex = 0, registerCount = 0;
    uint16_t cls = 0, type = 0, rows = 0, columns = 0, elements = 0;
};

struct ShaderInfo {
    bool isPixelShader = false;
    uint8_t major = 0, minor = 0;
    std::vector<CtabConstant> constants;   // from the CTAB comment
    bool hasPreshader = false;
    uint32_t instructionTokens = 0;        // non-comment tokens between version and END
    bool endTokenFound = false;
};

// Walks comment blocks, reads CTAB, finds END. Works for vs_1_1..3_0 / ps_1_1..3_0 and for
// FXLC expression streams (version 0x4658xxxx).
ShaderInfo inspectShader(const uint8_t* data, size_t size);

}  // namespace alamo::fx
