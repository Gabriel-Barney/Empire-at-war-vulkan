#!/usr/bin/env python3
"""Convert full CRC goldens (ALAMO_CRC 1, one line per tick) to checkpoint goldens (ALAMO_CRC 2).

Format and comparison rules: engine/core/include/alamo/core/sim_crc.hpp (CrcLog) and docs/status/golden-checkpoints.md.
A v2 file keeps the header lines and the comments, adds `checkpoint <N>` (and an optional `keep <tick> ...` line) and stores only the first record, the last record, every tick with tick % N == 0 and the kept
ticks. It is a subset of the full log, so it compares against a full log (or a fresh run) on its stored ticks.

  python tools/goldens/convert_goldens.py --in-place --check FILE...        v1 -> v2, N = 64
  python tools/goldens/convert_goldens.py --out-dir DIR [--checkpoint N] [--keep T,T,...] [--check] FILE...
  python tools/goldens/convert_goldens.py --all --in-place --check          every tracked CRC golden under engine/ (git ls-files)
  python tools/goldens/convert_goldens.py --verify GOLDEN_V2 SOURCE.crc     compare a v2 file with a full (or v2) log
  python tools/goldens/convert_goldens.py --selftest

--check (with a conversion): re-reads every converted file and proves, on every checkpoint, that it equals its source,
that exactly the rule's ticks are stored, that header and comments are unchanged and that the text is canonical (the
output of parse + write is the file itself). A v2 file given as input keeps its own checkpoint / keep unless
--checkpoint / --keep say otherwise (so a second run changes nothing). The C++ writer (alamo_headless --crc-convert,
CrcLog::checkpointed) is the same rule; a ctest (alamo_goldens_convert) checks both give identical bytes.

Exit code: 0 ok, 1 a check failed / the logs differ, 2 usage or unreadable input.
"""
from __future__ import annotations

import argparse
import os
import sys
from dataclasses import dataclass, field

DEFAULT_CHECKPOINT = 64


class CrcError(ValueError):
    pass


@dataclass
class Record:
    tick: int
    combined: int
    parity: int
    systems: list[int]

    def key(self):
        return (self.combined, self.parity, tuple(self.systems))


@dataclass
class CrcLog:
    systems: list[str] = field(default_factory=list)
    parity_parts: list[str] = field(default_factory=list)
    comments: list[str] = field(default_factory=list)
    records: list[Record] = field(default_factory=list)
    checkpoint: int = 0  # 0 = full v1 log
    keep: list[int] = field(default_factory=list)


def _is_stored(tick: int, every: int, keep) -> bool:
    return (every != 0 and tick % every == 0) or tick in keep


def parse(text: str) -> CrcLog:
    """Parses v1 and v2 text the way CrcLog::parse does (same errors for the same malformed input)."""
    log = CrcLog()
    magic = None
    have_systems = have_parity = have_checkpoint = False
    for no, raw in enumerate(text.split("\n"), 1):
        line = raw[:-1] if raw.endswith("\r") else raw
        where = f"line {no}: "
        if line.startswith("#"):
            c = line[1:]
            log.comments.append(c[1:] if c.startswith(" ") else c)
            continue
        tok = line.split()
        if not tok:
            continue
        if magic is None:
            if len(tok) != 2 or tok[0] != "ALAMO_CRC" or tok[1] not in ("1", "2"):
                raise CrcError(where + "expected 'ALAMO_CRC 1' or 'ALAMO_CRC 2'")
            magic = tok[1]
            continue
        if not have_systems:
            if tok[0] != "systems" or len(tok) < 2 or not tok[1].isdigit() or len(tok) != int(tok[1]) + 2:
                raise CrcError(where + "expected 'systems <count> <names...>'")
            log.systems = tok[2:]
            have_systems = True
            continue
        if not have_parity:
            if tok[0] != "parity" or len(tok) < 2 or not tok[1].isdigit() or len(tok) != int(tok[1]) + 2:
                raise CrcError(where + "expected 'parity <count> <names...>'")
            log.parity_parts = tok[2:]
            have_parity = True
            continue
        if magic == "2" and not have_checkpoint:
            if tok[0] != "checkpoint" or len(tok) != 2 or not tok[1].isdigit() or int(tok[1]) == 0:
                raise CrcError(where + "expected 'checkpoint <N>' (N > 0) after the parity line of an ALAMO_CRC 2 file")
            log.checkpoint = int(tok[1])
            have_checkpoint = True
            continue
        if magic == "2" and not log.records and tok[0] == "keep":
            if log.keep:
                raise CrcError(where + "second 'keep' line")
            for t in tok[1:]:
                if not t.isdigit():
                    raise CrcError(where + "bad tick in 'keep' line")
                if log.keep and int(t) <= log.keep[-1]:
                    raise CrcError(where + "'keep' ticks must increase")
                log.keep.append(int(t))
            continue
        if len(tok) != len(log.systems) + 3:
            raise CrcError(where + "wrong number of CRC columns")
        try:
            rec = Record(int(tok[0]), int(tok[1], 16), int(tok[2], 16), [int(v, 16) for v in tok[3:]])
        except ValueError:
            raise CrcError(where + "bad tick, combined or parity CRC") from None
        if rec.parity > 0xFFFFFFFF:
            raise CrcError(where + "bad tick, combined or parity CRC")
        if log.records and rec.tick <= log.records[-1].tick:
            raise CrcError(where + "ticks must increase")
        log.records.append(rec)
    if not have_parity:
        raise CrcError("missing header")
    if magic == "2":
        if not have_checkpoint:
            raise CrcError("missing 'checkpoint <N>' line")
        kept = set(log.keep)
        for r in log.records[1:-1]:
            if not _is_stored(r.tick, log.checkpoint, kept):
                raise CrcError(f"tick {r.tick} is neither a multiple of the checkpoint {log.checkpoint} nor in the keep line")
    return log


