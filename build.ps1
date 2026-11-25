$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    New-Item -ItemType Directory -Force build | Out-Null
    $sources = @(
        'backend/server.c', 'backend/cli.c', 'backend/data.c', 'backend/common.c', 'backend/routing.c', 'backend/impact.c',
        'backend/cascade.c', 'backend/recovery.c', 'backend/scenario.c', 'backend/news.c',
        'backend/model.c', 'backend/vendor/cJSON.c', 'backend/vendor/yxml.c'
    )
    & gcc -std=gnu11 -D__USE_MINGW_ANSI_STDIO=1 -O2 -Wall -Wextra -Wno-unused-parameter @sources -o build/harborshift.exe -lws2_32 -lwininet -lrpcrt4
    if ($LASTEXITCODE -ne 0) { throw 'The HarborShift C build failed.' }
} finally {
    Pop-Location
}
