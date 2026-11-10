# Test of pause, trim and "show keys and clicks" for GIF and video recordings, with the test picture
# (CANIVETE_GIF_FAKE) and test tone (CANIVETE_AUDIO_FAKE). Run it on the hidden desktop:
#   powershell -ExecutionPolicy Bypass -File tools\hidden_test.ps1 "powershell -ExecutionPolicy Bypass -File tools\test_record.ps1"
# Needs ffmpeg / ffprobe and Python (numpy, Pillow) for the checks in check_record.py. Prints
# PASS/FAIL lines; the exit code is the number of failures. Proof files go to -Out (test_out\).
param([string]$Exe = (Join-Path $PSScriptRoot "..\build\Release\Canivete.exe"),
      [string]$Out = (Join-Path $PSScriptRoot "..\test_out"), [string]$Theme = "light")

$ErrorActionPreference = "Stop"
$dir = $env:CANIVETE_TEST
if (-not $dir) { "CANIVETE_TEST is not set: run this through hidden_test.ps1"; exit 1 }
$Exe = (Resolve-Path $Exe).Path
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$log = Join-Path $dir "canivete.log"
$check = Join-Path $PSScriptRoot "check_record.py"
$env:CANIVETE_GIF_FAKE = "1"
$env:CANIVETE_GRAB_FAKE = "1"
$env:CANIVETE_AUDIO_FAKE = "1"
$script:fails = 0

Add-Type -ReferencedAssemblies System.Drawing @"
using System; using System.Runtime.InteropServices; using System.Drawing; using System.Drawing.Imaging;
public static class W {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
  [DllImport("user32.dll")] public static extern bool RedrawWindow(IntPtr h, IntPtr r, IntPtr g, uint f);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int l, t, r, b; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  public static void Capture(IntPtr h, string file) {
    RECT r; GetWindowRect(h, out r);
    using (var b = new Bitmap(r.r - r.l, r.b - r.t)) {
      using (var g = Graphics.FromImage(b)) { IntPtr dc = g.GetHdc(); PrintWindow(h, dc, 2); g.ReleaseHdc(dc); }
      b.Save(file, ImageFormat.Png);
    }
  }
}
"@

function Check($ok, $what) {
    if ($ok) { "PASS  $what" } else { "FAIL  $what"; $script:fails++ }
}
function LogLines { if (Test-Path $log) { @(Get-Content $log -Encoding UTF8) } else { @() } }
$script:mark = 0
function Mark { $script:mark = (LogLines).Count }
function WaitLog($pattern, [int]$sec = 20) {
    $end = (Get-Date).AddSeconds($sec)
    do {
        $hit = LogLines | Select-Object -Skip $script:mark | Where-Object { $_ -match $pattern } | Select-Object -First 1
        if ($hit) { return $hit }
        Start-Sleep -Milliseconds 200
    } while ((Get-Date) -lt $end)
    return $null
}
function SinceMark { LogLines | Select-Object -Skip $script:mark }
function Py([string[]]$a) { (& python $check @a) -join "`n" }
function Num($text, $name) { if ($text -match "$name=([\d.]+)") { [double]$Matches[1] } else { -1 } }

