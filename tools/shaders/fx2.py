#!/usr/bin/env python3
"""fx2.py - parser for compiled D3DX9 Effect binaries (fx_2_0, tag 0xFEFF0901).

Clean-room reader written from the publicly documented behaviour of
ID3DXEffect / D3DXCreateEffect (layout as implemented by Wine's d3dx9 and
vkd3d-shader's fx.c). Recovers:

  * parameters: name, semantic, type, class, rows/cols, elements, struct
    members, flags, annotations, default values (incl. sampler states)
  * techniques -> passes -> states (render / texture-stage / sampler /
    shader / transform / light / material ...), with each state's value
    resolved to a constant, a parameter reference, an FXLC expression or an
    embedded shader blob
  * the object table (strings, shader bytecode blobs)

Usage as a library:   eff = fx2.parse(open(path,'rb').read())
Usage from CLI:       python -I fx2.py file.fxo [--json out.json]

Layout summary (all little-endian u32):
  0x00  tag 0xFEFF0901
  0x04  size of the "base" block that starts at 0x08; every offset inside
        typedefs / values / names is relative to 0x08
  [0x08 + size]:
        u32 parameter_count, technique_count, unused, object_count
        parameter[parameter_count]:
            u32 typedef_off, value_off, flags, annotation_count
            annotation[annotation_count]: u32 typedef_off, value_off
        technique[technique_count]:
            u32 name_off, annotation_count, pass_count
            annotation[...]
            pass[pass_count]:
                u32 name_off, annotation_count, state_count
                annotation[...]
                state[state_count]: u32 operation, index, typedef_off, value_off
        u32 string_count, resource_count
        string[string_count]: u32 object_id, u32 size, bytes (padded to 4)
        resource[resource_count]:
            u32 technique_index (0xFFFFFFFF = parameter), index,
                element_index, state_index, usage, size, bytes (padded)
  typedef: u32 type, class, name_off, semantic_off, element_count, then
           (columns, rows) for numeric classes, or member_count + member
           typedefs for structs.
  name:    u32 length (incl. NUL) + chars.
"""
import json
import struct
import sys

TAG_FX20 = 0xFEFF0901

PT = ["void", "bool", "int", "float", "string", "texture", "texture1d", "texture2d",
      "texture3d", "texturecube", "sampler", "sampler1d", "sampler2d", "sampler3d",
      "samplercube", "pixelshader", "vertexshader", "pixelfragment", "vertexfragment",
      "unsupported"]
PC = ["scalar", "vector", "matrix_rows", "matrix_columns", "object", "struct"]
SAMPLER_TYPES = {10, 11, 12, 13, 14}
TEXTURE_TYPES = {5, 6, 7, 8, 9}
SHADER_TYPES = {15, 16}
OBJ_ID_TYPES = {4, 5, 6, 7, 8, 9, 15, 16, 17, 18}

PARAM_FLAGS = {1: "shared", 2: "literal", 4: "annotation"}

# ---------------------------------------------------------------- state table
# index == the "operation" field of a state; (class, name, d3d enum value)
_RS = ["ZENABLE", "FILLMODE", "SHADEMODE", "ZWRITEENABLE", "ALPHATESTENABLE", "LASTPIXEL",
       "SRCBLEND", "DESTBLEND", "CULLMODE", "ZFUNC", "ALPHAREF", "ALPHAFUNC", "DITHERENABLE",
       "ALPHABLENDENABLE", "FOGENABLE", "SPECULARENABLE", "FOGCOLOR", "FOGTABLEMODE",
       "FOGSTART", "FOGEND", "FOGDENSITY", "RANGEFOGENABLE", "STENCILENABLE", "STENCILFAIL",
       "STENCILZFAIL", "STENCILPASS", "STENCILFUNC", "STENCILREF", "STENCILMASK",
       "STENCILWRITEMASK", "TEXTUREFACTOR"] + ["WRAP%d" % i for i in range(16)] + [
       "CLIPPING", "LIGHTING", "AMBIENT", "FOGVERTEXMODE", "COLORVERTEX", "LOCALVIEWER",
       "NORMALIZENORMALS", "DIFFUSEMATERIALSOURCE", "SPECULARMATERIALSOURCE",
       "AMBIENTMATERIALSOURCE", "EMISSIVEMATERIALSOURCE", "VERTEXBLEND", "CLIPPLANEENABLE",
       "POINTSIZE", "POINTSIZE_MIN", "POINTSIZE_MAX", "POINTSPRITEENABLE", "POINTSCALEENABLE",
       "POINTSCALE_A", "POINTSCALE_B", "POINTSCALE_C", "MULTISAMPLEANTIALIAS",
       "MULTISAMPLEMASK", "PATCHEDGESTYLE", "DEBUGMONITORTOKEN", "INDEXEDVERTEXBLENDENABLE",
       "COLORWRITEENABLE", "TWEENFACTOR", "BLENDOP", "POSITIONDEGREE", "NORMALDEGREE",
       "SCISSORTESTENABLE", "SLOPESCALEDEPTHBIAS", "ANTIALIASEDLINEENABLE",
       "MINTESSELLATIONLEVEL", "MAXTESSELLATIONLEVEL", "ADAPTIVETESS_X", "ADAPTIVETESS_Y",
       "ADAPTIVETESS_Z", "ADAPTIVETESS_W", "ENABLEADAPTIVETESSELLATION", "TWOSIDEDSTENCILMODE",
       "CCW_STENCILFAIL", "CCW_STENCILZFAIL", "CCW_STENCILPASS", "CCW_STENCILFUNC",
       "COLORWRITEENABLE1", "COLORWRITEENABLE2", "COLORWRITEENABLE3", "BLENDFACTOR",
       "SRGBWRITEENABLE", "DEPTHBIAS", "SEPARATEALPHABLENDENABLE", "SRCBLENDALPHA",
       "DESTBLENDALPHA", "BLENDOPALPHA"]
