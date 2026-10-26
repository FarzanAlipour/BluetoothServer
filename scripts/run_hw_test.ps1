# Build Release and run ble_test_server (Windows) with verbose logging.
# The log goes to logs\hw-test-windows-<timestamp>.log (git-ignored): attach it to bug reports.
# Extra arguments are passed to ble_test_server, e.g.:  .\scripts\run_hw_test.ps1 --name MyServer --heartbeat-ms 500
# If scripts are blocked:  powershell -ExecutionPolicy Bypass -File .\scripts\run_hw_test.ps1
# Procedure and checklist: docs\TESTING.md
$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')

$buildDir = 'build-hw'
cmake -S . -B $buildDir
if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }
cmake --build $buildDir --config Release --target ble_test_server
if ($LASTEXITCODE -ne 0) { throw 'cmake build failed' }

$exe = Join-Path $buildDir 'examples\Release\ble_test_server.exe'
if (-not (Test-Path $exe)) { throw "not found: $exe" }

New-Item -ItemType Directory -Force -Path logs | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$log = "logs\hw-test-windows-$stamp.log"
$commit = (git rev-parse --short HEAD 2>$null)
@(
    '# ble_test_server hardware test',
    "# os: windows  $([System.Environment]::OSVersion.VersionString)",
    "# commit: $commit",
    "# started: $(Get-Date)"
) | Set-Content -Path $log

Write-Host "Logging to $log (Ctrl-C to stop; commands: n = notify, l = large message, s = stop/start advertising, q = quit)"
# Merge stderr into stdout and tee into the log file; the exit code is reported, not thrown.
& $exe --verbose @args 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath $log -Append
Write-Host "Exit code: $LASTEXITCODE. Log saved: $log"
