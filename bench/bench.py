#!/usr/bin/env python3
"""Boot a Canon camera in qemu-eos headless, run scripted GUI scenarios and measure them.

Runs inside the qemu-eos image (CI: ci/bench-suite.sh, ci.yml smoke test). Needs the camera ROMs.

Timeline:
  start QEMU -> poll the screen (monitor `screendump`, every --poll s) until the GUI frame is up
  (= --expect hash, else the first non-black frame that stays unchanged --stable s)
  -> for each scenario: send its keys, wait until the screen changes and settles again
  -> idle window (--idle s; MIPS / IRQ rate from the wakeups plugin) -> optional savevm -> quit.

Metrics (result.json): boot_to_gui_s (wall), boot_insns (guest instructions until the GUI, plugin
needed), per-scenario seconds + frame hash, idle MIPS, IRQ/s, WFI/s, host CPU seconds and peak RSS of
the QEMU process, TB flushes (`info jit`), UART/log markers. Frame hashes are SHA-256 of the raw RGB
pixels, so they compare across Pillow versions; screenshots are saved as PNG for humans.

Disk images: the ML base card (magiclantern/disk_images/sd.qcow2.xz) is used for both SD and CF, raw
by default (what ML-Optimized's scenarios use) or qcow2 (needed by savevm/loadvm, which store the
snapshot in the qcow2 files). Snapshots contain guest RAM, i.e. ROM-derived data: keep --disk-dir on a
tmpfs that is never uploaded.
"""
import argparse
import hashlib
import json
import lzma
import os
import re
import shutil
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))     # python3 -I drops the script dir
import card     # noqa: E402  (SD card reader, bench/card.py)

QEMU_EOS = os.environ.get("QEMU_EOS", "/opt/qemu-eos")

# Per camera: scripted scenarios (name, keys) and markers expected in the UART log once the GUI is up.
CAMS = {
    "1100D": {
        "debugmsg": "0xFF06C914",          # DryosDebugMsg, platform/1100D.105/stubs.S in ML-Optimized
        "scenarios": [("menu", ["m"]), ("menu_right", ["right"]), ("menu_close", ["m"])],
        "uart_markers": ["K288 READY", "[MC] PROP_GUI_STATE 0"],
        "log_markers": ["GUI_Initialize", "DlgShootOlc.c LOCAL_DIALOG_REFRESH"],
    },
    "4000D": {
        "debugmsg": "0xFE117178",          # DryosDebugMsg, docs/models/4000D.md (ROM pattern search)
        "scenarios": [("menu", ["m"]), ("menu_right", ["right"]), ("menu_close", ["m"])],
        "uart_markers": ["K422 READY", "[MC] PROP_GUI_STATE 0", "[STARTUP] startupInitializeComplete"],
        "log_markers": ["GUI_Initialize", "DlgShootOlc.c LOCAL_DIALOG_REFRESH"],
    },
}


class Monitor:
    """HMP over a unix socket, one persistent connection (reconnecting per command costs ~ms)."""

    def __init__(self, path, timeout=20.0):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(timeout)
        self.s.connect(path)
        self.buf = b""
        self._until_prompt()

    def _until_prompt(self):
        while b"(qemu) " not in self.buf:
            chunk = self.s.recv(65536)
            if not chunk:
                raise OSError("monitor closed")
            self.buf += chunk
        out, self.buf = self.buf.split(b"(qemu) ", 1)
        return out.decode(errors="replace")

    def cmd(self, line):
        self.s.sendall(line.encode() + b"\n")
        return self._until_prompt()

    def quit(self):
        """`quit` closes the monitor before any prompt comes back."""
        self.s.sendall(b"quit\n")

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    # "P6\n<w> <h>\n255\n" + pixels
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    if not m:
        raise ValueError("not a PPM")
    return int(m.group(1)), int(m.group(2)), data[m.end():]


