# Build Release and install Canivete to %LOCALAPPDATA%\Programs\Canivete, then start it.
# Usage:  powershell -ExecutionPolicy Bypass -File tools\deploy.ps1
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$cmake = "C:\Program Files\CMake\bin\cmake.exe"
$dest = Join-Path $env:LOCALAPPDATA "Programs\Canivete"

if (-not (Test-Path "$root\build\Canivete.sln") -and -not (Test-Path "$root\build\Canivete.slnx")) {
    & $cmake -S $root -B "$root\build" -G "Visual Studio 18 2026" -A x64 | Out-Null
}
& $cmake --build "$root\build" --config Release -- /v:minimal
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

Get-Process Canivete -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item "$root\build\Release\Canivete.exe" $dest -Force
Start-Process "$dest\Canivete.exe"
"Installed and started: $dest\Canivete.exe ({0:N0} KB)" -f ((Get-Item "$dest\Canivete.exe").Length / 1KB)
