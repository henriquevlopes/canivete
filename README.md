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
| 3 | Image resizer | done |
| 4 | Renamer | done |
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
Shapes stay editable: a new shape is selected right after it's drawn, and the Select tool (V) picks
any shape. A selected shape can be dragged (Shift = straight), resized by its handles (text and
markers stretch from a corner), nudged with the arrow keys (Shift = 10 px), recolored or resized
with the color and size buttons / keys / wheel, and removed with Delete; Esc unselects. Clicking a
text with the Text tool, or double-clicking it with any tool, retypes it. Every change is one undo step.

Image right-click menu: select images in Explorer, right-click > Show more options > Canivete:
- Resize > Small / Medium / Large / Half size: new files next to the originals (`photo (Medium).jpg`),
  no window; Choose a size… opens the resizer below.
- Convert to > JPG / PNG / TIFF / BMP: new files next to the originals (`photo.png` > `photo.jpg`);
  see-through parts become white in JPG; images already in that format are skipped.
- Rotate > Turn right / left / upside down, Flip left to right / top to bottom: saves over the
  original (same name and dates; JPG at quality 95 or more, photo info kept). JPG, PNG, BMP, TIFF only.
- Copy to clipboard: the (first) image, upright, ready to paste.
A tray notice says what was done; problems show in a message.

Image resizer: select images in Explorer, right-click > Show more options > Canivete > Resize > Choose a size…
(or Send to > Canivete - Resize images, which also takes folders), or tray > Resize images…, or
drop files on its window. Pick Small 854×480, Medium 1366×768, Large 1920×1080 or a custom size
(pixels, or percent); Fit inside / Fill and crop / Stretch; "only make smaller" and "turn the size
sideways for tall images" are on by default. Save as new files next to the originals
(`photo (Medium).jpg`), into a folder, or replace the originals (old ones go to the Recycle Bin;
images already small enough are left alone). Format: same as the original (WebP/HEIC/GIF become
JPG, or PNG when see-through), JPG (quality 90) or PNG. Photo info (camera, date, GPS, color
profile) is kept when the format stays the same; phone photos are always turned upright. Uses
every CPU core: 40 photos of 24 MP take about 2 s. Choices are remembered in `[Resizer]` in
settings.ini. With no errors the window closes and a tray notice says how many were done.

Renamer: select files or folders in Explorer, right-click > Show more options > Rename with
Canivete (or Send to > Canivete - Rename), or tray > Rename files…, or drop them on its window.
Type what to search for and what to put instead; the list shows every new name as you type, and
problems (a name used twice, a name that already exists, characters Windows forbids) in red.
Options: regular expression (`$1` puts back a captured group), every match or only the first,
match case, change the name, the extension or both, text case (lowercase, UPPERCASE, Title Case,
Sentence case), and which items count (files, folders, everything inside the folders). Empty search
replaces the whole name. Tokens in the new text (the Insert button lists them): `${}` a counter,
`${padding=3}` 001, `${start=10,increment=10}`, and `$YYYY $MM $DD $hh $mm $ss` from the file's
modified date; only matched items use up a counter number. The Numbers button numbers everything
in one click: in front (`01 Holiday.jpg`), at the end (`Holiday 01.jpg`), a new name with a number
(`Photo 01.jpg`, with "Photo" selected so you can type over it) or numbers only; it pads to the
digits needed (01 for up to 99 items, 001 above). Delete removes a row from the list.
All renames are one Windows operation, so Ctrl+Z in Explorer undoes the whole batch.

Command line: `Canivete.exe --region | --window | --full | --color | --settings` acts in the running copy.
`Canivete.exe --edit image.png` opens any image in the editor.
`Canivete.exe --resize a.jpg b.png folder` opens the resizer with those files (each copy hands its
files to the running one, so Explorer's one-copy-per-file launch ends in one window).
`Canivete.exe --rename a.txt folder` opens the renamer the same way.
`Canivete.exe --do <action> a.jpg b.png` runs a right-click menu action (`small medium large half custom
jpg png tif bmp right left 180 fliph flipv copy`).
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
- `src/editor.cpp` — screenshot editor window (GDI+ shapes and text, undo, zoom and pan)
- `src/resizer.cpp` — image resizer window, WIC resize engine on worker threads, right-click quick actions (convert, rotate, copy)
- `src/renamer.cpp` — renamer window, live preview, regex/counter/date tokens, IFileOperation rename with undo
- `src/shell.cpp` — Explorer right-click entries (the Canivete image submenu, Rename) and Send to shortcuts
