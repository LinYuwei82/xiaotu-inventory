#requires -Version 5.1
<#
  Windows 壳的 UI 回归检查（替代 verify.ps1 里的三项 MANUAL）。

  - 窗口位置与大小记忆
  - 打印预览（WebView2 内置打印界面）
  - WebView2 缺失提示

  每次都复制一份壳与服务端到独立临时目录，并把 LOCALAPPDATA 指向该目录，
  不读写真实数据目录，也不写 dist。
#>
[CmdletBinding()]
param(
    # 壳路径；默认取仓库里 shell\win\build\xiaotu-shell.exe
    [string]$ShellPath = "",
    # 服务端路径；默认优先用壳同目录，其次 build\windows\server\xiaotu-server.exe
    [string]$ServerPath = "",
    # 等待窗口/服务端就绪的上限（秒）
    [int]$StartupTimeoutSec = 60,
    # 等待 CDP / 打印界面出现的上限（秒）
    [int]$CdpTimeoutSec = 90,
    # 窗口位置与大小的允许偏差（像素）
    [int]$WindowTolerancePx = 2,
    # WebView2 远程调试端口
    [int]$CdpPort = 9345,
    [switch]$SkipWindowState,
    [switch]$SkipPrint,
    [switch]$SkipWebView2Missing
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class UiChecksWin {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int hh, bool repaint);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  [DllImport("shcore.dll")] public static extern int SetProcessDpiAwareness(int v);

  public static string Title(IntPtr h) { var sb = new StringBuilder(2048); GetWindowTextW(h, sb, 2048); return sb.ToString(); }
  public static string Class(IntPtr h) { var sb = new StringBuilder(256); GetClassNameW(h, sb, 256); return sb.ToString(); }
  public static uint PidOf(IntPtr h) { uint pid; GetWindowThreadProcessId(h, out pid); return pid; }

  public static List<IntPtr> AllTop() {
    var list = new List<IntPtr>();
    EnumWindows(delegate(IntPtr h, IntPtr p) { list.Add(h); return true; }, IntPtr.Zero);
    return list;
  }
  public static List<IntPtr> Deep() {
    var list = new List<IntPtr>();
    EnumWindows(delegate(IntPtr h, IntPtr p) {
      list.Add(h);
      EnumChildWindows(h, delegate(IntPtr c, IntPtr q) { list.Add(c); return true; }, IntPtr.Zero);
      return true;
    }, IntPtr.Zero);
    return list;
  }
  public static IntPtr MainOf(int pid) {
    IntPtr best = IntPtr.Zero; long area = 0;
    foreach (var h in AllTop()) {
      if ((int)PidOf(h) != pid) continue;
      if (Class(h) == "#32770") continue;
      if (!IsWindowVisible(h)) continue;
      RECT r; if (!GetWindowRect(h, out r)) continue;
      long a = (long)(r.Right - r.Left) * (r.Bottom - r.Top);
      if (a > area) { area = a; best = h; }
    }
    return best;
  }
  public static IntPtr DialogOf(int pid) {
    IntPtr best = IntPtr.Zero; long area = 0;
    foreach (var h in AllTop()) {
      if ((int)PidOf(h) != pid) continue;
      if (Class(h) != "#32770") continue;
      if (!IsWindowVisible(h)) continue;
      RECT r; if (!GetWindowRect(h, out r)) continue;
      long a = (long)(r.Right - r.Left) * (r.Bottom - r.Top);
      if (a > area) { area = a; best = h; }
    }
    return best;
  }
  public static List<string> Children(IntPtr parent) {
    var list = new List<string>();
    EnumChildWindows(parent, delegate(IntPtr h, IntPtr p) { list.Add(Class(h) + " | " + Title(h)); return true; }, IntPtr.Zero);
    return list;
  }
  public static void Close(IntPtr h) { PostMessageW(h, 0x0010, IntPtr.Zero, IntPtr.Zero); }
}
'@
try { $null = [UiChecksWin]::SetProcessDpiAwareness(2) } catch { }

# ---------------- 结果与汇总 ----------------

