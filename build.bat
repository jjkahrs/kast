@echo off
rem Builds the editor. Usage: build.bat [debug|release|relwithdebinfo]   (default: relwithdebinfo)
setlocal
set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=relwithdebinfo"
if /i not "%PRESET%"=="debug" if /i not "%PRESET%"=="release" if /i not "%PRESET%"=="relwithdebinfo" (
    echo Usage: build.bat [debug^|release^|relwithdebinfo]
    exit /b 2
)

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
where cl >nul 2>&1 || (
    echo VS 2022 x64 build tools not found. See README.md, Prerequisites.
    exit /b 1
)
rem vcvars64 points VCPKG_ROOT at Visual Studio's bundled vcpkg; use ours.
set "VCPKG_ROOT=C:/vcpkg"

cd /d "%~dp0"
cmake --preset %PRESET% || exit /b 1
cmake --build --preset %PRESET% || exit /b 1
echo Built build\%PRESET%\kast_editor.exe
