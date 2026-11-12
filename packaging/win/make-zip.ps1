#requires -Version 5.1
<#
.SYNOPSIS
  从 windows-release 构建产物生成可分发的 Windows zip。

.DESCRIPTION
  只读取 build\windows\server\xiaotu-server.exe 与 deploy\config.toml.example，
  组装到临时目录后压缩。不修改仓库里的任何受版本控制的文件。

.EXAMPLE
  .\packaging\win\make-zip.ps1

.EXAMPLE
  .\packaging\win\make-zip.ps1 -NoArchive
#>
[CmdletBinding()]
param(
  [string]$BuildDir = "",
  [string]$OutDir = "",
  [string]$Name = "",
  [switch]$NoArchive
)

$ErrorActionPreference = 'Stop'

# packaging\win\make-zip.ps1 -> 仓库根
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $BuildDir) { $BuildDir = Join-Path $root 'build\windows' }
if (-not $OutDir) { $OutDir = Join-Path $root 'build\package\win' }

$exe = Join-Path $BuildDir 'server\xiaotu-server.exe'
if (-not (Test-Path -LiteralPath $exe)) {
  throw "未找到 $exe。请先执行：cmake --preset windows-release; cmake --build --preset windows-release"
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

Copy-Item -LiteralPath $exe -Destination $stage
$configExample = Join-Path $root 'deploy\config.toml.example'
if (Test-Path -LiteralPath $configExample) {
  Copy-Item -LiteralPath $configExample -Destination $stage
} else {
  Write-Warning "未找到 $configExample，分发包将不含配置样例"
}
$readme = Join-Path $PSScriptRoot 'README.md'
if (Test-Path -LiteralPath $readme) {
  Copy-Item -LiteralPath $readme -Destination (Join-Path $stage 'README.md')
}

# logs 目录放一个占位文件，解压后不会被当成空目录忽略。
Set-Content -LiteralPath (Join-Path $stage 'logs\.keep') -Value '运行日志写入此目录（app.log）' -Encoding UTF8

if ($NoArchive) {
  Write-Host "已生成目录：$stage"
  return
}

if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
# 压缩目录本身（不加 \*），解压后得到单层 xiaotu-inventory-<版本>-win64\ 目录。
Compress-Archive -Path $stage -DestinationPath $zipPath -CompressionLevel Optimal

$zip = Get-Item -LiteralPath $zipPath
$hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash
Write-Host ''
Write-Host "分发包：$($zip.FullName)"
Write-Host ("大小：{0:N2} MB" -f ($zip.Length / 1MB))
Write-Host "SHA256：$hash"
Write-Host "已解压目录：$stage"
