"""Runs a g1.py capture with some of the player's FoC graphics options changed, then puts them back.

The options live in HKCU\\Software\\Petroglyph\\StarWars FOC\\Profiles\\Profile0\\Player (REG_DWORD values such as
Bloom, HeatDistortion, SoftShadows, ShadowDetail). The game reads them at startup.

    python -I g1_options.py --set Bloom=1 HeatDistortion=1 -- apitrace --non-interactive --drive K ...
    python -I g1_options.py --restore          # puts back a backup left by an interrupted run

Before changing anything the current values are written to out\\state\\options_backup.json; they are restored in a
finally block (also when g1.py fails) and read back to verify. Exit code: g1.py's, or 7 if the restore did not verify.
"""
import json
import os
import subprocess
import sys
import winreg

KIT = os.path.dirname(os.path.abspath(__file__))
KEY = r"Software\Petroglyph\StarWars FOC\Profiles\Profile0\Player"
BACKUP = os.path.join(KIT, "out", "state", "options_backup.json")
ALLOWED = {"Bloom", "HeatDistortion", "SoftShadows", "ShadowDetail", "ShadowVolumes", "ShaderDetail"}


def read(names):
    out = {}
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, KEY) as k:
        for n in names:
            try:
                v, t = winreg.QueryValueEx(k, n)
                out[n] = {"value": v, "type": t}
            except FileNotFoundError:
                out[n] = None
    return out


def write(values):
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, KEY, 0, winreg.KEY_SET_VALUE) as k:
        for n, e in values.items():
            if e is None:
                try:
                    winreg.DeleteValue(k, n)
                except FileNotFoundError:
                    pass
            else:
                winreg.SetValueEx(k, n, 0, e["type"], e["value"])


def restore():
    if not os.path.isfile(BACKUP):
        print("no options backup: nothing to restore")
        return True
    saved = json.load(open(BACKUP, encoding="utf-8"))
    write(saved)
    ok = read(list(saved)) == saved
    print("options restored and verified" if ok else "OPTIONS RESTORE NOT VERIFIED: %s" % saved)
    if ok:
        os.replace(BACKUP, BACKUP + ".done")
    return ok


def main(argv):
    if argv[:1] == ["--restore"]:
        return 0 if restore() else 7
    if "--" not in argv or argv[:1] != ["--set"]:
        print(__doc__)
        return 1
    sets = argv[1:argv.index("--")]
    g1_args = argv[argv.index("--") + 1:]
    want = {}
    for s in sets:
        n, v = s.split("=", 1)
        if n not in ALLOWED:
            raise SystemExit("option %s not allowed (%s)" % (n, ", ".join(sorted(ALLOWED))))
        want[n] = int(v)
    if os.path.isfile(BACKUP):
        raise SystemExit("an options backup is pending (%s): run --restore first" % BACKUP)
    saved = read(list(want))
    os.makedirs(os.path.dirname(BACKUP), exist_ok=True)
    with open(BACKUP, "w", encoding="utf-8") as f:
        json.dump(saved, f, indent=1)
    print("options before: %s" % {n: (e or {}).get("value") for n, e in saved.items()})
    rc = 1
    try:
        write({n: {"value": v, "type": winreg.REG_DWORD} for n, v in want.items()})
        print("options set: %s" % want)
        rc = subprocess.call([sys.executable, "-I", os.path.join(KIT, "g1.py")] + g1_args)
    finally:
        if not restore():
            rc = 7
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
