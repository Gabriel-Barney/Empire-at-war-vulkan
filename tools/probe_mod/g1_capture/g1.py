"""G1 in-game capture kit (roadmap step G1, approved by Gabriel 2026-10-27).

Run with the project venv:  toolchain\\venv\\Scripts\\python.exe -I tools\\probe_mod\\g1_capture\\g1.py <mode> [options]

Modes
  dxvk       DXVK HUD + DXVK log + timed / requested window screenshots
  renderdoc  RenderDoc frame captures (Vulkan, on top of DXVK): on request, on a timer, or F12 in the game
  apitrace   D3D9 API trace of the whole session (apitrace d3d9.dll wrapper -> native D3D9)
  cleanup    restore the game folder from the manifest (safe to run any number of times)
  status     print the state as JSON (game, prior patch chain, d3d9.dll, active run / manifest)
  trigger    ask the running capture for a capture now (--label NAME, --frames N)
  quit       ask the running capture to close the game (WM_CLOSE, forced after --grace seconds)
  analyze    analyse a finished run folder (--run-dir); also done automatically after each run

Every capture run: find the game, auto-restore a stale manifest, check the prior patch chain
(docs/re/PRIOR_PATCH.md), back it up + disable it, place only the files the mode needs (manifest
out\\state\\manifest.json, backups out\\state\\backup), launch, wait (timeout), collect logs, ALWAYS restore
(finally), verify by SHA-256 against the pre-run snapshot, then analyse. A JSON status file is written to
<run>\\status.json and out\\last_status.json.

Exit codes: 0 ok, 1 usage/internal, 2 game not found, 3 unexpected game-folder state, 4 capture tool missing,
5 Steam not running, 6 launch failed, 7 restore verification failed, 8 no capture produced, 9 run already active.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import traceback
import zlib

KIT = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(KIT, "..", "..", ".."))
CAP = os.path.join(ROOT, "toolchain", "capture")
TOOLS = {
    "dxvk_d3d9": os.path.join(CAP, "dxvk-3.1.1", "x64", "d3d9.dll"),
    "rdoc_dir": os.path.join(CAP, "renderdoc-1.46"),
    "qrenderdoc": os.path.join(CAP, "renderdoc-1.46", "qrenderdoc.exe"),
    "apitrace_d3d9": os.path.join(CAP, "apitrace-14.0", "lib", "wrappers", "d3d9.dll"),
    "apitrace": os.path.join(CAP, "apitrace-14.0", "bin", "apitrace.exe"),
    "d3dretrace": os.path.join(CAP, "apitrace-14.0", "bin", "d3dretrace.exe"),
}
DEFAULT_GAME = r"F:\SteamLibrary\steamapps\common\Star Wars Empire at War"
PATCH_JSON = "eaw_unified_component_regression_v2_install.json"
# Files the game itself rewrites on every launch; allowed to differ after a run.
GAME_WRITTEN = re.compile(r"^(_.*\.txt|.*\.log|temp\.rec|LogFile\.txt)$", re.I)
DXVK_HUD = "devinfo,fps,frametimes,drawcalls,submissions,pipelines,memory,api,version"


class G1Exit(Exception):
    def __init__(self, code, msg):
        super().__init__(msg)
        self.code = code


def now():
    return time.strftime("%Y-%m-%dT%H:%M:%S")


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest().upper()


def save_json(obj, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(obj, f, indent=2)
    os.replace(tmp, path)


def load_json(path):
    with open(path, "r", encoding="utf-8-sig") as f:
        return json.load(f)


class Kit:
    def __init__(self, a):
        self.a = a
        self.out = os.path.abspath(a.out_dir or os.path.join(KIT, "out"))
        self.state_dir = os.path.join(self.out, "state")
        self.manifest_path = os.path.join(self.state_dir, "manifest.json")
        self.backup_dir = os.path.join(self.state_dir, "backup")
        self.control = os.path.join(self.out, "control")
        self.current_run = os.path.join(self.control, "current_run.json")
        self.status = {"kit": "g1_capture", "mode": a.mode, "started": now(), "finished": None, "dry_run": a.dry_run,
                       "non_interactive": a.non_interactive, "result": "running", "exit_code": None, "error": None,
                       "game_dir": None, "run_dir": None, "warnings": [], "steps": [], "actions": [], "launch": None,
                       "captures": [], "screenshots": [], "markers": [], "cleanup": None, "analysis": None}

    # ------------------------------------------------------------------ log
    def step(self, m):
        self.status["steps"].append("%s %s" % (now(), m))
        print("  - " + m, flush=True)

    def warn(self, m):
        self.status["warnings"].append(m)
        print("  ! " + m, flush=True)

    # ------------------------------------------------------------------ game discovery
    def find_game(self):
        cands = []
        if self.a.game_dir:
            cands.append(self.a.game_dir)
        if os.environ.get("ALAMO_GAME_DIR"):
            cands.append(os.environ["ALAMO_GAME_DIR"])
        cands.append(DEFAULT_GAME)
        try:
            import winreg
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as k:
                sp = winreg.QueryValueEx(k, "SteamPath")[0]
            vdf = os.path.join(sp, "steamapps", "libraryfolders.vdf")
            if os.path.isfile(vdf):
                for m in re.finditer(r'"path"\s+"([^"]+)"', open(vdf, encoding="utf-8", errors="replace").read()):
                    cands.append(os.path.join(m.group(1).replace("\\\\", "\\"), "steamapps", "common",
                                              "Star Wars Empire at War"))
        except OSError:
            pass
        for c in cands:
            g = c.rstrip("\\/")
            if os.path.basename(g).lower() == "corruption":
                g = os.path.dirname(g)
            if os.path.isfile(os.path.join(g, "corruption", "StarWarsG.exe")):
                return os.path.abspath(g)
        return None

    @staticmethod
    def snapshot(d):
        s = {}
        for n in sorted(os.listdir(d)):
            p = os.path.join(d, n)
            if os.path.isfile(p):
                s[n] = {"sha": sha(p), "size": os.path.getsize(p)}
        return s

    @staticmethod
    def patch_info(corr):
        info = {"install_json": False, "proxy_sha": None, "original_sha": None, "engine_sha": None, "current": None,
                "state": "unknown"}
        pj = os.path.join(corr, PATCH_JSON)
        if os.path.isfile(pj):
            j = load_json(pj)
            info["install_json"] = True
            info["proxy_sha"] = j["files"]["d3dx9_43.dll"].upper()
            info["original_sha"] = j["files"]["eaw_modifier_d3dx_original.dll"].upper()
            info["engine_sha"] = j.get("engine_sha256", "").upper() or None
        d = os.path.join(corr, "d3dx9_43.dll")
        o = os.path.join(corr, "eaw_modifier_d3dx_original.dll")
        if not os.path.isfile(d):
            # no proxy, no original, no install JSON: the prior patch chain is not installed (vanilla folder,
            # as found on 2026-10-29); the capture needs no d3dx change then
            info["state"] = "no_chain" if not info["install_json"] and not os.path.isfile(o) else "no_d3dx9_43"
            return info
        info["current"] = sha(d)
        if not info["original_sha"] and os.path.isfile(o):
            info["original_sha"] = sha(o)
        if info["proxy_sha"] and info["current"] == info["proxy_sha"]:
            info["state"] = "enabled"
        elif info["original_sha"] and info["current"] == info["original_sha"]:
            info["state"] = "disabled"
        elif not os.path.isfile(o):
            info["state"] = "no_chain"
        return info

    # ------------------------------------------------------------------ manifest
    def save_manifest(self, man):
        if not self.a.dry_run:
            save_json(man, self.manifest_path)

    def do_replace(self, man, name, src, why):
        dst = os.path.join(man["corruption"], name)
        new_sha = sha(src)
        exists = os.path.isfile(dst)
        if exists and sha(dst) == new_sha:
            self.step("keep %s (already the wanted file)" % name)
            return
        act = {"op": "replace" if exists else "add", "file": name, "orig_sha": sha(dst) if exists else None,
               "new_sha": new_sha, "src": src, "why": why, "state": "pending"}
        self.step("%s %s <- %s (%s)" % (act["op"], name, src, why))
        self.status["actions"].append(act)
        if self.a.dry_run:
            return
        man["actions"].append(act)
        self.save_manifest(man)
        if exists:
            os.makedirs(self.backup_dir, exist_ok=True)
            bk = os.path.join(self.backup_dir, name)
            shutil.copy2(dst, bk)
            if sha(bk) != act["orig_sha"]:
                raise G1Exit(3, "backup of %s failed verification" % name)
        shutil.copy2(src, dst)
        if sha(dst) != new_sha:
            raise G1Exit(3, "placing %s failed verification" % name)
        act["state"] = "done"
        self.save_manifest(man)

    def do_move_aside(self, man, name, why):
        dst = os.path.join(man["corruption"], name)
        if not os.path.isfile(dst):
            return
        act = {"op": "remove", "file": name, "orig_sha": sha(dst), "new_sha": None, "why": why, "state": "pending"}
        self.step("move aside %s (%s)" % (name, why))
        self.status["actions"].append(act)
        if self.a.dry_run:
            return
        man["actions"].append(act)
        self.save_manifest(man)
        os.makedirs(self.backup_dir, exist_ok=True)
        bk = os.path.join(self.backup_dir, name)
        shutil.copy2(dst, bk)
        if sha(bk) != act["orig_sha"]:
            raise G1Exit(3, "backup of %s failed verification" % name)
        os.remove(dst)
        act["state"] = "done"
        self.save_manifest(man)

    def restore(self, collect_dir=None):
        """Restore from the manifest; verify against its before-snapshot. Safe when nothing is active."""
        rep = {"had_manifest": False, "restored": [], "verified": False, "mismatches": [], "game_written": [],
               "moved_new_files": [], "unexpected_new_files": []}
        if not os.path.isfile(self.manifest_path):
            rep["verified"] = True
            self.step("no active manifest: nothing to restore")
            return rep
        rep["had_manifest"] = True
        man = load_json(self.manifest_path)
        corr = man["corruption"]
        self.step("restoring %s from %s" % (corr, self.manifest_path))
        for a in reversed(man["actions"]):
            p = os.path.join(corr, a["file"])
            bk = os.path.join(self.backup_dir, a["file"])
            if a["op"] == "add":
                if os.path.isfile(p):
                    if self.a.dry_run:
                        self.step("would delete added " + a["file"])
                    else:
                        os.remove(p)
                rep["restored"].append("deleted added " + a["file"])
                continue
            if not os.path.isfile(bk):
                if os.path.isfile(p) and sha(p) == a["orig_sha"]:
                    rep["restored"].append(a["file"] + " already original")
                else:
                    rep["mismatches"].append(a["file"] + ": backup missing")
                continue
            if sha(bk) != a["orig_sha"]:
                rep["mismatches"].append(a["file"] + ": backup hash differs from manifest")
                continue
            if self.a.dry_run:
                self.step("would restore %s from backup" % a["file"])
            else:
                shutil.copy2(bk, p)
            rep["restored"].append("restored " + a["file"])
        if self.a.dry_run:
            rep["verified"] = None
            return rep
        after = self.snapshot(corr)
        for n, v in man["before"].items():
            if n not in after:
                (rep["game_written"] if GAME_WRITTEN.match(n) else rep["mismatches"]).append(n + " missing")
            elif after[n]["sha"] != v["sha"]:
                (rep["game_written"] if GAME_WRITTEN.match(n) else rep["mismatches"]).append(n + " hash differs")
        for n in after:
            if n in man["before"]:
                continue
            if re.search(r"\.(trace|rdc)$", n, re.I) or GAME_WRITTEN.match(n):
                dest = collect_dir or os.path.join(self.out, "from_game")
                os.makedirs(dest, exist_ok=True)
                shutil.move(os.path.join(corr, n), os.path.join(dest, n))
                rep["moved_new_files"].append(n)
            else:
                rep["unexpected_new_files"].append(n)
        rep["verified"] = not rep["mismatches"]
        if rep["verified"]:
            hist = os.path.join(self.state_dir, "history", time.strftime("%Y%m%d_%H%M%S"))
            os.makedirs(hist, exist_ok=True)
            shutil.move(self.manifest_path, os.path.join(hist, "manifest.json"))
            if os.path.isdir(self.backup_dir):
                shutil.move(self.backup_dir, os.path.join(hist, "backup"))
            self.step("restore verified: every pre-run file has its pre-run SHA-256 (game-written logs excepted)")
        else:
            self.warn("restore NOT verified: %s - manifest and backups kept in %s" % ("; ".join(rep["mismatches"]),
                                                                                       self.state_dir))
        if rep["unexpected_new_files"]:
            self.warn("new files left in the game folder: " + ", ".join(rep["unexpected_new_files"]))
        return rep

    # ------------------------------------------------------------------ run
    def show_scenes(self, mode):
        sp = os.path.join(KIT, "scenes.json")
        if not os.path.isfile(sp):
            return
        print("\nWhat to do in the game (%s run):" % mode)
        for s in load_json(sp)["scenes"]:
            if mode in s["modes"]:
                print("  [%s] %s" % (s["id"], s["title"]))
                for line in s["manual"]:
                    print("      " + line)
        print("", flush=True)

    def run(self):
        a = self.a
        game = self.find_game()
        if not game:
            raise G1Exit(2, "game install not found (tried --game-dir, ALAMO_GAME_DIR, %s, Steam library folders)" % DEFAULT_GAME)
        self.status["game_dir"] = game
        corr = os.path.join(game, "corruption")
        exe = os.path.join(corr, "StarWarsG.exe")
        self.step("game: " + game)
        for k in ("dxvk_d3d9", "qrenderdoc", "apitrace_d3d9", "apitrace"):
            if not os.path.isfile(TOOLS[k]):
                raise G1Exit(4, "capture tool missing: %s - run toolchain\\6_capture_tools.cmd" % TOOLS[k])
        if os.path.isfile(self.current_run):
            cr = load_json(self.current_run)
            if pid_alive(cr.get("pid", 0)) and cr.get("pid") != os.getpid():
                raise G1Exit(9, "another capture run is active (pid %s)" % cr.get("pid"))
            if not a.dry_run:
                os.remove(self.current_run)
        if os.path.isfile(self.manifest_path):
            self.warn("a previous run was not cleaned up: restoring it first")
            pre = self.restore()
            if not a.dry_run and not pre["verified"]:
                raise G1Exit(7, "could not restore the previous run; run cleanup and check " + self.state_dir)
        run_id = "%s_%s" % (a.mode, time.strftime("%Y%m%d_%H%M%S"))
        run_dir = os.path.join(self.out, "runs", run_id)
        self.status["run_dir"] = run_dir
        if not a.dry_run:
            os.makedirs(run_dir, exist_ok=True)
            os.makedirs(self.control, exist_ok=True)
        self.step("run folder: " + run_dir)

        pi = self.patch_info(corr)
        self.status["patch_chain_before"] = pi["state"]
        if pi["engine_sha"] and sha(exe) != pi["engine_sha"]:
            self.warn("StarWarsG.exe SHA-256 differs from the patch manifest's engine_sha256 (not Steam x64 v1.121?)")
        if not a.skip_steam_check and not a.dry_run and not process_running("steam.exe"):
            if a.start_steam:
                se = steam_exe()
                if se:
                    self.step("starting Steam (-silent)")
                    subprocess.Popen([se, "-silent"])
                    time.sleep(25)
            if not process_running("steam.exe"):
                raise G1Exit(5, "Steam is not running (start it, or pass --start-steam)")
        if a.mode == "apitrace" and shutil.disk_usage(self.out if os.path.isdir(self.out) else KIT).free < 20 * (1 << 30):
            self.warn("less than 20 GB free for the trace; a session can need several GB")

        man = {"version": 1, "created": now(), "mode": a.mode, "game_dir": game, "corruption": corr, "run_dir": run_dir,
               "before": self.snapshot(corr), "actions": []}
        if not a.dry_run:
            save_json({"pid": os.getpid(), "mode": a.mode, "run_dir": run_dir, "started": now()}, self.current_run)
        rdoc = None
        game_proc = None
        try:
            # 1. prior patch chain (PRIOR_PATCH.md / RENDER.md 7.2 preconditions)
            if a.keep_patch_chain:
                self.warn("prior patch chain left as is (--keep-patch-chain): not a parity capture")
            elif pi["state"] == "enabled":
                self.do_replace(man, "d3dx9_43.dll", os.path.join(corr, "eaw_modifier_d3dx_original.dll"),
                                "disable prior patch chain: genuine d3dx9_43 (PRIOR_PATCH.md)")
            elif pi["state"] in ("disabled", "no_chain"):
                self.step("prior patch chain: %s (nothing to do)" % pi["state"])
            elif not a.allow_unknown_state:
                raise G1Exit(3, "prior patch chain state '%s': d3dx9_43.dll is neither the proxy nor the original" % pi["state"])
            else:
                self.warn("prior patch chain state '%s': continuing (--allow-unknown-state)" % pi["state"])
            # 2. capture DLLs
            if a.mode == "apitrace":
                self.do_replace(man, "d3d9.dll", TOOLS["apitrace_d3d9"], "apitrace D3D9 wrapper (forwards to the system d3d9.dll)")
                self.do_move_aside(man, "dxgi.dll", "keep DXVK's dxgi out of the native D3D9 path")
            else:
                self.do_replace(man, "d3d9.dll", TOOLS["dxvk_d3d9"], "DXVK 3.1.1 d3d9 (pinned)")
            self.save_manifest(man)

            # 3. launch
            gargs = [] if a.fullscreen else ["WINDOWED", "SCREENWIDTH=%d" % a.width, "SCREENHEIGHT=%d" % a.height]
            if a.probe_mod:
                gargs.append("MODPATH=" + self.build_probe_mod(corr, a.probe_mod))
            gargs += a.extra_args.split() if a.extra_args else []
            env_set = {}
            if a.mode == "dxvk":
                env_set = {"DXVK_HUD": DXVK_HUD, "DXVK_LOG_LEVEL": "info", "DXVK_LOG_PATH": run_dir}
            elif a.mode == "renderdoc":
                env_set = {"DXVK_LOG_LEVEL": "info", "DXVK_LOG_PATH": run_dir, "VK_ADD_LAYER_PATH": TOOLS["rdoc_dir"],
                           "VK_INSTANCE_LAYERS": "VK_LAYER_RENDERDOC_Capture", "ENABLE_VULKAN_RENDERDOC_CAPTURE": "1"}
            else:
                env_set = {"TRACE_FILE": os.path.join(run_dir, "eaw.trace")}
            env_set.update(dict(kv.split("=", 1) for kv in a.env))
            timeout = a.timeout or (900 if a.non_interactive else 2400)
            self.status["launch"] = {"exe": exe, "args": " ".join(gargs), "env": env_set, "timeout_s": timeout,
                                     "started": None, "ended": None, "timed_out": False, "forced": False, "exit_code": None}
            if not a.non_interactive:
                self.show_scenes(a.mode)
            if a.dry_run:
                self.step('would launch: "%s" %s (cwd %s)' % (exe, " ".join(gargs), corr))
                for k, v in env_set.items():
                    self.step("  env %s=%s" % (k, v))
                return
            for n in os.listdir(self.control):
                if n.endswith(".req"):
                    os.remove(os.path.join(self.control, n))
            env = dict(os.environ)
            env.update(env_set)
            if a.mode == "renderdoc":
                cfg = {"exe": exe, "workdir": corr, "args": " ".join(gargs), "out_dir": run_dir, "control_dir": self.control,
                       "auto_trigger_sec": list(a.auto_trigger_sec), "auto_labels": list(a.auto_labels),
                       "timeout_sec": timeout + 120, "env": env_set}
                cfg_path = os.path.join(run_dir, "rdoc_config.json")
                save_json(cfg, cfg_path)
                set_renderdoc_quiet(self)
                renv = dict(os.environ)
                renv["G1_RDOC_CONFIG"] = cfg_path
                rdoc = subprocess.Popen([TOOLS["qrenderdoc"], "--python", os.path.join(KIT, "rdoc_controller.py")], env=renv)
                self.step("RenderDoc controller started (pid %d); it launches the game" % rdoc.pid)
            else:
                game_proc = subprocess.Popen([exe] + gargs, cwd=corr, env=env)
                self.step("game started (pid %d)" % game_proc.pid)
            self.status["launch"]["started"] = now()
            save_json(self.status, os.path.join(run_dir, "status.json"))

            # 4. monitor (and, with --drive, the menu driver thread)
            drive_thread = None
            t0 = time.time()
            last_shot = t0
            quit_at = None
            seen = False
            shots_dir = os.path.join(run_dir, "shots")
            while True:
                time.sleep(0.5)
                el = time.time() - t0
                pids = pids_of_exe(exe)
                if pids:
                    seen = True
                rdoc_alive = rdoc is not None and rdoc.poll() is None
                if seen and not pids and not rdoc_alive:
                    break
                if a.drive and pids and drive_thread is None:
                    drive_thread = self.start_driver(exe, shots_dir)
                if not seen and el > 180:
                    raise G1Exit(6, "the game process never appeared")
                for n in sorted(os.listdir(self.control)):
                    if not n.endswith(".req"):
                        continue
                    lab = n[:-4].split("_", 1)[-1]
                    rp = os.path.join(self.control, n)
                    if lab == "quit":
                        if quit_at is None:
                            quit_at = time.time()
                            self.step("quit requested")
                        os.replace(rp, rp[:-4] + ".taken")
                        continue
                    if a.mode == "renderdoc":
                        continue  # the controller takes these
                    if a.mode == "dxvk" and pids:
                        p = screenshot_pid(pids[0], shots_dir, lab)
                        if p:
                            self.status["screenshots"].append(p)
                            self.step("screenshot " + lab)
                    if a.mode == "apitrace":
                        tf = os.path.join(run_dir, "eaw.trace")
                        sz = os.path.getsize(tf) if os.path.isfile(tf) else 0
                        self.status["markers"].append({"label": lab, "time": now(), "elapsed_s": round(el, 1), "trace_bytes": sz})
                        self.step("marker %s (trace %d bytes)" % (lab, sz))
                    os.replace(rp, rp[:-4] + ".taken")
                if a.mode == "dxvk" and pids and a.shot_interval > 0 and time.time() - last_shot >= a.shot_interval:
                    last_shot = time.time()
                    p = screenshot_pid(pids[0], shots_dir, "timer")
                    if p:
                        self.status["screenshots"].append(p)
                if quit_at is None and el > timeout:
                    quit_at = time.time()
                    self.status["launch"]["timed_out"] = True
                    self.warn("timeout after %d s: closing the game" % timeout)
                if quit_at is not None:
                    since = time.time() - quit_at
                    for pid in pids:
                        close_windows_of(pid)
                    if since > a.grace:
                        for pid in pids:
                            kill_pid(pid)
                            self.status["launch"]["forced"] = True
                        if rdoc_alive and since > a.grace + 30:
                            rdoc.kill()
            if drive_thread is not None:
                drive_thread.join(30)
            self.status["launch"]["ended"] = now()
            if game_proc is not None:
                self.status["launch"]["exit_code"] = game_proc.poll()
            self.step("game exited")
            if rdoc is not None:
                try:
                    rdoc.wait(30)
                except subprocess.TimeoutExpired:
                    rdoc.kill()
                rs = os.path.join(run_dir, "rdoc_status.json")
                if os.path.isfile(rs):
                    self.status["captures"] = load_json(rs).get("captures", [])
                err = os.path.join(run_dir, "rdoc_controller_error.txt")
                if os.path.isfile(err):
                    self.warn("RenderDoc controller error: " + open(err).read())
            if a.mode == "apitrace":
                tf = os.path.join(run_dir, "eaw.trace")
                if os.path.isfile(tf):
                    self.status["captures"] = [{"label": "session", "path": tf, "bytes": os.path.getsize(tf)}]
            logs = os.path.join(run_dir, "game_logs")
            os.makedirs(logs, exist_ok=True)
            for n in os.listdir(corr):
                p = os.path.join(corr, n)
                if os.path.isfile(p) and GAME_WRITTEN.match(n) and os.path.getmtime(p) >= t0 - 1:
                    shutil.copy2(p, logs)
        finally:
            if rdoc is not None and rdoc.poll() is None:
                rdoc.kill()
            self.status["cleanup"] = self.restore(run_dir if not a.dry_run else None)
            if not a.dry_run and os.path.isfile(self.current_run):
                os.remove(self.current_run)
        if self.status["cleanup"]["verified"] is False:
            raise G1Exit(7, "restore verification failed")
        if a.mode != "dxvk" and not self.status["captures"]:
            raise G1Exit(8, "no capture was produced")
        if not a.no_analyze:
            self.analyze(run_dir)

    # ------------------------------------------------------------------ probe mod (outside the game folder)
    def build_probe_mod(self, corr, name):
        """Builds a MODPATH mod under out\\mods\\<name> (never inside the game folder) and returns its 8.3 path
        (the game's command-line parser splits at spaces). The XML is taken from the game's own Data\\config.meg
        (read only) and changed minimally:
          admonitor: Skirmish_Empire_Star_Base_1 also offers Admonitor_Star_Destroyer (Thrawn's Star Destroyer;
                     normally it needs space station level 4), so a short Kuat skirmish can build it at once.
                     (g1f: the item is paid and finishes, but no ship ever appears on a level-1 station.)
          admonitor4: Skirmish_Empire_Star_Base_1 is replaced by a copy of Skirmish_Empire_Star_Base_4 (renamed), i.e.
                     the start station is a level-4 station that offers Admonitor_Star_Destroyer as the game ships it.
          admonitor_spawn: Skirmish_Empire_Star_Base_1 launches one Admonitor_Star_Destroyer from its garrison
                     (SPAWN_SQUADRON starting list), so the ship appears without the tactical build path, which
                     never delivered a human player's unit in the 2026-10-30/31 runs."""
        if name not in ("admonitor", "admonitor4", "admonitor_spawn"):
            raise G1Exit(1, "unknown probe mod " + name)
        sys.path.insert(0, os.path.join(ROOT, "tools", "meg"))
        import megtool
        mod = os.path.join(self.out, "mods", "g1_" + name)
        if os.path.isdir(mod):
            shutil.rmtree(mod)
        xml_dir = os.path.join(mod, "Data", "XML")
        os.makedirs(xml_dir)
        meg = megtool.MegArchive(os.path.join(corr, "Data", "config.meg"))
        ent = [e for e in meg.entries if e.name.upper() == "DATA\\XML\\STARBASES.XML"]
        if len(ent) != 1:
            raise G1Exit(3, "STARBASES.XML not found in config.meg")
        txt = meg.read(ent[0]).decode("latin-1")
        if name == "admonitor4":
            b1 = re.search(r'<StarBase Name="Skirmish_Empire_Star_Base_1">.*?</StarBase>', txt, re.S)
            b4 = re.search(r'<StarBase Name="Skirmish_Empire_Star_Base_4">.*?</StarBase>', txt, re.S)
            if not b1 or not b4 or "Admonitor_Star_Destroyer" not in b4.group(0):
                raise G1Exit(3, "Skirmish_Empire_Star_Base_1/4 blocks not in the expected form")
            blk = b4.group(0).replace('Name="Skirmish_Empire_Star_Base_4"', 'Name="Skirmish_Empire_Star_Base_1"', 1)
            new = txt[:b1.start()] + blk + txt[b1.end():]
        elif name == "admonitor_spawn":
            m = re.search(r'<StarBase Name="Skirmish_Empire_Star_Base_1">.*?(</Tactical_Buildable_Objects_Multiplayer>)', txt, re.S)
            if not m or "Spawned_Units" in m.group(0):
                raise G1Exit(3, "Skirmish_Empire_Star_Base_1 not in the expected form")
            nl = "\r\n" if "\r\n" in m.group(0) else "\n"
            new = (txt[:m.end()] + nl + "\t\t<Starting_Spawned_Units_Tech_0>Admonitor_Star_Destroyer, 1</Starting_Spawned_Units_Tech_0>"
                   + nl + "\t\t<Reserve_Spawned_Units_Tech_0>Admonitor_Star_Destroyer, 0</Reserve_Spawned_Units_Tech_0>" + txt[m.end():])
        else:
            m = re.search(r'(<StarBase Name="Skirmish_Empire_Star_Base_1">.*?ES_Level_Two_Starbase_Upgrade,)', txt, re.S)
            if not m or "Admonitor" in m.group(1):
                raise G1Exit(3, "Skirmish_Empire_Star_Base_1 build list not in the expected form")
            nl = "\r\n" if "\r\n" in m.group(1) else "\n"
            new = txt[:m.end()] + nl + "\t\t\t\tAdmonitor_Star_Destroyer," + txt[m.end():]
        with open(os.path.join(xml_dir, "STARBASES.XML"), "wb") as f:
            f.write(new.encode("latin-1"))
        short = ctypes.create_unicode_buffer(1024)
        n = ctypes.windll.kernel32.GetShortPathNameW(mod, short, 1024)
        path = short.value if n and " " not in short.value else None
        if not path:
            raise G1Exit(1, "no space-free 8.3 path for " + mod)
        self.status["probe_mod"] = {"name": name, "dir": mod, "modpath": path,
                                    "change": {"admonitor4": "Skirmish_Empire_Star_Base_1 := copy of Skirmish_Empire_Star_Base_4",
                                               "admonitor_spawn": "Skirmish_Empire_Star_Base_1 garrison += Admonitor_Star_Destroyer x1"}.get(name, "Skirmish_Empire_Star_Base_1 += Admonitor_Star_Destroyer"),
                                    "starbases_sha": sha(os.path.join(xml_dir, "STARBASES.XML"))}
        self.step("probe mod %s at %s (MODPATH=%s)" % (name, mod, path))
        return path

    # ------------------------------------------------------------------ menu driving
    def start_driver(self, exe, shots_dir):
        """Runs scenes.json "auto" steps of the --drive scenes in a thread; quits the game when done or on failure."""
        import threading
        if KIT not in sys.path:
            sys.path.insert(0, KIT)
        import g1_ui
        sc = load_json(os.path.join(KIT, "scenes.json"))
        by_id = {s["id"]: s for s in sc["scenes"]}
        wanted = [x.strip() for x in self.a.drive.split(",") if x.strip()]
        kit = self
        ui = {"scenes": wanted, "method": self.a.ui_method, "ok": None, "error": None, "done": [], "log": []}
        self.status["ui"] = ui
        os.makedirs(shots_dir, exist_ok=True)

        class Ctx:
            def hwnd(self):
                return g1_ui.main_window(pids_of_exe(exe))

            def alive(self):
                return bool(pids_of_exe(exe))

            def request(self, label):
                kit.request(label, 1)

            def log(self, m):
                kit.step(m)

        Ctx.shots_dir = shots_dir
        drv = g1_ui.Driver(Ctx(), sc.get("refs", {}), self.a.ui_method)

        def body():
            try:
                for idx, sid in enumerate(wanted):
                    steps = by_id[sid].get("auto")
                    if not steps:
                        raise g1_ui.DriveError("scene %s has no 'auto' steps" % sid)
                    if idx:   # the previous scene ended on the skirmish setup screen: skip the main-menu steps
                        steps = [s for s in steps if not s.get("menu")]
                    drv.run(steps, sid)
                    ui["done"].append(sid)
                ui["ok"] = True
            except Exception as e:  # any failure: record it, close the game, the run restores as usual
                ui["ok"] = False
                ui["error"] = str(e)
                kit.warn("menu driver stopped: %s" % e)
            finally:
                ui["log"] = drv.log
                if not self.a.drive_keep_open and drv.ctx.alive():
                    kit.request("quit", 1)

        t = threading.Thread(target=body, name="g1-drive", daemon=True)
        t.start()
        self.step("menu driver started for scenes " + ",".join(wanted))
        return t

    # ------------------------------------------------------------------ analysis
    def analyze(self, run_dir):
        if not run_dir or not os.path.isdir(run_dir):
            raise G1Exit(1, "run folder not found: %s" % run_dir)
        an = {"rdoc_extract": None, "analyze_exit": None, "report": None}
        rdcs = sorted(os.path.join(run_dir, n) for n in os.listdir(run_dir) if n.lower().endswith(".rdc"))
        if rdcs:
            self.step("extracting %d RenderDoc capture(s)" % len(rdcs))
            cfg = os.path.join(run_dir, "rdoc_extract_config.json")
            save_json({"captures": rdcs, "out_dir": run_dir}, cfg)
            set_renderdoc_quiet(self)
            renv = dict(os.environ)
            renv["G1_RDOC_EXTRACT"] = cfg
            p = subprocess.Popen([TOOLS["qrenderdoc"], "--python", os.path.join(KIT, "rdoc_extract.py")], env=renv)
            try:
                p.wait(3600)
            except subprocess.TimeoutExpired:
                p.kill()
                self.warn("RenderDoc extraction timed out")
            an["rdoc_extract"] = os.path.join(run_dir, "rdoc_extract_status.json")
        self.step("running g1_analyze.py")
        r = subprocess.run([sys.executable, "-I", os.path.join(KIT, "g1_analyze.py"), "--run-dir", run_dir,
                            "--apitrace", TOOLS["apitrace"], "--d3dretrace", TOOLS["d3dretrace"]])
        an["analyze_exit"] = r.returncode
        an["report"] = os.path.join(run_dir, "analysis", "report.md")
        self.status["analysis"] = an

    def request(self, label, frames):
        if not os.path.isfile(self.current_run):
            raise G1Exit(1, "no capture run is active")
        clean = re.sub(r"[^A-Za-z0-9\-]", "-", label)
        p = os.path.join(self.control, "%s_%s.req" % (time.strftime("%H%M%S") + "%03d" % (int(time.time() * 1000) % 1000), clean))
        with open(p, "w") as f:
            f.write(str(frames))
        self.status["request"] = p
        self.step("request written: " + p)

    def do_status(self):
        g = self.find_game()
        self.status["game_dir"] = g
        if g:
            c = os.path.join(g, "corruption")
            self.status["patch_chain"] = self.patch_info(c)
            d = os.path.join(c, "d3d9.dll")
            s = sha(d) if os.path.isfile(d) else None
            self.status["d3d9"] = ("missing" if s is None else "dxvk-3.1.1" if s == sha(TOOLS["dxvk_d3d9"])
                                   else "apitrace-wrapper" if s == sha(TOOLS["apitrace_d3d9"]) else "other " + s)
            self.status["dxgi_present"] = os.path.isfile(os.path.join(c, "dxgi.dll"))
        self.status["tools_present"] = {k: os.path.exists(v) for k, v in TOOLS.items()}
        self.status["active_manifest"] = os.path.isfile(self.manifest_path)
        self.status["active_run"] = load_json(self.current_run) if os.path.isfile(self.current_run) else None
        self.status["steam_running"] = process_running("steam.exe")


# ---------------------------------------------------------------------- Win32 helpers (ctypes)
user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
EnumWindowsProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
user32.EnumWindows.argtypes = [EnumWindowsProc, wt.LPARAM]
user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
user32.IsWindowVisible.argtypes = [wt.HWND]
user32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.GetDC.argtypes = [wt.HWND]
user32.GetDC.restype = wt.HDC
user32.ReleaseDC.argtypes = [wt.HWND, wt.HDC]
gdi32.CreateCompatibleDC.argtypes = [wt.HDC]
gdi32.CreateCompatibleDC.restype = wt.HDC
gdi32.CreateCompatibleBitmap.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int]
gdi32.CreateCompatibleBitmap.restype = wt.HBITMAP
gdi32.SelectObject.argtypes = [wt.HDC, wt.HGDIOBJ]
gdi32.SelectObject.restype = wt.HGDIOBJ
gdi32.BitBlt.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wt.HDC, ctypes.c_int, ctypes.c_int, wt.DWORD]
gdi32.GetDIBits.argtypes = [wt.HDC, wt.HBITMAP, wt.UINT, wt.UINT, ctypes.c_void_p, ctypes.c_void_p, wt.UINT]
gdi32.DeleteObject.argtypes = [wt.HGDIOBJ]
gdi32.DeleteDC.argtypes = [wt.HDC]
kernel32.OpenProcess.restype = wt.HANDLE
kernel32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
kernel32.QueryFullProcessImageNameW.argtypes = [wt.HANDLE, wt.DWORD, wt.LPWSTR, ctypes.POINTER(wt.DWORD)]
kernel32.TerminateProcess.argtypes = [wt.HANDLE, wt.UINT]
kernel32.CloseHandle.argtypes = [wt.HANDLE]
kernel32.GetExitCodeProcess.argtypes = [wt.HANDLE, ctypes.POINTER(wt.DWORD)]
psapi.EnumProcesses.argtypes = [ctypes.POINTER(wt.DWORD), wt.DWORD, ctypes.POINTER(wt.DWORD)]
try:
    user32.SetProcessDPIAware()