_TSS = ["COLOROP", "COLORARG0", "COLORARG1", "COLORARG2", "ALPHAOP", "ALPHAARG0", "ALPHAARG1",
        "ALPHAARG2", "RESULTARG", "BUMPENVMAT00", "BUMPENVMAT01", "BUMPENVMAT10",
        "BUMPENVMAT11", "TEXCOORDINDEX", "BUMPENVLSCALE", "BUMPENVLOFFSET",
        "TEXTURETRANSFORMFLAGS", "CONSTANT"]
_SAMP = ["AddressU", "AddressV", "AddressW", "BorderColor", "MagFilter", "MinFilter",
         "MipFilter", "MipMapLodBias", "MaxMipLevel", "MaxAnisotropy", "SRGBTexture",
         "ElementIndex", "DMAPOffset"]

STATE_TABLE = []
for n in _RS:
    STATE_TABLE.append(("render", n))
for n in _TSS:
    STATE_TABLE.append(("texstage", n))
STATE_TABLE += [("npatch", "NPatchMode"), ("fvf", "FVF"),
                ("transform", "PROJECTION"), ("transform", "VIEW"), ("transform", "WORLD"),
                ("transform", "TEXTURE0"),
                ("material", "MaterialDiffuse"), ("material", "MaterialAmbient"),
                ("material", "MaterialSpecular"), ("material", "MaterialEmissive"),
                ("material", "MaterialPower")]
for n in ["Type", "Diffuse", "Specular", "Ambient", "Position", "Direction", "Range",
          "Falloff", "Attenuation0", "Attenuation1", "Attenuation2", "Theta", "Phi"]:
    STATE_TABLE.append(("light", "Light" + n))
STATE_TABLE += [("lightenable", "LightEnable"), ("vertexshader", "VertexShader"),
                ("pixelshader", "PixelShader")]
for n in ["VertexShaderConstantF", "VertexShaderConstantB", "VertexShaderConstantI",
          "VertexShaderConstant", "VertexShaderConstant1", "VertexShaderConstant2",
          "VertexShaderConstant3", "VertexShaderConstant4",
          "PixelShaderConstantF", "PixelShaderConstantB", "PixelShaderConstantI",
          "PixelShaderConstant", "PixelShaderConstant1", "PixelShaderConstant2",
          "PixelShaderConstant3", "PixelShaderConstant4"]:
    STATE_TABLE.append(("shaderconst", n))
STATE_TABLE.append(("texture", "Texture"))
for n in _SAMP:
    STATE_TABLE.append(("sampler", n))
STATE_TABLE.append(("setsampler", "Sampler"))
assert len(STATE_TABLE) == 0xB3, len(STATE_TABLE)

