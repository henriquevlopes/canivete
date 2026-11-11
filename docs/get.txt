# Installs the latest Canivete. Run in PowerShell:
#   irm https://raw.githubusercontent.com/henriquevlopes/canivete/main/install.ps1 | iex
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$release = Invoke-RestMethod 'https://api.github.com/repos/henriquevlopes/canivete/releases/latest'
$asset = $release.assets | Where-Object name -like 'Canivete-Setup-*.exe' | Select-Object -First 1
if (-not $asset) { throw 'No installer found in the latest release.' }
$file = Join-Path $env:TEMP $asset.name
Write-Host "Downloading Canivete $($release.tag_name)..."
Invoke-WebRequest $asset.browser_download_url -OutFile $file
Write-Host 'Installing...'
Start-Process $file -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES' -Wait
Remove-Item $file -ErrorAction SilentlyContinue
Write-Host 'Canivete is installed and running (look for its tray icon).'
