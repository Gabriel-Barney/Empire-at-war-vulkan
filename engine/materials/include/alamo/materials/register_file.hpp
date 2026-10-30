// CTAB-driven D3D9 register-file fill for translated SM1-3 shaders (PORTING.md 1, interface
// convention): what ID3DXEffect::CommitChanges did. Build a plan once per shader, then fill the
// VSConstants / PSConstants image per draw.
#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "alamo/materials/constants.hpp"
#include "alamo/materials/fxlc.hpp"
#include "alamo/materials/params.hpp"

namespace alamo::fx {
struct Effect;
struct State;
}  // namespace alamo::fx

namespace alamo::materials {

struct RegisterBinding {
    std::string name;            // CTAB constant = effect parameter name
    int parameter = -1;          // index into Effect::parameters, -1 if the effect lacks it
    uint16_t reg = 0, count = 0; // float4 registers c[reg .. reg+count)
    uint16_t cls = 0, rows = 0, columns = 0, elements = 0;
};
struct SamplerRegister {
    std::string sampler_param;   // effect sampler parameter
    uint16_t reg = 0, count = 1; // s#
};

struct RegisterFilePlan {
    bool pixel_shader = false;
    uint8_t major = 0, minor = 0;
    std::vector<RegisterBinding> floats;
    std::vector<SamplerRegister> samplers;
    std::vector<std::string> unbound;        // CTAB names not found in the effect
    uint32_t bool_int_constants = 0;         // b#/i# constants (set 0 binding 2/3 for translated code)
    std::vector<RegisterBinding> ints, bools; // CTAB int4 / bool constants (fill_int_bool_constants)
    std::optional<Expression> preshader;     // PRES block: writes c# outputs after the CTAB fill
    uint32_t used_registers = 0;             // highest c# + 1
};

// Throws nothing; returns false (and *err) on malformed bytecode.
bool build_register_plan(const fx::Effect& effect, std::span<const uint8_t> bytecode, RegisterFilePlan& plan,
                         std::string* err = nullptr);

// Writes the register image (registers not covered by the CTAB are left untouched). `regs` is
// VSConstants::c (256) or PSConstants::c (32).
void fill_register_file(const fx::Effect& effect, const RegisterFilePlan& plan, const ParamLookup& lookup,
                        std::span<float4> regs);
// The two halves of fill_register_file (= every plan.floats binding in order, then the preshader), for callers
// that keep a filled image and rewrite only the bindings whose values changed (render: register images built
// once per material and pass, per-draw parameters patched per draw). No heap allocation for values up to 1024
// floats (a full register file).
void fill_register_binding(const fx::Effect& effect, const RegisterBinding& binding, const ParamLookup& lookup,
                           std::span<float4> regs);
void fill_register_preshader(const fx::Effect& effect, const RegisterFilePlan& plan, const ParamLookup& lookup,
                             std::span<float4> regs);

// A preshader re-run per draw with only some inputs changing: bake_preshader() packs the inputs `per_draw`
// rejects once (input order, the values `lookup` gives now) and records the output components the code writes;
// run_preshader() re-reads the per-draw inputs, runs the code on stack register files and writes exactly what
// fill_register_preshader() would for the same values (no heap allocation, no name lookups). bake_preshader()
// returns false (usable = false) without a preshader, with register files over kMaxFxlcRegisters, or when a
// per-draw input shares registers with another input; callers then use fill_register_preshader().
struct PreshaderBake {
    std::vector<float> inputs;                 // input register file with the static inputs packed
    std::vector<uint16_t> dynamic;             // Expression::inputs indices re-read per run
    std::vector<const fx::Parameter*> params;  // per input (null: not an effect parameter, skipped)
    std::vector<uint8_t> written;              // output components the code writes
    PreparedExpression prepared;               // operands resolved (run_expression when !prepared.ok())
    bool usable = false;
};
bool bake_preshader(const fx::Effect& effect, const RegisterFilePlan& plan, const ParamLookup& lookup,
                    const std::function<bool(const fx::Parameter&)>& per_draw, PreshaderBake& out);
void run_preshader(const RegisterFilePlan& plan, const PreshaderBake& bake, const ParamLookup& lookup, std::span<float4> regs);

// Bytecode of a VertexShader/PixelShader state (inline asm blob, a shader parameter reference,
// or an object id). Empty for NULL / missing.
std::span<const uint8_t> shader_bytecode(const fx::Effect& effect, const fx::State& state);

}  // namespace alamo::materials
