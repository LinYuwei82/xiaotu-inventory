@echo off
setlocal EnableExtensions
rem xiaotu-inventory Windows release helper.
rem Initializes the MSVC x64 environment (vcvars64) then calls package_windows.ps1.
rem
rem Usage:
rem   package_windows.cmd                  -- build number from current UTC time
rem   package_windows.cmd 202611180930     -- explicit build number
rem
rem Note: keep this file ASCII + CRLF. cmd.exe decodes batch files with the
rem current console code page, so non-ASCII text breaks parsing on GBK systems.

set "SCRIPT_DIR=%~dp0"
pushd "%SCRIPT_DIR%..\.." || exit /b 1

set "VSPATH="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" call :use_vswhere
if not defined VSPATH call :probe_paths
if not defined VSPATH goto :no_vs

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto :vcvars_failed

if "%~1"=="" (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%package_windows.ps1"
) else (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%package_windows.ps1" -Build "%~1"
)
set "RC=%ERRORLEVEL%"

popd
exit /b %RC%

:use_vswhere
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
exit /b 0

:probe_paths
for %%e in (Community Professional Enterprise BuildTools) do call :probe_one "%%e"
exit /b 0

:probe_one
if defined VSPATH exit /b 0
set "CAND=%ProgramFiles%\Microsoft Visual Studio\2022\%~1"
if exist "%CAND%\VC\Auxiliary\Build\vcvars64.bat" set "VSPATH=%CAND%"
exit /b 0

:no_vs
echo [ERROR] Visual Studio 2022 C++ toolchain not found.
echo         Install Visual Studio 2022 with the "Desktop development with C++" workload.
popd
exit /b 1

:vcvars_failed
echo [ERROR] Failed to initialize MSVC environment: %VSPATH%
popd
exit /b 1
