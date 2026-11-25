param([string]$CsvDirectory)
$ErrorActionPreference = 'Stop'
if ($CsvDirectory) { $CsvDirectory = (Resolve-Path -LiteralPath $CsvDirectory).Path }

Push-Location $PSScriptRoot
$stage = $null
try {
    if (-not (Test-Path -LiteralPath build/harborshift.exe)) { & ./build.ps1 }
    $buildRoot = (Resolve-Path -LiteralPath build).Path
    $stage = Join-Path $buildRoot ('csv-import-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $stage | Out-Null
    if (-not $CsvDirectory) {
        $zip = 'harber_shift_sample_datasets/harber_shift_sample_datasets.zip'
        & tar.exe -xf $zip -C $stage
        if ($LASTEXITCODE -ne 0) { throw 'Could not extract the bundled dataset ZIP.' }
        $CsvDirectory = Join-Path $stage 'harber_shift_sample_datasets'
    }

    $snapshot = Join-Path $stage 'network.json'
    $csvPath = Join-Path $CsvDirectory '.'
    $arguments = 'import "' + $csvPath + '"'
    $result = Start-Process -FilePath (Join-Path $buildRoot 'harborshift.exe') -ArgumentList $arguments `
        -WorkingDirectory $PSScriptRoot -RedirectStandardOutput $snapshot -NoNewWindow -Wait -PassThru
    if ($result.ExitCode -ne 0 -or (Get-Item -LiteralPath $snapshot).Length -eq 0) {
        throw 'CSV validation failed. The existing dataset was kept.'
    }
    Move-Item -LiteralPath $snapshot -Destination (Join-Path $PSScriptRoot 'data/network.json') -Force
    Write-Output 'Rebuilt data/network.json from the CSV dataset.'
} finally {
    if ($stage -and (Test-Path -LiteralPath $stage)) {
        $resolved = (Resolve-Path -LiteralPath $stage).Path
        if ([IO.Path]::GetDirectoryName($resolved) -eq $buildRoot -and
            [IO.Path]::GetFileName($resolved) -match '^csv-import-[0-9a-f]{32}$') {
            Remove-Item -LiteralPath $resolved -Recurse -Force
        }
    }
    Pop-Location
}