class Screen:
    def __init__(self, mon, tmpdir, t0):
        self.mon, self.ppm, self.t0 = mon, os.path.join(tmpdir, "screen.ppm"), t0
        self.timeline = []      # (t, hash) at every change
        self.last = None
        self.frames = 0
        self.latency = []

    def grab(self):
        """Current frame: (time since start, sha256 of pixels, is_black, w, h, pixels)."""
        ts = time.monotonic()
        self.mon.cmd("screendump " + self.ppm)
        t = time.monotonic() - self.t0
        # A monitor command needs the big QEMU lock: its latency shows how much the vCPU hogs it.
        self.latency.append(time.monotonic() - ts)
        w, h, px = read_ppm(self.ppm)
        hsh = hashlib.sha256(px).hexdigest()[:16]
        self.frames += 1
        if hsh != self.last:
            self.timeline.append((round(t, 3), hsh))
            self.last = hsh
        return t, hsh, px.count(0) == len(px), w, h, px

    def wait_settled(self, poll, stable, timeout, want=None, changed_from=None):
        """Wait until the frame is `want` (if given), or differs from `changed_from` (if given) and
        then stays the same for `stable` s. Returns (t_first_seen, hash, w, h, pixels) or None."""
        deadline = time.monotonic() + timeout
        cand = None             # (t_first_seen, hash, w, h, px)
        while time.monotonic() < deadline:
            t, hsh, black, w, h, px = self.grab()
            if want is not None:
                if hsh == want:
                    return t, hsh, w, h, px
            elif not black and hsh != changed_from:
                if cand is None or cand[1] != hsh:
                    cand = (t, hsh, w, h, px)
                elif t - cand[0] >= stable:
                    return cand
            else:
                cand = None
            time.sleep(poll)
        return None


def save_png(path, w, h, px):
    try:
        from PIL import Image
        Image.frombytes("RGB", (w, h), px).save(path)
    except Exception as e:      # screenshots are for humans; never lose the metrics over them
        print("png save failed: %s" % e, file=sys.stderr)


def make_disks(disk_dir, fmt):
    os.makedirs(disk_dir, exist_ok=True)
    base = os.path.join(QEMU_EOS, "magiclantern", "disk_images", "sd.qcow2.xz")
    paths = {}
    for name in ("sd", "cf"):
        qcow = os.path.join(disk_dir, name + ".qcow2")
        with lzma.open(base) as src, open(qcow, "wb") as dst:
            shutil.copyfileobj(src, dst)
        if fmt == "raw":
            raw = os.path.join(disk_dir, name + ".img")
            subprocess.run([os.path.join(QEMU_EOS, "bin", "qemu-img"), "convert", "-f", "qcow2",
                            "-O", "raw", qcow, raw], check=True)
            os.remove(qcow)
            paths[name] = raw
        else:
            paths[name] = qcow
    return paths


def proc_stats(pid):
    """Host CPU seconds (user, sys) and peak RSS (MiB) of a live process, from /proc."""
    tck = os.sysconf("SC_CLK_TCK")
    with open("/proc/%d/stat" % pid) as f:
        fields = f.read().rsplit(")", 1)[1].split()
    utime, stime = int(fields[11]) / tck, int(fields[12]) / tck
    hwm = 0
    with open("/proc/%d/status" % pid) as f:
        for line in f:
            if line.startswith("VmHWM:"):
                hwm = int(line.split()[1]) / 1024
    return round(utime, 2), round(stime, 2), round(hwm, 1)


def plugin_records(path):
    recs = []
    if os.path.exists(path):
        with open(path) as f:
            for line in f:
                line = line.strip()
                if line.startswith("{"):
                    try:
                        recs.append(json.loads(line))
                    except ValueError:
                        pass
    return recs


