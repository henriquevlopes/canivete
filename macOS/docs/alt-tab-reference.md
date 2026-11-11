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

Canivete follows the same interaction model: a configurable global shortcut, ordered window previews, forward/reverse cycling while held, and selection on modifier release. The catalog admits AX standard windows from running GUI apps, uses WindowServer front-to-back order, and retains minimized windows confirmed by Accessibility; utility surfaces are excluded. The switcher presents four thumbnail cards at a time. Private Spaces APIs and third-party source code are intentionally excluded, so Space-specific focus and some edge cases still differ from upstream.

Reference: upstream `docs/contributing.md`, `src/windowserver/`, `src/events/`, and `src/switcher/` in the repository above; inspected 2026-11-10.
