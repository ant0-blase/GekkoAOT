[CmdletBinding()]
param(
    [ValidateSet("Release", "Debug", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [string]$QtVersion = $(if ($env:GEKKOAOT_QT_VERSION) { $env:GEKKOAOT_QT_VERSION } else { "6.8.3" }),
    [string]$BuildDir = "build-windows",
    [string]$PackageDir = "dist\GekkoAOT",
    [switch]$NoQtInstall,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $RepoRoot

function Require-Command([string]$Name, [string]$Hint) {
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "$Name is required. $Hint"
    }
}

function Test-QtPrefix([string]$Prefix) {
    if ([string]::IsNullOrWhiteSpace($Prefix)) { return $false }
    return (Test-Path (Join-Path $Prefix "lib\cmake\Qt6\Qt6Config.cmake")) -and
           (Test-Path (Join-Path $Prefix "bin\windeployqt.exe"))
}

Require-Command "cmake.exe" "Install CMake and add it to PATH."
Require-Command "git.exe" "Install Git for Windows and add it to PATH."

$QtPrefix = $env:GEKKOAOT_QT_DIR
if (-not (Test-QtPrefix $QtPrefix)) {
    $QtPrefix = Join-Path $RepoRoot ".deps\Qt\$QtVersion\msvc2022_64"
}

if (-not (Test-QtPrefix $QtPrefix)) {
    if ($NoQtInstall) {
        throw "Qt $QtVersion MSVC 2022 x64 was not found. Run .\scripts\install-qt6-windows.ps1 first."
    }

    Write-Host "[GekkoAOT] Qt is missing; bootstrapping Qt $QtVersion..."
    $Output = & (Join-Path $PSScriptRoot "install-qt6-windows.ps1") -QtVersion $QtVersion
    if ($LASTEXITCODE -ne 0) {
        throw "Qt bootstrap failed."
    }
    $QtPrefix = ($Output | Select-Object -Last 1).ToString().Trim()
}

if (-not (Test-QtPrefix $QtPrefix)) {
    throw "Invalid Qt prefix: $QtPrefix"
}

$BuildPath = Join-Path $RepoRoot $BuildDir
$PackagePath = Join-Path $RepoRoot $PackageDir

if ($Clean) {
    if (Test-Path $BuildPath) { Remove-Item -Recurse -Force $BuildPath }
    if (Test-Path $PackagePath) { Remove-Item -Recurse -Force $PackagePath }
}

Write-Host "[GekkoAOT] Qt prefix: $QtPrefix"
Write-Host "[GekkoAOT] Configuring Visual Studio 2022 x64..."

& cmake.exe `
    -S $RepoRoot `
    -B $BuildPath `
    -G "Visual Studio 17 2022" `
    -A x64 `
    "-DCMAKE_PREFIX_PATH=$QtPrefix" `
    -DGEKKOAOT_BUILD_GUI=ON `
    -DGEKKOAOT_BUILD_NATIVE_HOST=ON `
    -DGEKKOAOT_NATIVE_NOD=ON
if ($LASTEXITCODE -ne 0) {
    throw "CMake configure failed."
}

Write-Host "[GekkoAOT] Building $Configuration..."
& cmake.exe --build $BuildPath --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) {
    throw "Build failed."
}

if (Test-Path $PackagePath) {
    Remove-Item -Recurse -Force $PackagePath
}

Write-Host "[GekkoAOT] Installing deployable tree..."
& cmake.exe --install $BuildPath --config $Configuration --prefix $PackagePath
if ($LASTEXITCODE -ne 0) {
    throw "CMake install/deploy failed."
}

$Exe = Join-Path $PackagePath "bin\gekkoaot.exe"
$WinDeployQt = Join-Path $QtPrefix "bin\windeployqt.exe"
if (-not (Test-Path $WinDeployQt)) {
    throw "windeployqt.exe was not found at $WinDeployQt"
}

$DeployMode = if ($Configuration -eq "Debug") { "--debug" } else { "--release" }
Write-Host "[GekkoAOT] Running windeployqt..."
& $WinDeployQt `
    $DeployMode `
    --force `
    --no-translations `
    --compiler-runtime `
    --dir (Join-Path $PackagePath "bin") `
    $Exe
if ($LASTEXITCODE -ne 0) {
    throw "windeployqt failed."
}

$Required = @(
    (Join-Path $PackagePath "bin\gekkoaot.exe"),
    (Join-Path $PackagePath "bin\Qt6Core.dll"),
    (Join-Path $PackagePath "bin\Qt6Gui.dll"),
    (Join-Path $PackagePath "bin\Qt6Widgets.dll"),
    (Join-Path $PackagePath "bin\platforms\qwindows.dll")
)

foreach ($File in $Required) {
    if (-not (Test-Path $File)) {
        throw "Windows package is incomplete; missing: $File"
    }
}

Write-Host ""
Write-Host "[GekkoAOT] Windows package is ready."
Write-Host "[GekkoAOT] Run:"
Write-Host "  $Exe"
