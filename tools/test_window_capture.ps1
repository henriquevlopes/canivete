# Window capture with Canivete's own Settings window open: clicking it must capture exactly that window.
# Run through the hidden desktop:
#   powershell -ExecutionPolicy Bypass -File tools\hidden_test.ps1 "powershell -ExecutionPolicy Bypass -File tools\test_window_capture.ps1"
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$exe = "$root\build\Release\Canivete.exe"
$log = Join-Path $env:CANIVETE_TEST "logs\canivete.log"
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W {
  [StructLayout(LayoutKind.Sequential)] public struct R { public int l, t, r, b; }
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out R r, int n);
}
"@
Remove-Item $log, (Join-Path $env:CANIVETE_TEST "cursor.txt") -ErrorAction SilentlyContinue
$env:CANIVETE_GRAB_FAKE = "1"
Start-Process $exe
Start-Sleep -Seconds 2
& $exe --settings
$h = [IntPtr]::Zero
for ($i = 0; $i -lt 50 -and $h -eq [IntPtr]::Zero; $i++) {
    Start-Sleep -Milliseconds 100
    $h = [W]::FindWindow([NullString]::Value, "Canivete Settings")
}
if ($h -eq [IntPtr]::Zero) { "FAIL: Settings window not found"; exit 1 }
$r = New-Object W+R
[void][W]::DwmGetWindowAttribute($h, 9, [ref]$r, 16)  # DWMWA_EXTENDED_FRAME_BOUNDS
"{0},{1}" -f ([int](($r.l + $r.r) / 2)), ([int](($r.t + $r.b) / 2)) | Set-Content -Encoding ascii (Join-Path $env:CANIVETE_TEST "cursor.txt")
& $exe --window
Start-Sleep -Seconds 2
$want = "Capture {0}x{1} at {2},{3}" -f ($r.r - $r.l), ($r.b - $r.t), $r.l, $r.t
$got = Select-String -Path $log -Pattern "^.*Capture \d+x\d+ at" | Select-Object -Last 1
"Settings window: $want"
"Logged:          $got"
Get-Process Canivete -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe } | Stop-Process -Force
if ($got -and $got.Line.Contains($want)) { "PASS"; exit 0 } else { "FAIL"; exit 1 }
