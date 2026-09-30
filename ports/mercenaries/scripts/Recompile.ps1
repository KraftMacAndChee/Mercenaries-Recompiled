[CmdletBinding()]
param(
    [string]$GameDirectory,
    [ValidateRange(2, 64)]
    [int]$MaxPasses = 32
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$portRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $portRoot "..\.."))

if (-not $GameDirectory) {
    $GameDirectory = Join-Path $repoRoot "game_files\mercenaries-retail"
}
$gameRoot = [System.IO.Path]::GetFullPath($GameDirectory)
$xbePath = Join-Path $gameRoot "default.xbe"
$python = Join-Path $repoRoot ".venv\Scripts\python.exe"
$analysisRoot = Join-Path $portRoot "analysis"
$disasmRoot = Join-Path $analysisRoot "disasm"
$funcIdRoot = Join-Path $analysisRoot "func_id"
$recompRoot = Join-Path $analysisRoot "recomp"
$xbeJson = Join-Path $analysisRoot "xbe.json"
$generatedRoot = Join-Path $portRoot "src\recomp\gen"
$manualSeeds = Join-Path $portRoot "manual-seeds.json"
$mergedSeeds = Join-Path $analysisRoot "seeds.json"
$mergeScript = Join-Path $PSScriptRoot "Merge-FunctionSeeds.ps1"
$expectedXbeHash = "AA08EA21D952AC35F49C02C7E2ED08AA25AD7535FBBBCCC95636775F37BE99D7"

if (-not (Test-Path -LiteralPath $python -PathType Leaf)) {
    throw "Python environment not found. Run Setup-Python.ps1 first."
}
if (-not (Test-Path -LiteralPath $xbePath -PathType Leaf)) {
    throw "default.xbe not found at: $xbePath"
}

$actualHash = (Get-FileHash -LiteralPath $xbePath -Algorithm SHA256).Hash
if ($actualHash -ne $expectedXbeHash) {
    throw "Unsupported default.xbe revision. Expected $expectedXbeHash, got $actualHash."
}

New-Item -ItemType Directory -Force -Path $analysisRoot | Out-Null

function Invoke-Python {
    param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments)

    & $python @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Python command failed with exit code $LASTEXITCODE."
    }
}

