import AppKit
import ApplicationServices
import ServiceManagement
import Carbon.HIToolbox

final class CaniveteMacApp: NSObject, NSApplicationDelegate {
    private let switcher = WindowSwitcher()
    private let dockMinimize = DockMinimizeModule()
    private var statusItem: NSStatusItem?
    private var settingsWindow: NSWindow?
    private weak var shortcutRecorder: NSButton?
    private var shortcutRecordingMonitor: Any?
    private let defaults = UserDefaults.standard
    private var accessibilityTimer: Timer?

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)
        if !defaults.bool(forKey: "macModulesInitialized") {
            defaults.set(true, forKey: "macModulesInitialized")
            defaults.set(true, forKey: "switcherEnabled")
            defaults.set(true, forKey: "dockMinimizeEnabled")
            defaults.set(true, forKey: "startAtLogin")
        }
        rebuildMenu()
        if defaults.bool(forKey: "switcherEnabled") { switcher.enabled = true; switcher.start() }
        dockMinimize.enabled = defaults.bool(forKey: "dockMinimizeEnabled")
        updateLoginItem()
        if CommandLine.arguments.contains("--settings") { DispatchQueue.main.async { self.openSettings() } }
    }

    private func requestAccessibilityIfNeeded() {
        let prompt = [kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String: true] as CFDictionary
        guard !AXIsProcessTrusted() else { return }
        AXIsProcessTrustedWithOptions(prompt)
        accessibilityTimer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] timer in
            guard AXIsProcessTrusted() else { return }
            timer.invalidate()
            self?.accessibilityTimer = nil
            self?.switcher.start()
            self?.dockMinimize.enabled = self?.defaults.bool(forKey: "dockMinimizeEnabled") ?? false
        }
    }

    private func rebuildMenu() {
        if statusItem == nil { statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength) }
        guard let item = statusItem else { return }
        item.button?.image = NSImage(systemSymbolName: "wrench.and.screwdriver", accessibilityDescription: "Canivete")
        let menu = NSMenu()
        menu.addItem(moduleItem("Window switcher (\(switcher.shortcutDisplayName()))", action: #selector(toggleSwitcher), enabled: defaults.bool(forKey: "switcherEnabled")))
        menu.addItem(moduleItem("Dock Minimize", action: #selector(toggleDockMinimize), enabled: defaults.bool(forKey: "dockMinimizeEnabled")))
        menu.addItem(.separator())
        let login = menu.addItem(withTitle: "Start at Login", action: #selector(toggleLogin), keyEquivalent: "")
        login.target = self
        login.state = defaults.bool(forKey: "startAtLogin") ? .on : .off
        menu.addItem(.separator())
        let quit = menu.addItem(withTitle: "Quit Canivete", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        quit.target = NSApp
        let settings = NSMenuItem(title: "Settings…", action: #selector(openSettings), keyEquivalent: ",")
        settings.target = self
        menu.insertItem(settings, at: menu.numberOfItems - 1)
        item.menu = menu
        statusItem = item
    }

    private func moduleItem(_ title: String, action: Selector, enabled: Bool) -> NSMenuItem {
        let item = NSMenuItem(title: title, action: action, keyEquivalent: "")
        item.target = self
        item.state = enabled ? .on : .off
        return item
    }

    @objc private func toggleSwitcher() {
        defaults.set(!switcher.enabled, forKey: "switcherEnabled")
        if switcher.enabled { switcher.stop() } else { switcher.enabled = true; switcher.start() }
        rebuildMenu()
    }

    @objc private func toggleDockMinimize() {
        defaults.set(!dockMinimize.enabled, forKey: "dockMinimizeEnabled")
        dockMinimize.enabled = defaults.bool(forKey: "dockMinimizeEnabled")
        rebuildMenu()
    }

    @objc private func toggleLogin() {
        let shouldStart = !defaults.bool(forKey: "startAtLogin")
        defaults.set(shouldStart, forKey: "startAtLogin")
        updateLoginItem()
        rebuildMenu()
    }

    @objc private func openSettings() {
        if settingsWindow == nil { settingsWindow = makeSettingsWindow() }
        NSApp.activate(ignoringOtherApps: true)
        settingsWindow?.makeKeyAndOrderFront(nil)
    }

    private func makeSettingsWindow() -> NSWindow {
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 580, height: 410), styleMask: [.titled, .closable], backing: .buffered, defer: false)
        window.title = "Canivete Settings"
        window.center()
        let content = NSView(frame: window.contentView?.bounds ?? .zero)
        window.contentView = content
        let heading = NSTextField(labelWithString: "Tools")
        heading.font = .systemFont(ofSize: 20, weight: .semibold)
        heading.frame = NSRect(x: 24, y: 356, width: 520, height: 28)
        content.addSubview(heading)
        addSetting("Alt+Tab window switcher", key: "switcherEnabled", y: 315, to: content)
        let recorder = NSButton(title: "Record Shortcut…", target: self, action: #selector(beginShortcutRecording(_:)))
        recorder.frame = NSRect(x: 24, y: 242, width: 300, height: 34)
        recorder.bezelStyle = .rounded
        shortcutRecorder = recorder
        content.addSubview(recorder)
        let activeShortcut = NSTextField(labelWithString: "Current: \(switcher.shortcutDisplayName())")
        activeShortcut.frame = NSRect(x: 24, y: 212, width: 400, height: 20)
        activeShortcut.tag = 301
        content.addSubview(activeShortcut)
        addSetting("Dock Minimize", key: "dockMinimizeEnabled", y: 174, to: content)
        addSetting("Start Canivete when macOS starts", key: "startAtLogin", y: 134, to: content)
        let help = NSTextField(wrappingLabelWithString: "Window switching and Dock Minimize need Accessibility permission.")
        help.frame = NSRect(x: 24, y: 80, width: 520, height: 36)
        content.addSubview(help)
        let requestAccess = NSButton(title: "Request Accessibility Permission", target: self, action: #selector(requestAccessibilityPermission))
        requestAccess.bezelStyle = .rounded
        requestAccess.frame = NSRect(x: 24, y: 30, width: 270, height: 30)
        content.addSubview(requestAccess)
        let access = NSButton(title: "Accessibility Settings…", target: self, action: #selector(openAccessibilitySettings))
        access.bezelStyle = .rounded
        access.frame = NSRect(x: 326, y: 30, width: 220, height: 30)
        content.addSubview(access)
        return window
    }

    @objc private func beginShortcutRecording(_ sender: NSButton) {
        if let shortcutRecordingMonitor { NSEvent.removeMonitor(shortcutRecordingMonitor) }
        switcher.suspendShortcutForRecording()
        sender.title = "Press the shortcut… (Esc cancels)"
        shortcutRecordingMonitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self, weak sender] event in
            guard let self, let sender else { return event }
            if event.keyCode == UInt16(kVK_Escape) {
                sender.title = "Record Shortcut…"
                if let monitor = self.shortcutRecordingMonitor { NSEvent.removeMonitor(monitor); self.shortcutRecordingMonitor = nil }
                self.switcher.updateShortcut()
                return nil
            }
            let modifiers: NSEvent.ModifierFlags = [.command, .option, .control, .shift]
            guard !event.modifierFlags.intersection(modifiers).isEmpty else {
                sender.title = "Add a modifier, then press a key…"
                return nil
            }
            self.saveShortcut(keyCode: event.keyCode, name: event.charactersIgnoringModifiers ?? "Key", flags: event.modifierFlags)
            sender.title = "Record Shortcut…"
            if let monitor = self.shortcutRecordingMonitor { NSEvent.removeMonitor(monitor); self.shortcutRecordingMonitor = nil }
            self.switcher.updateShortcut()
            return nil
        }
        settingsWindow?.makeFirstResponder(settingsWindow?.contentView)
    }

    private func saveShortcut(keyCode: UInt16, name: String, flags: NSEvent.ModifierFlags) {
        let map: [(NSEvent.ModifierFlags, CGEventFlags)] = [(.command, .maskCommand), (.option, .maskAlternate), (.control, .maskControl), (.shift, .maskShift)]
        var modifiers: CGEventFlags = []
        for (cocoa, cg) in map where flags.contains(cocoa) { modifiers.insert(cg) }
        defaults.set(UInt64(modifiers.rawValue), forKey: "switcherShortcutModifiers")
        defaults.set(0, forKey: "switcherShortcutPhysicalModifierCode")
        defaults.set(Int(keyCode), forKey: "switcherShortcutKeyCode")
        defaults.set(name, forKey: "switcherShortcutKeyName")
        settingsWindow?.contentView?.viewWithTag(301).flatMap { $0 as? NSTextField }?.stringValue = "Current: \(switcher.shortcutDisplayName())"
        switcher.updateShortcut()
        rebuildMenu()
    }

    private func addSetting(_ title: String, key: String, y: CGFloat, to view: NSView) {
        let button = NSButton(checkboxWithTitle: title, target: self, action: #selector(settingChanged(_:)))
        button.identifier = NSUserInterfaceItemIdentifier(key)
        button.state = defaults.bool(forKey: key) ? .on : .off
        button.frame = NSRect(x: 24, y: y, width: 380, height: 24)
        view.addSubview(button)
    }

    @objc private func settingChanged(_ sender: NSButton) {
        guard let key = sender.identifier?.rawValue else { return }
        let enabled = sender.state == .on
        defaults.set(enabled, forKey: key)
        switch key {
        case "switcherEnabled":
            switcher.enabled = enabled
            if enabled { switcher.start() }
        case "dockMinimizeEnabled": dockMinimize.enabled = enabled
        case "startAtLogin": updateLoginItem()
        default: break
        }
        rebuildMenu()
    }

    @objc private func openAccessibilitySettings() {
        guard let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility") else { return }
        NSWorkspace.shared.open(url)
    }

    @objc private func requestAccessibilityPermission() {
        guard !AXIsProcessTrusted() else {
            switcher.start()
            dockMinimize.enabled = defaults.bool(forKey: "dockMinimizeEnabled")
            return
        }
        requestAccessibilityIfNeeded()
    }

    private func updateLoginItem() {
        if defaults.bool(forKey: "startAtLogin") {
            if SMAppService.mainApp.status != .enabled { try? SMAppService.mainApp.register() }
        } else if SMAppService.mainApp.status == .enabled {
            try? SMAppService.mainApp.unregister()
        }
    }
}

let application = NSApplication.shared
let delegate = CaniveteMacApp()
application.delegate = delegate
application.run()
