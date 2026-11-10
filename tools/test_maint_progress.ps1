# Computer maintenance jobs with simulated work (CANIVETE_MAINT_FAKE=1: nothing is installed or deleted,
# no Windows prompt): the window must stay visible and answer while a cleanup and an update job run,
# tabs must switch mid-job, and the progress panel must end with a result line. Saves window grabs
# in test_out\. Run through the hidden desktop:
#   powershell -ExecutionPolicy Bypass -File tools\hidden_test.ps1 "powershell -ExecutionPolicy Bypass -File tools\test_maint_progress.ps1"
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$exe = "$root\build\Release\Canivete.exe"
$outDir = "$root\test_out"
New-Item -ItemType Directory -Force $outDir | Out-Null
Add-Type -ReferencedAssemblies System.Drawing @"
using System; using System.Text; using System.Runtime.InteropServices; using System.Drawing; using System.Drawing.Imaging;
public static class M {
  [StructLayout(LayoutKind.Sequential)] public struct R { public int l, t, r, b; }
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, int m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, int m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr h, int m, IntPtr w, IntPtr l, int f, int ms, out IntPtr r);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, int f);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, int f);
  public static string Text(IntPtr h) { var s = new StringBuilder(1024); GetWindowText(h, s, 1024); return s.ToString(); }
  public static int Ping(IntPtr h) {  // ms for the window to answer WM_NULL; -1 = no answer within 2 s
    var sw = System.Diagnostics.Stopwatch.StartNew(); IntPtr r;
    return SendMessageTimeout(h, 0, IntPtr.Zero, IntPtr.Zero, 2, 2000, out r) == IntPtr.Zero ? -1 : (int)sw.ElapsedMilliseconds; }
  public static void Grab(IntPtr h, string path) {
    R r; GetWindowRect(h, out r);
    using (var b = new Bitmap(r.r - r.l, r.b - r.t)) {
      using (var g = Graphics.FromImage(b)) { IntPtr dc = g.GetHdc(); if (!PrintWindow(h, dc, 2)) PrintWindow(h, dc, 0); g.ReleaseHdc(dc); }
      b.Save(path, ImageFormat.Png); } }
}
"@
$env:CANIVETE_MAINT_FAKE = "1"
$fails = 0
function Check($ok, $what) { if ($ok) { "  ok    $what" } else { "  FAIL  $what"; $script:fails++ } }
function Ctl($id) { [M]::GetDlgItem($script:h, $id) }
function Click($id) { [void][M]::PostMessage($script:h, 0x111, [IntPtr]$id, (Ctl $id)) }  # WM_COMMAND, BN_CLICKED
function Tab($i) {
  [void][M]::SendMessage((Ctl 100), 0x186, [IntPtr]$i, [IntPtr]::Zero)               # LB_SETCURSEL
  [void][M]::SendMessage($script:h, 0x111, [IntPtr](100 + (1 -shl 16)), (Ctl 100))   # LBN_SELCHANGE
}
function WaitFor($cond, $sec) { $t = [DateTime]::Now.AddSeconds($sec); while (-not (& $cond)) { if ([DateTime]::Now -gt $t) { return $false }; Start-Sleep -Milliseconds 100 }; $true }
function Alive($label) {
  $ms = [M]::Ping($script:h)
  Check ([M]::IsWindowVisible($script:h) -and -not [M]::IsIconic($script:h)) "$label : window visible, not minimized"
  Check ($ms -ge 0 -and $ms -lt 250) "$label : window answers ($ms ms)"
}
# Answer a confirmation box if one comes up (emptying the Recycle Bin): simulated anyway.
function Confirm { $d = [M]::FindWindow("#32770", "Canivete Maintenance"); if ($d -ne [IntPtr]::Zero) { [void][M]::PostMessage($d, 0x111, [IntPtr]1, [IntPtr]::Zero) } }

Start-Process $exe
Start-Sleep -Seconds 2
& $exe --maintenance
$script:h = [IntPtr]::Zero
[void](WaitFor { $script:h = [M]::FindWindow("CaniveteMaintenance", [NullString]::Value); $script:h -ne [IntPtr]::Zero } 10)
if ($h -eq [IntPtr]::Zero) { "FAIL: Maintenance window not found"; exit 1 }
[void][M]::SetWindowPos($h, [IntPtr]::Zero, 40, 40, 1100, 720, 0x14)

