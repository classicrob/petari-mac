// Lists the on-screen windows of one process and every window above them, for
// diagnosing clicks that miss the game window (for example after leaving
// fullscreen). Read only: it sends no input and changes nothing.
//
// Usage: list-app-windows <pid>
// Build: swiftc -O native/tools/list_app_windows.swift -o build/list-app-windows
//
// Output, one window per line, front to back:
//   <mark> id=<n> pid=<n> owner=<name> layer=<n> alpha=<a> bounds=(x,y w×h) name=<title>
// mark: "*" for the target process's windows, " " for windows in front of the
// first target window (which could take its clicks). Screens follow.

import AppKit
import CoreGraphics

let args = CommandLine.arguments
guard args.count == 2, let pid = Int32(args[1]) else {
    FileHandle.standardError.write("usage: list-app-windows <pid>\n".data(using: .utf8)!)
    exit(2)
}

let options: CGWindowListOption = [.optionOnScreenOnly, .excludeDesktopElements]
guard let list = CGWindowListCopyWindowInfo(options, kCGNullWindowID) as? [[String: Any]] else {
    print("no window list")
    exit(1)
}

var sawTarget = false
for window in list {  // front to back
    let owner = window[kCGWindowOwnerPID as String] as? Int32 ?? -1
    let isTarget = owner == pid
    if !isTarget && sawTarget {
        continue  // behind the game: cannot take its clicks
    }
    sawTarget = sawTarget || isTarget
    let id = window[kCGWindowNumber as String] as? Int ?? -1
    let layer = window[kCGWindowLayer as String] as? Int ?? 0
    let alpha = window[kCGWindowAlpha as String] as? Double ?? 1.0
    let name = window[kCGWindowName as String] as? String ?? ""
    let ownerName = window[kCGWindowOwnerName as String] as? String ?? ""
    var bounds = CGRect.zero
    if let dict = window[kCGWindowBounds as String] as? NSDictionary {
        bounds = CGRect(dictionaryRepresentation: dict) ?? .zero
    }
    print(String(format: "%@ id=%d pid=%d owner=%@ layer=%d alpha=%.2f bounds=(%.0f,%.0f %.0f×%.0f) name=%@",
                 isTarget ? "*" : " ", id, owner, ownerName, layer, alpha, bounds.origin.x, bounds.origin.y,
                 bounds.size.width, bounds.size.height, name))
}
if !sawTarget {
    print("no on-screen window for pid \(pid)")
}
for (i, screen) in NSScreen.screens.enumerated() {
    let f = screen.frame
    print(String(format: "screen %d frame=(%.0f,%.0f %.0f×%.0f) scale=%.1f", i, f.origin.x, f.origin.y, f.size.width,
                 f.size.height, screen.backingScaleFactor))
}
