# Installs or data-preservingly updates the current tiered DLS18 build on a USB-debugging device.
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent (Split-Path -Parent $here)
$adb = Join-Path $root "platform-tools\adb.exe"
if (-not (Test-Path $adb)) {
    Expand-Archive -Path (Join-Path $root "platform-tools-latest-windows.zip") -DestinationPath $root -Force
}
$apk = Join-Path $here "build\DLS18_skill_tiered_dribble_v9.apk"
$obb = Join-Path (Split-Path -Parent $here) "original\main.82.com.firsttouchgames.dls3.obb"
$remoteObbDir = "/sdcard/Android/obb/com.firsttouchgames.dls3"
$remoteObb = "$remoteObbDir/main.82.com.firsttouchgames.dls3.obb"

if (-not (Test-Path -LiteralPath $apk)) {
    throw "APK not found: $apk"
}
$devices = & $adb devices
Write-Host $devices
& $adb install -r $apk
$installCode = $LASTEXITCODE
if ($installCode -ne 0) {
    throw "APK installation failed with exit code $installCode. The installed app and its data were not uninstalled."
}

$localObbHash = (Get-FileHash -LiteralPath $obb -Algorithm SHA256).Hash.ToLowerInvariant()
$remoteHashOutput = & $adb shell sha256sum $remoteObb 2>$null
$remoteObbHash = if ($remoteHashOutput) { ($remoteHashOutput -split '\s+')[0].ToLowerInvariant() } else { "" }
if ($remoteObbHash -ne $localObbHash) {
    & $adb shell mkdir -p $remoteObbDir
    if ($LASTEXITCODE -ne 0) { throw "Could not prepare the OBB directory on the device." }
    & $adb push $obb $remoteObb
    if ($LASTEXITCODE -ne 0) { throw "Could not copy the game OBB to the device." }
}
Write-Host "Done. The game APK is installed and its OBB matches the project copy."
