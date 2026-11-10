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
| 5 | MP4 recording | done (video + sound) |
| 6 | GIF recording | done |
| 7 | Computer maintenance (overview, updates, installed apps, cleanup, startup apps, health, daily check) | done |
| 8 | Installer and self-update | done |
| 9 | Uploads (Google Drive, Amazon S3) | done |

## Use

| Hotkey | Action |
|---|---|
| Ctrl+PrtSc | Region: drag a box, or click the highlighted window / monitor under the mouse to take just that (no separate window hotkey needed). Esc cancels, arrows nudge 1 px (Shift = 10), right-click drops a drag |
| PrtSc | All monitors (or the one under the mouse, see settings) |
| Win+C | Color picker: the screen freezes, a magnifier follows the mouse. Click, Enter or Space copies the color; Esc or right-click cancels; arrows nudge |
| Ctrl+Shift+PrtSc | Record a GIF: pick an area like a region screenshot; press again (or Stop) to finish |
| Shift+PrtSc | Record a video (MP4): pick an area the same way; press again (or Stop) to finish |

GIF recording: a red border marks the area and a small bar shows the time with Stop and Cancel
(neither shows up in the GIF). Recording runs at 25 frames a second and full size; only the parts
that change are kept. After Stop, a "Save GIF" window offers three ready-made GIFs, each with its
real file size: **Smallest file** (10 fps, 64 colors), **Balanced** (15 fps, 256 colors) and
**Smoothest** (25 fps, 256 colors, smooth gradients). Click one: it goes to the screenshots folder
and is copied as a file, ready to paste. The last choice is the default next time (Enter takes it).
The encoder builds one palette for the whole GIF and writes only the changed part of each frame.

Video recording: the same border and bar (the dot is grey with "…" for the moment the video
encoder takes to start, then red with the time). 30 frames a second, full size (an odd width or
height loses one pixel, as video needs even sizes), mouse pointer included. Sound: what the computer plays is
recorded by default, the microphone only if switched on (Settings → Video recording); both are
mixed into one track (AAC, 48 kHz stereo). Stop opens a save window like the GIF one, with three
sizes, each showing its real file size (the two smaller ones take a few seconds to work out):
- Smallest file: lower quality, sound 96 kbps; recordings with lots of motion also go to half size
  (never below 640 pixels). Scrolling text came out bigger at half size, so calm ones keep theirs.
- Balanced: full size, a little softer (text looks the same), sound 128 kbps.
- Best quality: the recording as it is.
10 s of 1080p: scrolling text 0.8 / 1.0 / 1.8 MB, full-screen motion 0.75 / 8.6 / 24.5 MB.
Enter takes the choice made last time; the file goes to the screenshots folder and is copied,
ready to paste. Discard (or Cancel on the bar) throws it away. H.264 on the graphics card's own encoder when there is one (NVIDIA, AMD, Intel), otherwise
Windows' software encoder; quality-based, so a still screen costs almost nothing (10 s of a
2560×1440 desktop: about 750 KB).

Screenshots are saved as PNG (or JPG) to the Windows Screenshots folder and copied to the clipboard
(as image and PNG). A small preview pops up bottom-right: click opens the editor, right-click shows
the file in Explorer. Settings can open the editor right after every capture instead. Tray icon left-click opens Settings; right-click: capture menu, Pick a color, Recent colors (last 10; click one to
copy it again) and Settings.

After a pick, a small menu lists the color in every format: Gray 0-1 (brightness like Substance's
linear grayscale value), HEX, RGB and HSL. The default format from Settings
sits on top, so Enter takes it; Esc copies nothing. Windows reserves Win+C,
so Win-key hotkeys it refuses are caught with a keyboard hook instead (the Start menu stays closed).
Recent colors live in `%APPDATA%\Canivete\colors.txt`.

