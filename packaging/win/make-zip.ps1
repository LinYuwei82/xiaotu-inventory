#requires -Version 5.1
<#
.SYNOPSIS
  从 windows-release 构建产物生成可分发的 Windows zip（可选包含桌面壳）。

.DESCRIPTION
  读取 build\windows\server\xiaotu-server.exe，若存在 shell\win\build\xiaotu-shell.exe
  则一并打入（壳按同目录找服务端，因此两个 exe 必须同级）。再配合
  deploy\config.toml.example 与面向终端用户的 package-readme.md 组装后压缩。
  不修改仓库里任何受版本控制的文件，也不打包 WebView2 SDK 缓存。

.EXAMPLE
  .\packaging\win\make-zip.ps1            # 有壳就打壳，没有只打服务端

.EXAMPLE
  .\packaging\win\make-zip.ps1 -NoShell   # 强制只打服务端

.EXAMPLE
  .\packaging\win\make-zip.ps1 -NoArchive # 只生成解压后的目录
#>
[CmdletBinding()]
param(
  [string]$BuildDir = "",
  [string]$OutDir = "",
  [string]$Name = "",
  [string]$ShellExe = "",
  [switch]$NoShell,
  [switch]$NoArchive
)

$ErrorActionPreference = 'Stop'

# packaging\win\make-zip.ps1 -> 仓库根
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $BuildDir) { $BuildDir = Join-Path $root 'build\windows' }
if (-not $OutDir) { $OutDir = Join-Path $root 'build\package\win' }
if (-not $ShellExe) { $ShellExe = Join-Path $root 'shell\win\build\xiaotu-shell.exe' }

$serverExe = Join-Path $BuildDir 'server\xiaotu-server.exe'
if (-not (Test-Path -LiteralPath $serverExe)) {
  throw "未找到 $serverExe。请先执行：cmake --preset windows-release; cmake --build --preset windows-release"
}

# 默认策略：壳存在就打进去；不存在只打服务端并提示。
$includeShell = $false
if ($NoShell) {
  Write-Host '按 -NoShell 指定的模式打包：仅服务端（浏览器访问）'
} elseif (Test-Path -LiteralPath $ShellExe) {
  $includeShell = $true
} else {
  Write-Warning "未找到桌面壳 $ShellExe，本次只打服务端。需要桌面窗口请先构建：" `
    + 'cmake -S shell\win -B shell\win\build -G Ninja; cmake --build shell\win\build'
}

# 版本号取自根 CMakeLists.txt（只读，不修改）。
$projectLine = Select-String -LiteralPath (Join-Path $root 'CMakeLists.txt') `
  -Pattern 'project\(\s*xiaotu_inventory\s+VERSION\s+([0-9][0-9.]*)' | Select-Object -First 1
if (-not $projectLine) { throw '无法从 CMakeLists.txt 读取项目版本号' }
$version = $projectLine.Matches[0].Groups[1].Value

if (-not $Name) { $Name = "xiaotu-inventory-$version-win64" }
$stage = Join-Path $OutDir $Name
$zipPath = Join-Path $OutDir ($Name + '.zip')

if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'logs') | Out-Null

Copy-Item -LiteralPath $serverExe -Destination $stage
if ($includeShell) {
  # 与 xiaotu-server.exe 同级，壳才能按同目录找到服务端。
  Copy-Item -LiteralPath $ShellExe -Destination $stage
}

$configExample = Join-Path $root 'deploy\config.toml.example'
if (Test-Path -LiteralPath $configExample) {
  Copy-Item -LiteralPath $configExample -Destination $stage
} else {
  Write-Warning "未找到 $configExample，分发包将不含配置样例"
}

# 包内 README 面向终端用户：双击用法、WebView2 依赖与安装地址、浏览器访问方式。
$packageReadme = Join-Path $PSScriptRoot 'package-readme.md'
if (Test-Path -LiteralPath $packageReadme) {
  $readmeText = Get-Content -LiteralPath $packageReadme -Raw -Encoding UTF8
  if (-not $includeShell) {
    $readmeText = $readmeText -replace '(?m)^(# 小兔进销存（Windows）)',
      "`$1`r`n`r`n> 注意：本包不含桌面窗口 xiaotu-shell.exe，请直接用「方式二：用浏览器访问」。`r`n"
  }
  Set-Content -LiteralPath (Join-Path $stage 'README.md') -Value $readmeText -Encoding UTF8
} else {
  Write-Warning "未找到 $packageReadme，分发包将不含说明文件"
}

# logs 目录放一个占位文件，解压后不会被当成空目录忽略。
Set-Content -LiteralPath (Join-Path $stage 'logs\.keep') -Value '运行日志写入此目录（app.log）' -Encoding UTF8

# 只报告打包机上的 WebView2 状态，供出包前确认；真正的要求写在包内 README。
$wv2Keys = @(
  'HKLM:\SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}',
  'HKLM:\SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}',
  'HKCU:\SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}'
)
$wv2Version = $null
foreach ($key in $wv2Keys) {
  if (Test-Path -LiteralPath $key) {
    $pv = (Get-ItemProperty -LiteralPath $key -ErrorAction SilentlyContinue).pv
    if ($pv) { $wv2Version = $pv; break }
  }
}

if ($NoArchive) {
  Write-Host "已生成目录：$stage"
} else {
  if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
  # 压缩目录本身（不加 \*），解压后得到单层 xiaotu-inventory-<版本>-win64\ 目录。
  Compress-Archive -Path $stage -DestinationPath $zipPath -CompressionLevel Optimal
}

Write-Host ''
Write-Host ("模式：{0}" -f $(if ($includeShell) { '桌面壳 + 服务端（双击即用）' } else { '仅服务端（浏览器访问）' }))
Write-Host ("包内文件：{0}" -f ((Get-ChildItem -LiteralPath $stage -Recurse -File |
  ForEach-Object { $_.FullName.Substring($stage.Length + 1) }) -join ', '))
if ($includeShell) {
  if ($wv2Version) {
    Write-Host "WebView2 Runtime（本打包机）：$wv2Version"
  } else {
    Write-Warning '本打包机未检测到 WebView2 Runtime；目标机器仍需安装，包内 README 已写明下载地址。'
  }
}
if (-not $NoArchive) {
  $zip = Get-Item -LiteralPath $zipPath
  $hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash
  Write-Host "分发包：$($zip.FullName)"
  Write-Host ("大小：{0:N2} MB" -f ($zip.Length / 1MB))
  Write-Host "SHA256：$hash"
  Write-Host "已解压目录：$stage"
}
