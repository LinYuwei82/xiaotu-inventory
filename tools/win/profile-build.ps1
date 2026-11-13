#Requires -Version 5.1
<#
  小兔进销存 Windows 构建耗时采集（默认只读）
  只采集环境；不加 -Measure / -FullMeasure 时绝不运行 cmake 或 ctest。
#>
[CmdletBinding()]
param(
    [string]$Repo = "C:\src\xiaotu-inventory",
    [string]$Vcpkg = "",
    [string]$BuildPreset = "windows-release",
    [string]$BuildDir = "",
    [string]$OutFile = "",
    [switch]$Measure,
    [switch]$FullMeasure
)

$ErrorActionPreference = "Continue"
$script:Missing = New-Object System.Collections.ArrayList
$script:Timings = [ordered]@{ configure = $null; build = $null; ctest = $null; full = $null }
$script:Notes = New-Object System.Collections.ArrayList
$script:TempBuild = ""
$script:Started = Get-Date

function Say([string]$Text) { Write-Host $Text }
function Note([string]$Text) { [void]$script:Notes.Add($Text) }
function Missing([string]$What, [string]$Reason) {
    [void]$script:Missing.Add("$What（$Reason）")
    Write-Host "[无法采集] $What - $Reason"
}
function Format-Sec($Span) {
    if ($null -eq $Span) { return "-" }
    return ("{0:N1}s" -f $Span.TotalSeconds)
}
function Invoke-Timed([string]$Label, [string]$Exe, [string[]]$ArgList, [string]$WorkDir) {
    if (-not (Test-Path $WorkDir)) { Missing $Label "目录不存在: $WorkDir"; return $null }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    Push-Location $WorkDir
    try {
        & $Exe @ArgList 2>&1 | ForEach-Object { Write-Host "    $_" }
        $code = $LASTEXITCODE
    } finally { Pop-Location }
    $sw.Stop()
    if ($code -ne 0) { Note "$Label 返回非零退出码 $code" }
    Write-Host "  $Label 耗时 $(Format-Sec $sw.Elapsed)"
    return $sw.Elapsed
}

# ---------- 0. 环境 ----------
if ($env:OS -ne "Windows_NT") { Write-Host "必须在 Windows 上运行"; exit 2 }
if (-not (Test-Path (Join-Path $Repo ".git"))) { Write-Host "仓库路径不正确: $Repo"; exit 2 }
if (-not $BuildDir) { $BuildDir = Join-Path $Repo "build\windows" }
if (-not $Vcpkg) {
    $vcpkgRoot = $env:VCPKG_ROOT
    if (-not $vcpkgRoot) { $vcpkgRoot = Join-Path $env:LOCALAPPDATA "vcpkg" }
    $Vcpkg = $vcpkgRoot
}
$repoPath = (Resolve-Path $Repo).Path

Say "== 采集开始：$repoPath =="

