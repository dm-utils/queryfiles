@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

call .\build_harness.cmd %1 >nul
if errorlevel 1 ( echo Harness build failed & exit /b 1 )

set H=.\build\harness\test_harness.exe
set FAIL=0

:: -- pure sql_text tests: mode | fixture | golden-suffix --
for %%C in (
    "split  split_in.sql   split"
    "this   this_in.sql    this"
    "header header_in.sql  header"
    "script q_basic.sql    script"
    "csv    csv_in.txt     csv"
) do (
    for /f "tokens=1,2,3" %%a in (%%C) do (
        %H% %%a tests\%%b > tests\actual_%%c.txt
        fc /a tests\golden_%%c.txt tests\actual_%%c.txt >nul
        if errorlevel 1 ( echo MISMATCH: %%c & set FAIL=1 ) else ( echo OK: %%c )
    )
)

:: -- end-to-end through duckdb.exe (QF_DUCKDB, else PATH) --
if "%QF_DUCKDB%"=="" for /f "delims=" %%d in ('where duckdb 2^>nul') do if "!QF_DUCKDB!"=="" set "QF_DUCKDB=%%d"
if "%QF_DUCKDB%"=="" (
    echo SKIP: duckdb tests ^(set QF_DUCKDB or put duckdb.exe on PATH^)
) else (
    for %%C in (
        "q_basic.sql text duck_basic_text"
        "q_basic.sql grid duck_basic_grid"
        "q_error.sql text duck_error_text"
    ) do (
        for /f "tokens=1,2,3" %%a in (%%C) do (
            %H% duck "%QF_DUCKDB%" tests\%%a orders_nl.csv %%b > tests\actual_%%c.txt
            fc /a tests\golden_%%c.txt tests\actual_%%c.txt >nul
            if errorlevel 1 ( echo MISMATCH: %%c & set FAIL=1 ) else ( echo OK: %%c )
        )
    )
)

if !FAIL! neq 0 (
    echo.
    echo Output regressed. If the new output is correct, review it and copy
    echo tests\actual_*.txt over tests\golden_*.txt.
    exit /b 1
)

echo.
echo All regression tests passed.
exit /b 0
