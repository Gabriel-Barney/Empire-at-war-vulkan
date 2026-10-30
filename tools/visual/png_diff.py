"""Screenshot diffs for the render_* / ui_gui_* / game_* PNGs against stored baselines (PAF t05). Standard library only.

  python tools/visual/png_diff.py compare BASELINE_DIR NEW_DIR [--tol 4] [--max-frac 0.005] [--diff-dir DIR] [--only GLOB] [--strict]
  python tools/visual/png_diff.py update  BASELINE_DIR NEW_DIR [--only GLOB]      copy NEW_DIR's PNGs over the baselines
  python tools/visual/png_diff.py --selftest

A PNG matches when at most --max-frac of its pixels differ by more than --tol in any channel (8-bit values; defaults absorb
driver-level rounding but not a moved quad or a missing texture) and the sizes agree. Files only in NEW_DIR are reported as
"new" (a failure only with --strict); files only in BASELINE_DIR are "missing" and always fail. --diff-dir gets a
<name>.diff.png for every mismatch: the new image dimmed, differing pixels in red. Exit 0 = all match, 1 = a mismatch.

Reads 8-bit grey / grey+alpha / RGB / RGBA non-interlaced PNGs (what the engine writes: 8-bit RGBA). Identical files and
identical decompressed pixel data are recognised without unfiltering; only a real mismatch pays for the pure-Python decode.
"""
import argparse
import fnmatch
import os
import shutil
import struct
import sys
import tempfile
import zlib

SIG = b"\x89PNG\r\n\x1a\n"
CHANNELS = {0: 1, 2: 3, 4: 2, 6: 4}


def read_chunks(data):
    if data[:8] != SIG:
        raise ValueError("not a PNG")
    pos = 8
    while pos + 8 <= len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        yield kind, data[pos + 8:pos + 8 + n]
        pos += 12 + n


def load_raw(path):
    """(width, height, channels, filtered scanline bytes) of a PNG file."""
    with open(path, "rb") as f:
        data = f.read()
    width = height = ct = None
    idat = []
    for kind, body in read_chunks(data):
        if kind == b"IHDR":
            width, height, depth, ct, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or ct not in CHANNELS or interlace:
                raise ValueError(f"{path}: only 8-bit non-interlaced grey / RGB(A) PNGs are supported")
        elif kind == b"IDAT":
            idat.append(body)
    if width is None:
        raise ValueError(f"{path}: no IHDR")
    return width, height, CHANNELS[ct], zlib.decompress(b"".join(idat))


def unfilter(width, height, ch, raw):
    """Pixel rows (list of bytearray) from filtered scanlines."""
    stride = width * ch
    if len(raw) != height * (stride + 1):
        raise ValueError("pixel data size mismatch")
    rows = []
    prev = bytearray(stride)
    for y in range(height):
        base = y * (stride + 1)
        ft = raw[base]
        cur = bytearray(raw[base + 1:base + 1 + stride])
        if ft == 1:
            for i in range(ch, stride):
                cur[i] = (cur[i] + cur[i - ch]) & 255
        elif ft == 2:
            for i in range(stride):
                cur[i] = (cur[i] + prev[i]) & 255
        elif ft == 3:
            for i in range(stride):
                left = cur[i - ch] if i >= ch else 0
                cur[i] = (cur[i] + ((left + prev[i]) >> 1)) & 255
        elif ft == 4:
            for i in range(stride):
                a = cur[i - ch] if i >= ch else 0
                b = prev[i]
                c = prev[i - ch] if i >= ch else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                cur[i] = (cur[i] + pred) & 255
        elif ft != 0:
            raise ValueError(f"bad filter type {ft}")
        rows.append(cur)
        prev = cur
    return rows


