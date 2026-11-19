# setup-hooks.ps1 — 配置 git pre-commit hook（UTF-8 BOM 门禁）
#
# 在任何有 .ps1 变更的仓库根目录执行一次即可：
#   powershell -ExecutionPolicy Bypass -File tools\hooks\setup-hooks.ps1
#
# 做两件事：
#   1. git config core.hooksPath tools/hooks — 让 git 用仓库里的 hooks 模板
#      （git 2.9+ 支持这个配置，替代手动复制到 .git/hooks/）
#   2. 确保 tools/hooks/pre-commit 有可执行权限（git bash 下 chmod +x）
#
# 之后每次 git commit 都会自动扫所有 staged .ps1，缺 BOM 直接阻断。

[CmdletBinding()]
param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
)

$ErrorActionPreference = "Stop"
Set-Location $RepoRoot

$hooksDir = Join-Path $RepoRoot "tools\hooks"
$preCommit = Join-Path $hooksDir "pre-commit"

# 1. 检查 pre-commit 脚本存在
if (-not (Test-Path $preCommit)) {
    Write-Error "pre-commit hook 不存在：$preCommit"
    exit 1
}

# 2. 配置 core.hooksPath
git config core.hooksPath "tools/hooks"
$current = git config core.hooksPath
Write-Host "core.hooksPath = $current" -ForegroundColor Cyan

if ($current -ne "tools/hooks") {
    Write-Error "core.hooksPath 设置失败（当前值：$current）"
    exit 1
}

# 3. 如果有 Git Bash，给 pre-commit 加可执行权限（Windows 上必要）
$bash = Get-Command bash -ErrorAction SilentlyContinue
if ($bash) {
    & $bash -c "chmod +x '$preCommit'"
    Write-Host "chmod +x tools/hooks/pre-commit" -ForegroundColor Green
} else {
    Write-Host "(未找到 git bash，跳过 chmod；Mac/Linux 请执行 chmod +x tools/hooks/pre-commit)" -ForegroundColor Yellow
}

# 4. 验证 hook 能否跑（dry-run，当前仓库 staged 无 .ps1 应该直接退出 0）
$tmpFile = Join-Path $Env:TEMP "hooks-test.txt"
git diff --cached --name-only --diff-filter=ACM | Set-Content -Path $tmpFile -NoNewline
if ((Get-Content $tmpFile -Raw) -notmatch '\.ps1$') {
    Write-Host ""
    Write-Host "验证 hook（当前仓库 staged 无 .ps1，应该直接通过）：" -ForegroundColor Cyan
    if ($bash) {
        & $bash -c "'$preCommit'; echo exit=$?"
    }
} else {
    Write-Host ""
    Write-Host "注意：当前仓库 staged 有 .ps1，下一次 git commit 时会跑 hook 检查 BOM。" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "✅ hooks 已配置。下次 git commit 时自动扫描 .ps1 UTF-8 + BOM。" -ForegroundColor Green
