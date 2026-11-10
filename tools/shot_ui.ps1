# Grabs every Canivete window (Settings tabs, Maintenance, editor, Make alpha, Resize, Rename, the
# capture preview, Remove background, the recording bar and its save window) in one theme, as PNGs,
# to look at the design. Nothing is installed, cleaned or uploaded (test copy, simulated jobs, fake
# grabs). Run it on the hidden desktop:
#   powershell -ExecutionPolicy Bypass -File tools\hidden_test.ps1 "powershell -ExecutionPolicy Bypass -File tools\shot_ui.ps1 -Theme dark -Out test_out\ui\after"
param([string]$Exe = (Join-Path $PSScriptRoot "..\build\Release\Canivete.exe"),
      [string]$Out = (Join-Path $PSScriptRoot "..\test_out\ui"), [string]$Theme = "dark", [string]$Prefix = "")

$ErrorActionPreference = "Stop"
$dir = $env:CANIVETE_TEST
if (-not $dir) { "CANIVETE_TEST is not set: run this through hidden_test.ps1"; exit 1 }
$Exe = (Resolve-Path $Exe).Path
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$tag = if ($Prefix) { "$Prefix" + "_" } else { "" }

Add-Type -ReferencedAssemblies System.Drawing @"
using System; using System.Runtime.InteropServices; using System.Drawing; using System.Drawing.Imaging;
public static class S {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool RedrawWindow(IntPtr h, IntPtr r, IntPtr g, uint f);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int l, t, r, b; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  public static void Capture(IntPtr h, string file) {
    RedrawWindow(h, IntPtr.Zero, IntPtr.Zero, 0x585);  // invalidate, erase, frame, all children, update now
    RECT r; GetWindowRect(h, out r);
    using (var b = new Bitmap(r.r - r.l, r.b - r.t)) {
      using (var g = Graphics.FromImage(b)) { IntPtr dc = g.GetHdc(); if (!PrintWindow(h, dc, 2)) PrintWindow(h, dc, 0); g.ReleaseHdc(dc); }
      b.Save(file, ImageFormat.Png);
    }
  }
  public static void MakePicture(string file, int w, int hgt) {
    using (var b = new Bitmap(w, hgt)) {
      using (var g = Graphics.FromImage(b)) {
        g.Clear(Color.FromArgb(240, 244, 250));
        g.FillRectangle(new SolidBrush(Color.FromArgb(0, 120, 215)), w / 8, hgt / 6, w / 3, hgt / 3);
        g.FillEllipse(new SolidBrush(Color.FromArgb(232, 44, 56)), w / 2, hgt / 3, w / 4, w / 4);
        using (var f = new Font("Segoe UI", 20)) g.DrawString("Canivete test picture", f, Brushes.Black, 30, hgt - 70);
      }
      b.Save(file, ImageFormat.Png);
    }
  }
}
"@

$script:app = $null
function StopAll {
    Get-Process Canivete -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $Exe } | Stop-Process -Force
    Start-Sleep -Milliseconds 300
}
function WriteIni([string]$extra = "") {
    $ini = "[General]`r`nCheckUpdates=0`r`nTheme=$Theme`r`n[Maintenance]`r`nDailyCheck=0`r`n" +
           "[Screenshot]`r`nFolder=$dir\shots`r`nOpenEditor=0`r`nShowNotification=1`r`nCopyToClipboard=1`r`n$extra"
    [IO.File]::WriteAllText((Join-Path $dir "settings.ini"), $ini)
}
function Window($class, $title = $null, [int]$sec = 15) {
    $end = (Get-Date).AddSeconds($sec)
    do {
        $t = if ($title) { $title } else { [NullString]::Value }
        $c = if ($class) { $class } else { [NullString]::Value }
        $h = [S]::FindWindow($c, $t)
        if ($h -ne [IntPtr]::Zero -and [S]::IsWindowVisible($h)) { return $h }
        Start-Sleep -Milliseconds 100
    } while ((Get-Date) -lt $end)
    return [IntPtr]::Zero
}
function Shot($h, $name) {
    if ($h -eq [IntPtr]::Zero) { "MISSING  $name"; return }
    $f = Join-Path $Out "$tag$name`_$Theme.png"
    [S]::Capture($h, $f)
    "saved    $f"
}

