# 小兔进销存 · Windows 在线更新发布（OSS 清单，更新清单 Schema v2）
#
# 输入：tools\package\package_windows.ps1 产出的 setup.exe + 可选绿色包 zip
# 动作：生成 Ed25519 签名的 manifest.json（v2）→ 上传 dl.xiaotu.online → 线上门禁
#
# 与 macOS（publish_macos.sh）的分工：
#   macOS   appcast.xml（Sparkle 验签）+ manifest.json（v2）→ .dmg
#   Windows 没有 Sparkle，只发 manifest.json（v2）→ setup.exe + 可选绿色包 zip
#   客户端只读 platforms.windows：先比 version、同版本再比 build
#
# 重要：BUILD 必须等于 Windows 侧编译时的 -DXIAOTU_BUILD，否则客户端会反复提示同一版本。
# -Build 可省略：依次从 CMakeCache、绿色包内 xiaotu-shell.exe 的 "XIAOTU_BUILD=<digits>"
# 标记自动识别，最后才用 gen_build.py 按当前 git 计数生成；识别后与 CMakeCache 交叉校验。
#
# 目录约定（v2 命名：<product>-<version>-<os>-<arch>-<type>.<ext>）：
#   oss://<bucket>/xiaotu/xiaotu-<ver>-win64-setup.exe           长缓存 immutable
#   oss://<bucket>/xiaotu/xiaotu-<ver>-win64-portable.zip        存在时一并上传
#   oss://<bucket>/xiaotu/updates/<channel>/manifest.json         60 秒缓存
#   oss://<bucket>/xiaotu/release-notes-<ver>.html               3600 秒缓存（仓库存在说明页时）
#
# 凭据与签名私钥：
#   OSS 凭据：环境变量 OSS_ACCESS_KEY_ID / OSS_ACCESS_KEY_SECRET，或
#             %USERPROFILE%\.config\xiaotu-updates\.env（与 Mac 同路径风格）
#   清单签名私钥：~/.config/xiaotu-updates/update_signing_2026.pem（Ed25519 PKCS#8 PEM）
#             或 CI 环境变量 XIAOTU_UPDATE_SIGNING_KEY（PEM 内容）
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File tools\release\publish_windows.ps1 -Build <tag 序号>
#   powershell -ExecutionPolicy Bypass -File tools\release\publish_windows.ps1 -SkipUpload   # 构建号自动识别
#   powershell ... -NotesOnly                         # 只补发说明页，不需要 setup / zip / Build
#   powershell ... -SetupPath ... -ZipPath ... -Build <tag 序号>
#   powershell ... -Rollout 10 -Build <tag 序号>       # 灰度 10%
#   powershell ... -AllowBuildDrift -Build <tag 序号>  # 单平台热修：放行同版本 build 差异
#   powershell ... -Mandatory -MandatoryReason SECURITY -MandatoryDeadline 2026-12-01T00:00:00Z

param(
    [string]$SetupPath = "",
    [string]$ZipPath = "",
    [ValidateSet("stable", "beta")][string]$Channel = "stable",
    [string]$CdnDomain = "https://dl.xiaotu.online",
    [string]$ProductDir = "xiaotu",
    [string]$MinOs = "10.0",
    [string]$Build = "",
    [string]$EnvFile = "",
    [string]$SignKeyPath = "",
    [string]$KeyId = "2026-11",
    [switch]$SkipUpload,
    [switch]$NotesOnly,
    [switch]$AllowBuildDrift,
    [int]$Rollout = 100,
    [switch]$Mandatory,
    [string]$MandatoryReason = "",
    [string]$MandatoryDeadline = ""
)

$ErrorActionPreference = "Stop"

# ---- 仓库根 ----
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

# 本地产物与清单统一落在仓库根 dist\
$distDir = Join-Path $root "dist"
New-Item -ItemType Directory -Force -Path $distDir | Out-Null

function Fail([string]$message) {
    Write-Host "[失败] $message" -ForegroundColor Red
    exit 1
}

# 灰度 / 强制更新参数校验：唯一实现在 tools/release/publish_args.py（三仓库共享）。
# 非 0 退出直接 exit 2，脚本里不再重复 if 校验。
# Windows 上 python3 常是 Microsoft Store 的占位程序（退出码 9009），优先用 python
$pyArgs = Get-Command python -ErrorAction SilentlyContinue
if (-not $pyArgs) { $pyArgs = Get-Command python3 -ErrorAction SilentlyContinue }
if (-not $pyArgs) { Fail "找不到 python（publish_args / manifest_v2 需要）" }
$mandatoryArg = if ($Mandatory) { "1" } else { "0" }
$argsOutput = & $pyArgs.Source (Join-Path $root "tools\release\publish_args.py") validate `
    "--product=xiaotu" "--rollout=$Rollout" "--mandatory=$mandatoryArg" `
    "--mandatory-reason=$MandatoryReason" "--mandatory-deadline=$MandatoryDeadline" --emit sh