$script:Results = New-Object System.Collections.ArrayList
$script:HasFail = $false
$script:TempDirs = New-Object System.Collections.ArrayList

function Add-Result([string]$Name, [string]$Status, [string]$Detail) {
    [void]$script:Results.Add([pscustomobject]@{ Name = $Name; Status = $Status; Detail = $Detail })
    if ($Status -eq "FAIL") { $script:HasFail = $true }
    Write-Host ("[{0}] {1}：{2}" -f $Status, $Name, $Detail)
}
function Add-Pass([string]$n, [string]$d) { Add-Result $n "PASS" $d }
function Add-Fail([string]$n, [string]$d) { Add-Result $n "FAIL" $d }
function Add-Skip([string]$n, [string]$d) { Add-Result $n "SKIP" $d }

# ---------------- 路径、临时目录与进程 ----------------

$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $ShellPath) { $ShellPath = Join-Path $repoRoot "shell\win\build\xiaotu-shell.exe" }
if (-not $ServerPath) {
    $beside = Join-Path (Split-Path $ShellPath -Parent) "xiaotu-server.exe"
    if (Test-Path $beside) { $ServerPath = $beside }
    else { $ServerPath = Join-Path $repoRoot "build\windows\server\xiaotu-server.exe" }
}

function Assert-Prereq {
    if (-not (Test-Path $ShellPath)) { throw "找不到壳：$ShellPath（先按 README 构建 windows 壳）" }
    if (-not (Test-Path $ServerPath)) { throw "找不到服务端：$ServerPath（先构建 windows-release）" }
}

function New-Stage([string]$Tag) {
    Assert-Prereq
    $dir = Join-Path $env:TEMP ("ui-checks-" + $Tag + "-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
    New-Item -ItemType Directory -Force $dir | Out-Null
    [void]$script:TempDirs.Add($dir)
    Copy-Item $ShellPath (Join-Path $dir "xiaotu-shell.exe") -Force
    Copy-Item $ServerPath (Join-Path $dir "xiaotu-server.exe") -Force
    $local = Join-Path $dir "localappdata"
    New-Item -ItemType Directory -Force $local | Out-Null
    return [pscustomobject]@{ Dir = $dir; Local = $local; DataDir = (Join-Path $local "com.simem.xiaotu") }
}

function Start-Shell($Stage, [hashtable]$ExtraEnv) {
    $saved = @{}
    foreach ($k in $ExtraEnv.Keys) {
        $saved[$k] = [Environment]::GetEnvironmentVariable($k, "Process")
        [Environment]::SetEnvironmentVariable($k, $ExtraEnv[$k], "Process")
    }
    try {
        return Start-Process -FilePath (Join-Path $Stage.Dir "xiaotu-shell.exe") -WorkingDirectory $Stage.Dir -PassThru
    } finally {
        foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k], "Process") }
    }
}

function Stop-AllShells {
    Get-Process xiaotu-shell, xiaotu-server -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
}

function Wait-Window([int]$ProcId, [int]$TimeoutMs) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $TimeoutMs) {
        $h = [UiChecksWin]::MainOf($ProcId)
        if ($h -ne [IntPtr]::Zero) { return $h }
        Start-Sleep -Milliseconds 300
    }
    return [IntPtr]::Zero
}

function Wait-Dialog([int]$ProcId, [int]$TimeoutMs) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $TimeoutMs) {
        $h = [UiChecksWin]::DialogOf($ProcId)
        if ($h -ne [IntPtr]::Zero) { return $h }
        Start-Sleep -Milliseconds 300
    }
    return [IntPtr]::Zero
}

function Get-RectOf([IntPtr]$Hwnd) {
    $r = New-Object UiChecksWin+RECT
    if (-not [UiChecksWin]::GetWindowRect($Hwnd, [ref]$r)) { return $null }
    return $r
}

# ---------------- CDP ----------------

