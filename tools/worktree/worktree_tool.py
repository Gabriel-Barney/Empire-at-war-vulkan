#!/usr/bin/env python
"""Create and clean git worktrees for parallel agents (Windows, standard library only).

  python tools/worktree/worktree_tool.py new <name> [--base origin/dev] [--fetch] [--no-sync] [--modules "core;x"]
  python tools/worktree/worktree_tool.py clean [--apply] [--days N] [--keep a,b] [--only a,b]
                                               [--pushed] [--trees] [--main] [--no-size] [--fetch]
  python tools/worktree/worktree_tool.py --selftest

Wrappers: tools\\new_worktree.cmd and tools\\clean_worktrees.cmd.

new    `git worktree add --no-track .claude/worktrees/<name> -b claude/<name> <base>` run from the MAIN checkout (found
       with `git worktree list`). <base> defaults to origin/dev, the development branch (CLAUDE.md "Branches"; origin/main
       when origin has no dev). Then directory junctions (`mklink /J`, no admin) for Data, extracted, toolchain and
       roadmap-store pointing at the main checkout's folders, `roadmap_store.py sync` (a failure only warns), and the
       exact next commands. re_reference/ is never linked
       (clean-room rule: only RE/doc tasks open it, from the main checkout).
clean  dry run by default: a table of every worktree (branch, merged into origin/dev or origin/main, uncommitted changes, unpushed
       commits, age, build-tree GB) and what --apply would do. --apply removes a worktree only when its HEAD is
       reachable from origin/dev or origin/main (merged), the tree is clean, nothing
       is unpushed, it has been idle for --days days (default 2: a fresh worktree on the dev tip looks "merged"),
       it is not locked, not the main checkout, not the worktree this was started from and not in --keep.
       Links are removed first with `cmd /c rmdir` (never recursive: that would delete the real Data / toolchain),
       then `git worktree remove`. --pushed also allows clean worktrees whose commits are all on some remote branch.
       --trees prunes build-* trees whose newest file activity is older than --days in the kept worktrees (--main
       adds the main checkout; build-msvc, build-clang and build-asan are never pruned there). Local branches are
       never deleted.
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path

LINKED = ("Data", "extracted", "toolchain", "roadmap-store")
MAIN_TREES_KEPT = {"build-msvc", "build-clang", "build-asan"}
NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")
REPARSE_ATTR = 0x400
DIR_ATTR = 0x10
LINK_TAGS = (0xA0000003, 0xA000000C)  # mount point (junction), symlink
DAY = 86400.0


class ToolError(Exception):
    pass


def norm(p) -> str:
    return os.path.normcase(os.path.abspath(str(p)))


def run(args, cwd=None, timeout=None):
    return subprocess.run(args, cwd=str(cwd) if cwd else None, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=timeout)


def git(cwd, *args, check=True):
    p = run(["git", *args], cwd=cwd)
    if check and p.returncode != 0:
        raise ToolError(f"git {' '.join(args)} failed in {cwd}: {(p.stderr or p.stdout).strip()}")
    return p


# ---------------------------------------------------------------- links and trees

def is_link_dir(path) -> bool:
    """True for a directory junction or directory symlink (the entry itself, never its target)."""
    try:
        st = os.lstat(path)
    except OSError:
        return False
    attrs = getattr(st, "st_file_attributes", 0)
    if not attrs & REPARSE_ATTR or not attrs & DIR_ATTR:
        return False
    return getattr(st, "st_reparse_tag", LINK_TAGS[0]) in LINK_TAGS


def tree_scan(path):
    """Walk `path` without ever entering a junction/symlink/reparse dir. Returns (bytes, [link dirs])."""
    size, links, stack = 0, [], [str(path)]
    while stack:
        d = stack.pop()
        try:
            it = os.scandir(d)
        except OSError:
            continue
        with it:
            for e in it:
                try:
                    st = e.stat(follow_symlinks=False)
                except OSError:
                    continue
                attrs = getattr(st, "st_file_attributes", 0)
                if attrs & REPARSE_ATTR and attrs & DIR_ATTR:
                    if getattr(st, "st_reparse_tag", LINK_TAGS[0]) in LINK_TAGS:
                        links.append(e.path)
                    continue  # never descend into a reparse dir
                if attrs & DIR_ATTR or stat.S_ISDIR(st.st_mode):
                    stack.append(e.path)
                else:
                    size += st.st_size
    return size, links


def remove_link(path):
    """Remove one junction with `cmd /c rmdir` (no /s: removes the link only, never the target's content)."""
    p = run(["cmd", "/c", "rmdir", os.path.normpath(str(path))])
    if os.path.lexists(path):
        raise ToolError(f"could not remove link {path}: {(p.stderr or p.stdout).strip()}")


def make_junction(link, target):
    p = run(["cmd", "/c", "mklink", "/J", os.path.normpath(str(link)), os.path.normpath(str(target))])
    if p.returncode != 0 or not is_link_dir(link):
        raise ToolError(f"mklink /J {link} -> {target} failed: {(p.stderr or p.stdout).strip()}")


def _force_delete(func, path, exc=None, *rest):
    try:
        os.chmod(path, stat.S_IWRITE)
        func(path)
    except OSError:
        pass


def rmtree_safe(path):
    """Delete a directory tree after removing every junction/symlink inside it (targets stay untouched)."""
    if is_link_dir(path):
        remove_link(path)
        return
    _, links = tree_scan(path)
    for link in links:
        remove_link(link)
    kw = {"onexc": _force_delete} if sys.version_info >= (3, 12) else {"onerror": _force_delete}
    shutil.rmtree(path, **kw)
    if os.path.exists(path):
        raise ToolError(f"could not fully delete {path}")


def gb(n) -> float:
    return n / (1024.0 ** 3)


# ---------------------------------------------------------------- git worktrees

@dataclass
class Worktree:
    path: str
    head: str = ""
    branch: str | None = None
    detached: bool = False
    locked: bool = False
    prunable: bool = False
    is_main: bool = False
    name: str = ""


@dataclass
class Ctx:
    root: Path          # any checkout of the repo: where git commands run
    main: Path = None   # the main checkout
    current: set = field(default_factory=set)  # normalised paths that must never be removed

    @staticmethod
    def discover(start: Path | None = None) -> "Ctx":
        root = Path(start) if start else Path(__file__).resolve().parents[2]
        ctx = Ctx(root=root)
        wts = list_worktrees(ctx)
        ctx.main = Path(wts[0].path)
        ctx.current.add(norm(root))
        top = run(["git", "rev-parse", "--show-toplevel"], cwd=os.getcwd())
        if top.returncode == 0 and top.stdout.strip():
            ctx.current.add(norm(top.stdout.strip()))
        ctx.current.add(norm(os.getcwd()))
        return ctx


def list_worktrees(ctx: Ctx) -> list[Worktree]:
    out = git(ctx.root, "worktree", "list", "--porcelain").stdout
    res: list[Worktree] = []
    for block in re.split(r"\n\s*\n", out.strip()):
        wt = None
        for line in block.splitlines():
            key, _, val = line.partition(" ")
            if key == "worktree":
                wt = Worktree(path=os.path.normpath(val), is_main=not res)
                wt.name = os.path.basename(wt.path)
            elif wt is None:
                continue
            elif key == "HEAD":
                wt.head = val
            elif key == "branch":
                wt.branch = val[len("refs/heads/"):] if val.startswith("refs/heads/") else val
            elif key == "detached":
                wt.detached = True
            elif key == "locked":
                wt.locked = True
            elif key == "prunable":
                wt.prunable = True
        if wt:
            res.append(wt)
    if not res:
        raise ToolError("git worktree list returned nothing")
    return res


def admin_dir(wt: Worktree) -> str | None:
    f = os.path.join(wt.path, ".git")
    try:
        if os.path.isfile(f):
            with open(f, encoding="utf-8") as fh:
                line = fh.read().strip()
            if line.startswith("gitdir:"):
                return os.path.normpath(line[7:].strip())
        elif os.path.isdir(f):
            return f
    except OSError:
        pass
    return None


def activity_ts(wt: Worktree, commit_ts: float) -> float:
    """Newest sign of life: last commit, the worktree's creation, its HEAD / reflog changes."""
    best = commit_ts
    cands = [os.path.join(wt.path, ".git")]
    ad = admin_dir(wt)
    if ad:
        cands += [os.path.join(ad, "HEAD"), os.path.join(ad, "logs", "HEAD")]  # not index: git status rewrites it
    for c in cands:
        try:
            st = os.stat(c)
            best = max(best, st.st_mtime, getattr(st, "st_birthtime", st.st_ctime))
        except OSError:
            pass
    return best


LANDED = ("origin/dev", "origin/main")  # dev first: the development branch and the default base (CLAUDE.md "Branches")


def landed_refs(ctx: Ctx) -> list:
    # Remote branches that count as "landed", in preference order, keeping only those that exist.
    return [r for r in LANDED
            if git(ctx.main, "rev-parse", "--verify", "--quiet", r, check=False).returncode == 0]


@dataclass
class Info:
    wt: Worktree
    exists: bool = True
    merged: bool | None = None
    dirty: int = 0
    unpushed: int = 0
    commit_ts: float = 0.0
    idle_days: float = 0.0
    build_bytes: int = 0
    build_trees: int = 0
    reasons: list = field(default_factory=list)

    @property
    def removable(self) -> bool:
        return not self.reasons


def inspect(ctx: Ctx, wt: Worktree, keep: set, only: set, days: float, pushed_ok: bool, sizes: bool) -> Info:
    inf = Info(wt)
    inf.exists = os.path.isdir(wt.path)
    if only and wt.name not in only and (wt.branch or "") not in only:
        inf.reasons.append("not in --only")
    if wt.name in keep or (wt.branch or "") in keep:
        inf.reasons.append("in --keep")
    if wt.is_main:
        inf.reasons.append("main checkout")
    if norm(wt.path) in ctx.current:
        inf.reasons.append("current worktree")
    if wt.locked:
        inf.reasons.append("locked")
    if wt.prunable or not inf.exists:
        inf.exists = False
        inf.reasons.append("folder missing (git worktree prune)")
        return inf
    if wt.head:
        inf.merged = None
        for ref in landed_refs(ctx):
            p = git(ctx.main, "merge-base", "--is-ancestor", wt.head, ref, check=False)
            if p.returncode == 0:
                inf.merged = True
                break
            if p.returncode == 1:
                inf.merged = False
        n = git(ctx.main, "rev-list", "--count", wt.head, "--not", "--remotes", check=False).stdout.strip()
        inf.unpushed = int(n) if n.isdigit() else 0
        t = git(ctx.main, "log", "-1", "--format=%ct", wt.head, check=False).stdout.strip()
        inf.commit_ts = float(t) if t.isdigit() else 0.0
    st = git(wt.path, "status", "--porcelain", check=False).stdout.splitlines()
    inf.dirty = len(st)
    inf.idle_days = (time.time() - activity_ts(wt, inf.commit_ts)) / DAY
    if sizes:
        for e in sorted(os.listdir(wt.path)):
            full = os.path.join(wt.path, e)
            if e.startswith("build-") and os.path.isdir(full) and not is_link_dir(full):
                inf.build_bytes += tree_scan(full)[0]
                inf.build_trees += 1
    if inf.merged is None:
        inf.reasons.append("origin/dev and origin/main missing")
    elif not inf.merged and not (pushed_ok and inf.unpushed == 0):
        inf.reasons.append("not merged into origin/dev or origin/main")
    if inf.dirty:
        inf.reasons.append("uncommitted changes")
    if inf.unpushed:
        inf.reasons.append("unpushed commits")
    if inf.idle_days < days:
        inf.reasons.append(f"active {inf.idle_days:.1f}d ago (< --days)")
    return inf


def print_table(infos: list, out=print):
    rows = [("worktree", "branch", "merged", "dirty", "unpushed", "age d", "build GB", "decision")]
    for i in infos:
        w = i.wt
        merged = "?" if i.merged is None else ("yes" if i.merged else "no")
        dirty = "-" if not i.exists else (str(i.dirty) if i.dirty else "no")
        age = f"{(time.time() - i.commit_ts) / DAY:.1f}" if i.commit_ts else "-"
        build = f"{gb(i.build_bytes):.2f} ({i.build_trees})" if i.build_trees else "-"
        branch = w.branch or ("(detached)" if w.detached else "-")
        dec = "REMOVE" if i.removable else "keep: " + "; ".join(i.reasons)
        rows.append((w.name, branch, merged, dirty, str(i.unpushed) if i.exists else "-", age, build, dec))
    widths = [max(len(r[c]) for r in rows) for c in range(len(rows[0]) - 1)]
    for r in rows:
        out("  ".join(r[c].ljust(widths[c]) for c in range(len(widths))) + "  " + r[-1])


# ---------------------------------------------------------------- new

def cmd_new(ctx: Ctx, name: str, base: str | None = None, fetch: bool = False, sync: bool = True,
            modules: str = "<modules>", out=print) -> Path:
    if not NAME_RE.match(name):
        raise ToolError(f"bad worktree name {name!r}: use letters, digits, '.', '_', '-'")
    target = ctx.main / ".claude" / "worktrees" / name
    branch = f"claude/{name}"
    if target.exists():
        raise ToolError(f"{target} already exists")
    if git(ctx.main, "show-ref", "--verify", "--quiet", f"refs/heads/{branch}", check=False).returncode == 0:
        raise ToolError(f"branch {branch} already exists (pick another name, or delete the stale branch)")
    if fetch:
        out("git fetch origin ...")
        p = git(ctx.main, "fetch", "origin", "--prune", check=False)
        if p.returncode:
            out(f"warning: fetch failed: {(p.stderr or '').strip()}")
    if base is None:
        refs = landed_refs(ctx)
        if not refs:
            raise ToolError("origin/dev and origin/main not found (run `git fetch origin`), or pass --base")
        base = refs[0]
    out(f"git worktree add --no-track {target} -b {branch} {base}")
    git(ctx.main, "worktree", "add", "--no-track", str(target), "-b", branch, base)

    for d in LINKED:
        src = ctx.main / d
        dst = target / d
        if not src.exists():
            out(f"  skip {d}: {src} does not exist")
        elif os.path.lexists(dst):
            out(f"  skip {d}: already present in the new worktree")
        else:
            make_junction(dst, src)
            out(f"  junction {d} -> {src}")

    store = target / "roadmap-store" / "roadmap_store.py"
    if sync and store.is_file():
        out("roadmap_store.py sync ...")
        try:
            p = run([sys.executable, os.path.join("roadmap-store", "roadmap_store.py"), "sync"], cwd=target, timeout=180)
            tail = (p.stdout + p.stderr).strip().splitlines()[-3:]
            out(("  sync ok: " if p.returncode == 0 else f"  warning: sync exit {p.returncode}: ") + " | ".join(tail))
        except (OSError, subprocess.TimeoutExpired) as ex:
            out(f"  warning: sync failed: {ex}")
    elif sync:
        out("  skip sync: roadmap-store/roadmap_store.py not found")

    if not modules.split(";")[0] == "core":
        modules = "core;" + modules
    out("")
    out(f"Worktree ready: {target}")
    out(f"Branch {branch} from {base}.")
    out("Next, in PowerShell (one build tree per worktree: `wt` gives build-wt-msvc / build-wt-clang):")
    out(f'  cd "{target}"')
    out(f"  python roadmap-store\\roadmap_store.py claim <group> --by {name}")
    out(f'  & .\\build_agent.cmd --% msvc wt "{modules}"     # fast tier; read the summary, not the log')
    out(f'  & .\\build_agent.cmd --% clang wt "{modules}"    # sim code too (everything but render/audio/video/UI)')
    out("  .\\build_asan.cmd                                      # engine/jobs or other threading code")
    out("Finish: commit, then tools\\land_on_dev.cmd (merges origin/dev in, fast-forwards dev; no PR, main is released from dev).")
    out(f"Remove it once it is on dev:  tools\\clean_worktrees.cmd --apply --only {name}")
    return target


# ---------------------------------------------------------------- clean

def newest_activity(tree: str) -> float:
    best = 0.0
    for p in (tree, os.path.join(tree, ".ninja_log"), os.path.join(tree, ".ninja_deps"),
              os.path.join(tree, "CMakeCache.txt"), os.path.join(tree, "tmp"), os.path.join(tree, "Testing")):
        try:
            best = max(best, os.stat(p).st_mtime)
        except OSError:
            pass
    return best


def cmd_clean(ctx: Ctx, apply: bool = False, days: float = 2.0, keep=(), only=(), pushed_ok: bool = False,
              trees: bool = False, main_trees: bool = False, sizes: bool = True, fetch: bool = False,
              out=print) -> dict:
    if fetch:
        out("git fetch origin --prune ...")
        git(ctx.main, "fetch", "origin", "--prune", check=False)
    if not landed_refs(ctx):
        out("warning: origin/dev and origin/main not found: nothing counts as merged (run `git fetch origin`)")
    keep, only = set(keep), set(only)
    wts = list_worktrees(ctx)
    infos = [inspect(ctx, w, keep, only, days, pushed_ok, sizes) for w in wts]
    print_table(infos, out)
    removable = [i for i in infos if i.removable]
    total = sum(i.build_bytes for i in removable)
    out("")
    out(f"{len(infos)} worktrees, {len(removable)} removable (merged, clean, nothing unpushed, idle >= {days:g} d)"
        + (f", their build trees {gb(total):.1f} GB" if sizes else ""))
    result = {"removed": [], "failed": [], "trees_removed": []}
    if not apply:
        out("DRY RUN: nothing removed. Add --apply to remove the REMOVE rows." if removable else "DRY RUN: nothing to remove.")
    for i in removable if apply else []:
        w = i.wt
        out(f"removing {w.name} ({w.branch or 'detached'}) ...")
        try:
            _, links = tree_scan(w.path)
            for link in links:
                remove_link(link)
            args = ["worktree", "remove"] + (["--force"] if i.dirty else []) + [w.path]
            git(ctx.main, *args)
            if os.path.exists(w.path):  # git left ignored files behind (locked file, long path)
                rmtree_safe(w.path)
            result["removed"].append(w.name)
            out(f"  removed ({len(links)} link(s) unlinked first)")
        except (ToolError, OSError) as ex:
            result["failed"].append(w.name)
            out(f"  FAILED: {ex}")
            if os.path.isdir(w.path):
                out("  note: its junctions are gone; recreate them or remove the worktree by hand")
    if apply and any(i.wt.prunable for i in infos):
        git(ctx.main, "worktree", "prune", check=False)
        out("git worktree prune (folders that no longer exist)")
    if trees:
        removed = set(result["removed"])
        for i in infos:
            if i.wt.name in removed or not i.exists or (i.wt.is_main and not main_trees):
                continue
            if only and "not in --only" in i.reasons:
                continue
            _prune_trees(i.wt, days, apply, out, result)
    return result


def _prune_trees(wt: Worktree, days: float, apply: bool, out, result: dict):
    for e in sorted(os.listdir(wt.path)):
        full = os.path.join(wt.path, e)
        if not (e.startswith("build-") and os.path.isdir(full)) or is_link_dir(full):
            continue
        if wt.is_main and e in MAIN_TREES_KEPT:
            continue
        age = (time.time() - newest_activity(full)) / DAY
        size = tree_scan(full)[0]
        stale = age >= days
        label = "DELETE" if stale and apply else ("stale " if stale else "recent")
        out(f"  {label} {wt.name}/{e}  {gb(size):.2f} GB  idle {age:.1f} d")
        if stale and apply:
            try:
                rmtree_safe(full)
                result["trees_removed"].append(f"{wt.name}/{e}")
            except (ToolError, OSError) as ex:
                out(f"    FAILED: {ex}")


# ---------------------------------------------------------------- selftest

def selftest() -> int:
    """Everything runs in a throwaway repository under %TEMP%: the real worktrees are never touched."""
    tmp = Path(tempfile.mkdtemp(prefix="wt_selftest_"))
    checks = {"n": 0}
    log: list[str] = []

    def check(cond, msg):
        checks["n"] += 1
        if not cond:
            raise AssertionError(msg)

    def g(cwd, *a):
        return git(cwd, *a).stdout.strip()

    try:
        main = tmp / "main"
        main.mkdir()
        g(main, "init", "-q", "-b", "main")
        g(main, "config", "user.name", "selftest")
        g(main, "config", "user.email", "selftest@example.invalid")
        (main / ".gitignore").write_text(
            "/.claude/worktrees/\n/Data/\n/extracted/\n/toolchain/\n/roadmap-store/\n/build-*/\n")
        (main / "x.txt").write_text("x\n")
        g(main, "add", "-A")
        g(main, "commit", "-q", "-m", "init")
        g(main, "update-ref", "refs/remotes/origin/main", "HEAD")
        for d in ("Data", "extracted", "toolchain"):
            (main / d).mkdir()
            (main / d / "keep.txt").write_text("real\n")
        (main / "roadmap-store").mkdir()
        (main / "roadmap-store" / "roadmap_store.py").write_text(
            "import sys\nprint('fake sync', sys.argv[1:])\nsys.exit(1)\n")  # a failing sync must only warn
        ctx = Ctx(root=main, main=main, current={norm(main)})

        a = cmd_new(ctx, "st-a", out=log.append)                     # no origin/dev yet: falls back to origin/main
        check(g(a, "rev-parse", "HEAD") == g(main, "rev-parse", "origin/main"), "fallback base is not origin/main")
        check(git(a, "config", "--get", "branch.claude/st-a.remote", check=False).stdout.strip() == "",
              "worktree branch must not track its base")
        for d in LINKED:
            check(is_link_dir(a / d), f"{d} is not a junction")
            check(os.path.isdir(a / d), f"{d} junction does not resolve")
        check((a / "Data" / "keep.txt").read_text() == "real\n", "Data junction content")
        check(any("warning: sync exit 1" in ln for ln in log), "failing sync should warn, not abort")
        check(g(a, "branch", "--show-current") == "claude/st-a", "branch name")
        for bad in ("st-a", "bad name"):
            try:
                cmd_new(ctx, bad, out=log.append)
                check(False, f"name {bad!r} accepted")
            except ToolError:
                pass

        dv = cmd_new(ctx, "st-dev", sync=False, out=log.append)       # work that landed on dev, not on main
        (dv / "z.txt").write_text("z\n")
        g(dv, "add", "z.txt")
        g(dv, "commit", "-q", "-m", "landed")
        g(main, "update-ref", "refs/remotes/origin/dev", g(dv, "rev-parse", "HEAD"))
        c_ = cmd_new(ctx, "st-c", sync=False, out=log.append)         # default base is now origin/dev
        check(g(c_, "rev-parse", "HEAD") == g(main, "rev-parse", "origin/dev"), "default base is not origin/dev")

        b = cmd_new(ctx, "st-b", sync=False, out=log.append)          # unmerged work
        (b / "y.txt").write_text("y\n")
        g(b, "add", "y.txt")
        g(b, "commit", "-q", "-m", "work")
        d_ = cmd_new(ctx, "st-dirty", sync=False, out=log.append)     # merged but dirty
        (d_ / "scratch.txt").write_text("wip\n")
        k = cmd_new(ctx, "st-keep", sync=False, out=log.append)       # merged, in --keep
        cur = cmd_new(ctx, "st-current", sync=False, out=log.append)  # merged, the tool is "run from" here
        ctx.current.add(norm(cur))

        # old build tree with a nested junction into the real Data folder
        old = b / "build-old"
        (old / "sub").mkdir(parents=True)
        (old / "f.bin").write_bytes(b"1" * 1000)
        make_junction(old / "sub" / "lnk", main / "Data")
        t_old = time.time() - 10 * DAY
        os.utime(old, (t_old, t_old))
        (b / "build-new").mkdir()

        res = cmd_clean(ctx, apply=True, days=1, out=log.append)
        check(not res["removed"], f"fresh worktrees must be protected by --days: {res['removed']}")
        res = cmd_clean(ctx, apply=False, days=0, out=log.append)
        check(not res["removed"] and a.exists(), "dry run removed something")

        res = cmd_clean(ctx, apply=True, days=0, keep=["st-keep"], out=log.append)
        check(sorted(res["removed"]) == ["st-a", "st-c", "st-dev"], f"removed set: {res['removed']}")
        check(not a.exists() and not dv.exists(), "removed folders still exist")
        for kept in (b, d_, k, cur):
            check(kept.exists(), f"{kept.name} was removed")
        check((main / "Data" / "keep.txt").read_text() == "real\n", "REAL DATA WAS TOUCHED")
        check((main / "roadmap-store" / "roadmap_store.py").exists(), "REAL STORE WAS TOUCHED")
        check("refs/heads/claude/st-a" in g(main, "show-ref", "--verify", "refs/heads/claude/st-a"), "branch must stay")
        res = cmd_clean(ctx, apply=False, days=1, trees=True, out=log.append)
        check(old.exists(), "tree dry run deleted a tree")
        res = cmd_clean(ctx, apply=True, days=1, trees=True, out=log.append)
        check(not old.exists() and (b / "build-new").exists(), f"tree pruning: {res['trees_removed']}")
        check(b.exists() and d_.exists() and k.exists(), "tree pruning must not remove worktrees")
        check((main / "Data" / "keep.txt").exists(), "nested junction target was deleted by tree pruning")

        res = cmd_clean(ctx, apply=True, days=0, only=["st-dirty"], out=log.append)
        check(not res["removed"], "dirty worktree removed")
        print("\n".join(log[-14:]))
        print(f"selftest OK ({checks['n']} checks)")
        return 0
    except AssertionError as ex:
        print("\n".join(log[-30:]))
        print(f"selftest FAILED: {ex}")
        return 1
    finally:
        try:
            for wt in list(tmp.glob("main/.claude/worktrees/*")):
                rmtree_safe(wt)
            rmtree_safe(tmp)
        except (ToolError, OSError) as ex:
            print(f"selftest cleanup left {tmp}: {ex}")


# ---------------------------------------------------------------- cli

def csv(s: str) -> list:
    return [x.strip() for x in s.split(",") if x.strip()]


def main(argv=None) -> int:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors="replace")
        except (AttributeError, ValueError):
            pass
    argv = list(sys.argv[1:] if argv is None else argv)
    if "--selftest" in argv:
        return selftest()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    n = sub.add_parser("new", help="create a worktree + junctions")
    n.add_argument("name")
    n.add_argument("--base", default=None, help="start point (default origin/dev, else origin/main)")
    n.add_argument("--fetch", action="store_true", help="git fetch origin first")
    n.add_argument("--no-sync", action="store_true", help="skip roadmap_store.py sync")
    n.add_argument("--modules", default="<modules>", help="module list shown in the next-step commands")
    c = sub.add_parser("clean", help="list / remove merged worktrees (dry run unless --apply)")
    c.add_argument("--apply", action="store_true")
    c.add_argument("--days", type=float, default=2.0,
                   help="minimum idle days before a worktree or build tree is removed (default 2)")
    c.add_argument("--keep", default="", help="comma list of worktree or branch names to keep")
    c.add_argument("--only", default="", help="comma list: consider only these worktrees")
    c.add_argument("--pushed", action="store_true",
                   help="also remove clean worktrees whose commits are all on a remote branch")
    c.add_argument("--trees", action="store_true", help="prune stale build-* trees in the kept worktrees")
    c.add_argument("--main", action="store_true", help="with --trees: include the main checkout")
    c.add_argument("--no-size", action="store_true", help="skip the build-tree size column")
    c.add_argument("--fetch", action="store_true", help="git fetch origin first")
    a = ap.parse_args(argv)
    try:
        ctx = Ctx.discover()
        if a.cmd == "new":
            cmd_new(ctx, a.name, a.base, a.fetch, not a.no_sync, a.modules)
        else:
            res = cmd_clean(ctx, a.apply, a.days, csv(a.keep), csv(a.only), a.pushed, a.trees, a.main,
                            not a.no_size, a.fetch)
            return 1 if res["failed"] else 0
    except ToolError as ex:
        print(f"error: {ex}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
