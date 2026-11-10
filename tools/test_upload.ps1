# End-to-end test of uploads (Google Drive and Amazon S3) against the fake server in upload_mock.py.
# Run it on the hidden desktop, so no window shows up:
#   powershell -ExecutionPolicy Bypass -File tools\hidden_test.ps1 "powershell -ExecutionPolicy Bypass -File tools\test_upload.ps1"
# It starts its own test copies of Canivete (CANIVETE_TEST must be set, as hidden_test.ps1 does) and
# prints PASS/FAIL lines; the exit code is the number of failures.
param([string]$Exe = (Join-Path $PSScriptRoot "..\build\Release\Canivete.exe"), [int]$Port = 18765)

$ErrorActionPreference = "Stop"
$dir = $env:CANIVETE_TEST
if (-not $dir) { "CANIVETE_TEST is not set: run this through hidden_test.ps1"; exit 1 }
$Exe = (Resolve-Path $Exe).Path
$log = Join-Path $dir "canivete.log"
$mock = "http://127.0.0.1:$Port"
$env:CANIVETE_UPLOAD_MOCK = $mock
$env:CANIVETE_GIF_FAKE = "1"
$env:CANIVETE_GRAB_FAKE = "1"   # the hidden desktop has no screen to grab
$secret = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"
$script:fails = 0

Add-Type -ReferencedAssemblies System.Drawing @"
using System; using System.Runtime.InteropServices; using System.Drawing; using System.Drawing.Imaging;
public static class W {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool RedrawWindow(IntPtr h, IntPtr r, IntPtr g, uint f);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int l, t, r, b; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  public static void Capture(IntPtr h, string file) {
    RECT r; GetWindowRect(h, out r);
    using (var b = new Bitmap(r.r - r.l, r.b - r.t)) {
      using (var g = Graphics.FromImage(b)) { IntPtr dc = g.GetHdc(); PrintWindow(h, dc, 2); g.ReleaseHdc(dc); }
      b.Save(file, ImageFormat.Png);
    }
  }
}
"@

function Check($ok, $what) {
    if ($ok) { "PASS  $what" } else { "FAIL  $what"; $script:fails++ }
}
function LogLines { if (Test-Path $log) { @(Get-Content $log -Encoding UTF8) } else { @() } }
$script:mark = 0
function Mark { $script:mark = (LogLines).Count }
# Waits for a log line (regex) written since the last Mark; returns it or $null.
function WaitLog($pattern, [int]$sec = 20) {
    $end = (Get-Date).AddSeconds($sec)
    do {
        $new = LogLines | Select-Object -Skip $script:mark
        $hit = $new | Where-Object { $_ -match $pattern } | Select-Object -First 1
        if ($hit) { return $hit }
        Start-Sleep -Milliseconds 200
    } while ((Get-Date) -lt $end)
    return $null
}
function SinceMark { LogLines | Select-Object -Skip $script:mark }
function Ctl($path, $body) {
    if ($body) { Invoke-RestMethod -Method Post -Uri "$mock$path" -Body ($body | ConvertTo-Json -Compress) -ContentType "application/json" }
    else { Invoke-RestMethod -Uri "$mock$path" }
}
function Run([string[]]$a) { Start-Process $Exe -ArgumentList $a -Wait }  # a second copy that hands over and exits