except Exception:
    pass
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
PROCESS_TERMINATE = 0x0001
WM_CLOSE = 0x0010


def all_pids():
    arr = (wt.DWORD * 8192)()
    got = wt.DWORD()
    psapi.EnumProcesses(arr, ctypes.sizeof(arr), ctypes.byref(got))
    return [arr[i] for i in range(got.value // 4) if arr[i]]


def image_of(pid):
    h = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not h:
        return None
    try:
        buf = ctypes.create_unicode_buffer(1024)
        n = wt.DWORD(1024)
        if kernel32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n)):
            return buf.value
        return None
    finally:
        kernel32.CloseHandle(h)


def pids_of_exe(exe):
    exe = os.path.normcase(os.path.abspath(exe))
    return [p for p in all_pids() if (image_of(p) or "") and os.path.normcase(image_of(p) or "") == exe]


def process_running(name):
    name = name.lower()
    return any(os.path.basename(image_of(p) or "").lower() == name for p in all_pids())


def pid_alive(pid):
    if not pid:
        return False
    h = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, int(pid))
    if not h:
        return False
    try:
        code = wt.DWORD()
        kernel32.GetExitCodeProcess(h, ctypes.byref(code))
        return code.value == 259  # STILL_ACTIVE
    finally:
        kernel32.CloseHandle(h)


