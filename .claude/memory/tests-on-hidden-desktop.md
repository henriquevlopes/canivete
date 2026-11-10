---
name: tests-on-hidden-desktop
description: Canivete UI tests must run on a hidden desktop via tools/hidden_test.ps1 so no window pops up on Henri's screen
metadata:
  type: feedback
---

Run every Canivete UI test through `tools\hidden_test.ps1 "<command>"` (since 2026-11-10). It starts the
command on a hidden Windows desktop; Canivete copies started there run in test mode (`CANIVETE_TEST`
= `%TEMP%\CaniveteTest`: own settings.ini and log there, no tray icon, hotkeys or Explorer entries,
separate single-instance mutex) and are killed afterwards. The real copy in the tray is never touched.

**Why:** Henri works at the PC while tests run; test windows popping up on screen interrupted him.

**How to apply:** test scripts find windows, send them messages and capture them with PrintWindow
exactly as before (all of that works on the hidden desktop), but launch them only through the
runner. Read the test copy's log at `%TEMP%\CaniveteTest\logs\canivete.log`. Only Explorer verbs
(`Start-Process -Verb ...`) need the real copy; avoid them unless the verb itself changed.
In PowerShell P/Invoke, pass `[NullString]::Value`, never `$null`, for a string argument.