$script:app = $null
function StopApp {
    if ($script:app -and -not $script:app.HasExited) { $script:app.Kill(); $script:app.WaitForExit() }
    $script:app = $null
}
function StartApp([hashtable]$upload, [hashtable]$shot = @{}, [string[]]$a = @()) {
    StopApp
    $ini = "[Screenshot]`r`nFolder=$dir\shots`r`nOpenEditor=0`r`nShowNotification=1`r`n"
    foreach ($k in $shot.Keys) { $ini = $ini -replace "(?m)^$k=.*`r`n", ""; $ini += "$k=$($shot[$k])`r`n" }
    $ini += "[General]`r`nCheckUpdates=0`r`n[Maintenance]`r`nDailyCheck=0`r`n[Upload]`r`n"
    foreach ($k in $upload.Keys) { $ini += "$k=$($upload[$k])`r`n" }
    [IO.File]::WriteAllText((Join-Path $dir "settings.ini"), $ini)
    Mark
    $script:app = if ($a.Count) { Start-Process $Exe -ArgumentList $a -PassThru } else { Start-Process $Exe -PassThru }
    if (-not (WaitLog "Canivete .* started" 15)) { "FAIL  test copy did not start"; $script:fails++ }
    Start-Sleep -Milliseconds 300
}
function Window($class, [int]$sec = 15) {
    $end = (Get-Date).AddSeconds($sec)
    do {
        $h = [W]::FindWindow($class, [NullString]::Value)
        if ($h -ne [IntPtr]::Zero -and [W]::IsWindowVisible($h)) { return $h }
        Start-Sleep -Milliseconds 200
    } while ((Get-Date) -lt $end)
    return [IntPtr]::Zero
}
function Sha([string]$f) { (Get-FileHash $f -Algorithm SHA256).Hash.ToLower() }
function LastFile { (Ctl "/_ctl/state").files | Select-Object -Last 1 }

# ---- set up ----------------------------------------------------------------------------------
Get-ChildItem $dir -Exclude hidden_test_output.txt -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force
New-Item -ItemType Directory -Force (Join-Path $dir "shots"), (Join-Path $dir "files") | Out-Null
$py = Start-Process python -ArgumentList "`"$PSScriptRoot\upload_mock.py`" $Port `"$secret`"" -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput (Join-Path $dir "mock_out.txt") -RedirectStandardError (Join-Path $dir "mock_err.txt")
$up = $false
foreach ($i in 1..50) { try { Ctl "/_ctl/state" | Out-Null; $up = $true; break } catch { Start-Sleep -Milliseconds 200 } }
if (-not $up) { "FAIL  mock server did not start"; Get-Content (Join-Path $dir "mock_err.txt"); exit 1 }

$small = Join-Path $dir "files\small picture.png"
$bytes = New-Object byte[] 300000; (New-Object Random 1).NextBytes($bytes); [IO.File]::WriteAllBytes($small, $bytes)
$big = Join-Path $dir "files\big video.mp4"
$bytes = New-Object byte[] (20MB + 12345); (New-Object Random 2).NextBytes($bytes); [IO.File]::WriteAllBytes($big, $bytes)