def write_png(path, width, height, rows):
    """8-bit RGBA PNG from rows of RGBA bytes."""
    raw = b"".join(b"\x00" + bytes(r) for r in rows)

    def chunk(kind, body):
        c = struct.pack(">I", len(body)) + kind + body
        return c + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(SIG + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def compare_images(base_path, new_path, tol, diff_path=None):
    """(differing pixel fraction, max channel difference, size note) of two PNGs; fraction is 1.0 when the sizes differ."""
    bw, bh, bc, braw = load_raw(base_path)
    nw, nh, nc, nraw = load_raw(new_path)
    if (bw, bh, bc) != (nw, nh, nc):
        return 1.0, 255, f"size/format {bw}x{bh}x{bc} vs {nw}x{nh}x{nc}"
    if braw == nraw:
        return 0.0, 0, ""
    brows, nrows = unfilter(bw, bh, bc, braw), unfilter(nw, nh, nc, nraw)
    bad = 0
    worst = 0
    out_rows = []
    for br, nr in zip(brows, nrows):
        out = bytearray(bw * 4)
        if br != nr:
            for x in range(bw):
                o = x * bc
                d = max(abs(br[o + k] - nr[o + k]) for k in range(bc))
                if d > worst:
                    worst = d
                if d > tol:
                    bad += 1
                    out[x * 4:x * 4 + 4] = b"\xff\x00\x00\xff"
                    continue
                g = nr[o] if bc < 3 else (nr[o] + nr[o + 1] + nr[o + 2]) // 3
                out[x * 4:x * 4 + 4] = bytes((g // 3, g // 3, g // 3, 255))
        else:
            for x in range(bw):
                o = x * bc
                g = nr[o] if bc < 3 else (nr[o] + nr[o + 1] + nr[o + 2]) // 3
                out[x * 4:x * 4 + 4] = bytes((g // 3, g // 3, g // 3, 255))
        out_rows.append(out)
    frac = bad / float(bw * bh)
    if diff_path and bad:
        write_png(diff_path, bw, bh, out_rows)
    return frac, worst, ""


def list_pngs(folder, only):
    names = sorted(n for n in os.listdir(folder) if n.lower().endswith(".png"))
    return [n for n in names if not only or fnmatch.fnmatch(n, only)]


def cmd_compare(args):
    base, new = list_pngs(args.baseline, args.only), list_pngs(args.new, args.only)
    if args.diff_dir:
        os.makedirs(args.diff_dir, exist_ok=True)
    failures = 0
    for n in sorted(set(base) | set(new)):
        if n not in new:
            print(f"MISSING  {n}: baseline has no new image")
            failures += 1
        elif n not in base:
            print(f"NEW      {n}: no baseline")
            failures += 1 if args.strict else 0
        else:
            diff = os.path.join(args.diff_dir, n[:-4] + ".diff.png") if args.diff_dir else None
            try:
                frac, worst, note = compare_images(os.path.join(args.baseline, n), os.path.join(args.new, n), args.tol, diff)
            except (ValueError, zlib.error, struct.error) as e:
                print(f"ERROR    {n}: {e}")
                failures += 1
                continue
            ok = frac <= args.max_frac
            print(f"{'ok      ' if ok else 'MISMATCH'} {n}: {frac * 100:.4f}% pixels differ by more than {args.tol}, max diff {worst}{' ' + note if note else ''}")
            failures += 0 if ok else 1
    print(f"png_diff: {len(set(base) & set(new))} compared, {failures} failing")
    return 1 if failures else 0


def cmd_update(args):
    os.makedirs(args.baseline, exist_ok=True)
    names = list_pngs(args.new, args.only)
    for n in names:
        shutil.copyfile(os.path.join(args.new, n), os.path.join(args.baseline, n))
    print(f"png_diff: {len(names)} baseline(s) updated in {args.baseline}")
    return 0


def selftest():
    def solid(w, h, rgba):
        return [bytearray(rgba * w) for _ in range(h)]

    with tempfile.TemporaryDirectory() as tmp:
        a, b, c = (os.path.join(tmp, n) for n in ("a.png", "b.png", "c.png"))
        write_png(a, 16, 8, solid(16, 8, bytes((10, 20, 30, 255))))
        write_png(b, 16, 8, solid(16, 8, bytes((10, 20, 30, 255))))
        rows = solid(16, 8, bytes((10, 20, 30, 255)))
        rows[3][4 * 5:4 * 5 + 3] = bytes((200, 20, 30))  # one pixel clearly different
        rows[4][4 * 6:4 * 6 + 3] = bytes((12, 20, 30))  # one within the tolerance
        write_png(c, 16, 8, rows)
        assert compare_images(a, b, 4)[:2] == (0.0, 0)
        frac, worst, _ = compare_images(a, c, 4, os.path.join(tmp, "d.png"))
        assert abs(frac - 1 / 128) < 1e-9 and worst == 190, (frac, worst)
        assert os.path.exists(os.path.join(tmp, "d.png"))
        write_png(os.path.join(tmp, "e.png"), 8, 8, solid(8, 8, bytes((10, 20, 30, 255))))
        assert compare_images(a, os.path.join(tmp, "e.png"), 4)[0] == 1.0
        # a Paeth / Sub / Up / Average encoded image decodes to the same pixels as the unfiltered one
        w, h = 5, 4
        pix = [bytes((x * 40 + y, y * 50 + x, (x * y) & 255, 255 - x)) for y in range(h) for x in range(w)]
        rows_ref = [bytearray(b"".join(pix[y * w:(y + 1) * w])) for y in range(h)]
        raw = bytearray()
        prev = bytearray(w * 4)
        for y, ft in enumerate((1, 2, 3, 4)):
            cur = rows_ref[y]
            raw.append(ft)
            for i in range(w * 4):
                a_ = cur[i - 4] if i >= 4 else 0
                b_ = prev[i]
                c_ = prev[i - 4] if i >= 4 else 0
                if ft == 1:
                    pred = a_
                elif ft == 2:
                    pred = b_
                elif ft == 3:
                    pred = (a_ + b_) >> 1
                else:
                    p = a_ + b_ - c_
                    pa, pb, pc = abs(p - a_), abs(p - b_), abs(p - c_)
                    pred = a_ if pa <= pb and pa <= pc else (b_ if pb <= pc else c_)
                raw.append((cur[i] - pred) & 255)
            prev = cur
        assert unfilter(w, h, 4, bytes(raw)) == rows_ref
    print("png_diff selftest: ok")
    return 0


def main():
    if "--selftest" in sys.argv[1:]:
        return selftest()
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("compare", "update"):
        p = sub.add_parser(name)
        p.add_argument("baseline")
        p.add_argument("new")
        p.add_argument("--only", help="glob on the file name, e.g. 'ui_gui_*'")
        if name == "compare":
            p.add_argument("--tol", type=int, default=4)
            p.add_argument("--max-frac", type=float, default=0.005)
            p.add_argument("--diff-dir")
            p.add_argument("--strict", action="store_true", help="a PNG without a baseline fails")
    args = ap.parse_args()
    return cmd_compare(args) if args.cmd == "compare" else cmd_update(args)


if __name__ == "__main__":
    sys.exit(main())
