// fx2_reader.cpp - see fx2_reader.hpp
#include "fx2_reader.hpp"

#include <cstddef>
#include <cstring>

namespace alamo::fx {
namespace {

constexpr uint32_t kTagFx20 = 0xFEFF0901u;

class Reader {
public:
    Reader(const uint8_t* d, size_t n) : d_(d), n_(n) {}
    uint32_t u32(size_t off) const {
        if (off > n_ || n_ - off < 4) throw ParseError("read past end");
        uint32_t v;
        std::memcpy(&v, d_ + off, 4);
        return v;
    }
    std::string name(size_t off) const {
        uint32_t len = u32(off);
        if (len == 0) return {};
        if (off + 4 > n_ || n_ - (off + 4) < len) throw ParseError("truncated name");
        const char* s = reinterpret_cast<const char*>(d_ + off + 4);
        return std::string(s, strnlen(s, len));
    }
    std::vector<uint8_t> bytes(size_t off, size_t len) const {
        if (off > n_ || n_ - off < len) throw ParseError("truncated data");
        return std::vector<uint8_t>(d_ + off, d_ + off + len);
    }
    size_t size() const { return n_; }

private:
    const uint8_t* d_;
    size_t n_;
};

bool isSampler(ParamType t) { return t >= ParamType::Sampler && t <= ParamType::SamplerCube; }
bool isObjectId(ParamType t) {
    return t == ParamType::String || (t >= ParamType::Texture && t <= ParamType::TextureCube) ||
           (t >= ParamType::PixelShader && t <= ParamType::VertexFragment);
}

class Parser {
public:
    // Work budget (untrusted input): every typedef node, value leaf, array element and state costs
    // one unit. Offsets are absolute, so values can be re-read through many states / annotations;
    // the budget bounds the total work (and memory) to O(file size) whatever the nesting.
    Parser(const uint8_t* d, size_t n) : r_(d, n), budget_(kBaseBudget + 16 * static_cast<uint64_t>(n)) {}

