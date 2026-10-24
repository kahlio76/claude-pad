// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "ClaudePad",
    platforms: [.macOS(.v13)],
    products: [
        .executable(name: "padd", targets: ["padd"]),
        .executable(name: "fakepad", targets: ["fakepad"]),
        .executable(name: "pad-hook", targets: ["pad-hook"]),
    ],
    targets: [
        // Your C keypad logic, unchanged. Swift imports it as `PadCore`.
        .target(name: "PadCore"),
        // Small shared helpers: socket paths, Unix sockets, line reading.
        .target(name: "PadKit"),
        // The daemon: owns the core, talks to the pad and to Claude Code.
        .executableTarget(name: "padd", dependencies: ["PadCore", "PadKit"]),
        // A keyboard-driven stand-in for the Pico until the hardware exists.
        .executableTarget(name: "fakepad", dependencies: ["PadKit"]),
        // What Claude Code's hooks run. Forwards each event to the daemon.
        .executableTarget(name: "pad-hook", dependencies: ["PadKit"]),
    ]
)
