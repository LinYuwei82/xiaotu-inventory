#Requires -Version 5.1
<#
.SYNOPSIS
  Win10 干净虚拟机首次安装验收：统计安装总耗时，并拆出 WebView2 引导器的耗时。

.DESCRIPTION
  在“未安装 WebView2 Runtime”的干净 Win10 虚拟机中运行本脚本，流程：
    1. 环境预检：系统版本、是否已有 WebView2、是否管理员、能否连通微软下载域名；
    2. 静默运行安装包（/VERYSILENT），Stopwatch 记录墙钟总耗时，保留 Inno 安装日志；
    3. 解析日志，取出 MicrosoftEdgeWebview2Setup.exe 的执行区间与退出码；
    4. 校验安装结果：安装目录、壳版本、XIAOTU_BUILD 构建号、WebView2 版本；
    5. 启动壳做冒烟验证（xiaotu-server 起来、其动态监听端口可连）；
    6. 打印摘要并写出 JSON 报告。

  用法（安装包与本脚本放同一目录）：
    powershell -NoProfile -ExecutionPolicy Bypass -File .\test_first_install.ps1

  建议先给虚拟机打快照；跑完可加 -UninstallAfter 还原到“已装但可再次测试”的状态。
  已安装 WebView2 的机器会直接失败退出（除非加 -Force），因为首次安装耗时没有意义。

.PARAMETER Setup
  安装包路径。缺省在脚本同目录查找 xiaotu-inventory-*-win64-setup.exe，取版本最大的一个。

.PARAMETER OutDir
  日志与报告输出目录。缺省 %TEMP%\xiaotu-firstinstall（路径不要含空格）。

.PARAMETER UninstallAfter
  测试结束后静默卸载。数据目录会保留，属正常行为。

.PARAMETER SkipLaunchCheck
  跳过启动壳的冒烟验证，只测安装耗时。

.PARAMETER Force
  即使已检测到 WebView2 仍然继续（此时 WebView2 相关耗时不具参考价值）。

.EXAMPLE
  .\test_first_install.ps1

.EXAMPLE
  .\test_first_install.ps1 -Setup D:\pkg\xiaotu-inventory-1.0.10-win64-setup.exe -UninstallAfter
#>
[CmdletBinding()]
param(
  [string]$Setup = "",
  [string]$OutDir = (Join-Path $env:TEMP 'xiaotu-firstinstall'),
  [switch]$UninstallAfter,
  [switch]$SkipLaunchCheck,
  [switch]$Force
)

$ErrorActionPreference = 'Stop'
$script:Failures = New-Object System.Collections.Generic.List[string]

function Add-Failure([string]$Message) {
  $script:Failures.Add($Message) | Out-Null
  Write-Host ("  [FAIL] " + $Message) -ForegroundColor Red
}

function Write-Step([string]$Message) {
  Write-Host ("== " + $Message) -ForegroundColor Cyan
}

function Get-WebView2Version {
  $guid = '{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}'
  $paths = @(
    "HKCU:\Software\Microsoft\EdgeUpdate\Clients\$guid",
    "HKLM:\Software\WOW6432Node\Microsoft\EdgeUpdate\Clients\$guid",
    "HKLM:\Software\Microsoft\EdgeUpdate\Clients\$guid"
  )
  foreach ($p in $paths) {
    $pv = (Get-ItemProperty -LiteralPath $p -Name 'pv' -ErrorAction SilentlyContinue).pv
    if ($pv -and $pv -ne '0.0.0.0') { return [string]$pv }
  }
  return ''
}

function Test-TcpPort {
  param(
    [string]$TargetHost = '127.0.0.1',
    [int]$Port = 3091,
    [int]$TimeoutMs = 1000
  )
  $client = New-Object System.Net.Sockets.TcpClient
  try {
    $iar = $client.BeginConnect($TargetHost, $Port, $null, $null)
    if (-not $iar.AsyncWaitHandle.WaitOne($TimeoutMs)) { return $false }
    $client.EndConnect($iar)
    return $true
  } catch {
    return $false
  } finally {
    $client.Close()
  }
}

function Get-LineTime([string]$Line) {
  if ($Line -match '^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3})') {
    return [datetime]::ParseExact($matches[1], 'yyyy-MM-dd HH:mm:ss.fff',
      [Globalization.CultureInfo]::InvariantCulture)
  }
  return $null
}