    Effect run() {
        if (r_.u32(0) != kTagFx20) throw ParseError("not an fx_2_0 effect");
        base_ = 8;
        size_t p = base_ + r_.u32(4);
        uint32_t npar = r_.u32(p), ntech = r_.u32(p + 4), nobj = r_.u32(p + 12);
        (void)nobj;
        p += 16;
        Effect e;
        for (uint32_t i = 0; i < npar; ++i) {
            Parameter par;
            uint32_t toff = r_.u32(p), voff = r_.u32(p + 4);
            par.flags = r_.u32(p + 8);
            uint32_t nann = r_.u32(p + 12);
            p += 16;
            par.annotations = annotations(p, nann);
            p += 8 * nann;
            size_t tp = toff;
            par.type = typedefAt(tp);
            size_t vp = voff;
            par.value = valueAt(par.type, vp, false);
            e.parameters.push_back(std::move(par));
        }
        for (uint32_t i = 0; i < ntech; ++i) {
            Technique t;
            t.name = r_.name(base_ + r_.u32(p));
            uint32_t nann = r_.u32(p + 4), npass = r_.u32(p + 8);
            p += 12;
            t.annotations = annotations(p, nann);
            p += 8 * nann;
            for (uint32_t k = 0; k < npass; ++k) {
                Pass ps;
                ps.name = r_.name(base_ + r_.u32(p));
                uint32_t pann = r_.u32(p + 4), nst = r_.u32(p + 8);
                p += 12;
                ps.annotations = annotations(p, pann);
                p += 8 * pann;
                for (uint32_t s = 0; s < nst; ++s, p += 16) ps.states.push_back(stateAt(p - base_));
                t.passes.push_back(std::move(ps));
            }
            e.techniques.push_back(std::move(t));
        }
        uint32_t nstr = r_.u32(p), nres = r_.u32(p + 4);
        p += 8;
        for (uint32_t i = 0; i < nstr; ++i) {
            uint32_t id = r_.u32(p), sz = r_.u32(p + 4);
            e.objects[id].data = r_.bytes(p + 8, sz);
            p += 8 + ((size_t(sz) + 3) & ~size_t(3));
        }
        for (uint32_t i = 0; i < nres; ++i) {
            uint32_t ti = r_.u32(p), idx = r_.u32(p + 4), ei = r_.u32(p + 8), si = r_.u32(p + 12);
            uint32_t usage = r_.u32(p + 16), sz = r_.u32(p + 20);
            std::vector<uint8_t> data = r_.bytes(p + 24, sz);
            p += 24 + ((size_t(sz) + 3) & ~size_t(3));
            State* st = nullptr;
            if (ti == 0xFFFFFFFFu) {
                if (idx >= e.parameters.size()) throw ParseError("resource parameter index");
                auto& sm = e.parameters[idx].value.samplers;
                size_t el = ei == 0xFFFFFFFFu ? 0 : ei;
                if (el >= sm.size() || si >= sm[el].size()) throw ParseError("resource sampler index");
                st = &sm[el][si];
            } else {
                if (ti >= e.techniques.size() || idx >= e.techniques[ti].passes.size() ||
                    si >= e.techniques[ti].passes[idx].states.size())
                    throw ParseError("resource state index");
                st = &e.techniques[ti].passes[idx].states[si];
            }
            auto cstr = [&](size_t off) {
                if (off >= data.size()) return std::string();
                const char* c = reinterpret_cast<const char*>(data.data() + off);
                return std::string(c, strnlen(c, data.size() - off));
            };
            switch (usage) {
            case 0:
                st->kind = (st->type.type == ParamType::VertexShader || st->type.type == ParamType::PixelShader)
                               ? State::Kind::Shader : State::Kind::Expression;
                st->blob = std::move(data);
                break;
            case 1:
                st->kind = State::Kind::ParameterRef;
                st->refName = cstr(0);
                break;
            case 2: {
                if (data.size() < 4) throw ParseError("array selector");
                uint32_t n;
                std::memcpy(&n, data.data(), 4);
                st->kind = State::Kind::ArraySelector;
                st->refName = cstr(4);
                size_t skip = 4 + ((size_t(n) + 3) & ~size_t(3));
                if (skip <= data.size()) st->blob.assign(data.begin() + static_cast<std::ptrdiff_t>(skip), data.end());
                break;
            }
            default:
                throw ParseError("unknown resource usage");
            }
        }
        if (p != r_.size()) throw ParseError("trailing data after resource table");
        return e;
    }

private:
    static constexpr uint64_t kBaseBudget = 1u << 16;
    static constexpr int kMaxTypeDepth = 32;

    void spend(uint64_t units = 1) {
        if (units > budget_) throw ParseError("effect too complex (work budget exceeded)");
        budget_ -= units;
    }

    TypeDef typedefAt(size_t& off) {
        if (++typeDepth_ > kMaxTypeDepth) throw ParseError("struct nesting too deep");
        spend();
        TypeDef t;
        size_t p = base_ + off;
        t.type = static_cast<ParamType>(r_.u32(p));
        t.cls = static_cast<ParamClass>(r_.u32(p + 4));
        t.name = r_.name(base_ + r_.u32(p + 8));
        t.semantic = r_.name(base_ + r_.u32(p + 12));
        t.elements = r_.u32(p + 16);
        off += 20;
        switch (t.cls) {
        case ParamClass::Scalar: case ParamClass::Vector:
        case ParamClass::MatrixRows: case ParamClass::MatrixColumns:
            t.columns = r_.u32(base_ + off);       // stored (columns, rows)
            t.rows = r_.u32(base_ + off + 4);
            off += 8;
            break;
        case ParamClass::Struct: {
            uint32_t n = r_.u32(base_ + off);
            off += 4;
            if (n > 4096) throw ParseError("struct too large");
            for (uint32_t i = 0; i < n; ++i) t.members.push_back(typedefAt(off));
            break;
        }
        case ParamClass::Object:
            break;
        default:
            throw ParseError("bad parameter class");
        }
        --typeDepth_;
        return t;
    }

