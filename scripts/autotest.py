"""Run a Kenshi Town scenario against the real game and collect the results.

    python scripts/autotest.py scripts/autotest/smoke.txt
    python scripts/autotest.py scripts/autotest/repair.txt --no-install --timeout 600

What it does:
  1. refuses if Kenshi is already running (the test takes over the game);
  2. builds and installs the plugin and data mods (unless --no-install);
  3. puts the scenario at Kenshi/mods/TownLedger/autotest/scenario.txt, which
     switches the plugin into autopilot (see scripts/autotest/README.md);
  4. launches Kenshi and waits for the plugin's report to say DONE, Kenshi to
     exit, or the timeout;
  5. always removes the scenario again (so normal play is unaffected), ends
     Kenshi if it's still running, and copies report + screenshots to
     build/autotest/<scenario>-<time>/.

Exit status 0 only if the scenario finished with no FAIL lines.
"""

import argparse
import datetime
import os
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KENSHI = os.environ.get("KENSHI_DIR", r"C:\Program Files (x86)\Steam\steamapps\common\Kenshi")
EXE = os.path.join(KENSHI, "kenshi_x64.exe")
AT_DIR = os.path.join(KENSHI, "mods", "TownLedger", "autotest")
SCENARIO = os.path.join(AT_DIR, "scenario.txt")
OUT = os.path.join(AT_DIR, "out")
REPORT = os.path.join(OUT, "report.txt")
SAVES = os.path.join(os.environ.get("LOCALAPPDATA", ""), "kenshi", "save")
PLUGIN_LOG = os.path.join(KENSHI, "RE_Kenshi_log.txt")
STATE_FILES = ["contracts.txt", "profiles.txt", "hub_cleanup.txt"]


def kenshi_running():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq kenshi_x64.exe"], capture_output=True, text=True).stdout
    return "kenshi_x64.exe" in out.lower()


def check_saves(scenario_lines):
    for line in scenario_lines:
        parts = line.split(None, 1)
        if parts and parts[0] == "load" and len(parts) > 1:
            if not os.path.isdir(os.path.join(SAVES, parts[1].strip())):
                raise SystemExit("save %r not found in %s" % (parts[1].strip(), SAVES))


def kenshi_pids():
    out = subprocess.run(["tasklist", "/FO", "CSV", "/NH", "/FI", "IMAGENAME eq kenshi_x64.exe"],
                         capture_output=True, text=True).stdout
    return [int(line.split('","')[1]) for line in out.splitlines() if line.lower().startswith('"kenshi_x64.exe"')]


def click_launcher_ok():
    """Kenshi opens a settings launcher ("Kenshi 1.0.x - x64 (...)", with Video
    settings / Mods tabs) before the game. Press its OK through Windows
    messages, without touching the mouse. True if it was there."""
    import ctypes
    from ctypes import wintypes
    user32 = ctypes.windll.user32
    proc_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def text(h):
        buf = ctypes.create_unicode_buffer(256)
        user32.GetWindowTextW(h, buf, 256)
        return buf.value

    dialogs = []

    def top(h, _):
        t = text(h)
        if user32.IsWindowVisible(h) and t.startswith("Kenshi 1.") and " - x64" in t:
            dialogs.append(h)
        return True

    user32.EnumWindows(proc_type(top), 0)
    for dialog in dialogs:
        buttons = []

        def child(h, _):
            if text(h) == "OK":
                buttons.append(h)
            return True

        user32.EnumChildWindows(dialog, proc_type(child), 0)
        if buttons:
            user32.PostMessageW(buttons[0], 0x00F5, 0, 0)  # BM_CLICK
            return True
    return False


def read_report():
    try:
        with open(REPORT, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("scenario")
    ap.add_argument("--no-install", action="store_true")
    ap.add_argument("--timeout", type=int, default=420, help="seconds before Kenshi is ended")
    args = ap.parse_args()

    with open(args.scenario, encoding="utf-8") as f:
        lines = [l.strip() for l in f if l.strip() and not l.strip().startswith("#")]
    check_saves(lines)
    if kenshi_running():
        raise SystemExit("Kenshi is running: close it first (the test drives the game itself)")

    if not args.no_install:
        r = subprocess.run(["cmd", "/c", os.path.join(REPO, "scripts", "build.cmd"), "install"],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout[-3000:], r.stderr[-2000:])
            raise SystemExit("build/install failed")
        print("built and installed")

    # The plugin keeps per-save state (guided-start progress, garrison) in
    # files next to it; a test that jumps tasks would otherwise leak into the
    # save's record for the next run (2026-11-22: kt-test woke up on task 4).
    state = {}
    for f in STATE_FILES:
        path = os.path.join(KENSHI, "mods", "TownLedger", f)
        if os.path.isfile(path):
            with open(path, "rb") as fh:
                state[path] = fh.read()

    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    os.makedirs(OUT)
    shutil.copyfile(args.scenario, SCENARIO)

    name = os.path.splitext(os.path.basename(args.scenario))[0]
    started = time.time()
    try:
        # Started directly, Kenshi hands over to Steam and exits; Steam starts
        # it again as a new process. So follow kenshi_x64.exe by name.
        subprocess.Popen([EXE], cwd=KENSHI)
        print("launched Kenshi; waiting up to %ds" % args.timeout)
        clicked = False
        last_seen = time.time()
        while time.time() - started < args.timeout:
            if "\nDONE\t" in "\n" + read_report():
                break
            if kenshi_pids():
                last_seen = time.time()
            elif time.time() - last_seen > 30:
                print("Kenshi is gone")
                break
            if not clicked and click_launcher_ok():
                clicked = True
                print("pressed OK on Kenshi's launcher (%ds)" % (time.time() - started))
            time.sleep(1)
    finally:
        try:
            os.remove(SCENARIO)
        except OSError:
            pass
        time.sleep(3)
        for pid in kenshi_pids():
            subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True)
            print("ended Kenshi (pid %d)" % pid)
        for path, data in state.items():
            with open(path, "wb") as fh:
                fh.write(data)

    report = read_report()
    dest = os.path.join(REPO, "build", "autotest", "%s-%s" % (name, datetime.datetime.now().strftime("%Y%m%d-%H%M%S")))
    shutil.copytree(OUT, dest)
    print("\n=== report (%ds) ===" % (time.time() - started))
    print(report or "(no report: the plugin never started the scenario)")
    shots = [f for f in os.listdir(dest) if f.endswith(".png")]
    if shots:
        print("screenshots:", ", ".join(os.path.join(dest, s) for s in sorted(shots)))
    try:
        with open(PLUGIN_LOG, encoding="utf-8", errors="replace") as f:
            tail = [l.rstrip() for l in f if "TownLedger" in l and " hooked " not in l]
        print("\n=== plugin log (last 15) ===")
        print("\n".join(tail[-15:]))
    except OSError:
        pass
    print("\nresults in", dest)
    done = [l for l in report.splitlines() if l.startswith("DONE")]
    fails = [l for l in report.splitlines() if l.startswith("FAIL")]
    sys.exit(0 if done and not fails else 1)


if __name__ == "__main__":
    main()