# symbolic names for common enum-valued states
_BLEND = {1: "ZERO", 2: "ONE", 3: "SRCCOLOR", 4: "INVSRCCOLOR", 5: "SRCALPHA", 6: "INVSRCALPHA",
          7: "DESTALPHA", 8: "INVDESTALPHA", 9: "DESTCOLOR", 10: "INVDESTCOLOR",
          11: "SRCALPHASAT", 12: "BOTHSRCALPHA", 13: "BOTHINVSRCALPHA", 14: "BLENDFACTOR",
          15: "INVBLENDFACTOR"}
_CMP = {1: "NEVER", 2: "LESS", 3: "EQUAL", 4: "LESSEQUAL", 5: "GREATER", 6: "NOTEQUAL",
        7: "GREATEREQUAL", 8: "ALWAYS"}
_CULL = {1: "NONE", 2: "CW", 3: "CCW"}
_BLENDOP = {1: "ADD", 2: "SUBTRACT", 3: "REVSUBTRACT", 4: "MIN", 5: "MAX"}
_STENCILOP = {1: "KEEP", 2: "ZERO", 3: "REPLACE", 4: "INCRSAT", 5: "DECRSAT", 6: "INVERT",
              7: "INCR", 8: "DECR"}
_FILL = {1: "POINT", 2: "WIREFRAME", 3: "SOLID"}
_SHADE = {1: "FLAT", 2: "GOURAUD", 3: "PHONG"}
_ZB = {0: "FALSE", 1: "TRUE", 2: "USEW"}
_FOGMODE = {0: "NONE", 1: "EXP", 2: "EXP2", 3: "LINEAR"}
_MCS = {0: "MATERIAL", 1: "COLOR1", 2: "COLOR2"}
_TOP = {1: "DISABLE", 2: "SELECTARG1", 3: "SELECTARG2", 4: "MODULATE", 5: "MODULATE2X",
        6: "MODULATE4X", 7: "ADD", 8: "ADDSIGNED", 9: "ADDSIGNED2X", 10: "SUBTRACT",
        11: "ADDSMOOTH", 12: "BLENDDIFFUSEALPHA", 13: "BLENDTEXTUREALPHA",
        14: "BLENDFACTORALPHA", 15: "BLENDTEXTUREALPHAPM", 16: "BLENDCURRENTALPHA",
        17: "PREMODULATE", 18: "MODULATEALPHA_ADDCOLOR", 19: "MODULATECOLOR_ADDALPHA",
        20: "MODULATEINVALPHA_ADDCOLOR", 21: "MODULATEINVCOLOR_ADDALPHA", 22: "BUMPENVMAP",
        23: "BUMPENVMAPLUMINANCE", 24: "DOTPRODUCT3", 25: "MULTIPLYADD", 26: "LERP"}
_TA_SRC = {0: "DIFFUSE", 1: "CURRENT", 2: "TEXTURE", 3: "TFACTOR", 4: "SPECULAR", 5: "TEMP",
           6: "CONSTANT"}
_TADDR = {1: "WRAP", 2: "MIRROR", 3: "CLAMP", 4: "BORDER", 5: "MIRRORONCE"}
_TFILT = {0: "NONE", 1: "POINT", 2: "LINEAR", 3: "ANISOTROPIC", 6: "PYRAMIDALQUAD",
          7: "GAUSSIANQUAD"}
_TTF = {0: "DISABLE", 1: "COUNT1", 2: "COUNT2", 3: "COUNT3", 4: "COUNT4", 256: "PROJECTED"}

ENUM_FOR_STATE = {
    "SRCBLEND": _BLEND, "DESTBLEND": _BLEND, "SRCBLENDALPHA": _BLEND, "DESTBLENDALPHA": _BLEND,
    "ZFUNC": _CMP, "ALPHAFUNC": _CMP, "STENCILFUNC": _CMP, "CCW_STENCILFUNC": _CMP,
    "CULLMODE": _CULL, "BLENDOP": _BLENDOP, "BLENDOPALPHA": _BLENDOP,
    "STENCILFAIL": _STENCILOP, "STENCILZFAIL": _STENCILOP, "STENCILPASS": _STENCILOP,
    "CCW_STENCILFAIL": _STENCILOP, "CCW_STENCILZFAIL": _STENCILOP, "CCW_STENCILPASS": _STENCILOP,
    "FILLMODE": _FILL, "SHADEMODE": _SHADE, "ZENABLE": _ZB, "FOGTABLEMODE": _FOGMODE,
    "FOGVERTEXMODE": _FOGMODE, "DIFFUSEMATERIALSOURCE": _MCS, "SPECULARMATERIALSOURCE": _MCS,
    "AMBIENTMATERIALSOURCE": _MCS, "EMISSIVEMATERIALSOURCE": _MCS,
    "COLOROP": _TOP, "ALPHAOP": _TOP,
    "AddressU": _TADDR, "AddressV": _TADDR, "AddressW": _TADDR,
    "MagFilter": _TFILT, "MinFilter": _TFILT, "MipFilter": _TFILT,
    "TEXTURETRANSFORMFLAGS": _TTF,
}
_TA_STATES = {"COLORARG0", "COLORARG1", "COLORARG2", "ALPHAARG0", "ALPHAARG1", "ALPHAARG2",
              "RESULTARG"}


