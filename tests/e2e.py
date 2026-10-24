#!/usr/bin/env python3
"""End-to-end test: real padd + real pad-hook, with a scripted fake pad.

Run after `swift build`:  python3 tests/e2e.py
"""
import json, os, socket, subprocess, sys, tempfile, threading, time

BIN = os.path.join(os.path.dirname(__file__), "..", ".build", "debug")
DIR = tempfile.mkdtemp(prefix="claude-pad-")
ENV = dict(os.environ, CLAUDE_PAD_DIR=DIR)
failures = 0


def check(name, ok, detail=""):
    global failures
    print(("pass  " if ok else "FAIL  ") + name + ("" if ok else f"   ({detail})"))
    failures += 0 if ok else 1


def hook(event, session="sess-a", tool="Bash", wait=True, **extra):
    """Run pad-hook like Claude Code would. Returns a handle to collect output."""
    payload = {"hook_event_name": event, "session_id": session, "tool_name": tool,
               "tool_input": {"command": "rm -rf build"}, **extra}
    p = subprocess.Popen([os.path.join(BIN, "pad-hook")], env=ENV, stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, text=True)
    p.stdin.write(json.dumps(payload)); p.stdin.close()
    if not wait:
        return p
    p.wait(timeout=10)
    return p.stdout.read()


def result(p, timeout=10):
    p.wait(timeout=timeout)
    out = p.stdout.read()
    return json.loads(out)["hookSpecificOutput"]["decision"]["behavior"] if out else "none"


class Pad:
    def __init__(self):
        self.s = socket.socket(socket.AF_UNIX)
        for _ in range(50):
            try:
                self.s.connect(os.path.join(DIR, "pad.sock")); break
            except OSError:
                time.sleep(0.1)
        self.leds, self.buf = "", b""
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        while True:
            d = self.s.recv(4096)
            if not d:
                return
            self.buf += d
            *lines, self.buf = self.buf.split(b"\n")
            for l in lines:
                if l.startswith(b"L "):
                    self.leds = l.decode()

    def tap(self, key):
        self.s.sendall(f"D {key}\nU {key}\n".encode()); time.sleep(0.15)

    def down(self, key): self.s.sendall(f"D {key}\n".encode()); time.sleep(0.05)
    def up(self, key): self.s.sendall(f"U {key}\n".encode()); time.sleep(0.15)
    def close(self): self.s.close(); time.sleep(0.2)


# No daemon yet: the hook must stay out of the way.
t = time.time()
out = hook("PermissionRequest")
check("no padd running: hook prints nothing and exits fast", out == "" and time.time() - t < 3)

daemon = subprocess.Popen([os.path.join(BIN, "padd"), "--timeout", "3"], env=ENV,
                          stderr=open(os.path.join(DIR, "padd.log"), "w"))
pad = Pad()

try:
    hook("SessionStart")
    time.sleep(0.2)
    check("session claims S1, LEDs report it", pad.leds.startswith("L idle off off off 1"), pad.leds)

    p = hook("PermissionRequest", wait=False); time.sleep(0.3)
    check("permission request lights S1 as waiting", pad.leds.startswith("L wait"), pad.leds)
    pad.tap("Approve")
    check("Approve answers allow", result(p) == "allow")

    p = hook("PermissionRequest", wait=False); time.sleep(0.3)
    pad.tap("Deny")
    check("Deny answers deny", result(p) == "deny")

    p = hook("PermissionRequest", wait=False); time.sleep(0.3)
    t = time.time()
    check("unanswered request falls back to Claude's prompt", result(p) == "none" and time.time() - t > 2)

    p = hook("PermissionRequest", wait=False); time.sleep(0.3)
    pad.tap("Always")
    time.sleep(0.2)
    check("a quick tap on Always does not answer", p.poll() is None)
    pad.down("Always"); time.sleep(1.15); pad.up("Always")
    check("holding Always for 1 s allows", result(p) == "allow")
    check("after Always, the same tool is allowed without asking", result(hook("PermissionRequest", wait=False)) == "allow")

    p = hook("PermissionRequest", tool="Write", wait=False); time.sleep(0.3)
    pad.down("Approve")
    pad.close()
    pad = Pad(); time.sleep(0.2)
    pad.tap("Deny")
    check("pad disconnect mid-press approves nothing (Deny still works after)", result(p) == "deny")

    hook("SessionStart", session="sess-b")
    p = hook("PermissionRequest", session="sess-b", tool="Edit", wait=False); time.sleep(0.3)
    check("second session lands in S2 and gets auto-selected", pad.leds == "L work wait off off 2", pad.leds)
    pad.tap("Approve")
    check("Approve acts on the selected slot", result(p) == "allow")

    pad.tap("S1"); time.sleep(0.1)
    check("tapping S1 selects it", pad.leds.endswith(" 1"), pad.leds)

    p = hook("PermissionRequest", session="sess-b", tool="Edit", wait=False); time.sleep(0.3)
    hook("SessionEnd", session="sess-b"); time.sleep(0.2)
    check("ending a session hands its pending request back to Claude", result(p) == "none")
    check("ended session frees S2", pad.leds.startswith("L work off off off"), pad.leds)

    hook("UserPromptSubmit"); time.sleep(0.2)
    check("prompt submit shows working", pad.leds.startswith("L work"), pad.leds)
    hook("Stop"); time.sleep(0.2)
    check("stop shows done", pad.leds.startswith("L done"), pad.leds)

    pad.tap("Stop"); pad.tap("Mode")
    check("Stop and Mode don't crash the daemon", daemon.poll() is None)
finally:
    daemon.terminate()
    log = open(os.path.join(DIR, "padd.log")).read()

print("\n--- padd log ---\n" + log)
print("all passed" if not failures else f"{failures} FAILED")
sys.exit(1 if failures else 0)