Push-Location $repoRoot
try {
    Write-Host "Parsing XBE metadata..."
    $parserArgs = @("-m", "tools.xbe_parser", $xbePath, "--json", $xbeJson, "--quiet")
    Invoke-Python @parserArgs

    $seedPath = $manualSeeds
    $converged = $false

    for ($pass = 1; $pass -le $MaxPasses; $pass++) {
        Write-Host ""
        Write-Host "Analysis pass $pass of $MaxPasses..."

        # Function counts alone are not a fixed-point criterion. Vtable and
        # callback-table classification can discover a new indirect entry while
        # another speculative candidate disappears, leaving the count unchanged.
        # Hash the ordered seed file before this pass and require the merged seed
        # set itself to stabilize before lifting disposable generated C.
        $inputSeedHash = (Get-FileHash -LiteralPath $seedPath -Algorithm SHA256).Hash

        $disasmArgs = @(
            "-m", "tools.disasm", $xbePath,
            "-o", $disasmRoot,
            "--analysis-json", $xbeJson,
            "--extra-sections", "XMV,XACTENG,D3D,XGRPH,DSOUND,WMADEC,XPP,DOLBY",
            "--seed-functions", $seedPath,
            # Only human-reviewed entries are hard instruction boundaries.
            # The merged file also contains automatic classifier candidates,
            # which must remain rejectable when they overlap real functions.
            "--protected-seed-functions", $manualSeeds,
            "--force"
        )
        Invoke-Python @disasmArgs

        $functionsPath = Join-Path $disasmRoot "functions.json"
        $stringsPath = Join-Path $disasmRoot "strings.json"
        $xrefsPath = Join-Path $disasmRoot "xrefs.json"

        $identifyArgs = @(
            "-m", "tools.func_id", $xbePath,
            "--functions", $functionsPath,
            "--strings", $stringsPath,
            "--xrefs", $xrefsPath,
            "-o", $funcIdRoot
        )
        Invoke-Python @identifyArgs

        & $mergeScript -FunctionsPath $functionsPath -IdentifiedPath (Join-Path $funcIdRoot "identified_functions.json") -ManualSeedsPath $manualSeeds -OutputPath $mergedSeeds

        $parsedFunctions = Get-Content -LiteralPath $functionsPath -Raw |
            ConvertFrom-Json
        $functionCount = @($parsedFunctions | ForEach-Object { $_ }).Count
        $parsedSeeds = Get-Content -LiteralPath $mergedSeeds -Raw |
            ConvertFrom-Json
        $seedCount = @($parsedSeeds | ForEach-Object { $_ }).Count
        $outputSeedHash = (Get-FileHash -LiteralPath $mergedSeeds -Algorithm SHA256).Hash
        Write-Host "Detected $functionCount functions and retained $seedCount entry seeds."

        if ($outputSeedHash -eq $inputSeedHash) {
            $converged = $true
            break
        }

        $seedPath = $mergedSeeds
    }

    if (-not $converged) {
        Write-Warning "Function discovery did not converge after $MaxPasses passes."
    }

    if (Test-Path -LiteralPath $generatedRoot) {
        $generatedFiles = @(Get-ChildItem -LiteralPath $generatedRoot -File)
        if ($generatedFiles.Count -gt 0) {
            Write-Host ""
            Write-Host "Archiving generated-tree provenance before replacement..."
            $archiveScript = Join-Path $PSScriptRoot "Archive-GeneratedProvenance.py"
            $archiveRoot = Join-Path $repoRoot "artifacts\provenance\generated-trees"
            Invoke-Python $archiveScript `
                --generated-root $generatedRoot `
                --repo-root $repoRoot `
                --port-root $portRoot `
                --xbe $xbePath `
                --archive-root $archiveRoot
        }
        Get-ChildItem -LiteralPath $generatedRoot -File | Remove-Item -Force
    }

    Write-Host ""
    Write-Host "Lifting functions to generated C..."
    $recompArgs = @(
        "-m", "tools.recomp", $xbePath,
        "--all",
        "--split", "1000",
        # Weapon-selection event predicate: retain both retail return paths.
        "--function-range", "0x00110D00:0x00110D5B",
        # Vehicle-controller constructor. Function discovery mistakes the
        # instruction at 0x00150014 for a separate vtable entry and truncates
        # the real 0x0014FF00 routine before its three controller initializers
        # and stack epilogue. Keep the reviewed retail control-flow range whole.
        "--function-range", "0x0014FF00:0x0015009E",
        "--function-range", "0x001B10B0:0x001B169F",
        "--function-range", "0x001B1BA0:0x001B27E8",
        "--function-range", "0x001B9EB0:0x001BAC1A",
        # Retail heterogeneous comparator. Its type jump table reaches cases
        # through 0x65AA2 that share the entry frame and true/false epilogues.
        "--function-range", "0x000658B0:0x00065AA5",
        # Camera update method. Its mode jump table exposes interior entries
        # that must retain the original ESI/EDI frame and shared epilogue.
        "--function-range", "0x0008DB70:0x0008DC9D",
        # DataPod Status update. Its display-mode jump table and timer paths
        # share the 0x64-byte entry frame and common update epilogue.
        "--function-range", "0x000CB120:0x000CB4DC",
        # Front-end initialization has a shared continuation after an
        # undecoded table-shaped gap. Keep the reviewed retail owner intact;
        # ending it at 0x000E000A leaves startup without its state update,
        # stack cleanup, and tail call to 0x000E0181.
        "--function-range", "0x000DFF60:0x000E0097",
        # Vehicle physics update. Keep its event-mode jump table and shared
        # state-copy/stack epilogues in the original ESI/EDI/EBP frame.
        "--function-range", "0x0014F720:0x0014FEDE",
        # Asynchronous load/save state machine. All status jump-table cases
        # share the ECX/ESI/EDI/EBP frame and completion epilogues.
        "--function-range", "0x0018D2D0:0x0018D45E",
        # Lua string-format and topointer use internal jump-table cases that
        # share their entry frames and must remain local control-flow blocks.
        "--function-range", "0x001DAFA0:0x001DB0C5",
        "--function-range", "0x001DCF00:0x001DCF40",
        # Havok constraint command-stream builder. Its opcode jump table
        # targets interior blocks from 0x1C5456 through 0x1C5941 and exits
        # through the shared epilogue at 0x1C5990. Keep the whole retail
        # range together so those targets cannot become minimal-ret stubs.
        "--function-range", "0x001C53F0:0x001C5999",
        # Havok contact-response jump-table case; 0x231A01-0x231B05 is a
        # required shared tail that function discovery otherwise omits.
        "--function-range", "0x002318A2:0x00231B06",
        # Retail D3D state compiler. The indexed switch at 0x002966A7 enters
        # case blocks through 0x00296993, all of which retain the entry's
        # 0xB8-byte local frame and exit through ret 8 at 0x00296B44.
        "--function-range", "0x002965F0:0x00296B47",
        # Authored item-cache pickup rejection, mounted helicopter reticles,
        # and every winch update state must keep their native shared frames.
        "--function-range", "0x000330A0:0x00033457",
        "--function-range", "0x00108A00:0x00108E7D",
        "--function-range", "0x00145460:0x001455D2",
        "--function-range", "0x00162610:0x00162652",
        "--function-range", "0x00167E40:0x00168016",
        "--gen-dir", $generatedRoot,
        "--manual-function", "0x000112B0",
        "--manual-function", "0x00178870",
        "--manual-function", "0x001E26E0",
        "--manual-function", "0x001E89E0",
        "--manual-function", "0x001E91ED",
        "--manual-function", "0x001F9D70",
        "--manual-function", "0x001F9DA0",
        "--manual-function", "0x001F9DE0",
        "--manual-function", "0x0022B150",
        "--manual-function", "0x0022B178",
        "--manual-function", "0x0022DDA4",
        "--manual-function", "0x0022E51F",
        "--manual-function", "0x0022E713",
        "--manual-function", "0x0022F49E",
        "--manual-function", "0x0022F51C",
        "--manual-function", "0x0022F5A7",
        "--manual-function", "0x0022F6A5",
        "--manual-function", "0x0022F7C6",
        "--manual-function", "0x0022F9BC",
        "--manual-function", "0x0022FA9E",
        "--disasm-dir", $disasmRoot,
        "--func-id-dir", $funcIdRoot,
        "--manual-function", "0x0023BE3C",
        "--manual-function", "0x002A6713",
        "-o", $recompRoot
    )
    Invoke-Python @recompArgs

    $patchGenerated = Join-Path $PSScriptRoot "Patch-Generated.py"
    Invoke-Python $patchGenerated $generatedRoot

    $summary = Get-Content -LiteralPath (Join-Path $recompRoot "summary.json") -Raw | ConvertFrom-Json
    Write-Host ""
    Write-Host ("Lift complete: {0}/{1} functions, {2} failures, {3} unresolved direct targets." -f $summary.translated, $summary.total, $summary.failed, $summary.unresolved_stubs)
}
finally {
    Pop-Location
}
