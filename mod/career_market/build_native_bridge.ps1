param(
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'

$marketDir = $PSScriptRoot
$modDir = Split-Path -Parent $marketDir
$zigPath = 'C:\Users\Lenovo\AppData\Local\Temp\career-market-zig-0.14.1\zig-x86_64-windows-0.14.1\zig.exe'
if (-not (Test-Path -LiteralPath $zigPath)) {
    $zigCommand = Get-Command zig.exe -ErrorAction SilentlyContinue
    if ($null -eq $zigCommand) {
        throw 'Zig 0.14.1 was not found. Install Zig or update $zigPath in this script.'
    }
    $zigPath = $zigCommand.Source
}

$sourcePath = Join-Path $marketDir 'native_bridge.c'
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $outputPath = Join-Path $modDir 'build\lib\armeabi-v7a\libCareerMarket.so'
} elseif ([System.IO.Path]::IsPathRooted($OutputPath)) {
    $outputPath = $OutputPath
} else {
    $outputPath = Join-Path $modDir $OutputPath
}
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $outputPath) | Out-Null

$zigArgs = @(
    'cc',
    '-target', 'arm-linux-android',
    '-mfloat-abi=softfp',
    '-std=c11',
    '-O2',
    '-g0',
    '-ffreestanding',
    '-ffunction-sections',
    '-fdata-sections',
    '-fPIC',
    '-fvisibility=hidden',
    '-fno-stack-protector',
    '-fno-sanitize=all',
    '-nostdlib',
    '-shared',
    '-Wl,-soname,libCareerMarket.so',
    '-Wl,--no-undefined',
    '-Wl,--gc-sections',
    '-Wl,-S',
    '-Wl,-z,max-page-size=4096',
    $sourcePath,
    '-o', $outputPath
)

& $zigPath @zigArgs
if ($LASTEXITCODE -ne 0) {
    throw "Career market bridge compilation failed with exit code $LASTEXITCODE."
}

$bridge = Get-Item -LiteralPath $outputPath
Write-Output "Built $($bridge.FullName) ($($bridge.Length) bytes)."
