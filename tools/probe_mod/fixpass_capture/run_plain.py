"""run_plain.py - starts the unmodified FoC for the fix-pass screenshots (C2, C3) and adds nothing to the game folder.

Unlike g1.py it places no DLL. It reuses the kit's game-folder snapshot and its "another run is active" marker, and it
snapshots what the game can write outside its folder (saves, recordings, the player's registry profile). After the
game exits it compares everything and puts back the game's own [AutoSave] and AutoSave.rec, which playing overwrites.

  toolchain\\venv\\Scripts\\python.exe -I tools\\probe_mod\\fixpass_capture\\run_plain.py [--timeout 5400]

Drive the running game with g1_capture\\g1_ui.py (shot, click, key, ...). Ends when the game exits. Result: out\\plain_status.json
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
KIT = os.path.join(HERE, "..", "g1_capture")
sys.path.insert(0, KIT)
import g1  # noqa: E402

GAME = os.environ.get("ALAMO_GAME_DIR") or g1.DEFAULT_GAME
SAVED = os.path.join(os.environ["USERPROFILE"], "Saved Games", "Petroglyph", "Empire At War - Forces of Corruption")
REG = r"HKCU\Software\Petroglyph\StarWars FOC\Profiles\Profile0\Player"
OUT = os.path.join(HERE, "out")
# The game rewrites these by itself while playing; they are put back so the player's state is as it was.
OWN_AUTOSAVES = ["Save\\[AutoSave].PetroglyphFoC64Save", "Recordings\\AutoSave.rec"]


def tree(root):
    res = {}
    for sub in ("Save", "Recordings"):
        d = os.path.join(root, sub)
        for n in sorted(os.listdir(d)) if os.path.isdir(d) else []:
            p = os.path.join(d, n)
            if os.path.isfile(p):
                res[sub + "\\" + n] = g1.sha(p)
    return res


def reg_dump():
    return subprocess.run(["reg", "query", REG], capture_output=True, text=True).stdout.splitlines()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=5400)
    a = ap.parse_args()
    corr = os.path.join(GAME, "corruption")
    exe = os.path.join(corr, "StarWarsG.exe")
    if g1.pids_of_exe(exe):
        print("ERROR: the game is already running")
        return 9
    if not g1.process_running("steam.exe"):
        print("ERROR: Steam is not running")
        return 5
    control = os.path.join(KIT, "out", "control")
    cur = os.path.join(control, "current_run.json")
    if os.path.isfile(cur) and g1.pid_alive(g1.load_json(cur).get("pid", 0)):
        print("ERROR: a kit run is active")
        return 9
    os.makedirs(OUT, exist_ok=True)
    backup = os.path.join(OUT, "save_backup")
    shutil.rmtree(backup, ignore_errors=True)
    for rel in OWN_AUTOSAVES:
        src = os.path.join(SAVED, rel)
        if os.path.isfile(src):
            dst = os.path.join(backup, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)
    before = {"game": g1.Kit.snapshot(corr), "saved": tree(SAVED), "reg": reg_dump()}
    g1.save_json({"pid": os.getpid(), "mode": "fixpass_plain", "run_dir": OUT, "started": g1.now()}, cur)
    status = {"started": g1.now(), "timed_out": False}
    try:
        proc = subprocess.Popen([exe, "WINDOWED", "SCREENWIDTH=1920", "SCREENHEIGHT=1080"], cwd=corr)
        print("game started, pid", proc.pid, flush=True)
        t0 = time.time()
        seen = False
        while True:
            time.sleep(1)
            pids = g1.pids_of_exe(exe)
            seen = seen or bool(pids)
            if seen and not pids:
                break
            if not seen and time.time() - t0 > 180:
                status["error"] = "the game process never appeared"
                break
            if time.time() - t0 > a.timeout:
                status["timed_out"] = True
                for pid in pids:
                    g1.close_windows_of(pid)
                time.sleep(20)
                for pid in g1.pids_of_exe(exe):
                    g1.kill_pid(pid)
        status["exit_code"] = proc.poll()
    finally:
        os.remove(cur)
    time.sleep(2)
    after = {"game": g1.Kit.snapshot(corr), "saved": tree(SAVED), "reg": reg_dump()}
    gm = []
    for n, v in before["game"].items():
        if n not in after["game"]:
            gm.append(n + " missing")
        elif after["game"][n]["sha"] != v["sha"]:
            gm.append(n + " hash differs")
    for n in after["game"]:
        if n not in before["game"]:
            gm.append(n + " new")
    status["game_folder_changes"] = gm
    status["game_folder_changes_not_game_written"] = [m for m in gm if not g1.GAME_WRITTEN.match(m.rsplit(" ", 2)[0] if m.endswith("differs") else m.rsplit(" ", 1)[0])]
    restored = []
    for rel in OWN_AUTOSAVES:
        bk = os.path.join(backup, rel)
        dst = os.path.join(SAVED, rel)
        if os.path.isfile(bk) and (not os.path.isfile(dst) or g1.sha(dst) != g1.sha(bk)):
            shutil.copy2(bk, dst)
            restored.append(rel)
    status["autosaves_put_back"] = restored
    final = tree(SAVED)
    status["saved_changes"] = sorted(k for k in set(before["saved"]) | set(final) if before["saved"].get(k) != final.get(k))
    status["reg_changes"] = sorted(set(before["reg"]) ^ set(after["reg"]))
    status["finished"] = g1.now()
    g1.save_json(status, os.path.join(OUT, "plain_status.json"))
    print(json.dumps(status, indent=2))
    print("PLAIN_RUN_DONE", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
