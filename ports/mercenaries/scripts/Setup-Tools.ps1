[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$portRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$repoRoot = [IO.Path]::GetFullPath((Join-Path $portRoot "..\.."))
$toolsRoot = Join-Path $repoRoot ".tools"
$installRoot = Join-Path $toolsRoot "xdvdfs"
$executable = Join-Path $installRoot "xdvdfs.exe"
$bundleRoot = Join-Path $portRoot "third_party\xdvdfs"
$bundledExecutable = Join-Path $bundleRoot "xdvdfs.exe"
$bundledLicense = Join-Path $bundleRoot "LICENSE"
$expectedExecutableHash = "DF5F19954EF706C130C256546BF67CF7725611BDB30D1815577187007A6F907A"

if (-not (Test-Path -LiteralPath $bundledExecutable -PathType Leaf)) {
    throw "Bundled xdvdfs.exe is missing: $bundledExecutable"
}
if (-not (Test-Path -LiteralPath $bundledLicense -PathType Leaf)) {
    throw "Bundled xdvdfs license is missing: $bundledLicense"
}
$bundledHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $bundledExecutable).Hash
if ($bundledHash -ne $expectedExecutableHash) {
    throw "Bundled xdvdfs.exe has an unexpected SHA-256: $bundledHash"
}

if (Test-Path -LiteralPath $executable -PathType Leaf) {
    $actualExecutableHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $executable).Hash
    if ($actualExecutableHash -ne $expectedExecutableHash) {
        throw "Existing xdvdfs.exe has an unexpected SHA-256: $actualExecutableHash"
    }
    Write-Host "Verified xdvdfs v0.8.3: $executable"
    return
}
if (Test-Path -LiteralPath $installRoot) {
    throw "Refusing to overwrite incomplete tool directory: $installRoot"
}

New-Item -ItemType Directory -Force -Path $toolsRoot | Out-Null
$stagingRoot = Join-Path $toolsRoot "xdvdfs-staging-$([Guid]::NewGuid().ToString('N'))"
try {
    New-Item -ItemType Directory -Path $stagingRoot | Out-Null
    Copy-Item -LiteralPath $bundledExecutable -Destination $stagingRoot
    Copy-Item -LiteralPath $bundledLicense -Destination $stagingRoot
    $stagedExecutable = Join-Path $stagingRoot "xdvdfs.exe"
    $stagedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $stagedExecutable).Hash
    if ($stagedHash -ne $expectedExecutableHash) {
        throw "Installed xdvdfs.exe has an unexpected SHA-256: $stagedHash"
    }
    Move-Item -LiteralPath $stagingRoot -Destination $installRoot
}
finally {
    if (Test-Path -LiteralPath $stagingRoot) {
        Remove-Item -LiteralPath $stagingRoot -Recurse -Force
    }
}

Write-Host "Installed and verified bundled xdvdfs v0.8.3: $executable"
