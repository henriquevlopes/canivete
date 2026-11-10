# Makes the key pair that signs Canivete updates (run ONCE, on the release PC).
# The private key goes to %APPDATA%\Canivete-release\update_key.bin, encrypted for this Windows user
# (DPAPI), never into the repo. The public key is printed as C++ to paste into src/update.cpp
# (kUpdatePublicKey). Copies of Canivete only install updates signed with this key, so:
#   BACK UP update_key.bin together with this Windows account's DPAPI keys (or re-export it as below),
#   or no new version can ever be offered to installed copies again.
#   powershell -ExecutionPolicy Bypass -File tools\make_update_key.ps1 [-Force]
param([switch]$Force)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Security, System.Core

$dir = Join-Path $env:APPDATA 'Canivete-release'
$file = Join-Path $dir 'update_key.bin'
if ((Test-Path $file) -and -not $Force) {
    throw "$file already exists. Installed copies trust that key: replacing it means they never update again. Use -Force only if you really mean it."
}
New-Item -ItemType Directory -Force $dir | Out-Null

$p = New-Object Security.Cryptography.CngKeyCreationParameters
$p.ExportPolicy = [Security.Cryptography.CngExportPolicies]::AllowPlaintextExport
$key = [Security.Cryptography.CngKey]::Create([Security.Cryptography.CngAlgorithm]::ECDsaP256, $null, $p)
$priv = $key.Export([Security.Cryptography.CngKeyBlobFormat]::EccPrivateBlob)   # BCRYPT_ECCPRIVATE_BLOB
$pub = $key.Export([Security.Cryptography.CngKeyBlobFormat]::EccPublicBlob)     # BCRYPT_ECCPUBLIC_BLOB, 72 bytes
$key.Dispose()

$enc = [Security.Cryptography.ProtectedData]::Protect($priv, [Text.Encoding]::UTF8.GetBytes('canivete-update-key'),
    [Security.Cryptography.DataProtectionScope]::CurrentUser)
[Array]::Clear($priv, 0, $priv.Length)
[IO.File]::WriteAllBytes($file, $enc)

$hex = ($pub | ForEach-Object { '0x{0:X2}' -f $_ })
$lines = for ($i = 0; $i -lt $hex.Count; $i += 12) { '    ' + (($hex[$i..([Math]::Min($i + 11, $hex.Count - 1))]) -join ', ') + ',' }
"Private key saved (encrypted for $env:USERNAME): $file"
''
'Public key for src/update.cpp:'
'constexpr BYTE kUpdatePublicKey[72] = {'
$lines
'};'
''
'REMINDER: back up the private key. On this PC, as this user, you can make a password-protected copy with:'
'  powershell -ExecutionPolicy Bypass -File tools\sign_update.ps1 -ExportBackup <file> (asks for a password)'
'Keep that copy offline (USB stick, password manager). Without the key no update can be released.'
