# rdoc_controller.py - runs INSIDE RenderDoc's own Python (qrenderdoc.exe --python rdoc_controller.py).
# Launches the target with RenderDoc injected, then triggers frame captures:
#   * on request files written by `g1.ps1 -Mode trigger -Label NAME` (control\*.req), or
#   * at fixed delays (config "auto_trigger_sec"), or
#   * by the user pressing F12 / PrintScreen in the game (RenderDoc's default capture keys).
# Every new capture is copied to <out>\<label>_f<frame>.rdc and listed in rdoc_status.json.
# Config: JSON file named by the environment variable G1_RDOC_CONFIG (written by g1.ps1).
import json
import os
import shutil
import sys
import time
import traceback

import renderdoc as rd

CFG_PATH = os.environ.get("G1_RDOC_CONFIG", "")


def now():
    return time.strftime("%Y-%m-%dT%H:%M:%S")


def main():
    cfg = json.load(open(CFG_PATH, "r", encoding="utf-8-sig"))
    out_dir = cfg["out_dir"]
    control_dir = cfg["control_dir"]
    status_path = os.path.join(out_dir, "rdoc_status.json")
    status = {"started": now(), "result": "running", "captures": [], "events": [], "ident": 0}

    def save():
        with open(status_path, "w", encoding="utf-8") as f:
            json.dump(status, f, indent=2)

    def event(msg):
        status["events"].append(now() + " " + msg)
        save()

    env = []
    for k, v in cfg.get("env", {}).items():
        env.append(rd.EnvironmentModification(rd.EnvMod.Set, rd.EnvSep.NoSep, k, v))
    opts = rd.CaptureOptions()
    opts.allowFullscreen = True
    opts.allowVSync = True
    opts.refAllResources = bool(cfg.get("ref_all_resources", False))
    template = os.path.join(out_dir, "raw", "g1")
    os.makedirs(os.path.join(out_dir, "raw"), exist_ok=True)
    res = rd.ExecuteAndInject(cfg["exe"], cfg["workdir"], cfg.get("args", ""), env, template, opts, False)
    ok = res.result.OK() if hasattr(res.result, "OK") else (res.result == rd.ResultCode.Succeeded)
    if not ok or res.ident == 0:
        status["result"] = "inject_failed"
        status["error"] = str(res.result)
        save()
        return 3
    status["ident"] = res.ident
    event("injected ident %d" % res.ident)

    tc = None
    for _ in range(100):
        tc = rd.CreateTargetControl("", res.ident, "g1_capture", True)
        if tc is not None and tc.Connected():
            break
        tc = None
        time.sleep(0.2)
    if tc is None:
        status["result"] = "connect_failed"
        save()
        return 4
    event("connected: %s pid %d api %s" % (tc.GetTarget(), tc.GetPID(), tc.GetAPI()))

    pending_labels = []  # labels of requested captures, in order
    auto = sorted(float(s) for s in cfg.get("auto_trigger_sec", []))
    auto_labels = cfg.get("auto_labels", [])
    t0 = time.time()
    timeout = float(cfg.get("timeout_sec", 0))
    quit_requested = False
    while True:
        # control requests
        try:
            for name in sorted(os.listdir(control_dir)):
                p = os.path.join(control_dir, name)
                if name.endswith(".req"):
                    label = name[:-4].split("_", 1)[-1] or "capture"
                    if label == "quit":
                        continue  # g1.ps1 closes the game; this loop ends on Disconnected
                    else:
                        frames = 1
                        try:
                            frames = max(1, int(open(p, "r").read().strip() or "1"))
                        except Exception:
                            frames = 1
                        tc.TriggerCapture(frames)
                        pending_labels.extend([label] * frames)
                        event("trigger %s (%d frame(s))" % (label, frames))
                    os.replace(p, p[:-4] + ".taken")
        except FileNotFoundError:
            pass
        # timed triggers
        el = time.time() - t0
        while auto and el >= auto[0]:
            auto.pop(0)
            label = auto_labels.pop(0) if auto_labels else "auto%d" % int(el)
            tc.TriggerCapture(1)
            pending_labels.append(label)
            event("auto trigger %s at %.1f s" % (label, el))
        msg = tc.ReceiveMessage(None)
        t = msg.type
        if t == rd.TargetControlMessageType.Disconnected:
            event("target disconnected")
            break
        if t == rd.TargetControlMessageType.NewCapture:
            nc = msg.newCapture
            label = pending_labels.pop(0) if pending_labels else "hotkey"
            dst = os.path.join(out_dir, "%s_f%d.rdc" % (label, nc.frameNumber))
            n = 1
            while os.path.exists(dst):
                n += 1
                dst = os.path.join(out_dir, "%s_f%d_%d.rdc" % (label, nc.frameNumber, n))
            copied = False
            for _ in range(50):
                try:
                    shutil.move(nc.path, dst)
                    copied = True
                    break
                except OSError:
                    time.sleep(0.2)
            status["captures"].append({"label": label, "frame": nc.frameNumber, "path": dst if copied else nc.path,
                                       "byte_size": getattr(nc, "byteSize", 0), "api": getattr(nc, "api", "")})
            event("new capture %s frame %d -> %s" % (label, nc.frameNumber, dst))
        if quit_requested and not pending_labels:
            event("quit requested by control file")
            break
        if timeout > 0 and el > timeout:
            status["timed_out"] = True
            event("controller timeout %.0f s" % timeout)
            break
        time.sleep(0.05)
    tc.Shutdown()
    status["result"] = "ok" if status["captures"] else "no_captures"
    status["finished"] = now()
    save()
    return 0


if __name__ == "__main__" or True:
    code = 1
    try:
        code = main()
    except Exception:
        try:
            with open(os.path.join(os.path.dirname(CFG_PATH) or ".", "rdoc_controller_error.txt"), "w") as f:
                f.write(traceback.format_exc())
        except Exception:
            pass
        code = 2
    sys.stdout.flush()
    os._exit(code)
