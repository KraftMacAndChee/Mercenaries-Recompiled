[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$IsoPath,

    [string]$GameDirectory,

    [string]$BuildDirectory,

    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [ValidateSet("Visual Studio 17 2022")]
    [string]$Generator = "Visual Studio 17 2022",

    [switch]$SkipDependencySetup
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$portRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$repoRoot = [IO.Path]::GetFullPath((Join-Path $portRoot "..\.."))
$isoFull = [IO.Path]::GetFullPath($IsoPath)
if (-not $GameDirectory) {
    $GameDirectory = Join-Path $repoRoot "game_files\mercenaries-retail"
}
$gameFull = [IO.Path]::GetFullPath($GameDirectory)

function Get-DeterministicTreeHash {
    param([Parameter(Mandatory = $true)][string]$Root)

    $resolvedRoot = [IO.Path]::GetFullPath($Root).TrimEnd('\')
    $files = @(Get-ChildItem -LiteralPath $resolvedRoot -Recurse -File |
        Sort-Object FullName)
    $lines = foreach ($file in $files) {
        $relative = $file.FullName.Substring($resolvedRoot.Length).TrimStart('\').Replace('\', '/')
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash
        "generated/$relative`t$hash"
    }
    $manifestBytes = [Text.UTF8Encoding]::new($false).GetBytes(($lines -join "`n"))
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $treeHash = [BitConverter]::ToString($sha.ComputeHash($manifestBytes)).Replace('-', '')
    }
    finally {
        $sha.Dispose()
    }
    [pscustomobject]@{ Count = $files.Count; Hash = $treeHash }
}

function Get-CMakeCacheValue {
    param(
        [Parameter(Mandatory = $true)][string]$CachePath,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $line = Get-Content -LiteralPath $CachePath |
        Where-Object { $_ -match "^$([Regex]::Escape($Name)):[^=]+=(.*)$" } |
        Select-Object -First 1
    if (-not $line) { return $null }
    return ([Regex]::Match($line, "^[^=]+=(.*)$").Groups[1].Value)
}

function Get-CMakeSetValue {
    param(
        [Parameter(Mandatory = $true)][string]$MetadataPath,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $pattern = '^set\(' + [Regex]::Escape($Name) + '\s+(?:"([^"]*)"|([^\)]+))\)$'
    $line = Get-Content -LiteralPath $MetadataPath |
        Where-Object { $_ -match $pattern } |
        Select-Object -First 1
    if (-not $line) { return $null }
    $match = [Regex]::Match($line, $pattern)
    if ($match.Groups[1].Success) { return $match.Groups[1].Value }
    return $match.Groups[2].Value.Trim()
}

function Get-ProjectElementValue {
    param(
        [Parameter(Mandatory = $true)][string]$ProjectPath,
        [Parameter(Mandatory = $true)][string]$ElementName
    )

    $text = [IO.File]::ReadAllText($ProjectPath)
    $match = [Regex]::Match(
        $text,
        '<' + [Regex]::Escape($ElementName) + '>([^<]+)</' +
            [Regex]::Escape($ElementName) + '>'
    )
    if (-not $match.Success) { return $null }
    return $match.Groups[1].Value
}

if (-not $SkipDependencySetup) {
    & (Join-Path $PSScriptRoot "Setup-Tools.ps1")
    & (Join-Path $PSScriptRoot "Setup-Python.ps1")
}

& (Join-Path $PSScriptRoot "Extract-Game.ps1") -IsoPath $isoFull -OutputDirectory $gameFull
& (Join-Path $PSScriptRoot "Recompile.ps1") -GameDirectory $gameFull

$buildRoot = if ($BuildDirectory) {
    [IO.Path]::GetFullPath($BuildDirectory)
} else {
    Join-Path $repoRoot "build\mercenaries"
}
$buildParameters = @{
    Configuration = $Configuration
    Generator = $Generator
    BuildDirectory = $buildRoot
}
& (Join-Path $PSScriptRoot "Build.ps1") @buildParameters

$generatedRoot = Join-Path $portRoot "src\recomp\gen"
$generatedTree = Get-DeterministicTreeHash -Root $generatedRoot
$executable = Join-Path $buildRoot "bin\$Configuration\mercenaries_recomp.exe"
$launcher = Join-Path $buildRoot "bin\$Configuration\Mercenaries Recompiled.exe"
$runtimeExtractor = Join-Path $buildRoot "bin\$Configuration\tools\xdvdfs.exe"
$sdl2Runtime = Join-Path $buildRoot "bin\$Configuration\SDL2.dll"
$xbe = Join-Path $gameFull "default.xbe"
$xdvdfs = Join-Path $repoRoot ".tools\xdvdfs\xdvdfs.exe"
$python = Join-Path $repoRoot ".venv\Scripts\python.exe"
$cmake = Join-Path $repoRoot ".venv\Scripts\cmake.exe"
$archiveScript = Join-Path $PSScriptRoot "Archive-GeneratedProvenance.py"
$archiveRoot = Join-Path $repoRoot "artifacts\provenance\generated-trees"
$archiveArguments = @(
    "--generated-root", $generatedRoot,
    "--repo-root", $repoRoot,
    "--port-root", $portRoot,
    "--xbe", $xbe,
    "--archive-root", $archiveRoot
)
$archivePath = (& $python $archiveScript @archiveArguments).Trim()
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $archivePath -PathType Container)) {
    throw "Failed to archive the exact generated tree and toolchain source used by this build."
}
$archiveManifest = Get-Content -LiteralPath (Join-Path $archivePath "provenance.json") -Raw |
    ConvertFrom-Json
if ($archiveManifest.generatedFileCount -ne $generatedTree.Count -or
        $archiveManifest.generatedTreeSha256 -ne $generatedTree.Hash) {
    throw "Generated-tree identity disagrees with its archived provenance."
}

$cachePath = Join-Path $buildRoot "CMakeCache.txt"
$compilerMetadata = Get-ChildItem -LiteralPath (Join-Path $buildRoot "CMakeFiles") -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
    Sort-Object { [Version]$_.Name } -Descending |
    ForEach-Object { Join-Path $_.FullName "CMakeCCompiler.cmake" } |
    Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
    Select-Object -First 1
$projectPath = Join-Path $buildRoot "mercenaries_recomp.vcxproj"
if (-not $compilerMetadata -or -not (Test-Path -LiteralPath $projectPath -PathType Leaf)) {
    throw "CMake compiler metadata is missing from the completed build."
}
$compilerPath = Get-CMakeSetValue -MetadataPath $compilerMetadata -Name "CMAKE_C_COMPILER"
$linkerPath = Get-CMakeSetValue -MetadataPath $compilerMetadata -Name "CMAKE_C_COMPILER_LINKER"
if (-not $compilerPath -or -not $linkerPath) {
    throw "CMake did not record the compiler and linker paths."
}
$cmakeVersion = (& $cmake --version | Select-Object -First 1).Trim()
$pythonVersion = (& $python --version 2>&1 | Select-Object -First 1).ToString().Trim()
$gitRevision = $null
$gitDirty = $null
if (Get-Command git -ErrorAction SilentlyContinue) {
    $gitRevision = (& git -C $repoRoot rev-parse HEAD 2>$null)
    if ($gitRevision) { $gitRevision = $gitRevision.Trim() }
    $gitStatus = (& git -C $repoRoot status --porcelain --untracked-files=all 2>$null)
    $gitDirty = [bool]$gitStatus
}

$provenance = [ordered]@{
    schema = 3
    port = "mercenaries-retail-usa"
    sourceRevision = $gitRevision
    sourceWorkingTreeDirty = $gitDirty
    toolchainArchiveId = $archiveManifest.archiveId
    toolchainFileCount = $archiveManifest.toolchainFileCount
    toolchainTreeSha256 = $archiveManifest.toolchainTreeSha256
    toolchainArchiveSha256 = $archiveManifest.toolchainArchiveSha256
    inputIsoSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $isoFull).Hash
    retailXbeSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $xbe).Hash
    xdvdfsExeSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $xdvdfs).Hash
    requirementsSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $portRoot "requirements.txt")).Hash
    manualSeedsSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $portRoot "manual-seeds.json")).Hash
    patchPipelineSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $PSScriptRoot "Patch-Generated.py")).Hash
    generatedFileCount = $generatedTree.Count
    generatedTreeSha256 = $generatedTree.Hash
    configuration = $Configuration
    hostOperatingSystem = [Environment]::OSVersion.VersionString
    hostArchitecture = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
    powershellVersion = $PSVersionTable.PSVersion.ToString()
    pythonVersion = $pythonVersion
    pythonExeSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $python).Hash
    cmakeVersion = $cmakeVersion
    cmakeExeSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $cmake).Hash
    cmakeGenerator = Get-CMakeCacheValue -CachePath $cachePath -Name "CMAKE_GENERATOR"
    cmakeGeneratorPlatform = Get-CMakeCacheValue -CachePath $cachePath -Name "CMAKE_GENERATOR_PLATFORM"
    compilerId = Get-CMakeSetValue -MetadataPath $compilerMetadata -Name "CMAKE_C_COMPILER_ID"
    compilerVersion = Get-CMakeSetValue -MetadataPath $compilerMetadata -Name "CMAKE_C_COMPILER_VERSION"
    compilerArchitecture = Get-CMakeSetValue -MetadataPath $compilerMetadata -Name "CMAKE_C_COMPILER_ARCHITECTURE_ID"
    compilerExeSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $compilerPath).Hash
    linkerVersion = Get-CMakeSetValue -MetadataPath $compilerMetadata -Name "CMAKE_C_COMPILER_LINKER_VERSION"
    linkerExeSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $linkerPath).Hash
    visualStudioPlatformToolset = Get-ProjectElementValue -ProjectPath $projectPath -ElementName "PlatformToolset"
    windowsSdkVersion = Get-ProjectElementValue -ProjectPath $projectPath -ElementName "WindowsTargetPlatformVersion"
    executableSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $executable).Hash
    launcherSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $launcher).Hash
    bundledRuntimeExtractorSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $runtimeExtractor).Hash
    sdl2RuntimeSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $sdl2Runtime).Hash
}
$provenancePath = Join-Path (Split-Path -Parent $executable) "mercenaries-build-provenance.json"
$provenanceJson = $provenance | ConvertTo-Json -Depth 4
[IO.File]::WriteAllText($provenancePath, $provenanceJson + "`n", [Text.UTF8Encoding]::new($false))

Write-Host "ISO-to-port game runtime: $executable"
Write-Host "ISO-to-port first-launch executable: $launcher"
Write-Host "Build provenance: $provenancePath"