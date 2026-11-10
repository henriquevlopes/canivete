# Canivete for macOS

Native Swift app that begins the macOS port of Canivete with two independently switchable modules:

The app stays in the menu bar. Its menu provides tool toggles, a Settings entry for Accessibility help, a Start at Login toggle, and Quit. Start at Login is on by default at first launch and can be disabled at any time.

- **Alt+Tab window switcher:** hold Option and tap Tab to cycle windows; add Shift to cycle backwards; release Option to activate the selected window.
- **Dock Minimize:** click the Dock icon of the frontmost app to minimize its normal windows. The behavior is based on the author's existing Dock Minimize tool in `henriquevlopes/mac-tools`, adapted to run as an optional Canivete module.

## Build

Requires macOS 13 or newer and Apple's Command Line Tools with Swift. Run `./build.sh` to build the release binary and create `build/Canivete.app`. XCTest is not currently available in the Command Line Tools-only environment used for this initial build; the release build, app bundle, code signature, plist, and process launch are checked manually.

## Permissions

The switcher needs **Accessibility** permission to capture Option+Tab before macOS handles it, inspect hidden/minimized windows, and raise a selected window. Dock Minimize also needs **Accessibility** to recognize Dock icon clicks and minimize windows. Grant permission to Canivete in System Settings > Privacy & Security > Accessibility, then reopen the app. Use the Settings item in the menu bar to open Accessibility settings.

## Initial limitations

The initial window catalog uses public CoreGraphics APIs. It takes one snapshot when the switcher opens and sorts by descending WindowServer window ID, an approximation of recent window order. It filters nonstandard layers, transparent windows and very small surfaces; private macOS APIs for accurate Spaces enumeration/focus are not used. A window without an available capture still appears with its app title and icon. These choices keep capture and navigation direct, but exact AltTab parity across Spaces, Secure Input, Mission Control and every app requires additional work and manual validation.

See [AltTab behavior and research notes](docs/alt-tab-reference.md). No AltTab source code was copied; that project is GPL-3.0.
