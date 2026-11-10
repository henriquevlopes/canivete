# Runs a test command on a hidden Windows desktop, so no window shows up on screen while you work.
# Canivete copies started from it run in test mode (CANIVETE_TEST): their own settings and log in
# %TEMP%\CaniveteTest, no tray icon, hotkeys or Explorer entries, and they don't talk to the real copy.
# Windows on that desktop can still be found, sent messages and captured with PrintWindow.
#
#   powershell -ExecutionPolicy Bypass -File tools\hidden_test.ps1 "powershell -ExecutionPolicy Bypass -File test.ps1 -X 1"
param([Parameter(Mandatory)][string]$Command, [int]$TimeoutSec = 300)

Add-Type @"
using System; using System.Runtime.InteropServices;
public static class HiddenDesk {
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] public struct SI {
    public int cb; public string r, desktop, title; public int x, y, w, h, cx, cy, fill, flags; public short show, r2;
    public IntPtr r3, hin, hout, herr; }
  [StructLayout(LayoutKind.Sequential)] public struct PI { public IntPtr hProcess, hThread; public int pid, tid; }
  [DllImport("user32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  public static extern IntPtr CreateDesktop(string name, IntPtr dev, IntPtr mode, int flags, uint access, IntPtr sa);
  [DllImport("user32.dll")] public static extern bool CloseDesktop(IntPtr d);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  public static extern bool CreateProcess(string app, string cmd, IntPtr pa, IntPtr ta, bool inherit, uint flags,
                                          IntPtr env, string dir, ref SI si, out PI pi);
  [DllImport("kernel32.dll")] public static extern uint WaitForSingleObject(IntPtr h, uint ms);
  [DllImport("kernel32.dll")] public static extern bool GetExitCodeProcess(IntPtr h, out uint code);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
}
"@

$testDir = Join-Path $env:TEMP "CaniveteTest"
New-Item -ItemType Directory -Force $testDir | Out-Null
$env:CANIVETE_TEST = $testDir
$out = Join-Path $testDir "hidden_test_output.txt"
Remove-Item $out -ErrorAction SilentlyContinue

$before = @(Get-Process Canivete -ErrorAction SilentlyContinue | ForEach-Object Id)
$desk = [HiddenDesk]::CreateDesktop("CaniveteTest", [IntPtr]::Zero, [IntPtr]::Zero, 0, 0x01FF, [IntPtr]::Zero)
if ($desk -eq [IntPtr]::Zero) { "CreateDesktop failed"; exit 1 }
$si = New-Object HiddenDesk+SI
$si.cb = [Runtime.InteropServices.Marshal]::SizeOf($si)
$si.desktop = "WinSta0\CaniveteTest"
$pi = New-Object HiddenDesk+PI
$cmdLine = "cmd.exe /s /c `"($Command) > `"$out`" 2>&1`""
if (-not [HiddenDesk]::CreateProcess([NullString]::Value, $cmdLine, [IntPtr]::Zero, [IntPtr]::Zero, $false, 0x08000000,
                                     [IntPtr]::Zero, (Get-Location).Path, [ref]$si, [ref]$pi)) {
  "CreateProcess failed: " + [Runtime.InteropServices.Marshal]::GetLastWin32Error(); exit 1
}
$timedOut = [HiddenDesk]::WaitForSingleObject($pi.hProcess, [uint32]($TimeoutSec * 1000)) -ne 0
$code = 0; [HiddenDesk]::GetExitCodeProcess($pi.hProcess, [ref]$code) | Out-Null
[HiddenDesk]::CloseHandle($pi.hProcess) | Out-Null; [HiddenDesk]::CloseHandle($pi.hThread) | Out-Null

# Close test copies of Canivete left running (never the real one, which was running before).
Get-Process Canivete -ErrorAction SilentlyContinue | Where-Object { $before -notcontains $_.Id } | Stop-Process -Force
[HiddenDesk]::CloseDesktop($desk) | Out-Null

if (Test-Path $out) { Get-Content $out }
if ($timedOut) { "TIMED OUT after $TimeoutSec s"; exit 1 }
exit $code