function Find-InstalledDir {
  $uninstallRoots = @(
    'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*',
    'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*',
    'HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*'
  )
  foreach ($root in $uninstallRoots) {
    $entry = Get-ItemProperty -Path $root -ErrorAction SilentlyContinue |
      Where-Object { $_.DisplayName -like '*小兔进销存*' } |
      Select-Object -First 1
    if ($entry -and $entry.InstallLocation) {
      $p = [string]$entry.InstallLocation
      if (Test-Path -LiteralPath (Join-Path $p 'xiaotu-shell.exe')) { return $p }
    }
  }
  $searchRoots = @(
    (Join-Path $env:LOCALAPPDATA 'Programs'),
    $env:ProgramFiles,
    ${env:ProgramFiles(x86)}
  ) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }
  foreach ($root in $searchRoots) {
    $dirs = Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue
    foreach ($d in $dirs) {
      if (Test-Path -LiteralPath (Join-Path $d.FullName 'xiaotu-shell.exe')) { return $d.FullName }
    }
  }
  return ''
}

function Get-BuildMarker([string]$ExePath) {
  try {
    $bytes = [System.IO.File]::ReadAllBytes($ExePath)
    $text = [System.Text.Encoding]::GetEncoding(28591).GetString($bytes)
    if ($text -match 'XIAOTU_BUILD=(\d+)') { return $matches[1] }
  } catch { }
  return ''
}

function Invoke-Uninstall([string]$Dir) {
  $unins = Join-Path $Dir 'unins000.exe'
  if (-not (Test-Path -LiteralPath $unins)) { return $false }
  $p = Start-Process -FilePath $unins -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -PassThru
  Wait-Process -Id $p.Id -Timeout 120 -ErrorAction SilentlyContinue
  if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
  return $true
}

# ---------------- 0. 准备 ----------------
if (-not (Test-Path -LiteralPath $OutDir)) {
  New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
}

if (-not $Setup) {
  $cand = Get-ChildItem -LiteralPath $PSScriptRoot -Filter 'xiaotu-inventory-*-win64-setup.exe' -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending | Select-Object -First 1
  if ($cand) { $Setup = $cand.FullName }
}
if (-not $Setup -or -not (Test-Path -LiteralPath $Setup)) {
  Write-Host "找不到安装包，请用 -Setup 指定 xiaotu-inventory-*-win64-setup.exe" -ForegroundColor Red
  exit 4
}
$Setup = (Resolve-Path -LiteralPath $Setup).Path

$os = [System.Environment]::OSVersion.Version
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
  [Security.Principal.WindowsBuiltInRole]::Administrator)
$wv2Before = Get-WebView2Version
$netOk = Test-TcpPort -TargetHost 'go.microsoft.com' -Port 443 -TimeoutMs 5000

Write-Step '环境信息'
Write-Host ("  系统: Windows {0}.{1}.{2} ({3})" -f $os.Major, $os.Minor, $os.Build,
  $(if ([Environment]::Is64BitOperatingSystem) { 'x64' } else { 'x86' }))
Write-Host ("  管理员: {0}    PowerShell: {1}" -f $isAdmin, $PSVersionTable.PSVersion)
Write-Host ("  WebView2 安装前: {0}" -f $(if ($wv2Before) { $wv2Before } else { '未安装' }))
Write-Host ("  go.microsoft.com:443 连通: {0}" -f $netOk)
Write-Host ("  安装包: {0}" -f $Setup)

if ($os.Major -ne 10) {
  Add-Failure '本脚本面向 Windows 10（build >= 17763）。'
}
if (-not $Force -and $wv2Before) {
  Write-Host '  该虚拟机已安装 WebView2，不是首次安装环境。请用干净快照，或加 -Force 强制继续。' -ForegroundColor Yellow
  exit 2
}
if (-not $netOk) {
  Write-Host '  警告: 连不上 go.microsoft.com，WebView2 自动安装很可能失败。' -ForegroundColor Yellow
}
if (-not $isAdmin) {
  Write-Host '  提示: 非管理员运行；WebView2 若按机器级安装会失败，建议以管理员身份运行本脚本。' -ForegroundColor Yellow
}

$setupHash = (Get-FileHash -LiteralPath $Setup -Algorithm SHA256).Hash
$logPath = Join-Path $OutDir 'inno-install.log'
Remove-Item -LiteralPath $logPath -Force -ErrorAction SilentlyContinue

