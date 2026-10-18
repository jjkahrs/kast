@echo off
rem Runs the zone builder from the repo root. Usage: run.bat [debug|release] [zone builder flags...]   (default: debug)
rem Example: run.bat --project samples\starter-mmo --map meadow
setlocal
cd /d "%~dp0..\.."
set "PRESET=debug"
rem Forward %* raw: shift/%1 would split comma args like --window-pos X,Y.
set "ARGS=%*"
if /i "%~1"=="release" set "PRESET=release"
if /i "%~1"=="%PRESET%" call set "ARGS=%%ARGS:*%~1=%%"

if not exist "build\%PRESET%\kast_zone_builder.exe" (
    echo build\%PRESET%\kast_zone_builder.exe not found. Run: build.bat %PRESET%
    exit /b 1
)
"build\%PRESET%\kast_zone_builder.exe" %ARGS%
exit /b %errorlevel%
