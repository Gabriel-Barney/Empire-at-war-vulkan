"""q2_ui.py - drives the FoC menus up to the start of a Q2 run, then lets go of the window (ctypes only).

Reuses the G1 menu driver (tools/probe_mod/g1_capture/g1_ui.py: PostMessage input, PrintWindow screenshots,
reference-crop checks) by importing it read-only; the step lists and extra reference crops are this kit's own
(q2_scenes.json, refs/).

  python -I q2_ui.py drive galactic|skirmish --shots <dir> [--status <json>] [--pid N] [--method post|input]
        waits for the game window, runs the scenario's steps from the main menu to the moment the galaxy /
        battle is running, writes ui_status.json, and exits. After the last step it never touches the window
        again, so the command stream of the traced session stays empty.
  python -I q2_ui.py shot <out.png>        screenshot of the game's client area (calibration)
  python -I q2_ui.py click X Y [--method]  one click in 1920x1080 client space (calibration only)
  python -I q2_ui.py key NAME              one key press (calibration only)
  python -I q2_ui.py like <png> [ref...]   distance of a screenshot to the reference crops
  python -I q2_ui.py crop <png> x0 y0 x1 y1 <out.png>   cut a reference crop from a 1920x1080 screenshot

Exit codes: 0 reached the start, 3 a step failed (FAIL_*.png in --shots), 4 game window never appeared / game
exited.
"""
import json
import os
import sys
import time

KIT = os.path.dirname(os.path.abspath(__file__))
G1 = os.path.normpath(os.path.join(KIT, "..", "g1_capture"))
sys.path.insert(0, G1)
import g1_ui  # noqa: E402  (read-only reuse of the finished G1 kit)

GAME_EXE = "starwarsg.exe"


def load_scenes():
    with open(os.path.join(KIT, "q2_scenes.json"), encoding="utf-8") as f:
        sc = json.load(f)
    refs = {}
    with open(os.path.join(G1, "scenes.json"), encoding="utf-8") as f:
        for k, v in json.load(f).get("refs", {}).items():
            if isinstance(v, dict):
                refs[k] = dict(v, png=os.path.join(G1, v["png"]))
    for k, v in sc.get("refs", {}).items():
        if isinstance(v, dict):
            refs[k] = dict(v, png=os.path.join(KIT, v["png"]))
    return sc, refs


def game_pids(pid=0):
    from g1 import all_pids, image_of  # read-only helpers of the G1 kit
    if pid:
        return [pid] if os.path.basename(image_of(pid) or "").lower() == GAME_EXE else []
    return [p for p in all_pids() if os.path.basename(image_of(p) or "").lower() == GAME_EXE]


class Driver(g1_ui.Driver):
    """g1_ui.Driver whose reference crops may live in either kit (absolute png paths)."""

    def like_dist(self, img, name):
        r = self.refs.get(name)
        if not isinstance(r, dict) or "png" not in r:
            raise g1_ui.DriveError("no reference crop '%s'" % name)
        if name not in self._like:
            self._like[name] = g1_ui.box_signature(g1_ui.read_png(r["png"]))
        return g1_ui.sig_distance(g1_ui.box_signature(img, r["box"]), self._like[name])

    def one(self, s):
        d = s["do"]
        if d == "expect_any":   # {"do": "expect_any", "refs": {"name": "label"...}, "timeout": 60} -> self.matched
            end = time.time() + s.get("timeout", 60)
            while True:
                img = g1_ui.grab(self.h())
                if img is not None:
                    for name in s["refs"]:
                        dist = self.like_dist(img, name)
                        if dist <= s.get("max_dist", 10):
                            self.matched = name
                            self.note("expect_any %s ok (dist %.1f)" % (name, dist))
                            return
                if time.time() > end:
                    raise g1_ui.DriveError("none of %s appeared" % list(s["refs"]))
                time.sleep(s.get("poll", 1))
        elif d == "expect_gone":  # {"do": "expect_gone", "ref": name, "timeout": 10}: the crop must disappear
            end = time.time() + s.get("timeout", 10)
            while True:
                img = g1_ui.grab(self.h())
                if img is not None and self.like_dist(img, s["ref"]) > s.get("min_dist", 10):
                    self.note("expect_gone %s ok" % s["ref"])
                    return
                if time.time() > end:
                    raise g1_ui.DriveError("'%s' is still on screen" % s["ref"])
                time.sleep(s.get("poll", 1))
        elif d == "if_matched":  # {"do": "if_matched", "ref": name, "steps": [...]} runs steps only after that match
            if getattr(self, "matched", None) == s["ref"]:
                for sub in s["steps"]:
                    self.one(sub)
        else:
            super().one(s)