if ($LASTEXITCODE -ne 0) {
    $argsOutput | Out-Host
    Write-Host "[失败] 发布参数校验失败（publish_args.py 退出码 $LASTEXITCODE）"
    exit 2
}
foreach ($line in @($argsOutput)) {
    if ($line -notmatch "^([A-Z_]+)='(.*)'$") { continue }
    $key = $Matches[1]
    $val = $Matches[2].Replace("'\''", "'")
    switch ($key) {
        "ROLLOUT_PERCENT"    { $Rollout = [int]$val }
        "MANDATORY_ON"       { $Mandatory = ($val -eq "1") }
        "MANDATORY_REASON"   { $MandatoryReason = $val }
        "MANDATORY_DEADLINE" { $MandatoryDeadline = $val }
    }
}

# ---- 凭据文件解析：环境变量优先，其次 %USERPROFILE%\.config\xiaotu-updates\.env ----
function Import-EnvFile([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return }
    Get-Content -LiteralPath $path | ForEach-Object {
        $line = $_.Trim()
        if (-not $line -or $line.StartsWith("#")) { return }
        $pair = $line -split "=", 2
        if ($pair.Count -ne 2) { return }
        $name = $pair[0].Trim()
        if ($name -match '^export\s+(.+)$') { $name = $Matches[1].Trim() }
        $value = $pair[1].Trim().Trim('"').Trim("'")
        if ($name) { Set-Item -Path "Env:$name" -Value $value }
    }
}
if (-not $EnvFile) { $EnvFile = Join-Path $env:USERPROFILE ".config\xiaotu-updates\.env" }
Import-EnvFile $EnvFile

Write-Host "===== 小兔进销存 · Windows 发布 ====="
Write-Host ""

# ---- 读取版本 ----
$versionFile = Join-Path $root "VERSION.txt"
if (-not (Test-Path -LiteralPath $versionFile)) { Fail "无法读取 VERSION.txt：$versionFile" }
$Version = (Get-Content -LiteralPath $versionFile -Raw).Trim()
if ($Version -notmatch '^\d+\.\d+\.\d+$') { Fail "VERSION.txt 内容不是 x.y.z 形式：$Version" }
Write-Host "版本：$Version   通道：$Channel"

