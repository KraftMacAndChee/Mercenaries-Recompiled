[CmdletBinding()]
param(
    [switch]$Apply
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$testRoot = [IO.Path]::GetFullPath((Join-Path $workspace 'artifacts\test-runs'))
$referenceRun = 'run1428-vs2022-save-load'
$referenceRoot = [IO.Path]::GetFullPath(
    (Join-Path $testRoot "$referenceRun\saves\Cache\dataxbox")
)
$manifestRoot = Join-Path $workspace 'artifacts\cleanup-manifests'
$keepRuns = @(
    'run737-human-run-fixed',
    'run742-ambient-intersection-fixed',
    'run1428-vs2022-save-load',
    'run1514-fresh-reload-after-overwrite',
    'run1520-pathreader-refined-live'
)
$names = @('assets.dsk', 'english.dsk', 'streamed.dsk')

if (-not (Test-Path -LiteralPath $testRoot -PathType Container)) {
    throw "Test-run root missing: $testRoot"
}
if (-not (Test-Path -LiteralPath $referenceRoot -PathType Container)) {
    throw "Retained reference cache missing: $referenceRoot"
}

$authoritative = @{}
foreach ($name in $names) {
    $path = Join-Path $referenceRoot $name
    $item = Get-Item -LiteralPath $path
    $authoritative[$name] = [pscustomobject]@{
        Path = $item.FullName
        Length = $item.Length
        Sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash
    }
}

$rows = [Collections.Generic.List[object]]::new()
foreach ($run in Get-ChildItem -LiteralPath $testRoot -Directory) {
    if ($keepRuns -contains $run.Name) { continue }
    foreach ($name in $names) {
        $candidatePath = Join-Path $run.FullName "saves\Cache\dataxbox\$name"
        if (-not (Test-Path -LiteralPath $candidatePath -PathType Leaf)) { continue }
        $candidate = Get-Item -LiteralPath $candidatePath
        $full = [IO.Path]::GetFullPath($candidate.FullName)
        if (-not $full.StartsWith($testRoot + [IO.Path]::DirectorySeparatorChar,
                                 [StringComparison]::OrdinalIgnoreCase)) {
            throw "Candidate escaped test-run root: $full"
        }
        if (($candidate.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Refusing linked candidate: $full"
        }
        $source = $authoritative[$name]
        if ($candidate.Length -ne $source.Length) { continue }
        $hash = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash
        if ($hash -ne $source.Sha256) { continue }
        $rows.Add([pscustomobject]@{
            Run = $run.Name
            Path = $full
            Length = $candidate.Length
            Sha256 = $hash
            AuthoritativePath = $source.Path
            Action = if ($Apply) { 'Delete exact duplicate' } else { 'Dry run' }
        })
    }
}

$bytes = [int64]0
foreach ($row in $rows) {
    $bytes += [int64]$row.Length
}
Write-Output ("Matched {0} exact duplicate files ({1:N2} GiB)." -f
              $rows.Count, ($bytes / 1GB))

if ($Apply -and $rows.Count -gt 0) {
    New-Item -ItemType Directory -Force -Path $manifestRoot | Out-Null
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $manifest = Join-Path $manifestRoot "duplicate-retail-cache-$stamp.csv"
    $rows | Export-Csv -LiteralPath $manifest -NoTypeInformation
    foreach ($row in $rows) {
        Remove-Item -LiteralPath $row.Path -Force
    }
    Write-Output "Manifest: $manifest"
    Write-Output ("Deleted {0} verified duplicate files ({1:N2} GiB)." -f
                  $rows.Count, ($bytes / 1GB))
}
