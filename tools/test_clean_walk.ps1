# Cleanup deletes through handles and leaves links alone: builds a scratch folder with files, a
# read-only file, nested folders and a junction to a "victim" folder outside, empties it with
# --clean-walk-test (test copies only), and checks the victim is untouched. Only touches test_out\.
#   powershell -ExecutionPolicy Bypass -File tools\hidden_test.ps1 "powershell -ExecutionPolicy Bypass -File tools\test_clean_walk.ps1"
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$exe = "$root\build\Release\Canivete.exe"
$base = "$root\test_out\clean_walk"
$fails = 0
function Check($ok, $what) { if ($ok) { "  ok    $what" } else { "  FAIL  $what"; $script:fails++ } }

if (Test-Path $base) { cmd /c rmdir /s /q "`"$base`"" | Out-Null }
$place = "$base\place"; $victim = "$base\victim"
New-Item -ItemType Directory -Force "$place\a\b\c", "$place\empty", $victim | Out-Null
"x" * 1000 | Set-Content "$place\one.tmp"
"y" * 1000 | Set-Content "$place\a\two.tmp"
"z" * 1000 | Set-Content "$place\a\b\c\three.tmp"
"r" | Set-Content "$place\readonly.tmp"; (Get-Item "$place\readonly.tmp").IsReadOnly = $true
"keep me" | Set-Content "$victim\precious.txt"
New-Item -ItemType Directory "$victim\sub" | Out-Null
"keep me too" | Set-Content "$victim\sub\deep.txt"
cmd /c mklink /J "`"$place\link`"" "`"$victim`"" | Out-Null
cmd /c mklink /J "`"$place\a\link2`"" "`"$victim\sub`"" | Out-Null
Check (Test-Path "$place\link\precious.txt") "junction to the victim folder made"

$p = Start-Process $exe -ArgumentList @('--clean-walk-test', "`"$place`"") -Wait -PassThru
Check ($p.ExitCode -eq 0) "walk ran (exit $($p.ExitCode))"
Check (Test-Path $place) "the place itself stays"
Check (-not (Test-Path "$place\one.tmp")) "file deleted"
Check (-not (Test-Path "$place\a\b\c\three.tmp")) "nested file deleted"
Check (-not (Test-Path "$place\readonly.tmp")) "read-only file deleted"
Check (-not (Test-Path "$place\empty")) "empty folder removed"
Check (-not (Test-Path "$place\a\b")) "nested folders removed"
Check (Test-Path "$victim\precious.txt") "file behind the junction untouched"
Check (Test-Path "$victim\sub\deep.txt") "file behind the nested junction untouched"
$log = Get-Content "$env:CANIVETE_TEST\logs\canivete.log" -Tail 20 | Select-String "clean-walk-test" | Select-Object -Last 1
"  log: $log"
Check ($log -match "4 files") "four files deleted"

cmd /c rmdir /s /q "`"$base`"" | Out-Null
if ($fails) { "FAIL ($fails)"; exit 1 } else { "PASS"; exit 0 }
