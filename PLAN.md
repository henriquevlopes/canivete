# Canivete — build plan

*Canivete* (Portuguese: pocket knife). Personal tool, not for sale.

A single small Windows tray app with only five tools: screenshots, screen recording (MP4 + GIF),
color picker, file/folder renamer, image resizer. Built after studying how PowerToys, ShareX and
OBS do the same jobs (their source code, 2026-11-10).

## Goals

| | PowerToys | ShareX | Canivete target |
|---|---|---|---|
| Install size | ~1 GB | ~520 MB | **< 3 MB** (one .exe) |
| RAM while idle | 20–45 MB per running module | ~150 MB | **< 10 MB** |
| Hotkey → screen ready | – | fast (pre-warmed) | **< 100 ms** |
| CPU while recording | – | low (GPU) | **< 5 %** (GPU encoder) |

Why the others are heavy: PowerToys ships ~400 MB of WinUI/.NET runtimes and keeps a process per
tool; ShareX ships a 193 MB ffmpeg.exe, a full .NET runtime, an AI OCR engine, uploaders and Perl.
Canivete uses only what Windows already has built in.

## Technology

- **C++20, plain Win32**, built with Visual Studio 2026 (already installed) + CMake. Static runtime,
  one `Canivete.exe`, no installer needed (copy the exe, tick "start with Windows").
- Everything runs in **one process**. Tool windows are created when used and freed afterwards.
- Windows built-ins only:
  - **GDI / Direct2D**: overlay drawing, magnifier
  - **WIC**: PNG/JPEG read/write, resizing, photo metadata
  - **Windows.Graphics.Capture (WGC)** via C++/WinRT headers: video capture
  - **Media Foundation**: hardware H.264 video + AAC audio → MP4
  - **WASAPI**: desktop sound + microphone
  - **IFileOperation**: renaming with Explorer undo (Ctrl+Z)
