import AppKit
import ApplicationServices
import ServiceManagement

final class CaniveteMacApp: NSObject, NSApplicationDelegate {
    private let switcher = WindowSwitcher()
    private let dockMinimize = DockMinimizeModule()
    private var statusItem: NSStatusItem?
    private var settingsWindow: NSWindow?
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
        requestAccessibilityIfNeeded()
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
        menu.addItem(moduleItem("Alt+Tab window switcher", action: #selector(toggleSwitcher), enabled: defaults.bool(forKey: "switcherEnabled")))
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
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 440, height: 290), styleMask: [.titled, .closable], backing: .buffered, defer: false)
        window.title = "Canivete Settings"
        window.center()
        let content = NSView(frame: window.contentView?.bounds ?? .zero)
        window.contentView = content
        let heading = NSTextField(labelWithString: "Tools")
        heading.font = .systemFont(ofSize: 20, weight: .semibold)
        heading.frame = NSRect(x: 24, y: 236, width: 380, height: 28)
        content.addSubview(heading)
        addSetting("Alt+Tab window switcher", key: "switcherEnabled", y: 190, to: content)
        addSetting("Dock Minimize", key: "dockMinimizeEnabled", y: 150, to: content)
        addSetting("Start Canivete when macOS starts", key: "startAtLogin", y: 110, to: content)
        let help = NSTextField(wrappingLabelWithString: "Window switching and Dock Minimize need Accessibility permission.")
        help.frame = NSRect(x: 24, y: 58, width: 270, height: 38)
        content.addSubview(help)
        let access = NSButton(title: "Accessibility Settings…", target: self, action: #selector(openAccessibilitySettings))
        access.bezelStyle = .rounded
        access.frame = NSRect(x: 225, y: 22, width: 190, height: 30)
        content.addSubview(access)
        return window
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
