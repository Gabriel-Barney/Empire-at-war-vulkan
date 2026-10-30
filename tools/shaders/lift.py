#!/usr/bin/env python3
"""lift.py - best-effort lifter from D3D9 shader bytecode (parsed by d3d9sm) to readable
HLSL-like pseudocode.

The output is NOT meant to recompile verbatim; it is a faithful, register-level
translation with symbolic names:
  * c#  -> CTAB parameter names (m_world[1] = register row 1 of m_world), def'd
           literals are inlined
  * v#  -> in.<SEMANTIC> from dcl (vs, ps_3_0) / in.COLOR# / in.TEXCOORD# (ps_2_0, ps_1_x)
  * o*  -> out.POSITION / out.TEXCOORD# / out.COLOR# / out.FOG / out.DEPTH
  * s#  -> sampler parameter names from CTAB
  * preshader (CPU-side constant folding) is emitted as a separate "CPU" block that
    computes the shader constants that are not in the CTAB.

Usage: python -I lift.py blob.bin
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import d3d9sm as D  # noqa: E402

COMP = "xyzw"


def _fmtf(v):
    if v == int(v) and abs(v) < 1e7:
        return "%d" % int(v) if v != 0 or str(v)[0] != "-" else "0"
    return "%.7g" % v


class Lifter:
    def __init__(self, sh, label=""):
        self.sh = sh
        self.label = label
        self.defs = {}           # const reg -> 4 floats
        self.inputs = {}         # v# -> semantic
        self.outputs = {}        # o# (vs3) -> semantic
        self.ps_t = {}           # ps_2_0 t# -> semantic
        self.ps_samplers = {}    # s# -> texture type
        for ins in sh.instructions:
            if ins.opcode == 81:
                self.defs[ins.dst[0].num] = ins.extra
            elif ins.opcode == 31:
                d = ins.dst[0]
                tok = ins.extra
                usage = tok & 0x1F
                uidx = (tok >> 16) & 0xF
                sem = (D.USAGE[usage] if usage < len(D.USAGE) else "usage%d" % usage).upper() + str(uidx)
                if d.type == D.REG_INPUT:
                    if sh.stage == "vs" or sh.major >= 3:
                        self.inputs[d.num] = sem
                    else:
                        self.inputs[d.num] = "COLOR%d" % d.num
                elif d.type == D.REG_OUTPUT and sh.stage == "vs":
                    self.outputs[d.num] = sem
                elif d.type == D.REG_ADDR_TEX and sh.stage == "ps":
                    self.ps_t[d.num] = "TEXCOORD%d" % d.num
                elif d.type == D.REG_SAMPLER:
                    self.ps_samplers[d.num] = D.SAMPLER_TEX.get((tok >> 27) & 0xF, "2d") or "2d"

    # ------------------------------------------------------------ names
    def const_name(self, num, rel=None):
        c, off = D.ctab_lookup(self.sh.ctab, "float4", num)
        if rel is not None:
            if c is None:
                return "c[%s + %d]" % (rel, num)
            return "%s.reg[%s + %d]" % (c["name"], rel, off) if off else "%s.reg[%s]" % (c["name"], rel)
        if c is None:
            return "c%d" % num
        t = c["type"]
        if c["count"] == 1 and t["elements"] <= 1:
            return c["name"]
        return "%s[%d]" % (c["name"], off)

    def sampler_name(self, num):
        c, _ = D.ctab_lookup(self.sh.ctab, "sampler", num)
        return c["name"] if c else "sampler%d" % num

    def reg_base(self, p, rel=None):
        sh = self.sh
        t, n = p.type, p.num
        if t == D.REG_CONST:
            return self.const_name(n, rel)
        if t in (D.REG_CONST2, D.REG_CONST3, D.REG_CONST4):
            return self.const_name(n + {11: 2048, 12: 4096, 13: 6144}[t], rel)
        if t == D.REG_INPUT:
            if sh.stage == "ps" and sh.major < 3:
                return "in.COLOR%d" % n
            return "in.%s" % self.inputs.get(n, "v%d" % n)
        if t == D.REG_ADDR_TEX:
            if sh.stage == "ps":
                if sh.major >= 2 or sh.minor >= 4:
                    return "in.%s" % self.ps_t.get(n, "TEXCOORD%d" % n)
                return "t%d" % n
            return "a%d" % n
        if t == D.REG_RASTOUT:
            return ["out.POSITION", "out.FOG", "out.PSIZE"][n] if n < 3 else "out.rast%d" % n
        if t == D.REG_ATTROUT:
            return "out.COLOR%d" % n
        if t == D.REG_OUTPUT:
            if sh.major >= 3:
                return "out.%s" % self.outputs.get(n, "o%d" % n)
            return "out.TEXCOORD%d" % n
        if t == D.REG_COLOROUT:
            return "out.COLOR%d" % n
        if t == D.REG_DEPTHOUT:
            return "out.DEPTH"
        if t == D.REG_SAMPLER:
            return self.sampler_name(n)
        return sh.reg_name(t, n)

    # ------------------------------------------------------------ operands
    def src(self, s, comps):
        """Expression for source s reading swizzle positions `comps` (list of 0..3)."""
        sh = self.sh
        sel = [(s.swz >> (2 * i)) & 3 for i in comps]
        if s.type == D.REG_CONST and s.num in self.defs and s.rel is None:
            vals = [self.defs[s.num][k] for k in sel]
            if len(set(vals)) == 1:
                e = _fmtf(vals[0])
            else:
                e = "float%d(%s)" % (len(vals), ", ".join(_fmtf(v) for v in vals))
        else:
            rel = None
            if s.rel is not None:
                if isinstance(s.rel, str):
                    rel = s.rel
                else:
                    rel = sh.reg_name(s.rel.type, s.rel.num) + "." + COMP[s.rel.swz & 3]
            base = self.reg_base(s, rel)
            sw = "".join(COMP[k] for k in sel)
            if sw == "xyzw"[:len(sw)] and len(sw) == 4:
                e = base
            else:
                e = "%s.%s" % (base, sw)
        m = s.mod
        if m == 1:
            e = "-" + e
        elif m == 2:
            e = "(%s - 0.5)" % e
        elif m == 3:
            e = "-(%s - 0.5)" % e
        elif m == 4:
            e = "(2 * %s - 1)" % e
        elif m == 5:
            e = "-(2 * %s - 1)" % e
        elif m == 6:
            e = "(1 - %s)" % e
        elif m == 7:
            e = "(2 * %s)" % e
        elif m == 8:
            e = "-(2 * %s)" % e
        elif m == 9:
            e = "(%s / %s.z)" % (e, e)
        elif m == 10:
            e = "(%s / %s.w)" % (e, e)
        elif m == 11:
            e = "abs(%s)" % e
        elif m == 12:
            e = "-abs(%s)" % e
        elif m == 13:
            e = "!%s" % e
        return e

    def dst(self, d):
        base = self.reg_base(d)
        comps = [i for i in range(4) if d.mask & (1 << i)] or [0, 1, 2, 3]
        if len(comps) == 4:
            return base, comps
        return base + "." + "".join(COMP[i] for i in comps), comps

    @staticmethod
    def _wrap(d, expr):
        sat = d.resmod & 1
        sh = d.shift
        scale = {1: "2 * ", 2: "4 * ", 3: "8 * "}.get(sh)
        div = {15: " / 2", 14: " / 4", 13: " / 8"}.get(sh)
        if scale:
            expr = "%s(%s)" % (scale, expr)
        if div:
            expr = "(%s)%s" % (expr, div)
        if sat:
            expr = "saturate(%s)" % expr
        return expr

    # ------------------------------------------------------------ instructions
    def lift_ins(self, ins):
        sh = self.sh
        op = ins.opcode
        n = ins.name
        if op in (31, 81, 47, 48):
            return None
        if op == 0:
            return "// nop"
        if op == 0xFFFD:
            return "// phase (ps_1_4: second texture-addressing phase)"
        if op == 65:   # texkill
            d = ins.dst[0]
            return "clip(%s.xyz);   // texkill" % self.reg_base(d)
        if not ins.dst:
            srcs = [self.src(s, [0]) for s in ins.src]
            return "// %s %s" % (n, ", ".join(srcs))
        d = ins.dst[0]
        lhs, comps = self.dst(d)
        S = ins.src
        w = len(comps)

        def cw(i):
            return self.src(S[i], comps)

        def full(i, k):
            return self.src(S[i], list(range(k)))

        def scal(i):
            s = S[i]
            sel = [(s.swz >> (2 * j)) & 3 for j in range(4)]
            return self.src(s, [0] if len(set(sel)) == 1 else [3])

        tex_ps1 = sh.stage == "ps" and sh.major == 1
        e = None
        if op == 1:
            if d.type == D.REG_ADDR_TEX and sh.stage == "vs":
                return "%s = (int)%s;   // vs_1_1 mov to a0 truncates toward zero" % (lhs, cw(0))
            e = cw(0)
        elif op == 46:
            return "%s = (int)round(%s);" % (lhs, cw(0))
        elif op == 2:
            e = "%s + %s" % (cw(0), cw(1))
        elif op == 3:
            e = "%s - %s" % (cw(0), cw(1))
        elif op == 4:
            e = "%s * %s + %s" % (cw(0), cw(1), cw(2))
        elif op == 5:
            e = "%s * %s" % (cw(0), cw(1))
        elif op == 6:
            e = "1 / %s" % scal(0)
        elif op == 7:
            e = "rsqrt(%s)" % scal(0)
        elif op == 8:
            e = "dot(%s, %s)" % (full(0, 3), full(1, 3))
        elif op == 9:
            e = "dot(%s, %s)" % (full(0, 4), full(1, 4))
        elif op == 10:
            e = "min(%s, %s)" % (cw(0), cw(1))
        elif op == 11:
            e = "max(%s, %s)" % (cw(0), cw(1))
        elif op == 12:
            e = "(%s < %s) ? 1 : 0" % (cw(0), cw(1))
        elif op == 13:
            e = "(%s >= %s) ? 1 : 0" % (cw(0), cw(1))
        elif op in (78, 79) and sh.stage == "vs" and sh.major == 1:
            x = scal(0)
            if op == 78:   # vs_1_1 expp: (2^floor(x), frac(x), 2^x (partial), 1)
                per = {0: "exp2(floor(%s))" % x, 1: "frac(%s)" % x, 2: "exp2(%s)" % x, 3: "1"}
            else:          # vs_1_1 logp: (exponent(x), mantissa(x), log2(x) (partial), 1)
                per = {0: "floor(log2(abs(%s)))" % x, 1: "abs(%s) / exp2(floor(log2(abs(%s))))" % (x, x),
                       2: "log2(abs(%s))" % x, 3: "1"}
            if len(comps) == 1:
                e = per[comps[0]]
            else:
                e = "float%d(%s)" % (len(comps), ", ".join(per[c] for c in comps))
        elif op in (14, 78):
            e = "exp2(%s)%s" % (scal(0), "   /* partial precision */" if op == 78 else "")
        elif op in (15, 79):
            e = "log2(abs(%s))%s" % (scal(0), "   /* partial precision */" if op == 79 else "")
        elif op == 16:
            e = "lit(%s.x, %s.y, %s.w)" % ((full(0, 4),) * 3)
        elif op == 17:
            e = "float4(1, %s.y * %s.y, %s.z, %s.w)" % (full(0, 4), full(1, 4), full(0, 4), full(1, 4))
        elif op == 18:
            e = "lerp(%s, %s, %s)" % (cw(2), cw(1), cw(0))
        elif op == 19:
            e = "frac(%s)" % cw(0)
        elif op in (20, 21, 22, 23, 24):
            k, rows = {20: (4, 4), 21: (4, 3), 22: (3, 4), 23: (3, 3), 24: (3, 2)}[op]
            v = full(0, k)
            parts = []
            for r in range(rows):
                rs = D.Param(S[1].token, False)
                rs.num = S[1].num + r
                rs.rel = S[1].rel
                parts.append("dot(%s, %s)" % (v, self.src(rs, list(range(k)))))
            e = "float%d(%s)" % (rows, ", ".join(parts))
            lhs = self.reg_base(d) + "." + "xyzw"[:rows] if rows < 4 else self.reg_base(d)
            comps = list(range(rows))
        elif op == 32:
            e = "pow(abs(%s), %s)" % (scal(0), scal(1))
        elif op == 33:
            e = "cross(%s, %s)" % (full(0, 3), full(1, 3))
        elif op == 34:
            e = "sign(%s)" % cw(0)
        elif op == 35:
            e = "abs(%s)" % cw(0)
        elif op == 36:
            e = "normalize(%s)" % full(0, 3)
        elif op == 37:
            e = "float2(cos(%s), sin(%s))" % (scal(0), scal(0))
        elif op == 80:
            e = "(%s > 0.5) ? %s : %s" % (self.src(S[0], [3] * w) if tex_ps1 else cw(0), cw(1), cw(2))
        elif op == 88:
            e = "(%s >= 0) ? %s : %s" % (cw(0), cw(1), cw(2))
        elif op == 90:
            e = "dot(%s, %s) + %s" % (full(0, 2), full(1, 2), scal(2))
        elif op == 89:
            e = "bem(%s, %s)" % (cw(0), cw(1))
        elif op in (91, 92):
            e = "%s(%s)" % ("ddx" if op == 91 else "ddy", cw(0))
        elif op == 66:   # tex / texld
            if sh.stage == "ps" and sh.major >= 2:
                smp = self.sampler_name(S[1].num)
                ty = self.ps_samplers.get(S[1].num, "2d").upper().replace("VOLUME", "3D")
                fn = {0: "tex%s", 1: "tex%sproj", 2: "tex%sbias"}.get(ins.controls & 3, "tex%s") % ty
                e = "%s(%s, %s)" % (fn, smp, full(0, 4))
            elif (sh.major, sh.minor) >= (1, 4):
                smp = self.sampler_name(d.num)
                e = "tex2D(%s, %s)" % (smp, full(0, 3))
            else:
                smp = self.sampler_name(d.num)
                e = "tex2D(%s, in.TEXCOORD%d)" % (smp, d.num)
        elif op == 64:   # texcoord / texcrd
            if (sh.major, sh.minor) >= (1, 4):
                e = full(0, 3)
            else:
                e = "saturate(float4(in.TEXCOORD%d.xyz, 1))" % d.num
        elif op == 95:
            smp = self.sampler_name(S[1].num)
            e = "tex2Dlod(%s, %s)" % (smp, full(0, 4))
        elif op in (67, 68):
            e = "tex2D(%s, in.TEXCOORD%d.xy + bumpEnvMat%d * %s.xy)%s" % (
                self.sampler_name(d.num), d.num, d.num, full(0, 4),
                " * luminance(%s.z)" % full(0, 4) if op == 68 else "")
        elif op in (69, 70, 82):
            sw = {69: "wx", 70: "yz", 82: "xyz"}[op]
            e = "tex2D(%s, %s.%s)" % (self.sampler_name(d.num), full(0, 4), sw)
        elif op in (71, 73):
            e = "dot(in.TEXCOORD%d.xyz, %s)   // texm3x%spad: row of matrix" % (
                d.num, full(0, 3), "2" if op == 71 else "3")
        elif op == 87:
            return "out.DEPTH = %s.x / %s.y;   // texdepth" % (self.reg_base(d), self.reg_base(d))
        elif op in (72, 74, 76, 77, 84, 86, 83, 85):
            e = "%s(%s, in.TEXCOORD%d)   // ps_1_x texture-addressing op" % (n, ", ".join(full(i, 3) for i in range(len(S))), d.num)
        else:
            e = "%s(%s)" % (n, ", ".join(cw(i) for i in range(len(S))))
        e = self._wrap(d, e)
        pred = ""
        if ins.predicated and ins.pred is not None:
            pred = "if (%s) " % self.sh.fmt_src(ins.pred)
        return "%s%s = %s;" % (pred, lhs, e)

    # ------------------------------------------------------------ whole shader
    def lift(self):
        sh = self.sh
        L = []
        L.append("// %s %s  (lifted pseudocode; register-faithful, not verbatim HLSL)" % (sh.profile, self.label))
        if sh.ctab:
            L.append("// uniforms (CTAB):")
            for c in sorted(sh.ctab["constants"], key=lambda c: (c["regset"], c["reg"])):
                t = c["type"]
                ts = t["type"]
                if t["class"] == "vector":
                    ts += str(t["columns"])
                elif t["class"] in ("matrix_rows", "matrix_columns"):
                    ts += "%dx%d" % (t["rows"], t["columns"])
                arr = "[%d]" % t["elements"] if t["elements"] > 1 else ""
                pref = {"float4": "c", "int4": "i", "bool": "b", "sampler": "s"}[c["regset"]]
                L.append("uniform %s %s%s : register(%s%d);%s" % (
                    ts, c["name"], arr, pref, c["reg"],
                    "   // %d regs" % c["count"] if c["count"] > 1 else ""))
        if sh.preshader is not None and sh.preshader.instructions:
            L.append("")
            L.append("// CPU preshader: computes shader constants c# from effect parameters")
            for line in D.preshader_pseudo(sh.preshader, "c"):
                L.append("cpu: " + line)
        L.append("")
        if self.inputs or sh.stage == "vs":
            L.append("// inputs: " + ", ".join("v%d=%s" % (k, v) for k, v in sorted(self.inputs.items())))
        L.append("main {")
        for ins in sh.instructions:
            s = self.lift_ins(ins)
            if s:
                L.append("    " + s)
        if sh.stage == "ps" and sh.major == 1:
            L.append("    out.COLOR0 = r0;   // ps_1_x: r0 is the output color")
        L.append("}")
        return "\n".join(L) + "\n"


def lift(sh, label=""):
    return Lifter(sh, label).lift()


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    print(lift(D.parse(open(argv[1], "rb").read()), os.path.basename(argv[1])))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
