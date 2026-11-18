# Runs a real-UDP co-op simulation on this PC: 1 host + (Players-1) guests, each a separate process.
# Usage: powershell -ExecutionPolicy Bypass -File coop\run_sim.ps1 [-Players 4] [-Loss 5] [-Lat 30] [-Jitter 40]
param([int]$Players = 4, [float]$Loss = 0, [int]$Lat = 0, [int]$Jitter = 0, [int]$Port = 47000)
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$sim = "$root\build\sim.exe"
if (-not (Test-Path $sim)) { throw "build sim first (see coop\net\sim)" }
$logs = "$root\build\simlogs"; New-Item -ItemType Directory -Force $logs | Out-Null
Remove-Item "$logs\*.log" -ErrorAction SilentlyContinue
$net = @("--loss", $Loss, "--lat", $Lat, "--jitter", $Jitter)
$procs = @()
$procs += Start-Process $sim -ArgumentList (@("host","--port",$Port,"--name","Host","--players",$Players) + $net) -RedirectStandardOutput "$logs\host.log" -PassThru -WindowStyle Hidden
$null = $procs[0].Handle
Start-Sleep -Milliseconds 400
for ($i = 1; $i -lt $Players; $i++) {
    $procs += Start-Process $sim -ArgumentList (@("join","--addr","127.0.0.1:$Port","--name","Guest$i") + $net) -RedirectStandardOutput "$logs\guest$i.log" -PassThru -WindowStyle Hidden
    $null = $procs[-1].Handle
    Start-Sleep -Milliseconds 150
}
$deadline = (Get-Date).AddSeconds(90)
foreach ($p in $procs) { $left = [int](($deadline - (Get-Date)).TotalMilliseconds); if ($left -lt 1) { $left = 1 }; if (-not $p.WaitForExit($left)) { $p.Kill(); Write-Host "TIMEOUT: killed pid $($p.Id)" } }
$codes = $procs | % { $_.WaitForExit(); $_.ExitCode }
Get-ChildItem $logs -Filter *.log | % { "--- $($_.Name)"; Get-Content $_.FullName | Select-String -Pattern 'connected|RESULTS|#\d|INTERP|sent |LOST|REJECT|GO!|countdown' }
"exit codes: $($codes -join ' ')"
if (($codes | ? { $_ -ne 0 }).Count -gt 0) { exit 1 }
