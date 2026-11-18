# Builds coop components with Zig (32-bit). Usage: .\coop\build.ps1 [-Target trace|sim|all]
param([string]$Target = "all")
$ErrorActionPreference = "Stop"
$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$out  = Join-Path $root "build"
New-Item -ItemType Directory -Force $out | Out-Null

if ($Target -in "trace","all") {
    # Upstream trace hook, built unmodified for reference/testing only.
    $src = Join-Path $root "..\nfs13\client\trace_hook"
    zig cc -target x86-windows-gnu -O2 -shared -s "$src\proxy_d3d11.c" "$src\d3d11.def" -o "$out\trace_d3d11.dll" -lws2_32
    if ($LASTEXITCODE -ne 0) { throw "trace hook build failed" }
    Write-Host "built $out\trace_d3d11.dll"
}
if ($Target -in "sim","all") {
    $net = Join-Path $root "net"
    $srcs = @("$net\sim\sim.c","$net\cn_interp.c","$net\cn_session.c","$net\cn_transports.c")
    zig cc -Wall -Wextra -std=c11 -O2 @srcs -lws2_32 -o "$out\sim.exe"
    if ($LASTEXITCODE -ne 0) { throw "sim build failed" }
    zig cc -target x86-windows-gnu -Wall -Wextra -std=c11 -O2 @srcs -lws2_32 -o "$out\sim32.exe"
    if ($LASTEXITCODE -ne 0) { throw "sim32 build failed" }
    Write-Host "built sim.exe (64-bit) and sim32.exe (32-bit, like the game)"
}
# Tests: run_tests.ps1. Later phases add the coop hook DLL (Phase 2+).