$script:cdpId = 0
function WsSend($ws, $ct, $text) {
    $bytes = [Text.Encoding]::UTF8.GetBytes($text)
    $seg = [System.ArraySegment[byte]]::new($bytes)
    $t = $ws.SendAsync($seg, [System.Net.WebSockets.WebSocketMessageType]::Text, $true, $ct)
    if (-not $t.Wait(15000)) { throw "CDP 发送超时" }
}
function WsRecv($ws, $ct) {
    $buf = New-Object byte[] 262144
    $sb = New-Object System.Text.StringBuilder
    do {
        $seg = [System.ArraySegment[byte]]::new($buf)
        $t = $ws.ReceiveAsync($seg, $ct)
        if (-not $t.Wait(30000)) { throw "CDP 接收超时" }
        $r = $t.Result
        [void]$sb.Append([Text.Encoding]::UTF8.GetString($buf, 0, $r.Count))
    } while (-not $r.EndOfMessage)
    return $sb.ToString()
}
function CdpCall($ws, $ct, $method, $params) {
    $script:cdpId++
    $id = $script:cdpId
    $msg = @{ id = $id; method = $method; params = $params } | ConvertTo-Json -Compress -Depth 12
    WsSend $ws $ct $msg
    while ($true) {
        $raw = WsRecv $ws $ct
        $obj = $raw | ConvertFrom-Json
        if ($obj.id -eq $id) { return $obj }
    }
}
function EvalJs($ws, $ct, $code) {
    $r = CdpCall $ws $ct "Runtime.evaluate" @{ expression = $code; returnByValue = $true; awaitPromise = $true }
    if ($r.result.exceptionDetails) { throw ("页内脚本异常：" + $r.result.exceptionDetails.text) }
    return $r.result.result.value
}
function EvalJsSend($ws, $ct, $code) {
    $script:cdpId++
    $id = $script:cdpId
    $msg = @{ id = $id; method = "Runtime.evaluate"; params = @{ expression = $code; returnByValue = $true; awaitPromise = $true } } | ConvertTo-Json -Compress -Depth 12
    WsSend $ws $ct $msg
    return $id
}
function Get-Targets([int]$Port) {
    try { return Invoke-RestMethod -Uri "http://127.0.0.1:$Port/json/list" -TimeoutSec 3 -UseBasicParsing } catch { return $null }
}
function Connect-Cdp([int]$Port, [int]$TimeoutMs) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $TimeoutMs) {
        $list = Get-Targets $Port
        if ($list) {
            $t = $list | Where-Object { $_.type -eq "page" -and $_.url -like "http://127.0.0.1*" } | Select-Object -First 1
            if ($t) {
                $ws = New-Object System.Net.WebSockets.ClientWebSocket
                $ws.ConnectAsync([Uri]$t.webSocketDebuggerUrl, [Threading.CancellationToken]::None).Wait(15000) | Out-Null
                return [pscustomobject]@{ ws = $ws; url = $t.url }
            }
        }
        Start-Sleep -Milliseconds 500
    }
    return $null
}
function Connect-Target([string]$WsUrl) {
    $ws = New-Object System.Net.WebSockets.ClientWebSocket
    $ws.ConnectAsync([Uri]$WsUrl, [Threading.CancellationToken]::None).Wait(15000) | Out-Null
    return $ws
}

# ---------------- 检查 1：窗口位置与大小记忆 ----------------

