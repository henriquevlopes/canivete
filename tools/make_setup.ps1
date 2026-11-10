# Builds Release, then compiles installer\canivete.iss into dist\Canivete-Setup-<version>.exe
param([switch]$NoBuild)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$m = Select-String -Path CMakeLists.txt -Pattern 'project\(\s*Canivete\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)'
if (-not $m) { throw 'Version not found in CMakeLists.txt' }
$version = $m.Matches[0].Groups[1].Value

$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
          "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
          "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw 'ISCC.exe not found. Run: winget install --id JRSoftware.InnoSetup -e' }

if (-not $NoBuild) {
    & "C:\Program Files\CMake\bin\cmake.exe" --build build --config Release -- /v:minimal
    if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
}

& $iscc "/DAppVersion=$version" "installer\canivete.iss"
if ($LASTEXITCODE -ne 0) { throw 'ISCC failed' }
Get-Item "dist\Canivete-Setup-$version.exe" | Select-Object FullName, Length
