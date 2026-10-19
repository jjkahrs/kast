@echo off
rem Runs the editor from the repo root. Usage: run.bat [debug|release|relwithdebinfo] [editor flags...]   (default: debug)
setlocal
cd /d "%~dp0"
set "PRESET=debug"
rem The flags go to the editor as typed: %1 / shift would split a value like -1880,40 at the comma.
set "ARGS= %*"
for %%p in (debug release relwithdebinfo) do if /i "%~1"=="%%p" (
    set "PRESET=%%p"
    set "ARGS="
    for /f "tokens=1,*" %%a in ("%*") do set "ARGS= %%b"
)

if not exist "build\%PRESET%\kast_editor.exe" (
    echo build\%PRESET%\kast_editor.exe not found. Run: build.bat %PRESET%
    exit /b 1
)
"build\%PRESET%\kast_editor.exe"%ARGS%
exit /b %errorlevel%
