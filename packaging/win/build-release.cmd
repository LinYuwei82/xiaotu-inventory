@echo off
chcp 65001 >nul
setlocal
rem 小兔进销存 Windows 一键出包入口。
rem 自动初始化 MSVC 环境（vcvars64），再调用 build-release.ps1。
rem
rem 用法：
rem   build-release.cmd                 :: 构建号取 UTC 时间
rem   build-release.cmd 202611180930    :: 指定构建号

set "SCRIPT_DIR=%~dp0"
pushd "%SCRIPT_DIR%..\.." || exit /b 1

rem ---- 1) 定位 Visual Studio C++ 工具链 ----
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSPATH="
if exist "%VSWHERE%" (
  for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
)
if not defined VSPATH (
  for %%e in (Community Professional Enterprise BuildTools) do (
    if exist "%ProgramFiles%\Microsoft Visual Studio\2022\%%e\VC\Auxiliary\Build\vcvars64.bat" (
      set "VSPATH=%ProgramFiles%\Microsoft Visual Studio\2022\%%e"
    )
  )
)
if not defined VSPATH (
  echo [失败] 未找到 Visual Studio 2022 C++ 工具链。
  echo        请安装 Visual Studio 2022，并勾选「使用 C++ 的桌面开发」。
  popd ^& exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo [失败] vcvars64.bat 初始化失败：%VSPATH%
  popd ^& exit /b 1
)

rem ---- 2) 调用打包脚本 ----
if "%~1"=="" (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%build-release.ps1"
) else (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%build-release.ps1" -Build "%~1"
)
set "RC=%ERRORLEVEL%"

popd
exit /b %RC%
