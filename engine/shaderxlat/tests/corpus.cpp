// Test inputs: synthetic asm programs and the real-install corpus (FXO -> bytecode through the VFS).
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>

#include "fx2_reader.hpp"
#include "mini_asm.hpp"
#include "test_common.hpp"

#ifdef ALAMO_SHADERXLAT_HAVE_VFS
#include "alamo/core/ascii.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/host_fs.hpp"
#include "alamo/meg/meg.h"
#include "alamo/vfs/vfs.hpp"
#endif

namespace alamo::shaderxlat::test {

namespace {

struct Synthetic {
    const char* name;
    const char* text;
    TextureDim dims[16];
};

// Each program is valid D3D9 asm for its profile (within the parser's limits) and exercises a
// family of instructions; together they cover every opcode / modifier the translator implements.
const Synthetic kSynthetic[] = {
    {"vs11_ops", R"(vs_1_1
def c90, 0.5, 2, -1, 0
dcl_position v0
dcl_normal v1
dcl_texcoord v2
dcl_color v3
m4x4 oPos, v0, c0
m3x3 r0.xyz, v1, c4
dp3 r0.w, r0, r0
rsq r0.w, r0.w
mul r0.xyz, r0, r0.w
lit r1, c8
dst r2, c9, c10
mad oD0, r1, c11, v3
mul oD1, r2, c90.x
expp r3, v2.x
logp r4, v2.y
exp r5.x, v2.z
log r5.y, v2.w
rcp r5.z, c15.x
rsq r5.w, v2.y
frc r6.xy, v2
mov oT0, r3
mov oT1, r4
mov oT2, r5
max r7, v0, c12
min r7, r7, c13
slt r8, v1, c14
sge r9, v1, c14
add oT3, r7, r8
sub oT4, r9, r6
m4x3 r11.xyz, v0, c16
m3x4 r10, v1, c20
mov oT5, r10
m3x2 r10.xy, v1, c24
mov r10.zw, -r0.xxxy
mov oT6, r10
mov oT7.xyz, r11
mov oFog.x, r1.y
mov a0.x, v1.w
mad oT7.w, c[a0.x + 30].w, v3.w, -c[a0.x + 31].x
)", {}},
    {"vs11_skin", R"(vs_1_1
def c95, 3, 0, 0, 0
dcl_position v0
dcl_normal v1
expp r1.y, v1.w
add r1.x, v1.w, -r1.y
mul r1.x, r1.x, c95.x
mov a0.x, r1.x
dp4 r0.x, v0, c[a0.x + 0]
dp4 r0.y, v0, c[a0.x + 1]
dp4 r0.z, v0, c[a0.x + 2]
mov r0.w, v0.w
m4x4 oPos, r0, c72
mov oD0, c[a0.x + 2]
dp3 oT0.x, v1, c[a0.x + 1]
)", {}},
    {"vs20_flow", R"(vs_2_0
defi i0, 3, 0, 0, 0
defi i1, 4, 2, 1, 0
defb b0, true
def c200, 0, 1, 0.25, -0.5
dcl_position v0
dcl_texcoord v1
dcl_blendindices v2
mov r0, c200.x
rep i0
add r0, r0, v1
endrep
rep i0
add r0.y, r0.y, c200.y
breakc_gt r0.y, c200.w
endrep
loop aL, i1
add r0, r0, c[aL + 10]
endloop
if b0
mul r0, r0, c200.z
else
mov r0, c200.y
endif
ifc_lt v1.x, c200.y
add r0.x, r0.x, c200.w
endif
mova a0.x, v2.x
mov r1, c[a0.x + 20]
call l1
m4x4 oPos, v0, c0
mov oT0, r0
mov oT1, r1
sincos r2.xy, v1.x, c4, c5
mov oT2, r2
ret
label l1
nrm r3, v1
crs r4.xyz, v0, v1
abs r5, v0
pow r6.x, v1.x, v1.y
mov oT3, r3
mov oT4, r4
mov oT5, r5
mov oT6.x, r6.x
lrp r7, c6, v0, v1
mov oT7, r7
mov oD0, c[a0.y + 40]
ret
)", {}},
    {"vs2x_pred", R"(vs_2_x
defi i0, 2, 0, 0, 0
dcl_position v0
dcl_texcoord v1
setp_gt p0, v1, c0
mov r0, c1
(p0) mov r0, v1
(!p0.y) add r0.y, r0.y, c2.y
sgn r1, v1, r2, r3
m4x4 oPos, v0, c4
rep i0
breakp p0.z
add r0.z, r0.z, c2.z
endrep
mov oT0, r0
mov oT1, r1
if p0.x
mov oT2, c3
endif
)", {}},
    {"vs20_intconst", R"(vs_2_0
dcl_position v0
mov r0, c1
rep i3
add r0, r0, c2
endrep
if b5
mov r0.x, c3.x
endif
if !b6
mov r0.y, c3.y
endif
mov oT0, r0
mov oPos, v0
)", {}},
    {"ps11_ops", R"(ps_1_1
def c7, 0.5, 0.25, 1, -1
tex t0
tex t1
texcoord t2
texcoord t3
mul r0, t0, v0
mad r1, t1_bx2, c0, v1_bias
dp3_sat r1.w, t2_bx2, t3_bx2
lrp r0.xyz, t0.w, r0, r1
+mul r0.w, 1-t1.w, c7.x
mul_x2 r1, r0, -c1
add_d2 r0, r0, r1
cnd r0, r0.w, t0, r1
sub_x4 r1, r0, c7
add_sat r0.xyz, r0, r1
+mad_d4 r0.w, r1.w, c7.y, r0.w
)", {}},
    {"ps11_coissue_dep", R"(ps_1_1
tex t0
mov r0, t0
mul r0.xyz, r0, c0
+add r0.w, r0.z, c1.w
mul r1.xyz, r0.w, v0
+mul r1.w, r0.z, -c2_bx2.z
mov r0, r1
)", {}},
    {"ps11_texm3x2", R"(ps_1_1
tex t0
texm3x2pad t1, t0_bx2
texm3x2tex t2, t0_bx2
texkill t3
mov r0, t2
mul r0.xyz, r0, t0
)", {}},
    {"ps11_texbem", R"(ps_1_1
tex t0
texbem t1, t0
texbeml t2, t0
texreg2ar t3, t0
add r0, t1, t2
mul r0, r0, t3
)", {}},
    {"ps13_texm3x3spec", R"(ps_1_3
tex t0
texm3x3pad t1, t0_bx2
texm3x3pad t2, t0_bx2
texm3x3spec t3, t0_bx2, c0
mov r0, t3
)", {TextureDim::Unknown, TextureDim::Unknown, TextureDim::Unknown, TextureDim::Cube}},
    {"ps13_texm3x3vspec", R"(ps_1_3
tex t0
texm3x3pad t1, t0_bx2
texm3x3pad t2, t0_bx2
texm3x3vspec t3, t0_bx2
mul r0, t3, v0
)", {TextureDim::Unknown, TextureDim::Unknown, TextureDim::Unknown, TextureDim::Cube}},
    {"ps13_texm3x3tex", R"(ps_1_3
tex t0
texm3x3pad t1, t0
texm3x3pad t2, t0
texm3x3tex t3, t0
mul r0, t3, v1
)", {TextureDim::Unknown, TextureDim::Unknown, TextureDim::Unknown, TextureDim::Cube}},
    {"ps13_texm3x3", R"(ps_1_3
tex t0
texm3x3pad t1, t0_bx2
texm3x3pad t2, t0_bx2
texm3x3 t3, t0_bx2
mov r0, t3
)", {}},
    {"ps13_texdp3", R"(ps_1_3
tex t0
texdp3tex t1, t0
texdp3 t2, t0
texreg2gb t3, t1
mul r0, t1, t2
add r0, r0, t3
)", {}},
    {"ps13_texreg2rgb", R"(ps_1_3
tex t0
texreg2rgb t1, t0
cmp r0, t0_bias, t1, v0
)", {}},
    {"ps13_depth", R"(ps_1_3
tex t0
tex t1
texm3x2pad t2, t1
texm3x2depth t3, t1
mul r0, v0, t0
)", {}},
    {"ps14_phase", R"(ps_1_4
def c6, 0.5, 0.5, 0.5, 1
texld r0, t0
texcrd r1.xyz, t1
texcrd r2.xy, t2_dw
texld r3, t3_dz
bem r4.xy, r1, r0
dp3_x2 r5.x, r0_bx2, r1
mul r5.y, r0.w, c6.x
phase
texld r4, r4
texdepth r5
texkill r1
mul r0, r3, v0
cnd r1, r0, r4, c6
cmp r2, r1_bias, r0, r4
mad_x8 r0.xyz, r1, r2, c6_bx2
+mov r0.w, v1.w
)", {}},
    {"ps20_ops", R"(ps_2_0
def c31, 0.5, 2, -1, 0
dcl v0
dcl v1
dcl t0.xy
dcl t1
dcl t2.xyz
dcl_2d s0
dcl_cube s1
dcl_2d s2
texld r0, t0, s0
texldp r1, t1, s0
texldb r2, t1, s2
texld r3, t2, s1
dp2add r4.x, r0, r1, c31.x
pow r4.y, r0.x, c31.y
exp r4.z, c0.x
log r4.w, c0.y
nrm r5.xyz, t2
crs r6.xyz, r5, t1
rcp r7.x, t1.w
rsq r7.y, t1.x
sincos r7.zw, t0.x, c2, c3
cmp r8, r0_bias, r1, r2
lrp r9, c1, r2, r3
abs r10, t1
frc r11, t1
texkill t2
add r0, r0, r1
mad r0, r2, r3, r0
mul r0, r0, v0
add r0, r0, r4
add r0, r0, r5
add r0, r0, r6
min r1, r7, r8
max r1, r1, r9
add r1, r1, r10
mad_sat r1, r11, c4, r1
mad r0, r1, v1, r0
mov oC0, r0
mov oDepth, r1.x
)", {}},
    {"ps2x_flow", R"(ps_2_x
defb b0, true
defi i0, 3, 0, 0, 0
def c31, 0.25, 0, 1, 0
dcl t0.xy
dcl_2d s0
texld r0, t0, s0
dsx r1, t0
dsy r2, t0
texldd r3, t0, s0, r1, r2
rep i0
add r0, r0, c31.x
endrep
if b0
mul r0, r0, c31.z
endif
setp_gt p0, r0, c0
(p0) mov r0, r3
mov oC0, r0
)", {}},
    {"ps20_mrt", R"(ps_2_0
dcl t0
mov oC0, t0
mov oC1, c0
)", {}},
    // Shader model 3 (modc t22): semantic outputs / inputs, flow control, predication, a0 / aL addressing,
    // texldl, vPos / vFace, def constants above c31.
    {"vs30_io", R"(vs_3_0
defi i0, 3, 0, 0, 0
defi i1, 4, 2, 1, 0
defb b0, true
def c200, 0, 1, 0.25, -0.5
dcl_position v0
dcl_normal v1
dcl_texcoord v2
dcl_blendindices v3
dcl_position o0
dcl_color o1
dcl_color1 o2
dcl_texcoord o3.xy
dcl_texcoord1 o4
dcl_texcoord5 o5.xyz
dcl_fog o6.x
dcl_psize o7.x
m4x4 o0, v0, c0
mov r0, c200.x
rep i0
add r0, r0, v2
endrep
loop aL, i1
add r0, r0, c[aL + 10]
breakc_gt r0.x, c200.y
endloop
if b0
mul r0, r0, c200.z
else
mov r0, c200.y
endif
setp_lt p0, v1, c200.yyyy
(p0) mov r0.xy, v1
(!p0.y) add r0.z, r0.z, c200.w
mova a0.x, v3.x
mov r1, c[a0.x + 20]
sincos r2.xy, v2.x
mad o1, r0, c4, r1
mul o2, v1, c5
mov o3.xy, r2
mov o4, r0
mov o5.xyz, r1
add o6.x, r0.w, c200.z
mov o7.x, c200.y
)", {}},
    {"ps30_io", R"(ps_3_0
defi i0, 3, 0, 0, 0
defi i1, 2, 4, 1, 0
def c40, 0.5, -0.25, 2, 1
def c20, 0.125, 0.75, 0, 1
dcl_texcoord v0.xy
dcl_texcoord1_centroid v1
dcl_color v2
dcl_color1 v3
dcl_fog v4.x
dcl_texcoord7 v5
dcl vPos.xy
dcl vFace
dcl_2d s0
dcl_cube s1
dcl_2d s2
texld r0, v0, s0
texldl r1, v5, s2
texldb r2, v1, s0
texldp r3, v1, s2
texld r4, v1, s1
add r0, r0, r1
mov r5, c40.x
rep i0
add r5, r5, c20.x
breakc_gt r5.x, c40.w
endrep
loop aL, i1
add r5, r5, c[aL + 0]
endloop
setp_ge p0, v2, c40.yyyy
(p0) mov r0.xz, v3
ifc_lt vFace.x, c20.z
mov r0.w, c40.y
else
add r0.w, r0.w, c40.x
endif
frc r6.xy, vPos
mad r6.z, vPos.x, c20.x, c20.y
dsx r7, v0
dsy r8, v0
add r6.w, r7.x, r8.y
texkill r6
mad r0, r0, v2, r2
mad r0, r3, c1, r0
add r0, r0, r4
mad r0.xyz, r6, c2, r0
mul r0.xyz, r0, v4.x
add r0.w, r0.w, c40.z
mov oC0, r0
mov oDepth, r5.x
)", {}},
};

}  // namespace

std::vector<Blob> synthetic_blobs() {
    std::vector<Blob> out;
    for (const Synthetic& s : kSynthetic) {
        std::vector<uint32_t> tok;
        std::string err;
        if (!assemble(s.text, tok, &err)) {
            std::printf("synthetic %s: assembler error: %s\n", s.name, err.c_str());
            ++g_fail;
            continue;
        }
        Blob b;
        b.name = s.name;
        b.origin = "synthetic";
        b.bytes = to_bytes(tok);
        for (size_t i = 0; i < 16; ++i) b.options.sampler_dims[i] = s.dims[i];
        out.push_back(std::move(b));
    }
    return out;
}

// ---------------------------------------------------------------- SHA-1 (FIPS 180-4), test-only
std::string sha1_hex(const std::vector<uint8_t>& data) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    std::vector<uint8_t> m = data;
    const uint64_t bits = static_cast<uint64_t>(data.size()) * 8u;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 7; i >= 0; --i) m.push_back(static_cast<uint8_t>(bits >> (8 * i)));
    auto rol = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (static_cast<uint32_t>(m[off + 4 * static_cast<size_t>(i)]) << 24) | (static_cast<uint32_t>(m[off + 4 * static_cast<size_t>(i) + 1]) << 16) |
                   (static_cast<uint32_t>(m[off + 4 * static_cast<size_t>(i) + 2]) << 8) | m[off + 4 * static_cast<size_t>(i) + 3];
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%08x%08x%08x%08x%08x", h[0], h[1], h[2], h[3], h[4]);
    return buf;
}

