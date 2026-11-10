# UI test of the editor's Crop, Blur and Store-page frame. Run it on the hidden desktop, so no window shows up:
#   powershell -ExecutionPolicy Bypass -File tools\hidden_test.ps1 "powershell -ExecutionPolicy Bypass -File tools\test_editor.ps1 -Out C:\path\test_out"
# It opens a made-up picture with --edit in a test copy, drives it with mouse and key messages (and the
# test-mode WM_COPYDATA commands in editor.cpp), saves the results as PNGs into -Out and prints PASS/FAIL
# lines; the exit code is the number of failures.
param([string]$Exe = (Join-Path $PSScriptRoot "..\build\Release\Canivete.exe"),
      [string]$Out = (Join-Path $PSScriptRoot "..\test_out"), [string]$Theme = "light")

$ErrorActionPreference = "Stop"
$dir = $env:CANIVETE_TEST
if (-not $dir) { "CANIVETE_TEST is not set: run this through hidden_test.ps1"; exit 1 }
$Exe = (Resolve-Path $Exe).Path
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$log = Join-Path $dir "logs\canivete.log"
$script:fails = 0

Add-Type -ReferencedAssemblies System.Drawing @"
using System; using System.Runtime.InteropServices; using System.Drawing; using System.Drawing.Imaging;
public static class W {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int l, t, r, b; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [StructLayout(LayoutKind.Sequential)] public struct CDS { public IntPtr data; public int size; public IntPtr ptr; }
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, ref CDS l);
  public static void Command(IntPtr h, string text) {
    var cds = new CDS(); cds.data = (IntPtr)0x45444954; cds.size = text.Length * 2;
    cds.ptr = Marshal.StringToHGlobalUni(text);
    try { SendMessage(h, 0x004A, IntPtr.Zero, ref cds); } finally { Marshal.FreeHGlobal(cds.ptr); }
  }
  public static IntPtr XY(int x, int y) { return (IntPtr)((y << 16) | (x & 0xFFFF)); }
  public static void Capture(IntPtr h, string file) {
    RECT r; GetWindowRect(h, out r);
    using (var b = new Bitmap(r.r - r.l, r.b - r.t)) {
      using (var g = Graphics.FromImage(b)) { IntPtr dc = g.GetHdc(); PrintWindow(h, dc, 2); g.ReleaseHdc(dc); }
      b.Save(file, ImageFormat.Png);
    }
  }
  // Dark pixels in a box (text is dark; blurred text has far fewer of them).
  public static int Dark(string file, int x0, int y0, int x1, int y1) {
    using (var b = new Bitmap(file)) {
      int n = 0;
      for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) { var c = b.GetPixel(x, y); if (c.R + c.G + c.B < 200) n++; }
      return n;
    }
  }
  public static string Info(string file) {
    using (var b = new Bitmap(file)) {
      var c = b.GetPixel(0, 0); var m = b.GetPixel(b.Width / 2, b.Height / 2);
      return b.Width + "x" + b.Height + " corner=" + c.A + "," + c.R + "," + c.G + "," + c.B + " middle=" + m.A + "," + m.R + "," + m.G + "," + m.B;
    }
  }
  public static void MakePicture(string file) {
    using (var b = new Bitmap(1200, 700)) {
      using (var g = Graphics.FromImage(b)) {
        g.Clear(Color.White);
        g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.ClearTypeGridFit;
        g.FillRectangle(new SolidBrush(Color.FromArgb(32, 33, 36)), 0, 0, 1200, 56);
        using (var f = new Font("Segoe UI", 16, FontStyle.Bold)) g.DrawString("Canivete test page", f, Brushes.White, 20, 12);
        using (var f = new Font("Segoe UI", 14)) {
          g.DrawString("Account: henri.test@example.com   Password: hunter2-secret", f, Brushes.Black, 60, 120);
          g.DrawString("Card number 4111 1111 1111 1111   expires 12/30", f, Brushes.Black, 60, 160);
          for (int i = 0; i < 6; i++)
            g.DrawString("Line " + (i + 1) + ": the quick brown fox jumps over the lazy dog.", f, Brushes.DimGray, 60, 230 + i * 34);
        }
        g.FillRectangle(new SolidBrush(Color.FromArgb(0, 120, 215)), 760, 240, 340, 180);
        g.FillEllipse(new SolidBrush(Color.FromArgb(232, 44, 56)), 820, 470, 160, 160);
      }
      b.Save(file, ImageFormat.Png);
    }
  }
}
"@

