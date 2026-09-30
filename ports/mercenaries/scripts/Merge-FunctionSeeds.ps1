[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$FunctionsPath,

    [Parameter(Mandatory = $true)]
    [string]$IdentifiedPath,

    [Parameter(Mandatory = $true)]
    [string]$ManualSeedsPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Convert-XboxAddress {
    param([Parameter(Mandatory = $true)]$Value)

    $text = ([string]$Value).Trim()
    if ($text.StartsWith("0x", [System.StringComparison]::OrdinalIgnoreCase)) {
        $digits = $text.Substring(2)
        if ($digits -notmatch '^[0-9A-Fa-f]+$') {
            throw "Invalid hexadecimal Xbox address: '$text'."
        }
        return [uint32]::Parse(
            $digits,
            [System.Globalization.NumberStyles]::HexNumber,
            [System.Globalization.CultureInfo]::InvariantCulture
        )
    }
    if ($text -notmatch '^[0-9]+$') {
        throw "Invalid decimal Xbox address: '$text'."
    }
    return [uint32]::Parse($text, [System.Globalization.CultureInfo]::InvariantCulture)
}

# Windows PowerShell 5.1 emits a top-level JSON array as one non-enumerated
# pipeline object, while PowerShell 7 enumerates it. Assign first, then pass
# through a fresh pipeline so both versions produce a flat array.
$parsedFunctions = Get-Content -LiteralPath $FunctionsPath -Raw | ConvertFrom-Json
$parsedIdentified = Get-Content -LiteralPath $IdentifiedPath -Raw | ConvertFrom-Json
$parsedManualSeeds = Get-Content -LiteralPath $ManualSeedsPath -Raw | ConvertFrom-Json
$functions = @($parsedFunctions | ForEach-Object { $_ })
$identified = @($parsedIdentified | ForEach-Object { $_ })
$manualSeeds = @($parsedManualSeeds | ForEach-Object { $_ })

$orderedFunctions = @(
    $functions |
        ForEach-Object {
            [pscustomobject]@{
                Start = [uint32](Convert-XboxAddress -Value $_.start)
                End = [uint32](Convert-XboxAddress -Value $_.end)
            }
        } |
        Sort-Object Start
)

[uint32[]]$functionStarts = @($orderedFunctions | ForEach-Object { $_.Start })
[uint32[]]$functionEnds = @($orderedFunctions | ForEach-Object { $_.End })
$knownStarts = [System.Collections.Generic.HashSet[uint32]]::new()
foreach ($address in $functionStarts) {
    [void]$knownStarts.Add($address)
}

function Test-InsideKnownFunction {
    param([uint32]$Address)

    $index = [Array]::BinarySearch($functionStarts, $Address)
    if ($index -ge 0) {
        return $false
    }

    $priorIndex = (-$index) - 2
    if ($priorIndex -lt 0) {
        return $false
    }

    return ($functionStarts[$priorIndex] -lt $Address -and
            $Address -lt $functionEnds[$priorIndex])
}

$accepted = [System.Collections.Generic.HashSet[uint32]]::new()
$rejectedInterior = 0

foreach ($item in $identified) {
    $address = [uint32](Convert-XboxAddress -Value $item.start)
    if (-not $knownStarts.Contains($address) -and
        (Test-InsideKnownFunction -Address $address)) {
        $rejectedInterior++
        continue
    }
    [void]$accepted.Add($address)
}

# Manual seeds are reviewed entry points and intentionally bypass the
# interior-address filter.
foreach ($item in $manualSeeds) {
    [void]$accepted.Add([uint32](Convert-XboxAddress -Value $item.start))
}

$output = @(
    $accepted |
        Sort-Object |
        ForEach-Object {
            [pscustomobject]@{ start = ("0x{0:X8}" -f $_) }
        }
)

$parent = Split-Path -Parent $OutputPath
if ($parent) {
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
}
$json = $output | ConvertTo-Json -Depth 4
[System.IO.File]::WriteAllText(
    [System.IO.Path]::GetFullPath($OutputPath),
    $json + [Environment]::NewLine,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host ("Accepted {0} seeds; rejected {1} classifier entries inside known functions." -f
    $output.Count, $rejectedInterior)