    void valueInto(const TypeDef& t, size_t& off, Value& v, bool inElement) {
        if (t.elements && !inElement) {
            if (t.elements > 65536) throw ParseError("array too large");
            for (uint32_t i = 0; i < t.elements; ++i) {
                spend();
                const size_t before = off;
                valueInto(t, off, v, true);
                // Every element of an array consumes the same number of bytes: a zero-size element
                // type (0x0 scalar, empty struct) repeated would spin without reading anything.
                if (off == before && t.elements > 1) throw ParseError("zero-size array element");
            }
            return;
        }
        if (t.cls == ParamClass::Struct) {
            for (const auto& m : t.members) valueInto(m, off, v, false);
            return;
        }
        if (t.cls != ParamClass::Object) {
            uint64_t n = uint64_t(t.rows) * t.columns;
            if (n > 4096) throw ParseError("value too large");
            spend(n);
            for (uint64_t i = 0; i < n; ++i, off += 4) v.words.push_back(r_.u32(base_ + off));
            return;
        }
        if (isSampler(t.type)) {
            uint32_t n = r_.u32(base_ + off);
            off += 4;
            if (n > 256) throw ParseError("too many sampler states");
            spend(n);
            std::vector<State> states;
            for (uint32_t i = 0; i < n; ++i, off += 16) states.push_back(stateAt(off));
            v.samplers.push_back(std::move(states));
            return;
        }
        if (isObjectId(t.type)) {
            spend();
            v.objects.push_back(r_.u32(base_ + off));
            off += 4;
            return;
        }
        throw ParseError("unsupported object type");
    }

    Value valueAt(const TypeDef& t, size_t& off, bool inElement) {
        Value v;
        valueInto(t, off, v, inElement);
        return v;
    }

    State stateAt(size_t off) {
        if (++depth_ > 16) throw ParseError("state nesting too deep");
        State s;
        s.operation = r_.u32(base_ + off);
        s.index = r_.u32(base_ + off + 4);
        size_t tp = r_.u32(base_ + off + 8);
        s.type = typedefAt(tp);
        size_t vp = r_.u32(base_ + off + 12);
        s.value = valueAt(s.type, vp, false);
        --depth_;
        return s;
    }

    std::vector<Annotation> annotations(size_t p, uint32_t n) {
        std::vector<Annotation> out;
        for (uint32_t i = 0; i < n; ++i) {
            Annotation a;
            size_t tp = r_.u32(p + 8 * i), vp = r_.u32(p + 8 * i + 4);
            a.type = typedefAt(tp);
            a.value = valueAt(a.type, vp, false);
            out.push_back(std::move(a));
        }
        return out;
    }

