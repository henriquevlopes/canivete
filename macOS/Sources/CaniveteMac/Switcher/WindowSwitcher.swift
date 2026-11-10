import AppKit
import ApplicationServices

final class WindowSwitcher: NSObject {
    private let catalog = WindowCatalog()
    private var windows: [SwitcherWindow] = []
    private var selection = 0
    private var panel: NSPanel?
    private var eventTap: CFMachPort?
    private var eventSource: CFRunLoopSource?
    private var swallowedKeyUp = false
    private var swallowedModifierRelease = false
    private var isEnabled = false
    var enabled: Bool {
        get { isEnabled }
        set { isEnabled = newValue; newValue ? start() : stopListeners() }
    }

    func start() {
        guard enabled, eventTap == nil else { return }
        installEventTap()
    }

    func stop() { enabled = false }

    private func stopListeners() {
        if let eventTap { CFMachPortInvalidate(eventTap); self.eventTap = nil }
        if let eventSource { CFRunLoopRemoveSource(CFRunLoopGetMain(), eventSource, .commonModes); self.eventSource = nil }
        dismiss()
    }

    private func installEventTap() {
        guard AXIsProcessTrusted() else { return }
        let mask = CGEventMask(1 << CGEventType.keyDown.rawValue) | CGEventMask(1 << CGEventType.keyUp.rawValue) | CGEventMask(1 << CGEventType.flagsChanged.rawValue)
        let context = Unmanaged.passUnretained(self).toOpaque()
        eventTap = CGEvent.tapCreate(tap: .cgSessionEventTap, place: .headInsertEventTap, options: .defaultTap, eventsOfInterest: mask, callback: { _, type, event, refcon in
            let switcher = Unmanaged<WindowSwitcher>.fromOpaque(refcon!).takeUnretainedValue()
            return switcher.handleTap(type, event)
        }, userInfo: context)
        guard let eventTap else { return }
        let source = CFMachPortCreateRunLoopSource(nil, eventTap, 0)
        eventSource = source
        CFRunLoopAddSource(CFRunLoopGetMain(), source, .commonModes)
        CGEvent.tapEnable(tap: eventTap, enable: true)
    }

    private func handleTap(_ type: CGEventType, _ event: CGEvent) -> Unmanaged<CGEvent>? {
        if type == .tapDisabledByTimeout || type == .tapDisabledByUserInput {
            if let eventTap { CGEvent.tapEnable(tap: eventTap, enable: true) }
            return Unmanaged.passUnretained(event)
        }
        guard enabled else { return Unmanaged.passUnretained(event) }
        if type == .flagsChanged, swallowedModifierRelease, !event.flags.contains(.maskAlternate) {
            swallowedModifierRelease = false
            DispatchQueue.main.async { [weak self] in self?.activateSelection() }
            return nil
        }
        if type == .keyUp, swallowedKeyUp { swallowedKeyUp = false; return nil }
        guard type == .keyDown, event.flags.contains(.maskAlternate), event.getIntegerValueField(.keyboardEventKeycode) == 48 else { return Unmanaged.passUnretained(event) }
        swallowedKeyUp = true
        swallowedModifierRelease = true
        DispatchQueue.main.async { [weak self] in self?.cycle(reverse: event.flags.contains(.maskShift)) }
        return nil
    }

    private func cycle(reverse: Bool) {
        if panel == nil {
            windows = catalog.snapshot()
            guard !windows.isEmpty else { return }
            selection = windows.count > 1 ? 1 : 0
            showPanel()
        } else {
            selection = (selection + (reverse ? windows.count - 1 : 1)) % windows.count
            refreshPanel()
        }
    }

    private func showPanel() {
        let panel = NSPanel(contentRect: NSRect(x: 0, y: 0, width: 620, height: 300), styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        panel.level = .screenSaver
        panel.isOpaque = false
        panel.backgroundColor = NSColor.windowBackgroundColor.withAlphaComponent(0.92)
        panel.hasShadow = true
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .ignoresCycle]
        if let screen = NSScreen.main {
            panel.setFrameOrigin(NSPoint(x: screen.visibleFrame.midX - panel.frame.width / 2, y: screen.visibleFrame.midY - panel.frame.height / 2))
        } else { panel.center() }
        panel.contentView = makeContent()
        panel.orderFrontRegardless()
        self.panel = panel
        refreshPanel()
    }

    private func makeContent() -> NSView {
        let root = NSView(frame: NSRect(x: 0, y: 0, width: 620, height: 300))
        let image = NSImageView(frame: NSRect(x: 20, y: 64, width: 580, height: 205))
        image.imageScaling = .scaleProportionallyUpOrDown
        image.tag = 11
        root.addSubview(image)
        let title = NSTextField(labelWithString: "")
        title.frame = NSRect(x: 20, y: 35, width: 580, height: 22)
        title.alignment = .center
        title.font = .systemFont(ofSize: 14, weight: .semibold)
        title.tag = 12
        root.addSubview(title)
        let strip = NSStackView(frame: NSRect(x: 16, y: 5, width: 588, height: 26))
        strip.orientation = .horizontal
        strip.alignment = .centerY
        strip.distribution = .fillEqually
        root.addSubview(strip, positioned: .below, relativeTo: nil)
        return root
    }

    private func refreshPanel() {
        guard let root = panel?.contentView else { return }
        refresh(root: root)
    }

    private func refresh(root: NSView) {
        guard windows.indices.contains(selection) else { return }
        if windows[selection].image == nil { windows[selection].image = catalog.captureImage(for: windows[selection].id) }
        (root.viewWithTag(11) as? NSImageView)?.image = windows[selection].image
        (root.viewWithTag(12) as? NSTextField)?.stringValue = "\(windows[selection].appName) — \(windows[selection].title)"
        guard let strip = root.subviews.compactMap({ $0 as? NSStackView }).first else { return }
        strip.arrangedSubviews.forEach { strip.removeArrangedSubview($0); $0.removeFromSuperview() }
        for (index, window) in windows.prefix(12).enumerated() {
            let icon = NSImageView(image: window.appIcon ?? NSImage(size: NSSize(width: 18, height: 18)))
            icon.frame = NSRect(x: 0, y: 0, width: 20, height: 20)
            icon.imageScaling = .scaleProportionallyUpOrDown
            icon.contentTintColor = index == selection ? .controlAccentColor : nil
            strip.addArrangedSubview(icon)
        }
    }

    private func activateSelection() {
        guard windows.indices.contains(selection) else { dismiss(); return }
        let chosen = windows[selection]
        dismiss()
        guard let app = NSRunningApplication(processIdentifier: chosen.pid) else { return }
        var list: CFTypeRef?
        if AXUIElementCopyAttributeValue(AXUIElementCreateApplication(chosen.pid), kAXWindowsAttribute as CFString, &list) == .success,
           let elements = list as? [AXUIElement] {
            for element in elements {
                var number: CFTypeRef?
                if AXUIElementCopyAttributeValue(element, "AXWindowNumber" as CFString, &number) == .success,
                   (number as? NSNumber)?.uint32Value == chosen.id {
                    AXUIElementSetAttributeValue(element, kAXMinimizedAttribute as CFString, kCFBooleanFalse)
                    AXUIElementPerformAction(element, kAXRaiseAction as CFString)
                    break
                }
            }
        }
        app.activate(options: [.activateAllWindows, .activateIgnoringOtherApps])
    }

    private func dismiss() {
        panel?.orderOut(nil)
        panel = nil
        swallowedKeyUp = false
        swallowedModifierRelease = false
    }
}
