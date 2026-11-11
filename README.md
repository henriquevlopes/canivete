# Canivete

A small toolbox for Windows that lives in the tray (Portuguese *canivete* = pocket knife). One light
native app, no runtimes: press a hotkey, get the job done, back to work.

## Install

Open PowerShell (no admin needed) and run:

```powershell
irm https://henriquevlopes.github.io/canivete/get.txt | iex
```

Or download `Canivete-Setup-<version>.exe` from the
[releases page](https://github.com/henriquevlopes/canivete/releases/latest).

Updates: a green dot appears on the tray icon when a new version is ready. Right-click the icon,
**Update to X and restart**. Only releases signed by the author are installed.

## What it does

- **Screenshots**: region, window, monitor or all screens. A preview appears in the corner; drag it
  straight into a chat.
- **Editor**: arrows, shapes, text, numbered steps, blur, transparent background.
- **Screen recording**: GIF and MP4, with sound and a keys-and-clicks overlay.
- **Extract text** (OCR): select an area, the text is copied.
- **Color picker** with a magnifier.
- **Image tools**: resize, remove background (offline), bulk rename.
- **Uploads** to Google Drive or Amazon S3, link copied.
- **Computer maintenance**: updates, cleanup, startup apps, installed apps, health.

## Hotkeys (changeable in Settings)

| Hotkey | Action |
|---|---|
| Ctrl+PrtSc | Region, window or monitor screenshot |
| PrtSc | All monitors |
| Win+C | Color picker |
| Win+Shift+T | Extract text |
| Ctrl+Shift+PrtSc | Record a GIF (press again to stop) |
| Shift+PrtSc | Record a video (press again to stop) |
| Ctrl+Alt+PrtSc | Pause / resume the recording |
