# Canivete for macOS

Native Swift app that begins the macOS port of Canivete with two independently switchable modules:

The app stays in the menu bar. Its menu provides tool toggles, a Settings entry for Accessibility help and shortcut configuration, a Start at Login toggle, and Quit. Start at Login is on by default at first launch and can be disabled at any time. In Settings, use **Record Shortcut…** and press the desired modifier-plus-key combination.

- **Window switcher:** hold the configurable shortcut to cycle windows; add Shift to cycle backwards; release its modifiers to activate the selected window. The centered switcher shows up to four titled window previews at once with a blue selection outline.
- **Dock Minimize:** click the Dock icon of the frontmost app to minimize its normal windows. The behavior is based on the author's existing Dock Minimize tool in `henriquevlopes/mac-tools`, adapted to run as an optional Canivete module.

## Build

Requires macOS 13 or newer and Apple's Command Line Tools with Swift. Run `./build.sh` to build the release binary and create `build/Canivete.app`. The ad-hoc signature uses a stable designated requirement based on the bundle identifier, so the app remains identifiable across local rebuilds. XCTest is not currently available in the Command Line Tools-only environment; the release build, app bundle, code signature, plist, and process launch are checked manually.

## Permissions

The switcher and Dock Minimize need **Accessibility** permission. Canivete asks only when **Request Accessibility Permission** is pressed in Settings; once granted, enabled modules start automatically. The **Accessibility Settings…** button opens System Settings if the permission needs to be toggled manually.

## Initial limitations

The window catalog uses public CoreGraphics enumeration and Accessibility window roles. It includes standard windows from running GUI apps, retains minimized windows where Accessibility exposes them, and uses WindowServer front-to-back order for navigation; transient panels, menus, popovers, and non-GUI helpers are filtered out. Private Spaces APIs are excluded, so Space-specific focus and some edge cases still differ from upstream.

See [AltTab behavior and research notes](docs/alt-tab-reference.md). No AltTab source code was copied; that project is GPL-3.0.