const std::vector<HardCase>& vkd3d_hard_cases() {
    static const std::vector<HardCase> k = [] {
        std::vector<HardCase> v;
        const char* expp[] = {"059647860988", "1176a655f235", "15f27f6f3968", "1732194e176f", "25470fce6948", "29931c4a893b",
                              "370b56538d74", "3ad055eb09b9", "431d7e4919b7", "4632f085050f", "4ddf9cd9d030", "6646b4dd3f17",
                              "6dd7de952252", "753f913cfc29", "81630d128c2b", "82e0cc594ff9", "93d9e1fd6b52", "977cddeb2c47",
                              "9796d1ded976", "9b1d915925b2", "9d06b7402a74", "9ea57392d1f5", "a00a26019c9c", "aaf7237809b4",
                              "ae5115872a27", "b53795024903", "bb182a407d80", "cea5f1ef0810", "d104e9ade8e0", "d9e8af203a14",
                              "db0cb3a804e8", "e58ca524fbcf", "e6f7f4c09268", "f2b7f6a00b3a", "f79625176039"};
        for (const char* s : expp) v.push_back({s, "vs_1_1 expp"});
        for (const char* s : {"11cc917522ed", "d2b74c9d9903"}) v.push_back({s, "ps_1_x cnd"});
        for (const char* s : {"1394311143c3", "241298c1d72c", "cb81c0843364", "ea11e00ac1f1"}) v.push_back({s, "register form (oDepth)"});
        const char* bx2[] = {"1cd87769b176", "26673e06d10f", "2be18ecefe31", "4c3b044d7d8b", "4edaeb02fb39", "506cc0e09b22",
                             "5670cf5e52e4", "57cf53587894", "5aac2ce6057e", "5c4e64efa8f3", "9052b6ef7b2e", "a26ddc744235",
                             "b72e5934dbc2", "ecfd8a1eb8c9", "f476b6c68261", "fb37f533e05c"};
        for (const char* s : bx2) v.push_back({s, "ps_1_x _bx2"});
        for (const char* s : {"2421f271a5ef", "4b76c082c386"}) v.push_back({s, "texdepth"});
        for (const char* s : {"9afae340849e", "c154786be79c"}) v.push_back({s, "texm3x2pad"});
        return v;
    }();
    return k;
}

