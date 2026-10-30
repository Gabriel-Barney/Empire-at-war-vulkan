// D3DX FXLC expression evaluator: effect state expressions (AlphaBlendEnable = (m_lightScale.w < 1),
// TextureFactor = Colorization, MaterialDiffuse = m_lightScale * Diffuse, FOW texture transforms)
// and shader preshaders (PRES, Patch2 FXOs). Evaluated on the CPU per draw, as D3DX did.
//
// Stream format (tools/shaders/d3d9sm.py parse_fxlc): version token 0x4658xxxx, comment blocks
// CTAB (inputs), CLIT (double literals), FXLC (code), PRSI; END. Operand tables: 1 literal,
// 2 input constant, 4 output, 7 temp; offsets are in components (register = offset / 4).
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "alamo/materials/constants.hpp"
#include "alamo/materials/params.hpp"

namespace alamo::fx {
struct Effect;
}

namespace alamo::materials {

// Upper bound of every FXLC register file (inputs, temps, outputs, literals in float4 units):
// the size of the D3D9 vertex float register file. Streams referencing more fail to parse.
constexpr uint32_t kMaxFxlcRegisters = 256;

struct FxlcOperand {
    uint32_t table = 0, offset = 0;
    bool indexed = false;
    uint32_t index_table = 0, index_offset = 0;
};
struct FxlcInstruction {
    uint32_t op = 0, comps = 0;
    bool scalar = false;
    std::vector<FxlcOperand> inputs;
    FxlcOperand output;
};
struct FxlcInput {           // CTAB entry of the expression
    std::string name;
    uint16_t reg = 0, count = 0, cls = 0, rows = 0, columns = 0, elements = 0;
};
struct Expression {
    uint32_t version = 0;
    std::vector<double> literals;
    std::vector<FxlcInput> inputs;
    std::vector<FxlcInstruction> code;
    uint32_t output_registers = 0;   // highest written output register + 1
    uint32_t input_registers = 0, temp_registers = 0;
};

// Parses an FXLC stream (state expression blob or the payload of a shader PRES comment).
bool parse_expression(const uint8_t* data, size_t size, Expression& out, std::string* err = nullptr);
// True if every opcode is implemented by evaluate().
bool expression_supported(const Expression& e, std::string* first_unsupported = nullptr);
// Evaluates with inputs bound by name to `effect` parameters (lookup, else defaults). Writes
// `out` registers (expr0..exprN for states; c# for preshaders). False on unsupported ops.
bool evaluate(const Expression& e, const fx::Effect& effect, const ParamLookup& lookup, std::span<float4> out,
              std::string* err = nullptr);

// Runs the code on caller-provided register files (no allocation): `in` / `tmp` / `res` hold at
// least input_registers / temp_registers / output_registers float4s (4 floats each).
bool run_expression(const Expression& e, std::span<float> in, std::span<float> tmp, std::span<float> res,
                    std::string* err = nullptr);

// An expression resolved once for repeated runs (per-draw preshader re-runs): every operand component becomes a
// direct index into one stack register file [inputs | temps | outputs] or the literal table (literals converted to
// float once). run() gives bit-identical results to run_expression() with register files of the prepared sizes
// (same operations in the same order, same zero reads out of range). prepare() fails (use run_expression) for
// indexed operands, unsupported opcodes or files over kMaxFxlcRegisters.
class PreparedExpression {
public:
    static constexpr size_t kFile = size_t(kMaxFxlcRegisters) * 4;
    bool prepare(const Expression& e, size_t in_floats, size_t tmp_floats, size_t res_floats);
    bool ok() const { return ok_; }
    // in: in_floats input values (copied), res: receives res_floats outputs. No allocation; thread-safe (const).
    void run(std::span<const float> in, std::span<float> res) const;

private:
    struct Ins {
        uint32_t op = 0, n = 1;
        bool skip = false;
        int32_t src[3][4] = {};
        int32_t dst[4] = {};
    };
    std::vector<Ins> code_;
    std::vector<float> lits_;
    size_t nin_ = 0, ntmp_ = 0, nres_ = 0;
    bool ok_ = false;
};

// Engine values that change per draw or per frame and feed state expressions. Everything else an
// expression reads (material parameters, effect defaults) is baked when it is compiled.
struct ExprInputs {
    float4 light_scale{1, 1, 1, 1};            // LIGHT_SCALE (.w = fade alpha)
    float time = 0.0f;                         // TIME (seconds)
    const float4* colorization = nullptr;      // material "Colorization" override (team colour)
    const float* view_inv = nullptr;           // VIEWINVERSE, row-major D3DXMATRIX (16 floats)
    const float4* fow_tex_u = nullptr;         // FOW_TEX_U
    const float4* fow_tex_v = nullptr;         // FOW_TEX_V
    const float4* cloud_tex_u = nullptr;       // CLOUD_TEX_U (land: the terrain's cloud matrices; null = baked)
    const float4* cloud_tex_v = nullptr;       // CLOUD_TEX_V
};

// An expression with its static inputs baked: run() only patches the dynamic inputs and
// evaluates on stack buffers. Thread-safe (const), no allocation per call.
class CompiledExpression {
public:
    enum class Source : uint8_t { LightScale, Time, Colorization, ViewInv, FowTexU, FowTexV, CloudTexU, CloudTexV };
    // False if the expression uses an unsupported opcode.
    bool compile(const Expression& e, const fx::Effect& effect, const ParamLookup& material_lookup,
                 std::string* err = nullptr);
    // Writes min(out.size(), output_registers) registers; registers the code does not write keep
    // the caller's values (D3DX evaluates into the state's current value).
    void run(const ExprInputs& in, std::span<float4> out) const;
    bool depends_on(Source s) const;
    bool dynamic() const { return !bindings_.empty(); }
    const Expression& expression() const { return expr_; }

private:
    struct Binding {
        Source source;
        uint16_t reg = 0, count = 0, cls = 0, rows = 0, columns = 0, elements = 0;
        std::array<float, 16> fallback{};   // baked value when the input pointer is null
    };
    Expression expr_;
    std::vector<float> baked_;                    // input_registers * 4 floats
    std::vector<Binding> bindings_;
    std::vector<uint8_t> written_;                // output components the code writes
};

// Packs a parameter value into float4 registers per CTAB class (0 scalar, 1 vector: one register
// per element; 2 matrix_rows: one per row; 3 matrix_columns: one per column), at most
// `max_registers`. Returns the number of registers written.
uint32_t pack_registers(std::span<const float> value, uint16_t cls, uint16_t rows, uint16_t columns,
                        uint16_t elements, uint32_t max_registers, std::span<float4> out);

}  // namespace alamo::materials