def insns_until(intervals, t_ms):
    """Cumulative guest instructions from QEMU start to t_ms (plugin time), interpolating inside an interval."""
    total = 0.0
    for r in intervals:
        if r["t_end_ms"] <= t_ms:
            total += r["insns"]
        elif r["t_start_ms"] < t_ms:
            total += r["insns"] * (t_ms - r["t_start_ms"]) / max(1e-9, r["t_end_ms"] - r["t_start_ms"])
    return int(total)


def window_stats(intervals, t0_ms, t1_ms):
    """Rates over the intervals that lie fully inside [t0_ms, t1_ms]."""
    sel = [r for r in intervals if r["t_start_ms"] >= t0_ms and r["t_end_ms"] <= t1_ms]
    secs = sum(r["seconds"] for r in sel)
    if not sel or secs <= 0:
        return None
    s = lambda k: sum(r.get(k) or 0 for r in sel)
    return {"seconds": round(secs, 2), "mips": round(s("insns") / secs / 1e6, 2),
            "irq_per_s": round(s("irq") / secs, 1), "wfi_per_s": round(s("wfi") / secs, 1),
            "busy_ratio": round(1 - s("halted_ms") / 1000.0 / secs, 3)}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cam", default="1100D", choices=sorted(CAMS))
    ap.add_argument("--rom-dir", required=True, help="dir containing CAM/ROM1.BIN ...")
    ap.add_argument("--out", required=True)
    ap.add_argument("--label", default="")
    ap.add_argument("--qemu", default=os.path.join(QEMU_EOS, "bin", "qemu-system-arm"))
    ap.add_argument("--plugin", default="", help="wakeups plugin .so (adds interval records for MIPS)")
    ap.add_argument("--plugin-arg", action="append", default=[], help="extra KEY=VALUE for the plugin")
    ap.add_argument("--debugmsg", action="store_true", help="-d debugmsg via libmagiclantern.so (ML CI logging)")
    ap.add_argument("--tasks", action="store_true", help="-d tasks (ML CI logging)")
    ap.add_argument("--qemu-arg", action="append", default=[], help="extra QEMU argument (repeatable)")
    ap.add_argument("--env", action="append", default=[], help="extra KEY=VALUE in QEMU's environment")
    ap.add_argument("--disk-format", choices=["raw", "qcow2"], default="raw")
    ap.add_argument("--disk-dir", default="", help="reuse/keep disks here (default: fresh temp dir)")
    ap.add_argument("--savevm", default="", help="after the GUI and scenarios: savevm NAME (qcow2 only)")
    ap.add_argument("--loadvm", default="", help="start from snapshot NAME (-loadvm; reuses --disk-dir)")
    ap.add_argument("--no-scenarios", action="store_true")
    ap.add_argument("--idle", type=float, default=10.0, help="idle window after the scenarios, s")
    ap.add_argument("--expect", default="", help="JSON {frame_name: hash}: frames must match (smoke test)")
    ap.add_argument("--poll", type=float, default=0.1)
    ap.add_argument("--stable", type=float, default=3.0, help="s unchanged = settled (boot)")
    ap.add_argument("--key-stable", type=float, default=1.0, help="s unchanged = settled (scenarios)")
    ap.add_argument("--gui-timeout", type=float, default=120.0)
    ap.add_argument("--count", action="append", default=[], help="count lines of qemu.log containing this text")
    ap.add_argument("--min-count", action="append", default=[],
                    help="TEXT=N: error unless qemu.log has at least N lines containing TEXT")
    ap.add_argument("--step", action="append", default=[],
                    help="after the idle window, in order: key:K (sendkey K), wait:S, shot:NAME (frame hash + "
                         "PNG), mon:CMD (monitor command, output kept), until:TEXT[=N] (wait until qemu.log has "
                         "N lines containing TEXT, default 1, at most --until-timeout s); --count texts are "
                         "also counted per segment between steps")
    ap.add_argument("--until-timeout", type=float, default=120.0)
    ap.add_argument("--card", action="store_true",
                    help="after QEMU exits: files and pictures (with EXIF exposure) on the SD card -> card.json")
    ap.add_argument("--min-pictures", type=int, default=0, help="error unless the card has >= N DCIM pictures")
    a = ap.parse_args()

    cam = CAMS[a.cam]
    os.makedirs(a.out, exist_ok=True)
    out = os.path.abspath(a.out)
    expect = json.load(open(a.expect)).get(a.cam, {}) if a.expect else {}
    tmp = a.disk_dir or os.path.join("/tmp", "bench-%d" % os.getpid())
    if a.loadvm and not a.disk_dir:
        sys.exit("--loadvm needs --disk-dir with the disks that hold the snapshot")
    fmt = "qcow2" if (a.savevm or a.loadvm) else a.disk_format
    if a.loadvm:
        disks = {n: os.path.join(tmp, n + ".qcow2") for n in ("sd", "cf")}
    else:
        disks = make_disks(tmp, fmt)

    mon_path = os.path.join(tmp, "qemu.monitor")
    log = os.path.join(out, "qemu.log")
    wj = os.path.join(out, "wakeups.json")
    cmd = [a.qemu, "-M", "%s,firmware=boot=0" % a.cam,
           "-drive", "if=sd,format=%s,file=%s" % (fmt, disks["sd"]),
           "-drive", "if=ide,format=%s,file=%s" % (fmt, disks["cf"]),
           "-chardev", "socket,server,nowait,path=%s,id=monsock" % mon_path,
           "-mon", "chardev=monsock,mode=readline",
           "-display", "none", "-serial", "file:" + os.path.join(out, "uart.log"), "-name", a.cam]
    if a.plugin:
        cmd += ["-plugin", ",".join([a.plugin, "arg=out=" + wj, "arg=interval_ms=100"]
                                    + ["arg=" + x for x in a.plugin_arg])]
    d = []
    if a.debugmsg:
        cmd += ["-plugin", os.path.join(QEMU_EOS, "plugins", "libmagiclantern.so")
                + ",arg=debugmsg_addr=" + cam["debugmsg"]]
        d.append("debugmsg")
    if a.tasks:
        d.append("tasks")
    if d:
        cmd += ["-d", ",".join(d)]
    if a.loadvm:
        cmd += ["-loadvm", a.loadvm]
    cmd += a.qemu_arg

    env = dict(os.environ, QEMU_EOS_WORKDIR=a.rom_dir)
    for kv in a.env:
        k, v = kv.split("=", 1)
        env[k] = v
    print("[%s] %s" % (a.label, " ".join(cmd)), flush=True)

    res = {"label": a.label, "cam": a.cam, "cmd": cmd[1:], "env": a.env, "disk_format": fmt,
           "loadvm": a.loadvm or None, "scenarios": {}, "frames": {}, "errors": []}
    t0 = time.monotonic()
    with open(log, "wb") as logf:
        q = subprocess.Popen(cmd, env=env, stdin=subprocess.DEVNULL, stdout=logf, stderr=subprocess.STDOUT)
    mon = None
    try:
        while not os.path.exists(mon_path):
            if q.poll() is not None or time.monotonic() - t0 > 30:
                raise RuntimeError("QEMU did not start (code %s, see qemu.log)" % q.poll())
            time.sleep(0.02)
        mon = Monitor(mon_path)
        scr = Screen(mon, tmp, t0)

        got = scr.wait_settled(a.poll, a.stable, a.gui_timeout, want=expect.get("gui"))
        if not got:
            raise RuntimeError("GUI not reached in %.0f s" % a.gui_timeout)
        t_gui, h_gui, w, h, px = got
        res["boot_to_gui_s"] = round(t_gui, 2)
        res["gui_confirmed_s"] = round(time.monotonic() - t0, 2)
        res["frames"]["gui"] = h_gui
        save_png(os.path.join(out, "gui.png"), w, h, px)

        prev = h_gui
        if not a.no_scenarios:
            for name, keys in cam["scenarios"]:
                tk = time.monotonic() - t0
                for k in keys:
                    mon.cmd("sendkey " + k)
                got = scr.wait_settled(a.poll, a.key_stable, 30, want=expect.get(name), changed_from=prev)
                if not got:
                    res["errors"].append("scenario %s: screen did not settle" % name)
                    break
                ts, hs, w, h, px = got
                res["scenarios"][name] = round(ts - tk, 2)
                res["frames"][name] = hs
                save_png(os.path.join(out, name + ".png"), w, h, px)
                prev = hs

        t_idle0 = time.monotonic() - t0
        time.sleep(a.idle)
        t_idle1 = time.monotonic() - t0
        res["idle_window_s"] = [round(t_idle0, 2), round(t_idle1, 2)]
        # Scripted steps (e.g. display off, sleep, key to wake): log offsets mark the segments
        steps = []
        for st in a.step:
            kind, _, arg = st.partition(":")
            if kind == "key":
                mon.cmd("sendkey " + arg)
            elif kind == "wait":
                time.sleep(float(arg))
            elif kind == "until":       # anchored to the guest, not to a fixed delay (load-robust)
                txt, n = arg.rsplit("=", 1) if "=" in arg else (arg, "1")
                tu = time.monotonic()
                while True:
                    got = open(log, "rb").read().count(txt.encode())
                    if got >= int(n):
                        break
                    if q.poll() is not None or time.monotonic() - tu > a.until_timeout:
                        raise RuntimeError("step %s: %d of %s lines after %.0f s" % (st, got, n, time.monotonic() - tu))
                    time.sleep(0.2)
            elif kind == "mon":         # e.g. mon:xp /8wx 0x2b60 (guest state, kept in result.json)
                res.setdefault("monitor", []).append([arg, mon.cmd(arg).strip()])
            elif kind == "shot":
                _, hs, _, w, h, px = scr.grab()
                res["frames"][arg] = hs
                save_png(os.path.join(out, arg + ".png"), w, h, px)
            else:
                raise RuntimeError("bad --step " + st)
            steps.append({"step": st, "t": round(time.monotonic() - t0, 2), "log": os.path.getsize(log)})
        res["steps"] = steps
        jit = mon.cmd("info jit")
        m = re.search(r"TB flush count\s+(\d+)", jit)
        res["tb_flushes"] = int(m.group(1)) if m else None
        m = re.search(r"Translation buffer state:.*?gen code size\s+(\d+)/(\d+)", jit, re.S)
        if m:
            res["tb_code_bytes"] = [int(m.group(1)), int(m.group(2))]
        if a.savevm:
            ts = time.monotonic()
            r = mon.cmd("savevm " + a.savevm)
            res["savevm_s"] = round(time.monotonic() - ts, 2)
            if r.strip():
                res["errors"].append("savevm: " + r.strip()[:300])
        res["host_cpu_user_s"], res["host_cpu_sys_s"], res["peak_rss_mib"] = proc_stats(q.pid)
        res["wall_s"] = round(time.monotonic() - t0, 2)
        mon.quit()
        q.wait(timeout=30)
    except (RuntimeError, OSError, ValueError, subprocess.TimeoutExpired) as e:
        res["errors"].append(str(e))
        print("ERROR: %s" % e, file=sys.stderr)
    finally:
        if mon:
            mon.close()
        if q.poll() is None:
            q.kill()
            q.wait()
        for p in (mon_path, os.path.join(tmp, "screen.ppm")):
            if os.path.exists(p):
                os.remove(p)
        if a.card:
            try:
                res["card"] = {"files": card.Fat(disks["sd"]).files(), "pictures": card.pictures(disks["sd"])}
                with open(os.path.join(out, "card.json"), "w") as f:
                    json.dump(res["card"], f, indent=1)
            except Exception as e:     # a broken card image is a test failure, not a crash
                res["errors"].append("card: %s" % e)
        if not a.disk_dir:
            shutil.rmtree(tmp, ignore_errors=True)

    res["screen_polls"] = scr.frames if mon else 0
    if mon and scr.latency:
        lat = sorted(scr.latency)
        res["screendump_ms"] = {"median": round(lat[len(lat) // 2] * 1000, 1), "max": round(lat[-1] * 1000, 1)}
    res["timeline"] = scr.timeline if mon else []
    recs = plugin_records(wj)
    ivl = [r for r in recs if r.get("type") == "interval"]
    if ivl and "boot_to_gui_s" in res:
        res["boot_insns"] = insns_until(ivl, res["boot_to_gui_s"] * 1000)
        res["boot_mips"] = round(res["boot_insns"] / res["boot_to_gui_s"] / 1e6, 2)
    if ivl and "idle_window_s" in res:
        res["idle"] = window_stats(ivl, res["idle_window_s"][0] * 1000, res["idle_window_s"][1] * 1000)
    summ = [r for r in recs if r.get("type") == "summary"]
    res["plugin_summary"] = summ[-1] if summ else None

    uart = open(os.path.join(out, "uart.log"), "rb").read().decode(errors="replace") \
        if os.path.exists(os.path.join(out, "uart.log")) else ""
    res["uart_markers"] = {m: (m in uart) for m in cam["uart_markers"]}
    text = open(log, "rb").read().decode(errors="replace")
    if a.debugmsg:
        res["log_markers"] = {m: (m in text) for m in cam["log_markers"]}
    res["log_bytes"] = len(text)
    if a.count:
        res["log_counts"] = {c: sum(1 for l in text.splitlines() if c in l) for c in a.count}
    if a.count and res.get("steps"):
        raw = open(log, "rb").read()
        prev = 0
        for st in res["steps"]:
            seg = raw[prev:st["log"]].decode(errors="replace").splitlines()
            st["counts"] = {c: sum(1 for l in seg if c in l) for c in a.count}
            prev = st["log"]
    for spec in a.min_count:
        txt, n = spec.rsplit("=", 1)
        got = sum(1 for l in text.splitlines() if txt in l)
        res.setdefault("min_counts", {})[txt] = got
        if got < int(n):
            res["errors"].append("log: %d lines with %r, expected >= %s" % (got, txt, n))
    res["qemu_errors"] = [l for l in text.splitlines() if "Assertion" in l or "qemu-system-arm:" in l][-5:]

    # Frames this run tries to capture: the GUI, the scenarios (unless skipped) and the step shots
    wanted = {"gui"} | (set() if a.no_scenarios else {n for n, _ in cam["scenarios"]}) | \
        {st.split(":", 1)[1] for st in a.step if st.startswith("shot:")}
    for name, want in expect.items():
        if name in wanted and res["frames"].get(name) != want:
            res["errors"].append("frame %s: got %s, expected %s" % (name, res["frames"].get(name), want))
    if a.min_pictures and len(res.get("card", {}).get("pictures", [])) < a.min_pictures:
        res["errors"].append("card: %d pictures, expected >= %d" % (len(res.get("card", {}).get("pictures", [])),
                                                                   a.min_pictures))
    for m, ok in res["uart_markers"].items():
        if not ok:
            res["errors"].append("UART marker missing: " + m)

    with open(os.path.join(out, "result.json"), "w") as f:
        json.dump(res, f, indent=1)
    short = {k: res.get(k) for k in ("label", "boot_to_gui_s", "boot_insns", "scenarios", "idle",
                                      "host_cpu_user_s", "peak_rss_mib", "tb_flushes", "savevm_s", "steps", "errors")}
    print(json.dumps(short), flush=True)
    sys.exit(1 if res["errors"] else 0)


if __name__ == "__main__":
    main()
