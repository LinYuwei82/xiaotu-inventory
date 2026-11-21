#requires -Version 5.1
<#
.SYNOPSIS
  下载并解压 WebView2 SDK（nuget 包 microsoft.web.webview2）到 .cache\webview2。

.DESCRIPTION
  幂等：.cache\webview2\.version 记录的版本与请求版本一致时直接跳过。
  只写 .cache\ 目录，不改动仓库里任何受版本控制的文件。

.EXAMPLE
  .\shell\win\fetch-webview2.ps1

.EXAMPLE
  .\shell\win\fetch-webview2.ps1 -Version 1.0.4314.35 -Force
#>
[CmdletBinding()]
param(
  [string]$Version = '1.0.4314.35',
  [switch]$Force
)

$ErrorActionPreference = 'Stop'

$cacheRoot = Join-Path $PSScriptRoot '.cache'
$dest = Join-Path $cacheRoot 'webview2'
$stamp = Join-Path $dest '.version'
$nupkg = Join-Path $cacheRoot "microsoft.web.webview2.$Version.nupkg"
$extract = Join-Path $cacheRoot "extract-$Version"

# 与 shell/win/CMakeLists.txt 的架构选择保持一致：ARM64 机器校验 arm64 子目录，其余按 x64。
# nupkg 里 build\native 下各架构齐全，整目录拷贝后两种架构都能用。
$arch = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'x64' }

$needed = @(
  (Join-Path $dest 'include\WebView2.h'),
  (Join-Path $dest "build\native\$arch\WebView2LoaderStatic.lib")
)

if (-not $Force -and (Test-Path -LiteralPath $stamp)) {
  $current = (Get-Content -LiteralPath $stamp -Raw).Trim()
  if ($current -eq $Version -and ($needed | ForEach-Object { Test-Path -LiteralPath $_ }) -notcontains $false) {
    Write-Host "WebView2 SDK $Version 已就绪：$dest"
    return
  }
  if ($current -ne $Version) {
    Write-Host "缓存版本 $current 与请求版本 $Version 不一致，重新下载"
  }
}

New-Item -ItemType Directory -Force -Path $cacheRoot | Out-Null
$url = "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$Version/microsoft.web.webview2.$Version.nupkg"

try {
  Write-Host "下载 $url"
  # nuget.org 走 443 直连；本机 GitHub 不通不影响这里。
  Invoke-WebRequest -Uri $url -OutFile $nupkg -TimeoutSec 120 -UseBasicParsing
} catch {
  throw "下载 WebView2 SDK 失败（版本 $Version）：$($_.Exception.Message)`n" +
        "可重试或改用 -Version 指定其他版本，也可手动下载 nupkg 放到 $cacheRoot 后重跑。"
}

if (-not (Test-Path -LiteralPath $nupkg)) { throw "下载后未找到 $nupkg" }

if (Test-Path -LiteralPath $extract) { Remove-Item -LiteralPath $extract -Recurse -Force }
New-Item -ItemType Directory -Force -Path $extract | Out-Null

try {
  # nupkg 本质是 zip，但 PowerShell 5.1 的 Expand-Archive 只认 .zip 扩展名，改用 .NET 解压。
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  [System.IO.Compression.ZipFile]::ExtractToDirectory($nupkg, $extract)
} catch {
  throw "解压 nupkg 失败：$($_.Exception.Message)"
}

if (Test-Path -LiteralPath $dest) { Remove-Item -LiteralPath $dest -Recurse -Force }
New-Item -ItemType Directory -Force -Path $dest | Out-Null
# nuget 包里没有顶层 include/：头文件在 build\native\include，库在 build\native\x64。
Copy-Item -LiteralPath (Join-Path $extract 'build') -Destination $dest -Recurse
Copy-Item -LiteralPath (Join-Path $extract 'build\native\include') -Destination (Join-Path $dest 'include') -Recurse

foreach ($f in $needed) {
  if (-not (Test-Path -LiteralPath $f)) {
    throw "SDK 解压后缺少必要文件：$f（包结构可能变化，请核对 $Version）"
  }
}

Set-Content -LiteralPath $stamp -Value $Version -Encoding ASCII
Remove-Item -LiteralPath $extract -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $nupkg -Force -ErrorAction SilentlyContinue

Write-Host "WebView2 SDK $Version 已就绪：$dest"
Write-Host "  头文件：include\WebView2.h"
Write-Host "  静态库：build\native\$arch\WebView2LoaderStatic.lib"
Write-Host "注意：目标机器需已安装 WebView2 Runtime（本机已装 157.x）。"