def windows_of(pid, visible_only=True):
    res = []

    def cb(hwnd, _):
        p = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and (not visible_only or user32.IsWindowVisible(hwnd)):
            res.append(hwnd)
        return True

    user32.EnumWindows(EnumWindowsProc(cb), 0)
    return res


def close_windows_of(pid):
    for h in windows_of(pid):
        user32.PostMessageW(h, WM_CLOSE, 0, 0)


def kill_pid(pid):
    h = kernel32.OpenProcess(PROCESS_TERMINATE, False, pid)
    if h:
        kernel32.TerminateProcess(h, 1)
        kernel32.CloseHandle(h)


def steam_exe():
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as k:
            return winreg.QueryValueEx(k, "SteamExe")[0]
    except OSError:
        return None


def write_png(path, w, h, rgb_rows):
    raw = b"".join(b"\x00" + r for r in rgb_rows)

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def screenshot_pid(pid, out_dir, label):
    """Client area of the game's main window via PrintWindow (g1_ui.grab: not covered by other windows; includes the
    DXVK HUD); falls back to a screen copy of the largest visible window of pid. Returns the PNG path or None."""
    try:
        if KIT not in sys.path:
            sys.path.insert(0, KIT)
        import g1_ui
        h = g1_ui.main_window([pid])
        img = g1_ui.grab(h) if h else None
        if img is not None:
            return g1_ui.write_png(os.path.join(out_dir, "%s_%s.png" % (re.sub(r"[^A-Za-z0-9_-]", "_", label), time.strftime("%H%M%S"))), img)
    except Exception:
        pass
    best, area = None, 0
    for hwnd in windows_of(pid):
        r = wt.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(r))
        a = (r.right - r.left) * (r.bottom - r.top)
        if a > area:
            best, area = r, a
    if best is None or area <= 0:
        return None
    w, h = best.right - best.left, best.bottom - best.top
    sdc = user32.GetDC(None)
    mdc = gdi32.CreateCompatibleDC(sdc)
    bmp = gdi32.CreateCompatibleBitmap(sdc, w, h)
    old = gdi32.SelectObject(mdc, bmp)
    gdi32.BitBlt(mdc, 0, 0, w, h, sdc, best.left, best.top, 0x00CC0020 | 0x40000000)  # SRCCOPY | CAPTUREBLT
    bi = struct.pack("<IiiHHIIiiII", 40, w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    bib = ctypes.create_string_buffer(bi, 44)
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mdc, bmp, 0, h, buf, bib, 0)
    gdi32.SelectObject(mdc, old)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mdc)
    user32.ReleaseDC(None, sdc)
    data = buf.raw
    rows = []
    for y in range(h):
        row = data[y * w * 4:(y + 1) * w * 4]
        rgb = bytearray(w * 3)
        rgb[0::3] = row[2::4]
        rgb[1::3] = row[1::4]
        rgb[2::3] = row[0::4]
        rows.append(bytes(rgb))
    os.makedirs(out_dir, exist_ok=True)
    p = os.path.join(out_dir, "%s_%s.png" % (label, time.strftime("%H%M%S")))
    write_png(p, w, h, rows)
    return p