# ---------------- 1. 首次安装（计时） ----------------
Write-Step '静默安装（首次，含 WebView2 下载与安装）'
$setupArgs = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', "/LOG=$logPath")
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$proc = Start-Process -FilePath $Setup -ArgumentList $setupArgs -Wait -PassThru
$sw.Stop()
$wallSeconds = [math]::Round($sw.Elapsed.TotalSeconds, 1)
Write-Host ("  安装退出码: {0}，总耗时: {1} s" -f $proc.ExitCode, $wallSeconds)
if ($proc.ExitCode -ne 0) { Add-Failure ("安装器退出码非 0: " + $proc.ExitCode) }

# ---------------- 2. 解析日志 ----------------
$logFirst = $null; $logLast = $null
$runStart = $null; $runExit = $null; $runExitCode = $null; $runFile = ''
if (Test-Path -LiteralPath $logPath) {
  $lines = Get-Content -LiteralPath $logPath
  $pendingRun = $false
  foreach ($line in $lines) {
    $t = Get-LineTime $line
    if ($t) {
      if (-not $logFirst) { $logFirst = $t }
      $logLast = $t
    }
    if ($line -match '-- Run entry --') { $pendingRun = $true; continue }
    if ($pendingRun -and $line -match 'Filename:\s*(.+)$') {
      $f = $matches[1].Trim()
      if ($f -match 'MicrosoftEdgeWebview2Setup\.exe') {
        $runFile = $f
        if ($t) { $runStart = $t }
      }
      continue
    }
    if ($runStart -and -not $runExit -and $line -match 'Process exit code:\s*(-?\d+)') {
      $runExitCode = [int]$matches[1]
      if ($t) { $runExit = $t }
      $pendingRun = $false
    }
  }
} else {
  Add-Failure ("找不到安装日志: " + $logPath)
}

$bootstrapperRan = [bool]($runStart -and $runFile)
$bootstrapSeconds = if ($runStart -and $runExit) { [math]::Round(($runExit - $runStart).TotalSeconds, 1) } else { $null }
$beforeSeconds = if ($runStart -and $logFirst) { [math]::Round(($runStart - $logFirst).TotalSeconds, 1) } else { $null }
$afterSeconds = if ($runExit -and $logLast) { [math]::Round(($logLast - $runExit).TotalSeconds, 1) } else { $null }

Write-Host ("  WebView2 引导器: {0}" -f $(if ($bootstrapperRan) { '已执行' } else { '未执行（任务未触发）' }))
if ($bootstrapperRan) {
  Write-Host ("    bootstrapper 耗时: {0} s，退出码: {1}" -f $bootstrapSeconds, $runExitCode)
  Write-Host ("    引导器之前（下载/准备/复制）: {0} s，之后（收尾）: {1} s" -f $beforeSeconds, $afterSeconds)
  if ($runExitCode -ne 0) { Add-Failure ("WebView2 引导器退出码非 0: " + $runExitCode) }
} else {
  Add-Failure '日志里没有 WebView2 引导器的执行记录，自动安装任务未生效。'
}

# ---------------- 3. 安装结果 ----------------
Write-Step '安装结果'
$installedDir = Find-InstalledDir
$shellPath = ''
$shellVersion = ''
$buildMarker = ''
if ($installedDir) {
  $shellPath = Join-Path $installedDir 'xiaotu-shell.exe'
  Write-Host ("  安装目录: {0}" -f $installedDir)
  if (Test-Path -LiteralPath $shellPath) {
    $shellVersion = ([System.Diagnostics.FileVersionInfo]::GetVersionInfo($shellPath)).FileVersion
    $buildMarker = Get-BuildMarker $shellPath
    Write-Host ("  壳版本: {0}    XIAOTU_BUILD={1}" -f $shellVersion, $(if ($buildMarker) { $buildMarker } else { '(未找到)' }))
  }
  if (-not (Test-Path -LiteralPath $shellPath)) { Add-Failure '安装目录里找不到 xiaotu-shell.exe。' }
} else {
  Add-Failure '未找到安装目录（卸载注册表项或 Programs 目录）。'
}

$wv2After = Get-WebView2Version
Write-Host ("  WebView2 安装后: {0}" -f $(if ($wv2After) { $wv2After } else { '仍未安装' }))
if (-not $wv2After) { Add-Failure '安装结束后仍未检测到 WebView2 Runtime。' }

