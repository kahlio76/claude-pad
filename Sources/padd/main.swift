// padd: the Claude Pad daemon.
//
// One event loop connects three things:
//   the pad      key edges in ("D Approve" / "U Approve"), LED states out
//   Claude Code  hook events in through hook.sock; permission requests wait
//                here until the pad answers them
//   pad_core     your C logic; it decides what a press means
//
// Usage:
//   padd                       wait for fakepad on ~/.claude-pad/pad.sock
//   padd --serial /dev/cu.usbmodem1101   talk to the real Pico
//   padd --timeout 60          seconds before an unanswered request falls
//                              back to Claude Code's normal prompt
import Foundation
import PadCore
import PadKit

// MARK: - Model

/// Where a Claude Code session is, as far as the LEDs care.
enum Activity: String { case idle, work, wait, done }

/// A permission request held open until the pad answers it.
struct Request {
    let id: UInt64          // what pad_core binds a press to
    let fd: Int32           // the waiting pad-hook connection
    let tool: String
    let summary: String
    let deadline: UInt64
}

/// One Claude Code session occupying one of the four slots.
final class Session {
    let id: String
    let generation: UInt64
    var activity: Activity = .idle
    var queue: [Request] = []
    var alwaysAllowed: Set<String> = []   // tools granted with the Always key
    init(id: String, generation: UInt64) { self.id = id; self.generation = generation }
}

// MARK: - Daemon

final class Daemon {
    var core = PadCore()
    var actions: [PadAction] = []       // filled by the C callback, drained after
    let slotCount = Int(PAD_SLOTS)
    var slots: [Session?]
    var nextGeneration: UInt64 = 1
    var nextRequest: UInt64 = 1

    let serialPath: String?
    let requestTimeoutMs: UInt64
    var hookListen: Int32 = -1
    var padListen: Int32 = -1           // fake-pad mode only
    var pad: Int32 = -1
    var padBuffer = LineBuffer()
    var nextSerialTry: UInt64 = 0
    var hookClients: [Int32: LineBuffer] = [:]

    init(serialPath: String?, timeoutSeconds: UInt64) {
        self.serialPath = serialPath
        self.requestTimeoutMs = timeoutSeconds * 1000
        self.slots = Array(repeating: nil, count: Int(PAD_SLOTS))
        let emit: PadEmit = { user, action in
            guard let user, let action else { return }
            Unmanaged<Daemon>.fromOpaque(user).takeUnretainedValue().actions.append(action.pointee)
        }
        pad_init(&core, emit, Unmanaged.passUnretained(self).toOpaque())
    }

    func start() {
        PadPaths.ensureDir()
        guard let h = unixListen(PadPaths.hookSocket) else {
            log("another padd is already running (or \(PadPaths.hookSocket) is unusable)")
            exit(1)
        }
        hookListen = h
        if serialPath == nil {
            guard let p = unixListen(PadPaths.padSocket) else {
                log("can't listen on \(PadPaths.padSocket)"); exit(1)
            }
            padListen = p
            log("waiting for fakepad on \(PadPaths.padSocket)")
        }
        log("listening for Claude Code hooks on \(PadPaths.hookSocket)")
        run()
    }

    // MARK: Event loop

    func run() {
        while true {
            if serialPath != nil && pad < 0 { tryOpenSerial() }

            var fds: [pollfd] = []
            func watch(_ fd: Int32) { fds.append(pollfd(fd: fd, events: Int16(POLLIN), revents: 0)) }
            watch(hookListen)
            if padListen >= 0 { watch(padListen) }
            if pad >= 0 { watch(pad) }
            for fd in hookClients.keys { watch(fd) }

            // pad_core wants a tick at least every 10 ms to recognize holds.
            _ = poll(&fds, nfds_t(fds.count), 10)

            for p in fds where p.revents != 0 {
                if p.fd == hookListen { acceptHook() }
                else if p.fd == padListen { acceptFakePad() }
                else if p.fd == pad { readPad() }
                else { readHook(p.fd) }
            }
            pad_tick(&core, monotonicMs())
            drainActions()
            expireRequests()
        }
    }

    // MARK: Pad link

    func acceptFakePad() {
        let fd = accept(padListen, nil, nil)
        guard fd >= 0 else { return }
        if pad >= 0 { dropPad(reason: "replaced by a new fakepad") }
        setNonBlocking(fd)
        pad = fd
        log("pad connected")
        sendLEDs()
    }