function Test-WindowState {
    $name = "窗口位置与大小记忆"
    if ($SkipWindowState) { Add-Skip $name "按参数跳过"; return }
    $stage = $null; $p1 = $null; $p2 = $null
    try {
        $stage = New-Stage "window"
        $x = 120; $y = 140; $w = 1100; $h = 760
        $stateFile = Join-Path $stage.DataDir "shell-window.txt"

        $p1 = Start-Shell $stage @{ LOCALAPPDATA = $stage.Local }
        $h1 = Wait-Window $p1.Id ($StartupTimeoutSec * 1000)
        if ($h1 -eq [IntPtr]::Zero) { Add-Fail $name "第一次启动未出现主窗口"; return }
        if ([UiChecksWin]::DialogOf($p1.Id) -ne [IntPtr]::Zero) { Add-Skip $name "启动即出现提示框（可能缺少 WebView2 Runtime），无法验证窗口记忆"; return }
        Start-Sleep -Seconds 2
        if ([UiChecksWin]::IsZoomed($h1)) { $null = [UiChecksWin]::ShowWindow($h1, 9); Start-Sleep -Milliseconds 400 }
        $null = [UiChecksWin]::MoveWindow($h1, $x, $y, $w, $h, $true)
        Start-Sleep -Milliseconds 800
        $before = Get-RectOf $h1
        if (-not $before) { Add-Fail $name "取不到窗口矩形"; return }
        $beforeW = $before.Right - $before.Left
        $beforeH = $before.Bottom - $before.Top

        [UiChecksWin]::Close($h1) | Out-Null
        $p1.WaitForExit(25000) | Out-Null
        if (-not $p1.HasExited) { Add-Fail $name "关闭窗口后壳未退出"; return }
        if ($p1.ExitCode -ne 0) { Add-Fail $name ("关闭后退出码 " + $p1.ExitCode); return }

        if (-not (Test-Path $stateFile)) { Add-Fail $name "未生成记忆文件：$stateFile"; return }
        $parts = ((Get-Content $stateFile -Raw) -split "\s+") | Where-Object { $_ -ne "" }
        if ($parts.Count -lt 4) { Add-Fail $name ("记忆文件内容不完整：" + ((Get-Content $stateFile -Raw))); return }
        $fx = [int]$parts[0]; $fy = [int]$parts[1]; $fw = [int]$parts[2]; $fh = [int]$parts[3]
        if ($fx -ne $before.Left -or $fy -ne $before.Top -or $fw -ne $beforeW -or $fh -ne $beforeH) {
            Add-Fail $name ("记忆文件与关闭前不一致：文件 $fx,$fy,${fw}x${fh} / 关闭前 $($before.Left),$($before.Top),${beforeW}x${beforeH}")
            return
        }

        $p2 = Start-Shell $stage @{ LOCALAPPDATA = $stage.Local }
        $h2 = Wait-Window $p2.Id ($StartupTimeoutSec * 1000)
        if ($h2 -eq [IntPtr]::Zero) { Add-Fail $name "第二次启动未出现主窗口"; return }
        Start-Sleep -Seconds 2
        $after = Get-RectOf $h2
        if (-not $after) { Add-Fail $name "重启后取不到窗口矩形"; return }
        $afterW = $after.Right - $after.Left
        $afterH = $after.Bottom - $after.Top
        $tol = $WindowTolerancePx
        $diff = @(
            [Math]::Abs($after.Left - $before.Left),
            [Math]::Abs($after.Top - $before.Top),
            [Math]::Abs($afterW - $beforeW),
            [Math]::Abs($afterH - $beforeH)
        )
        if (($diff | Measure-Object -Maximum).Maximum -gt $tol) {
            Add-Fail $name ("重启后位置/大小超出容差 ${tol}px：关闭前 $($before.Left),$($before.Top),${beforeW}x${beforeH} / 重启后 $($after.Left),$($after.Top),${afterW}x${afterH}")
            return
        }
        Add-Pass $name ("记忆文件 $fx $fy $fw $fh；重启后 $($after.Left),$($after.Top),${afterW}x${afterH}（偏差 ≤ ${tol}px）")
    } catch {
        Add-Fail $name ("异常：" + $_.Exception.Message)
    } finally {
        if ($p1 -and -not $p1.HasExited) { $p1.WaitForExit(5000) | Out-Null }
        if ($p2 -and -not $p2.HasExited) { $p2.WaitForExit(5000) | Out-Null }
        Stop-AllShells
        if ($stage -and (Test-Path $stage.Dir)) { Remove-Item $stage.Dir -Recurse -Force -ErrorAction SilentlyContinue }
    }
}

# ---------------- 检查 2：打印预览 ----------------