    Reader r_;
    uint64_t budget_;
    size_t base_ = 8;
    int depth_ = 0;
    int typeDepth_ = 0;
};

const char* kRender[] = {
    "ZENABLE", "FILLMODE", "SHADEMODE", "ZWRITEENABLE", "ALPHATESTENABLE", "LASTPIXEL", "SRCBLEND",
    "DESTBLEND", "CULLMODE", "ZFUNC", "ALPHAREF", "ALPHAFUNC", "DITHERENABLE", "ALPHABLENDENABLE",
    "FOGENABLE", "SPECULARENABLE", "FOGCOLOR", "FOGTABLEMODE", "FOGSTART", "FOGEND", "FOGDENSITY",
    "RANGEFOGENABLE", "STENCILENABLE", "STENCILFAIL", "STENCILZFAIL", "STENCILPASS", "STENCILFUNC",
    "STENCILREF", "STENCILMASK", "STENCILWRITEMASK", "TEXTUREFACTOR", "WRAP0", "WRAP1", "WRAP2",
    "WRAP3", "WRAP4", "WRAP5", "WRAP6", "WRAP7", "WRAP8", "WRAP9", "WRAP10", "WRAP11", "WRAP12",
    "WRAP13", "WRAP14", "WRAP15", "CLIPPING", "LIGHTING", "AMBIENT", "FOGVERTEXMODE", "COLORVERTEX",
    "LOCALVIEWER", "NORMALIZENORMALS", "DIFFUSEMATERIALSOURCE", "SPECULARMATERIALSOURCE",
    "AMBIENTMATERIALSOURCE", "EMISSIVEMATERIALSOURCE", "VERTEXBLEND", "CLIPPLANEENABLE", "POINTSIZE",
    "POINTSIZE_MIN", "POINTSIZE_MAX", "POINTSPRITEENABLE", "POINTSCALEENABLE", "POINTSCALE_A",
    "POINTSCALE_B", "POINTSCALE_C", "MULTISAMPLEANTIALIAS", "MULTISAMPLEMASK", "PATCHEDGESTYLE",
    "DEBUGMONITORTOKEN", "INDEXEDVERTEXBLENDENABLE", "COLORWRITEENABLE", "TWEENFACTOR", "BLENDOP",
    "POSITIONDEGREE", "NORMALDEGREE", "SCISSORTESTENABLE", "SLOPESCALEDEPTHBIAS",
    "ANTIALIASEDLINEENABLE", "MINTESSELLATIONLEVEL", "MAXTESSELLATIONLEVEL", "ADAPTIVETESS_X",
    "ADAPTIVETESS_Y", "ADAPTIVETESS_Z", "ADAPTIVETESS_W", "ENABLEADAPTIVETESSELLATION",
    "TWOSIDEDSTENCILMODE", "CCW_STENCILFAIL", "CCW_STENCILZFAIL", "CCW_STENCILPASS",
    "CCW_STENCILFUNC", "COLORWRITEENABLE1", "COLORWRITEENABLE2", "COLORWRITEENABLE3", "BLENDFACTOR",
    "SRGBWRITEENABLE", "DEPTHBIAS", "SEPARATEALPHABLENDENABLE", "SRCBLENDALPHA", "DESTBLENDALPHA",
    "BLENDOPALPHA"};
const char* kTss[] = {"COLOROP", "COLORARG0", "COLORARG1", "COLORARG2", "ALPHAOP", "ALPHAARG0",
                      "ALPHAARG1", "ALPHAARG2", "RESULTARG", "BUMPENVMAT00", "BUMPENVMAT01",
                      "BUMPENVMAT10", "BUMPENVMAT11", "TEXCOORDINDEX", "BUMPENVLSCALE",
                      "BUMPENVLOFFSET", "TEXTURETRANSFORMFLAGS", "CONSTANT"};
const char* kTail[] = {
    "NPatchMode", "FVF", "PROJECTION", "VIEW", "WORLD", "TEXTURE0", "MaterialDiffuse",
    "MaterialAmbient", "MaterialSpecular", "MaterialEmissive", "MaterialPower", "LightType",
    "LightDiffuse", "LightSpecular", "LightAmbient", "LightPosition", "LightDirection", "LightRange",
    "LightFalloff", "LightAttenuation0", "LightAttenuation1", "LightAttenuation2", "LightTheta",
    "LightPhi", "LightEnable", "VertexShader", "PixelShader", "VertexShaderConstantF",
    "VertexShaderConstantB", "VertexShaderConstantI", "VertexShaderConstant",
    "VertexShaderConstant1", "VertexShaderConstant2", "VertexShaderConstant3",
    "VertexShaderConstant4", "PixelShaderConstantF", "PixelShaderConstantB",
    "PixelShaderConstantI", "PixelShaderConstant", "PixelShaderConstant1", "PixelShaderConstant2",
    "PixelShaderConstant3", "PixelShaderConstant4", "Texture", "AddressU", "AddressV", "AddressW",
    "BorderColor", "MagFilter", "MinFilter", "MipFilter", "MipMapLodBias", "MaxMipLevel",
    "MaxAnisotropy", "SRGBTexture", "ElementIndex", "DMAPOffset", "Sampler"};
constexpr size_t kNumRender = sizeof(kRender) / sizeof(*kRender);   // 103
constexpr size_t kNumTss = sizeof(kTss) / sizeof(*kTss);            // 18
constexpr size_t kNumTail = sizeof(kTail) / sizeof(*kTail);
static_assert(kNumRender + kNumTss + kNumTail == 0xB3, "state table size");

}  // namespace

const StateInfo& stateInfo(uint32_t op) {
    static std::vector<StateInfo> table = [] {
        std::vector<StateInfo> t;
        for (auto n : kRender) t.push_back({StateClass::Render, n});
        for (auto n : kTss) t.push_back({StateClass::TextureStage, n});
        for (size_t i = 0; i < kNumTail; ++i) {
            StateClass c;
            if (i == 0) c = StateClass::NPatch;
            else if (i == 1) c = StateClass::FVF;
            else if (i < 6) c = StateClass::Transform;
            else if (i < 11) c = StateClass::Material;
            else if (i < 24) c = StateClass::Light;
            else if (i == 24) c = StateClass::LightEnable;
            else if (i == 25) c = StateClass::VertexShader;
            else if (i == 26) c = StateClass::PixelShader;
            else if (i < 43) c = StateClass::ShaderConst;
            else if (i == 43) c = StateClass::Texture;
            else if (i < 57) c = StateClass::Sampler;
            else c = StateClass::SetSampler;
            t.push_back({c, kTail[i]});
        }
        return t;
    }();
    static const StateInfo unknown{StateClass::Unknown, "unknown"};
    return op < table.size() ? table[op] : unknown;
}

Effect parseEffect(const uint8_t* data, size_t size) { return Parser(data, size).run(); }

const Parameter* Effect::findParameter(const std::string& name) const {
    for (const auto& p : parameters)
        if (p.type.name == name) return &p;
    return nullptr;
}

std::optional<std::string> Effect::string(uint32_t id) const {
    auto it = objects.find(id);
    if (it == objects.end() || it->second.data.empty()) return std::nullopt;
    const auto& d = it->second.data;
    return std::string(reinterpret_cast<const char*>(d.data()),
                       strnlen(reinterpret_cast<const char*>(d.data()), d.size()));
}

std::string Effect::techniqueLod(const Technique& t) const {
    for (const auto& a : t.annotations)
        if (a.type.name == "LOD" && !a.value.objects.empty())
            if (auto s = string(a.value.objects[0])) return *s;
    return {};
}

ShaderInfo inspectShader(const uint8_t* data, size_t size) {
    Reader r(data, size);
    ShaderInfo si;
    uint32_t ver = r.u32(0);
    si.isPixelShader = (ver >> 16) == 0xFFFF;
    si.major = uint8_t(ver >> 8);
    si.minor = uint8_t(ver);
    size_t n = size / 4;
    for (size_t p = 1; p < n;) {
        uint32_t tok = r.u32(4 * p);
        if ((tok & 0xFFFF) == 0xFFFE) {
            uint32_t len = (tok >> 16) & 0x7FFF;
            if (p + 1 + len > n) throw ParseError("comment past end");
            uint32_t fourcc = len ? r.u32(4 * (p + 1)) : 0;
            if (fourcc == 0x42415443u /* CTAB */ && len >= 8) {
                size_t b = 4 * (p + 2);                 // CTAB payload start
                size_t blen = 4 * size_t(len - 1);
                Reader c(data + b, blen);
                uint32_t count = c.u32(12), info = c.u32(16);
                for (uint32_t i = 0; i < count && i < 4096; ++i) {
                    size_t ci = info + 20 * size_t(i);
                    CtabConstant k;
                    uint32_t nameOff = c.u32(ci);
                    uint32_t w1 = c.u32(ci + 4), w2 = c.u32(ci + 8), toff = c.u32(ci + 12);
                    k.registerSet = uint16_t(w1);
                    k.registerIndex = uint16_t(w1 >> 16);
                    k.registerCount = uint16_t(w2);
                    if (nameOff < blen) {
                        const char* s = reinterpret_cast<const char*>(data + b + nameOff);
                        k.name.assign(s, strnlen(s, blen - nameOff));
                    }
                    uint32_t t0 = c.u32(toff), t1 = c.u32(toff + 4), t2 = c.u32(toff + 8);
                    k.cls = uint16_t(t0); k.type = uint16_t(t0 >> 16);
                    k.rows = uint16_t(t1); k.columns = uint16_t(t1 >> 16);
                    k.elements = uint16_t(t2);
                    si.constants.push_back(std::move(k));
                }
            } else if (fourcc == 0x53455250u /* PRES */ && len > 2) {
                si.hasPreshader = true;
            }
            p += 1 + len;
            continue;
        }
        if (tok == 0x0000FFFFu) {
            si.endTokenFound = true;
            break;
        }
        ++si.instructionTokens;
        ++p;
    }
    return si;
}

}  // namespace alamo::fx
