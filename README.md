# Canivete

Henri's personal Windows tray toolbox (Portuguese *canivete* = pocket knife). One small native exe,
no runtimes, nothing to install. Personal use only, not for sale or distribution.

Plan and research notes: [PLAN.md](PLAN.md).

## Status

| Phase | Tool | State |
|---|---|---|
| 0 | Tray app, hotkeys, settings | done |
| 1 | Screenshots (region / window / full) | done |
| 2 | Color picker | done |
| 2b | Screenshot editor (shapes, text, numbers) | done |
| 3 | Image resizer | – |
| 4 | Renamer | – |
| 5 | MP4 recording | – |
| 6 | GIF recording | – |

## Use

| Hotkey | Action |
|---|---|
| Ctrl+PrtSc | Region: drag a box, or click a highlighted window / monitor. Esc cancels, arrows nudge 1 px (Shift = 10), right-click drops a drag |
| Alt+PrtSc | The window under the mouse (the whole monitor over the desktop or taskbar) |
| PrtSc | All monitors (or the one under the mouse, see settings) |
| Win+C | Color picker: the screen freezes, a magnifier follows the mouse. Click, Enter or Space copies the color; Esc or right-click cancels; arrows nudge |

Screenshots are saved as PNG (or JPG) to the Windows Screenshots folder and copied to the clipboard
(as image and PNG). A small preview pops up bottom-right: click opens the editor, right-click shows
the file in Explorer. Settings can open the editor right after every capture instead. Tray icon right-click: capture menu, Pick a color, Recent colors (last 10; click one to
copy it again) and Settings.

After a pick, a small menu lists the color in every format: Gray 0-1 (brightness like Substance's
linear grayscale value), HEX, RGB and HSL. The default format from Settings
sits on top, so Enter takes it; Esc copies nothing. Windows reserves Win+C,
so Win-key hotkeys it refuses are caught with a keyboard hook instead (the Start menu stays closed).
Recent colors live in `%APPDATA%\Canivete\colors.txt`.

Settings: tray > Settings… opens a window for hotkeys (click a box, press the keys), folder, file
name (with an Insert menu of tokens and a live example), PNG/JPG and quality, full-screen mode,
clipboard, preview, mouse pointer, magnifier, color copy format and start with Windows. Everything is stored in
`%APPDATA%\Canivete\settings.ini`; edits to that file apply as soon as it is saved.
Log: `%APPDATA%\Canivete\canivete.log`.

Editor: tools Rectangle (R), Ellipse (E), Arrow (A), Line (L), Text (T), Number (N, counts up),
Highlighter (H) and Pixelate (P); colors 1-8; size with `[` `]` or the mouse wheel; Shift draws
squares, circles and 45° lines. Ctrl+wheel zooms at the mouse (Ctrl+0 fits, Ctrl+1 = 100 %, Ctrl +/-);
hold Space and drag (or drag with the middle button) to move around. Ctrl+Z / Ctrl+Y undo/redo, Ctrl+C copies, Ctrl+S saves (over the
capture's file). Enter or Done copies, saves and closes; Esc cancels a shape or closes (asks if there
are unsaved edits). Text: click, type, Enter finishes, Shift+Enter new line, Ctrl+V pastes.

Command line: `Canivete.exe --region | --window | --full | --color | --settings` acts in the running copy.
`Canivete.exe --edit image.png` opens any image in the editor.
`Canivete.exe --grab out.png` (or `.jpg`) grabs all monitors to a file and exits (self-test).

## Build and install

Visual Studio 2026 (C++ desktop workload) and CMake.

```powershell
powershell -ExecutionPolicy Bypass -File tools\deploy.ps1
```

Builds Release, installs to `%LOCALAPPDATA%\Programs\Canivete\Canivete.exe` and restarts it.

## Code

- `src/main.cpp` — tray icon, hotkeys (plus a keyboard hook for reserved Win combos), menu, settings-file watch, single instance
- `src/settings.cpp` — INI settings, hotkey parsing, log
- `src/settings_ui.cpp` — Settings window (built in code, per-monitor DPI, hotkey recording hook)
- `src/capture.cpp` — GDI screen grab, region overlay (frozen screen, window snapping, magnifier), color picker mode
- `src/color.cpp` — color text formats, clipboard text, recent colors
- `src/output.cpp` — PNG/JPG via WIC, file naming, clipboard, notification toast
- `src/editor.cpp` — screenshot editor window (GDI+ shapes and text, undo, zoom-to-fit)