function Test-PrintPreview {
    $name = "打印预览"
    if ($SkipPrint) { Add-Skip $name "按参数跳过"; return }
    $stage = $null; $p = $null; $ws = $null; $pws = $null
    try {
        $stage = New-Stage "print"
        $p = Start-Shell $stage @{
            LOCALAPPDATA = $stage.Local
            WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS = "--remote-debugging-port=$CdpPort"
        }
        $hwnd = Wait-Window $p.Id ($StartupTimeoutSec * 1000)
        if ($hwnd -eq [IntPtr]::Zero) { Add-Fail $name "壳未出现主窗口"; return }

        $cdp = Connect-Cdp $CdpPort ($CdpTimeoutSec * 1000)
        if (-not $cdp) { Add-Skip $name "无法连接 WebView2 调试端口（可能未安装 WebView2 Runtime）"; return }
        $ws = $cdp.ws
        $ct = [Threading.CancellationToken]::None
        Start-Sleep -Seconds 2

        $nav = EvalJs $ws $ct "(() => { const b = document.querySelector('button[data-key=""reports""]'); if (!b) return 'no-nav'; b.click(); return 'clicked'; })()"
        if ($nav -ne "clicked") { Add-Fail $name "找不到报表导航（$nav）"; return }

        $btn = ""
        $swBtn = [Diagnostics.Stopwatch]::StartNew()
        while ($swBtn.ElapsedMilliseconds -lt 15000) {
            Start-Sleep -Milliseconds 400
            $btn = EvalJs $ws $ct "document.querySelector('[data-role=""print-balance""]') ? 'present' : 'missing'"
            if ($btn -eq "present") { break }
        }
        if ($btn -ne "present") { Add-Fail $name "报表页未出现「打印」按钮"; return }
        Start-Sleep -Seconds 2

        $clicked = EvalJs $ws $ct "(() => { const b = document.querySelector('[data-role=""print-balance""]'); if (!b) return 'no'; b.click(); return 'clicked'; })()"
        if ($clicked -ne "clicked") { Add-Fail $name "点击打印失败（$clicked）"; return }

        # 纸张告警确认层（有则点「继续」）
        $modal = ""
        for ($i = 0; $i -lt 12; $i++) {
            Start-Sleep -Milliseconds 350
            $modal = EvalJs $ws $ct "(() => { const b = document.querySelector('.confirm-box'); return b ? b.innerText.replace(/\s+/g, ' ') : ''; })()"
            if ($modal) { break }
        }
        if ($modal) {
            $null = EvalJsSend $ws $ct "(() => { const b = document.querySelector('.confirm-actions .btn.primary'); if (b) b.click(); return 'ok'; })()"
        }

        $target = $null
        $sw = [Diagnostics.Stopwatch]::StartNew()
        while ($sw.ElapsedMilliseconds -lt ($CdpTimeoutSec * 1000 / 3)) {
            Start-Sleep -Milliseconds 500
            $target = (Get-Targets $CdpPort) | Where-Object { $_.url -like "edge://print*" } | Select-Object -First 1
            if ($target) { break }
        }
        if (-not $target) { Add-Fail $name "点击打印后未出现 edge://print/ 目标"; return }

        $pws = Connect-Target $target.webSocketDebuggerUrl
        Start-Sleep -Milliseconds 800
        $uiText = EvalJs $pws $ct @'
(() => {
  const parts = [];
  function walk(root, depth) {
    if (!root || depth > 12) return;
    for (const el of root.querySelectorAll("*")) {
      if (el.shadowRoot) walk(el.shadowRoot, depth + 1);
      if (el.children.length === 0) {
        const t = (el.innerText || "").trim();
        if (t) parts.push(t);
      }
    }
  }
  walk(document, 0);
  return parts.join(" | ");
})()
'@
        if ($uiText -notmatch "打印机") { Add-Fail $name ("打印界面缺少「打印机」（界面文本：" + $uiText.Substring(0, [Math]::Min(120, $uiText.Length)) + "…）"); return }
        if ($uiText -notmatch "份数") { Add-Fail $name "打印界面缺少「份数」"; return }

        $cancel = EvalJs $pws $ct @'
(() => {
  function find(root, depth) {
    if (!root || depth > 16) return null;
    for (const n of root.querySelectorAll("*")) {
      if (n.shadowRoot) { const r = find(n.shadowRoot, depth + 1); if (r) return r; }
      if ((n.innerText || "").trim() === "取消" && n.click) return n;
    }
    return null;
  }
  const b = find(document, 0);
  if (!b) return "not-found";
  b.click();
  return "clicked";
})()
'@
        if ($cancel -ne "clicked") { Add-Fail $name "打印界面里找不到「取消」按钮"; return }

        $closed = $false
        for ($i = 0; $i -lt 20; $i++) {
            Start-Sleep -Milliseconds 500
            if (-not ((Get-Targets $CdpPort) | Where-Object { $_.url -like "edge://print*" })) { $closed = $true; break }
        }
        if (-not $closed) { Add-Fail $name "点取消后打印界面未关闭"; return }

        $responsive = ""
        for ($i = 0; $i -lt 20; $i++) {
            Start-Sleep -Milliseconds 500
            try {
                $v = EvalJs $ws $ct "1+1"
                if ($v -eq 2) { $responsive = "ok"; break }
            } catch { }
        }
        if ($responsive -ne "ok") { Add-Fail $name "取消后主界面无响应"; return }

        $state = [string](EvalJs $ws $ct "(() => { return 'printView=' + !!document.querySelector('[data-print-view]') + ' printMode=' + document.body.classList.contains('print-mode'); })()")
        if ($state -ne "printView=false printMode=false") { Add-Fail $name ("取消后打印视图未清理（$state）"); return }

        Add-Pass $name "出现 edge://print/（含打印机/份数），点取消后关闭，主界面可交互且打印视图已清理"
    } catch {
        Add-Fail $name ("异常：" + $_.Exception.Message)
    } finally {
        if ($pws) { try { $pws.Dispose() } catch { } }
        if ($ws) { try { $ws.Dispose() } catch { } }
        if ($p -and -not $p.HasExited) { $p.WaitForExit(5000) | Out-Null }
        Stop-AllShells
        if ($stage -and (Test-Path $stage.Dir)) { Remove-Item $stage.Dir -Recurse -Force -ErrorAction SilentlyContinue }
    }
}