# ---------- 1. 仓库 / vcpkg 盘与卷类型 ----------
function Get-VolumeInfo([string]$Path) {
    if ($Path -match '^\\\\') {
        return [pscustomobject]@{ Path = $Path; Drive = "UNC"; Media = "网络盘"; Bus = "Network" }
    }
    try {
        $full = [IO.Path]::GetFullPath($Path)
    } catch {
        return [pscustomobject]@{ Path = $Path; Drive = "未知"; Media = "未知"; Bus = "未知" }
    }
    $root = [IO.Path]::GetPathRoot($full)
    $letter = $root.TrimEnd('\').TrimEnd(':')
    try {
        $vol = Get-Volume -DriveLetter $letter -ErrorAction Stop
        $media = "未知"; $bus = "未知"
        try {
            $part = Get-Partition -DriveLetter $letter -ErrorAction Stop | Select-Object -First 1
            $disk = Get-Disk -Number $part.DiskNumber -ErrorAction Stop
            $media = [string]$disk.MediaType
            $bus = [string]$disk.BusType
            if ($bus -eq "USB") { $media = "$media（USB）" }
        } catch { }
        return [pscustomobject]@{ Path = $full; Drive = $letter; Media = $media; Bus = $bus }
    } catch {
        return [pscustomobject]@{ Path = $full; Drive = $letter; Media = "未知"; Bus = "未知" }
    }
}

$repoVol = Get-VolumeInfo $repoPath
$vcpkgVol = Get-VolumeInfo $Vcpkg
Say "仓库: $($repoVol.Path)"
Say "  卷: $($repoVol.Drive) / $($repoVol.Media) / $($repoVol.Bus)"
Say "vcpkg: $($vcpkgVol.Path)"
Say "  卷: $($vcpkgVol.Drive) / $($vcpkgVol.Media) / $($vcpkgVol.Bus)"
if ($repoVol.Media -match "HDD|机械" -or $repoVol.Bus -eq "Network" -or $repoPath -match "OneDrive|Dropbox|坚果云|iCloud") {
    Note "仓库位于慢盘或同步盘"
}
if ($vcpkgVol.Media -match "HDD|机械" -or $vcpkgVol.Bus -eq "Network") {
    Note "vcpkg 位于慢盘或网络盘"
}

# ---------- 2. 生成器 / 缓存 / 全量或增量 ----------
$cacheFile = Join-Path $BuildDir "CMakeCache.txt"
$generator = "未配置"; $launcher = "无"; $fresh = "增量（存在构建目录）"
if (Test-Path $cacheFile) {
    $genLine = Select-String -Path $cacheFile -Pattern "^CMAKE_GENERATOR:INTERNAL=" | Select-Object -First 1
    if ($genLine) { $generator = $genLine.Line.Split("=", 2)[1] }
    $launchLine = Select-String -Path $cacheFile -Pattern "COMPILER_LAUNCHER" | Select-Object -First 1
    if ($launchLine) { $launcher = $launchLine.Line.Split("=", 2)[1] }
} else {
    $fresh = "全量（无构建目录）"
}
$ccache = (Get-Command ccache.exe -ErrorAction SilentlyContinue).Source
$sccache = (Get-Command sccache.exe -ErrorAction SilentlyContinue).Source
$cacheTool = @()
if ($ccache) { $cacheTool += "ccache: $ccache" }
if ($sccache) { $cacheTool += "sccache: $sccache" }
if ($launcher -ne "无" -and $launcher -ne "") { $cacheTool += "CMake launcher: $launcher" }
if ($cacheTool.Count -eq 0) { $cacheTool = @("未发现 ccache/sccache/launcher") }
Say "生成器: $generator"
Say "构建形态: $fresh"
Say "编译缓存: $($cacheTool -join '; ')"

# ---------- 3. Defender ----------
$rtp = $null; $exclusions = @(); $repoExcluded = $false; $vcpkgExcluded = $false
try {
    $status = Get-MpComputerStatus -ErrorAction Stop
    $rtp = [bool]$status.RealTimeProtectionEnabled
    $pref = Get-MpPreference -ErrorAction Stop
    $exclusions = @($pref.ExclusionPath)
    $repoExcluded = $false; $vcpkgExcluded = $false
    foreach ($ex in $exclusions) {
        if (-not $ex) { continue }
        if ($repoPath.StartsWith($ex.TrimEnd('\'), [StringComparison]::OrdinalIgnoreCase)) { $repoExcluded = $true }
        if ($Vcpkg.StartsWith($ex.TrimEnd('\'), [StringComparison]::OrdinalIgnoreCase)) { $vcpkgExcluded = $true }
    }
    Say "Defender 实时保护: $(if ($rtp) { '开启' } else { '关闭' })"
    Say "Defender 排除项: 仓库=$(if ($repoExcluded) {'已排除'} else {'未排除'})；vcpkg=$(if ($vcpkgExcluded) {'已排除'} else {'未排除'})"
    if ($rtp -and -not ($repoExcluded -and $vcpkgExcluded)) { Note "Defender 实时保护开启且未完全排除仓库/vcpkg" }
} catch {
    Missing "Defender 状态" $_.Exception.Message
}

# ---------- 4. CPU / 内存 / 页面文件 ----------
$cpuName = "未知"; $cores = 0; $threads = 0; $ramGB = 0; $pfGB = 0
try {
    $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    $cs = Get-CimInstance Win32_ComputerSystem
    $cores = ($cpu.NumberOfCores)
    $threads = ($cpu.NumberOfLogicalProcessors)
    $cpuName = $cpu.Name.Trim()
    $ramGB = [math]::Round($cs.TotalPhysicalMemory / 1GB, 1)
    $pf = Get-CimInstance Win32_PageFileUsage | Select-Object -First 1
    $pfGB = if ($pf) { [math]::Round($pf.AllocatedBaseSize / 1024, 1) } else { 0 }
    Say "CPU: $cpuName / ${cores}C${threads}T"
    Say "内存: ${ramGB} GB；页面文件: ${pfGB} GB"
    if ($cores -lt 4) { Note "物理核心少于 4" }
    if ($ramGB -lt 16) { Note "内存小于 16 GB" }
    if ($pfGB -lt 8) { Note "页面文件小于 8 GB" }
} catch {
    Missing "CPU/内存" $_.Exception.Message
}

# ---------- 5. 计时（显式开关） ----------
if ($Measure) {
    Say "== 增量计时（使用现有构建目录 $BuildDir） =="
    $script:Timings.configure = Invoke-Timed "configure" "cmake" @("--preset", $BuildPreset) $repoPath
    $script:Timings.build = Invoke-Timed "build" "cmake" @("--build", "--preset", $BuildPreset) $repoPath
    $script:Timings.ctest = Invoke-Timed "ctest" "ctest" @("--test-dir", $BuildDir, "--output-on-failure") $repoPath
} else {
    Say "增量计时: 未开启（加 -Measure）"
}

if ($FullMeasure) {
    $script:TempBuild = Join-Path $env:TEMP ("xiaotu-fullbuild-" + [guid]::NewGuid().ToString("N"))
    Say "== 全量计时（临时目录 $script:TempBuild，结束后删除） =="
    try {
        $sw = [Diagnostics.Stopwatch]::StartNew()
        & cmake -S $repoPath -B $script:TempBuild -G Ninja -DCMAKE_BUILD_TYPE=Release 2>&1 |
            ForEach-Object { Write-Host "    $_" }
        $sw.Stop(); $script:Timings.configure = $sw.Elapsed
        Say "  configure 耗时 $(Format-Sec $sw.Elapsed)"
        $sw = [Diagnostics.Stopwatch]::StartNew()
        & cmake --build $script:TempBuild 2>&1 | ForEach-Object { Write-Host "    $_" }
        $sw.Stop(); $script:Timings.build = $sw.Elapsed
        Say "  build 耗时 $(Format-Sec $sw.Elapsed)"
        $sw = [Diagnostics.Stopwatch]::StartNew()
        & ctest --test-dir $script:TempBuild --output-on-failure 2>&1 | ForEach-Object { Write-Host "    $_" }
        $sw.Stop(); $script:Timings.ctest = $sw.Elapsed
        Say "  ctest 耗时 $(Format-Sec $sw.Elapsed)"
        $script:Timings.full = $script:Timings.configure + $script:Timings.build + $script:Timings.ctest
    } catch {
        Missing "全量计时" $_.Exception.Message
    } finally {
        if (Test-Path $script:TempBuild) { Remove-Item -Recurse -Force $script:TempBuild -ErrorAction SilentlyContinue }
    }
} else {
    Say "全量计时: 未开启（加 -FullMeasure，单独开关）"
}

# ---------- 6. 摘要与结论 ----------
$verdict = "无法判断"; $basis = "未发现明显配置或硬件瓶颈"
$configIssues = @($script:Notes | Where-Object { $_ -match "Defender|慢盘|同步盘|vcpkg|页面文件" })
$hardwareIssues = @($script:Notes | Where-Object { $_ -match "核心|内存" })
if ($configIssues.Count -gt 0) {
    $verdict = "配置问题"; $basis = ($configIssues -join "；")
} elseif ($hardwareIssues.Count -gt 0) {
    $verdict = "硬件问题"; $basis = ($hardwareIssues -join "；")
} elseif ($script:Timings.build -and $script:Timings.build.TotalSeconds -gt 600) {
    $verdict = "无法判断"; $basis = "构建超过 10 分钟但环境无明显异常，建议用 ETW/Procmon 看单文件耗时"
}

$lines = @()
$lines += "小兔进销存 Windows 构建耗时采集"
$lines += "仓库: $($repoVol.Path) [$(if ($repoVol.Media) {$repoVol.Media} else {'未知'}) / $($repoVol.Bus)]"
$lines += "vcpkg: $($vcpkgVol.Path) [$(if ($vcpkgVol.Media) {$vcpkgVol.Media} else {'未知'})]"
$lines += "生成器: $generator；形态: $fresh"
$lines += "缓存: $($cacheTool -join '; ')"
$lines += "Defender: 实时=$(if ($null -eq $rtp) {'未知'} elseif ($rtp) {'开'} else {'关'})；排除 仓库=$(if ($rtp -eq $null) {'未知'} elseif ($repoExcluded) {'是'} else {'否'}) vcpkg=$(if ($rtp -eq $null) {'未知'} elseif ($vcpkgExcluded) {'是'} else {'否'})"
$lines += "CPU/内存: $cpuName ${cores}C${threads}T / ${ramGB}GB / PF ${pfGB}GB"
$lines += "耗时: configure=$(Format-Sec $script:Timings.configure) build=$(Format-Sec $script:Timings.build) ctest=$(Format-Sec $script:Timings.ctest)"
if ($script:Timings.full) { $lines += "全量合计: $(Format-Sec $script:Timings.full)" }
if ($script:Missing.Count -gt 0) { $lines += "缺失: $($script:Missing -join '；')" }
$lines += "结论: $verdict - $basis"

Say ""
Say "===== 摘要（可直接复制） ====="
$lines | ForEach-Object { Say $_ }
Say "=============================="

if ($OutFile) {
    $lines | Set-Content -Path $OutFile -Encoding UTF8
    Say "已写入 $OutFile"
}

Say ("采集耗时 " + (Format-Sec ((Get-Date) - $script:Started)))
if ($script:Missing.Count -gt 0) { exit 1 }
exit 0
