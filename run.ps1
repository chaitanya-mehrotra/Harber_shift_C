param([ValidateRange(1,65535)][int]$Port = 8000)
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    if (-not (Test-Path build/harborshift.exe)) { & ./build.ps1 }
    & ./build/harborshift.exe $Port
    if ($LASTEXITCODE -ne 0) { throw 'The HarborShift C server exited with an error.' }
} finally {
    Pop-Location
}
