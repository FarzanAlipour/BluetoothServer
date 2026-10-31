# Runs the EditMode tests in Unity batchmode (-nographics) on Windows.
# Builds the native plugin first when it is missing. Override the Editor with $env:UNITY.
$ErrorActionPreference = 'Stop'

$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Version = '2022.3.62f3'
$Unity = $env:UNITY
if (-not $Unity) { $Unity = "C:\Program Files\Unity\Hub\Editor\$Version\Editor\Unity.exe" }
if (-not (Test-Path $Unity)) { throw "Unity $Version not found at $Unity (set `$env:UNITY)" }

$Plugin = Join-Path $Here 'Assets\Plugins\Windows\x86_64\BleServer.dll'
if (-not (Test-Path $Plugin)) { & (Join-Path $Here 'build_plugin.ps1') }

Copy-Item (Join-Path $Here 'PluginMeta\Windows\x86_64\BleServer.dll.meta') "$Plugin.meta" -Force

$Results = Join-Path $Here 'TestResults-editmode.xml'
$Log = Join-Path $Here 'test-run.log'
Remove-Item $Results -ErrorAction SilentlyContinue

$p = Start-Process -FilePath $Unity -Wait -PassThru -ArgumentList @(
    '-batchmode', '-nographics', '-projectPath', "`"$Here`"",
    '-runTests', '-testPlatform', 'EditMode',
    '-testResults', "`"$Results`"", '-logFile', "`"$Log`"")

if (Test-Path $Results) {
    [xml]$xml = Get-Content $Results
    $run = $xml.'test-run'
    Write-Host "result=$($run.result) total=$($run.total) passed=$($run.passed) failed=$($run.failed)"
    $xml.SelectNodes('//test-case') | ForEach-Object { Write-Host "  $($_.result)  $($_.name)" }
}
if ($p.ExitCode -ne 0) { Write-Error "Unity exited with code $($p.ExitCode) (see $Log)" }
exit $p.ExitCode
