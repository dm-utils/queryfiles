@echo off
setlocal
cd /d "%~dp0"

set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist %VSWHERE% ( echo vswhere.exe not found & exit /b 1 )

for /f "usebackq tokens=*" %%i in (
    `%VSWHERE% -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`
) do set VS_PATH=%%i
if "%VS_PATH%"=="" ( echo VS C++ tools not found & exit /b 1 )

call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

mkdir build\harness 2>nul

cl /nologo /O2 /EHsc /std:c++17 /utf-8 /W3 ^
   src\test_harness.cpp src\sql_text.cpp src\duck_runner.cpp ^
   /Fe:build\harness\test_harness.exe ^
   /Fo:build\harness\
if errorlevel 1 ( echo Build failed & exit /b 1 )

echo Harness build complete.
exit /b 0
