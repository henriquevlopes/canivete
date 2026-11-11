import AppKit
import ApplicationServices
import Carbon.HIToolbox

final class WindowSwitcher: NSObject {
    private let catalog = WindowCatalog()
    private var windows: [SwitcherWindow] = []
    private var pendingImageIDs = Set<CGWindowID>()
    private var attemptedImageIDs = Set<CGWindowID>()
    private var selection = 0
    private var isLoadingWindows = false
    private var pendingCycles = 0
    private var pendingActivation = false
    private var panel: NSPanel?
    private var eventTap: CFMachPort?
    private var eventSource: CFRunLoopSource?
    private var hotKey: EventHotKeyRef?
    private var reverseHotKey: EventHotKeyRef?
    private var hotKeyHandler: EventHandlerRef?
    private let defaults = UserDefaults.standard
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
        if let hotKey { UnregisterEventHotKey(hotKey); self.hotKey = nil }
        if let reverseHotKey { UnregisterEventHotKey(reverseHotKey); self.reverseHotKey = nil }
        if let hotKeyHandler { RemoveEventHandler(hotKeyHandler); self.hotKeyHandler = nil }
        if let eventTap { CFMachPortInvalidate(eventTap); self.eventTap = nil }
        if let eventSource { CFRunLoopRemoveSource(CFRunLoopGetMain(), eventSource, .commonModes); self.eventSource = nil }
        dismiss()
    }

    private func installEventTap() {
        guard AXIsProcessTrusted() else { return }
        let mask = CGEventMask(1 << CGEventType.keyDown.rawValue) | CGEventMask(1 << CGEventType.flagsChanged.rawValue)
        let context = Unmanaged.passUnretained(self).toOpaque()
        eventTap = CGEvent.tapCreate(tap: .cgSessionEventTap, place: .headInsertEventTap, options: .listenOnly, eventsOfInterest: mask, callback: { _, type, event, refcon in
            let switcher = Unmanaged<WindowSwitcher>.fromOpaque(refcon!).takeUnretainedValue()
            switcher.observeTap(type, event)
            return Unmanaged.passUnretained(event)
        }, userInfo: context)
        guard let eventTap else { return }
        let source = CFMachPortCreateRunLoopSource(nil, eventTap, 0)
        eventSource = source
        CFRunLoopAddSource(CFRunLoopGetMain(), source, .commonModes)
        CGEvent.tapEnable(tap: eventTap, enable: true)
        registerShortcut()
    }

    private func observeTap(_ type: CGEventType, _ event: CGEvent) {
        if type == .tapDisabledByTimeout || type == .tapDisabledByUserInput {
            if let eventTap { CGEvent.tapEnable(tap: eventTap, enable: true) }
            return
        }
        guard enabled else { return }
        let key = Int(defaults.object(forKey: "switcherShortcutKeyCode") as? Int ?? 48)
        let holdFlags = configuredModifiers()
        if type == .keyDown,
           event.getIntegerValueField(.keyboardEventKeycode) == key,
           event.getIntegerValueField(.keyboardEventAutorepeat) != 0,
           event.flags.intersection(holdFlags) == holdFlags {
            DispatchQueue.main.async { [weak self] in self?.cycle(reverse: event.flags.contains(.maskShift)) }
        } else if type == .flagsChanged, panel != nil,
                  event.flags.intersection(holdFlags) != holdFlags {
            DispatchQueue.main.async { [weak self] in self?.activateSelection() }
        }
    }

    private func registerShortcut() {
        guard enabled, eventTap != nil else { return }
        if let hotKey { UnregisterEventHotKey(hotKey); self.hotKey = nil }
        if let reverseHotKey { UnregisterEventHotKey(reverseHotKey); self.reverseHotKey = nil }
        let code = UInt32(defaults.object(forKey: "switcherShortcutKeyCode") as? Int ?? 48)
        var modifiers: UInt32 = 0
        let flags = configuredModifiers()
        if flags.contains(.maskCommand) { modifiers |= UInt32(cmdKey) }
        if flags.contains(.maskAlternate) { modifiers |= UInt32(optionKey) }
        if flags.contains(.maskControl) { modifiers |= UInt32(controlKey) }
        if flags.contains(.maskShift) { modifiers |= UInt32(shiftKey) }
        if hotKeyHandler == nil {
            var types = [
                EventTypeSpec(eventClass: OSType(kEventClassKeyboard), eventKind: OSType(kEventHotKeyPressed)),
                EventTypeSpec(eventClass: OSType(kEventClassKeyboard), eventKind: OSType(kEventHotKeyReleased))
            ]
            let handlerStatus = InstallEventHandler(GetEventDispatcherTarget(), { _, event, userData in
                guard let event, let userData else { return noErr }
                let switcher = Unmanaged<WindowSwitcher>.fromOpaque(userData).takeUnretainedValue()
                let kind = GetEventKind(event)
                if kind == OSType(kEventHotKeyPressed) {
                    var identifier = EventHotKeyID()
                    GetEventParameter(event, EventParamName(kEventParamDirectObject), EventParamType(typeEventHotKeyID), nil, MemoryLayout<EventHotKeyID>.size, nil, &identifier)
                    let reverse = identifier.id == 2
                    DispatchQueue.main.async { switcher.cycle(reverse: reverse) }
                } else if kind == OSType(kEventHotKeyReleased) {
                    // The AltTab-style registered hotkey supplies reliable global key-down recognition.
                    // Selection is committed by the passive flagsChanged observer when the hold modifier lifts.
                }
                return noErr
            }, types.count, &types, Unmanaged.passUnretained(self).toOpaque(), &hotKeyHandler)
            guard handlerStatus == noErr else { hotKeyHandler = nil; return }
        }
        let identifier = EventHotKeyID(signature: OSType(0x43414E49), id: 1)
        let status = RegisterEventHotKey(code, modifiers, identifier, GetEventDispatcherTarget(), 0, &hotKey)
        guard status == noErr else { hotKey = nil; return }
        NSLog("Canivete hotkey register status=%d key=%u modifiers=%u", status, code, modifiers)
        if flags.contains(.maskShift) == false {
            let reverseIdentifier = EventHotKeyID(signature: OSType(0x43414E49), id: 2)
            RegisterEventHotKey(code, modifiers | UInt32(shiftKey), reverseIdentifier, GetEventDispatcherTarget(), 0, &reverseHotKey)
        }
    }

    func updateShortcut() { registerShortcut() }

    func suspendShortcutForRecording() {
        if let hotKey { UnregisterEventHotKey(hotKey); self.hotKey = nil }
        if let reverseHotKey { UnregisterEventHotKey(reverseHotKey); self.reverseHotKey = nil }
    }

    private func configuredModifiers() -> CGEventFlags {
        CGEventFlags(rawValue: (defaults.object(forKey: "switcherShortcutModifiers") as? UInt64) ?? CGEventFlags.maskAlternate.rawValue)
    }

    func shortcutDisplayName() -> String {
        let keyCode = UInt16(defaults.object(forKey: "switcherShortcutKeyCode") as? Int ?? 48)
        let physicalCode = defaults.integer(forKey: "switcherShortcutPhysicalModifierCode")
        let modifiers = CGEventFlags(rawValue: (defaults.object(forKey: "switcherShortcutModifiers") as? UInt64) ?? CGEventFlags.maskAlternate.rawValue)
        if physicalCode > 0 {
            let names: [Int: String] = [55: "Left Command", 54: "Right Command", 58: "Left Option", 61: "Right Option", 59: "Left Control", 62: "Right Control", 56: "Left Shift", 60: "Right Shift"]
            let keyName = Self.shortcutName(keyCode: keyCode, modifiers: [], characters: defaults.string(forKey: "switcherShortcutKeyName") ?? "")
            return "\(names[physicalCode] ?? "Modifier") + \(keyName)"
        }
        return Self.shortcutName(keyCode: keyCode, modifiers: modifiers, characters: defaults.string(forKey: "switcherShortcutKeyName") ?? "")
    }

    static func shortcutName(keyCode: UInt16, modifiers: CGEventFlags, characters: String) -> String {
        var name = ""
        if modifiers.contains(.maskControl) { name += "⌃" }
        if modifiers.contains(.maskAlternate) { name += "⌥" }
        if modifiers.contains(.maskShift) { name += "⇧" }
        if modifiers.contains(.maskCommand) { name += "⌘" }
        let special: [UInt16: String] = [36: "Return", 48: "Tab", 49: "Space", 51: "Delete", 53: "Escape", 115: "Home", 116: "Page Up", 117: "Forward Delete", 119: "End", 121: "Page Down", 123: "←", 124: "→", 125: "↓", 126: "↑"]
        name += special[keyCode] ?? characters.trimmingCharacters(in: .whitespacesAndNewlines).uppercased()
        return name
    }

    private func cycle(reverse: Bool) {
        if panel == nil {
            isLoadingWindows = true
            pendingCycles = 0
            pendingActivation = false
            pendingImageIDs.removeAll()
            attemptedImageIDs.removeAll()
            showPanel()
            DispatchQueue.global(qos: .userInteractive).async { [weak self] in
                guard let self else { return }
                var snapshot = self.catalog.snapshot()
                if snapshot.count > 1 {
                    let start = min(initialCardStart(selection: reverse ? snapshot.count - 1 : 1, total: snapshot.count), snapshot.count)
                    for index in start..<min(snapshot.count, start + 4) {
                        snapshot[index].image = self.catalog.captureImage(for: snapshot[index].id)
                    }
                } else if let first = snapshot.indices.first {
                    snapshot[first].image = self.catalog.captureImage(for: snapshot[first].id)
                }
                DispatchQueue.main.async { [weak self] in self?.finishLoading(snapshot, initialReverse: reverse) }
            }
        } else if isLoadingWindows {
            pendingCycles += reverse ? -1 : 1
        } else {
            selection = (selection + (reverse ? windows.count - 1 : 1)) % windows.count
            refreshPanel()
        }
    }

    private func finishLoading(_ snapshot: [SwitcherWindow], initialReverse: Bool) {
        guard panel != nil, isLoadingWindows else { return }
        windows = snapshot
        isLoadingWindows = false
        guard !windows.isEmpty else { dismiss(); return }
        selection = windows.count > 1 ? (initialReverse ? windows.count - 1 : 1) : 0
        if windows.count > 1, pendingCycles != 0 {
            selection = (selection + pendingCycles % windows.count + windows.count) % windows.count
        }
        pendingCycles = 0
        refreshPanel()
        if pendingActivation { pendingActivation = false; activateSelection() }
    }

    private func initialCardStart(selection: Int, total: Int) -> Int {
        let visible = min(4, total)
        return min(max(0, selection - visible / 2), max(0, total - visible))
    }

    private func showPanel() {
        let panel = NSPanel(contentRect: NSRect(x: 0, y: 0, width: 900, height: 226), styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        panel.level = .floating
        panel.isOpaque = false
        panel.backgroundColor = .clear
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
        let root = NSVisualEffectView(frame: NSRect(x: 0, y: 0, width: 900, height: 226))
        root.material = .hudWindow
        root.blendingMode = .behindWindow
        root.state = .active
        root.wantsLayer = true
        root.layer?.cornerRadius = 18
        root.layer?.borderWidth = 1
        root.layer?.borderColor = NSColor.white.withAlphaComponent(0.16).cgColor
        let status = NSTextField(labelWithString: "")
        status.frame = NSRect(x: 20, y: 12, width: 860, height: 18)
        status.font = .systemFont(ofSize: 11, weight: .medium)
        status.textColor = NSColor.secondaryLabelColor
        status.alignment = .center
        status.tag = 12
        root.addSubview(status)
        let strip = NSStackView(frame: NSRect(x: 16, y: 40, width: 868, height: 172))
        strip.orientation = .horizontal
        strip.alignment = .top
        strip.distribution = .fillEqually
        strip.spacing = 10
        strip.identifier = NSUserInterfaceItemIdentifier("windowCards")
        root.addSubview(strip, positioned: .below, relativeTo: nil)
        return root
    }

    private func refreshPanel() {
        guard let root = panel?.contentView else { return }
        refresh(root: root)
    }

    private func refresh(root: NSView) {
        guard let strip = root.subviews.first(where: { $0.identifier == NSUserInterfaceItemIdentifier("windowCards") }) as? NSStackView,
              let status = root.viewWithTag(12) as? NSTextField else { return }
        strip.arrangedSubviews.forEach { strip.removeArrangedSubview($0); $0.removeFromSuperview() }
        guard windows.indices.contains(selection) else {
            status.stringValue = isLoadingWindows ? "Loading open windows…" : "No open app windows"
            return
        }
        let visibleCount = min(4, windows.count)
        let start = min(max(0, selection - visibleCount / 2), max(0, windows.count - visibleCount))
        status.stringValue = "\(selection + 1) of \(windows.count) windows"
        for index in start..<(start + visibleCount) {
            if windows[index].image == nil, attemptedImageIDs.insert(windows[index].id).inserted {
                pendingImageIDs.insert(windows[index].id)
            }
            let window = windows[index]
            strip.addArrangedSubview(makeCard(for: window, selected: index == selection, width: 208, height: 164))
        }
        loadMissingPreviews()
        let width = CGFloat(visibleCount) * 208 + CGFloat(max(0, visibleCount - 1)) * 10 + 32
        let height: CGFloat = 226
        panel?.setContentSize(NSSize(width: width, height: height))
        root.frame = NSRect(x: 0, y: 0, width: width, height: height)
        status.frame = NSRect(x: 16, y: 10, width: width - 32, height: 18)
        strip.frame = NSRect(x: 16, y: 38, width: width - 32, height: 174)
        if let screen = NSScreen.main {
            panel?.setFrameOrigin(NSPoint(x: screen.visibleFrame.midX - width / 2, y: screen.visibleFrame.midY - height / 2))
        }
    }

    private func loadMissingPreviews() {
        let missing = windows.filter { $0.image == nil && pendingImageIDs.contains($0.id) }
        guard !missing.isEmpty else { return }
        let ids = missing.map(\.id)
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self else { return }
            let images = Dictionary(uniqueKeysWithValues: ids.compactMap { id in
                self.catalog.captureImage(for: id).map { (id, $0) }
            })
            DispatchQueue.main.async { [weak self] in
                guard let self else { return }
                self.pendingImageIDs.subtract(ids)
                for index in self.windows.indices {
                    if let image = images[self.windows[index].id] { self.windows[index].image = image }
                }
                self.refreshPanel()
            }
        }
    }

    private func makeCard(for window: SwitcherWindow, selected: Bool, width: CGFloat, height: CGFloat) -> NSView {
        let card = NSView(frame: NSRect(x: 0, y: 0, width: width, height: height))
        card.wantsLayer = true
        card.layer?.cornerRadius = 12
        card.layer?.borderWidth = selected ? 2.5 : 1
        card.layer?.borderColor = selected ? NSColor.controlAccentColor.cgColor : NSColor.white.withAlphaComponent(0.10).cgColor
        card.layer?.backgroundColor = NSColor.white.withAlphaComponent(selected ? 0.10 : 0.045).cgColor
        let icon = NSImageView(frame: NSRect(x: 10, y: height - 30, width: 18, height: 18))
        icon.image = window.appIcon
        icon.imageScaling = .scaleProportionallyUpOrDown
        card.addSubview(icon)
        let title = NSTextField(labelWithString: window.title)
        title.frame = NSRect(x: 34, y: height - 31, width: width - 44, height: 20)
        title.font = .systemFont(ofSize: 12, weight: selected ? .semibold : .medium)
        title.textColor = .labelColor
        title.lineBreakMode = .byTruncatingTail
        card.addSubview(title)
        let preview = NSImageView(frame: NSRect(x: 8, y: 28, width: width - 16, height: height - 66))
        preview.image = window.image
        preview.imageScaling = .scaleProportionallyUpOrDown
        preview.wantsLayer = true
        preview.layer?.cornerRadius = 7
        preview.layer?.masksToBounds = true
        card.addSubview(preview)
        let appName = NSTextField(labelWithString: window.appName)
        appName.frame = NSRect(x: 10, y: 7, width: width - 20, height: 16)
        appName.font = .systemFont(ofSize: 10, weight: .regular)
        appName.textColor = .secondaryLabelColor
        appName.lineBreakMode = .byTruncatingTail
        card.addSubview(appName)
        return card
    }

    private func activateSelection() {
        if isLoadingWindows { pendingActivation = true; return }
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
        isLoadingWindows = false
        pendingCycles = 0
        pendingActivation = false
    }
}
