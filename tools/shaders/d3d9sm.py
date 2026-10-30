#!/usr/bin/env python3
"""d3d9sm.py - Direct3D 9 shader bytecode (vs_1_1..vs_3_0, ps_1_1..ps_3_0) disassembler,
CTAB constant-table reader and D3DX preshader (FXLC) disassembler.

Written from the documented D3D9 token format (D3DSHADER_INSTRUCTION_OPCODE_TYPE,
D3DSHADER_PARAM_REGISTER_TYPE, D3DSHADER_PARAM_*_MASK in d3d9types.h and the
D3DXSHADER_CONSTANTTABLE layout in d3dx9shader.h).

API
    sh = parse(blob)            -> Shader (instructions, ctab, preshader, unknown opcodes)
    text = sh.asm()             -> standard asm text
    fx = parse_fxlc(blob)       -> Preshader (for effect state expressions)

CLI
    python -I d3d9sm.py blob.bin
"""
import struct
import sys

OPCODES = {
    0: "nop", 1: "mov", 2: "add", 3: "sub", 4: "mad", 5: "mul", 6: "rcp", 7: "rsq", 8: "dp3",
    9: "dp4", 10: "min", 11: "max", 12: "slt", 13: "sge", 14: "exp", 15: "log", 16: "lit",
    17: "dst", 18: "lrp", 19: "frc", 20: "m4x4", 21: "m4x3", 22: "m3x4", 23: "m3x3", 24: "m3x2",
    25: "call", 26: "callnz", 27: "loop", 28: "ret", 29: "endloop", 30: "label", 31: "dcl",
    32: "pow", 33: "crs", 34: "sgn", 35: "abs", 36: "nrm", 37: "sincos", 38: "rep", 39: "endrep",
    40: "if", 41: "ifc", 42: "else", 43: "endif", 44: "break", 45: "breakc", 46: "mova",
    47: "defb", 48: "defi",
    64: "texcoord", 65: "texkill", 66: "tex", 67: "texbem", 68: "texbeml", 69: "texreg2ar",
    70: "texreg2gb", 71: "texm3x2pad", 72: "texm3x2tex", 73: "texm3x3pad", 74: "texm3x3tex",
    75: "reserved0", 76: "texm3x3spec", 77: "texm3x3vspec", 78: "expp", 79: "logp", 80: "cnd",
    81: "def", 82: "texreg2rgb", 83: "texdp3tex", 84: "texm3x2depth", 85: "texdp3", 86: "texm3x3",
    87: "texdepth", 88: "cmp", 89: "bem", 90: "dp2add", 91: "dsx", 92: "dsy", 93: "texldd",
    94: "setp", 95: "texldl", 96: "breakp",
    0xFFFD: "phase", 0xFFFE: "comment", 0xFFFF: "end",
}

# (dst count, src count) for shader model 1.x streams where there is no length field
_SM1_ARGS = {
    0: (0, 0), 1: (1, 1), 2: (1, 2), 3: (1, 2), 4: (1, 3), 5: (1, 2), 6: (1, 1), 7: (1, 1),
    8: (1, 2), 9: (1, 2), 10: (1, 2), 11: (1, 2), 12: (1, 2), 13: (1, 2), 14: (1, 1), 15: (1, 1),
    16: (1, 1), 17: (1, 2), 18: (1, 3), 19: (1, 1), 20: (1, 2), 21: (1, 2), 22: (1, 2),
    23: (1, 2), 24: (1, 2), 32: (1, 2), 33: (1, 2), 34: (1, 3), 35: (1, 1), 36: (1, 1),
    46: (1, 1),
    64: (1, 0), 65: (1, 0), 66: (1, 0), 67: (1, 1), 68: (1, 1), 69: (1, 1), 70: (1, 1),
    71: (1, 1), 72: (1, 1), 73: (1, 1), 74: (1, 1), 76: (1, 2), 77: (1, 1), 78: (1, 1),
    79: (1, 1), 80: (1, 3), 82: (1, 1), 83: (1, 1), 84: (1, 1), 85: (1, 1), 86: (1, 1),
    87: (1, 0), 88: (1, 3), 89: (1, 2), 90: (1, 3), 0xFFFD: (0, 0),
}

