[CmdletBinding()]
param(
    [string]$PythonVersion = "3.12"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$portRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $portRoot "..\.."))
$venvRoot = Join-Path $repoRoot ".venv"
$python = Join-Path $venvRoot "Scripts\python.exe"
$requirements = Join-Path $portRoot "requirements.txt"

Push-Location $repoRoot
try {
    if (-not (Test-Path -LiteralPath $python -PathType Leaf)) {
        & py "-$PythonVersion" -m venv $venvRoot
        if ($LASTEXITCODE -ne 0) {
            throw "Could not create a Python $PythonVersion virtual environment."
        }
    }

    & $python -m pip install --disable-pip-version-check -r $requirements
    if ($LASTEXITCODE -ne 0) {
        throw "Dependency installation failed with exit code $LASTEXITCODE."
    }

    & $python --version
}
finally {
    Pop-Location
}
