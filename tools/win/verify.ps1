#Requires -Version 5.1
<#
  小兔进销存 Windows 实机验收脚本（PowerShell 5.1 / 7）
  只读检查 + 安装/卸载验证。任何 FAIL 都会使退出码非 0。
#>
[CmdletBinding()]
param(
    [string]$Repo = "C:\src\xiaotu-inventory",
    [string]$Installer = "",
    [string]$ExpectedVersion = "0.9.0",
    [string]$DataDir = (Join-Path $env:LOCALAPPDATA "com.simem.xiaotu"),
    [int]$Port = 18080,
    [string]$LogDir = (Join-Path $env:TEMP ("xiaotu-verify-" + (Get-Date -Format "yyyyMMdd-HHmmss"))),
    [switch]$SkipBuild,
    [switch]$SkipInstall,
    [switch]$AllowExistingData,
    [switch]$Yes
)

$ErrorActionPreference = "Stop"
$script:Results = New-Object System.Collections.ArrayList
$script:HasFail = $false
$script:TempDirs = New-Object System.Collections.ArrayList

function Write-Result([string]$Name, [string]$Status, [string]$Detail) {
    $line = "[{0}] {1} - {2}" -f $Status, $Name, $Detail
    Write-Host $line
    [void]$script:Results.Add([pscustomobject]@{ Name = $Name; Status = $Status; Detail = $Detail })
    if ($Status -eq "FAIL") { $script:HasFail = $true }
}
function Pass([string]$n, [string]$d) { Write-Result $n "PASS" $d }
function Fail([string]$n, [string]$d) { Write-Result $n "FAIL" $d }
function Skip([string]$n, [string]$d) { Write-Result $n "SKIP" $d }
function Manual([string]$n, [string]$d) { Write-Result $n "MANUAL" $d }

function Invoke-Check([string]$Name, [scriptblock]$Body) {
    try { & $Body }
    catch {
        $msg = $_.Exception.Message
        Fail $Name ("异常: " + $msg)
        Write-Host ("  详细: " + ($_ | Out-String))
    }
}

# 原生命令统一出口。git/cmake/ctest 会往 stderr 写正常提示（例如
# "Preparing worktree (detached HEAD ...)"），在 $ErrorActionPreference="Stop" 下
# PowerShell 5.1 会把这类 stderr 记录当成终止性错误。这里只以退出码判定成败，
# 输出原样落日志并回显，避免「正常提示」被误报成命令失败。
function Invoke-Native([string]$LogFile, [scriptblock]$Call) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = & $Call 2>&1
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
    }
    if ($output) { $output | ForEach-Object { Write-Host $_ } }
    if ($LogFile) { $output | Out-File -FilePath $LogFile -Encoding utf8 }
    return [pscustomobject]@{ ExitCode = $code; Output = ($output | Out-String) }
}

function Get-HttpResult([string]$Method, [string]$Url, $Body, $WebSession) {
    $params = @{ Uri = $Url; Method = $Method; UseBasicParsing = $true; TimeoutSec = 10 }
    if ($null -ne $Body) {
        $params["Body"] = [Text.Encoding]::UTF8.GetBytes($Body)
        $params["ContentType"] = "application/json; charset=utf-8"
    }
    if ($null -ne $WebSession) { $params["WebSession"] = $WebSession }
    try {
        $r = Invoke-WebRequest @params
        return [pscustomobject]@{ Status = [int]$r.StatusCode; Body = $r.Content }
    } catch {
        if ($_.Exception.Response) {
            $resp = $_.Exception.Response
            $status = [int]$resp.StatusCode
            if ($resp.PSObject.Methods["GetResponseStream"]) {
                $reader = New-Object IO.StreamReader($resp.GetResponseStream())
                return [pscustomobject]@{ Status = $status; Body = $reader.ReadToEnd() }
            }
            return [pscustomobject]@{ Status = $status; Body = $_.ErrorDetails.Message }
        }
        throw
    }
}

