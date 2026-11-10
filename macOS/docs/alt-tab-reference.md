# AltTab behavior used for Canivete macOS

This implementation is original code. The upstream [AltTab for macOS](https://github.com/lwouis/alt-tab-macos) is GPL-3.0; no source files or code snippets were copied into Canivete. This document records public behavior and engineering findings used to define scope.

## Upstream behavior studied

- Enumerates application windows rather than applications; includes minimized windows, windows belonging to hidden apps, fullscreen windows, and windows across displays and Spaces.
- Offers thumbnails and app icons, titles, navigation while the trigger is held, selection on modifier release, and mouse selection.
- Provides configurable shortcut sets, ordering and appearance options, filtering/exclusions, and accessibility onboarding.
- Tracks application/window lifecycle and display/Space changes so the switcher stays current.
- Handles Mission Control, fullscreen transitions, keyboard repeat timing, Secure Input, multiple keyboard layouts, and Reduce Transparency.
- Documents that Spaces enumeration/focus and some low-level events require undocumented private APIs, which can break across macOS releases and prevent Mac App Store distribution.

## Canivete implementation boundaries

The first implementation uses public CoreGraphics window enumeration and capture, Accessibility for restoring and raising windows, and an Option+Tab hold gesture. A snapshot is taken when opening the switcher to avoid repeated global enumeration on every key press. Private Spaces APIs and third-party code are intentionally excluded. This keeps the initial build independent and maintainable but means Space-specific filtering/focus and some edge cases do not yet match upstream.

Reference: upstream `docs/contributing.md`, `src/windowserver/`, `src/events/`, and `src/switcher/` in the repository above; inspected 2026-11-10.