REG_TEMP, REG_INPUT, REG_CONST, REG_ADDR_TEX, REG_RASTOUT, REG_ATTROUT, REG_OUTPUT, \
    REG_CONSTINT, REG_COLOROUT, REG_DEPTHOUT, REG_SAMPLER, REG_CONST2, REG_CONST3, REG_CONST4, \
    REG_CONSTBOOL, REG_LOOP, REG_TEMPF16, REG_MISC, REG_LABEL, REG_PREDICATE = range(20)

USAGE = ["position", "blendweight", "blendindices", "normal", "psize", "texcoord", "tangent",
         "binormal", "tessfactor", "positiont", "color", "fog", "depth", "sample"]
SAMPLER_TEX = {0: "", 1: "", 2: "2d", 3: "cube", 4: "volume"}
SRCMOD = {0: ("", ""), 1: ("-", ""), 2: ("", "_bias"), 3: ("-", "_bias"), 4: ("", "_bx2"),
          5: ("-", "_bx2"), 6: ("1-", ""), 7: ("", "_x2"), 8: ("-", "_x2"), 9: ("", "_dz"),
          10: ("", "_dw"), 11: ("", "_abs"), 12: ("-", "_abs"), 13: ("!", "")}
SHIFT = {0: "", 1: "_x2", 2: "_x4", 3: "_x8", 15: "_d2", 14: "_d4", 13: "_d8"}
CMPS = {1: "_gt", 2: "_eq", 3: "_ge", 4: "_lt", 5: "_ne", 6: "_le"}
COMP = "xyzw"

REGSET = {0: "bool", 1: "int4", 2: "float4", 3: "sampler"}
CT_CLASS = {0: "scalar", 1: "vector", 2: "matrix_rows", 3: "matrix_columns", 4: "object",
            5: "struct"}
CT_TYPE = {0: "void", 1: "bool", 2: "int", 3: "float", 4: "string", 5: "texture",
           6: "texture1d", 7: "texture2d", 8: "texture3d", 9: "texturecube", 10: "sampler",
           11: "sampler1d", 12: "sampler2d", 13: "sampler3d", 14: "samplercube",
           15: "pixelshader", 16: "vertexshader"}


class ShaderError(Exception):
    pass


def _cstr(b, off):
    if off >= len(b):
        return ""
    e = b.find(b"\0", off)
    return b[off: e if e >= 0 else len(b)].decode("latin-1")