# ---- NotesOnly：只补发说明页并重签清单 ----
if ($NotesOnly) {
    Write-Host ""
    Write-Host "===== 仅补发说明页（-NotesOnly）====="
    $NotesFile = Join-Path $root "docs\release-notes-$Version.html"
    if (-not (Test-Path -LiteralPath $NotesFile)) {
        Fail "发行说明不存在：$NotesFile（请先把说明页放到 docs\release-notes-$Version.html）"
    }
    $NotesKey = "$ProductDir/release-notes-$Version.html"
    $NotesUrl = "$CdnDomain/$NotesKey"
    Write-Host "说明页：$NotesFile"
    Write-Host "远端：$NotesUrl"

    # 签名私钥
    if (-not $SignKeyPath) {
        if (-not [string]::IsNullOrWhiteSpace($env:XIAOTU_UPDATE_SIGNING_KEY)) {
            $SignKeyPath = Join-Path $env:TEMP ("xiaotu-manifest-key-" + [guid]::NewGuid().ToString("N") + ".pem")
            [IO.File]::WriteAllText($SignKeyPath, $env:XIAOTU_UPDATE_SIGNING_KEY, (New-Object System.Text.UTF8Encoding($false)))
        } else {
            $SignKeyPath = Join-Path $env:USERPROFILE ".config\xiaotu-updates\update_signing_2026.pem"
        }
    }
    if (-not (Test-Path -LiteralPath $SignKeyPath)) {
        Fail "签名私钥不存在：$SignKeyPath（或设置环境变量 XIAOTU_UPDATE_SIGNING_KEY）"
    }

    $python = (Get-Command python -ErrorAction SilentlyContinue).Source
    if (-not $python) { Fail "找不到 python" }

    $manifestUrl = "$CdnDomain/$ProductDir/updates/$Channel/manifest.json"
    $live = Join-Path $env:TEMP "xiaotu-manifest-live-$Version.json"
    $signed = Join-Path $root "dist\manifest.json"

    Write-Host "拉取线上清单：$manifestUrl"
    try {
        Invoke-WebRequest -Uri $manifestUrl -OutFile $live -UseBasicParsing -TimeoutSec 30
    } catch {
        Fail "拉取线上清单失败：$($_.Exception.Message)"
    }

    # 校验线上清单版本与本地 VERSION 一致
    $body = Get-Content -LiteralPath $live -Raw | ConvertFrom-Json
    $onlineVersions = @{}
    foreach ($platform in $body.platforms.PSObject.Properties) {
        $onlineVersions[$platform.Name] = $platform.Value.version
    }
    if ($onlineVersions.Count -eq 0 -or ($onlineVersions.Values | Select-Object -Unique).Count -ne 1 -or
        ($onlineVersions.Values | Select-Object -First 1) -ne $Version) {
        Fail "线上清单版本 $($onlineVersions.Values -join ',') 与 VERSION=$Version 不一致，已中止"
    }
    Write-Host "线上清单版本：$Version（$($onlineVersions.Keys -join ', ')）"

    Write-Host "重签清单：notes_url=$NotesUrl（platform=all）"
    & $python (Join-Path $root "tools\release\manifest_v2.py") resign `
        --manifest $live --key $SignKeyPath --notes-url $NotesUrl --platform all --out $signed
    if ($LASTEXITCODE -ne 0) { Fail "manifest_v2 resign 失败" }
    & $python (Join-Path $root "tools\release\manifest_v2.py") verify --manifest $signed --key $SignKeyPath
    if ($LASTEXITCODE -ne 0) { Fail "本地验签失败" }

    if ($SkipUpload) {
        Write-Host ""
        Write-Host "[SkipUpload] 生成完毕："
        Write-Host "  manifest.json：$signed"
        Write-Host "  release-notes：$NotesFile"
    } else {
        Write-Host ""
        Write-Host "===== 上传 ====="
        foreach ($name in @("OSS_ACCESS_KEY_ID", "OSS_ACCESS_KEY_SECRET", "OSS_BUCKET", "OSS_REGION")) {
            if (-not (Get-Item -Path "Env:$name" -ErrorAction SilentlyContinue)) {
                Fail "缺少环境变量 $name（或写进 $EnvFile）"
            }
        }

        # 凭据轮换自检：AccessKey 不得出现在受版本控制的文件里。
        # 用 git grep 直接扫已跟踪内容，避免 git ls-files 的转义路径在 PS 5.1
        # 下触发「Illegal characters in path」。
        $ak = $env:OSS_ACCESS_KEY_ID
        if (Test-Path -LiteralPath (Join-Path $root ".git")) {
            Push-Location $root
            $hits = @(git grep -l -F -- $ak 2>$null)
            Pop-Location
            if ($hits.Count -gt 0) {
                Fail ("AccessKey 出现在受版本控制的文件 {0} 里，已拒绝发布。请先移除该值，再到 RAM 控制台轮换这对密钥后重跑。" -f
                    ($hits -join ', '))
            }
        }

        $uploadScript = Join-Path $env:TEMP "xiaotu-notes-upload.py"
        $pyCode = @'
import os, oss2
bucket, region = os.environ["OSS_BUCKET"], os.environ["OSS_REGION"]
endpoint = f"https://oss-{region}.aliyuncs.com"
bkt = oss2.Bucket(oss2.Auth(os.environ["OSS_ACCESS_KEY_ID"],
                            os.environ["OSS_ACCESS_KEY_SECRET"]), endpoint, bucket)
bkt.put_object_from_file(os.environ["UPLOAD_NOTES_KEY"], os.environ["UPLOAD_NOTES_PATH"],
                         headers={"Cache-Control": "public, max-age=3600",
                                  "Content-Type": "text/html; charset=utf-8"})
bkt.put_object_from_file(os.environ["UPLOAD_MANIFEST_KEY"], os.environ["UPLOAD_MANIFEST_PATH"],
                         headers={"Cache-Control": "no-cache, max-age=60",
                                  "Content-Type": "application/json"})
print("  release-notes -> " + os.environ["UPLOAD_NOTES_KEY"])
print("  manifest.json -> " + os.environ["UPLOAD_MANIFEST_KEY"])
'@
        [IO.File]::WriteAllText($uploadScript, $pyCode, (New-Object System.Text.UTF8Encoding($false)))
        $env:UPLOAD_NOTES_PATH = $NotesFile
        $env:UPLOAD_NOTES_KEY = $NotesKey
        $env:UPLOAD_MANIFEST_PATH = $signed
        $env:UPLOAD_MANIFEST_KEY = "$ProductDir/updates/$Channel/manifest.json"
        & $python $uploadScript
        if ($LASTEXITCODE -ne 0) { Fail "上传失败" }
        Write-Host ""
        Write-Host "完成：仅补发说明页，setup / zip 未改动"
    }
    exit 0
}

# ---- Build：必须等于编译时的 -DXIAOTU_BUILD ----
# 解析顺序：-Build 显式传入 → CMakeCache → 绿色包内 xiaotu-shell.exe 的
# "XIAOTU_BUILD=<digits>" 标记 → gen_build.py 兜底；取到后与 CMakeCache 交叉校验。
# 与 Mote / DevCard 的 Windows 发布脚本同一口径。
$BuildSource = "参数 -Build"
if ([string]::IsNullOrWhiteSpace($Build)) {
    $cachePath = Join-Path $root "build\windows\CMakeCache.txt"
    if (-not (Test-Path -LiteralPath $cachePath)) {
        # 兼容旧 preset 的构建目录
        foreach ($candidate in @(
            (Join-Path $root "build\windows\CMakeCache.txt"),
            (Join-Path $root "build\CMakeCache.txt")
        )) {
            if (Test-Path -LiteralPath $candidate) { $cachePath = $candidate; break }
        }
    }
    if (Test-Path -LiteralPath $cachePath) {
        $cacheText = Get-Content -LiteralPath $cachePath -Raw
        $match = [regex]::Match($cacheText, 'XIAOTU_BUILD:[A-Z]+=(\d+)')
        if ($match.Success) { $Build = $match.Groups[1].Value; $BuildSource = "CMakeCache" }
    }
}
if ([string]::IsNullOrWhiteSpace($Build)) {
    # 兜底：直接从绿色包里的 xiaotu-shell.exe 识别（updater_win.cpp 内嵌 "XIAOTU_BUILD=<digits>"）
    if ([string]::IsNullOrWhiteSpace($ZipPath)) {
        $ZipPath = Join-Path $root "dist\xiaotu-inventory-$Version-win64.zip"
    }
    if (Test-Path -LiteralPath $ZipPath) {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $zipForBuild = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $ZipPath).Path)
        try {
            $entryForBuild = $zipForBuild.Entries | Where-Object {
                $_.FullName.EndsWith("xiaotu-shell.exe", [StringComparison]::OrdinalIgnoreCase)
            } | Select-Object -First 1
            if ($entryForBuild) {
                $tmpExe = Join-Path $env:TEMP ("xiaotu-build-" + [guid]::NewGuid().ToString("N") + ".exe")
                [IO.Compression.ZipFileExtensions]::ExtractToFile($entryForBuild, $tmpExe)
                try {
                    $ascii = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($tmpExe))
                    $m = [regex]::Match($ascii, 'XIAOTU_BUILD=(\d+)')
                    if ($m.Success) { $Build = $m.Groups[1].Value; $BuildSource = "绿色包内 xiaotu-shell.exe" }
                } finally {
                    Remove-Item $tmpExe -Force -ErrorAction SilentlyContinue
                }
            }
        } finally {
            $zipForBuild.Dispose()
        }
    }
}
if ([string]::IsNullOrWhiteSpace($Build)) {
    # 最后兜底：按当前 git 计数生成（与编译值不一定相同，因此显式提示来源）
    $genBuild = Join-Path $root "tools\release\gen_build.py"
    if (Test-Path -LiteralPath $genBuild) {
        # PS 5.1 下原生命令写 stderr 会被当成终止错误并绕过 $LASTEXITCODE 判断，
        # 这里临时放宽 EAP 并合并 stderr，失败时仍落到下面的统一提示。
        $prevEap = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        $genOutput = & $pyArgs.Source $genBuild --product xiaotu --repo-root $root 2>&1
        $ErrorActionPreference = $prevEap
        $genText = ($genOutput | Out-String).Trim()
        if ($LASTEXITCODE -eq 0 -and $genText -match '^\d+$') {
            $Build = $genText
            $BuildSource = "gen_build.py"
            Write-Host "[警告] 未找到 CMakeCache / 绿色包构建痕迹，按当前 git 计数生成构建号；如与编译值不一致，请用 -Build 显式指定"
        }
    }
}
if ([string]::IsNullOrWhiteSpace($Build)) {
    Fail "未能确定编译时的 XIAOTU_BUILD。请用 -Build <构建号> 显式传入，或先跑 tools\package\package_windows.ps1（会自动生成并写进 CMakeCache）"
}
if ($Build -notmatch '^\d+$') { Fail "非法构建号：$Build（应为纯数字）" }

# 二次验证：与 CMakeCache 里的 XIAOTU_BUILD 一致（防止 -Build 与编译值不同步）
if ($BuildSource -ne "CMakeCache") {
    $cachePath = Join-Path $root "build\windows\CMakeCache.txt"
    if (-not (Test-Path -LiteralPath $cachePath)) {
        foreach ($candidate in @(
            (Join-Path $root "build\windows\CMakeCache.txt"),
            (Join-Path $root "build\CMakeCache.txt")
        )) {
            if (Test-Path -LiteralPath $candidate) { $cachePath = $candidate; break }
        }
    }
    if (Test-Path -LiteralPath $cachePath) {
        $cachedMatch = [regex]::Match((Get-Content -LiteralPath $cachePath -Raw), 'XIAOTU_BUILD:[A-Z]+=(\d+)')
        if ($cachedMatch.Success) {
            $cachedBuild = $cachedMatch.Groups[1].Value
            if ($cachedBuild -ne $Build) {
                Fail "-Build $Build 与编译时 CMakeCache XIAOTU_BUILD=$cachedBuild 不一致；请保持同步（或清理构建目录重编）"
            }
            Write-Host "构建号：$Build（来源 $BuildSource，CMakeCache 一致 ✓）"
        } else {
            Write-Host "构建号：$Build（来源 $BuildSource，CMakeCache 未找到 XIAOTU_BUILD，跳过二次验证）"
        }
    } else {
        Write-Host "构建号：$Build（来源 $BuildSource，无 CMakeCache，跳过二次验证）"
    }
} else {
    Write-Host "构建号：$Build（来源 $BuildSource）"
}

# ---- 签名私钥 ----
if (-not $SignKeyPath) {
    if (-not [string]::IsNullOrWhiteSpace($env:XIAOTU_UPDATE_SIGNING_KEY)) {
        $SignKeyPath = Join-Path $env:TEMP ("xiaotu-manifest-key-" + [guid]::NewGuid().ToString("N") + ".pem")
        [IO.File]::WriteAllText($SignKeyPath, $env:XIAOTU_UPDATE_SIGNING_KEY, (New-Object System.Text.UTF8Encoding($false)))
    } else {
        $SignKeyPath = Join-Path $env:USERPROFILE ".config\xiaotu-updates\update_signing_2026.pem"
    }
}
if (-not (Test-Path -LiteralPath $SignKeyPath)) {
    Fail "签名私钥不存在：$SignKeyPath（或设置环境变量 XIAOTU_UPDATE_SIGNING_KEY）"
}

# ---- 定位产物（setup.exe 必填，zip 可选）----
if (-not $SetupPath) {
    $SetupPath = Join-Path $root "dist\xiaotu-inventory-$Version-win64-setup.exe"
}
if (-not (Test-Path -LiteralPath $SetupPath)) {
    Fail "安装包不存在：$SetupPath（先跑 tools\package\package_windows.ps1）"
}
$SetupFull = (Resolve-Path $SetupPath).Path

if (-not $ZipPath) {
    $ZipPath = Join-Path $root "dist\xiaotu-inventory-$Version-win64.zip"
}
$ZipFull = ""
if (Test-Path -LiteralPath $ZipPath) {
    $ZipFull = (Resolve-Path $ZipPath).Path
}

# v2 远端命名（本地产物名保持构建输出不变，上传时按 v2 改名）
$V2SetupName = "$ProductDir-$Version-win64-setup.exe"
$V2ZipName = "$ProductDir-$Version-win64-portable.zip"
$SetupKey = "$ProductDir/$V2SetupName"
$ZipKey = "$ProductDir/$V2ZipName"
$SetupUrl = "$CdnDomain/$SetupKey"
$ZipUrl = "$CdnDomain/$ZipKey"
$ManifestKey = "$ProductDir/updates/$Channel/manifest.json"
$ManifestUrl = "$CdnDomain/$ManifestKey"

$SetupSize = (Get-Item -LiteralPath $SetupFull).Length
$SetupSha256 = (Get-FileHash -LiteralPath $SetupFull -Algorithm SHA256).Hash.ToLower()

if ($ZipFull) {
    $ZipSize = (Get-Item -LiteralPath $ZipFull).Length
    $ZipSha256 = (Get-FileHash -LiteralPath $ZipFull -Algorithm SHA256).Hash.ToLower()
} else {
    $ZipSize = 0
    $ZipSha256 = ""
}

# ---- 发行说明 ----
$NotesFile = Join-Path $root "docs\release-notes-$Version.html"
$NotesUrl = ""
$NotesKey = ""
if (Test-Path -LiteralPath $NotesFile) {
    $NotesKey = "$ProductDir/release-notes-$Version.html"
    $NotesUrl = "$CdnDomain/$NotesKey"
} else {
    Write-Host "提示：未找到 docs\release-notes-$Version.html，清单不写 notes_url"
}

Write-Host ""
Write-Host "产物："
Write-Host "  setup    : $SetupFull  ($SetupSize B)"
Write-Host "  zip      : $(if ($ZipFull) { $ZipFull + "  ($ZipSize B)" } else { '（未提供，仅发布安装包）' })"
Write-Host "  v2 setup : $SetupUrl"
if ($ZipFull) { Write-Host "  v2 zip   : $ZipUrl" }
Write-Host "  manifest : $ManifestUrl"
if ($NotesUrl) { Write-Host "  notes_url: $NotesUrl" }

# ---- Python ----
$python = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $python) { Fail "找不到 python（manifest_v2 / guard / oss2 都需要）" }

# ---- ① 生成 manifest.json（v2，Ed25519 签名）----
Write-Host ""
Write-Host "===== ① 生成 manifest.json（v2）"
$manifestPath = Join-Path $distDir "manifest.json"

# 另一平台段可能已在线上：先取线上清单，--prev 合并后整体重签
# （dist\manifest.json 是持久文件，每次都先拉线上覆盖，避免拿旧版本当 --prev）
try {
    Invoke-WebRequest -Uri $ManifestUrl -OutFile $manifestPath -UseBasicParsing -TimeoutSec 15
} catch {
    Remove-Item -LiteralPath $manifestPath -ErrorAction SilentlyContinue
}

$manifestArgs = @(
    "generate", "--product", $ProductDir, "--channel", $Channel,
    "--platform", "windows", "--version", $Version, "--build", $Build,
    "--min-os", $MinOs, "--key-id", $KeyId,
    "--artifact", "setup=$SetupFull",
    "--key", $SignKeyPath, "--out", $manifestPath
)
if ($ZipFull) { $manifestArgs += @("--artifact", "portable=$ZipFull") }
$manifestArgs += @("--rollout", "$Rollout")
if ($Mandatory) {
    $manifestArgs += @("--mandatory", "--mandatory-reason", $MandatoryReason)
    if ($MandatoryDeadline) { $manifestArgs += @("--mandatory-deadline", $MandatoryDeadline) }
    Write-Host "  更新策略：灰度 $Rollout% · 强制更新（$MandatoryReason$(if ($MandatoryDeadline) { "，截止 $MandatoryDeadline" })）"
} else {
    Write-Host "  更新策略：灰度 $Rollout% · 非强制"
}
if ($NotesUrl) { $manifestArgs += @("--notes-url", $NotesUrl) }
if (Test-Path -LiteralPath $manifestPath) { $manifestArgs += @("--prev", $manifestPath) }

& $python (Join-Path $root "tools\release\manifest_v2.py") @manifestArgs
if ($LASTEXITCODE -ne 0) { Fail "manifest_v2 generate 失败" }
if (-not (Test-Path -LiteralPath $manifestPath)) { Fail "清单未生成：$manifestPath" }

# 本地验签
& $python (Join-Path $root "tools\release\manifest_v2.py") verify `
    --manifest $manifestPath --key $SignKeyPath --artifact "setup=$SetupFull"
if ($LASTEXITCODE -ne 0) { Fail "本地验签失败" }
if ($ZipFull) {
    & $python (Join-Path $root "tools\release\manifest_v2.py") verify `
        --manifest $manifestPath --key $SignKeyPath --artifact "portable=$ZipFull"
    if ($LASTEXITCODE -ne 0) { Fail "绿色包验签失败" }
}

$pubHexLine = & $python (Join-Path $root "tools\release\manifest_v2.py") pubkey --key $SignKeyPath
$PubHex = (($pubHexLine -join "") -replace '.*：', '').Trim()
Write-Host "  key-id   : $KeyId"
Write-Host "  pub-hex  : $PubHex"
if ($NotesUrl) { Write-Host "  notes_url: $NotesUrl" }

# ---- ② 门禁（guard，v2）----
Write-Host ""
Write-Host "===== ② 发布门禁"
$guardArgs = @(
    "--product", $ProductDir, "--kind", "win", "--channel", $Channel,
    "--manifest", $manifestPath,
    "--pub-hex", $PubHex, "--build", $Build,
    "--file", $SetupFull, "--name", $V2SetupName
)
if ($ZipFull) {
    $guardArgs += @("--zip", $ZipFull, "--zip-name", $V2ZipName)
}
# 单平台热修：同版本时 Windows build 可以高于 macOS build，但必须显式放行，
# 并在发布记录写明原因（与 guard.py 的 --allow-build-drift 同一口径）。
if ($AllowBuildDrift) { $guardArgs += "--allow-build-drift" }
if ($SkipUpload) { $guardArgs += "--offline" }

& $python (Join-Path $root "tools\release\guard.py") preflight @guardArgs
if ($LASTEXITCODE -ne 0) { Fail "guard preflight 未通过（v2 命名/版本/覆盖/内嵌地址/签名）" }

# ---- ③ 上传 OSS ----
if ($SkipUpload) {
    Write-Host ""
    Write-Host "===== ③ [SkipUpload] 跳过上传"
} else {
    Write-Host ""
    Write-Host "===== ③ 上传"
    foreach ($name in @("OSS_ACCESS_KEY_ID", "OSS_ACCESS_KEY_SECRET", "OSS_BUCKET", "OSS_REGION")) {
        if (-not (Get-Item -Path "Env:$name" -ErrorAction SilentlyContinue)) {
            Fail "缺少环境变量 $name（或写进 $EnvFile）"
        }
    }

    # 凭据轮换自检：AccessKey 不得出现在受版本控制的文件里。
    # 用 git grep 直接扫已跟踪内容：git ls-files 默认输出带引号的转义路径，
    # PS 5.1 下 Test-Path 会因中文路径报「Illegal characters in path」。
    $ak = $env:OSS_ACCESS_KEY_ID
    if (Test-Path -LiteralPath (Join-Path $root ".git")) {
        Push-Location $root
        $hits = @(git grep -l -F -- $ak 2>$null)
        Pop-Location
        if ($hits.Count -gt 0) {
            Fail ("AccessKey 出现在受版本控制的文件 {0} 里，已拒绝发布。请先移除该值，再到 RAM 控制台轮换这对密钥后重跑。" -f
                ($hits -join ', '))
        }
    }

    $uploadScript = Join-Path $env:TEMP "xiaotu-publish-upload.py"
    $pyCode = @'
import os, oss2
bucket, region = os.environ["OSS_BUCKET"], os.environ["OSS_REGION"]
endpoint = f"https://oss-{region}.aliyuncs.com"
bkt = oss2.Bucket(oss2.Auth(os.environ["OSS_ACCESS_KEY_ID"],
                            os.environ["OSS_ACCESS_KEY_SECRET"]), endpoint, bucket)

bkt.put_object_from_file(os.environ["UPLOAD_SETUP_KEY"], os.environ["UPLOAD_SETUP_PATH"],
                         headers={"Cache-Control": "public, max-age=31536000, immutable",
                                  "Content-Type": "application/octet-stream"})
print("  setup        -> " + os.environ["UPLOAD_SETUP_KEY"])

if os.environ.get("UPLOAD_ZIP_KEY"):
    bkt.put_object_from_file(os.environ["UPLOAD_ZIP_KEY"], os.environ["UPLOAD_ZIP_PATH"],
                             headers={"Cache-Control": "public, max-age=31536000, immutable",
                                      "Content-Type": "application/zip"})
    print("  zip          -> " + os.environ["UPLOAD_ZIP_KEY"])

if os.environ.get("UPLOAD_NOTES_KEY"):
    bkt.put_object_from_file(os.environ["UPLOAD_NOTES_KEY"], os.environ["UPLOAD_NOTES_PATH"],
                             headers={"Cache-Control": "public, max-age=3600",
                                      "Content-Type": "text/html; charset=utf-8"})
    print("  release-notes-> " + os.environ["UPLOAD_NOTES_KEY"])

bkt.put_object_from_file(os.environ["UPLOAD_MANIFEST_KEY"], os.environ["UPLOAD_MANIFEST_PATH"],
                         headers={"Cache-Control": "no-cache, max-age=60",
                                  "Content-Type": "application/json"})
print("  manifest.json-> " + os.environ["UPLOAD_MANIFEST_KEY"])
'@
    [IO.File]::WriteAllText($uploadScript, $pyCode, (New-Object System.Text.UTF8Encoding($false)))

    $env:UPLOAD_SETUP_PATH = $SetupFull
    $env:UPLOAD_SETUP_KEY = $SetupKey
    $env:UPLOAD_MANIFEST_PATH = $manifestPath
    $env:UPLOAD_MANIFEST_KEY = $ManifestKey
    if ($ZipFull) {
        $env:UPLOAD_ZIP_PATH = $ZipFull
        $env:UPLOAD_ZIP_KEY = $ZipKey
    } else {
        Remove-Item Env:UPLOAD_ZIP_PATH -ErrorAction SilentlyContinue
        Remove-Item Env:UPLOAD_ZIP_KEY -ErrorAction SilentlyContinue
    }
    if ($NotesUrl) {
        $env:UPLOAD_NOTES_PATH = $NotesFile
        $env:UPLOAD_NOTES_KEY = $NotesKey
    } else {
        Remove-Item Env:UPLOAD_NOTES_PATH -ErrorAction SilentlyContinue
        Remove-Item Env:UPLOAD_NOTES_KEY -ErrorAction SilentlyContinue
    }

    & $python $uploadScript
    if ($LASTEXITCODE -ne 0) { Fail "上传失败（python 退出码 $LASTEXITCODE）" }
}

# ---- ④ 线上校验 ----
Write-Host ""
Write-Host "===== ④ 线上校验"
if ($SkipUpload) {
    Write-Host "  （跳过）本地清单：$manifestPath"
} else {
    $ProgressPreference = "SilentlyContinue"

    # 4a. setup.exe 可下载且大小一致
    try {
        $head = Invoke-WebRequest -Uri $SetupUrl -Method Head -UseBasicParsing -TimeoutSec 30
        if ($head.StatusCode -eq 200 -and [int64]$head.Headers["Content-Length"] -eq $SetupSize) {
            Write-Host "  ✓ setup.exe 可下载，长度与本地一致（$SetupSize B）"
        } else {
            Write-Host "  ✗ setup.exe 校验异常：HTTP $($head.StatusCode) 长度 $($head.Headers['Content-Length'])"
        }
    } catch {
        Write-Host "  ✗ setup.exe 不可访问：$($_.Exception.Message)"
    }

    # 4b. manifest.json 内容正确
    try {
        $live = Invoke-WebRequest -Uri $ManifestUrl -UseBasicParsing -TimeoutSec 30
        $shaPattern = [regex]::Escape($SetupSha256)
        $versionPattern = '"version":\s*"' + [regex]::Escape($Version) + '"'
        $setupUrlPattern = '"url":\s*"' + [regex]::Escape($SetupUrl) + '"'
        $buildPattern = '"build":\s*' + $Build
        if ($live.StatusCode -eq 200 -and
            $live.Content -match $shaPattern -and
            $live.Content -match $versionPattern -and
            $live.Content -match $buildPattern -and
            $live.Content -match $setupUrlPattern) {
            Write-Host "  ✓ manifest.json 版本 / build / SHA256 / URL 均与产物一致"
        } else {
            Write-Host "  ✗ manifest.json 内容异常"
        }
    } catch {
        Write-Host "  ✗ manifest.json 不可访问：$($_.Exception.Message)"
    }

    # 4c. zip（绿色包）
    if ($ZipFull) {
        try {
            $head = Invoke-WebRequest -Uri $ZipUrl -Method Head -UseBasicParsing -TimeoutSec 30
            if ($head.StatusCode -eq 200 -and [int64]$head.Headers["Content-Length"] -eq $ZipSize) {
                Write-Host "  ✓ portable.zip 可下载，长度与本地一致（$ZipSize B）"
            } else {
                Write-Host "  ✗ portable.zip 校验异常：HTTP $($head.StatusCode)"
            }
        } catch {
            Write-Host "  ✗ portable.zip 不可访问：$($_.Exception.Message)"
        }
    }

    # 4d. notes_url
    if ($NotesUrl) {
        try {
            $head = Invoke-WebRequest -Uri $NotesUrl -Method Head -UseBasicParsing -TimeoutSec 30
            if ($head.StatusCode -eq 200) {
                Write-Host "  ✓ release-notes 可访问"
            } else {
                Write-Host "  ✗ release-notes 校验异常：HTTP $($head.StatusCode)"
            }
        } catch {
            Write-Host "  ✗ release-notes 不可访问：$($_.Exception.Message)"
        }
    }

    # 4e. guard verify-live
    Write-Host ""
    Write-Host "===== ④b 线上门禁（guard）"
    & $python (Join-Path $root "tools\release\guard.py") verify-live @guardArgs
    if ($LASTEXITCODE -ne 0) { Fail "guard verify-live 未通过" }
}

# ---- 完成 ----
Write-Host ""
Write-Host "================ 发布完成 ================"
Write-Host ("  版本 / build : {0} / {1}" -f $Version, $Build)
Write-Host ("  setup URL    : {0}" -f $SetupUrl)
Write-Host ("  setup SHA256 : {0}" -f $SetupSha256)
if ($ZipFull) {
    Write-Host ("  zip URL      : {0}" -f $ZipUrl)
    Write-Host ("  zip SHA256   : {0}" -f $ZipSha256)
}
Write-Host ("  manifest     : {0}" -f $ManifestUrl)
Write-Host ("  签名 key_id  : {0}" -f $KeyId)
Write-Host ""
Write-Host "  下载页（pre.xiaotu.online/download）由 site.js 读 manifest.json 自动同步，无需改 HTML"
