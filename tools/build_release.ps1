# Builds release\TiTiRelay8-<version>\: the full flash image, the OTA image, the LittleFS image of
# data\ (the web UI) and flash.bat with esptool.exe, ready for a production PC.
#   powershell -ExecutionPolicy Bypass -File tools\build_release.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$arduino = Join-Path $env:LOCALAPPDATA 'Arduino15\packages\esp32'
$cli = Join-Path $env:LOCALAPPDATA 'Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'
$esptool = Get-ChildItem (Join-Path $arduino 'tools\esptool_py') -Directory | Sort-Object Name -Descending | Select-Object -First 1
$mklittlefs = Get-ChildItem (Join-Path $arduino 'tools\mklittlefs') -Directory | Sort-Object Name -Descending | Select-Object -First 1

# Minimal SPIFFS partition table: LittleFS ("spiffs") at 0x3D0000, 128 KB.
$fsOffset = '0x3D0000'
$fsSize = 0x20000

$version = (Select-String -Path (Join-Path $root 'board.h') -Pattern '#define FW_VERSION\s+"([^"]+)"').Matches[0].Groups[1].Value
$out = Join-Path $root "release\TiTiRelay8-$version"
$build = Join-Path $root 'build'
New-Item -ItemType Directory -Force $out, $build | Out-Null

Write-Host "Compiling firmware $version ..."
& $cli compile --fqbn 'esp32:esp32:esp32:PartitionScheme=min_spiffs' --output-dir $build $root
if ($LASTEXITCODE -ne 0) { throw 'compile failed' }

Write-Host 'Building the LittleFS image of data\ ...'
& (Join-Path $mklittlefs.FullName 'mklittlefs.exe') -c (Join-Path $root 'data') -p 256 -b 4096 -s $fsSize (Join-Path $out "TiTiRelay8-$version-littlefs.bin")
if ($LASTEXITCODE -ne 0) { throw 'mklittlefs failed (does data\ fit in 128 KB?)' }

# One image for a blank chip: bootloader, partition table, OTA data, firmware and the web UI.
# (The IDE's .merged.bin is padded to 4 MB and would overlap the LittleFS image.)
$bootApp0 = Get-ChildItem (Join-Path $arduino 'hardware\esp32') -Directory | Sort-Object Name -Descending | Select-Object -First 1
& (Join-Path $esptool.FullName 'esptool.exe') --chip esp32 merge-bin -o (Join-Path $out "TiTiRelay8-$version-full.bin") `
  --flash-mode dio --flash-freq 80m --flash-size 4MB `
  0x1000 (Join-Path $build 'TiTiRelay8.ino.bootloader.bin') `
  0x8000 (Join-Path $build 'TiTiRelay8.ino.partitions.bin') `
  0xe000 (Join-Path $bootApp0.FullName 'tools\partitions\boot_app0.bin') `
  0x10000 (Join-Path $build 'TiTiRelay8.ino.bin') `
  $fsOffset (Join-Path $out "TiTiRelay8-$version-littlefs.bin")
if ($LASTEXITCODE -ne 0) { throw 'merge-bin failed' }
Copy-Item (Join-Path $build 'TiTiRelay8.ino.bin') (Join-Path $out "TiTiRelay8-$version-ota.bin") -Force
Copy-Item (Join-Path $esptool.FullName 'esptool.exe') $out -Force
Copy-Item (Join-Path $root 'platform\product-template.json') $out -Force
Copy-Item (Join-Path $root 'docs\flash-fa.md') (Join-Path $out 'README-fa.md') -Force

$flash = @"
@echo off
rem Writes TiTi Relay-8 $version to an ESP32 on the given port: flash.bat COM5
if "%~1"=="" ( echo Usage: flash.bat COM5 & exit /b 1 )
cd /d "%~dp0"
esptool.exe --chip esp32 --port %1 --baud 921600 erase-flash || goto :fail
esptool.exe --chip esp32 --port %1 --baud 921600 write-flash 0x0 TiTiRelay8-$version-full.bin || goto :fail
echo.
echo Done. Open the serial monitor at 115200 baud to see the start-up.
exit /b 0
:fail
echo.
echo FLASHING FAILED. Check the port and hold BOOT while it connects if needed.
exit /b 1
"@
Set-Content -Path (Join-Path $out 'flash.bat') -Value $flash -Encoding ascii

Get-ChildItem $out | ForEach-Object { '{0,-36} {1,10:N0} bytes' -f $_.Name, $_.Length }
Get-ChildItem $out -Filter *.bin | ForEach-Object { '{0}  {1}' -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Name } |
  Set-Content (Join-Path $out 'SHA256SUMS.txt') -Encoding ascii
Write-Host "Release ready: $out"