function Check($ok, $what) {
    if ($ok) { "PASS  $what" } else { "FAIL  $what"; $script:fails++ }
}
function LogLines { if (Test-Path $log) { @(Get-Content $log -Encoding UTF8) } else { @() } }
function Info {
    [W]::Command($script:h, "info")
    $line = LogLines | Where-Object { $_ -match "Editor test: image" } | Select-Object -Last 1
    if ($line -notmatch "image (\d+) x (\d+), canvas (\d+) x (\d+), shapes (\d+), undo (\d+), zoom ([\d.]+), origin (-?\d+) (-?\d+)") {
        return $null
    }
    [pscustomobject]@{ W = [int]$Matches[1]; H = [int]$Matches[2]; CW = [int]$Matches[3]; CH = [int]$Matches[4]
                       Shapes = [int]$Matches[5]; Undo = [int]$Matches[6]; Zoom = [double]$Matches[7]
                       X = [int]$Matches[8]; Y = [int]$Matches[9] }
}
# Image pixel -> window client point.
function Pt($i, $ix, $iy) { [W]::XY([int][math]::Round($i.X + $ix * $i.Zoom), [int][math]::Round($i.Y + $iy * $i.Zoom)) }
function Key($vk) {
    [W]::SendMessage($script:h, 0x0100, [IntPtr]$vk, [IntPtr]1) | Out-Null   # WM_KEYDOWN
    [W]::SendMessage($script:h, 0x0101, [IntPtr]$vk, [IntPtr]0xC0000001) | Out-Null  # WM_KEYUP
}
function Drag($i, $x0, $y0, $x1, $y1, [int]$keys = 0) {
    [W]::SendMessage($script:h, 0x0201, [IntPtr](1 -bor $keys), (Pt $i $x0 $y0)) | Out-Null  # WM_LBUTTONDOWN
    foreach ($t in 1..5) {
        $x = $x0 + ($x1 - $x0) * $t / 5; $y = $y0 + ($y1 - $y0) * $t / 5
        [W]::SendMessage($script:h, 0x0200, [IntPtr](1 -bor $keys), (Pt $i $x $y)) | Out-Null  # WM_MOUSEMOVE
    }
    [W]::SendMessage($script:h, 0x0202, [IntPtr]$keys, (Pt $i $x1 $y1)) | Out-Null  # WM_LBUTTONUP
}
function Ini($key) {
    $m = Get-Content (Join-Path $dir "settings.ini") | Where-Object { $_ -match "^$key=" } | Select-Object -First 1
    if ($m) { $m.Split("=")[1] } else { "" }
}

Get-ChildItem $dir -Exclude hidden_test_output.txt -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force
$ini = "[General]`r`nCheckUpdates=0`r`nTheme=$Theme`r`n[Maintenance]`r`nDailyCheck=0`r`n[Screenshot]`r`nFolder=$dir\shots`r`n"
[IO.File]::WriteAllText((Join-Path $dir "settings.ini"), $ini)
$pic = Join-Path $dir "test picture.png"
[W]::MakePicture($pic)
Copy-Item $pic (Join-Path $Out "input.png") -Force

