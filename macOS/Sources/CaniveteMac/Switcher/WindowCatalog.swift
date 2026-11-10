import AppKit
import ApplicationServices
import CoreGraphics

struct SwitcherWindow: Identifiable, Equatable {
    let id: CGWindowID
    let pid: pid_t
    let title: String
    let appName: String
    let appIcon: NSImage?
    var image: NSImage?
    let minimized: Bool
    let bounds: CGRect

    static func == (lhs: SwitcherWindow, rhs: SwitcherWindow) -> Bool { lhs.id == rhs.id }
}

final class WindowCatalog {
    private let excludedBundleIDs: Set<String> = ["com.apple.dock", "com.apple.WindowManager", "com.apple.controlcenter", "com.apple.notificationcenterui"]

    func snapshot() -> [SwitcherWindow] {
        let options: CGWindowListOption = [.optionAll, .excludeDesktopElements]
        guard let rows = CGWindowListCopyWindowInfo(options, kCGNullWindowID) as? [[String: Any]] else { return [] }
        var seen = Set<CGWindowID>()
        var minimizedByProcess: [pid_t: [CGWindowID: Bool]] = [:]
        let windows = rows.compactMap { row -> SwitcherWindow? in
            guard let number = row[kCGWindowNumber as String] as? NSNumber,
                  let ownerPID = row[kCGWindowOwnerPID as String] as? NSNumber,
                  let layer = row[kCGWindowLayer as String] as? NSNumber, layer.intValue == 0,
                  let alpha = row[kCGWindowAlpha as String] as? NSNumber, alpha.doubleValue > 0,
                  let rect = row[kCGWindowBounds as String] as? [String: CGFloat],
                  let width = rect["Width"], let height = rect["Height"], width >= 32, height >= 24 else { return nil }
            let id = CGWindowID(number.uint32Value)
            guard seen.insert(id).inserted, let app = NSRunningApplication(processIdentifier: ownerPID.int32Value), !app.isTerminated,
                  let bundleID = app.bundleIdentifier, !excludedBundleIDs.contains(bundleID) else { return nil }
            let title = (row[kCGWindowName as String] as? String)?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
            let appName = app.localizedName ?? bundleID
            let pid = ownerPID.int32Value
            if minimizedByProcess[pid] == nil { minimizedByProcess[pid] = minimizedWindows(pid: pid) }
            let minimized = minimizedByProcess[pid]?[id] ?? false
            return SwitcherWindow(id: id, pid: pid, title: title.isEmpty ? appName : title, appName: appName, appIcon: app.icon, image: nil, minimized: minimized, bounds: CGRect(x: rect["X"] ?? 0, y: rect["Y"] ?? 0, width: width, height: height))
        }
        return windows.sorted { $0.id > $1.id }
    }

    func captureImage(for id: CGWindowID) -> NSImage? {
        guard let cgImage = CGWindowListCreateImage(.null, .optionIncludingWindow, id, [.boundsIgnoreFraming, .nominalResolution]) else { return nil }
        return NSImage(cgImage: cgImage, size: NSSize(width: cgImage.width, height: cgImage.height))
    }

    private func minimizedWindows(pid: pid_t) -> [CGWindowID: Bool] {
        let app = AXUIElementCreateApplication(pid)
        var list: CFTypeRef?
        guard AXUIElementCopyAttributeValue(app, kAXWindowsAttribute as CFString, &list) == .success,
              let elements = list as? [AXUIElement] else { return [:] }
        var result: [CGWindowID: Bool] = [:]
        for element in elements {
            var identifier: CFTypeRef?
            guard AXUIElementCopyAttributeValue(element, "AXWindowNumber" as CFString, &identifier) == .success,
                  let windowID = (identifier as? NSNumber)?.uint32Value else { continue }
            var minimized: CFTypeRef?
            let isMinimized = AXUIElementCopyAttributeValue(element, kAXMinimizedAttribute as CFString, &minimized) == .success && (minimized as? Bool ?? false)
            result[CGWindowID(windowID)] = isMinimized
        }
        return result
    }
}
