# Builds and runs the co-op net library tests (native build). Usage: powershell -ExecutionPolicy Bypass -File coop\run_tests.ps1
$ErrorActionPreference = "Stop"
$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
New-Item -ItemType Directory -Force "$root\build" | Out-Null
Push-Location "$root\net"
try {
    zig cc -Wall -Wextra -std=c11 -O1 -g test_net.c cn_interp.c cn_session.c cn_transports.c -lws2_32 -o "$root\build\test_net.exe"
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
    & "$root\build\test_net.exe"
    exit $LASTEXITCODE
} finally { Pop-Location }
