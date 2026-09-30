[CmdletBinding()]
param(
    [string]$GameDirectory,

    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$portRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $portRoot "..\.."))
if (-not $GameDirectory) {
    $GameDirectory = Join-Path $repoRoot "game_files\mercenaries"
    $GameDirectory = Join-Path $repoRoot "game_files\mercenaries-retail"
}

$gameRoot = [System.IO.Path]::GetFullPath($GameDirectory)
$xbePath = Join-Path $gameRoot "default.xbe"
$executable = Join-Path $repoRoot "build\mercenaries\bin\$Configuration\mercenaries_recomp.exe"

if (-not (Test-Path -LiteralPath $xbePath -PathType Leaf)) {
    throw "default.xbe not found at: $xbePath"
}
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Executable not found. Run Build.ps1 first."
}

& $executable $gameRoot
exit $LASTEXITCODE
