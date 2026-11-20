#Requires -Version 5.1
<#
.SYNOPSIS
  小兔进销存 Windows 一键出包：编译服务端与壳 → 绿色包 → 安装包 → 自检。

.DESCRIPTION
  只在本机产出文件，不签名、不上传。完成后打印 Mac 上要执行的发布命令。
  Ed25519 私钥与 OSS 凭据保持在 Mac，不进入 Windows。

  前置：
    - VS2022 + CMake + Ninja（未在 Developer 环境时会自动加载 vcvars64）
    - Inno Setup 6（ISCC.exe）
    - WebView2 SDK 由 shell\win\fetch-webview2.ps1 自动获取

.PARAMETER Build
  构建号（纯数字）。缺省由 tools/release/gen_build.py 生成（自动校验严格大于线上 build）。
  会同时写入服务端与壳。

.EXAMPLE
  .\tools\package\package_windows.ps1

.EXAMPLE
  .\tools\package\package_windows.ps1 -Build 202611180930
#>
[CmdletBinding()]
param(
  [string]$Build = ""
)

$ErrorActionPreference = 'Stop'

# 与另两仓一致只保留一个入口脚本：不在 Developer 环境时自动加载 MSVC。
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (Test-Path -LiteralPath $vswhere) {
    $vsPath = & $vswhere -latest -products * `
      -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($vsPath) {
      $vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
      if (Test-Path -LiteralPath $vcvars) {
        Write-Host "加载 MSVC 环境：$vcvars"
        cmd /c "call `"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
          if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2] }
        }
      }
    }
  }
}

function Step([string]$Message) {
  Write-Host ""
  Write-Host "===== $Message" -ForegroundColor Cyan
}

function Fail([string]$Message) {
  Write-Host ""
  Write-Host "[失败] $Message" -ForegroundColor Red
  exit 1
}

function Invoke-Native([string]$Exe, [string[]]$Arguments) {
  & $Exe @Arguments
  if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) {
    Fail "$Exe 退出码 $LASTEXITCODE"
  }
}

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
  Fail "当前不是 Developer PowerShell（找不到 cl.exe）。请从「Developer PowerShell for VS 2022」启动后再运行。"
}

$version = (Get-Content (Join-Path $root 'VERSION.txt') -Raw).Trim()
if (-not $version) { Fail "无法读取 VERSION.txt" }

# 构建号：缺省走 gen_build.py（year*10^12 + git_count，自动校验 > 线上），
# 三仓库统一口径；不再用 UTC 时间戳，避免与 macOS 各自生成导致撞号。
if (-not $Build) {
  $genBuild = Join-Path $root 'tools\release\gen_build.py'
  if (-not (Test-Path $genBuild)) { Fail "找不到 gen_build.py：$genBuild" }
  # Windows 上 python3 常是 Microsoft Store 的占位程序（退出码 9009），优先用 python
  $py = Get-Command python -ErrorAction SilentlyContinue
  if (-not $py) { $py = Get-Command python3 -ErrorAction SilentlyContinue }
  if (-not $py) { Fail "找不到 python，无法运行 gen_build.py" }
  $Build = (& $py.Source $genBuild --product xiaotu --repo-root $root 2>$null | Select-Object -Last 1)
  if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($Build)) {
    Fail "gen_build.py 失败，无法生成构建号（可用 -Build 显式指定）"
  }
  $Build = $Build.Trim()
  Write-Host "构建号来源：gen_build.py"
}
if ($Build -notmatch '^\d+$') { Fail "构建号必须是纯数字：$Build" }

Write-Host "版本：$version   构建号：$Build"
Write-Host "提示：构建号必须大于线上 manifest.json 里 platforms.windows 的 build，否则 guard 会拒绝发布。"

$env:XIAOTU_BUILD = $Build

Step "1/5 取 WebView2 SDK"
& (Join-Path $root 'shell\win\fetch-webview2.ps1')

Step "2/5 构建服务端"
Invoke-Native 'cmake' @('--preset', 'windows-release', '-DXIAOTU_RELEASE_BUILD=ON', "-DXIAOTU_BUILD=$Build")
Invoke-Native 'cmake' @('--build', '--preset', 'windows-release')

$serverExe = Join-Path $root 'build\windows\server\xiaotu-server.exe'
if (-not (Test-Path -LiteralPath $serverExe)) { Fail "未找到服务端产物：$serverExe" }

Step "3/5 构建桌面壳"
Invoke-Native 'cmake' @('-S', 'shell/win', '-B', 'shell/win/build', '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DXIAOTU_RELEASE_BUILD=ON')
Invoke-Native 'cmake' @('--build', 'shell/win/build')

$shellExe = Join-Path $root 'shell\win\build\xiaotu-shell.exe'
if (-not (Test-Path -LiteralPath $shellExe)) { Fail "未找到壳产物：$shellExe" }

# 1.0.0 的包就是这里漏了：壳内清单地址必须与本次发布一致
$text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($shellExe))
if ($text -notlike '*xiaotu/updates/stable/manifest.json*') {
  Fail "壳内未包含 xiaotu/updates/stable/manifest.json，发布后会永远检查不到更新。请检查 shell/win 源码后重试。"
}
Write-Host "  ✓ 壳内清单地址正确"

Step "4/5 打绿色包"
& (Join-Path $root 'packaging\win\make-zip.ps1')

$zip = Join-Path $root "dist\xiaotu-inventory-$version-win64.zip"
if (-not (Test-Path -LiteralPath $zip)) { Fail "未找到绿色包：$zip" }

Step "5/5 打安装包"
$iscc = $null
foreach ($candidate in @(
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "$env:ProgramFiles\Inno Setup 6\ISCC.exe")) {
  if (Test-Path -LiteralPath $candidate) { $iscc = $candidate; break }
}
if (-not $iscc) { Fail "未找到 ISCC.exe（Inno Setup 6），请先安装" }

New-Item -ItemType Directory -Force (Join-Path $root 'dist') | Out-Null
Invoke-Native $iscc @("/DAppVersion=$version", (Join-Path $root 'packaging\win\xiaotu-inventory.iss'))

$setup = Join-Path $root "dist\xiaotu-inventory-$version-win64-setup.exe"
if (-not (Test-Path -LiteralPath $setup)) { Fail "未找到安装包：$setup" }

Step "产物"
foreach ($file in @($setup, $zip)) {
  $item = Get-Item -LiteralPath $file
  $sha = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLower()
  Write-Host ("  {0}" -f $item.FullName)
  Write-Host ("    {0:N0} 字节  sha256={1}" -f $item.Length, $sha)
}

Write-Host ""
Write-Host "下一步：" -ForegroundColor Green
Write-Host "  选项 A · 直接在本 Windows 机器上发布清单（需要签名私钥 PEM 或 CI 环境变量 XIAOTU_UPDATE_SIGNING_KEY）："
Write-Host "    powershell -ExecutionPolicy Bypass -File tools\release\publish_windows.ps1 -Build $Build -SkipUpload   # 先空跑"
Write-Host "    powershell -ExecutionPolicy Bypass -File tools\release\publish_windows.ps1 -Build $Build                 # 正式上传"
Write-Host ""
Write-Host "  选项 B · 拷回 Mac 执行 .sh（与之前相同）："
Write-Host "    cd <xiaotu-inventory 仓库>"
Write-Host "    OSS_SKIP=true BUILD=$Build tools/release/publish_windows.sh   # 先空跑验签"
Write-Host "    BUILD=$Build tools/release/publish_windows.sh                # 正式上传"