# ------------------------------------------------------------------ CTAB
def parse_ctab(b):
    """b = comment payload after the 'CTAB' fourcc."""
    size, creator, version, n, cinfo, flags, target = struct.unpack_from("<7I", b, 0)
    ct = {"creator": _cstr(b, creator), "version": version, "flags": flags,
          "target": _cstr(b, target), "constants": []}

    def typeinfo(off):
        cls, ty, rows, cols, elems, nmem, memoff = struct.unpack_from("<6HI", b, off)
        t = {"class": CT_CLASS.get(cls, cls), "type": CT_TYPE.get(ty, ty), "rows": rows,
             "columns": cols, "elements": elems}
        if nmem:
            mems = []
            for i in range(nmem):
                mn, mt = struct.unpack_from("<II", b, memoff + 8 * i)
                mems.append({"name": _cstr(b, mn), "type": typeinfo(mt)})
            t["members"] = mems
        return t

    for i in range(n):
        name, rset, ridx, rcnt, _res, tinfo, dval = struct.unpack_from("<I4HII", b, cinfo + 20 * i)
        c = {"name": _cstr(b, name), "regset": REGSET.get(rset, rset), "reg": ridx,
             "count": rcnt, "type": typeinfo(tinfo)}
        if dval:
            t = c["type"]
            nfl = max(1, t["rows"] * t["columns"] * max(1, t["elements"]))
            nfl = min(nfl, (len(b) - dval) // 4)
            c["default"] = list(struct.unpack_from("<%df" % nfl, b, dval))
        ct["constants"].append(c)
    return ct


def ctab_lookup(ctab, regset, reg):
    """Return (constant, offset_in_registers) for register `reg` of `regset`."""
    if not ctab:
        return None, 0
    for c in ctab["constants"]:
        if c["regset"] == regset and c["reg"] <= reg < c["reg"] + max(1, c["count"]):
            return c, reg - c["reg"]
    return None, 0


# ------------------------------------------------------------------ preshader (FXLC)
FXLC_OPS = {
    0x000: "nop", 0x100: "mov", 0x101: "neg", 0x103: "rcp", 0x104: "frc", 0x105: "exp",
    0x106: "log", 0x107: "rsq", 0x108: "sin", 0x109: "cos", 0x10A: "asin", 0x10B: "acos",
    0x10C: "atan", 0x112: "sqrt", 0x120: "ineg", 0x121: "not", 0x130: "itof", 0x131: "utof",
    0x133: "ftou", 0x137: "ftob", 0x139: "floor", 0x13A: "ceil", 0x200: "min", 0x201: "max",
    0x202: "lt", 0x203: "ge", 0x204: "add", 0x205: "mul", 0x206: "atan2", 0x208: "div",
    0x210: "bilt", 0x211: "bige", 0x212: "bieq", 0x213: "bine", 0x214: "buge", 0x215: "bult",
    0x216: "iadd", 0x219: "imul", 0x21A: "udiv", 0x21D: "imin", 0x21E: "imax", 0x21F: "umin",
    0x220: "umax", 0x230: "and", 0x231: "or", 0x233: "xor", 0x234: "ishl", 0x235: "ishr",
    0x236: "ushr", 0x300: "cmp", 0x301: "movc", 0x500: "dot", 0x502: "noise",
    0x70E: "dotswiz", 0x711: "noiseswiz",
}
# ops whose destination is a single scalar regardless of the component count
FXLC_SCALAR_OUT = {0x500, 0x70E}
FXLC_TABLES = {1: "imm", 2: "c", 3: "i", 4: "out", 5: "ob", 6: "oi", 7: "r"}


class Preshader:
    def __init__(self):
        self.version = 0
        self.literals = []
        self.ctab = None
        self.instructions = []      # dicts
        self.prsi = None
        self.unknown_ops = set()

    def _opnd(self, o, ncomp, out_name="out"):
        tab, off = o["table"], o["offset"]
        if tab == 1:
            vals = self.literals[off: off + ncomp]
            return "(" + ", ".join("%.9g" % v for v in vals) + ")"
        name = FXLC_TABLES.get(tab, "t%d" % tab)
        if tab == 4:
            name = out_name
        reg, c0 = divmod(off, 4)
        sw = "".join(COMP[(c0 + i) % 4] for i in range(ncomp))
        s = "%s%d.%s" % (name, reg, sw)
        if o.get("index") is not None:
            it, io = o["index"]
            s = "%s[%s%d.%s + %d].%s" % (name, FXLC_TABLES.get(it, "t%d" % it), io // 4,
                                         COMP[io % 4], reg, sw)
        return s

    def asm(self, out_name="out", indent=""):
        lines = []
        if self.ctab:
            for c in self.ctab["constants"]:
                lines.append("%s// in  c%d..c%d = %s" % (indent, c["reg"], c["reg"] + c["count"] - 1,
                                                       c["name"]))
        for ins in self.instructions:
            n = ins["comps"]
            ins_ops = []
            for i, o in enumerate(ins["inputs"]):
                k = 1 if (ins["scalar"] and i == 0) or ins["op"] == 0x70E else n
                ins_ops.append(self._opnd(o, k, out_name))
            nout = 1 if ins["op"] in FXLC_SCALAR_OUT else n
            lines.append("%s%s %s, %s" % (indent, ins["name"], self._opnd(ins["output"], nout, out_name),
                                         ", ".join(ins_ops)))
        return "\n".join(lines)


def _parse_fxlc_payload(ps, b):
    w = struct.unpack_from("<%dI" % (len(b) // 4), b)
    n = w[0]
    p = 1
    for _ in range(n):
        code, nargs = w[p], w[p + 1]
        p += 2
        op = (code >> 20) & 0x7FF
        comps = code & 0xFFFF
        scalar = bool(code & 0x80000000)
        name = FXLC_OPS.get(op)
        if name is None:
            ps.unknown_ops.add(op)
            name = "op_0x%x" % op
        opnds = []
        for _a in range(nargs + 1):
            has_idx = w[p]
            if has_idx:
                o = {"index": (w[p + 1], w[p + 2]), "table": w[p + 3], "offset": w[p + 4]}
                p += 5
            else:
                o = {"index": None, "table": w[p + 1], "offset": w[p + 2]}
                p += 3
            opnds.append(o)
        ps.instructions.append({"op": op, "name": name, "comps": comps, "scalar": scalar,
                                "inputs": opnds[:-1], "output": opnds[-1]})


def parse_fxlc(b):
    """Parse a preshader token stream (0x4658xxxx version token ... 0x0000FFFF)."""
    w = struct.unpack_from("<%dI" % (len(b) // 4), b)
    if not w or (w[0] >> 16) != 0x4658:
        raise ShaderError("not a preshader (0x%08x)" % w[0])
    ps = Preshader()
    ps.version = w[0]
    p = 1
    while p < len(w):
        t = w[p]
        if t == 0xFFFF:
            break
        if (t & 0xFFFF) == 0xFFFE:
            ln = (t >> 16) & 0x7FFF
            body = b[4 * (p + 1): 4 * (p + 1 + ln)]
            fourcc = body[:4]
            if fourcc == b"CTAB":
                ps.ctab = parse_ctab(body[4:])
            elif fourcc == b"CLIT":
                cnt = struct.unpack_from("<I", body, 4)[0]
                ps.literals = list(struct.unpack_from("<%dd" % cnt, body, 8))
            elif fourcc == b"FXLC":
                _parse_fxlc_payload(ps, body[4:])
            elif fourcc == b"PRSI":
                ps.prsi = list(struct.unpack_from("<%dI" % ((len(body) - 4) // 4), body, 4))
            p += 1 + ln
        else:
            raise ShaderError("unexpected token 0x%08x in preshader" % t)
    return ps


def preshader_pseudo(ps, out_prefix="c"):
    """Readable statements for a preshader / FXLC expression. Inputs are named from the
    preshader's own CTAB; outputs are `<out_prefix>N.comp` (shader constants for a
    shader preshader, `expr` for an effect state expression)."""
    names = {}
    if ps.ctab:
        for c in ps.ctab["constants"]:
            for k in range(max(1, c["count"])):
                names[c["reg"] + k] = c["name"] + ("[%d]" % k if c["count"] > 1 else "")
    lines = []
    for ins in ps.instructions:
        n = ins["comps"]

        def opnd(o, k):
            reg, c0 = divmod(o["offset"], 4)
            sw = "".join(COMP[(c0 + i) % 4] for i in range(k))
            if o["table"] == 2 and o.get("index") is None:
                return "%s.%s" % (names.get(reg, "pc%d" % reg), sw)
            if o["table"] == 4:
                return "%s%d.%s" % (out_prefix, reg, sw)
            return ps._opnd(o, k, out_prefix)
        ins_ops = [opnd(o, 1 if (ins["scalar"] and i == 0) or ins["op"] == 0x70E else n)
                   for i, o in enumerate(ins["inputs"])]
        nout = 1 if ins["op"] in FXLC_SCALAR_OUT else n
        outs = opnd(ins["output"], nout)
        nm = ins["name"]
        bin_ops = {"add": "+", "mul": "*", "div": "/"}
        if nm in bin_ops and len(ins_ops) == 2:
            ex = "%s %s %s" % (ins_ops[0], bin_ops[nm], ins_ops[1])
        elif nm == "mov":
            ex = ins_ops[0]
        elif nm == "neg":
            ex = "-" + ins_ops[0]
        elif nm == "lt" and len(ins_ops) == 2:
            ex = "(%s < %s) ? 1 : 0" % tuple(ins_ops)
        elif nm == "ge" and len(ins_ops) == 2:
            ex = "(%s >= %s) ? 1 : 0" % tuple(ins_ops)
        elif nm == "rcp":
            ex = "1 / " + ins_ops[0]
        elif nm == "cmp" and len(ins_ops) == 3:
            ex = "(%s >= 0) ? %s : %s" % tuple(ins_ops)
        elif nm == "movc" and len(ins_ops) == 3:
            ex = "%s ? %s : %s" % tuple(ins_ops)
        else:
            ex = "%s(%s)" % (nm, ", ".join(ins_ops))
        lines.append("%s = %s;" % (outs, ex))
    return lines


# ------------------------------------------------------------------ shader bytecode
class Param:
    __slots__ = ("type", "num", "rel", "mask", "swz", "mod", "resmod", "shift", "is_dst",
                 "token")

    def __init__(self, tok, is_dst):
        self.token = tok
        self.type = ((tok >> 28) & 7) | ((tok >> 8) & 0x18)
        self.num = tok & 0x7FF
        self.rel = None
        self.is_dst = is_dst
        if is_dst:
            self.mask = (tok >> 16) & 0xF
            self.resmod = (tok >> 20) & 0xF
            self.shift = (tok >> 24) & 0xF
            self.swz = 0xE4
            self.mod = 0
        else:
            self.swz = (tok >> 16) & 0xFF
            self.mod = (tok >> 24) & 0xF
            self.mask = 0xF
            self.resmod = 0
            self.shift = 0


class Instruction:
    __slots__ = ("opcode", "name", "controls", "coissue", "predicated", "dst", "src", "pred",
                 "extra", "offset", "tokens")

    def __init__(self):
        self.dst = []
        self.src = []
        self.pred = None
        self.extra = None
        self.coissue = False
        self.predicated = False
        self.controls = 0


class Shader:
    def __init__(self, blob):
        self.blob = blob
        n = len(blob) // 4
        self.w = struct.unpack_from("<%dI" % n, blob)
        v = self.w[0]
        kind = v >> 16
        if kind == 0xFFFE:
            self.stage = "vs"
        elif kind == 0xFFFF:
            self.stage = "ps"
        else:
            raise ShaderError("bad version token 0x%08x" % v)
        self.major = (v >> 8) & 0xFF
        self.minor = v & 0xFF
        self.instructions = []
        self.comments = []
        self.ctab = None
        self.preshader = None
        self.unknown_opcodes = {}
        self.ended = False
        self.end_token_index = None
        self._parse()

    @property
    def profile(self):
        return "%s_%d_%d" % (self.stage, self.major, self.minor)

    def _has_length(self):
        return self.major >= 2

    def _parse(self):
        w = self.w
        p = 1
        while p < len(w):
            tok = w[p]
            op = tok & 0xFFFF
            if op == 0xFFFE:
                ln = (tok >> 16) & 0x7FFF
                body = self.blob[4 * (p + 1): 4 * (p + 1 + ln)]
                self.comments.append((body[:4], body))
                if body[:4] == b"CTAB":
                    self.ctab = parse_ctab(body[4:])
                elif body[:4] == b"PRES" and len(body) >= 12:
                    try:
                        self.preshader = parse_fxlc(body[4:])
                    except ShaderError as e:
                        self.comments.append((b"ERR!", str(e).encode()))
                p += 1 + ln
                continue
            if op == 0xFFFF:
                self.ended = True
                self.end_token_index = p
                p += 1
                break
            ins = Instruction()
            ins.offset = p
            ins.opcode = op
            ins.name = OPCODES.get(op)
            ins.controls = (tok >> 16) & 0xFF
            ins.coissue = bool(tok & (1 << 30))
            ins.predicated = bool(tok & (1 << 28))
            if ins.name is None:
                self.unknown_opcodes[op] = self.unknown_opcodes.get(op, 0) + 1
                ins.name = "op_%d" % op
            p += 1
            if self._has_length():
                ln = (tok >> 24) & 0xF
                end = p + ln
                self._decode_args(ins, p, end)
                p = end
            else:
                p = self._decode_sm1(ins, p)
            self.instructions.append(ins)
        self.trailing = len(w) - p

    def _read_src(self, p, end):
        s = Param(self.w[p], False)
        p += 1
        if self.w[p - 1] & (1 << 13):
            if self.major >= 2 and p < end:
                s.rel = Param(self.w[p], False)
                p += 1
            else:
                s.rel = "a0.x"
        return s, p

    def _read_dst(self, p):
        d = Param(self.w[p], True)
        p += 1
        if self.w[p - 1] & (1 << 13) and self.major >= 3:
            d.rel = Param(self.w[p], False)
            p += 1
        return d, p

    def _decode_args(self, ins, p, end):
        op = ins.opcode
        w = self.w
        if op == 31:   # dcl
            ins.extra = w[p]
            ins.dst = [Param(w[p + 1], True)]
            return
        if op == 81:   # def
            ins.dst = [Param(w[p], True)]
            ins.extra = list(struct.unpack("<4f", struct.pack("<4I", *w[p + 1: p + 5])))
            return
        if op == 48:   # defi
            ins.dst = [Param(w[p], True)]
            ins.extra = list(struct.unpack("<4i", struct.pack("<4I", *w[p + 1: p + 5])))
            return
        if op == 47:   # defb
            ins.dst = [Param(w[p], True)]
            ins.extra = w[p + 1]
            return
        ndst = 1
        if op in (0, 25, 26, 27, 28, 29, 30, 38, 39, 40, 41, 42, 43, 44, 45, 96, 0xFFFD):
            ndst = 0
        if p < end and ndst:
            d, p = self._read_dst(p)
            ins.dst = [d]
        if ins.predicated and p < end:
            ins.pred, p = self._read_src(p, end)
        while p < end:
            s, p = self._read_src(p, end)
            ins.src.append(s)

    def _decode_sm1(self, ins, p):
        op = ins.opcode
        w = self.w
        ln = (w[ins.offset] >> 24) & 0xF
        if op == 31:
            ins.extra = w[p]
            ins.dst = [Param(w[p + 1], True)]
            return p + 2
        if op == 81:
            ins.dst = [Param(w[p], True)]
            ins.extra = list(struct.unpack("<4f", struct.pack("<4I", *w[p + 1: p + 5])))
            return p + 5
        if self.stage == "ps" and op in (64, 66) and (self.major, self.minor) >= (1, 4):
            nd, ns = 1, 1
        elif op in _SM1_ARGS:
            nd, ns = _SM1_ARGS[op]
        elif ln:
            end = p + ln
            self._decode_args(ins, p, end)
            return end
        else:
            nd, ns = 0, 0
        if nd:
            d, p = self._read_dst(p)
            ins.dst = [d]
        for _ in range(ns):
            s, p = self._read_src(p, len(w))
            ins.src.append(s)
        return p

    # -------------------------------------------------------------- formatting
    def reg_name(self, rtype, num):
        ps = self.stage == "ps"
        if rtype == REG_TEMP:
            return "r%d" % num
        if rtype == REG_INPUT:
            return "v%d" % num
        if rtype == REG_CONST:
            return "c%d" % num
        if rtype == REG_ADDR_TEX:
            return ("t%d" if ps else "a%d") % num
        if rtype == REG_RASTOUT:
            return ["oPos", "oFog", "oPts"][num] if num < 3 else "oRast%d" % num
        if rtype == REG_ATTROUT:
            return "oD%d" % num
        if rtype == REG_OUTPUT:
            return ("o%d" if (self.major >= 3 and not ps) else "oT%d") % num
        if rtype == REG_CONSTINT:
            return "i%d" % num
        if rtype == REG_COLOROUT:
            return "oC%d" % num
        if rtype == REG_DEPTHOUT:
            return "oDepth"
        if rtype == REG_SAMPLER:
            return "s%d" % num
        if rtype == REG_CONST2:
            return "c%d" % (num + 2048)
        if rtype == REG_CONST3:
            return "c%d" % (num + 4096)
        if rtype == REG_CONST4:
            return "c%d" % (num + 6144)
        if rtype == REG_CONSTBOOL:
            return "b%d" % num
        if rtype == REG_LOOP:
            return "aL"
        if rtype == REG_TEMPF16:
            return "h%d" % num
        if rtype == REG_MISC:
            return ["vPos", "vFace"][num] if num < 2 else "misc%d" % num
        if rtype == REG_LABEL:
            return "l%d" % num
        if rtype == REG_PREDICATE:
            return "p%d" % num
        return "reg%d_%d" % (rtype, num)

    @staticmethod
    def swz_str(swz):
        comps = [COMP[(swz >> (2 * i)) & 3] for i in range(4)]
        if swz == 0xE4:
            return ""
        if len(set(comps)) == 1:
            return "." + comps[0]
        return "." + "".join(comps)

    @staticmethod
    def mask_str(mask):
        if mask == 0xF or mask == 0:
            return ""
        return "." + "".join(COMP[i] for i in range(4) if mask & (1 << i))

    def fmt_src(self, s):
        if isinstance(s, str):
            return s
        name = self.reg_name(s.type, s.num)
        if s.rel is not None:
            rel = s.rel if isinstance(s.rel, str) else (
                self.reg_name(s.rel.type, s.rel.num) + self.swz_str(s.rel.swz))
            base = name[0] if s.type in (REG_CONST, REG_INPUT, REG_OUTPUT) else name.rstrip("0123456789")
            name = "%s[%s + %d]" % (base, rel, s.num) if s.num else "%s[%s]" % (base, rel)
        pre, post = SRCMOD.get(s.mod, ("?", "_mod%d" % s.mod))
        return "%s%s%s%s" % (pre, name, post, self.swz_str(s.swz))

    def fmt_dst(self, d):
        name = self.reg_name(d.type, d.num)
        if d.rel is not None:
            name = "%s[%s + %d]" % (name[0], self.fmt_src(d.rel), d.num)
        return name + self.mask_str(d.mask)

    def mnemonic(self, ins):
        n = ins.name
        ps = self.stage == "ps"
        if ps and ins.opcode == 66:
            if (self.major, self.minor) >= (1, 4):
                n = "texld"
                if self.major >= 2:
                    n = {0: "texld", 1: "texldp", 2: "texldb"}.get(ins.controls & 3, "texld")
        if ps and ins.opcode == 64 and (self.major, self.minor) >= (1, 4):
            n = "texcrd"
        if ins.opcode in (41, 45, 94):
            n += CMPS.get(ins.controls & 7, "_c%d" % ins.controls)
        if ins.dst:
            d = ins.dst[0]
            if d.resmod & 1:
                n += "_sat"
            if d.resmod & 2:
                n += "_pp"
            if d.resmod & 4:
                n += "_centroid"
            n += SHIFT.get(d.shift, "_shift%d" % d.shift)
        return n

    def fmt_ins(self, ins):
        if ins.opcode == 31:
            d = ins.dst[0]
            tok = ins.extra
            if d.type == REG_SAMPLER:
                tt = SAMPLER_TEX.get((tok >> 27) & 0xF, "tt%d" % ((tok >> 27) & 0xF))
                return "dcl_%s %s" % (tt, self.fmt_dst(d)) if tt else "dcl %s" % self.fmt_dst(d)
            if self.stage == "vs" or self.major >= 3:
                u = tok & 0x1F
                ui = (tok >> 16) & 0xF
                us = USAGE[u] if u < len(USAGE) else "usage%d" % u
                return "dcl_%s%s %s" % (us, ui if ui else "", self.fmt_dst(d))
            if d.resmod & 4:
                return "dcl_centroid %s" % self.fmt_dst(d)
            return "dcl %s" % self.fmt_dst(d)
        if ins.opcode == 81:
            return "def %s, %s" % (self.fmt_dst(ins.dst[0]), ", ".join("%.9g" % f for f in ins.extra))
        if ins.opcode == 48:
            return "defi %s, %s" % (self.fmt_dst(ins.dst[0]), ", ".join(str(i) for i in ins.extra))
        if ins.opcode == 47:
            return "defb %s, %s" % (self.fmt_dst(ins.dst[0]), "true" if ins.extra else "false")
        ops = [self.fmt_dst(d) for d in ins.dst] + [self.fmt_src(s) for s in ins.src]
        s = self.mnemonic(ins)
        if ops:
            s += " " + ", ".join(ops)
        if ins.predicated and ins.pred is not None:
            s = "(%s) %s" % (self.fmt_src(ins.pred), s)
        if ins.coissue:
            s = "+" + s
        return s

    def asm(self, with_ctab=True):
        out = []
        if with_ctab and self.ctab:
            out.append("//")
            out.append("// Generated by %s" % self.ctab["creator"])
            out.append("//")
            out.append("// Parameters / registers:")
            for c in sorted(self.ctab["constants"], key=lambda c: (c["regset"], c["reg"])):
                t = c["type"]
                pref = {"float4": "c", "int4": "i", "bool": "b", "sampler": "s"}.get(c["regset"], "?")
                ts = t["type"]
                if t["class"] == "vector":
                    ts += str(t["columns"])
                elif t["class"] in ("matrix_rows", "matrix_columns"):
                    ts += "%dx%d" % (t["rows"], t["columns"])
                if t["elements"] > 1:
                    ts += "[%d]" % t["elements"]
                out.append("//   %-28s %-16s %s%d%s" % (c["name"], ts, pref, c["reg"],
                                                       ("-%s%d" % (pref, c["reg"] + c["count"] - 1)) if c["count"] > 1 else ""))
            out.append("//")
        if self.preshader is not None:
            out.append("// preshader:")
            out.append(self.preshader.asm(out_name="c", indent="//   "))
            out.append("//")
        out.append(self.profile)
        for ins in self.instructions:
            out.append("    " + self.fmt_ins(ins))
        out.append("// %d instruction slots used" % self.instruction_count())
        return "\n".join(out) + "\n"

    def instruction_count(self):
        """Arithmetic+texture instruction count (excludes dcl/def/comments), like fxc's count."""
        return sum(1 for i in self.instructions if i.opcode not in (31, 81, 47, 48, 0xFFFD))


def parse(blob):
    return Shader(blob)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    data = open(argv[1], "rb").read()
    if struct.unpack_from("<I", data)[0] >> 16 == 0x4658:
        print(parse_fxlc(data).asm())
    else:
        print(parse(data).asm())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
