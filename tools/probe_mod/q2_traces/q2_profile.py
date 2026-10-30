"""q2_profile.py - backs up and restores the player's FoC options/profile around a Q2 run (stdlib only).

The game keeps the player's options and remembered menu choices in the registry
(HKCU\\Software\\Petroglyph\\StarWars FOC, whole tree) and writes autosaves / the last-game recording into
%USERPROFILE%\\Saved Games\\Petroglyph\\Empire At War - Forces of Corruption\\{Save,Recordings}.

  python -I q2_profile.py snapshot <run_dir>   registry values + file hashes -> <run_dir>\\profile_backup\\
                                               (files up to 64 MB are copied; bigger ones are only hashed)
  python -I q2_profile.py restore  <run_dir>   puts every registry value back (deletes values/keys the run
                                               added), puts back changed or deleted files from the backup,
                                               moves files the run created into <run_dir>\\profile_new\\,
                                               then re-reads everything and verifies (exit 0 = verified)
  python -I q2_profile.py diff     <run_dir>   prints what differs from the snapshot, changes nothing

Restore never deletes a file: a file the run created is moved into the run folder. A bigger-than-64 MB file
that changed is reported (exit 2) and left alone.
"""
import hashlib
import json
import os
import shutil
import sys
import winreg

REG_ROOT = r"Software\Petroglyph\StarWars FOC"
SAVED = os.path.join(os.environ.get("USERPROFILE", ""), "Saved Games", "Petroglyph",
                     "Empire At War - Forces of Corruption")
FILE_DIRS = ["Save", "Recordings"]
COPY_LIMIT = 64 * 1024 * 1024


def _sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def _enc(v, t):
    if isinstance(v, bytes):
        return {"type": t, "hex": v.hex()}
    return {"type": t, "value": v}


def _dec(e):
    return bytes.fromhex(e["hex"]) if "hex" in e else e["value"]


def read_reg(sub=REG_ROOT):
    """{key path: {value name: {type, value|hex}}} for the whole tree (missing root = {})."""
    out = {}
    try:
        k = winreg.OpenKey(winreg.HKEY_CURRENT_USER, sub)
    except FileNotFoundError:
        return out
    with k:
        vals = {}
        i = 0
        while True:
            try:
                n, v, t = winreg.EnumValue(k, i)
            except OSError:
                break
            vals[n] = _enc(v, t)
            i += 1
        out[sub] = vals
        i = 0
        subs = []
        while True:
            try:
                subs.append(winreg.EnumKey(k, i))
            except OSError:
                break
            i += 1
    for s in sorted(subs):
        out.update(read_reg(sub + "\\" + s))
    return out


def read_files():
    out = {}
    for d in FILE_DIRS:
        root = os.path.join(SAVED, d)
        if not os.path.isdir(root):
            continue
        for n in sorted(os.listdir(root)):
            p = os.path.join(root, n)
            if os.path.isfile(p):
                out[os.path.join(d, n)] = {"sha256": _sha(p), "size": os.path.getsize(p)}
    return out