"Cleanup job"
Tab 3
Check (WaitFor { [M]::IsWindowEnabled((Ctl 152)) } 20) "cleanup measured, Clean selected available"
Click 152
Start-Sleep -Milliseconds 400; Confirm
Check (WaitFor { (Ctl 200) -ne [IntPtr]::Zero -and [M]::IsWindowVisible((Ctl 200)) -and ([M]::Text((Ctl 200)) -match "of") } 10) "progress panel shows"
Start-Sleep -Milliseconds 1500
Alive "mid-cleanup"
Check (-not [M]::IsWindowEnabled((Ctl 152))) "Clean selected is off while it runs"
"  line: " + [M]::Text((Ctl 200))
[M]::Grab($h, "$outDir\cleanup_mid.png")
Tab 5
Check ([M]::Text((Ctl 101)) -eq "Health") "tab switch mid-job (Health)"
Alive "on another tab"
Tab 3
Check ([M]::Text((Ctl 101)) -eq "Cleanup") "back on Cleanup"
Check (WaitFor { [M]::Text((Ctl 202)) -eq "Close" } 60) "cleanup finished"
Start-Sleep -Milliseconds 300
"  result: " + [M]::Text((Ctl 200))
Check ([M]::Text((Ctl 200)) -match "^Freed ") "result says what was freed"
[M]::Grab($h, "$outDir\cleanup_done.png")
Click 202
Start-Sleep -Milliseconds 300
Check (-not [M]::IsWindowVisible((Ctl 200))) "Close hides the panel"

"Update job"
Tab 1
Check (WaitFor { [M]::IsWindowEnabled((Ctl 137)) } 30) "updates checked, Install selected available"
Click 137
Check (WaitFor { [M]::Text((Ctl 200)) -match "of \d" } 15) "progress panel shows steps"
Start-Sleep -Seconds 4
Alive "mid-update"
Check (-not [M]::IsWindowEnabled((Ctl 137))) "Install selected is off while it runs"
"  line: " + [M]::Text((Ctl 200))
[M]::Grab($h, "$outDir\update_mid.png")
Tab 0
Check ([M]::Text((Ctl 101)) -eq "Overview") "tab switch mid-job (Overview)"
Alive "on Overview"
Tab 3
Click 152   # a second job must not start
Start-Sleep -Milliseconds 300
Check ([M]::Text((Ctl 123)) -match "Wait for the job") "second job refused: $([M]::Text((Ctl 123)))"
Tab 1
Check (WaitFor { [M]::Text((Ctl 202)) -eq "Close" } 90) "update job finished"
Start-Sleep -Milliseconds 300
"  result: " + [M]::Text((Ctl 200))
Check ([M]::Text((Ctl 200)) -match "updated") "result line counts the updates"
[M]::Grab($h, "$outDir\update_done.png")
Click 202
Start-Sleep -Milliseconds 300

"Update job, stopped with Cancel"
Check (WaitFor { [M]::IsWindowEnabled((Ctl 137)) } 30) "updates checked again"
Click 137
Check (WaitFor { [M]::Text((Ctl 200)) -match "of \d" } 15) "running"
Start-Sleep -Seconds 3
Click 202
Start-Sleep -Milliseconds 500
"  line: " + [M]::Text((Ctl 200))
[M]::Grab($h, "$outDir\update_stopping.png")
Check (WaitFor { [M]::Text((Ctl 202)) -eq "Close" } 30) "stopped after the current item"
Start-Sleep -Milliseconds 300
"  result: " + [M]::Text((Ctl 200))
Check ([M]::Text((Ctl 200)) -match "^Stopped") "result says it was stopped"
[M]::Grab($h, "$outDir\update_stopped.png")

Get-Process Canivete -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe } | Stop-Process -Force
if ($fails) { "FAIL ($fails)"; exit 1 } else { "PASS"; exit 0 }