def set_renderdoc_quiet(kit):
    """qrenderdoc asks a modal analytics question on its first start, which would block a scripted run.
    Give the privacy-preserving answer (opt out) and turn off update checks in RenderDoc's per-user UI.config."""
    f = os.path.join(os.environ.get("APPDATA", ""), "qrenderdoc", "UI.config")
    if os.path.isfile(f):
        t = open(f, encoding="utf-8").read()
        n = re.sub(r'"Analytics_TotalOptOut":\s*false', '"Analytics_TotalOptOut": true', t)
        n = re.sub(r'"CheckUpdate_AllowChecks":\s*true', '"CheckUpdate_AllowChecks": false', n)
        n = re.sub(r'"Tips_HasSeenFirst":\s*false', '"Tips_HasSeenFirst": true', n)
        if n != t:
            open(f, "w", encoding="utf-8").write(n)
            kit.step("RenderDoc UI.config: analytics opt-out, no update checks")
    else:
        os.makedirs(os.path.dirname(f), exist_ok=True)
        open(f, "w", encoding="utf-8").write('{\n    "Analytics_TotalOptOut": true,\n    "CheckUpdate_AllowChecks": false,\n'
                                             '    "Tips_HasSeenFirst": true\n}\n')
        kit.step("RenderDoc UI.config created: analytics opt-out, no update checks")