$script:app = $null
function StopApp {
    if ($script:app -and -not $script:app.HasExited) { $script:app.Kill(); $script:app.WaitForExit() }
    $script:app = $null
}
function WriteIni([string]$extra = "") {
    $ini = "[Screenshot]`r`nFolder=$dir\shots`r`nOpenEditor=0`r`nShowNotification=0`r`n" +
           "[General]`r`nCheckUpdates=0`r`nTheme=$Theme`r`n[Maintenance]`r`nDailyCheck=0`r`n" +
           "[Gif]`r`nShowKeys=1`r`n[Video]`r`nSound=1`r`nShowKeys=1`r`n$extra"
    [IO.File]::WriteAllText((Join-Path $dir "settings.ini"), $ini)
}
function StartApp([string[]]$a) {
    StopApp
    Mark
    $script:app = Start-Process $Exe -ArgumentList $a -PassThru
}
function Window($class, [int]$sec = 15) {
    $end = (Get-Date).AddSeconds($sec)
    do {
        $h = [W]::FindWindow($class, [NullString]::Value)
        if ($h -ne [IntPtr]::Zero -and [W]::IsWindowVisible($h)) { return $h }
        Start-Sleep -Milliseconds 200
    } while ((Get-Date) -lt $end)
    return [IntPtr]::Zero
}
# Waits until the three choices are made (all buttons enabled), then grabs and saves with Enter.
function SaveWindow([string]$grab) {
    $save = Window "CaniveteGifSave" 40
    if ($save -eq [IntPtr]::Zero) { return $false }
    $end = (Get-Date).AddSeconds(90)
    while ((Get-Date) -lt $end) {
        $ready = @(110, 111, 112 | Where-Object { [W]::IsWindowEnabled([W]::GetDlgItem($save, $_)) }).Count -eq 3
        if ($ready) { break }
        Start-Sleep -Milliseconds 300
    }
    [W]::RedrawWindow($save, [IntPtr]::Zero, [IntPtr]::Zero, 0x185) | Out-Null   # everything, now
    Start-Sleep -Milliseconds 700
    if ($grab) { [W]::Capture($save, $grab) }
    $end = (Get-Date).AddSeconds(60)
    while ([W]::IsWindow($save) -and (Get-Date) -lt $end) {
        [W]::PostMessage($save, 0x111, [IntPtr]1, [IntPtr]::Zero) | Out-Null   # Enter (IDOK)
        Start-Sleep -Milliseconds 500
    }
    return -not [W]::IsWindow($save)
}
function Newest($filter) { Get-ChildItem (Join-Path $dir "shots") -Filter $filter | Sort-Object LastWriteTime | Select-Object -Last 1 }

Get-ChildItem $dir -Exclude hidden_test_output.txt -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force
New-Item -ItemType Directory -Force (Join-Path $dir "shots") | Out-Null

