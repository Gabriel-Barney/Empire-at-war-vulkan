"""G1 kit self-test: runs every mode end to end against a FAKE game folder (never the real install).

  toolchain\\venv\\Scripts\\python.exe -I tools\\probe_mod\\g1_capture\\selftest\\selftest.py

The fake folder (selftest\\work\\fakegame\\corruption) mimics the real one: StarWarsG.exe is fake_d3d9_game.exe
(a small D3D9 app, built by build_fake_game.cmd), d3d9.dll/dxgi.dll are the pinned DXVK, d3dx9_43.dll is a dummy
"proxy" and eaw_modifier_d3dx_original.dll a dummy "original", listed in a patch install JSON like the real one.
Checks: dry run changes nothing; dxvk / renderdoc / apitrace runs exit 0, produce screenshots / captures / a trace
and analysis, and leave every file byte-identical (recursive SHA-256); a run killed mid-way leaves a manifest that
`cleanup` restores, and a second `cleanup` is a no-op. Writes selftest\\calibration.json (D3D9 cull mode -> what
RenderDoc shows after DXVK) and selftest\\work\\selftest_report.json.
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
KIT = os.path.dirname(HERE)
ROOT = os.path.abspath(os.path.join(KIT, "..", "..", ".."))
CAP = os.path.join(ROOT, "toolchain", "capture")
WORK = os.path.join(HERE, "work")
FAKE = os.path.join(WORK, "fakegame")
CORR = os.path.join(FAKE, "corruption")
OUT = os.path.join(WORK, "out")
PY = sys.executable
G1 = os.path.join(KIT, "g1.py")
FAKE_EXE = os.path.join(HERE, "bin", "fake_d3d9_game.exe")
results = []
sys.path.insert(0, KIT)
from g1 import kill_pid, pids_of_exe  # noqa: E402


def sha(p):
    return hashlib.sha256(open(p, "rb").read()).hexdigest().upper()


def tree(d):
    out = {}
    for base, _, files in os.walk(d):
        for f in files:
            p = os.path.join(base, f)
            out[os.path.relpath(p, d)] = sha(p)
    return out


def check(name, ok, detail=""):
    results.append({"check": name, "ok": bool(ok), "detail": detail})
    print("%s  %s %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)


def make_fake():
    if os.path.isdir(FAKE):
        shutil.rmtree(FAKE)
    os.makedirs(os.path.join(CORR, "Data"))
    os.makedirs(os.path.join(FAKE, "GameData"))
    shutil.copy2(FAKE_EXE, os.path.join(CORR, "StarWarsG.exe"))
    shutil.copy2(os.path.join(CAP, "dxvk-3.1.1", "x64", "d3d9.dll"), CORR)
    shutil.copy2(os.path.join(CAP, "dxvk-3.1.1", "x64", "dxgi.dll"), CORR)
    open(os.path.join(CORR, "d3dx9_43.dll"), "wb").write(b"FAKE PROXY d3dx9_43 (patch chain head)\n" * 50)
    open(os.path.join(CORR, "eaw_modifier_d3dx_original.dll"), "wb").write(b"FAKE GENUINE d3dx9_43\n" * 80)
    open(os.path.join(CORR, "eaw_ai_pacing.ini"), "w").write("[pacing]\nbudget_ms=6\n")
    open(os.path.join(CORR, "_GraphicsInit.txt"), "w").write("")
    open(os.path.join(CORR, "Data", "keep.txt"), "w").write("data")
    files = {n: sha(os.path.join(CORR, n)).lower() for n in ("d3dx9_43.dll", "eaw_modifier_d3dx_original.dll", "eaw_ai_pacing.ini")}
    json.dump({"version": 1, "patch": "fake", "engine_sha256": sha(os.path.join(CORR, "StarWarsG.exe")).lower(), "files": files},
              open(os.path.join(CORR, "eaw_unified_component_regression_v2_install.json"), "w"), indent=2)


def g1(*args, timeout=600):
    cmd = [PY, "-I", G1] + list(args) + ["--game-dir", FAKE, "--out-dir", OUT]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return r


def last_status():
    return json.load(open(os.path.join(OUT, "last_status.json"), encoding="utf-8"))


def same_tree(before, after):
    diff = sorted(k for k in set(before) | set(after) if before.get(k) != after.get(k) and not os.path.basename(k).startswith("_"))
    return not diff, diff


def run_mode(mode, extra, during=None):
    before = tree(FAKE)
    t = None
    if during:
        t = threading.Thread(target=during)
        t.start()
    r = g1(mode, "--non-interactive", "--skip-steam-check", "--timeout", "40", "--env", "G1_FAKE_SECONDS=8", *extra)
    if t:
        t.join()
    st = last_status()
    open(os.path.join(WORK, "log_%s.txt" % mode), "w", encoding="utf-8").write(r.stdout + "\n" + r.stderr)
    check("%s exit 0" % mode, r.returncode == 0, "exit %d %s" % (r.returncode, st.get("error") or ""))
    ok, diff = same_tree(before, tree(FAKE))
    check("%s restores the fake folder byte-identically" % mode, ok, ", ".join(diff))
    check("%s restore verified by the kit" % mode, (st.get("cleanup") or {}).get("verified") is True)
    return st


def request(args, delay):
    def f():
        time.sleep(delay)
        subprocess.run([PY, "-I", G1] + args + ["--out-dir", OUT], capture_output=True)
    return f


def main():
    if not os.path.isfile(FAKE_EXE):
        subprocess.run(["cmd", "/c", os.path.join(HERE, "build_fake_game.cmd")], check=True)
    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    make_fake()
    t0 = tree(FAKE)

    # dry run: nothing changes
    r = g1("dxvk", "--dry-run", "--non-interactive", "--skip-steam-check")
    check("dry run exit 0", r.returncode == 0)
    check("dry run changes nothing", tree(FAKE) == t0)
    check("dry run plans the patch-chain swap", "replace d3dx9_43.dll" in r.stdout)

    # DXVK: timer screenshots + a requested one
    st = run_mode("dxvk", ["--shot-interval", "2"], during=request(["trigger", "--label", "hud"], 4))
    check("dxvk screenshots", len(st["screenshots"]) >= 2, str(len(st["screenshots"])))
    check("dxvk requested screenshot", any("hud_" in os.path.basename(p) for p in st["screenshots"]))
    check("dxvk log in run folder", any(n.endswith("_d3d9.log") for n in os.listdir(st["run_dir"])))
    check("dxvk patch chain was disabled during the run", any(a["file"] == "d3dx9_43.dll" for a in st["actions"]))

    # RenderDoc: two timed captures + one requested, extraction, analysis
    st = run_mode("renderdoc", ["--auto-trigger-sec", "2", "4", "--auto-labels", "t2", "t4"],
                  during=request(["trigger", "--label", "req"], 6))
    caps = st.get("captures", [])
    check("renderdoc captures", len(caps) >= 3, ", ".join(c["label"] for c in caps))
    facts = json.load(open(os.path.join(st["run_dir"], "analysis", "facts.json")))
    rds = facts.get("renderdoc", {})
    check("renderdoc extraction + analysis", len(rds) >= 3, "; ".join(facts.get("errors", [])))
    first = sorted(rds)[0] if rds else None
    if first:
        heat = rds[first].get("heat_shift")
        check("renderdoc heat shift measured", heat is not None and not heat.get("image_is_flat"), json.dumps(heat))
        summ = json.load(open(os.path.join(st["run_dir"], first)))
        draws = [d for d in summ["draws"] if not any("lit" in m.lower() for m in d["markers"])]
        if len(draws) >= 3:
            calib = {"source": "selftest fake_d3d9_game through DXVK 3.1.1 + RenderDoc 1.46",
                     "dxvk_cull_map": {"D3DCULL_CW": {"cull": draws[0]["cull"], "front_ccw": draws[0]["front_ccw"]},
                                       "D3DCULL_CCW": {"cull": draws[1]["cull"], "front_ccw": draws[1]["front_ccw"]},
                                       "D3DCULL_NONE": {"cull": draws[2]["cull"], "front_ccw": draws[2]["front_ccw"]}}}
            json.dump(calib, open(os.path.join(HERE, "calibration.json"), "w"), indent=2)
            m = calib["dxvk_cull_map"]
            check("cull calibration distinct", len({json.dumps(v, sort_keys=True) for v in m.values()}) == 3, json.dumps(m))

    # apitrace: whole-session trace, a marker, then a quit request (graceful close path)
    st = run_mode("apitrace", [], during=None)
    # second apitrace run exercising trigger + quit
    st = run_mode("apitrace", ["--grace", "5"], during=lambda: (request(["trigger", "--label", "m1"], 3)(), request(["quit"], 1)()))
    check("apitrace trace", any(c["label"] == "session" for c in st.get("captures", [])))
    check("apitrace marker", len(st.get("markers", [])) >= 1)
    check("apitrace quit request closed the game early", st["launch"] and not st["launch"]["timed_out"])
    facts = json.load(open(os.path.join(st["run_dir"], "analysis", "facts.json")))
    tr = list(facts.get("apitrace", {}).values())
    if tr:
        f = tr[0]
        check("apitrace engine cull default found", (f["cull"]["first_set"] or {}).get("value") == "D3DCULL_CW", json.dumps(f["cull"]["first_set"]))
        check("apitrace Colorization value found", "(0.3058824, 0.5882353, 0.9294118, 1.0)" in f["colorization_values_at_draw"])
        check("apitrace stencil NOTEQUAL found", any(x["state"].get("D3DRS_STENCILFUNC") == "D3DCMP_NOTEQUAL" for x in f["stencil_states_at_draw"]))
        check("apitrace heat clear found", f["heat_passes_found"] > 0)
        hs = f.get("heat_shift_apitrace", {})
        check("apitrace heat shift measured", "dx_px" in hs and not hs.get("image_is_flat"), json.dumps(hs))
    else:
        check("apitrace analysis", False, "; ".join(facts.get("errors", [])))

    # crash: kill the kit mid-run, then cleanup twice
    before = tree(FAKE)
    p = subprocess.Popen([PY, "-I", G1, "dxvk", "--non-interactive", "--skip-steam-check", "--timeout", "60", "--env",
                          "G1_FAKE_SECONDS=30", "--game-dir", FAKE, "--out-dir", OUT], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(5)
    p.kill()
    p.wait()
    for pid in pids_of_exe(os.path.join(CORR, "StarWarsG.exe")):  # the fake only, matched by full path
        kill_pid(pid)
    time.sleep(1)
    check("killed run left a manifest", os.path.isfile(os.path.join(OUT, "state", "manifest.json")))
    check("killed run left the folder modified", tree(FAKE) != before)
    r1 = g1("cleanup")
    ok, diff = same_tree(before, tree(FAKE))
    check("cleanup after a crash exit 0", r1.returncode == 0, r1.stdout[-400:])
    check("cleanup after a crash restores byte-identically", ok, ", ".join(diff))
    r2 = g1("cleanup")
    check("second cleanup is a no-op (exit 0)", r2.returncode == 0 and "nothing to restore" in r2.stdout)
    check("folder still identical after second cleanup", same_tree(before, tree(FAKE))[0])

    rep = {"passed": sum(r["ok"] for r in results), "failed": sum(not r["ok"] for r in results), "results": results}
    json.dump(rep, open(os.path.join(WORK, "selftest_report.json"), "w"), indent=2)
    print("SELFTEST %d passed, %d failed" % (rep["passed"], rep["failed"]))
    return 0 if rep["failed"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
