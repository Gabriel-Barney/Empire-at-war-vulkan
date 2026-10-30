"""Reference Python parser for Alamo assets (ALO models/particles, ALA, MTD, DAT, TEM/TEE).

Mirrors the C++ library (tools/formats) for quick iteration and cross-checking. Pure stdlib.
Run with `python -I alo.py <files...>` to print a one-line summary per file (exit 1 on any failure).
"""
import struct
import sys
import zlib

__all__ = ["chunks", "minis", "parse_alo", "parse_ala", "parse_mtd", "parse_dat", "parse_minifile"]


class FormatError(Exception):
    pass


def chunks(d, off=0, end=None):
    """Yield (id, has_children, data_offset, size) for sibling chunks in d[off:end]."""
    end = len(d) if end is None else end
    while off < end:
        if end - off < 8:
            raise FormatError(f"truncated chunk header at {off}")
        cid, raw = struct.unpack_from("<II", d, off)
        size = raw & 0x7FFFFFFF
        if off + 8 + size > end:
            raise FormatError(f"chunk {cid:#x} at {off} overruns parent")
        yield cid, bool(raw >> 31), off + 8, size
        off += 8 + size


def minis(d, off, size):
    """Return list of (id, bytes) mini-chunks in a leaf chunk."""
    out, end = [], off + size
    while off < end:
        mid, ms = d[off], d[off + 1]
        if off + 2 + ms > end:
            raise FormatError(f"mini-chunk {mid:#x} at {off} overruns chunk")
        out.append((mid, bytes(d[off + 2:off + 2 + ms])))
        off += 2 + ms
    return out


def cstr(b):
    return bytes(b).split(b"\0", 1)[0].decode("latin-1")


# ---------------------------------------------------------------- ALO
VERTEX = struct.Struct("<3f3f8f3f3f4f4f4I4f")  # 144 bytes
VERTEX_128 = struct.Struct("<3f3f8f3f3f4f4I4f")  # 128 bytes (legacy 0x10005)
PARAM_KIND = {0x10102: "int", 0x10103: "float", 0x10104: "float3", 0x10105: "texture", 0x10106: "float4"}


def _param(kind, v):
    if kind == "int":
        return struct.unpack("<i", v)[0]
    if kind == "float":
        return struct.unpack("<f", v)[0]
    if kind == "float3":
        return struct.unpack("<3f", v)
    if kind == "float4":
        return struct.unpack("<4f", v)
    return cstr(v)


