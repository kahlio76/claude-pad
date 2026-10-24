// Keystrokes for the actions Claude Code has no hook for (interrupt, mode,
// send, rewind). They go to whichever window is focused, so keep the terminal
// running Claude Code in front. macOS will ask once to grant Accessibility
// permission to the app padd runs in (Terminal, Ghostty, iTerm...).
#if canImport(CoreGraphics)
import CoreGraphics

enum Keys {
    static func escape() { tap(53) }
    static func enter() { tap(36) }
    static func shiftTab() { tap(48, flags: .maskShift) }

    private static func tap(_ code: CGKeyCode, flags: CGEventFlags = []) {
        let source = CGEventSource(stateID: .hidSystemState)
        for down in [true, false] {
            guard let e = CGEvent(keyboardEventSource: source, virtualKey: code, keyDown: down) else { return }
            e.flags = flags
            e.post(tap: .cghidEventTap)
        }
    }
}
#else
// Linux builds (CI, tests) have no keystroke injection.
enum Keys {
    static func escape() {}
    static func enter() {}
    static func shiftTab() {}
}
#endif