def snapshot(run_dir):
    bdir = os.path.join(run_dir, "profile_backup")
    os.makedirs(os.path.join(bdir, "files"), exist_ok=True)
    files = read_files()
    for rel, e in files.items():
        if e["size"] <= COPY_LIMIT:
            dst = os.path.join(bdir, "files", rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(os.path.join(SAVED, rel), dst)
            e["copied"] = True
    snap = {"registry_root": "HKCU\\" + REG_ROOT, "registry": read_reg(), "saved_games": SAVED, "files": files}
    with open(os.path.join(bdir, "profile.json"), "w", encoding="utf-8") as f:
        json.dump(snap, f, indent=1)
    print("profile snapshot: %d registry keys, %d files -> %s" % (len(snap["registry"]), len(files), bdir))
    return 0


def _load(run_dir):
    p = os.path.join(run_dir, "profile_backup", "profile.json")
    if not os.path.isfile(p):
        return None
    return json.load(open(p, encoding="utf-8"))


def diff(snap):
    """List of human-readable differences between the snapshot and now."""
    out = []
    now_r, old_r = read_reg(), snap["registry"]
    for k in sorted(set(now_r) | set(old_r)):
        if k not in old_r:
            out.append("reg key added %s" % k)
            continue
        if k not in now_r:
            out.append("reg key removed %s" % k)
            continue
        for n in sorted(set(now_r[k]) | set(old_r[k])):
            if now_r[k].get(n) != old_r[k].get(n):
                out.append("reg value %s\\%s: %s -> %s" % (k, n or "(default)", old_r[k].get(n), now_r[k].get(n)))
    now_f, old_f = read_files(), snap["files"]
    for rel in sorted(set(now_f) | set(old_f)):
        a, b = old_f.get(rel), now_f.get(rel)
        if a is None:
            out.append("file added %s" % rel)
        elif b is None:
            out.append("file removed %s" % rel)
        elif a["sha256"] != b["sha256"]:
            out.append("file changed %s" % rel)
    return out


def _delete_key_tree(sub):
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, sub) as k:
        subs = []
        i = 0
        while True:
            try:
                subs.append(winreg.EnumKey(k, i))
            except OSError:
                break
            i += 1
    for s in subs:
        _delete_key_tree(sub + "\\" + s)
    winreg.DeleteKey(winreg.HKEY_CURRENT_USER, sub)


def restore(run_dir):
    snap = _load(run_dir)
    if snap is None:
        print("no profile snapshot in %s: nothing to restore" % run_dir)
        return 0
    before = diff(snap)
    if not before:
        print("profile unchanged by the run (registry and saved-games files verified)")
        return 0
    for d in before:
        print("changed: " + d)
    old_r, now_r = snap["registry"], read_reg()
    for k in sorted(now_r, key=len, reverse=True):        # keys the run added, deepest first
        if k not in old_r:
            _delete_key_tree(k)
    for k, vals in old_r.items():
        with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, k, 0, winreg.KEY_ALL_ACCESS) as h:
            for n in now_r.get(k, {}):
                if n not in vals:
                    winreg.DeleteValue(h, n)
            for n, e in vals.items():
                if now_r.get(k, {}).get(n) != e:
                    winreg.SetValueEx(h, n, 0, e["type"], _dec(e))
    left = 0
    now_f = read_files()
    for rel in sorted(set(now_f) | set(snap["files"])):
        a, b = snap["files"].get(rel), now_f.get(rel)
        src = os.path.join(SAVED, rel)
        if a is None:
            dst = os.path.join(run_dir, "profile_new", rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.move(src, dst)
            print("moved new file %s -> %s" % (rel, dst))
        elif b is None or a["sha256"] != b["sha256"]:
            if not a.get("copied"):
                print("NOT RESTORED (too big to back up): %s" % rel)
                left += 1
                continue
            shutil.copy2(os.path.join(run_dir, "profile_backup", "files", rel), src)
            print("restored file %s" % rel)
    after = diff(snap)
    for d in after:
        print("STILL DIFFERENT: " + d)
    ok = not after and not left
    print("profile restored and verified" if ok else "PROFILE RESTORE NOT VERIFIED")
    with open(os.path.join(run_dir, "profile_restore.json"), "w", encoding="utf-8") as f:
        json.dump({"changed_by_run": before, "still_different": after, "verified": ok}, f, indent=1)
    return 0 if ok else 2


def main(argv):
    if len(argv) != 2 or argv[0] not in ("snapshot", "restore", "diff"):
        print(__doc__)
        return 1
    cmd, run_dir = argv
    if cmd == "snapshot":
        return snapshot(run_dir)
    if cmd == "restore":
        return restore(run_dir)
    snap = _load(run_dir)
    if snap is None:
        print("no snapshot")
        return 1
    for d in diff(snap):
        print(d)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