def to_text(log: CrcLog) -> str:
    """The writer of CrcLog::to_text, byte for byte."""
    out = ["ALAMO_CRC 2" if log.checkpoint else "ALAMO_CRC 1"]
    out.append(" ".join(["systems", str(len(log.systems))] + log.systems))
    out.append(" ".join(["parity", str(len(log.parity_parts))] + log.parity_parts))
    if log.checkpoint:
        out.append(f"checkpoint {log.checkpoint}")
        if log.keep:
            out.append("keep " + " ".join(str(t) for t in log.keep))
    out += ["# " + c for c in log.comments]
    for r in log.records:
        out.append(f"{r.tick} {r.combined:016x} {r.parity:08x}" + "".join(f" {v:016x}" for v in r.systems))
    return "\n".join(out) + "\n"


def checkpointed(log: CrcLog, every: int, keep=()) -> CrcLog:
    """CrcLog::checkpointed: first and last record, every tick with tick % every == 0 and the kept ticks."""
    if every == 0:
        return log
    ks = sorted(set(keep))
    kset = set(ks)
    recs = [r for i, r in enumerate(log.records) if i == 0 or i == len(log.records) - 1 or _is_stored(r.tick, every, kset)]
    return CrcLog(list(log.systems), list(log.parity_parts), list(log.comments), recs, every, ks)


@dataclass
class Result:
    ok: bool
    message: str
    tick: int = 0
    window: tuple[int, int] = (0, 0)
    columns: list[str] = field(default_factory=list)


def compare(expected: CrcLog, actual: CrcLog) -> Result:
    """first_divergence for the checkpoint case (and for two full logs): the checkpoint log's stored ticks are
    compared with the other log's records at the same ticks; both logs must end at the same tick."""
    if expected.systems != actual.systems or expected.parity_parts != actual.parity_parts:
        return Result(False, "system lists differ")
    drive_expected = expected.checkpoint != 0 or actual.checkpoint == 0
    drv, oth = (expected, actual) if drive_expected else (actual, expected)
    if not drv.records or not oth.records:
        same = not drv.records and not oth.records
        return Result(same, "identical (0 ticks)" if same else f"expected {len(expected.records)} records, got {len(actual.records)}")
    by_tick = {r.tick: r for r in oth.records}
    prev = None
    for dr in drv.records:
        o = by_tick.get(dr.tick)
        if o is None:
            return Result(False, f"checkpoint tick {dr.tick} of the {'expected' if drive_expected else 'actual'} log is not in the other log", dr.tick)
        e, a = (dr, o) if drive_expected else (o, dr)
        if e.key() == a.key():
            prev = dr.tick
            continue
        cols = [expected.systems[i] for i in range(len(e.systems)) if e.systems[i] != a.systems[i]]
        if e.parity != a.parity:
            cols.append("parity")
        if e.combined != a.combined:
            cols.append("combined")
        window = ((prev + 1) if prev is not None else dr.tick, dr.tick)
        return Result(False, f"tick {dr.tick} differs in {', '.join(cols)}; the first differing tick is in {window[0]}..{window[1]}",
                      dr.tick, window, cols)
    el, al = expected.records[-1].tick, actual.records[-1].tick
    if el != al:
        return Result(False, f"checkpoints match but the runs end at different ticks: expected last tick {el}, got {al}", min(el, al) + 1)
    n = len(drv.records) if (expected.checkpoint or actual.checkpoint) else len(expected.records)
    return Result(True, f"identical on {n} records")


