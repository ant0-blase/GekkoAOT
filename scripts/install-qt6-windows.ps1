[CmdletBinding()]
param(
    [string]$QtVersion = $(if ($env:GEKKOAOT_QT_VERSION) { $env:GEKKOAOT_QT_VERSION } else { "6.8.3" }),
    [string]$Arch = "win64_msvc2022_64",
    [string]$InstallRoot = "",
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if ([string]::IsNullOrWhiteSpace($InstallRoot)) {
    $InstallRoot = Join-Path $RepoRoot ".deps\Qt"
} elseif (-not [System.IO.Path]::IsPathRooted($InstallRoot)) {
    $InstallRoot = Join-Path $RepoRoot $InstallRoot
}
$InstallRoot = [System.IO.Path]::GetFullPath($InstallRoot)

function Test-QtPrefix([string]$Prefix) {
    if ([string]::IsNullOrWhiteSpace($Prefix)) { return $false }
    return (Test-Path (Join-Path $Prefix "lib\cmake\Qt6\Qt6Config.cmake")) -and
           (Test-Path (Join-Path $Prefix "bin\windeployqt.exe"))
}

if ($env:GEKKOAOT_QT_DIR -and (Test-QtPrefix $env:GEKKOAOT_QT_DIR)) {
    Write-Host "[GekkoAOT] Using GEKKOAOT_QT_DIR=$env:GEKKOAOT_QT_DIR"
    Write-Output ([System.IO.Path]::GetFullPath($env:GEKKOAOT_QT_DIR))
    exit 0
}

$ExpectedPrefix = Join-Path $InstallRoot "$QtVersion\msvc2022_64"
if ((-not $Force) -and (Test-QtPrefix $ExpectedPrefix)) {
    Write-Host "[GekkoAOT] Qt $QtVersion already installed at $ExpectedPrefix"
    Write-Output $ExpectedPrefix
    exit 0
}

$PythonExe = $null
$PythonPrefixArgs = @()

$PyLauncher = Get-Command py.exe -ErrorAction SilentlyContinue
if ($PyLauncher) {
    $PythonExe = $PyLauncher.Source
    $PythonPrefixArgs = @("-3")
} else {
    $Python = Get-Command python.exe -ErrorAction SilentlyContinue
    if ($Python) {
        $PythonExe = $Python.Source
    }
}

if (-not $PythonExe) {
    throw @"
Python 3 is required only for the Qt bootstrap helper.
Install it once, for example:
  winget install --id Python.Python.3.13 -e
Then rerun:
  .\scripts\install-qt6-windows.ps1
"@
}

if ($Force -and (Test-Path $ExpectedPrefix)) {
    Write-Host "[GekkoAOT] Removing existing Qt prefix: $ExpectedPrefix"
    Remove-Item -Recurse -Force $ExpectedPrefix
}

New-Item -ItemType Directory -Force -Path $InstallRoot | Out-Null

Write-Host "[GekkoAOT] Installing/updating aqtinstall..."
& $PythonExe @PythonPrefixArgs -m pip install --user --disable-pip-version-check --upgrade aqtinstall
if ($LASTEXITCODE -ne 0) {
    throw "Failed to install aqtinstall."
}

Write-Host "[GekkoAOT] Installing Qt $QtVersion ($Arch) into $InstallRoot ..."
& $PythonExe @PythonPrefixArgs -m aqt install-qt `
    --outputdir $InstallRoot `
    windows desktop $QtVersion $Arch
if ($LASTEXITCODE -ne 0) {
    throw "Qt installation failed."
}

if (-not (Test-QtPrefix $ExpectedPrefix)) {
    $Config = Get-ChildItem -Path $InstallRoot -Filter Qt6Config.cmake -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -like "*$QtVersion*" } |
        Select-Object -First 1

    if (-not $Config) {
        throw "Qt was downloaded, but Qt6Config.cmake could not be located under $InstallRoot."
    }

    $ExpectedPrefix = [System.IO.Path]::GetFullPath(
        (Join-Path $Config.Directory.FullName "..\..\..")
    )
}

if (-not (Test-QtPrefix $ExpectedPrefix)) {
    throw "Qt prefix is incomplete: $ExpectedPrefix"
}

Write-Host "[GekkoAOT] Qt ready: $ExpectedPrefix"
Write-Output $ExpectedPrefix