def texarg_name(v):
    s = _TA_SRC.get(v & 0xF, "0x%x" % (v & 0xF))
    if v & 0x10:
        s += "|COMPLEMENT"
    if v & 0x20:
        s += "|ALPHAREPLICATE"
    return s


def enum_name(state_name, value):
    if not isinstance(value, int):
        return None
    if state_name in _TA_STATES:
        return texarg_name(value)
    if state_name == "COLORWRITEENABLE" or state_name.startswith("COLORWRITEENABLE"):
        return "".join(c for b, c in ((1, "R"), (2, "G"), (4, "B"), (8, "A")) if value & b) or "0"
    t = ENUM_FOR_STATE.get(state_name)
    if t:
        return t.get(value)
    return None


class FxError(Exception):
    pass


class _Reader:
    def __init__(self, data):
        self.d = data

    def u32(self, off):
        if off < 0 or off + 4 > len(self.d):
            raise FxError("read past end at 0x%x" % off)
        return struct.unpack_from("<I", self.d, off)[0]

    def name(self, off):
        n = self.u32(off)
        if n == 0:
            return ""
        raw = self.d[off + 4: off + 4 + n]
        if len(raw) != n:
            raise FxError("truncated name at 0x%x" % off)
        return raw.split(b"\0", 1)[0].decode("latin-1")


def _type_str(t):
    """HLSL-ish type spelling of a typedef dict."""
    ty = PT[t["type"]] if t["type"] < len(PT) else "type%d" % t["type"]
    cls = t["class"]
    if cls in (0, 1, 2, 3) and t["rows"] * t["columns"] == 1:
        s = ty
    elif cls == 1:
        s = "%s%d" % (ty, t["columns"])
    elif cls in (2, 3):
        s = "%s%dx%d" % (ty, t["rows"], t["columns"])
        if cls == 3:
            s = "column_major " + s
    elif cls == 5:
        s = "struct"
    else:
        s = ty
    return s