def convert(src: CrcLog, every: int, keep) -> CrcLog:
    return checkpointed(src, every, keep)


def check_conversion(src: CrcLog, out_text: str, every: int, keep) -> list[str]:
    """Everything --check proves; returns the list of problems (empty = proven)."""
    problems: list[str] = []
    try:
        back = parse(out_text)
    except CrcError as e:
        return [f"converted text does not parse: {e}"]
    if back.checkpoint != every:
        problems.append(f"checkpoint is {back.checkpoint}, expected {every}")
    if back.systems != src.systems or back.parity_parts != src.parity_parts:
        problems.append("system / parity header differs from the source")
    if back.comments != src.comments:
        problems.append("comments differ from the source")
    if to_text(back) != out_text:
        problems.append("text is not canonical (write(parse(text)) != text)")
    # Exactly the rule's ticks, nothing else.
    kset = set(keep)
    want = [r.tick for i, r in enumerate(src.records) if i in (0, len(src.records) - 1) or _is_stored(r.tick, every, kset)]
    if [r.tick for r in back.records] != want:
        problems.append("stored ticks differ from the checkpoint rule")
    # Every stored checkpoint equals the source record at that tick.
    by_tick = {r.tick: r for r in src.records}
    bad = [r.tick for r in back.records if r.tick not in by_tick or by_tick[r.tick].key() != r.key()]
    if bad:
        problems.append(f"{len(bad)} checkpoint(s) differ from the source, first at tick {bad[0]}")
    res = compare(back, src)
    if not res.ok:
        problems.append("compare(converted, source): " + res.message)
    res = compare(src, back)
    if not res.ok:
        problems.append("compare(source, converted): " + res.message)
    return problems