Get-ChildItem $dir -Exclude hidden_test_output.txt -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force
New-Item -ItemType Directory -Force (Join-Path $dir "shots") | Out-Null
$pic = Join-Path $dir "picture.png"
[S]::MakePicture($pic, 1200, 700)
$pic2 = Join-Path $dir "second picture.png"
[S]::MakePicture($pic2, 800, 600)
$env:CANIVETE_GRAB_FAKE = "1"
$env:CANIVETE_MAINT_FAKE = "1"
$env:CANIVETE_GIF_FAKE = "1"
$env:CANIVETE_RMBG_FAKE = "1"

try {
    WriteIni
    # ---- Settings: a few tabs --------------------------------------------------------------------
    Start-Process $Exe
    Start-Sleep -Seconds 2
    & $Exe --settings
    $h = Window $null "Canivete Settings"
    if ($h -ne [IntPtr]::Zero) {
        $nav = [S]::GetDlgItem($h, 255)  # IDC_NAV
        foreach ($t in @(@(0, "general"), @(1, "screenshots"), @(5, "uploads"), @(8, "maintenance"))) {
            [void][S]::SendMessage($nav, 0x186, [IntPtr]$t[0], [IntPtr]::Zero)                  # LB_SETCURSEL
            [void][S]::SendMessage($h, 0x111, [IntPtr](255 + (1 -shl 16)), $nav)                 # LBN_SELCHANGE
            Start-Sleep -Milliseconds 300
            Shot $h ("settings_" + $t[1])
        }
        [void][S]::PostMessage($h, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)
    } else { "MISSING  settings" }

    # ---- Maintenance -----------------------------------------------------------------------------
    & $Exe --maintenance
    $h = Window "CaniveteMaintenance" $null 15
    if ($h -ne [IntPtr]::Zero) {
        Start-Sleep -Seconds 4   # simulated checks fill the lists
        $nav = [S]::GetDlgItem($h, 100)
        for ($i = 0; $i -lt 6; $i++) {
            [void][S]::SendMessage($nav, 0x186, [IntPtr]$i, [IntPtr]::Zero)
            [void][S]::SendMessage($h, 0x111, [IntPtr](100 + (1 -shl 16)), $nav)
            Start-Sleep -Milliseconds 400
            Shot $h ("maint_tab$i")
        }
    }
    StopAll

    # ---- capture preview (toast) -----------------------------------------------------------------
    Start-Process $Exe
    Start-Sleep -Seconds 2
    & $Exe --full
    $h = Window "CaniveteToast" $null 10
    Start-Sleep -Milliseconds 250
    Shot $h "toast"
    StopAll

    # ---- editor, Make alpha ----------------------------------------------------------------------
    Start-Process $Exe -ArgumentList "--edit", "`"$pic`""
    $h = Window $null "Canivete Editor" 15
    Start-Sleep -Milliseconds 800
    Shot $h "editor"
    StopAll
    Start-Process $Exe -ArgumentList "--alpha", "`"$pic`""
    $h = Window $null "Make alpha" 15
    Start-Sleep -Milliseconds 800
    Shot $h "alpha"
    StopAll

    # ---- Resize, Rename --------------------------------------------------------------------------
    Start-Process $Exe -ArgumentList "--resize", "`"$pic`"", "`"$pic2`""
    $h = Window "CaniveteResizer" $null 15
    Start-Sleep -Milliseconds 800
    Shot $h "resizer"
    StopAll
    Start-Process $Exe -ArgumentList "--rename", "`"$pic`"", "`"$pic2`""
    $h = Window "CaniveteRenamer" $null 15
    Start-Sleep -Milliseconds 800
    Shot $h "renamer"
    StopAll

    # ---- Remove background (progress window only, simulated) -------------------------------------
    Start-Process $Exe
    Start-Sleep -Seconds 2
    & $Exe --do nobg "$pic"
    $h = Window "CaniveteRemoveBackground" $null 10
    Start-Sleep -Milliseconds 600
    Shot $h "rmbg"
    StopAll

    # ---- recording bar and save window -----------------------------------------------------------
    Start-Process $Exe -ArgumentList "--gif-test", "200", "200", "320", "200", "2500"
    $h = Window "CaniveteGifBar" $null 10
    Start-Sleep -Milliseconds 700
    Shot $h "recorder_bar"
    $s = Window "CaniveteGifSave" $null 40
    if ($s -ne [IntPtr]::Zero) {
        $end = (Get-Date).AddSeconds(60)
        while ((Get-Date) -lt $end) {
            if (@(110, 111, 112 | Where-Object { [S]::IsWindowVisible([S]::GetDlgItem($s, $_)) }).Count -eq 3) { break }
            Start-Sleep -Milliseconds 300
        }
        Start-Sleep -Seconds 3
    }
    Shot $s "recorder_save"
    StopAll
} finally {
    StopAll
}