class Effect:
    def __init__(self, data):
        self.raw = data
        self.r = _Reader(data)
        if len(data) < 8 or self.r.u32(0) != TAG_FX20:
            raise FxError("not an fx_2_0 effect (tag 0x%08x)" % (self.r.u32(0) if len(data) >= 4 else 0))
        self.base_size = self.r.u32(4)
        self.base = 8
        self.objects = {}        # id -> {'kind':..., 'data': bytes}
        self.parameters = []
        self.techniques = []
        self.warnings = []
        self._parse()

    # ------------------------------------------------------------ primitives
    def _bu32(self, off):
        return self.r.u32(self.base + off)

    def _bname(self, off):
        return self.r.name(self.base + off)

    def _typedef(self, off):
        """Parse a typedef at base-relative offset; returns (dict, size_in_dwords_consumed)."""
        p = off
        t = {"type": self._bu32(p), "class": self._bu32(p + 4)}
        t["name"] = self._bname(self._bu32(p + 8))
        t["semantic"] = self._bname(self._bu32(p + 12))
        t["elements"] = self._bu32(p + 16)
        p += 20
        cls = t["class"]
        if cls in (0, 1, 2, 3):
            # stored as (columns, rows): float4 vector = cols 4, rows 1
            t["columns"] = self._bu32(p)
            t["rows"] = self._bu32(p + 4)
            p += 8
        elif cls == 5:
            cnt = self._bu32(p)
            p += 4
            members = []
            for _ in range(cnt):
                m, p = self._typedef(p)
                members.append(m)
            t["members"] = members
        elif cls == 4:
            pass
        else:
            raise FxError("bad parameter class %d" % cls)
        return t, p

    def _scalar_count(self, t):
        cls = t["class"]
        if cls in (0, 1, 2, 3):
            return t["rows"] * t["columns"]
        return None

    def _value(self, t, off, in_element=False):
        """Parse a value for typedef t at base-relative off; returns (value, new_off)."""
        if t["elements"] and not in_element:
            vals = []
            for _ in range(t["elements"]):
                v, off = self._value(t, off, True)
                vals.append(v)
            return vals, off
        cls = t["class"]
        if cls == 5:
            out = {}
            for m in t["members"]:
                v, off = self._value(m, off)
                out[m["name"]] = v
            return out, off
        if cls in (0, 1, 2, 3):
            n = t["rows"] * t["columns"]
            raw = [self._bu32(off + 4 * i) for i in range(n)]
            off += 4 * n
            ty = t["type"]
            if ty == 3:
                vals = [struct.unpack("<f", struct.pack("<I", x))[0] for x in raw]
                vals = [float("%.7g" % v) for v in vals]
            elif ty == 2:
                vals = [struct.unpack("<i", struct.pack("<I", x))[0] for x in raw]
            elif ty == 1:
                vals = [bool(x) for x in raw]
            else:
                vals = raw
            return (vals[0] if n == 1 else vals), off
        # object
        ty = t["type"]
        if ty in SAMPLER_TYPES:
            cnt = self._bu32(off)
            off += 4
            states = []
            for _ in range(cnt):
                st = self._state(off)
                off += 16
                states.append(st)
            return {"sampler_states": states}, off
        if ty in OBJ_ID_TYPES:
            oid = self._bu32(off)
            self.objects.setdefault(oid, {"kind": PT[ty]})
            return {"object": oid}, off + 4
        raise FxError("unsupported object type %d" % ty)

    def _state(self, off):
        op = self._bu32(off)
        index = self._bu32(off + 4)
        t, _ = self._typedef(self._bu32(off + 8))
        v, _ = self._value(t, self._bu32(off + 12))
        if op < len(STATE_TABLE):
            cls, nm = STATE_TABLE[op]
        else:
            cls, nm = "unknown", "state_0x%x" % op
            self.warnings.append("unknown state operation 0x%x" % op)
        st = {"op": op, "class": cls, "name": nm, "index": index,
              "type": _type_str(t), "value": v}
        return st

    def _annotations(self, off, count):
        anns = []
        for _ in range(count):
            t, _ = self._typedef(self._bu32(off))
            v, _ = self._value(t, self._bu32(off + 4))
            anns.append({"name": t["name"], "type": _type_str(t), "typedef": t, "value": v})
            off += 8
        return anns, off

    # ------------------------------------------------------------ main parse
    def _parse(self):
        r = self.r
        p = self.base + self.base_size
        npar, ntech, _unused, nobj = (r.u32(p), r.u32(p + 4), r.u32(p + 8), r.u32(p + 12))
        self.object_count = nobj
        p += 16
        for _ in range(npar):
            toff, voff, flags, nann = r.u32(p), r.u32(p + 4), r.u32(p + 8), r.u32(p + 12)
            p += 16
            anns, p2 = self._annotations(p - self.base, nann)
            p = p2 + self.base
            t, _ = self._typedef(toff)
            v, _ = self._value(t, voff)
            self.parameters.append({
                "name": t["name"], "semantic": t["semantic"], "type": _type_str(t),
                "typedef": t, "flags": flags,
                "flag_names": [n for b, n in PARAM_FLAGS.items() if flags & b],
                "annotations": anns, "value": v})
        for _ in range(ntech):
            name = self._bname(r.u32(p))
            nann, npass = r.u32(p + 4), r.u32(p + 8)
            p += 12
            anns, p2 = self._annotations(p - self.base, nann)
            p = p2 + self.base
            passes = []
            for _ in range(npass):
                pname = self._bname(r.u32(p))
                pann, nst = r.u32(p + 4), r.u32(p + 8)
                p += 12
                panns, p2 = self._annotations(p - self.base, pann)
                p = p2 + self.base
                states = []
                for _ in range(nst):
                    states.append(self._state(p - self.base))
                    p += 16
                passes.append({"name": pname, "annotations": panns, "states": states})
            self.techniques.append({"name": name, "annotations": anns, "passes": passes})
        nstr, nres = r.u32(p), r.u32(p + 4)
        p += 8
        for _ in range(nstr):
            oid, size = r.u32(p), r.u32(p + 4)
            data = bytes(self.raw[p + 8: p + 8 + size])
            p += 8 + ((size + 3) & ~3)
            o = self.objects.setdefault(oid, {"kind": "?"})
            o["data"] = data
        self.resources = []
        for _ in range(nres):
            ti, idx, ei, si, usage, size = (r.u32(p + 4 * i) for i in range(6))
            data = bytes(self.raw[p + 24: p + 24 + size])
            p += 24 + ((size + 3) & ~3)
            self._apply_resource(ti, idx, ei, si, usage, data)
        if p != len(self.raw):
            self.warnings.append("trailing bytes: parsed to 0x%x of 0x%x" % (p, len(self.raw)))
        # resolve object payloads into parameter/annotation values
        for o in self.objects.values():
            if o.get("kind") == "string" and "data" in o:
                o["text"] = o["data"].split(b"\0", 1)[0].decode("latin-1")

    def _locate_state(self, ti, idx, ei, si):
        if ti == 0xFFFFFFFF:
            par = self.parameters[idx]
            v = par["value"]
            if isinstance(v, list):
                v = v[ei if ei != 0xFFFFFFFF else 0]
            return v["sampler_states"][si], ("param", par["name"], ei, si)
        tech = self.techniques[ti]
        return tech["passes"][idx]["states"][si], ("pass", tech["name"], idx, si)

    def _apply_resource(self, ti, idx, ei, si, usage, data):
        st, where = self._locate_state(ti, idx, ei, si)
        if usage == 0:
            if data[:4] == b"" :
                st["resolved"] = {"kind": "empty"}
            elif st["type"] in ("vertexshader", "pixelshader"):
                st["resolved"] = {"kind": "shader", "data": data}
            else:
                st["resolved"] = {"kind": "fxlc", "data": data}
        elif usage == 1:
            st["resolved"] = {"kind": "parameter",
                              "name": data.split(b"\0", 1)[0].decode("latin-1")}
        elif usage == 2:
            n = struct.unpack_from("<I", data, 0)[0]
            nm = data[4:4 + n].split(b"\0", 1)[0].decode("latin-1")
            rest = data[4 + ((n + 3) & ~3):]
            st["resolved"] = {"kind": "array_selector", "name": nm, "data": rest}
        else:
            st["resolved"] = {"kind": "usage%d" % usage, "data": data}
            self.warnings.append("unknown resource usage %d" % usage)
        self.resources.append({"where": where, "usage": usage, "size": len(data)})

    # ------------------------------------------------------------ helpers
    def string(self, oid):
        o = self.objects.get(oid, {})
        return o.get("text")

    def value_repr(self, v):
        if isinstance(v, dict) and "object" in v:
            o = self.objects.get(v["object"], {})
            if "text" in o:
                return o["text"]
            if "data" in o and o["data"]:
                return "<%s blob %d bytes>" % (o.get("kind"), len(o["data"]))
            return None
        return v

    def shader_blobs(self):
        """Yield (key, kind, bytes) for every distinct shader blob: parameter-held
        shaders and inline pass shaders."""
        for par in self.parameters:
            ty = par["typedef"]["type"]
            if ty in SHADER_TYPES:
                vals = par["value"] if isinstance(par["value"], list) else [par["value"]]
                for i, v in enumerate(vals):
                    o = self.objects.get(v["object"], {})
                    if o.get("data"):
                        key = par["name"] + ("[%d]" % i if isinstance(par["value"], list) else "")
                        yield key, PT[ty], o["data"]
        for t in self.techniques:
            for pi, ps in enumerate(t["passes"]):
                for st in ps["states"]:
                    res = st.get("resolved", {})
                    if res.get("kind") == "shader" and res["data"]:
                        yield "%s.%s.%s" % (t["name"], ps["name"], st["name"]), st["type"], res["data"]


def parse(data):
    return Effect(data)


def _jsonable(o):
    if isinstance(o, bytes):
        return {"bytes": len(o)}
    if isinstance(o, dict):
        return {k: _jsonable(v) for k, v in o.items() if k != "typedef"}
    if isinstance(o, list):
        return [_jsonable(x) for x in o]
    return o


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    eff = parse(open(argv[1], "rb").read())
    out = {"parameters": _jsonable(eff.parameters), "techniques": _jsonable(eff.techniques),
           "objects": {k: {"kind": v.get("kind"), "text": v.get("text"),
                           "size": len(v.get("data", b""))} for k, v in eff.objects.items()},
           "warnings": eff.warnings}
    if "--json" in argv:
        json.dump(out, open(argv[argv.index("--json") + 1], "w"), indent=1)
    else:
        print(json.dumps(out, indent=1)[:20000])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