function Wait-Server([string]$OutFile, [int]$Seconds) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $OutFile) {
            $text = Get-Content -Raw $OutFile -ErrorAction SilentlyContinue
            if ($text -match "服务地址:\s*(http://127\.0\.0\.1:(\d+))") {
                $port = [int]$Matches[2]
                $token = ""
                if ($text -match "#t=([0-9a-fA-F]+)") { $token = $Matches[1] }
                return [pscustomobject]@{ Base = "http://127.0.0.1:$port"; Token = $token }
            }
        }
        Start-Sleep -Milliseconds 300
    }
    return $null
}

function Find-Installer([string]$Explicit) {
    if ($Explicit -and (Test-Path $Explicit)) { return (Resolve-Path $Explicit).Path }
    $candidates = @()
    $candidates += Get-ChildItem -Path (Join-Path $Repo "build\package\win") -Filter "*setup*.exe" -ErrorAction SilentlyContinue
    $candidates += Get-ChildItem -Path (Join-Path $env:TEMP "xiaotu-win-installer") -Filter "*setup*.exe" -Recurse -ErrorAction SilentlyContinue
    if ($candidates.Count -eq 0) { return $null }
    return ($candidates | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
}

# ---------- 0. 环境与安全检查 ----------
Write-Host "== 小兔进销存 Windows 验收 =="
if ($env:OS -ne "Windows_NT") { Write-Error "必须在 Windows 上运行"; exit 2 }
if (-not (Test-Path (Join-Path $Repo ".git"))) { Write-Error "仓库路径不正确: $Repo"; exit 2 }
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
Start-Transcript -Path (Join-Path $LogDir "verify.log") -Force | Out-Null

if ((Test-Path $DataDir) -and (Test-Path (Join-Path $DataDir "app.db")) -and -not $AllowExistingData) {
    Write-Host "数据目录已存在 app.db: $DataDir"
    Write-Host "为保护真实数据，脚本默认中止。请改用 -DataDir <全新空目录>，"
    Write-Host "或加 -AllowExistingData（脚本会先备份 app.db 与 config.toml）。"
    Stop-Transcript | Out-Null
    exit 2
}
if ((Test-Path (Join-Path $DataDir "app.db")) -and $AllowExistingData) {
    $backup = Join-Path $LogDir "data-backup"
    New-Item -ItemType Directory -Force -Path $backup | Out-Null
    Copy-Item (Join-Path $DataDir "app.db") $backup -Force
    if (Test-Path (Join-Path $DataDir "config.toml")) {
        Copy-Item (Join-Path $DataDir "config.toml") $backup -Force
    }
    Write-Host "已备份现有数据到 $backup"
}

$installDir = Join-Path $env:LOCALAPPDATA "Programs\小兔进销存"
$serverExe = Join-Path $installDir "xiaotu-server.exe"
$shellExe = Join-Path $installDir "xiaotu-shell.exe"

# ---------- 1. 编译 ----------
if ($SkipBuild) {
    Skip "MSVC 构建" "指定了 -SkipBuild"
} else {
    Invoke-Check "MSVC 构建" {
        Push-Location $Repo
        try {
            $configure = Invoke-Native (Join-Path $LogDir "configure.log") { cmake --preset windows-release }
            if ($configure.ExitCode -ne 0) { throw "cmake configure 失败，退出码 $($configure.ExitCode)" }
            $build = Invoke-Native (Join-Path $LogDir "build.log") { cmake --build --preset windows-release }
            if ($build.ExitCode -ne 0) { throw "构建失败，退出码 $($build.ExitCode)" }
            Pass "MSVC 构建" "windows-release 配置与构建成功"
        } finally { Pop-Location }
    }
    Invoke-Check "编译参数不含 /we7560" {
        $ninja = Join-Path $Repo "build\windows\build.ninja"
        if (-not (Test-Path $ninja)) { throw "未找到 build/windows/build.ninja，先跑 MSVC 构建" }
        $hits = @(Select-String -Path $ninja -Pattern "/we7560" -ErrorAction SilentlyContinue)
        if ($hits.Count -gt 0) {
            throw "build.ninja 中仍有 $($hits.Count) 处 /we7560：MSVC 14.4x 下 cl 会判其无效并每 TU 刷一条 D9014，应移除"
        }
        Pass "编译参数不含 /we7560" "build.ninja 中 /we7560 出现 0 次（C7560 在 MSVC 默认即 error，无需该开关）"
    }
    Invoke-Check "ctest" {
        Push-Location $Repo
        try {
            $ctestLog = Join-Path $LogDir "ctest.log"
            $run = Invoke-Native $ctestLog { ctest --preset windows-release --output-on-failure }
            if ($run.ExitCode -ne 0) {
                $failed = Select-String -Path $ctestLog -Pattern "^\s*\d+/\d+ Test\s+#\d+:\s+.+\*\*\*Failed" -ErrorAction SilentlyContinue
                $names = ($failed | ForEach-Object { $_.Line.Trim() }) -join "; "
                throw "ctest 失败（退出码 $($run.ExitCode)）: $names"
            }
            $summary = Select-String -Path $ctestLog -Pattern "tests passed" | Select-Object -Last 1
            Pass "ctest" ($summary.Line.Trim())
        } finally { Pop-Location }
    }
}

# ---------- 2. 静态运行库 ----------
Invoke-Check "静态运行库 /MT" {
    $dumpbin = (Get-Command dumpbin.exe -ErrorAction SilentlyContinue).Source
    if (-not $dumpbin) {
        Skip "静态运行库 /MT" "未在 PATH 找到 dumpbin，请在 VS 开发者命令行运行"
        return
    }
    $targets = @()
    $builtServer = Join-Path $Repo "build\windows\server\xiaotu-server.exe"
    if (Test-Path $builtServer) { $targets += $builtServer }
    if (Test-Path $serverExe) { $targets += $serverExe }
    $builtShell = Join-Path $Repo "shell\win\build\xiaotu-shell.exe"
    if (Test-Path $builtShell) { $targets += $builtShell }
    if (Test-Path $shellExe) { $targets += $shellExe }
    if ($targets.Count -eq 0) { Skip "静态运行库 /MT" "找不到已构建的 exe"; return }
    $bad = @()
    foreach ($exe in $targets) {
        $out = & $dumpbin /dependents $exe 2>&1 | Out-String
        foreach ($dll in @("vcruntime140.dll", "msvcp140.dll", "ucrtbased.dll")) {
            if ($out -match [regex]::Escape($dll)) { $bad += ("{0} -> {1}" -f (Split-Path $exe -Leaf), $dll) }
        }
    }
    if ($bad.Count -gt 0) { throw ("仍依赖动态运行库: " + ($bad -join "; ")) }
    Pass "静态运行库 /MT" ("检查 " + $targets.Count + " 个 exe，未发现 vcruntime/msvcp/ucrtbased")
}

# ---------- 3. C7560 是否被当作错误（临时 worktree） ----------
# 只验证行为，不依赖 /we7560 这个开关：MSVC 14.44 把 C7560 默认就当错误处理，
# 而 /we7560 会被 cl 判为无效参数（warning D9014，见构建日志），开关本身不起作用。
Invoke-Check "C7560 生效" {
    $wt = Join-Path $env:TEMP ("xiaotu-we7560-" + [guid]::NewGuid().ToString("N"))
    [void]$script:TempDirs.Add($wt)
    $add = Invoke-Native (Join-Path $LogDir "worktree.log") { git -C $Repo worktree add --detach $wt HEAD }
    if ($add.ExitCode -ne 0) { throw "git worktree 创建失败（退出码 $($add.ExitCode)），见 worktree.log" }
    try {
        $file = Join-Path $wt "core\src\document.cpp"
        $probe = "`nnamespace xiaotu_we7560_probe {`nstruct Probe { int a; int b; };`nint probe() { Probe p{ .b = 2, .a = 1 }; return p.a; }`n}`n"
        Add-Content -Path $file -Value $probe -Encoding UTF8
        Push-Location $wt
        try {
            $configure = Invoke-Native (Join-Path $LogDir "we7560-configure.log") { cmake --preset windows-release }
            if ($configure.ExitCode -ne 0) { throw "worktree 内 cmake configure 失败，退出码 $($configure.ExitCode)" }
            $build = Invoke-Native (Join-Path $LogDir "we7560.log") { cmake --build --preset windows-release --target xiaotu_core }
        } finally { Pop-Location }
        if ($build.ExitCode -eq 0) { throw "错序指定初始化器仍然编译成功，C7560 未按错误处理" }
        if ($build.Output -notmatch "C7560") { throw "编译失败但输出中没有 C7560，需人工看 we7560.log" }
        Pass "C7560 生效" "错序初始化器编译失败且输出含 C7560（MSVC 14.44 默认即 error；/we7560 无效，cl 报 D9014）"
    } finally {
        $remove = Invoke-Native (Join-Path $LogDir "worktree-remove.log") { git -C $Repo worktree remove --force $wt }
        if ($remove.ExitCode -ne 0 -and (Test-Path $wt)) {
            & git -C $Repo worktree prune 2>&1 | Out-Null
            Remove-Item -Recurse -Force $wt -ErrorAction SilentlyContinue
        }
    }
    $dirty = & git -C $Repo status --porcelain -- core/src/document.cpp
    if ($dirty) { throw "仓库 document.cpp 被改动: $dirty" }
    Pass "C7560 未污染仓库" "document.cpp 无改动"
}

# ---------- 4. 安装包 ----------
$installerPath = Find-Installer $Installer
if ($SkipInstall) {
    Skip "安装包检查" "指定了 -SkipInstall"
} elseif (-not $installerPath) {
    Skip "安装包检查" "未找到安装包，请用 -Installer 指定"
} else {
    Invoke-Check "安装包签名状态" {
        $dir = Split-Path $installerPath -Parent
        $sign = Join-Path $dir "SIGNING.txt"
        if (-not (Test-Path $sign)) { throw "安装包同目录缺少 SIGNING.txt: $dir" }
        $text = Get-Content -Raw $sign
        if ($text -match "已签名" -and $text -notmatch "未签名") { throw "SIGNING.txt 标记为已签名，与预期不符" }
        if ($text -notmatch "未签名") { throw "SIGNING.txt 未明确写未签名" }
        Pass "安装包签名状态" "SIGNING.txt: $($text.Trim())"
    }
    Invoke-Check "安装包 SHA256" {
        $h = Get-FileHash -Algorithm SHA256 $installerPath
        Pass "安装包 SHA256" ("{0}  {1}" -f $h.Hash, $h.Path)
    }

    if (-not $Yes) {
        Write-Host "即将静默安装到 $installDir，并写入 %LOCALAPPDATA%；按 Enter 继续，Ctrl+C 取消。"
        Read-Host | Out-Null
    }

    # ---------- 5. 静默安装 ----------
    Invoke-Check "静默安装" {
        $log = Join-Path $LogDir "install.log"
        $p = Start-Process -FilePath $installerPath -ArgumentList @(
            "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/LOG=$log"
        ) -Wait -PassThru
        if ($p.ExitCode -ne 0) { throw "安装器退出码 $($p.ExitCode)（Inno：0=成功，1=用户取消，2=致命错误）" }
        Pass "静默安装" "安装器退出码 0"
    }

    # ---------- 6. 路径与快捷方式 ----------
    Invoke-Check "安装路径" {
        if (-not (Test-Path $serverExe)) { throw "未找到 $serverExe" }
        if (Test-Path (Join-Path $env:ProgramFiles "小兔进销存")) { throw "在 Program Files 发现安装目录，疑似提权安装" }
        Pass "安装路径" $serverExe
    }
    Invoke-Check "开始菜单快捷方式" {
        $start = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs"
        $found = Get-ChildItem -Path $start -Recurse -Filter "*小兔进销存*.lnk" -ErrorAction SilentlyContinue
        if (-not $found) { throw "开始菜单未找到快捷方式" }
        Pass "开始菜单快捷方式" $found[0].FullName
    }
    Invoke-Check "桌面快捷方式" {
        $hasShell = Test-Path $shellExe
        $desktop = Join-Path $env:USERPROFILE "Desktop"
        $found = Get-ChildItem -Path $desktop -Filter "*小兔进销存*.lnk" -ErrorAction SilentlyContinue
        if ($hasShell -and -not $found) { throw "有壳但桌面没有快捷方式" }
        if (-not $hasShell -and $found) { throw "无壳却出现桌面快捷方式" }
        Pass "桌面快捷方式" $(if ($hasShell) { "存在（有壳）" } else { "不存在（无壳）" })
    }
    Invoke-Check "数据目录位置" {
        if (-not (Test-Path $DataDir)) { throw "数据目录不存在: $DataDir" }
        Pass "数据目录位置" $DataDir
    }

    # ---------- 7. 启动与接口 ----------
    $script:ServerProc = $null
    $script:WebSession = New-Object Microsoft.PowerShell.Commands.WebRequestSession
    $script:BaseUrl = ""
    Invoke-Check "单机版启动与接口" {
        $out = Join-Path $LogDir "server-stdout.log"
        $err = Join-Path $LogDir "server-stderr.log"
        $script:ServerProc = Start-Process -FilePath $serverExe -ArgumentList @("--data", "`"$DataDir`"") `
            -RedirectStandardOutput $out -RedirectStandardError $err -PassThru
        $info = Wait-Server $out 20
        if (-not $info) { throw "20 秒内未读到服务地址，见 $out" }
        $login = Get-HttpResult "POST" ($info.Base + "/api/session") ('{"token":"' + $info.Token + '"}') $script:WebSession
        if ($login.Status -ne 200) { throw "单机 token 换会话失败: HTTP $($login.Status) $($login.Body)" }
        $script:BaseUrl = $info.Base
        $health = Get-HttpResult "GET" ($info.Base + "/api/health") $null $null
        if ($health.Status -ne 200) { throw "health HTTP $($health.Status)" }
        $hv = ($health.Body | ConvertFrom-Json).version
        if ($hv -ne $ExpectedVersion) { throw "版本号 $hv，期望 $ExpectedVersion" }
        $me = Get-HttpResult "GET" ($info.Base + "/api/me") $null $script:WebSession
        if ($me.Status -ne 200) { throw "/api/me HTTP $($me.Status)" }
        # 数据必须落在指定的 -DataDir，而不是其它位置（默认目录/上次的目录）。
        $db = Join-Path $DataDir "app.db"
        if (-not (Test-Path $db)) { throw "启动后未在 -DataDir 生成 app.db: $db" }
        Pass "单机版启动与接口" "health 200，version=$hv；/api/me 200；app.db 落在 -DataDir"
    }

    # ---------- 8. 业务冒烟 ----------
    Invoke-Check "业务冒烟" {
        if (-not $script:BaseUrl) { throw "服务未就绪，先看上一项" }
        $session = $script:WebSession
        $info = [pscustomobject]@{ Base = $script:BaseUrl }
        $item = Get-HttpResult "POST" ($info.Base + "/api/items") '{"code":"VERIFY-ITEM","name":"验收商品","unit_id":1}' $session
        if ($item.Status -ne 200) { throw "建商品 HTTP $($item.Status): $($item.Body)" }
        $itemId = ($item.Body | ConvertFrom-Json).id
        $cus = Get-HttpResult "POST" ($info.Base + "/api/customers") '{"code":"VERIFY-CUS","name":"验收客户"}' $session
        if ($cus.Status -ne 200) { throw "建客户 HTTP $($cus.Status): $($cus.Body)" }
        $today = (Get-Date).ToString("yyyy-MM-dd")
        $doc = '{"doc_type":"PUR_IN","warehouse_id":1,"doc_date":"' + $today + '","partner":"验收供应商","save_as_draft":true,"lines":[{"item_id":' + $itemId + ',"qty":10,"price_cents":100}]}'
        $created = Get-HttpResult "POST" ($info.Base + "/api/documents") $doc $session
        if ($created.Status -ne 200) { throw "建采购入库 HTTP $($created.Status): $($created.Body)" }
        $docId = ($created.Body | ConvertFrom-Json).id
        $appr = Get-HttpResult "POST" ($info.Base + "/api/documents/$docId/approve") '{}' $session
        if ($appr.Status -ne 200) { throw "审核 HTTP $($appr.Status): $($appr.Body)" }
        $bal = Get-HttpResult "GET" ($info.Base + "/api/stock/balances") $null $session
        if ($bal.Status -ne 200) { throw "库存余额 HTTP $($bal.Status)" }
        if ($bal.Body -notmatch "验收商品") { throw "库存余额中未出现验收商品" }
        Pass "业务冒烟" "商品/客户/采购入库/审核/库存均 200"
    }

    Invoke-Check "停止服务" {
        $proc = $script:ServerProc
        if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
        Start-Sleep -Seconds 1
        if ($proc -and (Get-Process -Id $proc.Id -ErrorAction SilentlyContinue)) { throw "进程未退出" }
        Pass "停止服务" "进程已退出"
    }

    # ---------- 9. 升级 ----------
    Invoke-Check "同版本覆盖安装" {
        $db = Join-Path $DataDir "app.db"
        if (-not (Test-Path $db)) { throw "覆盖安装前 app.db 不存在" }
        $before = (Get-FileHash -Algorithm SHA256 $db).Hash
        $log = Join-Path $LogDir "upgrade.log"
        $p = Start-Process -FilePath $installerPath -ArgumentList @(
            "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/LOG=$log"
        ) -Wait -PassThru
        if ($p.ExitCode -ne 0) { throw "覆盖安装退出码 $($p.ExitCode)" }
        if (-not (Test-Path $db)) { throw "覆盖安装后 app.db 丢失" }
        $stale = Get-ChildItem -Path $installDir -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.Extension -eq ".old" -or $_.Extension -eq ".tmp" }
        if ($stale) { throw ("程序目录残留旧文件: " + (($stale | ForEach-Object { $_.Name }) -join ", ")) }
        Pass "同版本覆盖安装" "app.db 保留，程序目录无 .old/.tmp"
    }

    # ---------- 10. 卸载 ----------
    Invoke-Check "静默卸载" {
        $unins = Join-Path $installDir "unins000.exe"
        if (-not (Test-Path $unins)) { throw "未找到卸载器 $unins" }
        $p = Start-Process -FilePath $unins -ArgumentList @(
            "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART"
        ) -Wait -PassThru
        if ($p.ExitCode -ne 0) { throw "卸载器退出码 $($p.ExitCode)（静默卸载应为 0）" }
        Start-Sleep -Seconds 2
        if (Test-Path (Join-Path $installDir "xiaotu-server.exe")) { throw "卸载后程序仍在" }
        if (-not (Test-Path $DataDir)) { throw "卸载后数据目录被删除，应保留" }
        $start = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs"
        $lnk = Get-ChildItem -Path $start -Recurse -Filter "*小兔进销存*.lnk" -ErrorAction SilentlyContinue
        if ($lnk) { throw "卸载后快捷方式仍在" }
        Pass "静默卸载" "程序清除、快捷方式清除、数据目录保留"
    }
}

# ---------- 11. MANUAL ----------
Manual "WebView2 缺失提示" "在虚拟机卸载 WebView2 后双击壳，应提示缺失而非白屏/闪退"
Manual "窗口位置与大小记忆" "移动并缩放窗口后重启，应恢复上次位置与大小"
Manual "打印对话框" "打开打印预览并调起系统对话框，检查纸型/边距/缩放"
Manual "SmartScreen 表现" "未签名包首次运行应出现「未知发布者 → 更多信息 → 仍要运行」"

# ---------- 汇总 ----------
$pass = @($script:Results | Where-Object { $_.Status -eq "PASS" }).Count
$fail = @($script:Results | Where-Object { $_.Status -eq "FAIL" }).Count
$skip = @($script:Results | Where-Object { $_.Status -eq "SKIP" }).Count
$manual = @($script:Results | Where-Object { $_.Status -eq "MANUAL" }).Count
Write-Host ""
Write-Host ("汇总: PASS={0} FAIL={1} SKIP={2} MANUAL={3}" -f $pass, $fail, $skip, $manual)
Write-Host ("日志: " + (Join-Path $LogDir "verify.log"))
foreach ($t in $script:TempDirs) {
    if (Test-Path $t) { Remove-Item -Recurse -Force $t -ErrorAction SilentlyContinue }
}
Stop-Transcript | Out-Null
if ($script:HasFail) { exit 1 } else { exit 0 }