    func tryOpenSerial() {
        let now = monotonicMs()
        guard now >= nextSerialTry, let path = serialPath else { return }
        nextSerialTry = now + 1000
        let fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK)
        guard fd >= 0 else { return }
        var tio = termios()
        tcgetattr(fd, &tio)
        cfmakeraw(&tio)
        cfsetspeed(&tio, speed_t(B115200))
        tcsetattr(fd, TCSANOW, &tio)
        pad = fd
        padBuffer = LineBuffer()
        log("pad connected on \(path)")
        sendLEDs()
    }

    func dropPad(reason: String) {
        // Never a synthetic key-up: your core guarantees a disconnect can't
        // send, close or approve anything.
        pad_cancel(&core, monotonicMs())
        drainActions()
        close(pad)
        pad = -1
        padBuffer = LineBuffer()
        log("pad disconnected (\(reason))")
    }

    func readPad() {
        guard padBuffer.fill(from: pad) else { dropPad(reason: "link closed"); return }
        while let line = padBuffer.nextLine() {
            let parts = line.split(separator: " ", maxSplits: 1).map(String.init)
            guard parts.count == 2, parts[0] == "D" || parts[0] == "U" else { continue }
            let k = pad_key_parse(parts[1])
            guard k >= 0 else { log("pad sent unknown key \(parts[1])"); continue }
            let key = PadKey(rawValue: numericCast(k))
            pad_edge(&core, key, parts[0] == "D", monotonicMs())
            drainActions()
        }
    }

    /// "L <slot1> <slot2> <slot3> <slot4> <selected>", each slot off|idle|work|wait|done.
    func sendLEDs() {
        guard pad >= 0 else { return }
        let states = slots.map { s -> String in
            guard let s else { return "off" }
            return s.queue.isEmpty ? s.activity.rawValue : Activity.wait.rawValue
        }
        if !writeAll(pad, "L \(states.joined(separator: " ")) \(core.selected + 1)\n") {
            dropPad(reason: "write failed")
        }
    }

    // MARK: Hook link

    func acceptHook() {
        while true {
            let fd = accept(hookListen, nil, nil)
            guard fd >= 0 else { return }
            setNonBlocking(fd)
            hookClients[fd] = LineBuffer()
        }
    }

    func readHook(_ fd: Int32) {
        guard var buf = hookClients[fd] else { return }
        let alive = buf.fill(from: fd)
        hookClients[fd] = buf
        if let line = buf.nextLine() {
            hookClients[fd] = nil   // one message per connection
            handleHook(line, fd: fd)
        } else if !alive {
            hookClients[fd] = nil
            forgetConnection(fd)
            close(fd)
        }
    }

    func handleHook(_ line: String, fd: Int32) {
        guard let obj = try? JSONSerialization.jsonObject(with: Data(line.utf8)) as? [String: Any],
              let event = obj["hook_event_name"] as? String,
              let sessionID = obj["session_id"] as? String else {
            close(fd); return
        }

        if event == "SessionEnd" {
            endSession(sessionID)
            close(fd); return
        }
        guard let slot = slotFor(sessionID) else {
            log("all \(slotCount) slots full, ignoring session \(sessionID.prefix(8))")
            reply(fd, "ask"); return
        }
        let session = slots[slot]!

        switch event {
        case "PermissionRequest":
            let tool = obj["tool_name"] as? String ?? "?"
            if session.alwaysAllowed.contains(tool) {
                log("S\(slot + 1) \(tool): allowed (Always is on for this tool)")
                reply(fd, "allow"); return
            }
            let req = Request(id: nextRequest, fd: fd, tool: tool,
                              summary: summarize(obj["tool_input"]),
                              deadline: monotonicMs() + requestTimeoutMs)
            nextRequest += 1
            session.queue.append(req)
            log("S\(slot + 1) waiting: \(tool) \(req.summary)")
            // If the selected slot has nothing to answer, jump to this one.
            if slots[Int(core.selected)]?.queue.isEmpty ?? true { pad_select(&core, Int32(slot)) }
            publish(slot)
            return                       // fd stays open until the pad answers
        case "UserPromptSubmit", "PreToolUse", "PostToolUse":
            session.activity = .work
        case "Stop":
            session.activity = .done
        case "Notification":
            if obj["notification_type"] as? String == "idle_prompt" { session.activity = .idle }
        default:
            break                        // SessionStart just claims the slot
        }
        close(fd)
        publish(slot)
    }

    func reply(_ fd: Int32, _ answer: String) {
        writeAll(fd, answer + "\n")
        close(fd)
    }

    /// A hook gave up (Claude Code killed it): drop its request without answering.
    func forgetConnection(_ fd: Int32) {
        for (i, s) in slots.enumerated() {
            guard let s, let idx = s.queue.firstIndex(where: { $0.fd == fd }) else { continue }
            log("S\(i + 1) \(s.queue[idx].tool): hook went away, dropping request")
            s.queue.remove(at: idx)
            publish(i)
        }
    }

    // MARK: Sessions and slots

    func slotFor(_ id: String) -> Int? {
        if let i = slots.firstIndex(where: { $0?.id == id }) { return i }
        guard let free = slots.firstIndex(where: { $0 == nil }) else { return nil }
        slots[free] = Session(id: id, generation: nextGeneration)
        nextGeneration += 1
        log("S\(free + 1) ← session \(id.prefix(8))")
        return free
    }

    func endSession(_ id: String) {
        guard let i = slots.firstIndex(where: { $0?.id == id }), let s = slots[i] else { return }
        for r in s.queue { reply(r.fd, "ask") }   // hand anything pending back to Claude Code
        slots[i] = nil
        log("S\(i + 1) session ended")
        publish(i)
    }

    /// Tell pad_core who is in a slot and which request (if any) is waiting.
    func publish(_ slot: Int) {
        let s = slots[slot]
        // A freed slot gets a fresh generation so old presses can't touch it.
        let gen = s?.generation ?? { defer { nextGeneration += 1 }; return nextGeneration }()
        pad_slot(&core, Int32(slot), s != nil, gen, s?.queue.first?.id ?? 0)
        sendLEDs()
    }

    func expireRequests() {
        let now = monotonicMs()
        for (i, s) in slots.enumerated() {
            guard let s, let head = s.queue.first, now >= head.deadline else { continue }
            log("S\(i + 1) \(head.tool): no answer from the pad, falling back to Claude Code's prompt")
            s.queue.removeFirst()
            reply(head.fd, "ask")
            publish(i)
        }
    }

    // MARK: Actions from pad_core

    func drainActions() {
        while !actions.isEmpty {
            let batch = actions
            actions.removeAll()
            batch.forEach(handle)
        }
    }

    func handle(_ a: PadAction) {
        let name = String(cString: pad_action_name(a.type))
        let slot = Int(a.slot)
        guard slot >= 0, slot < slotCount else { return }
        let s = slots[slot].flatMap { $0.generation == a.generation ? $0 : nil }

        switch a.type {
        case PAD_SELECT:
            pad_select(&core, a.slot)
            log("selected S\(slot + 1)")
            sendLEDs()
        case PAD_ALLOW_ONCE, PAD_ALLOW_SESSION, PAD_REJECT:
            // Answer only the exact request that was waiting at key-down.
            guard let s, let head = s.queue.first, head.id == a.request else { return }
            s.queue.removeFirst()
            if a.type == PAD_REJECT {
                reply(head.fd, "deny")
                log("S\(slot + 1) \(head.tool): denied")
            } else {
                reply(head.fd, "allow")
                s.activity = .work
                if a.type == PAD_ALLOW_SESSION {
                    s.alwaysAllowed.insert(head.tool)
                    log("S\(slot + 1) \(head.tool): allowed, and always for this session")
                    // Anything else already queued for the same tool goes through too.
                    for r in s.queue where r.tool == head.tool { reply(r.fd, "allow") }
                    s.queue.removeAll { $0.tool == head.tool }
                } else {
                    log("S\(slot + 1) \(head.tool): allowed once")
                }
            }
            publish(slot)
        case PAD_INTERRUPT:
            Keys.escape(); log("interrupt (Esc)")
        case PAD_REWIND:
            Keys.escape(); Keys.escape(); log("rewind (Esc Esc)")
        case PAD_CYCLE_MODE:
            Keys.shiftTab(); log("cycle mode (Shift+Tab)")
        case PAD_SEND:
            Keys.enter(); log("send (Return)")
        default:
            // create, close, agents, plan, voice: not wired to Claude Code yet.
            log("\(name) on S\(slot + 1): not wired up yet")
        }
    }

    func summarize(_ input: Any?) -> String {
        guard let d = input as? [String: Any] else { return "" }
        let s = (d["command"] ?? d["file_path"] ?? d["url"] ?? d["pattern"]) as? String ?? ""
        return s.count > 60 ? String(s.prefix(57)) + "..." : s
    }
}

// MARK: - Main

signal(SIGPIPE, SIG_IGN)   // a vanished hook must not kill the daemon
setvbuf(stderr, nil, _IOLBF, 0)

var serial: String?
var timeout: UInt64 = 60
var args = CommandLine.arguments.dropFirst()
while let a = args.popFirst() {
    switch a {
    case "--serial": serial = args.popFirst()
    case "--timeout": timeout = args.popFirst().flatMap { UInt64($0) } ?? timeout
    default:
        fputs("usage: padd [--serial DEVICE] [--timeout SECONDS]\n", stderr); exit(2)
    }
}
Daemon(serialPath: serial, timeoutSeconds: timeout).start()
