import Cocoa
import ApplicationServices

final class DockMinimizeModule {
    private var tap: CFMachPort?
    private var source: CFRunLoopSource?
    private var swallowedMouseUp = false
    private var isEnabled = false
    var enabled: Bool {
        get { isEnabled }
        set { isEnabled = newValue; newValue ? start() : stopTap() }
    }

    private func start() {
        guard isEnabled, tap == nil, AXIsProcessTrusted() else { return }
        let mask = CGEventMask((1 << CGEventType.leftMouseDown.rawValue) | (1 << CGEventType.leftMouseUp.rawValue))
        let context = Unmanaged.passUnretained(self).toOpaque()
        tap = CGEvent.tapCreate(tap: .cgSessionEventTap, place: .headInsertEventTap, options: .defaultTap, eventsOfInterest: mask, callback: { _, type, event, refcon in
            Unmanaged<DockMinimizeModule>.fromOpaque(refcon!).takeUnretainedValue().handle(type, event)
        }, userInfo: context)
        guard let tap else { NSLog("Canivete Dock Minimize: could not create event tap"); return }
        let source = CFMachPortCreateRunLoopSource(nil, tap, 0)
        self.source = source
        CFRunLoopAddSource(CFRunLoopGetMain(), source, .commonModes)
        CGEvent.tapEnable(tap: tap, enable: true)
    }

    private func stopTap() {
        guard let tap else { return }
        CFMachPortInvalidate(tap)
        self.tap = nil
        if let source { CFRunLoopRemoveSource(CFRunLoopGetMain(), source, .commonModes); self.source = nil }
    }

    private func handle(_ type: CGEventType, _ event: CGEvent) -> Unmanaged<CGEvent>? {
        guard enabled else { return Unmanaged.passUnretained(event) }
        if type == .tapDisabledByTimeout || type == .tapDisabledByUserInput {
            if let tap { CGEvent.tapEnable(tap: tap, enable: true) }
            return Unmanaged.passUnretained(event)
        }
        if type == .leftMouseUp {
            if swallowedMouseUp { swallowedMouseUp = false; return nil }
            return Unmanaged.passUnretained(event)
        }
        let modifiers: CGEventFlags = [.maskCommand, .maskAlternate, .maskControl, .maskShift]
        if event.flags.intersection(modifiers).isEmpty && minimizeFrontmostAppIfDockIcon(at: event.location) {
            swallowedMouseUp = true
            return nil
        }
        return Unmanaged.passUnretained(event)
    }

    private func minimizeFrontmostAppIfDockIcon(at point: CGPoint) -> Bool {
        guard let frontmost = NSWorkspace.shared.frontmostApplication, let appURL = frontmost.bundleURL else { return false }
        var hit: AXUIElement?
        guard AXUIElementCopyElementAtPosition(AXUIElementCreateSystemWide(), Float(point.x), Float(point.y), &hit) == .success, let element = hit else { return false }
        var pid: pid_t = 0
        AXUIElementGetPid(element, &pid)
        guard NSRunningApplication(processIdentifier: pid)?.bundleIdentifier == "com.apple.dock" else { return false }
        guard (attribute(element, kAXSubroleAttribute) as? String) == (kAXApplicationDockItemSubrole as String),
              let url = attribute(element, kAXURLAttribute) as? URL,
              url.standardizedFileURL.resolvingSymlinksInPath() == appURL.standardizedFileURL.resolvingSymlinksInPath() else { return false }
        let app = AXUIElementCreateApplication(frontmost.processIdentifier)
        guard let windows = attribute(app, kAXWindowsAttribute) as? [AXUIElement] else { return false }
        var minimized = false
        for window in windows where (attribute(window, kAXSubroleAttribute) as? String) == (kAXStandardWindowSubrole as String) && (attribute(window, kAXMinimizedAttribute) as? Bool) == false {
            if AXUIElementSetAttributeValue(window, kAXMinimizedAttribute as CFString, kCFBooleanTrue) == .success { minimized = true }
        }
        return minimized
    }

    private func attribute(_ element: AXUIElement, _ name: String) -> AnyObject? {
        var value: AnyObject?
        return AXUIElementCopyAttributeValue(element, name as CFString, &value) == .success ? value : nil
    }
}
