@echo off
setlocal
cd /d "%~dp0"

:: -- Find Visual Studio --
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist %VSWHERE% ( echo vswhere.exe not found & exit /b 1 )

for /f "usebackq tokens=*" %%i in (
    `%VSWHERE% -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`
) do set VS_PATH=%%i
if "%VS_PATH%"=="" ( echo VS C++ tools not found & exit /b 1 )

call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

mkdir build 2>nul

:: -- Compile resource --
rc.exe /nologo /fo build\settings.res src\settings.rc
if errorlevel 1 ( echo Resource compile failed & exit /b 1 )

:: -- Compile and link (options before sources to avoid D9026) --
cl /LD /O2 /EHsc /std:c++17 /MT /utf-8 /W3 ^
   src\dllmain.cpp src\settings_dialog.cpp src\result_grid.cpp src\sql_text.cpp src\duck_runner.cpp ^
   build\settings.res ^
   /Fe:build\QueryFiles.dll ^
   /Fo:build\ ^
   /link user32.lib gdi32.lib shell32.lib comctl32.lib comdlg32.lib
if errorlevel 1 ( echo Build failed & exit /b 1 )

if /i "%1"=="nodeploy" ( echo Build complete, not deployed. & exit /b 0 )

:: -- Close Notepad++ --
taskkill /f /im notepad++.exe >nul 2>&1
ping -n 2 127.0.0.1 >nul 2>&1

:: -- Deploy DLL --
set DST=C:\Program Files\Notepad++\plugins\QueryFiles
if not exist "%DST%" mkdir "%DST%"
copy /y "build\QueryFiles.dll" "%DST%\QueryFiles.dll"
if errorlevel 1 ( echo Copy failed & exit /b 1 )
copy /y "src\help.txt" "%DST%\help.txt"
if errorlevel 1 ( echo Help file copy failed & exit /b 1 )

:: -- Restart Notepad++ --
:: Via explorer.exe, so Notepad++ never inherits this (elevated) prompt's
:: admin rights - an elevated Notepad++ ignores messages from normal apps.
explorer.exe "C:\Program Files\Notepad++\notepad++.exe"

echo Build and deploy complete.
exit /b 0