Settings: tray > Settings… opens a small window with a tab per feature on the left (General,
Screenshots, Color picker, GIF recording, Video recording, Image tools, Renamer, Maintenance; Ctrl+Tab switches).
General has the folder, file name (with an Insert menu of tokens and a live example), PNG/JPG and
quality, start with Windows and Appearance (Follow Windows, Light or Dark: every Canivete window, its title bar and the
tray menu switch at once, and Follow Windows tracks the Windows app mode live); each feature tab has its hotkey (click the box, press the keys) and
options. Each feature also has an On/Off switch next to its name in the list: a feature that is off loses its hotkeys, its
tray menu entries and its Explorer right-click and Send to entries (its tab is greyed out).
Everything is stored in
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

Make alpha (editor α button, or M): turns the screenshot into a square alpha for Substance Painter,
details white on a black background. Mark exactly what you want with **Lasso** (L, drag freely) or
**Rectangle** (R, drag a box); several areas add up and outside goes dark. Click a detail's color to pick
it (up to 8), right-click the background color; with no clicks it guesses from the most common color.
The **Levels** bar under the result works like Photoshop's: the histogram shows how "detail-like" each
pixel is; drag the black, mid and white handles to choose where black ends, where white starts and how
the grays in between bend (red curve). Double-click it to reset. Shortcuts: wheel or + / - = more / less
detail, `[` `]` = sharper / softer. Buttons: Invert (I), Size (auto picks 512-4096 from the detail size;
remembered), Start over (Backspace), Copy (Ctrl+C), Save (Enter or Ctrl+S: `<capture name> alpha.png`
in the screenshots folder, also copied). Ctrl+Z undoes the last pick or area, Esc closes.

Right-click menu: select files or folders in Explorer, right-click > Show more options > Canivete.
On images:
- Resize > Small / Medium / Large: new files next to the originals (`photo (Medium).jpg`),
  no window; Choose a size… opens the resizer below.
- Convert to > JPG / PNG / TIFF: new files next to the originals (`photo.png` > `photo.jpg`);
  see-through parts become white in JPG; images already in that format are skipped.
- Rotate > Turn right / left, Flip left to right: saves over the
  original (same name and dates; JPG at quality 95 or more, photo info kept). JPG, PNG, BMP, TIFF only.
- Remove background: cuts the subject out and saves `photo (no background).png` next to the
  original, with a transparent background, at full size (see below).
- Copy to clipboard: the (first) image, upright, ready to paste.

On everything (files and folders): Rename… opens the renamer below. A tray notice says what was
done; problems show in a message. Explorer refreshes the thumbnails of changed images. Windows
shows at most 16 entries in this menu, so a few extras (half size, upside down, flip top to bottom,
BMP) are only on the command line.

