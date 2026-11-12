# Builds release\TiTiRelay8-<version>\: the full flash image, the OTA image, the LittleFS image of
# data\ (the web UI) and flash.bat with esptool.exe, ready for a production PC.
#   powershell -ExecutionPolicy Bypass -File tools\build_release.ps1 [-Ca path\to\ca.crt]
# The device CA (deploy/ca/ca.crt of the server, never ca.key) is copied into the web UI image as
# /ca.pem, so every board trusts the server's TLS certificate without a per-board step. Without
# -Ca it uses certs\ca.crt if that file exists.
param([string]$Ca = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$arduino = Join-Path $env:LOCALAPPDATA 'Arduino15\packages\esp32'
$cli = Join-Path $env:LOCALAPPDATA 'Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'
$esptool = Get-ChildItem (Join-Path $arduino 'tools\esptool_py') -Directory | Sort-Object Name -Descending | Select-Object -First 1
$mklittlefs = Get-ChildItem (Join-Path $arduino 'tools\mklittlefs') -Directory | Sort-Object Name -Descending | Select-Object -First 1

# Minimal SPIFFS partition table: LittleFS ("spiffs") at 0x3D0000, 128 KB.
$fsOffset = '0x3D0000'
$fsSize = 0x20000

$version = (Select-String -Path (Join-Path $root 'config.h') -Pattern '#define FW_VERSION\s+"([^"]+)"').Matches[0].Groups[1].Value
$out = Join-Path $root "release\TiTiRelay8-$version"
$build = Join-Path $root 'build'
# --export-binaries puts the result in build\<fqbn>\ and, unlike --output-dir, reuses the compiler's
# cache (a full rebuild takes 10-15 minutes, a cached one about a minute).
$art = Join-Path $build 'esp32.esp32.esp32'
New-Item -ItemType Directory -Force $out, $build | Out-Null

Write-Host "Compiling firmware $version ..."
& $cli compile --fqbn 'esp32:esp32:esp32:PartitionScheme=min_spiffs' --export-binaries $root
if ($LASTEXITCODE -ne 0) { throw 'compile failed' }

if (-not $Ca -and (Test-Path (Join-Path $root 'certs\ca.crt'))) { $Ca = Join-Path $root 'certs\ca.crt' }
$fsDir = Join-Path $build 'fs'
if (Test-Path $fsDir) { Remove-Item -Recurse -Force $fsDir }
Copy-Item (Join-Path $root 'data') $fsDir -Recurse
if ($Ca) {
  $pem = Get-Content $Ca -Raw
  if ($pem -match 'PRIVATE KEY') { throw "$Ca holds a private key. Give ca.crt only, never ca.key." }
  if ($pem -notmatch 'BEGIN CERTIFICATE') { throw "$Ca is not a PEM certificate" }
  if ($pem.Length -gt 3900) { throw "$Ca is too large (4000 bytes at most)" }
  Set-Content -Path (Join-Path $fsDir 'ca.pem') -Value $pem -NoNewline -Encoding ascii
  Write-Host "Device CA included: $Ca"
} else {
  Write-Host 'No device CA given: a TLS server needs it pasted in the portal (Connection page).'
}

# Compress the page and scripts (the board sends index.html.gz / *.js.gz with Content-Encoding: gzip);
# without it data\ plus the two Vazirmatn fonts would not fit in the 128 KB partition.
foreach ($f in Get-ChildItem $fsDir -Recurse -File | Where-Object { $_.Extension -in '.html', '.js', '.css' }) {
  $src = [IO.File]::OpenRead($f.FullName)
  $dst = [IO.File]::Create($f.FullName + '.gz')
  $gz = New-Object IO.Compression.GZipStream($dst, [IO.Compression.CompressionLevel]::Optimal)
  $src.CopyTo($gz); $gz.Dispose(); $dst.Dispose(); $src.Dispose()
  Remove-Item $f.FullName
}

Write-Host 'Building the LittleFS image (the web UI) ...'
$mk = Join-Path $mklittlefs.FullName 'mklittlefs.exe'
$fsImage = Join-Path $out "TiTiRelay8-$version-littlefs.bin"
$mkOut = & $mk -c $fsDir -p 256 -b 4096 -s $fsSize $fsImage 2>&1 | Out-String
# mklittlefs ends with exit code 0 even when the partition is full (the image then silently lacks
# files), so the output is checked and every file of the folder must be listed in the image.
if ($LASTEXITCODE -ne 0 -or $mkOut -match 'error') { throw "the web UI does not fit in the 128 KB LittleFS partition:`n$mkOut" }
$listing = (& $mk -l $fsImage -s $fsSize -b 4096 -p 256 2>&1 | Out-String)
$fsRoot = (Resolve-Path $fsDir).Path
$total = 0
foreach ($f in Get-ChildItem $fsDir -Recurse -File) {
  $rel = '/' + ($f.FullName.Substring($fsRoot.Length + 1) -replace '\\', '/')
  if ($listing -notmatch ('(?m)^' + $f.Length + '\s+' + [regex]::Escape($rel) + '\s')) { throw "$rel is missing in the LittleFS image (partition full?)" }
  $total += $f.Length
}
Write-Host ('Web files: {0:N0} bytes of the {1:N0}-byte partition (keep the total under about 110 KB)' -f $total, $fsSize)

# One image for a blank chip: bootloader, partition table, OTA data, firmware and the web UI.
# (The IDE's .merged.bin is padded to 4 MB and would overlap the LittleFS image.)
$bootApp0 = Get-ChildItem (Join-Path $arduino 'hardware\esp32') -Directory | Sort-Object Name -Descending | Select-Object -First 1
& (Join-Path $esptool.FullName 'esptool.exe') --chip esp32 merge-bin -o (Join-Path $out "TiTiRelay8-$version-full.bin") `
  --flash-mode dio --flash-freq 80m --flash-size 4MB `
  0x1000 (Join-Path $art 'TiTiRelay8.ino.bootloader.bin') `
  0x8000 (Join-Path $art 'TiTiRelay8.ino.partitions.bin') `
  0xe000 (Join-Path $bootApp0.FullName 'tools\partitions\boot_app0.bin') `
  0x10000 (Join-Path $art 'TiTiRelay8.ino.bin') `
  $fsOffset (Join-Path $out "TiTiRelay8-$version-littlefs.bin")
if ($LASTEXITCODE -ne 0) { throw 'merge-bin failed' }
Copy-Item (Join-Path $art 'TiTiRelay8.ino.bin') (Join-Path $out "TiTiRelay8-$version-ota.bin") -Force
Copy-Item (Join-Path $esptool.FullName 'esptool.exe') $out -Force
Copy-Item (Join-Path $root 'platform\product-template.json') $out -Force
Copy-Item (Join-Path $root 'docs\flash-fa.md') (Join-Path $out 'README-fa.md') -Force

$flash = @"
@echo off
rem Writes TiTi Relay-8 $version to an ESP32 on the given port: flash.bat COM5
if "%~1"=="" ( echo Usage: flash.bat COM5 [baud, default 921600; use 115200 if it fails] & exit /b 1 )
set BAUD=%~2
if "%BAUD%"=="" set BAUD=921600
cd /d "%~dp0"
esptool.exe --chip esp32 --port %1 --baud %BAUD% erase-flash || goto :fail
esptool.exe --chip esp32 --port %1 --baud %BAUD% write-flash 0x0 TiTiRelay8-$version-full.bin || goto :fail
echo.
echo Done. Open the serial monitor at 115200 baud to see the start-up.
exit /b 0
:fail
echo.
echo FLASHING FAILED. Check the port and hold BOOT while it connects if needed.
exit /b 1
"@
Set-Content -Path (Join-Path $out 'flash.bat') -Value $flash -Encoding ascii

# Only the web UI (LittleFS at 0x3D0000): settings, identity and firmware stay as they are.
$flashWeb = @"
@echo off
rem Writes only the web UI of TiTi Relay-8 $version (LittleFS): flash-web.bat COM5
if "%~1"=="" ( echo Usage: flash-web.bat COM5 [baud, default 921600; use 115200 if it fails] & exit /b 1 )
set BAUD=%~2
if "%BAUD%"=="" set BAUD=921600
cd /d "%~dp0"
esptool.exe --chip esp32 --port %1 --baud %BAUD% write-flash 0x3D0000 TiTiRelay8-$version-littlefs.bin || goto :fail
echo.
echo Done: the web UI is updated. The board keeps its settings and its firmware.
exit /b 0
:fail
echo.
echo FLASHING FAILED. Check the port and hold BOOT while it connects if needed.
exit /b 1
"@
Set-Content -Path (Join-Path $out 'flash-web.bat') -Value $flashWeb -Encoding ascii

# Only the firmware (app slot at 0x10000): settings, identity and the web UI stay as they are.
# otadata is erased first so the board boots the slot written here (a board that was updated over
# the air may have been running the other slot).
$flashFw = @"
@echo off
rem Writes only the firmware of TiTi Relay-8 ${version}: flash-firmware.bat COM5
if "%~1"=="" ( echo Usage: flash-firmware.bat COM5 [baud, default 921600; use 115200 if it fails] & exit /b 1 )
set BAUD=%~2
if "%BAUD%"=="" set BAUD=921600
cd /d "%~dp0"
esptool.exe --chip esp32 --port %1 --baud %BAUD% erase-region 0xe000 0x2000 || goto :fail
esptool.exe --chip esp32 --port %1 --baud %BAUD% write-flash 0x10000 TiTiRelay8-$version-ota.bin || goto :fail
echo.
echo Done: the firmware is updated. The board keeps its settings and its web UI.
exit /b 0
:fail
echo.
echo FLASHING FAILED. Check the port and hold BOOT while it connects if needed.
exit /b 1
"@
Set-Content -Path (Join-Path $out 'flash-firmware.bat') -Value $flashFw -Encoding ascii

Get-ChildItem $out | ForEach-Object { '{0,-36} {1,10:N0} bytes' -f $_.Name, $_.Length }
Get-ChildItem $out -Filter *.bin | ForEach-Object { '{0}  {1}' -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Name } |
  Set-Content (Join-Path $out 'SHA256SUMS.txt') -Encoding ascii
Write-Host "Release ready: $out"
