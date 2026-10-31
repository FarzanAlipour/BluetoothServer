# Builds the BleServer native plugin (CMake, Release) on Windows and copies BleServer.dll into
#   Assets/Plugins/Windows/x86_64/
# Requires: Visual Studio 2022 (Desktop C++ workload, Windows 10/11 SDK) and CMake on PATH.
# macOS/Linux: use build_plugin.sh.
$ErrorActionPreference = 'Stop'

$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Repo = (Resolve-Path (Join-Path $Here '..\..')).Path
$BuildDir = Join-Path $Repo 'build-unity-sample'
$Out = Join-Path $Here 'Assets\Plugins\Windows\x86_64\BleServer.dll'

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { throw 'cmake not found on PATH' }

cmake -S $Repo -B $BuildDir -A x64 -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF "-DUNITY_PROJECT_PATH=$Here"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }
cmake --build $BuildDir --config Release --target BleServer
if ($LASTEXITCODE -ne 0) { throw "cmake build failed ($LASTEXITCODE)" }

if (-not (Test-Path $Out)) { throw "Build finished but $Out is missing" }
# Install the committed import settings (see README, "Why PluginMeta/"): Unity deletes a .meta whose
# binary is missing, so the canonical copies live in PluginMeta\ and are installed next to the binary.
Copy-Item (Join-Path $Here 'PluginMeta\Windows\x86_64\BleServer.dll.meta') "$Out.meta" -Force
Write-Host "Plugin ready: $Out"
