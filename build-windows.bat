@echo off
REM Quick build script for existing Visual Studio project

echo === Unity BLE Server Build Script ===
echo.

REM Check if we're in a Visual Studio Developer Command Prompt
where msbuild >nul 2>nul
if %errorlevel% neq 0 (
    echo [ERROR] MSBuild not found!
    echo Please run this script from a Visual Studio Developer Command Prompt
    echo.
    echo To open it:
    echo 1. Open Start Menu
    echo 2. Search for "Developer Command Prompt for VS 2019" or VS 2022
    echo 3. Run this script again
    pause
    exit /b 1
)

REM Find the solution file
set SOLUTION_FILE=
if exist "*.sln" (
    for %%F in (*.sln) do set SOLUTION_FILE=%%F
) else if exist "BluetoothServer.sln" (
    set SOLUTION_FILE=BluetoothServer.sln
) else (
    echo [ERROR] No solution file found!
    echo Please run this script from your project directory
    pause
    exit /b 1
)

echo Found solution: %SOLUTION_FILE%
echo.

REM Clean previous build
echo Cleaning previous build...
msbuild %SOLUTION_FILE% /t:Clean /p:Configuration=Release /p:Platform=x64 >nul 2>&1

REM Build the project
echo Building Release x64...
msbuild %SOLUTION_FILE% /t:Build /p:Configuration=Release /p:Platform=x64

if %errorlevel% neq 0 (
    echo.
    echo [ERROR] Build failed!
    echo Check the error messages above.
    pause
    exit /b 1
)

echo.
echo === Build Successful! ===
echo.

REM Find the output DLL
set OUTPUT_DIR=x64\Release
if not exist %OUTPUT_DIR%\*.dll (
    set OUTPUT_DIR=Release
)

if exist %OUTPUT_DIR%\*.dll (
    echo Output files:
    dir /b %OUTPUT_DIR%\*.dll
    echo.
    echo Location: %CD%\%OUTPUT_DIR%
) else (
    echo [WARNING] Could not find output DLL
)

echo.
echo Next steps:
echo 1. Copy the DLL to your Unity project's Assets/Plugins/Windows/x86_64/ folder
echo 2. Copy unity/BleServer.cs to your Unity project's Scripts folder
echo 3. Import in Unity and start using!

pause