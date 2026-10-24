// Shared plumbing for padd, fakepad and pad-hook: where the sockets live,
// how to open them, and how to read newline-terminated messages.
#if canImport(Darwin)
import Darwin
#else
import Glibc
#endif

public enum PadPaths {
    /// Everything lives in ~/.claude-pad (override with CLAUDE_PAD_DIR).
    /// The folder is private (0700): anything that can write to pad.sock
    /// can press Approve, so other users must not be able to reach it.
    public static var dir: String {
        if let d = getenv("CLAUDE_PAD_DIR") { return String(cString: d) }
        let home = getenv("HOME").map { String(cString: $0) } ?? "/tmp"
        return home + "/.claude-pad"
    }
    public static var hookSocket: String { dir + "/hook.sock" }
    public static var padSocket: String { dir + "/pad.sock" }

    public static func ensureDir() {
        mkdir(dir, 0o700)
        chmod(dir, 0o700)
    }
}

/// Milliseconds from a clock that never jumps backwards (pad_core requires one).
public func monotonicMs() -> UInt64 {
    var ts = timespec()
    clock_gettime(CLOCK_MONOTONIC, &ts)
    return UInt64(ts.tv_sec) * 1000 + UInt64(ts.tv_nsec) / 1_000_000
}

#if canImport(Glibc)
private let streamType = Int32(SOCK_STREAM.rawValue)
#else
private let streamType = SOCK_STREAM
#endif

private func withAddress<T>(_ path: String, _ body: (UnsafePointer<sockaddr>, socklen_t) -> T) -> T? {
    var addr = sockaddr_un()
    addr.sun_family = sa_family_t(AF_UNIX)
    let bytes = Array(path.utf8)
    let fits = withUnsafeMutableBytes(of: &addr.sun_path) { buf -> Bool in
        guard bytes.count < buf.count else { return false }
        for (i, b) in bytes.enumerated() { buf[i] = b }
        buf[bytes.count] = 0
        return true
    }
    guard fits else { return nil }
    #if canImport(Darwin)
    addr.sun_len = UInt8(MemoryLayout<sockaddr_un>.size)
    #endif
    return withUnsafePointer(to: &addr) {
        $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
            body($0, socklen_t(MemoryLayout<sockaddr_un>.size))
        }
    }
}

/// Connect to a Unix socket. Returns nil if nothing is listening.
public func unixConnect(_ path: String) -> Int32? {
    let fd = socket(AF_UNIX, streamType, 0)
    guard fd >= 0 else { return nil }
    let rc = withAddress(path) { connect(fd, $0, $1) } ?? -1
    if rc != 0 { close(fd); return nil }
    return fd
}

/// Listen on a Unix socket, replacing a stale socket file left by a crash.
public func unixListen(_ path: String) -> Int32? {
    if let probe = unixConnect(path) { close(probe); return nil } // already in use
    unlink(path)
    let fd = socket(AF_UNIX, streamType, 0)
    guard fd >= 0 else { return nil }
    let rc = withAddress(path) { bind(fd, $0, $1) } ?? -1
    guard rc == 0, listen(fd, 16) == 0 else { close(fd); return nil }
    chmod(path, 0o600)
    setNonBlocking(fd)
    return fd
}

public func setNonBlocking(_ fd: Int32) {
    let flags = fcntl(fd, F_GETFL, 0)
    _ = fcntl(fd, F_SETFL, flags | O_NONBLOCK)
}

/// Write the whole string, retrying short writes. False if the peer is gone.
@discardableResult
public func writeAll(_ fd: Int32, _ s: String) -> Bool {
    var bytes = Array(s.utf8)[...]
    while !bytes.isEmpty {
        let n = bytes.withUnsafeBytes { write(fd, $0.baseAddress, $0.count) }
        if n < 0 {
            if errno == EINTR || errno == EAGAIN { usleep(1000); continue }
            return false
        }
        bytes = bytes.dropFirst(n)
    }
    return true
}

/// Collects bytes from a stream and hands back complete lines.
public struct LineBuffer {
    private var bytes: [UInt8] = []
    public init() {}

    /// Read whatever is available. Returns false on EOF or error.
    public mutating func fill(from fd: Int32) -> Bool {
        var chunk = [UInt8](repeating: 0, count: 4096)
        let n = chunk.withUnsafeMutableBytes { read(fd, $0.baseAddress, $0.count) }
        if n > 0 { bytes.append(contentsOf: chunk[0..<n]); return true }
        if n < 0 && (errno == EAGAIN || errno == EINTR) { return true }
        return false
    }

    public mutating func nextLine() -> String? {
        guard let nl = bytes.firstIndex(of: 10) else { return nil }
        let line = String(decoding: bytes[0..<nl], as: UTF8.self)
        bytes.removeFirst(nl + 1)
        return line.trimmingCharacters(in: " \r")
    }

    public var isEmpty: Bool { bytes.isEmpty }
}

extension String {
    func trimmingCharacters(in set: String) -> String {
        let drop = Set(set)
        var s = Substring(self)
        while let c = s.first, drop.contains(c) { s.removeFirst() }
        while let c = s.last, drop.contains(c) { s.removeLast() }
        return String(s)
    }
}

/// Timestamped log line on stderr.
public func log(_ msg: String) {
    var t = time(nil)
    var tmv = tm()
    localtime_r(&t, &tmv)
    let stamp = String(format2: tmv.tm_hour, tmv.tm_min, tmv.tm_sec)
    fputs("[\(stamp)] \(msg)\n", stderr)
}

extension String {
    init(format2 h: Int32, _ m: Int32, _ s: Int32) {
        func p(_ v: Int32) -> String { v < 10 ? "0\(v)" : "\(v)" }
        self = "\(p(h)):\(p(m)):\(p(s))"
    }
}
