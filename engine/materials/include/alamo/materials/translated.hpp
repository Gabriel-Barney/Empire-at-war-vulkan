// Track B integration (PORTING.md 1B): FXO passes whose bytecode has no hand-written program are
// translated by engine/shaderxlat (D3D9 SM1-2 -> SM6 HLSL -> SPIR-V through runtime DXC).
//
// Load time (EffectLibrary::load, no DXC): resolve_pass keeps the pass bytecode, computes the
// TranslateOptions (sampler dims for stripped CTABs, projected ps_1_x stages), the 64-byte
// TranslatedPush defaults and the program ids (stable, kProgramIdTranslatedBit set), and marks the
// pass ProgramStatus::Translated.
// Draw-preparation time (renderer worker jobs): translate_pass() returns the SPIR-V and the
// shader interfaces (disk cache first). Thread-safe; RuntimeDxc/ShaderCache are shared read-only.
//
// Interface of a translated pass (shaderxlat translate.hpp), different from the hot set:
//   set 0 binding 0  VSConstants (float4 c[256])  - fill_register_file(vs_plan)
//   set 0 binding 1  PSConstants (float4 c[32])   - fill_register_file(ps_plan) + fill_bump_env()
//   set 0 binding 2  VSIntConstants { int4 i[16]; uint4 b[4]; }  only if vs iface.int_bool_constants
//   set 0 binding 3  PSIntConstants                               only if ps iface.int_bool_constants
//   set 1 binding n  combined image sampler s_n (ResolvedPass::samplers: texture + SamplerKey)
//   push constant    TranslatedPush, 64 bytes, VS + PS
//   vertex inputs    one float4 per VS dcl at fixed locations (TranslatedInput); EVERY declared
//                    input must be fed: inputs the mesh layout lacks (fed = false) need a zero buffer.
// A translated PS links only with a translated VS (varyings matched by D3D semantic).
#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "alamo/materials/constants.hpp"
#include "alamo/materials/params.hpp"
#include "alamo/materials/programs.hpp"
#include "alamo/materials/pso.hpp"
#include "alamo/shaderxlat/shaderxlat.hpp"

namespace alamo::fx {
struct Effect;
}

namespace alamo::materials {

struct ResolvedPass;

// Stable program id of translated bytecode: FNV-1a of (bytecode, options key, translator
// version), folded to 31 bits, kProgramIdTranslatedBit set. Known at load (no DXC needed).
// Ids are unique per process: see register_program_id().
uint32_t translated_program_id(std::span<const uint8_t> bytecode, const shaderxlat::TranslateOptions& options);

// Process-wide registry of kProgramIdTranslatedBit ids: maps the 31-bit fold of `full_hash` to
// itself. A fold already taken by a different full hash probes linearly to the next free id
// (deterministic for a given registration order), so two different programs never share a
// PsoKey id. The same full hash always returns the same id. Thread-safe.
uint32_t register_program_id(uint64_t full_hash);

// PsoKey::flags bits (former reserved byte; 0 for hand-written / FF keys, so their hashes are unchanged).
enum PsoFlags : uint8_t {
    kPsoTranslated = 1u << 0,   // translated VS + PS (TranslatedPush layout, combined samplers)
    kPsoWritesDepth = 1u << 1,  // PS writes SV_Depth: keep out of early-Z sensitive batches
};

struct TranslatedInput {
    uint32_t location = 0;
    shaderxlat::Usage usage = shaderxlat::Usage::Position;
    uint8_t usage_index = 0;
    uint8_t components = 4;
    bool fed = false;           // the pass's mesh vertex layout provides this location
    uint32_t vk_format = 0;     // its format when fed (vertex_layout_desc), else 0 (bind a zero buffer)
    uint32_t offset = 0;
};

struct TranslatedProgram {
    shaderxlat::CompiledProgram vs, ps;
    uint32_t vs_id = 0, ps_id = 0;          // == the pass PsoKey ids
    std::vector<TranslatedInput> inputs;    // VS dcl inputs, sorted by location
    bool writes_depth = false, can_discard = false, bump_env = false;
    bool vs_int_bool = false, ps_int_bool = false;
    std::vector<ResourceBinding> bindings;  // set/binding table of this program
    // Pass PsoKey with the translated flags (and writes-depth) applied.
    PsoKey key(const PsoKey& pass_key) const;
};

// Translates + compiles the pass's VS and PS (cache first). Fails for passes whose status is not
// ProgramStatus::Translated or when DXC rejects the HLSL (*err has the diagnostics).
std::shared_ptr<const TranslatedProgram> translate_pass(const ResolvedPass& pass, const shaderxlat::RuntimeDxc& dxc,
                                                        const shaderxlat::ShaderCache* cache, std::string* err = nullptr);

// PSConstants c16+stage = (BUMPENVMAT00, 01, 10, 11), c24+stage = (BUMPENVLSCALE, BUMPENVLOFFSET, 0, 0)
// from the pass state (constants or FXLC expressions evaluated with `lookup`), stages 0..7.
// No-op (c16..c31 untouched) unless pass.uses_bump_env (ps_1_x pixel shader with texbem/texbeml/bem):
// for ps_2_x those registers are ordinary CTAB constants.
void fill_bump_env(const ResolvedPass& pass, const fx::Effect& effect, const ParamLookup& lookup,
                   std::span<float4> ps_regs);

// set 0 binding 2 / 3 image (std140/dx: int4 i[16]; uint4 b[4]), 320 bytes.
struct IntBoolConstants {
    int32_t i[16][4] = {};
    uint32_t b[4][4] = {};
};
static_assert(sizeof(IntBoolConstants) == 320);
struct RegisterFilePlan;
void fill_int_bool_constants(const fx::Effect& effect, const RegisterFilePlan& plan, const ParamLookup& lookup,
                             IntBoolConstants& out);

}  // namespace alamo::materials
