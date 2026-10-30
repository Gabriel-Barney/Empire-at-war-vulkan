// CPU D3D9 fixed-function reference. See ff_reference.hpp.
#include "ff_reference.hpp"

#include <algorithm>
#include <cmath>

namespace alamo::reftest {
namespace {

double sat(double x) { return std::clamp(x, 0.0, 1.0); }
Vec4 sat4(const Vec4& v) { return {sat(v[0]), sat(v[1]), sat(v[2]), sat(v[3])}; }

RefStage stage(int cop, int ca1, int ca2, int aop, int aa1, int aa2) {
    RefStage s;
    s.color_op = cop;
    s.color_arg1 = ca1;
    s.color_arg2 = ca2;
    s.alpha_op = aop;
    s.alpha_arg1 = aa1;
    s.alpha_arg2 = aa2;
    return s;
}

double op(int o, double a1, double a2, double current) {
    switch (o) {
    case d9::kSelectArg1: return a1;
    case d9::kSelectArg2: return a2;
    case d9::kModulate: return a1 * a2;
    case d9::kModulate2x: return 2.0 * a1 * a2;
    case d9::kModulate4x: return 4.0 * a1 * a2;
    case d9::kAdd: return a1 + a2;
    default: return current;
    }
}

double blend_factor(int f, double src_c, double src_a, double dst_c) {
    switch (f) {
    case d9::kBlendZero: return 0.0;
    case d9::kBlendOne: return 1.0;
    case d9::kSrcColor: return src_c;
    case d9::kInvSrcColor: return 1.0 - src_c;
    case d9::kSrcAlpha: return src_a;
    case d9::kInvSrcAlpha: return 1.0 - src_a;
    case d9::kDestColor: return dst_c;
    default: return 1.0;
    }
}

}  // namespace

std::vector<RefEffect> reference_effects() {
    using namespace d9;
    std::vector<RefEffect> v;
    {   // MESHALPHA sph_t1 [FIXEDFUNCTION]
        RefEffect e;
        e.name = "MeshAlpha";
        e.lighting = true;
        e.stages = {stage(kModulate2x, kTexture, kDiffuse, kModulate, kTexture, kDiffuse)};
        e.blend = true;
        e.src_blend = kSrcAlpha;
        e.dst_blend = kInvSrcAlpha;
        e.z_write = false;
        v.push_back(e);
        e.name = "MeshAlphaScroll";  // same pass + TEXTURE0[0] scroll, TEXTURETRANSFORMFLAGS[0] = COUNT2
        e.scroll = true;
        v.push_back(e);
    }
    {   // MESHADDITIVE t0 [DX8]: ps_1_1 r0 = t0 * v0; vs: v0 = (lightScale.rgb * Color * lightScale.w, 1); ONE/ONE
        RefEffect e;
        e.name = "MeshAdditive";
        e.shader_additive = true;
        e.blend = true;
        e.src_blend = kBlendOne;
        e.dst_blend = kBlendOne;
        e.z_write = false;
        v.push_back(e);
    }
    {   // PRIMALPHA t1
        RefEffect e;
        e.name = "PrimAlpha";
        e.vertex_color = true;
        e.stages = {stage(kModulate, kDiffuse, kTexture, kModulate, kDiffuse, kTexture)};
        e.blend = true;
        e.src_blend = kSrcAlpha;
        e.dst_blend = kInvSrcAlpha;
        e.z_write = false;
        v.push_back(e);
    }
    {   // PRIMADDITIVE t1
        RefEffect e;
        e.name = "PrimAdditive";
        e.vertex_color = true;
        e.stages = {stage(kModulate, kDiffuse, kTexture, kSelectArg1, kTexture, kCurrent)};
        e.blend = true;
        e.src_blend = kBlendOne;
        e.dst_blend = kBlendOne;
        e.z_write = false;
        v.push_back(e);
    }
    {   // PRIMDIFFUSEALPHA t0: LIGHTING, COLORVERTEX, DIFFUSEMATERIALSOURCE = COLOR1
        RefEffect e;
        e.name = "PrimDiffuseAlpha";
        e.vertex_color = true;
        e.lighting = true;
        e.color_vertex = true;
        e.diffuse_source = kMcsColor1;
        e.stages = {stage(kModulate2x, kTexture, kDiffuse, kModulate, kTexture, kDiffuse)};
        e.blend = true;
        e.src_blend = kSrcAlpha;
        e.dst_blend = kInvSrcAlpha;
        v.push_back(e);
    }
    {   // PRIMOPAQUE t0
        RefEffect e;
        e.name = "PrimOpaque";
        e.vertex_color = true;
        e.stages = {stage(kModulate, kDiffuse, kTexture, kSelectArg1, kTexture, kCurrent)};
        v.push_back(e);
    }
    {   // PRIMALPHASCANLINES t0: stage 1 ADD(TEXTURE, CURRENT), TCI[1] = CAMERASPACEPOSITION, COUNT2
        RefEffect e;
        e.name = "PrimAlphaScanlines";
        e.vertex_color = true;
        e.stages = {stage(kModulate, kDiffuse, kTexture, kModulate, kDiffuse, kTexture),
                    stage(kAdd, kTexture, kCurrent, kSelectArg1, kCurrent, kCurrent)};
        e.stages[1].camera_space_position = true;
        e.scanlines = true;
        e.blend = true;
        e.src_blend = kSrcAlpha;
        e.dst_blend = kInvSrcAlpha;
        e.z_write = false;
        v.push_back(e);
    }
    return v;
}

RefMaterial reference_material(const RefEffect& e, const RefParams& p) {
    RefMaterial m;
    if (e.name == "MeshAlpha" || e.name == "MeshAlphaScroll") {
        // MaterialAmbient = Diffuse.xyzw; MaterialDiffuse = m_lightScale.xyzw * Diffuse.xyzw;
        // MaterialEmissive.xyz = Emissive.xyz
        m.ambient = p.diffuse;
        for (int i = 0; i < 4; ++i) m.diffuse[static_cast<size_t>(i)] = p.light_scale[static_cast<size_t>(i)] * p.diffuse[static_cast<size_t>(i)];
        m.emissive = {p.emissive[0], p.emissive[1], p.emissive[2], 0};
    } else if (e.name == "PrimDiffuseAlpha") {
        // MaterialAmbient.xyz = Diffuse.xyz; MaterialDiffuse.xyz = Diffuse.xyz (overridden by COLOR1);
        // MaterialEmissive.xyz = Emissive.xyz
        m.ambient = {p.diffuse[0], p.diffuse[1], p.diffuse[2], 0};
        m.diffuse = {p.diffuse[0], p.diffuse[1], p.diffuse[2], 0};
        m.emissive = {p.emissive[0], p.emissive[1], p.emissive[2], 0};
    }
    return m;
}

void reference_texcoords(const RefEffect& e, const RefParams& p, const double uv0[2], const double world[3],
                         double out[2][2]) {
    out[0][0] = uv0[0];
    out[0][1] = uv0[1];
    out[1][0] = uv0[0];
    out[1][1] = uv0[1];
    if (e.scroll) {  // TEXTURE0[0]: expr0 = (1, 0, time * rate.x, 0), expr1 = (0, 1, time * rate.y, 0) on (u, v, 1, 0)
        out[0][0] = uv0[0] + p.time * p.uv_scroll[0];
        out[0][1] = uv0[1] + p.time * p.uv_scroll[1];
    }
    if (e.scanlines) {  // u = Scale * world.x, v = Scale * world.y + time * Speed (camera space x ViewInv)
        out[1][0] = p.scanline_scale * world[0];
        out[1][1] = p.scanline_scale * world[1] + p.time * p.scanline_speed;
    }
}

Vec4 reference_pixel(const RefEffect& e, const RefParams& p, const RefLights& l, const Vec4 tex[2], const Vec4& vc,
                     const double n[3], const Vec4& dst) {
    Vec4 src;
    if (e.shader_additive) {
        const double s = p.light_scale[3];
        const Vec4 c0 = {sat(p.light_scale[0] * p.color[0] * s), sat(p.light_scale[1] * p.color[1] * s),
                         sat(p.light_scale[2] * p.color[2] * s), 1.0};
        for (size_t i = 0; i < 4; ++i) src[i] = tex[0][i] * c0[i];
    } else {
        // Vertex colour stage (D3D9 lighting: Me + Ma * ambient + sum_i Md * Ld_i * max(N.L_i, 0)).
        const Vec4 vcolor = e.vertex_color ? vc : Vec4{1, 1, 1, 1};
        Vec4 diffuse = vcolor;
        if (e.lighting) {
            const RefMaterial m = reference_material(e, p);
            auto source = [&](int s, const Vec4& mat) {
                return (e.color_vertex && e.vertex_color && s == d9::kMcsColor1) ? vcolor : mat;
            };
            const Vec4 md = source(e.diffuse_source, m.diffuse), ma = source(e.ambient_source, m.ambient),
                       me = source(e.emissive_source, m.emissive);
            for (size_t c = 0; c < 3; ++c) {
                double v = me[c] + ma[c] * l.ambient[c];
                for (int li = 0; li < 3; ++li) {
                    const double ndl = n[0] * l.dir[li][0] + n[1] * l.dir[li][1] + n[2] * l.dir[li][2];
                    v += md[c] * l.diffuse[li][c] * std::max(ndl, 0.0);
                }
                diffuse[c] = sat(v);
            }
            diffuse[3] = sat(md[3]);
        }
        Vec4 cur = diffuse;
        for (size_t s = 0; s < e.stages.size(); ++s) {
            const RefStage& st = e.stages[s];
            if (st.color_op == d9::kDisable) break;
            auto arg = [&](int a) -> Vec4 {
                switch (a) {
                case d9::kDiffuse: return diffuse;
                case d9::kCurrent: return cur;
                case d9::kTexture: return tex[s];
                default: return Vec4{1, 1, 1, 1};
                }
            };
            const Vec4 c1 = arg(st.color_arg1), c2 = arg(st.color_arg2), a1 = arg(st.alpha_arg1), a2 = arg(st.alpha_arg2);
            Vec4 next;
            for (size_t c = 0; c < 3; ++c) next[c] = op(st.color_op, c1[c], c2[c], cur[c]);
            next[3] = st.alpha_op == d9::kDisable ? cur[3] : op(st.alpha_op, a1[3], a2[3], cur[3]);
            cur = sat4(next);
        }
        src = cur;
    }
    src = sat4(src);
    if (!e.blend) return src;
    Vec4 out;
    for (size_t c = 0; c < 4; ++c) {
        const double fs = blend_factor(e.src_blend, src[c], src[3], dst[c]);
        const double fd = blend_factor(e.dst_blend, src[c], src[3], dst[c]);
        out[c] = sat(src[c] * fs + dst[c] * fd);
    }
    return out;
}

}  // namespace alamo::reftest
