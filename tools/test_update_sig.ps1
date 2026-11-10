# The update signature check (src/update.cpp): a manifest signed with the real update key must pass,
# and any change to the version, the SHA256 or the signature must fail. Signs a made-up version, so
# nothing is published or downloaded. Needs the private key on this PC (tools\make_update_key.ps1).
#   powershell -ExecutionPolicy Bypass -File tools\hidden_test.ps1 "powershell -ExecutionPolicy Bypass -File tools\test_update_sig.ps1"
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$exe = "$root\build\Release\Canivete.exe"
$fails = 0
function Check($ok, $what) { if ($ok) { "  ok    $what" } else { "  FAIL  $what"; $script:fails++ } }
function Verify($v, $sha, $sig) {
    $p = Start-Process $exe -ArgumentList (@('--verify-update', $v, $sha, $sig) | Where-Object { $_ }) -Wait -PassThru
    $p.ExitCode
}

$version = "99.0.1"
$sha = -join ((1..32) | ForEach-Object { '{0:x2}' -f (Get-Random -Maximum 256) })
$sig = & "$PSScriptRoot\sign_update.ps1" -Version $version -Sha256 $sha
Check ($sig.Length -eq 88) "signed with the real key (88 characters of base64)"

Check ((Verify $version $sha $sig) -eq 0) "valid signature accepted"
$sha2 = ($sha.Substring(0, 63) + $(if ($sha[63] -eq 'a') { 'b' } else { 'a' }))
Check ((Verify $version $sha2 $sig) -eq 1) "tampered SHA256 rejected"
Check ((Verify "99.0.2" $sha $sig) -eq 1) "tampered version rejected"
$bytes = [Convert]::FromBase64String($sig); $bytes[10] = $bytes[10] -bxor 1
Check ((Verify $version $sha ([Convert]::ToBase64String($bytes))) -eq 1) "tampered signature rejected"
Check ((Verify $version $sha "not-a-signature") -eq 1) "garbage signature rejected"
Check ((Verify $version $sha "") -ne 0) "missing signature rejected"
$other = & "$PSScriptRoot\sign_update.ps1" -Version "99.0.2" -Sha256 $sha
Check ((Verify $version $sha $other) -eq 1) "signature of another release rejected"

if ($fails) { "FAIL ($fails)"; exit 1 } else { "PASS"; exit 0 }
