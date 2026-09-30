[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [string]$BuildDirectory,

    [ValidateSet("Visual Studio 17 2022")]
    [string]$Generator = "Visual Studio 17 2022"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$portRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $portRoot "..\.."))
$cmake = Join-Path $repoRoot ".venv\Scripts\cmake.exe"
# A separate directory preserves older compiler caches during toolchain validation.
$buildRoot = if ($BuildDirectory) {
    [IO.Path]::GetFullPath($BuildDirectory)
} else {
    Join-Path $repoRoot "build\mercenaries"
}
$dispatch = Join-Path $portRoot "src\recomp\gen\recomp_dispatch.c"

if (-not (Test-Path -LiteralPath $cmake -PathType Leaf)) {
    throw "CMake not found. Run Setup-Python.ps1 first."
}
if (-not (Test-Path -LiteralPath $dispatch -PathType Leaf)) {
    throw "Generated code not found. Run Recompile.ps1 first."
}

Push-Location $repoRoot
try {
    $configureArguments = @(
        "-S", $portRoot,
        "-B", $buildRoot,
        "-G", $Generator,
        "-A", "x64,version=10.0.26100.0",
        "-T", "v143,version=14.44.35207,host=x64"
    )
    & $cmake @configureArguments
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configuration failed with exit code $LASTEXITCODE."
    }

    & $cmake --build $buildRoot --config $Configuration --parallel
    if ($LASTEXITCODE -ne 0) {
        throw "Native build failed with exit code $LASTEXITCODE."
    }

    $executable = Join-Path $buildRoot "bin\$Configuration\mercenaries_recomp.exe"
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
        throw "Build succeeded but the executable was not found: $executable"
    }
    $launcher = Join-Path $buildRoot "bin\$Configuration\Mercenaries Recompiled.exe"
    $bundledExtractor = Join-Path $buildRoot "bin\$Configuration\tools\xdvdfs.exe"
    $bundledSdl = Join-Path $buildRoot "bin\$Configuration\SDL2.dll"
    if (-not (Test-Path -LiteralPath $launcher -PathType Leaf)) {
        throw "Build succeeded but the first-launch executable was not found: $launcher"
    }
    if (-not (Test-Path -LiteralPath $bundledExtractor -PathType Leaf)) {
        throw "Build succeeded but the bundled runtime extractor was not found: $bundledExtractor"
    }
    if (-not (Test-Path -LiteralPath $bundledSdl -PathType Leaf)) {
        throw "Build succeeded but the bundled SDL2 runtime was not found: $bundledSdl"
    }
    Write-Host "Built game runtime: $executable"
    Write-Host "Built first-launch executable: $launcher"
}
finally {
    Pop-Location
}