- Settings in one INI file: `%APPDATA%\Canivete\settings.ini`. Settings window = plain Win32 dialog.
- Personal use only, never sold or shared, so code from all three may be reused freely
  (PowerToys MIT, ShareX/OBS GPL; the GPL only matters when distributing). Most useful pieces:
  PowerRename engine (C++, drop-in), OBS hybrid-MP4 muxer (C, crash-safe recordings), OBS cursor
  capture and NV12 shaders, ShareX GIF palette + dirty-rectangle encoder (C#, port to C++).
  Keep a NOTICES file listing where each piece came from.

## App shell (phase 0)

- Tray icon with menu: each tool, recent colors, Settings, Exit.
- Global hotkeys with `RegisterHotKey` on a hidden tool window (ShareX's approach; survives an
  Explorer restart). Hotkeys are editable in Settings.
- Print Screen: Windows 11 gives it to Snipping Tool. Offer a checkbox that sets
  `HKCU\Control Panel\Keyboard\PrintScreenKeyForSnippingEnabled = 0` (what ShareX's installer does).
- Single instance: a second launch (e.g. from Explorer) forwards its file list to the running app
  via `WM_COPYDATA`, so selecting 200 files opens one renamer window, not 200.
- Per-Monitor-V2 DPI aware; all capture coordinates in physical pixels.

Default hotkeys (changeable):

| Action | Hotkey |
|---|---|
| Region screenshot | Ctrl+PrtSc |
| Full-screen screenshot | PrtSc |
| Record MP4 (press again = stop) | Shift+PrtSc |
| Record GIF (press again = stop) | Ctrl+Shift+PrtSc |
| Color picker | Win+C (same as Henri's PowerToys) |

## 1. Screenshots

How ShareX does it, and we copy the approach:
- On hotkey, grab the whole virtual desktop once with GDI `BitBlt(SRCCOPY|CAPTUREBLT)`, then show a
  borderless topmost window over all monitors that displays that frozen image.
- User drags a region, or hovers to snap to a window (list from `EnumWindows`, skipping invisible /
  cloaked / tool windows, bounds from `DWMWA_EXTENDED_FRAME_BOUNDS`). Esc cancels.
- Magnifier + pixel coordinates near the cursor for precise edges.
- The overlay window is created hidden at startup so the first capture is instant.
- Optional mouse cursor in the shot (`GetCursorInfo` + `DrawIconEx`, scaled for large accessibility
  cursors).
- After capture: save PNG (WIC) to a chosen folder **and** copy to clipboard (both on by default).
  File name pattern with tokens, e.g. `%y-%mo-%d_%h-%mi-%s` or `%pn` (app name).
- Later (only if needed): correct colors on HDR monitors via DXGI Desktop Duplication + tone-map.

## 2. Screen recording (MP4 and GIF)

Same pipeline ShareX 21 now uses by default, refined with OBS details:
- Pick region / window / monitor with the same overlay as screenshots.
- Capture with **WGC** (records a window even when covered), cursor on/off, and request
  **borderless** access so Windows doesn't draw its yellow frame. Region = crop on the GPU, size
  rounded to even numbers (video encoders need that).
- Frames stay on the graphics card: BGRA → NV12 conversion on the GPU, then the **Media Foundation
  hardware H.264 encoder** (works on NVIDIA, AMD, Intel without vendor SDKs). Fixed 30 fps clock
  (60 optional); when the screen doesn't change the last frame is repeated, which costs almost
  nothing.
- Quality: try constant-quality mode first (OBS uses quality ~20–23); fall back to bitrate scaled
  by region size. If the PC has no hardware encoder, use Windows' software encoder and say so.
- Sound: WASAPI desktop audio (+ optional microphone), AAC 192 kbps. Includes OBS's fix of playing
  silence on the device so the loopback stream doesn't stall when nothing is playing.
- On screen while recording: thin dashed border outside the region plus a small timer/stop button,
  both hidden from the video with `WDA_EXCLUDEFROMCAPTURE`. Same hotkey stops. Esc-hold aborts.
- **GIF** (own encoder, no ffmpeg): while recording, store frames compressed in a temp file at
  15 fps. After stop: build one shared 256-color palette from all frames, dither (Sierra Lite or
  ordered/Bayer for smaller files), write only the changed rectangle of each frame, exact timings.
  Port ShareX's `AdaptiveGifQuantizer` + `GifWriter` (already does exactly this).
  Optional later: animated WebP, much smaller than GIF.
  MP4 built (2026-11-10), video only: same BitBlt capture loop as the GIF at 30 fps, own BGRX → NV12
  (BT.709 video range) on the CPU, Media Foundation sink writer with hardware transforms on (NVIDIA
  H.264 Encoder MFT here), quality mode 70 accepted, bitrate fallback ~0.08 bit/pixel, software
  encoder fallback. 2560×1440: 30 fps steady, ~50% of one core, 10 s of desktop ≈ 750 KB. The
  "moov first" (fast start) sink option fails (access denied / broken file), so it is off.
  Sound built (2026-11-10): WASAPI shared mode asking Windows for 48 kHz stereo float (AUTOCONVERTPCM),
  loopback of the default output plus a silent render stream so loopback keeps flowing (OBS trick),
  optional default microphone mixed in; packets placed by their capture time (gaps filled with
  silence, overlaps cut, 50 ms slack); AAC 192 kbps as a second sink-writer stream (throttling off,
  writes behind a mutex). If the sound stream can't be added, the video is recorded without it.
  Settings: computer sound on, microphone off by default. Sound and picture end within ~20 ms.
  Still to do: WGC + GPU colour conversion if CPU use matters.
  Built (2026-11-10): records changed 32×32 tiles at 25 fps, then makes three GIFs at once
  (Smallest / Balanced / Smoothest) so the save window shows each real size. Own Wu + k-means
  palette and ordered dither instead of a ShareX port.

## 3. Color picker

Copy of PowerToys' behaviour without its resident WinUI process:
- Hotkey → transparent full-screen input window + small floating loupe following the cursor
  (live zoomed view, nearest-neighbour, pixel grid), loupe excluded from capture.
- Read the pixel with a 1×1 `BitBlt`. Arrow keys nudge the cursor 1 px, wheel changes zoom,
  click or Enter copies, Esc / right-click cancels.
- Copy format chosen in Settings: HEX (default), RGB, HSL, HSV, and **float 0–1** (sRGB or
  linear, handy for shaders, MAXScript and bpy).
- Last 10 colors in the tray menu, click to copy again.

## 4. File & folder renamer

Engine ported from PowerToys' PowerRename (MIT, portable C++):
- Search → replace, plain text or regex (`std::wregex`), first match or all, case sensitive.
- Apply to name, extension, or both; include/exclude files, folders, subfolders.
- Case: UPPER, lower, Title, Capitalize first.
- Counter: `${start=1,increment=1,padding=3}`; date tokens from file date (`$YYYY $MM $DD $hh …`).
- Live preview list (old name → new name, conflicts and invalid names in red), computed on a worker
  thread.
- Rename with one `IFileOperation`, deepest items first, with an undo record: Ctrl+Z in Explorer
  undoes it.
- Launch: Explorer right-click → "Show more options" entry and "Send to", or drag files onto the
  window. (The Windows 11 short menu needs a signed package: skipped on purpose, can come later.)

## 5. Image resizer

PowerToys' feature set, done directly with WIC:
- Presets: Small 854×480, Medium 1366×768, Large 1920×1080, Custom (px or %).
- Fit / Fill (crop centre) / Stretch, "only shrink", respect portrait/landscape.
- Output: new file named `%1 (%2)` (original name + preset), a chosen folder, or replace originals
  (old ones go to the Recycle Bin). JPEG quality setting (default 90). Keep or strip metadata;
  always honour photo rotation (EXIF orientation).
- High-quality scaling (WIC Fant / HighQualityCubic), all CPU cores in parallel.
- Same launch paths as the renamer.

## Build order (one phase at a time, each tested on this PC before the next)

| Phase | What you get | Why this order |
|---|---|---|
| 0 | Tray app, hotkeys, settings, start with Windows | Base for everything |
| 1 | Screenshots (region / window / full) | Builds the overlay that phases 2 and 5 reuse |
| 2 | Color picker | Small, reuses overlay + magnifier |
| 3 | Image resizer | Independent, quick win |
| 4 | Renamer | Independent, biggest UI |
| 5 | MP4 recording (video first, then sound) | Done 2026-11-10 |
| 6 | GIF recording | Reuses phase 5 capture |
| 7 | Polish: settings window, icons, mixed-DPI + multi-monitor checks, size/RAM measurement | |

Each phase ends with: build, copy the exe to its run folder, restart the app, test by hand, and
check idle RAM and size against the targets above.

## Known risks

- Mixed-DPI multi-monitor overlays are the classic source of bugs (offset or blurry selection):
  test early in phase 1.
- Media Foundation quality controls differ per GPU driver; quality mode may need a bitrate fallback.
- Hybrid laptops (two GPUs) can break desktop capture; WGC avoids most of it.
- HDR monitors: screenshots look washed out with GDI. Fix only if you use HDR.
