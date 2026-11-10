# Publishes a new Canivete version: builds the setup, then makes a GitHub release on the public
# henriquevlopes/canivete-releases repo (installer only, no source). Running copies find it there
# (update.cpp) and check the file against the SHA256 line in the release notes, and that line against
# the SIG line (signed here with the offline key, %APPDATA%\Canivete-release\update_key.bin).
#   powershell -ExecutionPolicy Bypass -File tools\release.ps1 [-Notes "What changed"]
# Bump the version in CMakeLists.txt first: copies only offer versions newer than their own.
param([string]$Notes = "", [switch]$NoBuild)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$repo = 'henriquevlopes/canivete-releases'

$m = Select-String -Path CMakeLists.txt -Pattern 'project\(\s*Canivete\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)'
$version = $m.Matches[0].Groups[1].Value
$tag = "v$version"

if ($NoBuild) { & "$PSScriptRoot\make_setup.ps1" -NoBuild } else { & "$PSScriptRoot\make_setup.ps1" }
$setup = "dist\Canivete-Setup-$version.exe"
$hash = (Get-FileHash $setup -Algorithm SHA256).Hash.ToLower()
# Installed copies only offer a version whose "SIG:" line checks out against the key in src/update.cpp.
$sig = & "$PSScriptRoot\sign_update.ps1" -Version $version -Sha256 $hash
if (-not $sig -or $sig.Length -ne 88) { throw 'Signing failed: no update key on this PC? See tools\sign_update.ps1.' }

$ErrorActionPreference = 'Continue'
gh repo view $repo --json name 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) {
    gh repo create $repo --public --add-readme --description "Canivete installers (Windows tray toolbox). Source is private." | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Could not create $repo" }
}
gh release view $tag --repo $repo 2>$null | Out-Null
if ($LASTEXITCODE -eq 0) { throw "$tag is already released. Bump the version in CMakeLists.txt." }

$body = "Canivete $version`n`n"
if ($Notes) { $body += "$Notes`n`n" }
$body += "Download Canivete-Setup-$version.exe and run it. No administrator rights needed.`n`nSHA256: $hash`nSIG: $sig"
$tmp = New-TemporaryFile
[IO.File]::WriteAllText($tmp, $body, (New-Object Text.UTF8Encoding $false))
gh release create $tag $setup --repo $repo --title "Canivete $version" --notes-file $tmp
$rc = $LASTEXITCODE
Remove-Item $tmp
if ($rc -ne 0) { throw 'gh release create failed' }
"Released $tag ($hash)"
