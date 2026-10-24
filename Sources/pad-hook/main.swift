// pad-hook: what Claude Code's hooks run.
//
// Reads the hook event JSON from stdin and forwards it to padd. For a
// PermissionRequest it waits for the pad's answer and prints the decision
// Claude Code expects. If padd isn't running, or anything goes wrong, it
// prints nothing and exits 0, which leaves Claude Code's normal behavior
// untouched: a broken pad must never block Claude.
import Foundation
import PadKit

let input = FileHandle.standardInput.readDataToEndOfFile()
guard let obj = try? JSONSerialization.jsonObject(with: input) as? [String: Any],
      let event = obj["hook_event_name"] as? String,
      let line = try? JSONSerialization.data(withJSONObject: obj),
      let fd = unixConnect(PadPaths.hookSocket) else { exit(0) }

writeAll(fd, String(decoding: line, as: UTF8.self) + "\n")
guard event == "PermissionRequest" else { close(fd); exit(0) }

// Wait for "allow", "deny" or "ask". padd answers "ask" itself after its own
// timeout, so this deadline is only a safety net.
var buf = LineBuffer()
let deadline = monotonicMs() + 10 * 60 * 1000
var answer: String?
while answer == nil && monotonicMs() < deadline {
    var p = pollfd(fd: fd, events: Int16(POLLIN), revents: 0)
    guard poll(&p, 1, 1000) >= 0 else { break }
    if p.revents == 0 { continue }
    guard buf.fill(from: fd) else { break }
    answer = buf.nextLine()
}

func decide(_ behavior: String, _ extra: [String: Any] = [:]) {
    var decision: [String: Any] = ["behavior": behavior]
    extra.forEach { decision[$0] = $1 }
    let out: [String: Any] = ["hookSpecificOutput": [
        "hookEventName": "PermissionRequest",
        "decision": decision,
    ]]
    if let data = try? JSONSerialization.data(withJSONObject: out) {
        FileHandle.standardOutput.write(data)
    }
}

switch answer {
case "allow": decide("allow")
case "deny": decide("deny", ["message": "Denied from Claude Pad."])
default: break   // "ask" or no answer: Claude Code shows its usual prompt
}
exit(0)
