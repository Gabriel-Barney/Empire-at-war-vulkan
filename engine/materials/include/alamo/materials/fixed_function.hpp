// Fixed-function passes -> FF ubershader data (PORTING.md 3.4). The texture-stage configuration
// and FF switches are resolved once per pass; the per-draw values (material colours, TFACTOR,
// texture transforms) are D3DX state expressions evaluated on the CPU per draw.
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "alamo/materials/constants.hpp"
#include "alamo/materials/d3d9_state.hpp"
#include "alamo/materials/fxlc.hpp"
#include "alamo/materials/params.hpp"

namespace alamo::fx {
struct Effect;
struct Pass;
}  // namespace alamo::fx

namespace alamo::materials {

struct FfValue {
    enum class Kind : uint8_t { Default, Constant, Expression, ParameterRef } kind = Kind::Default;
    std::array<float, 16> constant{};   // Constant (and the initial value for Expression)
    std::optional<Expression> expr;
    std::string param;                  // ParameterRef
};

struct FfPassState {
    int active_stages = 0;              // stages before the first COLOROP = DISABLE (max kFfStages)
    bool stages_truncated = false;      // the pass uses more than kFfStages stages
    std::array<FfStage, kFfStages> stage{};
    bool lighting = true, specular = false, color_vertex = true;
    std::array<uint32_t, 4> sources{};  // diffuse, specular, ambient, emissive (D3DMCS_*)
    FfValue mat_diffuse, mat_ambient, mat_specular, mat_emissive, mat_power;
    FfValue texture_factor;             // DWORD D3DCOLOR or expression (float4, quantised to 8 bits)
    std::array<FfValue, kFfStages> tex_transform;
    std::array<std::string, kFfStages> stage_texture;   // texture parameter per stage ("" = engine)
    std::string unsupported;            // first unsupported construct (empty when fully covered)
};

// `st` is the fully resolved device state (defaults <- phase <- pass).
FfPassState extract_ff_state(const fx::Effect& effect, const fx::Pass& pass, const DeviceState& st);

// Per-draw FF evaluation (C4b). build() bakes everything that does not change per draw (stage
// configuration, constant states, material parameters); update() re-evaluates only the state
// expressions that read per-draw engine values (LIGHT_SCALE, TIME, VIEWINVERSE, FOW_TEX_U/V, the
// Colorization override): MaterialDiffuse = m_lightScale * Diffuse, TextureFactor = Colorization,
// FOW / scroll texture transforms... update() is const, allocation-free and thread-safe, so the
// recording workers can call it per draw. update(defaults) == build_ff_draw_data(lookup with
// LIGHT_SCALE = 1, TIME = 0).
class FfDrawTemplate {
public:
    bool build(const FfPassState& ff, const fx::Effect& effect, const ParamLookup& material_lookup,
               bool vertex_has_color, std::string* err = nullptr);
    void update(const ExprInputs& in, FfDrawData& out) const;
    FfDrawData evaluate(const ExprInputs& in) const {
        FfDrawData d{};
        update(in, d);
        return d;
    }
    // True when some state depends on per-draw inputs (otherwise base() is final).
    bool dynamic() const { return !items_.empty(); }
    const FfDrawData& base() const { return base_; }

private:
    enum class Target : uint8_t { Diffuse, Ambient, Specular, Emissive, Power, TextureFactor, TexTransform0 };
    struct Item {
        Target target = Target::Diffuse;
        uint8_t stage = 0;
        std::array<float4, 4> init{};
        CompiledExpression expr;
    };
    FfDrawData base_{};
    std::vector<Item> items_;
};

// Per-draw record. `tex` is filled with pack_texture(kNoTexture, kSamplerWrapLinear); the caller
// patches the bindless indices of stage_texture[].
FfDrawData build_ff_draw_data(const FfPassState& ff, const fx::Effect& effect, const ParamLookup& lookup,
                              bool vertex_has_color);

}  // namespace alamo::materials