# ---------------- 检查 3：WebView2 缺失提示 ----------------

function Test-WebView2Missing {
    $name = "WebView2 缺失提示"
    if ($SkipWebView2Missing) { Add-Skip $name "按参数跳过"; return }
    $stage = $null; $p = $null; $p2 = $null; $ws = $null
    try {
        $stage = New-Stage "nowebview"
        $badDir = Join-Path $stage.Dir "no-webview2-runtime"

        $p = Start-Shell $stage @{
            LOCALAPPDATA = $stage.Local
            WEBVIEW2_BROWSER_EXECUTABLE_FOLDER = $badDir
        }
        $dlg = Wait-Dialog $p.Id ([Math]::Min($StartupTimeoutSec, 30) * 1000)
        if ($dlg -eq [IntPtr]::Zero) {
            if (-not $p.HasExited) {
                Stop-AllShells
                Add-Skip $name "WEBVIEW2_BROWSER_EXECUTABLE_FOLDER 未生效（壳正常启动），无法模拟运行时缺失"
            } else {
                Add-Skip $name ("壳提前退出（退出码 " + $p.ExitCode + "），未出现提示框")
            }
            return
        }

        $lines = [UiChecksWin]::Children($dlg)
        $text = ($lines | Where-Object { $_ -like "Static | *" }) -join "`n"
        if ($text -notmatch "HRESULT 0x80070002") {
            Add-Fail $name ("提示未包含 HRESULT 0x80070002： " + ($text -replace "\s+", " "))
            return
        }
        if ($text -notmatch "https://go\.microsoft\.com/fwlink/p/\?LinkId=2124703") {
            Add-Fail $name "提示未包含 WebView2 Runtime 下载链接"
            return
        }

        $okBtn = [IntPtr]::Zero
        foreach ($c in [UiChecksWin]::Deep()) {
            if ([UiChecksWin]::PidOf($c) -ne [UiChecksWin]::PidOf($dlg)) { continue }
            if ([UiChecksWin]::Class($c) -eq "Button" -and [UiChecksWin]::Title($c) -match "^(确定|OK)$") { $okBtn = $c; break }
        }
        if ($okBtn -eq [IntPtr]::Zero) { Add-Fail $name "提示框里找不到「确定」按钮"; return }
        [void][UiChecksWin]::SendMessageW($okBtn, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)

        $p.WaitForExit(20000) | Out-Null
        if (-not $p.HasExited) { Add-Fail $name "点确定后壳未退出"; return }
        if ($p.ExitCode -ne 1) { Add-Fail $name ("点确定后退出码 " + $p.ExitCode + "（预期 1）"); return }

        # 环境复原：再次正常启动，确认 WebView2 可用
        $p2 = Start-Shell $stage @{
            LOCALAPPDATA = $stage.Local
            WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS = "--remote-debugging-port=$CdpPort"
        }
        $h2 = Wait-Window $p2.Id ($StartupTimeoutSec * 1000)
        if ($h2 -eq [IntPtr]::Zero) { Add-Skip $name "提示文案与退出码正确；未出现主窗口（本机可能没有 WebView2 Runtime），跳过复原启动校验"; return }
        $cdp = Connect-Cdp $CdpPort ($CdpTimeoutSec * 1000)
        if (-not $cdp) { Add-Skip $name "提示文案与退出码正确；无法连接 WebView2（本机可能没有 WebView2 Runtime），跳过复原启动校验"; return }
        $ws = $cdp.ws
        $ct = [Threading.CancellationToken]::None
        Start-Sleep -Seconds 2
        $hash = [string](EvalJs $ws $ct "location.hash")
        if ($hash -ne "#/item") { Add-Fail $name ("复原后页面未加载到 #/item（当前 " + $hash + "）"); return }
        [UiChecksWin]::Close($h2) | Out-Null
        $p2.WaitForExit(20000) | Out-Null
        if (-not $p2.HasExited) { Add-Fail $name "复原后关闭壳未退出"; return }
        if ($p2.ExitCode -ne 0) { Add-Fail $name ("复原后退出码 " + $p2.ExitCode); return }

        Add-Pass $name "提示为「无法创建 WebView2 环境（HRESULT 0x80070002）」并含下载链接；确定后退出码 1；复原后正常加载 #/item"
    } catch {
        Add-Fail $name ("异常：" + $_.Exception.Message)
    } finally {
        if ($ws) { try { $ws.Dispose() } catch { } }
        if ($p -and -not $p.HasExited) { $p.WaitForExit(5000) | Out-Null }
        if ($p2 -and -not $p2.HasExited) { $p2.WaitForExit(5000) | Out-Null }
        Stop-AllShells
        if ($stage -and (Test-Path $stage.Dir)) { Remove-Item $stage.Dir -Recurse -Force -ErrorAction SilentlyContinue }
    }
}

# ---------------- 主流程 ----------------

Write-Host ("壳：    " + $ShellPath)
Write-Host ("服务端：" + $ServerPath)
Write-Host ""

Test-WindowState
Test-PrintPreview
Test-WebView2Missing

Write-Host ""
$pass = @($script:Results | Where-Object { $_.Status -eq "PASS" }).Count
$fail = @($script:Results | Where-Object { $_.Status -eq "FAIL" }).Count
$skip = @($script:Results | Where-Object { $_.Status -eq "SKIP" }).Count
Write-Host ("汇总: PASS={0} FAIL={1} SKIP={2}" -f $pass, $fail, $skip)

# 残留清理（正常情况下各检查已各自清理，这里兜底）
Stop-AllShells
foreach ($d in $script:TempDirs) {
    if (Test-Path $d) { Remove-Item $d -Recurse -Force -ErrorAction SilentlyContinue }
}
$left = @($script:TempDirs | Where-Object { Test-Path $_ }).Count
Write-Host ("临时目录残留: " + $left)

if ($script:HasFail) { exit 1 } else { exit 0 }