def _submesh(d, o, s, material):
    sm = {"material": material, "bone_map": [], "collision": None}
    for cid, _, co, cs in chunks(d, o, o + s):
        if cid == 0x10001:
            sm["num_vertices"], sm["num_primitives"] = struct.unpack_from("<II", d, co)
        elif cid == 0x10002:
            sm["vertex_format"] = cstr(d[co:co + cs])
        elif cid in (0x10005, 0x10007):
            n = sm["num_vertices"]
            if n and cs == n * 128 and cid == 0x10005:
                # EaW-era record: the 144-byte layout without the unknown float4 at +96 (SPEC.md 2.5).
                sm["vertices"] = [VERTEX_128.unpack_from(d, co + i * 128) for i in range(n)]
                sm["vertices"] = [v[:24] + (1.0, 1.0, 1.0, 1.0) + v[24:] for v in sm["vertices"]]
            elif n and cs != n * 144:
                raise FormatError("unexpected vertex stride")
            else:
                sm["vertices"] = [VERTEX.unpack_from(d, co + i * 144) for i in range(n)]
        elif cid == 0x10004:
            sm["indices"] = list(struct.unpack_from(f"<{cs // 2}H", d, co))
        elif cid == 0x10006:
            sm["bone_map"] = list(struct.unpack_from(f"<{cs // 4}I", d, co))
        elif cid == 0x1200:
            t = {}
            for c2, _, o2, s2 in chunks(d, co, co + cs):
                if c2 == 0x1201:
                    t["header"] = {k: v for k, v in minis(d, o2, s2)}
                elif c2 == 0x1202:
                    t["nodes"] = [struct.unpack_from("<6B2H", d, o2 + 10 * i) for i in range(s2 // 10)]
                elif c2 == 0x1203:
                    t["triangles"] = list(struct.unpack_from(f"<{s2 // 2}H", d, o2))
            sm["collision"] = t
    if len(sm.get("indices", [])) != 3 * sm["num_primitives"]:
        raise FormatError("index count mismatch")
    if any(i >= sm["num_vertices"] for i in sm.get("indices", [])):
        raise FormatError("index out of range")
    return sm


# Emitter property block (chunk 0x700/0x02) mini-chunk IDs -> (name, struct format).
# Mirrors particle::emitter_fields() in the C++ library (see SPEC.md section 3 for confidence).
EMITTER_FIELDS = {
    0x04: ("blend_mode", "<I"), 0x05: ("geometry", "<I"), 0x06: ("tool_field_06", "<I"),
    0x07: ("use_bursts", "<?"), 0x08: ("object_space", "<?"), 0x09: ("outward_speed", "<f"),
    0x0A: ("acceleration", "<3f"), 0x0B: ("outward_acceleration", "<f"), 0x0C: ("gravity", "<f"),
    0x0F: ("lifetime", "<f"), 0x10: ("texture_cells", "<I"), 0x11: ("random_color_legacy", "<f"),
    0x12: ("random_initial_rotation", "<f"), 0x13: ("random_lifetime", "<f"), 0x14: ("index", "<I"),
    0x15: ("enabled", "<?"), 0x17: ("random_rotation_speed", "<f"), 0x23: ("random_rotation_direction", "<?"),
    0x24: ("initial_delay", "<f"), 0x25: ("burst_delay", "<f"), 0x26: ("particles_per_burst", "<I"),
    0x27: ("burst_count", "<I"), 0x28: ("inherit_velocity_factor", "<f"), 0x2A: ("particles_per_second", "<I"),
    0x2B: ("depth_sort", "<?"), 0x2C: ("random_color", "<4f"), 0x2D: ("random_color_uniform", "<?"),
    0x2E: ("alternate_vertex_layout", "<?"), 0x2F: ("ground_behavior", "<I"), 0x30: ("bounciness", "<f"),
    0x31: ("affected_by_wind", "<?"), 0x32: ("stop_time", "<f"), 0x33: ("skip_time", "<f"),
    0x34: ("spawn_source", "<I"), 0x35: ("object_space_acceleration", "<?"), 0x3B: ("is_heat_particle", "<?"),
    0x3C: ("mesh_normal_offset", "<f"), 0x3D: ("is_weather_particle", "<?"), 0x3E: ("weather_cube_size", "<f"),
    0x3F: ("weather_fade_start", "<f"), 0x40: ("weather_fade_length", "<f"), 0x41: ("has_tail", "<?"),
    0x42: ("tail_size", "<f"), 0x43: ("inherit_velocity", "<?"), 0x44: ("affected_by_wind_disturbance", "<?"),
    0x46: ("render_flag_46", "<?"), 0x47: ("weather_camera_distance", "<f"), 0x48: ("rotation_fixed_at_spawn", "<?"),
    0x49: ("tool_field_49", "<f"),
}
SHAPE_ROLES = ("velocity", "lifetime", "position")
SHAPE_KINDS = ("point", "box", "cube", "sphere", "cylinder", "kind5")


def decode_emitter_properties(block):
    out = {}
    for mid, v in minis(block, 0, len(block)):
        name, fmt = EMITTER_FIELDS.get(mid, ("unknown_%02X" % mid, None))
        if fmt is None:
            out[name] = v.hex()
            continue
        if struct.calcsize(fmt) != len(v):
            raise FormatError(f"emitter property {name} has size {len(v)}")
        val = struct.unpack(fmt, v)
        out[name] = val[0] if len(val) == 1 else val
    return out


def decode_shape(rec):
    kind, *f = struct.unpack_from("<I6f2fB3xfB3xf3f", rec)
    return {"kind": SHAPE_KINDS[kind] if kind < len(SHAPE_KINDS) else kind, "box_min": f[0:3],
            "box_max": f[3:6], "cube_half_size": f[6], "sphere_radius": f[7], "sphere_surface": bool(f[8]),
            "cylinder_radius": f[9], "cylinder_surface": bool(f[10]), "cylinder_height": f[11], "point": f[12:15]}


def _particles(d, o, s):
    ps = {"emitters": []}
    for cid, _, co, cs in chunks(d, o, o + s):
        if cid == 0:
            ps["name"] = cstr(d[co:co + cs])
        elif cid == 0x800:
            for _, _, eo, es in chunks(d, co, co + cs):
                e = {"tracks": []}
                for c2, _, o2, s2 in chunks(d, eo, eo + es):
                    if c2 == 0x16:
                        e["name"] = cstr(d[o2:o2 + s2])
                    elif c2 == 0x03:
                        e["color_texture"] = cstr(d[o2:o2 + s2])
                    elif c2 == 0x45:
                        e["bump_texture"] = cstr(d[o2:o2 + s2])
                    elif c2 == 0x02:
                        e["properties"] = decode_emitter_properties(bytes(d[o2:o2 + s2]))
                    elif c2 == 0x29:
                        recs = [bytes(d[o4:o4 + s4]) for _, _, o3, s3 in chunks(d, o2, o2 + s2)
                                for _, _, o4, s4 in chunks(d, o3, o3 + s3)]
                        e["shapes"] = {role: decode_shape(r) for role, r in zip(SHAPE_ROLES, recs)}
                    elif c2 == 0x01:
                        for c3, _, o3, s3 in chunks(d, o2, o2 + s2):
                            if c3 == 0:
                                e["tracks"].append({"header": minis(d, o3, s3), "keys": []})
                            else:
                                e["tracks"][-1]["keys"] = [struct.unpack("<2f", v) for _, v in minis(d, o3, s3)]
                ps["emitters"].append(e)
    return ps


def parse_alo(d):
    top = list(chunks(d))
    if top and top[0][0] == 0x900:
        return {"kind": "particles", **_particles(d, top[0][2], top[0][3])}
    m = {"kind": "model", "bones": [], "meshes": [], "lights": [], "objects": [], "connections": [], "proxies": []}
    for cid, _, o, s in top:
        if cid == 0x200:
            for c2, _, o2, s2 in chunks(d, o, o + s):
                if c2 != 0x202:
                    continue
                b = {}
                for c3, _, o3, s3 in chunks(d, o2, o2 + s2):
                    if c3 == 0x203:
                        b["name"] = cstr(d[o3:o3 + s3])
                    elif c3 in (0x205, 0x206):
                        b["parent"], b["visible"] = struct.unpack_from("<iI", d, o3)
                        b["billboard"] = struct.unpack_from("<I", d, o3 + 8)[0] if c3 == 0x206 else 0
                        mo = o3 + (12 if c3 == 0x206 else 8)
                        f = struct.unpack_from("<12f", d, mo)
                        b["matrix"] = [f[0:4], f[4:8], f[8:12]]
                m["bones"].append(b)
        elif cid == 0x400:
            mesh = {"submeshes": []}
            mat = None
            for c2, _, o2, s2 in chunks(d, o, o + s):
                if c2 == 0x401:
                    mesh["name"] = cstr(d[o2:o2 + s2])
                elif c2 == 0x402:
                    v = struct.unpack_from("<I6f3I", d, o2)
                    mesh.update(num_submeshes=v[0], bbox=(v[1:4], v[4:7]), hidden=bool(v[8]), collision=bool(v[9]))
                elif c2 == 0x10100:
                    mat = {"params": {}}
                    for c3, _, o3, s3 in chunks(d, o2, o2 + s2):
                        if c3 == 0x10101:
                            mat["shader"] = cstr(d[o3:o3 + s3])
                        elif c3 in PARAM_KIND:
                            mm = dict(minis(d, o3, s3))
                            mat["params"][cstr(mm[1])] = _param(PARAM_KIND[c3], mm[2])
                elif c2 == 0x10000:
                    mesh["submeshes"].append(_submesh(d, o2, s2, mat))
                    mat = None
            if len(mesh["submeshes"]) != mesh["num_submeshes"]:
                raise FormatError("submesh count mismatch")
            m["objects"].append(("mesh", len(m["meshes"])))
            m["meshes"].append(mesh)
        elif cid == 0x1300:
            light = {}
            for c2, _, o2, s2 in chunks(d, o, o + s):
                if c2 == 0x1301:
                    light["name"] = cstr(d[o2:o2 + s2])
                elif c2 == 0x1302:
                    v = struct.unpack_from("<I3f5f", d, o2)
                    light.update(type=v[0], color=v[1:4], intensity=v[4], far_end=v[5], far_start=v[6],
                                 hotspot=v[7], falloff=v[8])
            m["objects"].append(("light", len(m["lights"])))
            m["lights"].append(light)
        elif cid == 0x600:
            for c2, _, o2, s2 in chunks(d, o, o + s):
                mm = dict(minis(d, o2, s2))
                if c2 == 0x602:
                    m["connections"].append((struct.unpack("<I", mm[2])[0], struct.unpack("<I", mm[3])[0]))
                elif c2 == 0x603:
                    m["proxies"].append({"name": cstr(mm[5]), "bone": struct.unpack("<I", mm[6])[0]})
    return m


# ---------------------------------------------------------------- ALA
def parse_ala(d):
    (cid, _, o, s), = list(chunks(d))
    if cid != 0x1000:
        raise FormatError("not an animation")
    a = {"bones": []}
    for c2, _, o2, s2 in chunks(d, o, o + s):
        if c2 == 0x1001:
            mm = dict(minis(d, o2, s2))
            a["frames"] = struct.unpack("<I", mm[1])[0]
            a["fps"] = struct.unpack("<f", mm[2])[0]
            a["num_bones"] = struct.unpack("<I", mm[3])[0]
            a["layout"] = "block" if 0x0B in mm else "per-bone"
            for k, n in ((0x0B, "rot_block"), (0x0C, "trans_block"), (0x0D, "scale_block")):
                a[n] = struct.unpack("<I", mm[k])[0] if k in mm else 0
        elif c2 == 0x1002:
            b = {}
            for c3, _, o3, s3 in chunks(d, o2, o2 + s2):
                if c3 == 0x1003:
                    mm = dict(minis(d, o3, s3))
                    b["name"] = cstr(mm[4])
                    b["index"] = struct.unpack("<I", mm[5])[0]
                    b["t_off"], b["t_scale"] = struct.unpack("<3f", mm[6]), struct.unpack("<3f", mm[7])
                    for k, n in ((0x0E, "t_idx"), (0x0F, "s_idx"), (0x10, "r_idx")):
                        b[n] = struct.unpack("<H", mm[k])[0] if k in mm else 0xFFFF
                    b["default_rot"] = struct.unpack("<4h", mm[0x11]) if 0x11 in mm else (0, 0, 0, 32767)
                else:
                    b[{0x1004: "t_data", 0x1005: "s_data", 0x1006: "r_data", 0x1007: "visibility",
                       0x1008: "steps"}.get(c3, hex(c3))] = bytes(d[o3:o3 + s3])
            a["bones"].append(b)
        elif c2 in (0x1009, 0x100A, 0x100B):
            a[{0x1009: "rot_data", 0x100A: "trans_data", 0x100B: "scale_data"}[c2]] = bytes(d[o2:o2 + s2])
    if len(a["bones"]) != a["num_bones"]:
        raise FormatError("bone count mismatch")
    if a["layout"] == "block":
        if len(a.get("rot_data", b"")) != 2 * a["frames"] * a["rot_block"]:
            raise FormatError("rotation block size mismatch")
        if len(a.get("trans_data", b"")) != 2 * a["frames"] * a["trans_block"]:
            raise FormatError("translation block size mismatch")
    return a


def ala_rotation(a, bone, frame):
    b = a["bones"][bone]
    if a["layout"] == "block":
        if b["r_idx"] == 0xFFFF:
            q = b["default_rot"]
        else:
            q = struct.unpack_from("<4h", a["rot_data"], 2 * (frame * a["rot_block"] + b["r_idx"]))
    else:
        r = b.get("r_data", b"")
        q = struct.unpack_from("<4h", r, 0 if len(r) == 8 else 8 * frame) if r else (0, 0, 0, 32767)
    return tuple(c / 32767.0 for c in q)


def ala_translation(a, bone, frame):
    b = a["bones"][bone]
    if a["layout"] == "block":
        if b["t_idx"] == 0xFFFF:
            return b["t_off"]
        q = struct.unpack_from("<3H", a["trans_data"], 2 * (frame * a["trans_block"] + b["t_idx"]))
    else:
        if not b.get("t_data"):
            return b["t_off"]
        q = struct.unpack_from("<3H", b["t_data"], 6 * frame)
    return tuple(o + s * v for o, s, v in zip(b["t_off"], b["t_scale"], q))


# ---------------------------------------------------------------- MTD / DAT / TEM / TEE
def parse_mtd(d):
    n = struct.unpack_from("<I", d)[0]
    if len(d) != 4 + 81 * n:
        raise FormatError("MTD size mismatch")
    out = []
    for i in range(n):
        o = 4 + 81 * i
        x, y, w, h, alpha = struct.unpack_from("<4IB", d, o + 64)
        out.append({"name": cstr(d[o:o + 64]), "x": x, "y": y, "w": w, "h": h, "alpha": alpha})
    return out


def parse_dat(d):
    n = struct.unpack_from("<I", d)[0]
    recs = [struct.unpack_from("<3I", d, 4 + 12 * i) for i in range(n)]
    p = 4 + 12 * n
    vals, keys = [], []
    for _, vl, _ in recs:
        vals.append(d[p:p + 2 * vl].decode("utf-16-le"))
        p += 2 * vl
    for _, _, kl in recs:
        keys.append(d[p:p + kl].decode("latin-1"))
        p += kl
    if p != len(d):
        raise FormatError("DAT size mismatch")
    return [{"crc": r[0], "key": k, "value": v, "crc_ok": zlib.crc32(k.encode("latin-1")) == r[0]}
            for r, k, v in zip(recs, keys, vals)]


def parse_minifile(d):
    out = []

    def walk(o, e):
        for cid, kids, co, cs in chunks(d, o, e):
            if kids:
                walk(co, co + cs)
            else:
                out.append((cid, minis(d, co, cs)))
    walk(0, len(d))
    return out


def summarize(path):
    d = open(path, "rb").read()
    ext = path.lower().rsplit(".", 1)[-1]
    if ext == "alo":
        m = parse_alo(d)
        if m["kind"] == "particles":
            return f"particles {len(m['emitters'])} emitters"
        nv = sum(s["num_vertices"] for me in m["meshes"] for s in me["submeshes"])
        nt = sum(s["num_primitives"] for me in m["meshes"] for s in me["submeshes"])
        return f"model {len(m['bones'])} bones {len(m['meshes'])} meshes {nv} verts {nt} tris {len(m['lights'])} lights {len(m['proxies'])} proxies"
    if ext == "ala":
        a = parse_ala(d)
        return f"anim {a['layout']} {a['frames']} frames {a['num_bones']} bones"
    if ext == "mtd":
        return f"mtd {len(parse_mtd(d))} entries"
    if ext == "dat":
        t = parse_dat(d)
        return f"dat {len(t)} strings {sum(not e['crc_ok'] for e in t)} crc mismatches"
    if ext in ("tem", "tee"):
        return f"{ext} {len(parse_minifile(d))} records"
    raise FormatError("unsupported extension")


if __name__ == "__main__":
    bad = 0
    for p in sys.argv[1:]:
        try:
            print(p.rsplit("/", 1)[-1], summarize(p))
        except Exception as e:  # noqa: BLE001
            bad += 1
            print(p.rsplit("/", 1)[-1], "FAIL", e)
    sys.exit(1 if bad else 0)