Remove background (right-click menu, or the editor's scissors button / B): a free AI model,
[BiRefNet](https://github.com/ZhengPeng7/BiRefNet) (MIT license), finds the subject; it runs on this
computer, on the graphics card through DirectML (about 1-2 s a photo on an RTX 3080 Ti laptop) or on
the processor when there is no usable graphics card. The first use asks to download it once, about
500 MB, from the official places: the model from Hugging Face (`onnx-community/BiRefNet-ONNX`, pinned
to one version) and Microsoft's ONNX Runtime 1.24.4 from NuGet (plus Microsoft's DirectML 1.15.4,
~200 MB, only on a Windows whose own DirectML.dll is older than 1.15). Every file is checked against
its SHA-256; all of it goes to `%APPDATA%\Canivete\ai`. Canivete then rewrites the model once (its
deformable convolutions become GridSample + a 1x1 convolution, same result, ~10x faster on DirectML;
`src/rmbg_model.cpp`, Python version in `tools/ai/fixdeform.py`). The mask is computed at 1024 x 1024
and brought back to full size with a guided filter, so hair and edges follow the photo, and the edge
colors are cleaned of the old background (blur-fusion foreground estimation) so there is no colored
fringe. A small window bottom-right shows progress and can cancel; the model stays loaded 3 minutes
after the last picture, then frees the graphics card. From the editor the result is saved as
`<capture name> (no background).png` in the screenshots folder and copied (PNG with transparency,
plus a plain copy on white for apps that don't read PNG).
Models considered: RMBG-2.0 (BRIA) is as good but non-commercial only; BEN2 (MIT) and the BiRefNet
lite model are close but slower or rougher here; ISNet/U²-Net are instant but cut hair coarsely;
Windows' own background remover needs a Copilot+ PC; cloud services need a subscription.

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

Renamer: select files or folders in Explorer, right-click > Show more options > Canivete > Rename…
(or Send to > Canivete - Rename), or tray > Rename files…, or drop them on its window.
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

Computer maintenance: tray > Computer maintenance… (or Start menu > Computer Maintenance).
- Overview: one list of everything that keeps the PC up to date and healthy, each with a colored dot
  and what to do: Windows updates (and days since the last one), drivers and device problems, app
  updates, graphics driver age, virus protection (definitions, last scan), restart, disk space per
  drive, cleanup, battery wear, Canivete's own version, Store apps. Double-click a row (or Do it) to
  deal with it. Last results are kept in `%APPDATA%\Canivete\maintenance.ini`.
- Daily check (Settings > Maintenance, on by default): about once a day Canivete checks for updates
  in the background and shows a notice when there are new ones (or virus protection needs a look);
  clicking it opens the Overview. Updates already seen don't notify again.
- Updates: Windows and driver updates (Windows Update), app updates (winget), and the graphics driver
  with a button to the maker's app (NVIDIA app, AMD Adrenalin, Intel). Tick what to install; apps left
  unticked stay unticked next time (`[Maintenance] SkipApps`). Install makes a restore point first
  (optional) and asks Windows for permission once.
- Cleanup: a fixed list of safe places (temp files older than a week, Windows Update downloads,
  Delivery Optimization cache, old crash dumps, shader caches, Recycle Bin, DISM component cleanup).
  No registry cleaning.
- Startup apps: the same switches as Task Manager's Startup tab (Run keys and Startup folders).
- Installed apps: every installed app (name, publisher, version, size, date) with search; Uninstall runs
  the app's own uninstaller (msiexec /x for MSI apps).
- Health: uptime and restart advice, memory, disk space, repair (DISM + sfc), optimize drives, restore
  point, Defender quick scan.

Uploads (Settings → Uploads): screenshots, GIFs and videos go to Google Drive or Amazon S3 and
the link is copied, ready to paste. Upload button on the screenshot preview, in the editor (key U:
copy, save, upload, close) and an "Also upload" box in the GIF/video save window; or tick
"Upload by itself" for screenshots, GIFs and/or videos. A tray notice shows it going up (the tray
tooltip shows the percent) and then "Link copied" (click it to open the link); errors say what to
fix and clicking them opens Settings → Uploads. Passwords and keys are kept encrypted for this
Windows user (DPAPI, `%APPDATA%\Canivete\secrets.ini`), never in settings.ini.
- Google Drive (once): in Google Cloud Console make a project, enable the Google Drive API, set up
  the OAuth consent screen (External, add yourself as a test user), then Credentials → Create
  credentials → OAuth client ID → Desktop app. Paste its Client ID and Client secret into
  Canivete and click Connect…: the browser asks which Google account to use. **Then publish the
  app** (OAuth consent screen → Publish app → In production): while it is in Testing, Google ends
  the sign-in after 7 days and you have to connect again. An unverified app is fine for your own
  account (Google shows an "unverified" warning once; Advanced → continue). Canivete only gets
  the `drive.file` permission: it sees only files it made itself. Files go into a "Canivete"
  folder (changeable) and are shared "anyone with the link can view" (can be turned off).
  Big files go up in 8 MB pieces and pick up where they stopped if the connection breaks.
- Amazon S3 (once): make a bucket, then in IAM a user just for Canivete with this inline policy
  (the "How to set it up…" button copies it, filled in with your bucket and folder), and an access
  key for it ("Application running outside AWS"). That key never expires; don't use temporary
  (session) keys, which stop working within hours.
  `{"Version":"2012-10-17","Statement":[{"Effect":"Allow","Action":["s3:PutObject","s3:GetObject"],"Resource":"arn:aws:s3:::BUCKET/canivete/*"}]}`
  Fill in bucket, region (e.g. `eu-west-1`), folder, access key ID and secret key, then Test.
  Links: the bucket's own address (the bucket or folder must allow public reads), a "Link
  address" in front (CloudFront or your own domain), or tick "Private bucket" for signed links
  that work for 7 days. Up to 5 GB a file.

Updates of Canivete itself: it checks the public `henriquevlopes/canivete-releases` GitHub releases
(no token, about once a day, Settings > General can turn it off), shows a notice and a tray menu item
when a newer version is out, downloads the setup, checks its SHA256 against the release notes and
installs it silently.

Command line: `Canivete.exe --region | --window | --full | --color | --gif | --mp4 | --settings` acts in the running copy.
`Canivete.exe --edit image.png` opens any image in the editor; `--alpha image.png` opens Make alpha.
`Canivete.exe --alpha-bench in.png out.png [x y]... [box x y w h]` makes an alpha with no window (test).
`Canivete.exe --nobg-bench in.jpg out.png` removes the background with no window, downloading the AI
files first if needed; timings go to the log (test).
`Canivete.exe --resize a.jpg b.png folder` opens the resizer with those files (each copy hands its
files to the running one, so Explorer's one-copy-per-file launch ends in one window).
`Canivete.exe --rename a.txt folder` opens the renamer the same way.
`Canivete.exe --maintenance` opens Computer maintenance (the Start menu's Computer Maintenance shortcut does this).
`Canivete.exe --maint-admin <window> <task> [items]` is the elevated copy that window starts for one job; it refuses
to run unless that window exists.
`Canivete.exe --maint-check` runs the daily maintenance check now; `--check-update` checks for a new Canivete.
`Canivete.exe --update-test` checks the releases repo, downloads the setup and verifies its SHA256 (self-test).
`Canivete.exe --uninstall` removes Canivete's Explorer entries, shortcuts and autostart (the uninstaller runs it).
`Canivete.exe --click-notice` (test mode only) acts as if the last notice was clicked.
`Canivete.exe --upload a.png b.mp4` uploads files with the running copy; `--drive-connect` starts the Google sign-in;
`--s3-test` uploads a small test file. `--upload-selftest` checks the AWS signing (against AWS's own examples),
PKCE and the encrypted secrets, and exits.
`tools\test_upload.ps1` (run through `tools\hidden_test.ps1`) tests uploads end to end against a fake Google and AWS
server (`tools\upload_mock.py`, needs Python): sign-in, resumable pieces with a broken connection, signatures,
the Upload buttons, errors. Test copies only: `CANIVETE_UPLOAD_MOCK=http://127.0.0.1:port` sends every upload
there, `--set-secret name value` stores a secret, `CANIVETE_GRAB_FAKE=1` makes screenshots a gradient (a hidden
desktop has no screen).
`Canivete.exe --do <action> a.jpg b.png` runs a right-click menu action (`small medium large half custom
jpg png tif bmp right left 180 fliph flipv nobg copy`).
`Canivete.exe --grab out.png` (or `.jpg`) grabs all monitors to a file and exits (self-test).
`Canivete.exe --mp4-bench x y w h ms out.mp4` records that area with no window and logs frames, CPU, size and sound (self-test).
`Canivete.exe --mp4-shrink in.mp4 out.mp4 percent quality kbps` makes one smaller copy and logs its size and time (self-test).
In test mode, `CANIVETE_AUDIO_FAKE=1` records a quiet 440 Hz tone instead of real sound.

## Build and install

Visual Studio 2026 (C++ desktop workload) and CMake.

```powershell
powershell -ExecutionPolicy Bypass -File tools\deploy.ps1
```

Builds Release, installs to `%LOCALAPPDATA%\Programs\Canivete\Canivete.exe` and restarts it.

Setup file (Inno Setup 6, `winget install JRSoftware.InnoSetup`): `tools\make_setup.ps1` builds
`dist\Canivete-Setup-<version>.exe`, a per-user installer (no admin) to the same folder, with a Start
menu entry and an uninstaller in Windows' app list. `/TESTRUN` makes it leave the running copy alone
(see `installer\canivete.iss`).

Release: bump the version in `CMakeLists.txt`, then
`powershell -ExecutionPolicy Bypass -File tools\release.ps1 -Notes "What changed"`. It builds the setup
and publishes it with its SHA256 on `henriquevlopes/canivete-releases`; running copies pick it up.

## Code

- `src/main.cpp` — tray icon, hotkeys (plus a keyboard hook for reserved Win combos), menu, settings-file watch, single instance
- `src/settings.cpp` — INI settings, hotkey parsing, log
- `src/settings_ui.cpp` — Settings window: tab list, per-feature On switches (built in code, per-monitor DPI, hotkey recording hook)
- `src/capture.cpp` — GDI screen grab, region overlay (frozen screen, window snapping, magnifier), color picker mode
- `src/color.cpp` — color text formats, clipboard text, recent colors
- `src/output.cpp` — PNG/JPG via WIC, file naming, clipboard, notification toast
- `src/editor.cpp` — screenshot editor window (GDI+ shapes and text, undo, zoom and pan)
- `src/resizer.cpp` — image resizer window, WIC resize engine on worker threads, right-click quick actions (convert, rotate, copy)
- `src/renamer.cpp` — renamer window, live preview, regex/counter/date tokens, IFileOperation rename with undo
- `src/gif.cpp` — GIF encoder: recording file, palette (exact colors or Wu + k-means), LZW, changed-area frames, presets
- `src/recorder.cpp` — recording border and bar, capture thread (GIF 25 fps, video 30 fps), Save GIF window, video save
- `src/video.cpp` — MP4 writer: BGRX to NV12 (BT.709), Media Foundation H.264 sink writer, hardware encoder with software fallback, AAC sound stream; the smaller copies offered after recording (decode, shrink, re-encode)
- `src/audio.cpp` — video sound: WASAPI loopback of the default output + optional microphone, mixed, timed by capture clock
- `src/alpha.cpp` — Make alpha window: freehand mask, detail/background picks, threshold engine, square power-of-two output
- `src/rmbg.cpp` — Remove background: first-use download (WinHTTP, SHA-256, `tar.exe` unpacks the NuGet packages), ONNX Runtime loaded at run time (C API, DirectML on the card with the most memory, CPU fallback), worker thread and progress window, guided-filter upscale of the mask, blur-fusion edge colors, transparent PNG
- `src/rmbg_model.cpp` — rewrites BiRefNet's exported deformable convolutions into GridSample + 1x1 Conv (minimal protobuf reader/writer)
- `third_party/onnxruntime/include` — ONNX Runtime 1.24.4 C API headers (MIT, `third_party/onnxruntime/LICENSE`); the DLL itself is downloaded
- `src/maint.cpp` — Computer maintenance window: Overview, Updates, Installed apps, Cleanup, Startup apps, Health tabs; worker threads; elevated jobs report back with WM_COPYDATA; daily background check
- `src/maint_info.cpp` — Overview checks: antivirus (Security Center + Defender WMI), device problems, battery wear, last Windows Update, installed apps
- `src/theme.cpp` — dark/light theme: palette shared by every window, dark title bars, control themes (DarkMode_Explorer/CFD/ItemsView), dark check boxes, dark tray menu, live switch on `ImmersiveColorSet`
- `src/upload.cpp` — uploads: Google sign-in (loopback + PKCE), Drive resumable upload, S3 SigV4 PUT and signed links (WinHTTP, BCrypt), DPAPI secrets, upload queue and notices
- `src/update.cpp` — Canivete self-update: GitHub releases API (WinHTTP), SHA256 check (BCrypt), silent setup run
- `src/maint_ops.cpp` — the checks and jobs: Windows Update agent (COM), winget, GPU driver info, cleanup whitelist, startup entries (StartupApproved), restore point, DISM/sfc, defrag
- `src/shell.cpp` — Explorer right-click Canivete submenu (image actions, Rename), Send to shortcuts, Start menu Computer Maintenance