# ---------------------------------------------------------------------- main
def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["dxvk", "renderdoc", "apitrace", "cleanup", "status", "trigger", "quit", "analyze"])
    ap.add_argument("--game-dir", default="")
    ap.add_argument("--non-interactive", action="store_true", help="no prompts; for unattended runs")
    ap.add_argument("--dry-run", action="store_true", help="print what would change; change nothing, launch nothing")
    ap.add_argument("--timeout", type=int, default=0, help="seconds before the game is closed (default 900 / 2400)")
    ap.add_argument("--grace", type=int, default=20, help="seconds between WM_CLOSE and a forced kill")
    ap.add_argument("--label", default="capture")
    ap.add_argument("--frames", type=int, default=1)
    ap.add_argument("--auto-trigger-sec", type=float, nargs="*", default=[], help="renderdoc: capture at these seconds")
    ap.add_argument("--auto-labels", nargs="*", default=[])
    ap.add_argument("--shot-interval", type=int, default=15, help="dxvk: seconds between timer screenshots (0 = off)")
    ap.add_argument("--fullscreen", action="store_true", help="do not pass WINDOWED SCREENWIDTH= SCREENHEIGHT=")
    ap.add_argument("--width", type=int, default=1920)
    ap.add_argument("--height", type=int, default=1080)
    ap.add_argument("--extra-args", default="", help="more game command-line arguments")
    ap.add_argument("--env", nargs="*", default=[], help="extra NAME=VALUE for the game process")
    ap.add_argument("--out-dir", default="")
    ap.add_argument("--run-dir", default="")
    ap.add_argument("--keep-patch-chain", action="store_true")
    ap.add_argument("--allow-unknown-state", action="store_true")
    ap.add_argument("--skip-steam-check", action="store_true")
    ap.add_argument("--start-steam", action="store_true")
    ap.add_argument("--no-analyze", action="store_true")
    ap.add_argument("--probe-mod", default="", choices=["", "admonitor", "admonitor4", "admonitor_spawn"],
                    help="start the game with a probe mod built under out\\mods (admonitor: Thrawn's SD buildable at station level 1; admonitor_spawn: the level-1 station launches it from its garrison)")
    ap.add_argument("--drive", default="", help="drive the menus through these scenes.json scenes, e.g. K or K,E")
    ap.add_argument("--ui-method", default="post", choices=["input", "post"], help="input injection method (post: PostMessage to the game window, no focus needed; input: SendInput)")
    ap.add_argument("--drive-keep-open", action="store_true", help="do not quit the game after the driven scenes")
    a = ap.parse_args(argv)
    kit = Kit(a)
    code = 0
    try:
        if a.mode == "status":
            kit.do_status()
        elif a.mode == "cleanup":
            kit.status["cleanup"] = kit.restore()
            if kit.status["cleanup"]["verified"] is False:
                raise G1Exit(7, "restore verification failed")
            g = kit.find_game()
            if g:
                kit.status["patch_chain_after"] = kit.patch_info(os.path.join(g, "corruption"))["state"]
                kit.step("prior patch chain now: " + kit.status["patch_chain_after"])
        elif a.mode == "trigger":
            kit.request(a.label, a.frames)
        elif a.mode == "quit":
            kit.request("quit", 1)
        elif a.mode == "analyze":
            kit.analyze(a.run_dir)
        else:
            kit.run()
        kit.status["result"] = "ok"
    except G1Exit as e:
        code = e.code
        kit.status["result"] = "error"
        kit.status["error"] = str(e)
        print("ERROR (%d): %s" % (code, e), flush=True)
    except Exception:
        code = 1
        kit.status["result"] = "error"
        kit.status["error"] = traceback.format_exc()
        print("ERROR: " + kit.status["error"], flush=True)
    kit.status["exit_code"] = code
    kit.status["finished"] = now()
    if not a.dry_run or a.mode == "status":
        rd = kit.status.get("run_dir")
        if rd and os.path.isdir(rd):
            save_json(kit.status, os.path.join(rd, "status.json"))
        save_json(kit.status, os.path.join(kit.out, "last_status.json"))
    if a.mode == "status" or a.dry_run:
        print(json.dumps(kit.status, indent=2))
    print("RESULT %s exit %d" % (kit.status["result"], code), flush=True)
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
