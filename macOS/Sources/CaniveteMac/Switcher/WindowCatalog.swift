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
        let runningApps = Dictionary(uniqueKeysWithValues: NSWorkspace.shared.runningApplications
            .filter { !$0.isTerminated && $0.activationPolicy == .regular }
            .compactMap { app -> (pid_t, NSRunningApplication)? in
                guard let bundleID = app.bundleIdentifier, !excludedBundleIDs.contains(bundleID) else { return nil }
                return (app.processIdentifier, app)
            })

        // AX subrole is the reliable distinction between real app windows and transient surfaces
        // (menus, popovers, tooltips, panels) that WindowServer also reports at layer zero.
        var axWindows: [pid_t: [CGWindowID: (title: String, minimized: Bool)]] = [:]
        for (pid, _) in runningApps {
            let application = AXUIElementCreateApplication(pid)
            AXUIElementSetMessagingTimeout(application, 0.25)
            var list: CFTypeRef?
            guard AXUIElementCopyAttributeValue(application, kAXWindowsAttribute as CFString, &list) == .success,
                  let elements = list as? [AXUIElement] else { continue }
            for element in elements {
                guard (attribute(element, kAXSubroleAttribute) as? String) == (kAXStandardWindowSubrole as String),
                      let number = attribute(element, "AXWindowNumber") as? NSNumber else { continue }
                let id = CGWindowID(number.uint32Value)
                let title = (attribute(element, kAXTitleAttribute) as? String ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
                let minimized = (attribute(element, kAXMinimizedAttribute) as? Bool) ?? false
                axWindows[pid, default: [:]][id] = (title, minimized)
            }
        }

        var seen = Set<CGWindowID>()
        var result: [SwitcherWindow] = []
        // CGWindowListCopyWindowInfo is front-to-back, which gives the switcher a useful recency order.
        for row in rows {
            guard let number = row[kCGWindowNumber as String] as? NSNumber,
                  let ownerPID = row[kCGWindowOwnerPID as String] as? NSNumber,
                  let layer = row[kCGWindowLayer as String] as? NSNumber, layer.intValue == 0 else { continue }
            let pid = ownerPID.int32Value
            let id = CGWindowID(number.uint32Value)
            guard seen.insert(id).inserted, let app = runningApps[pid],
                  let ax = axWindows[pid]?[id] else { continue }
            let rect = row[kCGWindowBounds as String] as? [String: CGFloat] ?? [:]
            let width = rect["Width"] ?? 0
            let height = rect["Height"] ?? 0
            let alpha = (row[kCGWindowAlpha as String] as? NSNumber)?.doubleValue ?? 1
            if !ax.minimized && (alpha <= 0 || width < 80 || height < 60) { continue }
            let rowTitle = (row[kCGWindowName as String] as? String ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
            let title = ax.title.isEmpty ? (rowTitle.isEmpty ? (app.localizedName ?? "Window") : rowTitle) : ax.title
            result.append(SwitcherWindow(id: id, pid: pid, title: title, appName: app.localizedName ?? app.bundleIdentifier ?? "App", appIcon: app.icon, image: nil, minimized: ax.minimized, bounds: CGRect(x: rect["X"] ?? 0, y: rect["Y"] ?? 0, width: width, height: height)))
        }

        // Some minimized windows have no drawable WindowServer row; keep only AX-confirmed standard windows.
        for pid in axWindows.keys.sorted() {
            guard let app = runningApps[pid] else { continue }
            for id in (axWindows[pid]?.keys.sorted(by: >) ?? []) {
                guard let ax = axWindows[pid]?[id], ax.minimized, !seen.contains(id) else { continue }
                result.append(SwitcherWindow(id: id, pid: pid, title: ax.title.isEmpty ? (app.localizedName ?? "Window") : ax.title, appName: app.localizedName ?? app.bundleIdentifier ?? "App", appIcon: app.icon, image: nil, minimized: true, bounds: .zero))
            }
        }
        return result
    }

    func captureImage(for id: CGWindowID) -> NSImage? {
        guard let cgImage = CGWindowListCreateImage(.null, .optionIncludingWindow, id, [.boundsIgnoreFraming, .nominalResolution]) else { return nil }
        return NSImage(cgImage: cgImage, size: NSSize(width: cgImage.width, height: cgImage.height))
    }

    private func attribute(_ element: AXUIElement, _ name: String) -> AnyObject? {
        var value: AnyObject?
        return AXUIElementCopyAttributeValue(element, name as CFString, &value) == .success ? value : nil
    }
}
