// CPU reference interpreter for parsed SM1-3 programs. It implements the semantics documented in
// translate.cpp's header table directly on the instruction list (no shared code with the HLSL
// emitter beyond the parser and the interface helpers), so the GPU differential test compares
// two independent implementations. Robust against any program the parser accepts: execution is
// bounded (instruction budget, call depth), so fuzzed input never hangs or crashes.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "alamo/shaderxlat/bytecode.hpp"
#include "alamo/shaderxlat/translate.hpp"

namespace alamo::shaderxlat {

using F4 = std::array<float, 4>;

// Texture lookups for the pixel interpreter (implicit LOD 0: tests use single-level textures).
class TextureSource {
public:
    virtual ~TextureSource() = default;
    // coord.xy for 2D, coord.xyz for cube / volume.
    virtual F4 sample(uint32_t slot, TextureDim dim, const F4& coord) const = 0;
};

struct ConstantState {
    std::array<F4, 256> c{};                       // VS uses 256, PS 32
    std::array<std::array<int32_t, 4>, 16> i{};
    std::array<uint32_t, 16> b{};
};

struct VsResult {
    F4 pos{}, color0{}, color1{};
    std::array<F4, 8> texcoord{};
    float fog = 1.0f, point_size = 1.0f;
};

struct PsInputs {
    std::array<F4, 2> color{};
    std::array<F4, 8> texcoord{};
    float fog = 1.0f;
    F4 frag_pos{0.5f, 0.5f, 0.5f, 1.0f};           // SV_Position (x, y, z, 1/w)
    bool front_face = true;                        // ps_3_0 vFace
};

struct PsResult {
    std::array<F4, 4> color{};
    float depth = 0.0f;
    bool depth_written = false;
    bool killed = false;                           // texkill or alpha test
};

class Interpreter {
public:
    // False (+ *err) for programs the translator would also reject (unbalanced control flow, opcodes
    // outside their shader type, SM3 usages without a varying slot, vertex texture fetch).
    bool init(const Program& program, const TranslateOptions& options, std::string* err = nullptr);

    // VS inputs by register v# (not by location).
    bool run_vs(const std::array<F4, 16>& inputs, const ConstantState& constants, const TranslatedPush& push,
                VsResult& out) const;
    bool run_ps(const PsInputs& inputs, const ConstantState& constants, const TranslatedPush& push,
                const TextureSource* textures, PsResult& out) const;

    const Program& program() const { return prog_; }

private:
    struct Frame;
    bool execute(Frame& f) const;

    Program prog_;
    TranslateOptions opt_;
    std::array<bool, 256> cdef_{};
    std::array<F4, 256> cval_{};
    std::array<bool, 16> idef_{}, bdef_{};
    std::array<std::array<int32_t, 4>, 16> ival_{};
    std::array<uint32_t, 16> bval_{};
    std::array<TextureDim, 16> dims_{};
    struct IoDecl {
        bool present = false;
        Usage usage = Usage::Position;
        uint8_t index = 0;
    };
    std::array<IoDecl, 16> io_{};    // SM3: vs_3_0 o# / ps_3_0 v# dcl usages
    std::vector<int32_t> match_;     // if -> else/endif, else -> endif, rep/loop <-> end
    std::array<int32_t, 2048> labels_{};
    bool writes_depth_ = false;
};

}  // namespace alamo::shaderxlat
