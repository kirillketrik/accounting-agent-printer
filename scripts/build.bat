@echo off
setlocal enabledelayedexpansion
REM Builds this agent from the command line (no CLion needed): Visual Studio
REM 2022 Build Tools with the "Desktop development with C++" workload.
REM
REM Usage:  scripts\build.bat [Debug|Release] [path-to-accounting-agent-core]
REM The core is taken from the argument, else from ..\accounting-agent-core, else
REM fetched by CMake from ACCOUNTING_AGENT_CORE_GIT.

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"
if /I not "%CONFIG%"=="Debug" if /I not "%CONFIG%"=="Release" (
    echo ERROR: unknown config "%CONFIG%" - expected "Debug" or "Release".
    exit /b 1
)
set "CORE_ARG="
if not "%~2"=="" set "CORE_ARG=-DACCOUNTING_AGENT_CORE_DIR=%~f2"

set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
if /I "%CONFIG%"=="Debug" (set "BUILD_DIR=%ROOT%\cmake-build-debug") else (set "BUILD_DIR=%ROOT%\cmake-build-release")

echo === Build: %CONFIG% ===
echo Project root: %ROOT%
echo Build dir:    %BUILD_DIR%
echo.

REM --- cmake / ninja: PATH, then CLion's copies; Visual Studio's after VS is located ---
set "CMAKE_EXE="
where cmake >nul 2>&1
if %errorlevel%==0 set "CMAKE_EXE=cmake"
if "%CMAKE_EXE%"=="" if exist "%LOCALAPPDATA%\Programs\CLion\bin\cmake\win\x64\bin\cmake.exe" set "CMAKE_EXE=%LOCALAPPDATA%\Programs\CLion\bin\cmake\win\x64\bin\cmake.exe"
set "NINJA_EXE="
where ninja >nul 2>&1
if %errorlevel%==0 set "NINJA_EXE=ninja"
if "%NINJA_EXE%"=="" if exist "%LOCALAPPDATA%\Programs\CLion\bin\ninja\win\x64\ninja.exe" set "NINJA_EXE=%LOCALAPPDATA%\Programs\CLion\bin\ninja\win\x64\ninja.exe"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found - is Visual Studio / Build Tools 2022 installed?
    exit /b 1
)
set "VSINSTALL="
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VSINSTALL=%%I"
)
if "%VSINSTALL%"=="" (
    echo ERROR: No Visual Studio install with the C++ ^(VC.Tools^) workload was found.
    exit /b 1
)
echo Using Visual Studio at: %VSINSTALL%

set "VSTOOLS=%VSINSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake"
if "%CMAKE_EXE%"=="" if exist "%VSTOOLS%\CMake\bin\cmake.exe" set "CMAKE_EXE=%VSTOOLS%\CMake\bin\cmake.exe"
if "%NINJA_EXE%"=="" if exist "%VSTOOLS%\Ninja\ninja.exe" set "NINJA_EXE=%VSTOOLS%\Ninja\ninja.exe"
if "%CMAKE_EXE%"=="" (
    echo ERROR: cmake.exe not found on PATH, in CLion or in Visual Studio.
    exit /b 1
)
if "%NINJA_EXE%"=="" (
    echo ERROR: ninja.exe not found on PATH, in CLion or in Visual Studio.
    exit /b 1
)
echo Using CMake: %CMAKE_EXE%
echo Using Ninja: %NINJA_EXE%

call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo ERROR: vcvars64.bat failed to set up the x64 MSVC environment.
    exit /b 1
)

echo.
echo --- Configuring ---
"%CMAKE_EXE%" -G Ninja -S "%ROOT%" -B "%BUILD_DIR%" -DCMAKE_MAKE_PROGRAM="%NINJA_EXE%" -DCMAKE_BUILD_TYPE=%CONFIG% %CORE_ARG%
if errorlevel 1 (
    echo ERROR: CMake configure failed.
    exit /b 1
)

echo.
echo --- Building ---
"%CMAKE_EXE%" --build "%BUILD_DIR%"
if errorlevel 1 (
    echo ERROR: Build failed.
    exit /b 1
)

echo.
echo === Build succeeded: %BUILD_DIR% ===
for %%F in ("%BUILD_DIR%\*_setup.exe") do echo Installer: %%~fF
echo Or copy the whole folder and run install_service.bat.
