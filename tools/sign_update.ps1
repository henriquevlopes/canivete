# Signs a Canivete release with the private update key (made by tools\make_update_key.ps1).
#   sign_update.ps1 -Version 0.6.0 -Sha256 <hex>   prints the base64 signature for the "SIG:" line
#   sign_update.ps1 -ExportBackup <file>           password-protected copy of the key (for a backup)
#   sign_update.ps1 -ImportBackup <file>           puts a backup back as this user's update_key.bin
# What is signed (UTF-8): "canivete-update|<version>|<sha256 hex, lower case>", ECDSA P-256 with SHA-256,
# signature = r||s (64 bytes). src/update.cpp checks exactly that.
param([string]$Version, [string]$Sha256, [string]$ExportBackup, [string]$ImportBackup, [string]$KeyFile)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Security, System.Core

if (-not $KeyFile) { $KeyFile = Join-Path $env:APPDATA 'Canivete-release\update_key.bin' }
$entropy = [Text.Encoding]::UTF8.GetBytes('canivete-update-key')

function PasswordKey([byte[]]$salt) {
    $s = Read-Host 'Backup password' -AsSecureString
    $b = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($s)
    try { $pw = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($b) } finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($b) }
    $kdf = New-Object Security.Cryptography.Rfc2898DeriveBytes($pw, $salt, 600000, [Security.Cryptography.HashAlgorithmName]::SHA256)
    $kdf.GetBytes(32)
}

if ($ImportBackup) {
    $all = [IO.File]::ReadAllBytes($ImportBackup)
    $salt = [byte[]]$all[0..15]; $iv = [byte[]]$all[16..31]; $ct = [byte[]]$all[32..($all.Length - 1)]
    $aes = [Security.Cryptography.Aes]::Create(); $aes.Key = PasswordKey $salt; $aes.IV = $iv
    $priv = $aes.CreateDecryptor().TransformFinalBlock($ct, 0, $ct.Length)
    New-Item -ItemType Directory -Force (Split-Path $KeyFile) | Out-Null
    [IO.File]::WriteAllBytes($KeyFile, [Security.Cryptography.ProtectedData]::Protect($priv, $entropy, 'CurrentUser'))
    "Key restored to $KeyFile"
    return
}

if (-not (Test-Path $KeyFile)) { throw "No update key at $KeyFile. Restore the backup (-ImportBackup) or, for a brand-new project only, run make_update_key.ps1." }
$priv = [Security.Cryptography.ProtectedData]::Unprotect([IO.File]::ReadAllBytes($KeyFile), $entropy, 'CurrentUser')

if ($ExportBackup) {
    $salt = New-Object byte[] 16; $iv = New-Object byte[] 16
    $rng = [Security.Cryptography.RandomNumberGenerator]::Create(); $rng.GetBytes($salt); $rng.GetBytes($iv)
    $aes = [Security.Cryptography.Aes]::Create(); $aes.Key = PasswordKey $salt; $aes.IV = $iv
    $ct = $aes.CreateEncryptor().TransformFinalBlock($priv, 0, $priv.Length)
    [IO.File]::WriteAllBytes($ExportBackup, [byte[]]($salt + $iv + $ct))
    "Backup written to $ExportBackup (needs the password to restore)."
    return
}

if (-not $Version -or $Sha256 -notmatch '^[0-9a-fA-F]{64}$') { throw 'Usage: sign_update.ps1 -Version <x.y.z> -Sha256 <64 hex>' }
$key = [Security.Cryptography.CngKey]::Import($priv, [Security.Cryptography.CngKeyBlobFormat]::EccPrivateBlob)
[Array]::Clear($priv, 0, $priv.Length)
$ec = New-Object Security.Cryptography.ECDsaCng($key)
$ec.HashAlgorithm = [Security.Cryptography.CngAlgorithm]::Sha256
$msg = [Text.Encoding]::UTF8.GetBytes("canivete-update|$Version|$($Sha256.ToLower())")
$sig = $ec.SignData($msg)   # IEEE P1363: r||s, 64 bytes
$ec.Dispose(); $key.Dispose()
[Convert]::ToBase64String($sig)
