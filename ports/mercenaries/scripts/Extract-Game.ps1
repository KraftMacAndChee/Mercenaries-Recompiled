[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$IsoPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$portRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $portRoot "..\.."))
$xdvdfs = Join-Path $repoRoot ".tools\xdvdfs\xdvdfs.exe"
# Supported retail XBE revisions. The EU/FR disc ships the same translated code
# (identical section layout and entry point); only the certificate differs.
$supportedXbeHashes = @{
    "AA08EA21D952AC35F49C02C7E2ED08AA25AD7535FBBBCCC95636775F37BE99D7" = "USA"
    "9DD26D6CE030F467462A4A8132AC82217ECEA69ECFEF5FAB9238392642FDC356" = "EU-FR"
}

if (-not (Test-Path -LiteralPath $IsoPath -PathType Leaf)) {
    throw "ISO not found: $IsoPath"
}
if (-not (Test-Path -LiteralPath $xdvdfs -PathType Leaf)) {
    throw @"
xdvdfs.exe was not found at:
  $xdvdfs

Run ports/mercenaries/scripts/Setup-Tools.ps1 to install the bundled, verified xdvdfs v0.8.3 executable.
"@
}

$destination = [System.IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $destination) {
    $existing = Get-ChildItem -LiteralPath $destination -Force | Select-Object -First 1
    if ($existing) {
        throw "Refusing to unpack over non-empty directory: $destination"
    }
}

& $xdvdfs unpack ([System.IO.Path]::GetFullPath($IsoPath)) $destination
if ($LASTEXITCODE -ne 0) {
    throw "ISO extraction failed with exit code $LASTEXITCODE."
}

$xbePath = Join-Path $destination "default.xbe"
if (-not (Test-Path -LiteralPath $xbePath -PathType Leaf)) {
    throw "Extraction completed without default.xbe."
}

$actualHash = (Get-FileHash -LiteralPath $xbePath -Algorithm SHA256).Hash
if (-not $supportedXbeHashes.ContainsKey($actualHash)) {
    throw "Unsupported default.xbe revision. Expected one of $($supportedXbeHashes.Keys -join ', '), got $actualHash."
}
Write-Host "Retail default.xbe revision: $($supportedXbeHashes[$actualHash])"

$files = Get-ChildItem -LiteralPath $destination -Recurse -File | Measure-Object Length -Sum
Write-Host ("Extracted {0} files ({1:N3} GiB) to {2}" -f $files.Count, ($files.Sum / 1GB), $destination)