try {
    # ---- not set up: the notice leads to Settings → Uploads ----------------------------------
    StartApp @{ Service = "drive" }
    Mark
    Run "--upload", "`"$small`""
    Check (WaitLog "Notice: Uploads aren't set up") "not set up: notice"
    Run "--click-notice"
    $set = Window "CaniveteSettings" 10
    if ($set -eq [IntPtr]::Zero) { $set = Window "#32770" 5 }
    Check ($set -ne [IntPtr]::Zero) "not set up: clicking the notice opens Settings"
    if ($set -ne [IntPtr]::Zero) { Start-Sleep -Milliseconds 500; [W]::Capture($set, (Join-Path $dir "settings_uploads_drive.png")) }

    # ---- Google Drive: connect -------------------------------------------------------------
    $drive = @{ Service = "drive"; DriveClientId = "1234-test.apps.googleusercontent.com"; DriveFolder = "Canivete" }
    Run "--set-secret", "DriveClientSecret", "GOCSPX-test-secret"
    StartApp $drive
    Mark
    Run "--drive-connect"
    $addr = WaitLog "Drive sign-in address: (.*)"
    Check $addr "drive: sign-in address made"
    if ($addr) {
        $url = ($addr -replace '^.*Drive sign-in address: ', '').Trim()
        $page = Invoke-WebRequest -Uri $url -UseBasicParsing   # the "browser": Google approves, back to Canivete
        Check ($page.Content -match "connected to Google Drive") "drive: browser gets the 'connected' page"
    }
    Check (WaitLog "Drive connect: ok") "drive: connected (PKCE and state checked by the server)"
    Check (WaitLog "Notice: Google Drive connected - Uploads go to tester@example.com") "drive: connected notice names the account"

    # ---- Google Drive: uploads ---------------------------------------------------------------
    StartApp $drive   # a fresh copy: the access token comes from the saved refresh token
    Mark
    Run "--upload", "`"$small`""
    $done = WaitLog "Upload small picture.png to Google Drive: .* -> (.*)"
    Check ($done -match "https://drive.google.com/file/d/file\w+/view") "drive: small upload gives the Drive link"
    Check (WaitLog "Link \(would be copied\): https://drive.google.com/") "drive: link copied"
    $f = LastFile
    Check ($f.sha256 -eq (Sha $small) -and $f.name -eq "small picture.png" -and $f.mime -eq "image/png") "drive: bytes, name and type arrive intact"
    $st = Ctl "/_ctl/state"
    Check ($f.parents -contains "folder1" -and $st.folders.folder1 -eq "Canivete") "drive: goes into the Canivete folder (made once)"
    Check ($st.shared -contains $f.id) "drive: shared as anyone-with-link"
    Check (WaitLog "Notice: Link copied - small picture.png is on Google Drive") "drive: 'Link copied' notice"
    Mark
    Run "--click-notice"
    Check (WaitLog "Would open https://drive.google.com/file/d/") "drive: clicking the notice opens the link"

    # Big file in 8 MB pieces; the second piece breaks halfway and the upload goes on from there.
    $pieces = (Ctl "/_ctl/state").pieces
    Ctl "/_ctl/mode" @{ break_piece = $pieces + 2 } | Out-Null
    Mark
    Run "--upload", "`"$big`""
    $done = WaitLog "Upload big video.mp4 to Google Drive: .* -> " 60
    Check $done "drive: 20 MB upload finishes"
    Check ((SinceMark) -match "piece at 8388608 failed \(503\), retrying") "drive: a broken piece is retried"
    $f = LastFile
    Check ($f.sha256 -eq (Sha $big) -and $f.size -eq (Get-Item $big).Length -and $f.mime -eq "video/mp4") "drive: 20 MB arrives intact after the resume"
    $st = Ctl "/_ctl/state"
    Check ($st.folders.PSObject.Properties.Count -eq 1) "drive: folder found from the saved id, not made again"

    # Sharing refused: the upload still works, the notice says only you can open it.
    Ctl "/_ctl/mode" @{ share = 403 } | Out-Null
    Mark
    Run "--upload", "`"$small`""
    Check (WaitLog "Notice: Link copied - .*Not shared: only you can open the link") "drive: sharing refused is reported"
    Ctl "/_ctl/mode" @{ share = 0 } | Out-Null

    # ---- screenshots -------------------------------------------------------------------------
    # By itself after a capture.
    StartApp ($drive + @{ AutoScreenshots = 1 })
    Mark
    Run "--full"
    $done = WaitLog "Upload (.*\.png) to Google Drive: .* -> https://drive" 30
    Check $done "screenshot: uploads by itself"
    $saved = Get-ChildItem (Join-Path $dir "shots") -Filter *.png | Sort-Object LastWriteTime | Select-Object -Last 1
    Check ($saved -and (LastFile).sha256 -eq (Sha $saved.FullName)) "screenshot: the saved file is what went up"

    # The Upload button on the notification.
    StartApp $drive
    Mark
    Run "--full"
    $toast = Window "CaniveteToast"
    Check ($toast -ne [IntPtr]::Zero) "screenshot: notification shows"
    if ($toast -ne [IntPtr]::Zero) {
        [W]::RedrawWindow($toast, [IntPtr]::Zero, [IntPtr]::Zero, 0x101) | Out-Null
        [W]::Capture($toast, (Join-Path $dir "toast.png"))
        $rc = New-Object W+RECT; [W]::GetClientRect($toast, [ref]$rc) | Out-Null
        $s = $rc.r / 300.0; $pad = [Math]::Round(12 * $s); $lineH = [Math]::Round(20 * $s)
        $x = [int]($rc.r - $pad - 12 * $s); $y = [int]($rc.b - $pad - 2 * $lineH + $lineH / 2)
        [W]::PostMessage($toast, 0x202, [IntPtr]::Zero, [IntPtr](($y -shl 16) -bor $x)) | Out-Null
        Check (WaitLog "Upload .*\.png to Google Drive: .* -> https://drive" 30) "screenshot: Upload button on the notification"
    }

    # The editor's Upload button (key U).
    StartApp $drive @{ OpenEditor = 1 }
    Mark
    Run "--full"
    $ed = Window "CaniveteEditor"
    Check ($ed -ne [IntPtr]::Zero) "editor: opens"
    if ($ed -ne [IntPtr]::Zero) {
        Start-Sleep -Milliseconds 400
        [W]::Capture($ed, (Join-Path $dir "editor.png"))
        [W]::PostMessage($ed, 0x100, [IntPtr]0x55, [IntPtr]::Zero) | Out-Null
        Check (WaitLog "Upload .*\.png to Google Drive: .* -> https://drive" 30) "editor: U saves, closes and uploads"
        Check ((SinceMark) -match "Edited image saved") "editor: saved first"
    }
    # Editor + upload by itself: Done (Enter) uploads.
    StartApp ($drive + @{ AutoScreenshots = 1 }) @{ OpenEditor = 1 }
    Mark
    Run "--full"
    $ed = Window "CaniveteEditor"
    if ($ed -ne [IntPtr]::Zero) {
        Start-Sleep -Milliseconds 300
        Check (-not ((SinceMark) -match "Upload .* to Google Drive")) "editor + auto: nothing goes up before Done"
        [W]::PostMessage($ed, 0x100, [IntPtr]0x0D, [IntPtr]::Zero) | Out-Null
        Check (WaitLog "Upload .*\.png to Google Drive: .* -> https://drive" 30) "editor + auto: Done uploads"
    } else { Check $false "editor + auto: opens" }

    # ---- GIF: the save window's checkbox --------------------------------------------------------
    StartApp ($drive + @{ AutoGif = 1 }) @{} @("--gif-test", "0", "0", "240", "160", "1200")
    $save = Window "CaniveteGifSave" 30
    Check ($save -ne [IntPtr]::Zero) "gif: save window"
    if ($save -ne [IntPtr]::Zero) {
        $cb = [W]::GetDlgItem($save, 122)
        Check ($cb -ne [IntPtr]::Zero -and [W]::SendMessage($cb, 0xF0, [IntPtr]::Zero, [IntPtr]::Zero) -eq [IntPtr]1) "gif: 'Also upload' is ticked (AutoGif)"
        Start-Sleep -Milliseconds 800
        [W]::Capture($save, (Join-Path $dir "gif_save.png"))
        $end = (Get-Date).AddSeconds(60)
        while ([W]::IsWindow($save) -and (Get-Date) -lt $end) {
            [W]::PostMessage($save, 0x111, [IntPtr]1, [IntPtr]::Zero) | Out-Null   # Enter (IDOK)
            Start-Sleep -Milliseconds 500
        }
        Check (WaitLog "Upload (.*\.gif) to Google Drive: .* -> https://drive" 30) "gif: saved and uploaded"
        Check ((LastFile).mime -eq "image/gif") "gif: sent as image/gif"
    }
    StopApp

    # ---- Google Drive: sign-in revoked (e.g. a Testing-mode app after 7 days) ---------------------
    Ctl "/_ctl/mode" @{ refresh = "invalid_grant" } | Out-Null
    StartApp $drive
    Mark
    Run "--upload", "`"$small`""
    Check (WaitLog "Notice: Upload failed - small picture.png: .*(Connect again|connect again)" 20) "drive: revoked sign-in says to connect again"
    Ctl "/_ctl/mode" @{ refresh = "" } | Out-Null

    # ---- Amazon S3 ------------------------------------------------------------------------------
    $s3 = @{ Service = "s3"; S3Bucket = "my-bucket"; S3Region = "eu-west-1"; S3Prefix = "shots"; S3AccessKeyId = "AKIAIOSFODNN7EXAMPLE" }
    Run "--set-secret", "S3SecretKey", $secret
    StartApp $s3
    Mark
    Run "--s3-test"
    Check (WaitLog "S3 test: http://127.0.0.1:$Port/my-bucket/shots/canivete-connection-test.txt") "s3: Test button upload"
    Check (WaitLog "Notice: Amazon S3 works") "s3: test notice"
    Mark
    Run "--upload", "`"$small`""
    Check (WaitLog "Upload small picture.png to Amazon S3: .* -> http://127.0.0.1:$Port/my-bucket/shots/small-picture.png") "s3: upload (signature checked by the server), key without spaces"
    $f = LastFile
    Check ($f.service -eq "s3" -and $f.sha256 -eq (Sha $small) -and $f.key -eq "shots/small-picture.png" -and $f.mime -eq "image/png") "s3: bytes, key and type intact"
    Mark
    Run "--upload", "`"$big`""
    Check (WaitLog "Upload big video.mp4 to Amazon S3: .* -> " 60) "s3: 20 MB upload"
    Check ((LastFile).sha256 -eq (Sha $big)) "s3: 20 MB intact"

    StartApp ($s3 + @{ S3LinkBase = "cdn.example.com/" })
    Mark
    Run "--upload", "`"$small`""
    Check (WaitLog "-> https://cdn.example.com/shots/small-picture.png") "s3: link address setting"

    StartApp ($s3 + @{ S3PrivateLinks = 1 })
    Mark
    Run "--upload", "`"$small`""
    $l = WaitLog "Link \(would be copied\): (.*)"
    Check ($l -match "X-Amz-Expires=604800" -and $l -match "X-Amz-Signature=[0-9a-f]{64}") "s3: private bucket gives a 7-day signed link"

    StartApp ($s3 + @{ AutoScreenshots = 1 })
    Mark
    Run "--full"
    Check (WaitLog "Upload .*\.png to Amazon S3: .* -> http://127.0.0.1:$Port/my-bucket/shots/" 30) "s3: screenshot uploads by itself"

    Ctl "/_ctl/mode" @{ s3 = "AccessDenied" } | Out-Null
    Mark
    Run "--upload", "`"$small`""
    Check (WaitLog "Notice: Upload failed - .*isn't allowed to write there") "s3: AccessDenied explained"
    Ctl "/_ctl/mode" @{ s3 = "" } | Out-Null

    Run "--set-secret", "S3SecretKey", "wrong-secret"
    StartApp $s3
    Mark
    Run "--s3-test"
    Check (WaitLog "Notice: Amazon S3 test failed - AWS refused the secret access key") "s3: wrong secret explained"

    # Not uploading a file that isn't there, and an empty file.
    Run "--set-secret", "S3SecretKey", $secret
    $empty = Join-Path $dir "files\empty.png"; [IO.File]::WriteAllBytes($empty, @())
    Mark
    Run "--upload", "`"$empty`""
    Check (WaitLog "Notice: Upload failed - empty.png: empty.png is empty") "empty file refused"

    # Settings window on the S3 page, for a look.
    Mark
    Ctl "/_ctl/mode" @{ s3 = "AccessDenied" } | Out-Null
    Run "--upload", "`"$small`""
    WaitLog "Notice: Upload failed" | Out-Null
    Run "--click-notice"
    $set = Window "CaniveteSettings" 10
    if ($set -ne [IntPtr]::Zero) { Start-Sleep -Milliseconds 500; [W]::Capture($set, (Join-Path $dir "settings_uploads_s3.png")) }

    $st = Ctl "/_ctl/state"
    Check ($st.errors.Count -eq 0) ("server saw no protocol errors" + $(if ($st.errors.Count) { ": " + ($st.errors -join "; ") }))
    Check (-not (Get-ChildItem "$env:TEMP\Canivete" -Filter "Canivete *.png" -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt (Get-Date).AddMinutes(-10) })) "temporary pictures cleaned up"
}
finally {
    StopApp
    if (-not $py.HasExited) { $py.Kill() }
}
"$script:fails failure(s)"
exit $script:fails
