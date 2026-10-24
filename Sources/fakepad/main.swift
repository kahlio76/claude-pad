// fakepad: the Pico, played by your keyboard.
//
// Speaks exactly what the real firmware will: "D <Key>" / "U <Key>" lines
// out, "L ..." LED lines in. Terminals can't report key releases, so a
// lowercase key is a quick tap, and its uppercase twin toggles a hold
// (press once to hold down, again to let go). Always needs a 1 s hold:
// press W, wait, press W.
#if canImport(Darwin)
import Darwin
#else
import Glibc
#endif
import PadKit

let taps: [Character: String] = [
    "1": "S1", "2": "S2", "3": "S3", "4": "S4",
    "a": "Approve", "w": "Always", "d": "Deny",
    "m": "Mode", "t": "Talk", "s": "Stop",
]
let holds: [Character: String] = [
    "!": "S1", "@": "S2", "#": "S3", "$": "S4",
    "A": "Approve", "W": "Always", "D": "Deny",
    "M": "Mode", "T": "Talk", "S": "Stop", "f": "FN", "F": "FN",
]

guard let fd = unixConnect(PadPaths.padSocket) else {
    fputs("padd isn't running. Start it first: swift run padd\n", stderr)
    exit(1)
}
setNonBlocking(fd)

// Raw-ish terminal: keys arrive one at a time, unechoed. Restored on exit.
var saved = termios()
tcgetattr(STDIN_FILENO, &saved)
var raw = saved
raw.c_lflag &= ~tcflag_t(ICANON | ECHO)
tcsetattr(STDIN_FILENO, TCSANOW, &raw)
func restore() { tcsetattr(STDIN_FILENO, TCSANOW, &saved); print("") }

var held: [String] = []
var leds = "waiting for padd..."

func render() {
    let h = held.isEmpty ? "" : "   holding: " + held.joined(separator: " ")
    print("\r\u{1B}[2K" + leds + h, terminator: "")
    fflush(stdout)
}

func send(_ s: String) {
    guard writeAll(fd, s + "\n") else { restore(); print("padd went away"); exit(1) }
}

func showLEDs(_ line: String) {
    // "L wait work off off 1" -> [S1 WAIT]* [S2 work] [S3 ----] [S4 ----]
    let p = line.split(separator: " ").dropFirst().map(String.init)
    guard p.count == 5, let sel = Int(p[4]) else { return }
    leds = (0..<4).map { i in
        let st = p[i] == "off" ? "----" : (p[i] == "wait" ? "WAIT" : p[i])
        return "[S\(i + 1) \(st)]" + (i + 1 == sel ? "*" : " ")
    }.joined(separator: " ")
}

print("""
fakepad: tap keys   1-4 select   a approve   d deny   m mode   t talk   s stop
         hold keys  shift+1-4 peek   W always (hold 1 s)   f FN   A D M T S hold
         q quits.   * marks the selected slot.  WAIT means Claude is asking.

""")
render()

var buf = LineBuffer()
while true {
    var fds = [pollfd(fd: STDIN_FILENO, events: Int16(POLLIN), revents: 0),
               pollfd(fd: fd, events: Int16(POLLIN), revents: 0)]
    _ = poll(&fds, 2, -1)

    if fds[1].revents != 0 {
        guard buf.fill(from: fd) else { restore(); print("padd went away"); exit(1) }
        while let line = buf.nextLine() {
            if line.hasPrefix("L ") { showLEDs(line) }
        }
        render()
    }
    if fds[0].revents != 0 {
        var c: UInt8 = 0
        guard read(STDIN_FILENO, &c, 1) == 1 else { continue }
        let ch = Character(UnicodeScalar(c))
        if ch == "q" {
            for k in held { send("U \(k)") }
            restore(); exit(0)
        }
        if let k = taps[ch] {
            send("D \(k)"); usleep(40_000); send("U \(k)")
        } else if let k = holds[ch] {
            if let i = held.firstIndex(of: k) { held.remove(at: i); send("U \(k)") }
            else { held.append(k); send("D \(k)") }
        }
        render()
    }
}
