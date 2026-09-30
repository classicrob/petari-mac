// Bounded real macOS keyboard holds for an explicitly authorized CUA playtest.
// Usage: cu-hold-input <active-session.json> <keys joined by comma> <seconds>
import AppKit
import CoreGraphics
import Foundation

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data((message + "\n").utf8))
    exit(2)
}
let args = CommandLine.arguments
guard args.count == 4, let duration = Double(args[3]), duration > 0, duration <= 3 else {
    fail("Expected active-session.json, comma-separated keys, and duration 0..3 seconds")
}
let codes: [String: CGKeyCode] = ["w":13,"a":0,"s":1,"d":2,"space":49,"f":3,"q":12,"e":14,"c":8,"shift":56]
let names = args[2].split(separator: ",").map(String.init)
guard !names.isEmpty, names.count <= 3, Set(names).count == names.count,
      names.allSatisfy({ codes[$0] != nil }) else { fail("Unsupported or repeated key") }
let recordURL = URL(fileURLWithPath: args[1]).standardizedFileURL
let data = try Data(contentsOf: recordURL)
guard let record = try JSONSerialization.jsonObject(with: data) as? [String: Any],
      let rawPid = record["pid"] as? Int, let appPath = record["app"] as? String,
      let app = NSRunningApplication(processIdentifier: pid_t(rawPid)),
      app.bundleURL?.standardizedFileURL.path == URL(fileURLWithPath: appPath).standardizedFileURL.path else {
    fail("The recorded playtest app must be alive with the expected bundle path")
}
let root = recordURL.deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
for name in [".petari-app.lock"] {
    let owner = try String(contentsOf: root.appendingPathComponent("build/" + name + "/owner"), encoding: .utf8)
    guard owner.trimmingCharacters(in: .whitespacesAndNewlines) == "cu-playtest" else { fail("Playtest must own app lock") }
}
guard CGPreflightPostEventAccess() else { fail("macOS event-post permission is not available; no permission prompt was opened") }
let source = CGEventSource(stateID: .hidSystemState)
let eventLock = NSRecursiveLock()
var held: [CGKeyCode] = []
func log(_ action: String, key: CGKeyCode? = nil, details: [String: Any] = [:]) {
    var item: [String: Any] = ["utc":ISO8601DateFormatter().string(from: Date()), "action":action,
                               "pid":rawPid,"keys":names,"seconds":duration,"mechanism":"CGEvent"]
    item.merge(details) { _, new in new }
    if let key = key { item["keycode"] = Int(key) }
    do {
        var bytes = try JSONSerialization.data(withJSONObject:item, options:[.sortedKeys]); bytes.append(10)
        let url = recordURL.deletingLastPathComponent().appendingPathComponent("actions.jsonl")
        let file = try FileHandle(forWritingTo:url); try file.seekToEnd(); try file.write(contentsOf:bytes); try file.close()
    } catch { FileHandle.standardError.write(Data(("input log error: \(error)\n").utf8)) }
}
func sameApp() -> Bool {
    guard let current = NSRunningApplication(processIdentifier: pid_t(rawPid)) else { return false }
    return current.bundleURL?.standardizedFileURL.path == URL(fileURLWithPath: appPath).standardizedFileURL.path
}
func frontDetails() -> [String: Any] {
    let front = NSWorkspace.shared.frontmostApplication
    return ["front_pid": Int(front?.processIdentifier ?? -1),
            "front_bundle_id": front?.bundleIdentifier ?? "nil",
            "front_bundle_path": front?.bundleURL?.path ?? "nil"]
}
func focused() -> Bool {
    guard sameApp() else {
        log("os-focus-target-invalid", details: frontDetails())
        return false
    }
    let first = frontDetails()
    if first["front_pid"] as? Int == rawPid { return true }
    log("os-focus-mismatch-first", details: first)
    RunLoop.current.run(until: Date().addingTimeInterval(0.04))
    let second = frontDetails()
    guard sameApp(), second["front_pid"] as? Int == rawPid else {
        log("os-focus-failure", details: second)
        FileHandle.standardError.write(Data(("Focus failure: first=\(first), second=\(second)\n").utf8))
        return false
    }
    log("os-focus-transient-recovered", details: second)
    return true
}
func release() {
    eventLock.lock(); defer { eventLock.unlock() }
    for key in held.reversed() {
        let front = frontDetails()
        let frontmost = sameApp() && front["front_pid"] as? Int == rawPid
        log(frontmost ? "os-key-up" : "os-key-up-focus-lost-cleanup", key:key, details: front)
        // Cleanup goes only to the original process even if another app gained focus.
        if sameApp() { CGEvent(keyboardEventSource:source, virtualKey:key, keyDown:false)?.postToPid(pid_t(rawPid)) }
    }
    held.removeAll()
}
let signalQueue = DispatchQueue(label:"cu-hold-input.signals")
var signalSources: [DispatchSourceSignal] = []
for number in [SIGTERM, SIGINT, SIGHUP] {
    signal(number, SIG_IGN)
    let handler = DispatchSource.makeSignalSource(signal:number, queue:signalQueue)
    handler.setEventHandler { release(); log("os-key-hold-signal-\(number)"); exit(128 + number) }
    handler.resume(); signalSources.append(handler)
}
guard focused() else { fail("The recorded playtest app must be frontmost") }
log("os-key-hold-start")
defer { release() }
for name in names {
    eventLock.lock()
    guard focused() else { eventLock.unlock(); release(); fail("Focus changed before keyDown") }
    let key = codes[name]!
    held.append(key)
    log("os-key-down",key:key)
    CGEvent(keyboardEventSource:source, virtualKey:key, keyDown:true)?.postToPid(pid_t(rawPid))
    eventLock.unlock()
}
let deadline = Date().addingTimeInterval(duration)
while Date() < deadline {
    guard focused() else { release(); fail("Focus changed during hold") }
    Thread.sleep(forTimeInterval:min(0.02, max(0,deadline.timeIntervalSinceNow)))
}
release()
log("os-key-hold-end")
