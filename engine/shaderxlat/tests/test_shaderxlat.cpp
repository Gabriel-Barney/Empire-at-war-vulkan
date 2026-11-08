// alamo_shaderxlat_tests
//   cpu : parser unit tests (per opcode / modifier, malformed input), disassembler round trip,
//         emitter golden text + interface checks, fuzz loop, determinism (1 and 8 threads), disk
//         cache round trip (incl. concurrent writers), runtime DXC compile, and - when
//         ALAMO_FOC_DATA / ALAMO_EAW_DATA are set - the whole real corpus (parse, translate, DXC).
//   gpu : differential test of the SPIR-V against the CPU interpreter (gpu_diff.cpp).
// Options: --write-golden (regenerate tests/golden/*.hlsl), --dump <dir> (write corpus HLSL).
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/shaderxlat/interpreter.hpp"
#include "alamo/shaderxlat/shaderxlat.hpp"
#include "mini_asm.hpp"
#include "test_common.hpp"

namespace alamo::shaderxlat::test {

namespace {

bool g_write_golden = false;
std::string g_dump_dir;
std::string g_exe;   // argv[0], for the multi-process cache test

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    uint32_t below(uint32_t n) { return n ? static_cast<uint32_t>(next() % n) : 0u; }
    float uni(float lo, float hi) { return lo + (hi - lo) * static_cast<float>(next() >> 40) / static_cast<float>(1ull << 24); }
};

Program asm_parse(const char* text, bool expect_ok = true) {
    std::vector<uint32_t> tok;
    std::string err;
    Program p;
    if (!assemble(text, tok, &err)) {
        std::printf("  assembler: %s\n", err.c_str());
        CHECK(false);
        return p;
    }
    const bool ok = parse(to_bytes(tok), p, &err);
    if (ok != expect_ok) std::printf("  parse %s: %s\n", ok ? "succeeded unexpectedly" : "failed", err.c_str());
    CHECK(ok == expect_ok);
    return p;
}

std::vector<uint32_t> asm_tokens(const char* text) {
    std::vector<uint32_t> tok;
    std::string err;
    if (!assemble(text, tok, &err)) {
        std::printf("  assembler: %s\n", err.c_str());
        CHECK(false);
    }
    return tok;
}

bool parse_tokens(const std::vector<uint32_t>& t, std::string* err = nullptr) {
    Program p;
    return parse(to_bytes(t), p, err);
}

// ---------------------------------------------------------------- parser
// A key without the source hash (source_hash 0): cache.hpp's CacheKey::make always keys on the HLSL text, so the
// concurrency / file-name tests build theirs from the blob fields directly (cheap, no translation needed).
CacheKey plain_key(std::span<const uint8_t> bytecode, const TranslateOptions& options, std::string_view compiler) {
    CacheKey k;
    k.blob_fnv = alamo::fnv1a64(bytecode.data(), bytecode.size());
    k.blob_crc = alamo::crc32(bytecode.data(), bytecode.size());
    k.blob_size = static_cast<uint32_t>(bytecode.size());
    k.options_key = options.key();
    k.translator_version = kTranslatorVersion;
    k.compiler = std::string(compiler);
    return k;
}

void test_parser_units() {
    std::printf("parser unit tests\n");
    {
        const Program p = asm_parse("ps_1_1\ntex t0\nmul_x2_sat r0.xyz, t0_bx2, -c1.w\n+mov r0.w, 1-v0\n");
        CHECK(p.ps1() && p.major == 1 && p.minor == 1 && p.instrs.size() == 3);
        if (p.instrs.size() == 3) {
            const Instr& t = p.instrs[0];
            CHECK(t.op == Op::Tex && t.has_dst && t.dst.type == reg::kTexture && t.dst.index == 0 && t.nsrc == 0);
            const Instr& m = p.instrs[1];
            CHECK(m.op == Op::Mul && m.dst.type == reg::kTemp && m.dst.mask == 7 && m.dst.shift == 1 && m.dst.saturate);
            CHECK(m.nsrc == 2 && m.src[0].type == reg::kTexture && m.src[0].mod == SrcMod::Sign);
            CHECK(m.src[1].type == reg::kConst && m.src[1].index == 1 && m.src[1].mod == SrcMod::Neg);
            CHECK(m.src[1].swz[0] == 3 && m.src[1].swz[1] == 3 && m.src[1].swz[2] == 3 && m.src[1].swz[3] == 3);
            const Instr& c = p.instrs[2];
            CHECK(c.coissue && c.op == Op::Mov && c.dst.mask == 8 && c.src[0].mod == SrcMod::Comp && c.src[0].type == reg::kInput);
        }
    }
    {
        const Program p = asm_parse("vs_1_1\ndcl_position v0\ndcl_texcoord3 v4\nmov r0, c[a0.x + 5]\nmov oPos, v0\n");
        CHECK(p.instrs.size() == 4);
        if (p.instrs.size() == 4) {
            CHECK(p.instrs[1].op == Op::Dcl && p.instrs[1].usage == Usage::Texcoord && p.instrs[1].usage_index == 3 && p.instrs[1].dst.index == 4);
            const Src& s = p.instrs[2].src[0];
            CHECK(s.relative && s.rel_type == reg::kAddr && s.rel_comp == 0 && s.index == 5 && s.type == reg::kConst);
        }
    }
    {
        const Program p = asm_parse("vs_2_0\ndcl_position v0\nmova a0.y, v0.x\nmov r0, c[a0.y + 7]\nmov oPos, r0\n");
        CHECK(p.instrs.size() == 4);
        if (p.instrs.size() == 4) {
            const Src& s = p.instrs[2].src[0];
            CHECK(s.relative && s.rel_comp == 1 && s.index == 7);
            CHECK(p.instrs[1].op == Op::Mova && p.instrs[1].dst.type == reg::kAddr && p.instrs[1].dst.mask == 2);
        }
    }
    {
        const Program p = asm_parse("ps_2_0\ndef c3, 1.5, -2, 0.25, 0\ndcl_centroid t0.xy\ndcl_cube s2\ntexldp r0, t0, s2\ntexldb_pp r1, t0, s2\nmul_d4 r0, r0, r1\nmov oC0, r0\n");
        CHECK(p.instrs.size() == 7);
        if (p.instrs.size() == 7) {
            float f[4];
            std::memcpy(f, p.instrs[0].literal.data(), 16);
            CHECK(p.instrs[0].op == Op::Def && f[0] == 1.5f && f[1] == -2.0f && f[2] == 0.25f && f[3] == 0.0f);
            CHECK(p.instrs[1].dst.centroid && p.instrs[1].dst.mask == 3 && p.instrs[1].dst.type == reg::kTexture);
            CHECK(p.instrs[2].tex_dim == TextureDim::Cube && p.instrs[2].dst.index == 2);
            CHECK(p.instrs[3].op == Op::Tex && (p.instrs[3].specific & kTexldProject) && p.instrs[3].nsrc == 2);
            CHECK((p.instrs[4].specific & kTexldBias) && p.instrs[4].dst.partial_precision);
            CHECK(p.instrs[5].dst.shift == -2);
        }
    }
    {
        const Program p = asm_parse("vs_2_x\ndefi i0, 3, -1, 2, 0\ndefb b1, true\ndcl_position v0\nifc_le v0.x, v0.y\nsetp_ne p0.xy, v0, c0\nendif\n(!p0.y) add r0.y, v0, c1\nmov oPos, r0\n");
        CHECK(p.instrs.size() == 8);
        if (p.instrs.size() == 8) {
            CHECK(p.instrs[0].op == Op::Defi && static_cast<int32_t>(p.instrs[0].literal[1]) == -1);
            CHECK(p.instrs[1].op == Op::Defb && p.instrs[1].literal[0] == 1 && p.instrs[1].dst.index == 1);
            CHECK(p.instrs[3].op == Op::Ifc && p.instrs[3].specific == 6);
            CHECK(p.instrs[4].op == Op::Setp && p.instrs[4].specific == 5 && p.instrs[4].dst.type == reg::kPredicate);
            const Instr& a = p.instrs[6];
            CHECK(a.predicated && a.pred.type == reg::kPredicate && a.pred.mod == SrcMod::Not && a.pred.swz[0] == 1);
        }
    }
    {
        const Program p = asm_parse("ps_1_4\ntexld r0, t0\ntexcrd r1.xy, t1_dz\nphase\ntexld r2, r1\nmul r0, r0, r2\n");
        CHECK(p.instrs.size() == 5);
        if (p.instrs.size() == 5) {
            CHECK(p.instrs[1].op == Op::TexCoord && p.instrs[1].src[0].mod == SrcMod::Dz && p.instrs[1].nsrc == 1);
            CHECK(p.instrs[2].op == Op::Phase);
            CHECK(p.instrs[3].src[0].type == reg::kTemp);
        }
    }
    // Every synthetic program: disassemble -> assemble -> identical tokens.
    for (const Blob& b : synthetic_blobs()) {
        Program p;
        std::string err;
        const bool ok = parse(b.bytes, p, &err);
        if (!ok) std::printf("  %s: %s\n", b.name.c_str(), err.c_str());
        CHECK(ok);
        std::vector<uint32_t> tok;
        CHECK(assemble(disassemble(p), tok, &err));
        CHECK(to_bytes(tok) == b.bytes);
    }
}

void test_parser_malformed() {
    std::printf("parser malformed-input tests\n");
    std::string err;
    Program p;
    CHECK(!parse({}, p, &err));
    const uint8_t junk[] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(!parse(junk, p, &err));
    const std::vector<uint32_t> good = asm_tokens("vs_2_0\ndcl_position v0\nmov oPos, v0\n");
    CHECK(parse_tokens(good));
    {
        auto t = good;
        t.pop_back();  // no END
        CHECK(!parse_tokens(t, &err));
    }
    {
        auto t = good;
        t.resize(t.size() - 2);  // cut inside "mov"
        CHECK(!parse_tokens(t, &err));
    }
    {
        auto t = good;
        t[0] = 0xFFFE0400u;  // vs_4_0
        CHECK(!parse_tokens(t, &err));
    }
    {
        auto t = good;
        t.insert(t.begin() + 1, 0x0010FFFEu);  // comment claiming 16 DWORDs
        t.resize(8);
        CHECK(!parse_tokens(t, &err));
    }
    {
        auto t = good;
        t[t.size() - 4] = (t[t.size() - 4] & 0xF0FFFFFFu) | (3u << 24);  // wrong SM2 instruction length
        CHECK(!parse_tokens(t, &err));
    }
    {
        auto t = good;
        t[t.size() - 2] &= 0x7FFFFFFFu;  // operand without bit 31
        CHECK(!parse_tokens(t, &err));
    }
    CHECK(!parse_tokens(asm_tokens("vs_1_1\nmov r0, c300\n"), &err));        // const out of range
    CHECK(!parse_tokens(asm_tokens("ps_2_0\nmov r0, c40\n"), &err));         // ps const out of range
    CHECK(!parse_tokens(asm_tokens("ps_1_1\nmov r0, t5\n"), &err));          // ps_1_1 has t0..t3
    CHECK(!parse_tokens(asm_tokens("ps_1_4\nmov t0, r0\n"), &err));          // t# read-only in 1.4
    CHECK(!parse_tokens(asm_tokens("vs_1_1\nmov r0, oPos\n"), &err));        // output not readable
    CHECK(!parse_tokens(asm_tokens("vs_1_1\nmov oPos, r0\n+mov r1, r0\n"), &err));  // co-issue in a VS
    CHECK(!parse_tokens(asm_tokens("ps_2_0\nmov r0, c[aL + 1]\n"), &err));   // ps relative (loop reg)
    {
        auto t = asm_tokens("vs_2_0\nmov oPos, c0\n");
        t[1] = (t[1] & 0xFFFF0000u) | 75u;  // reserved opcode
        CHECK(!parse_tokens(t, &err));
        t[1] = (t[1] & 0xFFFF0000u) | 200u;
        CHECK(!parse_tokens(t, &err));
    }
    // Translator-level rejections.
    auto tr = [&](const char* text) {
        Program q = asm_parse(text);
        Translation t;
        return translate(q, TranslateOptions{}, t, &err);
    };
    // Shader model 3 (modc t22): what the translator does not cover is rejected with a message.
    CHECK(tr("vs_3_0\ndcl_position v0\ndcl_position o0\nmov o0, v0\n"));
    CHECK(!tr("vs_3_0\ndcl_position v0\ndcl_position o0\nmov o1, v0\n"));                  // o1 without dcl
    CHECK(!tr("vs_3_0\ndcl_position v0\ndcl_normal o1\nmov o1, v0\n"));                    // no varying slot
    CHECK(!tr("vs_3_0\ndcl_position v0\ndcl_texcoord o0\ndcl_texcoord o1\nmov o0, v0\n"));  // same usage twice
    CHECK(!tr("vs_3_0\ndcl_texcoord v0\ndcl_2d s0\ndcl_position o0\ntexldl r0, v0, s0\nmov o0, r0\n"));  // vertex texture fetch
    CHECK(!tr("ps_3_0\nmov oC0, c40\n"));                                                    // c40 not in PSConstants
    CHECK(tr("ps_3_0\ndef c40, 1, 0, 0, 1\nmov oC0, c40\n"));                                 // def literal is fine
    CHECK(!tr("ps_3_0\nmov oC0, v0\n"));                                                     // v0 without dcl
    CHECK(!tr("ps_3_0\ndcl_normal v0\nmov oC0, v0\n"));                                      // no varying slot
    {
        std::string e2;
        CHECK(!parse_tokens(asm_tokens("ps_3_0\nmov oC0, t0\n"), &e2));  // no t# in ps_3_0
        CHECK(!parse_tokens(asm_tokens("ps_2_0\nmov oC0, vPos\n"), &e2));  // vPos is ps_3_0 only
        CHECK(parse_tokens(asm_tokens("ps_3_0\nmov oC0, c200\n"), &e2));   // ps_3_0 parses c0..c223
        CHECK(!parse_tokens(asm_tokens("ps_3_0\nmov oC0, c224\n"), &e2));
    }
    CHECK(!tr("vs_2_0\ndefb b0, true\ndcl_position v0\nif b0\nmov oPos, v0\n"));  // unterminated if
    CHECK(!tr("vs_2_0\nbreak\n"));
    CHECK(!tr("vs_2_0\ncall l3\n"));
    CHECK(!tr("vs_1_1\nmov oPos, v0\n"));  // v0 without dcl
    CHECK(!tr("ps_1_1\ntexm3x2tex t1, t0\n"));  // no pad
}

// Regressions for the 2026-10-27 review: out-of-range stage / register indices, bump_env, escaped
// sampler names, source-keyed cache.
void test_review_regressions() {
    std::printf("review regressions (operand kinds, bump_env, sampler names, cache key)\n");
    std::string err;
    // (1) ps_1_0..1_3 texture ops must name t# (the stage); r# / out-of-range indices are rejected.
    for (const char* bad : {"ps_1_1\ntexcoord r31\n", "ps_1_1\ntexkill r31\n", "ps_1_1\ntexm3x2pad r31, t0\n",
                            "ps_1_1\ntexcoord r1\n", "ps_1_1\ntexkill r0\n", "ps_1_1\ntex r1\n", "ps_1_1\ntexm3x2pad r1, t0\n",
                            "ps_1_3\ntexbem t1, r0\n", "ps_1_3\ntexbem t1, t2\n", "ps_1_3\ntexm3x3spec t3, t0, r0\n",
                            "ps_1_4\ntexbem r1, t0\n", "ps_1_4\ntexcrd r31, t0\n", "ps_1_4\ntexld t0, t0\n",
                            "ps_1_4\ntexkill r6\n", "ps_1_4\ntexdepth r4\n", "ps_2_0\ntexcoord r0\n", "ps_2_0\ntexbem r1, t0\n",
                            "ps_2_0\nbem r0.xy, r1, r2\n", "ps_1_1\nmov r2, v0\n", "ps_1_4\nmov r6, v0\n", "ps_1_1\nmov r0, c8\n",
                            "ps_1_1\nmov r0, c16\n", "vs_2_0\nrep c200\nendrep\n", "vs_2_0\nloop aL, c3\nendloop\n",
                            "vs_2_0\ncall c1\n", "vs_2_0\nif c0\nendif\n"}) {
        const bool ok = parse_tokens(asm_tokens(bad), &err);
        if (ok) std::printf("  accepted: %s", bad);
        CHECK(!ok);
    }
    // Hand-built programs bypassing the parser: the translator and the interpreter bounds-check too.
    {
        Program p = asm_parse("ps_1_1\ntexcoord t0\ntexkill t1\ntex t2\ntexm3x2pad t3, t0\nmov r0, t0\n");
        for (size_t i = 0; i < 4 && i < p.instrs.size(); ++i) {
            Program q = p;
            q.instrs[i].dst.type = reg::kTemp;
            q.instrs[i].dst.index = 31;
            Translation t;
            CHECK(!translate(q, TranslateOptions{}, t, &err));
            Interpreter in;
            CHECK(!in.init(q, TranslateOptions{}, &err));
            q.instrs[i].dst.type = reg::kTexture;
            q.instrs[i].dst.index = 9;
            CHECK(!translate(q, TranslateOptions{}, t, &err));
            CHECK(!in.init(q, TranslateOptions{}, &err));
        }
    }
    {
        Program p = asm_parse("vs_2_0\ndefi i0, 3, 0, 1, 0\ndcl_position v0\nrep i0\nadd r0, r0, v0\nendrep\nmov oPos, r0\n");
        CHECK(p.instrs.size() == 6);
        if (p.instrs.size() == 6) {
            Program q = p;
            q.instrs[2].src[0].type = reg::kConst;
            q.instrs[2].src[0].index = 200;
            Translation t;
            CHECK(!translate(q, TranslateOptions{}, t, &err));
            Interpreter in;
            CHECK(!in.init(q, TranslateOptions{}, &err));
            q.instrs[2].src[0].type = reg::kConstInt;
            q.instrs[2].src[0].index = 100;
            CHECK(!translate(q, TranslateOptions{}, t, &err));
            CHECK(!in.init(q, TranslateOptions{}, &err));
        }
    }
    // (3) bump_env only for ps_1_x programs that use texbem/texbeml/bem.
    auto iface = [&](const char* text) {
        Translation t;
        Program q = asm_parse(text);
        if (!translate(q, TranslateOptions{}, t, &err)) {
            std::printf("  translate failed: %s\n", err.c_str());
            CHECK(false);
        }
        return t.iface;
    };
    CHECK(!iface("ps_2_0\ndcl t0\nmov r0, c20\nadd r0, r0, c31\nmov oC0, r0\n").bump_env);
    CHECK(iface("ps_2_0\ndcl t0\nmov r0, c20\nadd r0, r0, c31\nmov oC0, r0\n").float_constants == 32);
    CHECK(!iface("ps_1_1\ntex t0\nmul r0, t0, c7\n").bump_env);
    CHECK(iface("ps_1_1\ntex t0\ntexbem t1, t0\nmov r0, t1\n").bump_env);
    CHECK(iface("ps_1_4\ntexld r0, t0\ntexcrd r1.xyz, t1\nbem r2.xy, r1, r0\nmov r0, r2\n").bump_env);
    // (6) Sampler names survive the interface text whatever bytes they contain.
    {
        ShaderInterface f;
        f.profile = "ps_2_0";
        f.stage = ShaderType::Pixel;
        const std::vector<std::string> names = {"Plain", "two words", "line\nbreak", "-", "", "100%", "tab\there",
                                                std::string("ctl\x01\x7f\xff", 6), "%2D", "trailing "};
        for (size_t i = 0; i < names.size(); ++i) f.samplers.push_back({static_cast<uint8_t>(i), TextureDim::Tex2D, names[i]});
        f.float_constants = 4;
        const std::string text = f.serialize();
        ShaderInterface back;
        const bool ok = ShaderInterface::deserialize(text, back);
        CHECK(ok && back.samplers.size() == names.size());
        for (size_t i = 0; ok && i < names.size() && i < back.samplers.size(); ++i) CHECK(back.samplers[i].name == names[i]);
        CHECK(ok && back.float_constants == 4 && back.serialize() == text);
        CHECK(!ShaderInterface::deserialize("profile ps_2_0\nsampler 0 1 a b\n", back));    // unescaped space
        CHECK(!ShaderInterface::deserialize("profile ps_2_0\nsampler 0 1 bad%4\n", back));  // truncated escape
        CHECK(!ShaderInterface::deserialize("profile ps_2_0\nsampler 0 1 \n", back));       // missing name
    }
    // (5) Cache keys depend on the emitted HLSL and the DXC arguments.
    {
        const std::vector<uint8_t> blob = {1, 2, 3, 4};
        Translation t;
        t.hlsl = "float4 PSMain() : SV_Target { return 1; }";
        t.entry = "PSMain";
        t.target = "ps_6_0";
        const CacheKey a = CacheKey::make(blob, TranslateOptions{}, "dxc-test 1.0", t);
        Translation t2 = t;
        t2.hlsl += " ";
        const CacheKey b = CacheKey::make(blob, TranslateOptions{}, "dxc-test 1.0", t2);
        const CacheKey plain = plain_key(blob, TranslateOptions{}, "dxc-test 1.0");
        CHECK(a.source_hash != 0 && a.source_hash != b.source_hash);
        CHECK(a.describe() != b.describe() && a.file_name() != b.file_name() && a.file_name() != plain.file_name());
        CHECK(a.translator_version == kTranslatorVersion && kTranslatorVersion >= 2);
        const std::vector<std::string> args1 = {"-O3", "-T", "ps_6_0"}, args2 = {"-O3", "-Tps_6_0"};
        CHECK(CacheKey::source_hash_of(t.hlsl, args1) != CacheKey::source_hash_of(t.hlsl, args2));
        CHECK(CacheKey::source_hash_of(t.hlsl, args1) == CacheKey::source_hash_of(t.hlsl, args1));
        CHECK(CacheKey::source_hash_of("ab", std::vector<std::string>{"c"}) != CacheKey::source_hash_of("a", std::vector<std::string>{"bc"}));
    }
}

// ---------------------------------------------------------------- emitter
bool translate_blob(const Blob& b, Translation& t, std::string* err) {
    Program p;
    if (!parse(b.bytes, p, err)) return false;
    return translate(p, b.options, t, err);
}

void test_emitter(const std::vector<Blob>& syn) {
    std::printf("emitter tests (%zu synthetic programs)\n", syn.size());
    std::map<std::string, Translation> tr;
    for (const Blob& b : syn) {
        Translation t;
        std::string err;
        const bool ok = translate_blob(b, t, &err);
        if (!ok) std::printf("  %s: %s\n", b.name.c_str(), err.c_str());
        CHECK(ok);
        tr[b.name] = t;
        // interface text round trip
        ShaderInterface back;
        CHECK(ShaderInterface::deserialize(t.iface.serialize(), back));
        CHECK(back.serialize() == t.iface.serialize());
    }
    auto has = [&](const char* name, const char* needle) {
        const bool ok = tr.count(name) && tr[name].hlsl.find(needle) != std::string::npos;
        if (!ok) std::printf("  %s: missing '%s'\n", name, needle);
        CHECK(ok);
    };
    has("vs11_ops", "xl_expp1(");
    has("vs11_ops", "xl_logp1(");
    has("vs11_ops", "xl_addr_floor(");
    has("vs11_ops", "xl_crel(a0.x + 30)");
    has("vs11_ops", "[[vk::binding(0, 0)]] cbuffer VSConstants { float4 c[256]; };");
    has("vs20_flow", "xl_addr_round(");
    has("vs20_flow", "void xl_sub1()");
    has("vs20_flow", "aL += xl_lp");
    has("vs20_intconst", "[[vk::binding(2, 0)]] cbuffer VSIntConstants");
    has("ps11_ops", "((t1 - 0.5) * 2.0)");
    has("ps11_ops", "select(r0.wwww > 0.5, t0, r1)");
    has("ps11_ops", "clamp(c[0], -1.0, 1.0)");
    has("ps11_ops", "[[vk::binding(1, 0)]] cbuffer PSConstants { float4 c[32]; };");
    has("ps14_phase", "oDepth.x = r5.y == 0.0 ? 1.0 : r5.x / r5.y;");
    has("ps14_phase", ", -8.0, 8.0)");
    has("ps20_ops", "[[vk::combinedImageSampler]][[vk::binding(1, 1)]] TextureCube<float4> tex1;");
    has("ps2x_flow", "SampleGrad");
    has("vs30_io", "    oD0 = o1;");
    has("vs30_io", "    OUT.oD0 = oD0;");         // vs_3_0 colors are not saturated
    has("vs30_io", "    OUT.oFog = oFog.x;");
    has("vs30_io", "[[vk::builtin(\"PointSize\")]]");
    has("ps30_io", "SampleLevel(");
    has("ps30_io", "bool xl_face : SV_IsFrontFace;");
    has("ps30_io", "vPos = float4(IN.vpos.xy - 0.5, 0.0, 0.0);");
    has("ps30_io", "    v4 = float4(IN.fog, 0.0, 0.0, 0.0);");
    has("ps30_io", "centroid float4 t1 : TEXCOORD1;");
    has("ps30_io", "OUT.color = xl_epilogue(oC0);");  // no fixed-function fog for ps_3_0
    has("ps30_io", "if (i == 40) v = k40;");

    // Interfaces.
    if (tr.count("vs11_ops")) {
        const ShaderInterface& f = tr["vs11_ops"].iface;
        CHECK(f.inputs.size() == 4);
        if (f.inputs.size() == 4) {
            CHECK(f.inputs[0].location == 0 && f.inputs[0].usage == Usage::Position);
            CHECK(f.inputs[1].location == 1 && f.inputs[1].usage == Usage::Normal);
            CHECK(f.inputs[2].location == 2 && f.inputs[2].usage == Usage::Texcoord);
            CHECK(f.inputs[3].location == 5 && f.inputs[3].usage == Usage::Color);
        }
        CHECK(f.relative_addressing && f.float_constants == 256);
        CHECK(f.varyings.size() == 11);
    }
    if (tr.count("ps20_ops")) {
        const ShaderInterface& f = tr["ps20_ops"].iface;
        CHECK(f.samplers.size() == 3 && f.writes_depth && f.can_discard && !f.bump_env);
        if (f.samplers.size() == 3) CHECK(f.samplers[0].dim == TextureDim::Tex2D && f.samplers[1].dim == TextureDim::Cube);
        CHECK(f.float_constants == 5);
    }
    if (tr.count("ps11_texbem")) CHECK(tr["ps11_texbem"].iface.bump_env);
    if (tr.count("vs20_intconst")) CHECK(tr["vs20_intconst"].iface.int_bool_constants);
    if (tr.count("ps20_mrt")) CHECK(tr["ps20_mrt"].iface.color_outputs == 2);
    if (tr.count("vs30_io")) {
        const ShaderInterface& f = tr["vs30_io"].iface;
        CHECK(f.major == 3 && f.inputs.size() == 4 && f.writes_point_size && f.relative_addressing);
        // COLOR0, COLOR1, TEXCOORD0 (.xy), TEXCOORD1, TEXCOORD5 (.xyz), FOG.
        CHECK(f.varyings.size() == 6);
        if (f.varyings.size() == 6) {
            CHECK(f.varyings[0].location == kVaryingColor0 && f.varyings[1].location == kVaryingColor1);
            CHECK(f.varyings[2].location == kVaryingTexcoord0 && f.varyings[2].mask == 3);
            CHECK(f.varyings[4].location == kVaryingTexcoord0 + 5 && f.varyings[4].mask == 7);
            CHECK(f.varyings[5].location == kVaryingFog);
        }
    }
    if (tr.count("ps30_io")) {
        const ShaderInterface& f = tr["ps30_io"].iface;
        CHECK(f.major == 3 && f.samplers.size() == 3 && f.writes_depth && f.can_discard && !f.int_bool_constants);
        // COLOR0, COLOR1, TEXCOORD0, TEXCOORD1, TEXCOORD7, FOG.
        CHECK(f.varyings.size() == 6);
        if (f.samplers.size() == 3) CHECK(f.samplers[1].dim == TextureDim::Cube);
    }

    // Golden text.
    const std::filesystem::path gdir = ALAMO_SHADERXLAT_GOLDEN_DIR;
    for (const char* name : {"vs11_skin", "vs20_flow", "ps11_ops", "ps14_phase", "ps20_ops", "vs30_io", "ps30_io"}) {
        if (!tr.count(name)) continue;
        const std::filesystem::path f = gdir / (std::string(name) + ".hlsl");
        if (g_write_golden) {
            std::filesystem::create_directories(gdir);
            std::ofstream o(f, std::ios::binary);
            o << tr[name].hlsl;
            std::printf("  wrote %s\n", f.string().c_str());
            continue;
        }
        std::ifstream in(f, std::ios::binary);
        const std::string want((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const bool same = !want.empty() && want == tr[name].hlsl;
        if (!same) std::printf("  golden mismatch: %s (rerun with --write-golden after reviewing)\n", f.string().c_str());
        CHECK(same);
    }
}

// ---------------------------------------------------------------- fuzz
void fuzz_one(const std::vector<uint8_t>& bytes, Rng& rng, uint32_t& parsed, uint32_t& translated, uint32_t& ran) {
    Program p;
    if (!parse(bytes, p, nullptr)) return;
    ++parsed;
    TranslateOptions opt;
    opt.projected_stages = static_cast<uint16_t>(rng.below(65536));
    Translation t;
    std::string err;
    const bool tok = translate(p, opt, t, &err);
    translated += tok ? 1u : 0u;
    Interpreter in;
    if (!in.init(p, opt, &err)) return;
    ConstantState k;
    for (auto& c : k.c)
        for (float& x : c) x = rng.uni(-4, 4);
    for (auto& i : k.i)
        for (int32_t& x : i) x = static_cast<int32_t>(rng.below(600)) - 100;
    for (uint32_t& b : k.b) b = rng.below(2);
    TranslatedPush push;
    push.alpha_func = rng.below(8);
    push.flags = rng.below(8);
    if (p.is_vs()) {
        std::array<F4, 16> v{};
        for (auto& c : v)
            for (float& x : c) x = rng.uni(-30, 30);
        VsResult r;
        in.run_vs(v, k, push, r);
    } else {
        PsInputs pi;
        for (auto& c : pi.texcoord)
            for (float& x : c) x = rng.uni(-3, 3);
        PsResult r;
        in.run_ps(pi, k, push, nullptr, r);
    }
    ++ran;
}

void test_fuzz(const std::vector<Blob>& seeds) {
    const uint32_t iters = 30000;
    std::printf("fuzz loop: %u mutations over %zu seed programs\n", iters, seeds.size());
    Rng rng(12345);
    uint32_t parsed = 0, translated = 0, ran = 0;
    for (uint32_t it = 0; it < iters; ++it) {
        const Blob& s = seeds[rng.below(static_cast<uint32_t>(seeds.size()))];
        std::vector<uint8_t> b = s.bytes;
        const uint32_t kind = rng.below(6);
        const uint32_t nmut = 1 + rng.below(4);
        for (uint32_t m = 0; m < nmut && !b.empty(); ++m) {
            switch (kind) {
            case 0: b.resize(rng.below(static_cast<uint32_t>(b.size()))); break;                    // truncate
            case 1: b[rng.below(static_cast<uint32_t>(b.size()))] ^= static_cast<uint8_t>(1u << rng.below(8)); break;  // bit flip
            case 2: {                                                                               // random token
                const uint32_t w = rng.below(static_cast<uint32_t>(b.size() / 4));
                const uint32_t v = static_cast<uint32_t>(rng.next());
                std::memcpy(b.data() + 4 * static_cast<size_t>(w), &v, 4);
                break;
            }
            case 3: {                                                                               // insert token
                const uint32_t w = rng.below(static_cast<uint32_t>(b.size() / 4 + 1));
                const uint32_t v = static_cast<uint32_t>(rng.next()) | (rng.below(2) ? 0x80000000u : 0u);
                uint8_t tmp[4];
                std::memcpy(tmp, &v, 4);
                b.insert(b.begin() + 4 * static_cast<ptrdiff_t>(w), tmp, tmp + 4);
                break;
            }
            case 4: {                                                                               // mutate a field
                const uint32_t w = rng.below(static_cast<uint32_t>(b.size() / 4));
                uint32_t v;
                std::memcpy(&v, b.data() + 4 * static_cast<size_t>(w), 4);
                v ^= (0xFu << (4 * rng.below(8)));
                std::memcpy(b.data() + 4 * static_cast<size_t>(w), &v, 4);
                break;
            }
            default: b.push_back(static_cast<uint8_t>(rng.next())); break;                          // odd size
            }
        }
        fuzz_one(b, rng, parsed, translated, ran);
    }
    // Pure random input.
    for (uint32_t it = 0; it < 2000; ++it) {
        std::vector<uint8_t> b(4 + rng.below(256));
        for (uint8_t& x : b) x = static_cast<uint8_t>(rng.next());
        if (rng.below(2)) {
            const uint32_t v = 0xFFFE0101u | (rng.below(2) << 16);
            std::memcpy(b.data(), &v, 4);
        }
        fuzz_one(b, rng, parsed, translated, ran);
    }
    // Interface text.
    for (uint32_t it = 0; it < 2000; ++it) {
        std::string s;
        const uint32_t n = rng.below(200);
        for (uint32_t i = 0; i < n; ++i) s += static_cast<char>(rng.below(2) ? "profile input varying sampler flags 0123456789\n"[rng.below(46)] : static_cast<char>(rng.next()));
        ShaderInterface f;
        ShaderInterface::deserialize(s, f);
    }
    std::printf("  parsed %u, translated %u, interpreted %u (no crash / hang)\n", parsed, translated, ran);
    CHECK(parsed > iters / 20);
}

// ---------------------------------------------------------------- determinism
void test_determinism(const std::vector<Blob>& blobs) {
    std::printf("determinism: %zu programs, 1 vs 8 threads\n", blobs.size());
    std::vector<std::string> ref(blobs.size());
    for (size_t i = 0; i < blobs.size(); ++i) {
        Translation a, b;
        std::string err;
        const bool ok = translate_blob(blobs[i], a, &err) && translate_blob(blobs[i], b, &err);
        if (!ok) continue;
        CHECK(a.hlsl == b.hlsl && a.iface.serialize() == b.iface.serialize());
        ref[i] = a.hlsl;
    }
    std::atomic<int> mismatches{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
        threads.emplace_back([&, t] {
            for (size_t k = 0; k < blobs.size(); ++k) {
                const size_t i = (k + static_cast<size_t>(t) * 7) % blobs.size();
                Translation x;
                std::string err;
                if (!translate_blob(blobs[i], x, &err)) {
                    if (!ref[i].empty()) ++mismatches;
                    continue;
                }
                if (x.hlsl != ref[i]) ++mismatches;
            }
        });
    for (auto& th : threads) th.join();
    std::printf("  %d mismatches\n", mismatches.load());
    CHECK(mismatches.load() == 0);
}

// ---------------------------------------------------------------- cache
std::filesystem::path temp_dir(const char* tag) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::error_code ec;
    return std::filesystem::temp_directory_path(ec) /
           ("alamo_shaderxlat_" + std::string(tag) + "_" + std::to_string(static_cast<long long>(now)));
}

CacheEntry cache_test_entry() {
    CacheEntry e;
    e.spirv = {0x07230203u, 0x00010600u, 0u, 12u, 0u, 0xDEADBEEFu};
    e.entry = "PSMain";
    e.iface.profile = "ps_2_0";
    e.iface.stage = ShaderType::Pixel;
    e.iface.samplers.push_back({3, TextureDim::Cube, "SkyCubeSampler"});
    e.iface.varyings.push_back({2, VaryingSemantic::Texcoord, 0, 3});
    e.iface.writes_depth = true;
    return e;
}

// Child process of the multi-process cache test: hammers one shared key and its own keys.
int cache_writer(const std::string& dir, int id) {
    ShaderCache c(dir);
    const std::vector<uint8_t> blob = {1, 2, 3, 4, 5, 6, 7, 8};
    const CacheKey shared = plain_key(blob, TranslateOptions{}, "dxc-test 1.0");
    const CacheEntry e = cache_test_entry();
    int bad = 0;
    for (int i = 0; i < 60; ++i) {
        if (!c.store(shared, e)) ++bad;
        CacheEntry r;
        if (c.load(shared, r) && r.spirv != e.spirv) ++bad;
        std::vector<uint8_t> b2 = blob;
        b2.push_back(0xEE);
        b2.push_back(static_cast<uint8_t>(id));
        b2.push_back(static_cast<uint8_t>(i));
        if (!c.store(plain_key(b2, TranslateOptions{}, "dxc-test 1.0"), e)) ++bad;
    }
    return bad ? 1 : 0;
}

// deep: the cache folder is nested beyond 300 characters (Windows MAX_PATH), made and removed via long_path().
void test_cache(bool deep) {
    std::printf("cache round trip%s\n", deep ? " (path over 300 characters)" : "");
    const auto root = temp_dir(deep ? "cache_deep" : "cache");
    auto dir = root;
    if (deep) {
        while (dir.native().size() <= 320) dir /= "nested_shader_cache_level_0123456789";
        std::error_code dec;
        CHECK(std::filesystem::create_directories(alamo::long_path(dir), dec) && !dec);
    }
    ShaderCache cache(dir);
    const std::vector<uint8_t> blob = {1, 2, 3, 4, 5, 6, 7, 8};
    TranslateOptions o;
    CacheKey k = plain_key(blob, o, "dxc-test 1.0");
    const CacheEntry e = cache_test_entry();
    CacheEntry got;
    CHECK(!cache.load(k, got));
    CHECK(cache.store(k, e));
    CHECK(cache.load(k, got));
    CHECK(got.spirv == e.spirv && got.entry == e.entry && got.iface.serialize() == e.iface.serialize());
    // Any key field change is a miss.
    CacheKey k2 = k;
    k2.compiler = "dxc-test 1.1";
    CHECK(!cache.load(k2, got));
    o.sampler_dims[3] = TextureDim::Cube;
    CHECK(!cache.load(plain_key(blob, o, "dxc-test 1.0"), got));
    CHECK(k.file_name() != plain_key(blob, o, "dxc-test 1.0").file_name());
    // Corruption / truncation is a miss.
    const auto file = dir / k.file_name();
    {
        std::fstream f(alamo::long_path(file), std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(40);
        f.put('\x5A');
    }
    CHECK(!cache.load(k, got));
    CHECK(cache.store(k, e));
    std::filesystem::resize_file(alamo::long_path(file), std::filesystem::file_size(alamo::long_path(file)) - 3);
    CHECK(!cache.load(k, got));
    // Concurrent writers (same key and distinct keys), as several processes would.
    std::vector<std::thread> threads;
    std::atomic<int> failures{0};
    for (int t = 0; t < 8; ++t)
        threads.emplace_back([&, t] {
            ShaderCache c(dir);
            for (int i = 0; i < 40; ++i) {
                if (!c.store(k, e)) ++failures;
                std::vector<uint8_t> b2 = blob;
                b2.push_back(static_cast<uint8_t>((t * 40 + i) & 0xFF));
                b2.push_back(static_cast<uint8_t>((t * 40 + i) >> 8));
                CacheEntry e2 = e;
                e2.spirv.push_back(static_cast<uint32_t>(t * 40 + i));
                if (!c.store(plain_key(b2, TranslateOptions{}, "dxc-test 1.0"), e2)) ++failures;
                CacheEntry r;
                if (c.load(k, r) && r.spirv != e.spirv) ++failures;
            }
        });
    for (auto& th : threads) th.join();
    CHECK(failures.load() == 0);
    CHECK(cache.load(k, got) && got.spirv == e.spirv);
    int ok = 0;
    for (int i = 0; i < 320; ++i) {
        std::vector<uint8_t> b2 = blob;
        b2.push_back(static_cast<uint8_t>(i & 0xFF));
        b2.push_back(static_cast<uint8_t>(i >> 8));
        CacheEntry r;
        if (cache.load(plain_key(b2, TranslateOptions{}, "dxc-test 1.0"), r) && r.spirv.back() == static_cast<uint32_t>(i)) ++ok;
    }
    CHECK(ok == 256 + 64);
    size_t tmp = 0;
    std::error_code lec;
    for (const auto& de : alamo::list_directory(dir, lec))
        if (de.extension() == ".tmp") ++tmp;  // write_file_atomic temp names end in .tmp
    CHECK(!lec);
    CHECK(tmp == 0);
    std::printf("  concurrent stores (8 threads): %d failures, %zu leftover temp files\n", failures.load(), tmp);
    // Several processes sharing the directory.
    if (!g_exe.empty()) {
        std::atomic<int> rc_bad{0};
        std::vector<std::thread> procs;
        for (int p = 0; p < 4; ++p)
            procs.emplace_back([&, p] {
                std::string cmd = "\"" + g_exe + "\" cache-writer \"" + dir.string() + "\" " + std::to_string(p);
#ifdef _WIN32
                cmd = "\"" + cmd + "\"";
#endif
                if (std::system(cmd.c_str()) != 0) ++rc_bad;
            });
        for (auto& th : procs) th.join();
        int found = 0;
        for (int p = 0; p < 4; ++p)
            for (int i = 0; i < 60; ++i) {
                std::vector<uint8_t> b2 = blob;
                b2.push_back(0xEE);
                b2.push_back(static_cast<uint8_t>(p));
                b2.push_back(static_cast<uint8_t>(i));
                CacheEntry r;
                found += cache.load(plain_key(b2, TranslateOptions{}, "dxc-test 1.0"), r) && r.spirv == e.spirv ? 1 : 0;
            }
        size_t tmp2 = 0;
        for (const auto& de : alamo::list_directory(dir, lec))
            if (de.extension() == ".tmp") ++tmp2;
        std::printf("  4 writer processes: %d failed, %d/240 entries readable, shared entry ok: %s, %zu temp files\n", rc_bad.load(),
                    found, cache.load(k, got) && got.spirv == e.spirv ? "yes" : "no", tmp2);
        CHECK(rc_bad.load() == 0 && found == 240 && tmp2 == 0);
        CHECK(cache.load(k, got) && got.spirv == e.spirv);
    }
    std::error_code ec;
    alamo::remove_all_long(root, ec);
    CHECK(!ec && !std::filesystem::exists(alamo::long_path(root), ec));
}

// ---------------------------------------------------------------- runtime DXC
bool g_dxc_ok = false;

void test_dxc(RuntimeDxc& dxc, const std::vector<Blob>& syn) {
    std::string err;
    if (!dxc.load({}, &err)) {
        std::printf("SKIP runtime DXC: %s\n", err.c_str());
        return;
    }
    g_dxc_ok = true;
    std::printf("runtime DXC: %s (%s)\n", dxc.version().c_str(), dxc.library_path().c_str());
    std::map<std::string, std::vector<uint32_t>> serial;
    for (const Blob& b : syn) {
        Translation t;
        if (!translate_blob(b, t, &err)) continue;
        std::vector<uint32_t> spv;
        std::string diag;
        const bool ok = dxc.compile(t, spv, &diag);
        if (!ok) std::printf("  %s: DXC failed:\n%s\n", b.name.c_str(), diag.c_str());
        CHECK(ok);
        serial[b.name] = spv;
    }
    // Q3 instanced form: every synthetic program also translates + compiles with storage-buffer constants; its options
    // key differs only when set (the default form's keys are unchanged); the interface text carries the flag.
    {
        uint32_t inst_ok = 0;
        for (const Blob& b : syn) {
            Program q;
            std::string e2;
            if (!parse(b.bytes, q, &e2)) continue;
            TranslateOptions o = b.options;
            o.instanced = true;
            Translation ti, td;
            if (!translate(q, o, ti, &e2) || !translate(q, b.options, td, &e2)) continue;
            CHECK(ti.iface.instanced && !td.iface.instanced && o.key() != b.options.key());
            CHECK(ti.hlsl.find("StructuredBuffer<float4> xl_c") != std::string::npos && ti.hlsl.find("cbuffer VSConstants") == std::string::npos &&
                  ti.hlsl.find("cbuffer PSConstants") == std::string::npos);
            ShaderInterface back;
            CHECK(ShaderInterface::deserialize(ti.iface.serialize(), back) && back.instanced);
            std::vector<uint32_t> spv;
            std::string diag;
            const bool ok = dxc.compile(ti, spv, &diag);
            if (!ok) std::printf("  %s (instanced): DXC failed:\n%s\n", b.name.c_str(), diag.c_str());
            CHECK(ok);
            inst_ok += ok ? 1u : 0u;
        }
        std::printf("  instanced form: %u synthetic programs compiled\n", inst_ok);
        CHECK(inst_ok > 0);
    }
    // 8 threads compiling concurrently give identical SPIR-V.
    std::atomic<int> bad{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
        threads.emplace_back([&] {
            for (const Blob& b : syn) {
                Translation tr;
                std::string e2;
                if (!translate_blob(b, tr, &e2)) continue;
                std::vector<uint32_t> spv;
                if (!dxc.compile(tr, spv, nullptr) || spv != serial[b.name]) ++bad;
            }
        });
    for (auto& th : threads) th.join();
    CHECK(bad.load() == 0);
    // translate_and_compile through the cache.
    const auto dir = temp_dir("dxccache");
    ShaderCache cache(dir);
    const Blob& b = syn.front();
    CompiledProgram a, c;
    CHECK(translate_and_compile(b.bytes, b.options, dxc, &cache, a, &err) && !a.from_cache);
    CHECK(translate_and_compile(b.bytes, b.options, dxc, &cache, c, &err) && c.from_cache);
    CHECK(a.spirv == c.spirv && a.iface.serialize() == c.iface.serialize() && c.entry == "VSMain");
    std::printf("  %zu synthetic programs compiled (serial + 8 threads identical: %s), cache hit on reload: %s\n",
                serial.size(), bad.load() == 0 ? "yes" : "no", c.from_cache ? "yes" : "no");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ---------------------------------------------------------------- corpus
void test_corpus(const std::vector<Blob>& corpus, const RuntimeDxc& dxc) {
    std::printf("corpus: %zu unique blobs\n", corpus.size());
    std::map<std::string, std::array<int, 4>> by_profile;  // total, parsed, translated, compiled
    std::vector<std::string> failures;
    std::vector<char> compiled(corpus.size(), 0), translated(corpus.size(), 0);
    std::vector<Translation> trs(corpus.size());
    for (size_t i = 0; i < corpus.size(); ++i) {
        const Blob& b = corpus[i];
        Program p;
        std::string err;
        std::string prof = "?";
        if (!parse(b.bytes, p, &err)) {
            failures.push_back(b.name + " " + b.origin + ": parse: " + err);
            by_profile[prof][0]++;
            continue;
        }
        prof = p.profile();
        auto& c = by_profile[prof];
        c[0]++;
        c[1]++;
        // disassembler round trip
        std::vector<uint32_t> tok;
        std::string e2;
        if (!assemble(disassemble(p), tok, &e2)) {
            failures.push_back(b.name + ": reassembly: " + e2);
        } else {
            Program q;
            if (!parse(to_bytes(tok), q, &e2) || disassemble(q) != disassemble(p)) failures.push_back(b.name + ": round trip differs");
        }
        Interpreter in;
        if (!in.init(p, b.options, &e2)) failures.push_back(b.name + ": interpreter init: " + e2);
        if (!translate(p, b.options, trs[i], &err)) {
            failures.push_back(b.name + " " + b.origin + " " + prof + ": translate: " + err);
            continue;
        }
        translated[i] = 1;
        c[2]++;
        if (!g_dump_dir.empty()) {
            std::filesystem::create_directories(g_dump_dir);
            std::ofstream(std::filesystem::path(g_dump_dir) / (b.name + "_" + prof + ".hlsl"), std::ios::binary) << trs[i].hlsl;
            std::ofstream(std::filesystem::path(g_dump_dir) / (b.name + "_" + prof + ".asm"), std::ios::binary)
                << "// " << b.origin << "\n" << disassemble(p);
        }
    }
    if (dxc.loaded()) {
        std::mutex m;
        std::atomic<size_t> next{0};
        std::vector<std::thread> threads;
        const auto t0 = std::chrono::steady_clock::now();
        for (int t = 0; t < 8; ++t)
            threads.emplace_back([&] {
                for (size_t i = next++; i < corpus.size(); i = next++) {
                    if (!translated[i]) continue;
                    std::vector<uint32_t> spv;
                    std::string diag;
                    if (dxc.compile(trs[i], spv, &diag)) {
                        compiled[i] = 1;
                    } else {
                        std::lock_guard<std::mutex> lock(m);
                        failures.push_back(corpus[i].name + " " + corpus[i].origin + ": dxc: " + diag.substr(0, 400));
                    }
                }
            });
        for (auto& th : threads) th.join();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        for (size_t i = 0; i < corpus.size(); ++i)
            if (compiled[i]) {
                Program p;
                parse(corpus[i].bytes, p, nullptr);
                by_profile[p.profile()][3]++;
            }
        std::printf("  DXC compile of the corpus: %.0f ms on 8 threads\n", ms);
    }
    int total = 0, ok = 0;
    for (const auto& kv : by_profile) {
        std::printf("  %-7s total %3d  parsed %3d  translated %3d  compiled %3d\n", kv.first.c_str(), kv.second[0], kv.second[1],
                    kv.second[2], kv.second[3]);
        total += kv.second[0];
        ok += dxc.loaded() ? kv.second[3] : kv.second[2];
    }
    std::printf("  TOTAL %d/%d %s\n", ok, total, dxc.loaded() ? "translated + compiled" : "translated (no DXC)");
    // vkd3d's 61 failures.
    std::map<std::string, size_t> idx;
    for (size_t i = 0; i < corpus.size(); ++i) idx[corpus[i].name] = i;
    int found = 0, hard_ok = 0;
    for (const HardCase& h : vkd3d_hard_cases()) {
        const auto it = idx.find(h.sha12);
        if (it == idx.end()) continue;
        ++found;
        if (dxc.loaded() ? compiled[it->second] : translated[it->second]) ++hard_ok;
    }
    std::printf("  vkd3d-hard blobs: %d of 61 present in the corpus, %d ok\n", found, hard_ok);
    for (const std::string& f : failures) std::printf("  FAIL %s\n", f.c_str());
    CHECK(failures.empty());
    CHECK(hard_ok == found);
}

}  // namespace
}  // namespace alamo::shaderxlat::test

int main(int argc, char** argv) {
    using namespace alamo::shaderxlat;
    using namespace alamo::shaderxlat::test;
    std::string mode = "all";
    if (argc >= 4 && std::string(argv[1]) == "cache-writer") return cache_writer(argv[2], std::atoi(argv[3]));
    g_exe = argc > 0 ? argv[0] : "";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--write-golden") g_write_golden = true;
        else if (a == "--dump" && i + 1 < argc) g_dump_dir = argv[++i];
        else mode = a;
    }
    const std::vector<Blob> syn = synthetic_blobs();
    std::string note;
    const std::vector<Blob> corpus = corpus_blobs(&note);
    std::printf("synthetic programs: %zu; corpus: %zu blobs (%s)\n", syn.size(), corpus.size(), note.c_str());
    int rc = 0;
    if (mode == "cpu" || mode == "all") {
        test_parser_units();
        test_parser_malformed();
        test_review_regressions();
        test_emitter(syn);
        std::vector<Blob> seeds = syn;
        seeds.insert(seeds.end(), corpus.begin(), corpus.end());
        test_fuzz(seeds);
        test_determinism(seeds);
        test_cache(false);
        test_cache(true);
        RuntimeDxc dxc;
        test_dxc(dxc, syn);
        if (corpus.empty()) std::printf("SKIP corpus: %s\n", note.c_str());
        else test_corpus(corpus, dxc);
        if (g_fail) {
            std::printf("alamo_shaderxlat_tests cpu: %d failure(s)\n", g_fail);
            rc = 1;
        } else {
            std::printf("alamo_shaderxlat_tests cpu: OK\n");
        }
    }
    if (mode == "gpu" || mode == "all") {
        const int g = run_gpu_tests(syn, corpus);
        if (g) rc = 1;
    }
    return rc;
}