def drive(scenario, shots, status_path, pid, method):
    sc, refs = load_scenes()
    steps = sc["scenarios"][scenario]["steps"]
    os.makedirs(shots, exist_ok=True)
    st = {"scenario": scenario, "method": method, "state": "waiting_window", "ok": None, "error": None,
          "started": time.strftime("%Y-%m-%dT%H:%M:%S"), "log": []}

    def save():
        if status_path:
            tmp = status_path + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(st, f, indent=1)
            os.replace(tmp, status_path)

    class Ctx:
        shots_dir = shots

        def hwnd(self):
            return g1_ui.main_window(game_pids(pid))

        def alive(self):
            return bool(game_pids(pid))

        def request(self, label):
            raise g1_ui.DriveError("q2 steps never request '%s'" % label)

        def log(self, m):
            print("[q2ui] " + m, flush=True)
            st["log"].append(time.strftime("%H:%M:%S ") + m)
            save()

    save()
    end = time.time() + 240
    while not game_pids(pid):
        if time.time() > end:
            st.update(state="failed", ok=False, error="no game process")
            save()
            return 4
        time.sleep(1)
    drv = Driver(Ctx(), refs, method)
    st["state"] = "driving"
    save()
    code = 0
    try:
        drv.run(steps, scenario)
        st.update(state="released", ok=True)
    except g1_ui.DriveError as e:
        st.update(state="failed", ok=False, error=str(e))
        code = 4 if not Ctx().alive() else 3
    finally:
        drv.park_at = None
        st["finished"] = time.strftime("%Y-%m-%dT%H:%M:%S")
        save()
    print("[q2ui] %s" % ("start reached; window released" if code == 0 else "FAILED: %s" % st["error"]), flush=True)
    return code


def main(argv):
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["drive", "shot", "click", "key", "like", "info", "crop"])
    ap.add_argument("args", nargs="*")
    ap.add_argument("--shots", default=os.path.join(KIT, "out", "ui_shots"))
    ap.add_argument("--status", default="")
    ap.add_argument("--pid", type=int, default=0)
    ap.add_argument("--method", default="post", choices=["post", "input"])
    a = ap.parse_args(argv)
    if a.cmd == "drive":
        return drive(a.args[0], a.shots, a.status, a.pid, a.method)
    if a.cmd == "crop":     # crop <shot.png> x0 y0 x1 y1 <out.png>: cut a reference crop (1920x1080 shots)
        w, h, data = g1_ui.read_png(a.args[0])
        x0, y0, x1, y1 = [int(v) for v in a.args[1:5]]
        rows = b"".join(data[(y * w + x0) * 4:(y * w + x1) * 4] for y in range(y0, y1))
        g1_ui.write_png(a.args[5], (x1 - x0, y1 - y0, rows))
        print(a.args[5], x1 - x0, y1 - y0)
        return 0
    if a.cmd == "like":
        _, refs = load_scenes()
        d = Driver(None, refs)
        img = g1_ui.read_png(a.args[0])
        for n in a.args[1:] or sorted(refs):
            print("%-18s %.1f" % (n, d.like_dist(img, n)))
        return 0
    h = g1_ui.main_window(game_pids(a.pid))
    if not h:
        print("game window not found")
        return 4
    if a.cmd == "info":
        print(json.dumps({"hwnd": h, "client": g1_ui.client_rect(h)}))
    elif a.cmd == "shot":
        img = g1_ui.grab(h)
        g1_ui.write_png(a.args[0], img)
        print(a.args[0], img[0], img[1])
    elif a.cmd == "click":
        g1_ui.click(h, float(a.args[0]), float(a.args[1]), a.method)
    elif a.cmd == "key":
        g1_ui.key(h, a.args[0], 1, a.method)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