// ---------------------------------------------------------------- real install
namespace {
using ::alamo::test::env_or_empty;

#ifdef ALAMO_SHADERXLAT_HAVE_VFS
bool is_shader_object(const std::vector<uint8_t>& d) {
    if (d.size() < 8) return false;
    uint32_t v;
    std::memcpy(&v, d.data(), 4);
    const uint32_t kind = v >> 16;
    return (kind == 0xFFFEu || kind == 0xFFFFu) && ((v >> 8) & 0xFFu) <= 3;
}

void collect_game(const std::string& root, const char* game, std::map<std::string, Blob>& out, std::string& note) {
    vfs::VfsConfig cfg;
    cfg.foc_root = root;
    cfg.eaw_root = std::string();
    std::string err;
    auto v = vfs::Vfs::create(cfg, &err);
    if (!v) {
        note += std::string(game) + ": VFS failed (" + err + "); ";
        return;
    }
    std::set<std::string> files;
    for (const char* g : {"DATA\\ART\\SHADERS\\*.FXO", "DATA\\ART\\SHADERS\\**\\*.FXO"})
        for (const std::string& f : v->list(g)) files.insert(f);
    // Effective files (what the VFS resolves) first, then every copy in every mounted MEG, so the
    // pre-patch versions that Patch*.meg shadow are part of the corpus too (the catalog's 291).
    std::vector<std::pair<std::string, std::vector<uint8_t>>> sources;
    for (const std::string& f : files) {
        std::vector<uint8_t> bytes;
        if (v->read(f, bytes)) sources.emplace_back(f, std::move(bytes));
    }
    size_t shadowed = 0;
    for (const auto& m : v->mounts()) {
        auto a = meg::Archive::open(m.path);
        if (!a) continue;
        for (const meg::Entry& en : a->entries()) {
            const std::string n = meg::normalize_name(en.name);
            if (n.rfind("DATA\\ART\\SHADERS\\", 0) != 0 || n.size() < 4 || n.compare(n.size() - 4, 4, ".FXO") != 0) continue;
            std::vector<uint8_t> bytes;
            if (!a->read(en, bytes)) continue;
            sources.emplace_back(n, std::move(bytes));
            ++shadowed;
        }
    }
    size_t fxo = 0;
    for (auto& src : sources) {
        const std::string& f = src.first;
        std::vector<uint8_t>& bytes = src.second;
        fx::Effect e;
        try {
            e = fx::parseEffect(bytes.data(), bytes.size());
        } catch (const std::exception&) {
            continue;
        }
        ++fxo;
        std::string effect = f.substr(f.find_last_of('\\') + 1);
        effect = effect.substr(0, effect.find('.'));
        for (const auto& kv : e.objects) {
            if (!is_shader_object(kv.second.data)) continue;
            std::vector<uint8_t> blob = kv.second.data;
            // Objects are padded to 4 bytes; the bytecode ends at END. Keep everything up to END.
            Program p;
            if (parse(blob, p, nullptr)) blob.resize(static_cast<size_t>(p.token_count) * 4);
            const std::string h = sha1_hex(blob).substr(0, 12);
            if (out.count(h)) continue;
            Blob b;
            b.name = h;
            b.origin = std::string(game) + "/" + effect;
            b.bytes = std::move(blob);
            out.emplace(h, std::move(b));
        }
        for (const auto& t : e.techniques)
            for (const auto& pass : t.passes)
                for (const auto& s : pass.states) {
                    if (s.kind != fx::State::Kind::Shader || s.blob.empty() || !is_shader_object(s.blob)) continue;
                    std::vector<uint8_t> blob = s.blob;
                    Program p;
                    if (parse(blob, p, nullptr)) blob.resize(static_cast<size_t>(p.token_count) * 4);
                    const std::string h = sha1_hex(blob).substr(0, 12);
                    if (out.count(h)) continue;
                    Blob b;
                    b.name = h;
                    b.origin = std::string(game) + "/" + effect;
                    b.bytes = std::move(blob);
                    out.emplace(h, std::move(b));
                }
    }
    note += std::string(game) + ": " + std::to_string(files.size()) + " effective FXOs + " + std::to_string(shadowed) +
            " MEG copies (" + std::to_string(fxo) + " parsed); ";
}

// Shader model 3 blobs of every workshop item under `root` (loose Data\Art\Shaders\**.fxo and the same paths in
// the item's MEG files). Only SM3 is taken: the SM1-2 programs mods ship are recompiles of the base corpus.
void collect_mod_sm3(const std::string& root, std::map<std::string, Blob>& out, std::string& note) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path base = alamo::path_from_utf8(root);
    if (!fs::is_directory(base, ec)) return;
    std::vector<fs::path> items;
    for (const auto& d : fs::directory_iterator(base, ec))
        if (d.is_directory(ec)) items.push_back(d.path());
    std::sort(items.begin(), items.end());
    size_t added = 0;
    for (const fs::path& item : items) {
        const std::string id = item.filename().string();
        const fs::path data = item / "Data";
        if (!fs::is_directory(data, ec)) continue;
        std::vector<fs::path> files;
        for (auto it = fs::recursive_directory_iterator(data, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            if (it->is_regular_file(ec)) files.push_back(it->path());
        std::sort(files.begin(), files.end());
        std::vector<std::pair<std::string, std::vector<uint8_t>>> sources;
        for (const fs::path& f : files) {
            const std::string ext = alamo::ascii_upper(f.extension().string());
            if (ext == ".FXO") {
                std::vector<uint8_t> bytes;
                if (alamo::read_host_file(f, bytes)) sources.emplace_back(f.stem().string(), std::move(bytes));
            } else if (ext == ".MEG") {
                auto a = meg::Archive::open(f.string());
                if (!a) continue;
                for (const meg::Entry& en : a->entries()) {
                    const std::string nn = meg::normalize_name(en.name);
                    if (nn.rfind("DATA\\ART\\SHADERS\\", 0) != 0 || nn.size() < 4 || nn.compare(nn.size() - 4, 4, ".FXO") != 0) continue;
                    std::vector<uint8_t> bytes;
                    if (!a->read(en, bytes)) continue;
                    const std::string e = nn.substr(nn.find_last_of('\\') + 1);
                    sources.emplace_back(e.substr(0, e.find('.')), std::move(bytes));
                }
            }
        }
        for (auto& src : sources) {
            fx::Effect e;
            try {
                e = fx::parseEffect(src.second.data(), src.second.size());
            } catch (const std::exception&) {
                continue;
            }
            auto add = [&](const std::vector<uint8_t>& raw) {
                if (!is_shader_object(raw)) return;
                std::vector<uint8_t> blob = raw;
                Program p;
                if (!parse(blob, p, nullptr) || p.major < 3) return;
                blob.resize(static_cast<size_t>(p.token_count) * 4);
                const std::string h = sha1_hex(blob).substr(0, 12);
                if (out.count(h)) return;
                Blob b;
                b.name = h;
                b.origin = "mod" + id + "/" + src.first;
                b.bytes = std::move(blob);
                out.emplace(h, std::move(b));
                ++added;
            };
            for (const auto& kv : e.objects) add(kv.second.data);
            for (const auto& t : e.techniques)
                for (const auto& pass : t.passes)
                    for (const auto& st : pass.states)
                        if (st.kind == fx::State::Kind::Shader) add(st.blob);
        }
    }
    note += "workshop " + root + ": " + std::to_string(items.size()) + " items, " + std::to_string(added) + " SM3 blobs; ";
}
#endif
}  // namespace

std::vector<Blob> corpus_blobs(std::string* note) {
    std::vector<Blob> out;
    std::string n;
#ifdef ALAMO_SHADERXLAT_HAVE_VFS
    const std::string foc = env_or_empty("ALAMO_FOC_DATA"), eaw = env_or_empty("ALAMO_EAW_DATA");
    std::map<std::string, Blob> m;
    if (!foc.empty()) collect_game(foc, "foc", m, n);
    if (!eaw.empty()) collect_game(eaw, "eaw", m, n);
    // modc t22: the shader model 3 programs installed workshop mods ship (ALAMO_WORKSHOP_DIR, else the
    // workshop folder of the Steam library ALAMO_FOC_DATA is in).
    std::string ws = env_or_empty("ALAMO_WORKSHOP_DIR");
    const size_t at = foc.find("\\steamapps\\common\\");
    if (ws.empty() && at != std::string::npos) ws = foc.substr(0, at) + "\\steamapps\\workshop\\content\\32470";
    if (!ws.empty()) collect_mod_sm3(ws, m, n);
    if (foc.empty() && eaw.empty()) n = "ALAMO_FOC_DATA / ALAMO_EAW_DATA not set";
    for (auto& kv : m) out.push_back(std::move(kv.second));
#else
    n = "built without alamo_vfs";
#endif
    if (note) *note = n;
    return out;
}

}  // namespace alamo::shaderxlat::test