try {
    # ---- 1. Video pause: the paused span is gone and sound stays in step -------------------------
    WriteIni
    Remove-Item Env:CANIVETE_REC_INPUT, Env:CANIVETE_REC_TRIM -ErrorAction SilentlyContinue
    $env:CANIVETE_REC_PAUSE = "1500,1300"   # pause at 1.5 s of recording, for 1.3 s
    $mp4 = Join-Path $Out "paused.mp4"
    Mark
    Start-Process $Exe -ArgumentList "--mp4-bench", "0", "0", "320", "240", "6000", "`"$mp4`"" -Wait
    Check (WaitLog "Recording resumed after 13\d\d ms paused" 5) "video pause: paused and resumed"
    $rec = WaitLog "Video bench: .* ms recorded" 5
    $recMs = if ($rec -match "(\d+) ms recorded \((\d+) ms with setup") { [int]$Matches[1] } else { 0 }
    $d = Py "duration", $mp4
    "      $d   ($rec)"
    $v = Num $d "video"; $a = Num $d "audio"
    Check ($recMs -gt 0 -and [math]::Abs($v * 1000 - $recMs) -lt 40) "video pause: file as long as the time recorded ($v s, $recMs ms)"
    Check ([math]::Abs($v - $a) -lt 0.05) "video pause: sound as long as the picture ($a s)"
    $s = Py "sync", $mp4
    $s -split "`n" | ForEach-Object { "      $_" }
    # The picture flashes each whole second of real time. Paused at 1.5 s for 1.3 s, the flash of
    # second 3 lands at 3.0 - 1.3 = 1.7 s in the video: the paused span is gone, to the frame.
    $flashes = (($s -split "`n" | Where-Object { $_ -like "flash at*" }) -replace "flash at", "").Trim() -split "\s+" | ForEach-Object { [double]$_ }
    $after = $flashes | Where-Object { $_ -gt 1.2 } | Select-Object -First 1
    Check ($after -and [math]::Abs($after - 1.7) -le 0.035) "video pause: 1.3 s left out (first flash after the pause at $after s, expected 1.7)"
    $worst = Num $s "worst"
    Check ((Num $s "pairs") -ge 4 -and $worst -ge 0 -and $worst -le 34) "video pause: flashes and beeps line up after resume (worst $worst ms, a frame is 33)"
    Remove-Item Env:CANIVETE_REC_PAUSE

    # ---- 2. Video: keys and clicks drawn in, trimmed on save --------------------------------------
    WriteIni
    $env:CANIVETE_REC_INPUT = "400:L:80,60;900:K:Ctrl+B;1100:R:300,150;1300:K:Ctrl+Z;1350:K:Ctrl+Z;1400:K:Ctrl+Z;" +
                              "1600:K:A;1700:K:Shift+A;1800:K:Tab;2000:K:Delete"
    $env:CANIVETE_REC_TRIM = "1000,3000"
    StartApp "--mp4-test", "0", "0", "480", "270", "4200"
    $ok = SaveWindow (Join-Path $Out "save_video_timeline.png")
    Check $ok "video: save window, timeline, saved"
    $lines = SinceMark
    Check ($lines -match "Overlay key: Ctrl\+B") "keys: Ctrl+B shown"
    Check ($lines -match "Overlay key: Ctrl\+Z x3") "keys: repeats counted (Ctrl+Z x3)"
    Check ($lines -match "Overlay key: Tab" -and $lines -match "Overlay key: Del") "keys: special keys shown (Tab, Del)"
    Check (-not ($lines -match "Overlay key: (Shift\+)?A$")) "keys: typed letters not shown (A, Shift+A)"
    Check ($lines -match "Overlay click: left" -and $lines -match "Overlay click: right") "clicks: left and right"
    Check ($lines -match "Trim: 1000 to 3000 ms") "video trim: applied"
    $f = Newest "*.mp4"
    if ($f) {
        Copy-Item $f.FullName (Join-Path $Out "trimmed.mp4") -Force
        $d = Py "duration", $f.FullName
        "      $d"
        $v = Num $d "video"; $a = Num $d "audio"
        Check ($v -gt 1.95 -and $v -lt 2.06) "video trim: 2.0 s kept ($v s)"
        Check ([math]::Abs($v - $a) -lt 0.06) "video trim: sound cut to the same length ($a s)"
        # 0.2 s into the trimmed video = 1.2 s of recording: the Ctrl+B pill and the right-click ring
        $fr = Py "frame", $f.FullName, "0.2", (Join-Path $Out "frame_keys_click.png")
        "      $fr"
        Check ((Num $fr "blue") -gt 40) "clicks: right-click ring in the frame"
        Check ((Num $fr "pill_dark") -gt 300 -and (Num $fr "pill_white") -gt 30) "keys: pill in the frame"
        $fr0 = Py "frame", $f.FullName, "1.9", (Join-Path $dir "late.png")
        Check ((Num $fr0 "blue") -lt 5) "clicks: ring gone later ($fr0)"
    } else { Check $false "video trim: file saved" }
    StopApp

    # ---- 3. GIF: pause, keys and trim --------------------------------------------------------------
    WriteIni
    $env:CANIVETE_REC_INPUT = "300:K:Ctrl+S;600:L:100,80"
    $env:CANIVETE_REC_PAUSE = "800,1000"
    $env:CANIVETE_REC_TRIM = "200,1400"
    StartApp "--gif-test", "0", "0", "320", "200", "3200"
    $ok = SaveWindow (Join-Path $Out "save_gif_timeline.png")
    Check $ok "gif: save window, timeline, saved"
    $stopped = WaitLog "GIF recording stopped: (\d+) ms" 5
    $ms = if ($stopped -match "stopped: (\d+) ms") { [int]$Matches[1] } else { 0 }
    # 3.2 s of real time minus the 0.15 s start delay and 1 s paused
    Check ($ms -gt 1900 -and $ms -lt 2150) "gif pause: recording $ms ms long (3.2 s with 1 s paused)"
    Check (WaitLog "GIF saved \(.*, 1200 ms\)" 5) "gif trim: 1.2 s saved"
    $g = Newest "*.gif"
    if ($g) {
        Copy-Item $g.FullName (Join-Path $Out "trimmed.gif") -Force
        $r = Py "gif", $g.FullName
        "      $r"
        $gd = Num $r "duration"
        Check ($gd -gt 1.15 -and $gd -lt 1.25) "gif trim: GIF plays $gd s"
    } else { Check $false "gif trim: file saved" }
    StopApp
} finally {
    StopApp
    Remove-Item Env:CANIVETE_REC_INPUT, Env:CANIVETE_REC_TRIM, Env:CANIVETE_REC_PAUSE -ErrorAction SilentlyContinue
}
"$($script:fails) failed"
exit $script:fails