def discover_goldens() -> list[str]:
    """Every tracked file under engine/ that starts with ALAMO_CRC (the .crc goldens and the module goldens named *_crc.txt)."""
    import subprocess
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    r = subprocess.run(["git", "ls-files", "engine"], cwd=root, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("git ls-files failed: " + r.stderr.strip())
    found = []
    for rel in r.stdout.splitlines():
        if not rel.endswith((".crc", ".txt")):
            continue
        p = os.path.join(root, rel)
        try:
            with open(p, "rb") as f:
                if f.read(10) == b"ALAMO_CRC ":
                    found.append(p)
        except OSError:
            pass
    return found


def _int_list(s: str | None) -> list[int]:
    return [int(x) for x in s.replace(" ", "").split(",") if x] if s else []


def _kb(n: int) -> str:
    return f"{n / 1024:.0f} KB"


def run_convert(args) -> int:
    rc = 0
    for path in args.files:
        try:
            with open(path, "rb") as f:
                raw = f.read()
            src = parse(raw.decode("utf-8"))
        except (OSError, CrcError, UnicodeDecodeError) as e:
            print(f"{path}: {e}", file=sys.stderr)
            return 2
        if src.checkpoint and args.checkpoint is None and args.keep is None:
            every, keep = src.checkpoint, list(src.keep)  # a v2 input keeps its own settings (idempotent)
        else:
            every = args.checkpoint if args.checkpoint is not None else DEFAULT_CHECKPOINT
            keep = _int_list(args.keep) if args.keep is not None else list(src.keep)
        if every <= 0:
            print("--checkpoint must be > 0", file=sys.stderr)
            return 2
        out = convert(src, every, keep)
        text = to_text(out)
        dest = path if args.in_place else os.path.join(args.out_dir or ".", os.path.basename(path))
        status = "unchanged" if text.encode("utf-8") == raw else "converted"
        if not args.dry_run:
            if not args.in_place:
                os.makedirs(args.out_dir, exist_ok=True)
            if status == "converted" or not args.in_place:
                with open(dest, "wb") as f:
                    f.write(text.encode("utf-8"))
        line = (f"{os.path.basename(path)}: {status}, {len(src.records)} -> {len(out.records)} records "
                f"(checkpoint {every}{', keep ' + ','.join(map(str, keep)) if keep else ''}), {_kb(len(raw))} -> {_kb(len(text))}")
        if args.check:
            problems = check_conversion(src, text, every, keep)
            if problems:
                rc = 1
                line += "\n  CHECK FAILED: " + "\n  CHECK FAILED: ".join(problems)
            else:
                line += f"; check ok on {len(out.records)} checkpoints"
        print(line)
    return rc


def run_verify(golden_path: str, source_path: str) -> int:
    try:
        golden = parse(open(golden_path, encoding="utf-8", newline="").read())
        source = parse(open(source_path, encoding="utf-8", newline="").read())
    except (OSError, CrcError) as e:
        print(e, file=sys.stderr)
        return 2
    res = compare(golden, source)
    print(("MATCH: " if res.ok else "DIVERGED: ") + res.message)
    return 0 if res.ok else 1


def _selftest() -> int:
    fails = 0

    def check(cond, what):
        nonlocal fails
        if not cond:
            print("selftest FAIL:", what)
            fails += 1

    def make(n):
        x = 12345
        recs = []
        for t in range(n):
            ss = []
            for _ in range(3):
                x = (x * 6364136223846793005 + 1442695040888963407) & (2**64 - 1)
                ss.append(x)
            recs.append(Record(t, (x ^ 0xABCDEF) & (2**64 - 1), x >> 32, ss))
        return CrcLog(["a", "b", "c"], ["p"], ["seed 5"], recs)

    full = make(200)
    text1 = to_text(full)
    check(text1.startswith("ALAMO_CRC 1\nsystems 3 a b c\nparity 1 p\n# seed 5\n0 "), "v1 header")
    check(to_text(parse(text1)) == text1, "v1 round trip")
    v2 = checkpointed(full, 64, [100, 7])
    check([r.tick for r in v2.records] == [0, 7, 64, 100, 128, 192, 199], "ticks")
    t2 = to_text(v2)
    check(t2.startswith("ALAMO_CRC 2\nsystems 3 a b c\nparity 1 p\ncheckpoint 64\nkeep 7 100\n# seed 5\n"), "v2 header")
    check(to_text(parse(t2)) == t2, "v2 round trip")
    check(check_conversion(full, t2, 64, [7, 100]) == [], "check_conversion ok")
    check(compare(parse(t2), full).ok and compare(full, parse(t2)).ok, "v1 vs v2 both ways")
    bad = make(200)
    bad.records[130].systems[0] ^= 1  # not a stored tick: invisible
    check(compare(parse(to_text(checkpointed(full, 64))), bad).ok, "off-checkpoint difference not seen")
    bad.records[128].systems[1] ^= 1
    bad.records[128].combined ^= 1
    r = compare(parse(to_text(checkpointed(full, 64))), bad)
    check(not r.ok and r.tick == 128 and r.window == (65, 128) and r.columns == ["b", "combined"], "mismatch report")
    # a tampered converted file fails the check
    tam = to_text(checkpointed(full, 64)).replace("\n64 ", "\n64 f", 1)
    check(check_conversion(full, tam, 64, []) != [], "tampered conversion fails")
    # malformed input
    for t in ["ALAMO_CRC 3\n", "ALAMO_CRC 2\nsystems 1 x\nparity 0\n0 1 0 1\n", "ALAMO_CRC 1\nsystems 1 x\nparity 0\n5 1 0 1\n4 1 0 1\n"]:
        try:
            parse(t)
            check(False, "malformed accepted: " + t[:20])
        except CrcError:
            pass
    # idempotent: converting a v2 text again changes nothing
    check(to_text(checkpointed(parse(t2), 64, [7, 100])) == t2, "idempotent")
    print("convert_goldens selftest:", "FAILED" if fails else "all passed")
    return 1 if fails else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="*", help="CRC golden files (ALAMO_CRC 1 or 2)")
    ap.add_argument("--checkpoint", type=int, default=None, help=f"N: store every Nth tick (default {DEFAULT_CHECKPOINT}; a v2 input keeps its own)")
    ap.add_argument("--keep", default=None, help="extra ticks to store, T,T,... (default: the input's keep line)")
    ap.add_argument("--in-place", action="store_true", help="rewrite the files")
    ap.add_argument("--out-dir", help="write the converted files here instead")
    ap.add_argument("--check", action="store_true", help="prove each converted file equals its source on every checkpoint")
    ap.add_argument("--dry-run", action="store_true", help="convert and check, write nothing")
    ap.add_argument("--all", action="store_true", help="convert every tracked ALAMO_CRC file under engine/ (instead of FILE...)")
    ap.add_argument("--verify", nargs=2, metavar=("GOLDEN", "SOURCE"), help="compare a golden with a log; exit 1 if they differ")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv)
    if args.selftest:
        return _selftest()
    if args.verify:
        return run_verify(*args.verify)
    if args.all:
        args.files = discover_goldens()
    if not args.files:
        ap.error("no files")
    if not args.in_place and not args.out_dir and not args.dry_run:
        ap.error("give --in-place, --out-dir DIR or --dry-run")
    return run_convert(args)


if __name__ == "__main__":
    sys.exit(main())
