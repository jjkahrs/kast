@echo off
rem Runs the editor from the repo root. Usage: run.bat [debug|release|relwithdebinfo] [editor flags...]   (default: debug)
setlocal
cd /d "%~dp0" &rem before any shift: shift also moves %0
set "PRESET=debug"
if /i "%~1"=="release" (set "PRESET=release" & shift) else if /i "%~1"=="relwithdebinfo" (
    set "PRESET=relwithdebinfo" & shift
) else if /i "%~1"=="debug" shift

set "ARGS="
:collect
if "%~1"=="" goto run
set ARGS=%ARGS% %1
shift
goto collect

:run
if not exist "build\%PRESET%\kast_editor.exe" (
    echo build\%PRESET%\kast_editor.exe not found. Run: build.bat %PRESET%
    exit /b 1
)
"build\%PRESET%\kast_editor.exe"%ARGS%
exit /b %errorlevel%