# ---------------- 4. 启动冒烟 ----------------
$launchChecked = -not $SkipLaunchCheck
$launchOk = $false
$portOk = $false
if ($launchChecked -and $shellPath -and (Test-Path -LiteralPath $shellPath)) {
  Write-Step '启动验证'
  Get-Process -Name 'xiaotu-shell', 'xiaotu-server' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
  $shellProc = Start-Process -FilePath $shellPath -PassThru -ErrorAction SilentlyContinue
  $serverPort = $null
  for ($i = 0; $i -lt 30; $i++) {
    Start-Sleep -Seconds 1
    $srv = Get-Process -Name 'xiaotu-server' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($srv) {
      $conn = Get-NetTCPConnection -State Listen -OwningProcess $srv.Id -ErrorAction SilentlyContinue |
        Where-Object { $_.LocalAddress -eq '127.0.0.1' } | Select-Object -First 1
      if ($conn) {
        $serverPort = [int]$conn.LocalPort
        $portOk = Test-TcpPort -TargetHost '127.0.0.1' -Port $serverPort -TimeoutMs 800
        if ($portOk) { break }
      }
    }
  }
  $launchOk = [bool]($shellProc -and -not $shellProc.HasExited -and $portOk)
  Write-Host ("  壳进程: {0}    server 端口: {1}" -f
    $(if ($shellProc -and -not $shellProc.HasExited) { '存活' } else { '已退出' }),
    $(if ($portOk) { [string]$serverPort } else { '未监听' }))
  if (-not $launchOk) { Add-Failure '启动验证失败（壳未存活或 server 端口未监听）。' }
  Get-Process -Name 'xiaotu-shell', 'xiaotu-server' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}

# ---------------- 5. 报告 ----------------
$verdict = if ($script:Failures.Count -eq 0) { 'PASS' } else { 'FAIL' }
$report = [ordered]@{
  generated_at = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss')
  host = [ordered]@{
    os = ("Windows {0}.{1}.{2}" -f $os.Major, $os.Minor, $os.Build)
    arch = $(if ([Environment]::Is64BitOperatingSystem) { 'x64' } else { 'x86' })
    admin = $isAdmin
    powershell = $PSVersionTable.PSVersion.ToString()
  }
  setup = [ordered]@{
    path = $Setup
    size = (Get-Item -LiteralPath $Setup).Length
    sha256 = $setupHash
  }
  environment = [ordered]@{
    webview2_before = $wv2Before
    network_go_microsoft_443 = $netOk
  }
  install = [ordered]@{
    exit_code = $proc.ExitCode
    wall_seconds = $wallSeconds
    log = $logPath
    bootstrapper_ran = $bootstrapperRan
    bootstrapper_file = $runFile
    bootstrapper_seconds = $bootstrapSeconds
    bootstrapper_exit_code = $runExitCode
    phase_before_bootstrapper_seconds = $beforeSeconds
    phase_after_bootstrapper_seconds = $afterSeconds
  }
  result = [ordered]@{
    installed_dir = $installedDir
    shell_version = $shellVersion
    build_marker = $buildMarker
    webview2_after = $wv2After
    launch_checked = $launchChecked
    launch_ok = $launchOk
    server_port = $serverPort
    server_port_ok = $portOk
  }
  verdict = $verdict
  failures = @($script:Failures)
}
$reportPath = Join-Path $OutDir 'first-install-report.json'
$report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $reportPath -Encoding UTF8

Write-Step '摘要'
Write-Host ("  安装总耗时: {0} s" -f $wallSeconds)
if ($bootstrapperRan) {
  Write-Host ("    其中 WebView2: {0} s（引导器退出码 {1}）" -f $bootstrapSeconds, $runExitCode)
  Write-Host ("    其余阶段: 之前 {0} s + 之后 {1} s" -f $beforeSeconds, $afterSeconds)
}
Write-Host ("  结果: {0}" -f $verdict) -ForegroundColor $(if ($verdict -eq 'PASS') { 'Green' } else { 'Red' })
Write-Host ("  报告: {0}" -f $reportPath)

if ($UninstallAfter -and $installedDir) {
  Write-Step '静默卸载（-UninstallAfter）'
  if (Invoke-Uninstall $installedDir) { Write-Host '  已启动卸载并等待结束。' } else { Write-Host '  未找到 unins000.exe。' -ForegroundColor Yellow }
}

if ($verdict -eq 'PASS') { exit 0 } else { exit 1 }