$app = Start-Process $Exe -ArgumentList "--edit", "`"$pic`"" -PassThru
$script:h = [IntPtr]::Zero
foreach ($n in 1..75) {
    $script:h = [W]::FindWindow("CaniveteEditor", [NullString]::Value)
    if ($script:h -ne [IntPtr]::Zero -and [W]::IsWindowVisible($script:h)) { break }
    Start-Sleep -Milliseconds 200
}
if ($script:h -eq [IntPtr]::Zero) { "FAIL  editor window did not open"; exit 1 }
Start-Sleep -Milliseconds 500
try {
    $i = Info
    Check ($i -and $i.W -eq 1200 -and $i.H -eq 700 -and $i.CW -eq 1200) "editor opened the 1200 x 700 picture, frame off"

    # ---- Blur (G), drawn with the mouse over the password line -------------------------------
    Key 0x47
    Drag $i 50 110 700 200
    $i = Info
    Check ($i.Shapes -eq 1) "blur: one shape drawn with the mouse"
    [W]::Command($script:h, "saveas $Out\blur.png")
    $before = [W]::Dark($pic, 60, 115, 700, 195)
    $after = [W]::Dark("$Out\blur.png", 60, 115, 700, 195)
    Check ($before -gt 300 -and $after -lt $before / 10) "blur: text under it unreadable (dark pixels $before -> $after)"
    $untouched = [W]::Dark("$Out\blur.png", 60, 230, 700, 430)
    Check ($untouched -eq [W]::Dark($pic, 60, 230, 700, 430)) "blur: text outside the box unchanged"
    Key 0xDD   # ] = bigger size: the selected blur gets stronger
    $i = Info
    Check ($i.Undo -eq 2) "blur: size change is an undo step"

    # ---- Crop (C), mouse drag + Enter ---------------------------------------------------------
    Key 0x43
    Drag $i 40 100 840 620
    Key 0x0D
    $i = Info
    Check ($i.W -eq 800 -and $i.H -eq 520) "crop: mouse box + Enter gives 800 x 520 (got $($i.W) x $($i.H))"
    [W]::Command($script:h, "saveas $Out\crop.png")
    $moved = [W]::Dark("$Out\crop.png", 20, 15, 660, 95)
    Check ($moved -lt $before / 10) "crop: the blur moved with the picture ($moved dark pixels where the password was)"
    Check ((LogLines | Where-Object { $_ -match "cropped to 800 x 520 at 40, 100" }).Count -ge 1) "crop: logged"
    [W]::Command($script:h, "undo")
    $i = Info
    Check ($i.W -eq 1200 -and $i.H -eq 700 -and $i.Shapes -eq 1) "crop: Ctrl+Z brings the whole picture back with its shape"
    [W]::Command($script:h, "saveas $Out\crop_undone.png")
    Check ([W]::Dark("$Out\crop_undone.png", 60, 115, 700, 195) -lt $before / 10) "crop: undone shape is back in place"
    [W]::Command($script:h, "redo")
    $i = Info
    Check ($i.W -eq 800 -and $i.H -eq 520) "crop: redo crops again"
    # Shift keeps the shape: a box from 0,0 with Shift on a 800 x 520 picture keeps 800:520.
    Key 0x43
    Drag $i 0 0 400 100 4
    [W]::Command($script:h, "info")
    Key 0x1B   # Esc removes the box
    Key 0x1B   # Esc again: back to the last tool
    $i = Info
    Check ($i.W -eq 800) "crop: Esc cancels without cropping"

    # ---- Store-page frame ---------------------------------------------------------------------
    [W]::Command($script:h, "frame on`nbg gradient`npadding 8`ncorners 6`nshadow 1`nsoftness 50`nshape 16:9")
    $i = Info
    Check ($i.CW -gt 800 -and [math]::Abs($i.CW / $i.CH - 16 / 9) -lt 0.01) "frame: 16:9 canvas ($($i.CW) x $($i.CH))"
    [W]::Command($script:h, "saveas $Out\frame_gradient.png")
    Check ((Ini "FrameBackground") -eq "gradient" -and (Ini "FrameShape") -eq "16:9") "frame: choices remembered in settings.ini"
    [W]::Command($script:h, "bg dark`nshape original`npadding 6`ncorners 8`nsoftness 70")
    [W]::Command($script:h, "saveas $Out\frame_dark.png")
    [W]::Command($script:h, "bg transparent`nshape 1:1")
    [W]::Command($script:h, "saveas $Out\frame_transparent.png")
    $t = [W]::Info("$Out\frame_transparent.png")
    Check ($t -match "corner=0," -and $t -match "middle=255,") "frame: transparent PNG keeps its alpha ($t)"
    [W]::Command($script:h, "bg white`nshape 4:3")
    [W]::Command($script:h, "saveas $Out\frame_white.png")
    $t = [W]::Info("$Out\frame_white.png")
    Check ($t -match "corner=255,255,255,255") "frame: white background ($t)"

    # Panel open, driven by keys: Tab to Padding, Right = +1.
    [W]::Command($script:h, "bg gradient`nshape 16:9`npadding 8`ncorners 6`nsoftness 50")
    Key 0x46   # F opens the panel (frame already on)
    Key 0x28   # Down: Padding
    Key 0x27   # Right: 9
    Check ((Ini "FramePadding") -eq "9") "frame panel: keyboard changes Padding (settings.ini says $(Ini 'FramePadding'))"
    Key 0x25   # back to 8
    Start-Sleep -Milliseconds 300
    [W]::Capture($script:h, "$Out\editor_frame_panel_$Theme.png")
    Key 0x1B   # Esc leaves the panel
    Key 0x46   # F closes it
    [W]::Command($script:h, "frame off")
    $i = Info
    Check ($i.CW -eq 800) "frame: off gives the plain picture"
    [W]::Command($script:h, "frame on")
} finally {
    if (-not $app.HasExited) { $app.Kill(); $app.WaitForExit() }
}
"--- log ---"
LogLines | Where-Object { $_ -match "Editor" } | Select-Object -Last 40
exit $script:fails
