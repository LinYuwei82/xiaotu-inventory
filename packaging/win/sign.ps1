<#
  小兔进销存 Windows 代码签名（PowerShell 5.1 / 7）

  顺序：先签 xiaotu-server.exe 与 xiaotu-shell.exe，再跑 ISCC 打包，最后签安装包。
  包内 exe 的签名在 Inno 复制时不会丢失；反过来先打包再签 exe，包里的 exe 仍是未签名。

  用法：
    $env:XIAOTU_CERT_PASSWORD = 'pfx 口令'
    .\packaging\win\sign.ps1 -PfxPath D:\certs\xiaotu.pfx `
      -Files build\windows\server\xiaotu-server.exe,shell\win\build\xiaotu-shell.exe
    .\packaging\win\sign.ps1 -PfxPath D:\certs\xiaotu.pfx          # 默认签 exe 与 out 下最新安装包
    .\packaging\win\sign.ps1 -Thumbprint 0123456789ABCDEF...       # 证书在存储/USB token 中

  说明：
    - 不传 -Files 时，签服务端 exe、按候选顺序找到的壳，以及 out 下最新的 *-setup.exe。
    - PFX 口令依次取 -PfxPassword、$env:XIAOTU_CERT_PASSWORD；不写入仓库与 SIGNING.txt。
    - 证书在 USB token / HSM 中时用 -Thumbprint，签名时可能弹出 PIN 输入框。
    - 任一文件签名或验签失败都以非 0 退出，并把「签名失败」写进 SIGNING.txt，绝不写「已签名」。
#>
[CmdletBinding()]
param(
    [string[]]$Files = @(),
    [string]$PfxPath = "",
    [string]$PfxPassword = "",
    [string]$Thumbprint = "",
    [switch]$MachineStore,
    [string]$TimestampUrl = "http://timestamp.digicert.com",
    [switch]$NoTimestamp,
    [string]$SigningNote = "",
    [string]$SignToolPath = ""
)

$ErrorActionPreference = "Stop"
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $SigningNote) {
    $SigningNote = Join-Path $repo "packaging\win\out\SIGNING.txt"
}

function Resolve-SignTool {
    if ($SignToolPath) {
        if (-not (Test-Path -LiteralPath $SignToolPath)) {
            throw "指定的 signtool.exe 不存在：$SignToolPath"
        }
        return (Resolve-Path -LiteralPath $SignToolPath).Path
    }
    $kits = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin\*\x64\signtool.exe"
    $found = Get-ChildItem $kits -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $found) {
        throw "未找到 signtool.exe，请安装 Windows SDK（含 Windows 签名工具）"
    }
    return $found.FullName
}

# 签名失败时也要让 SIGNING.txt 说真话，避免产物被当成已签名分发。
function Write-FailureNote([string]$reason) {
    try {
        New-Item -ItemType Directory -Force (Split-Path $SigningNote -Parent) | Out-Null
        "签名失败：$reason" | Out-File -FilePath $SigningNote -Encoding utf8
    } catch {
    }
}

try {
    if ($PfxPath -and $Thumbprint) {
        throw "PfxPath 与 Thumbprint 只能二选一"
    }

    if (-not $Files -or $Files.Count -eq 0) {
        $candidates = @(
            (Join-Path $repo "build\windows\server\xiaotu-server.exe"),
            (Join-Path $repo "build\windows\shell\win\xiaotu-shell.exe"),
            (Join-Path $repo "build\windows\shell\win\Release\xiaotu-shell.exe"),
            (Join-Path $repo "shell\win\build\xiaotu-shell.exe"),
            (Join-Path $repo "shell\win\build\Release\xiaotu-shell.exe")
        )
        $Files = @($candidates | Where-Object { Test-Path -LiteralPath $_ })
        $outDir = Join-Path $repo "packaging\win\out"
        $setup = Get-ChildItem $outDir -Filter "*-setup.exe" -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($setup) {
            $Files += $setup.FullName
        }
    }
    $Files = @($Files | ForEach-Object { (Resolve-Path -LiteralPath $_).Path })
    if ($Files.Count -eq 0) {
        throw "没有可签名的文件；先用 cmake 构建，或显式传 -Files"
    }

    $usePfx = [bool]$PfxPath
    if ($usePfx) {
        $pfx = (Resolve-Path -LiteralPath $PfxPath).Path
        if (-not $PfxPassword) {
            $PfxPassword = $env:XIAOTU_CERT_PASSWORD
        }
        if (-not $PfxPassword) {
            throw "PFX 需要口令：传 -PfxPassword，或设置环境变量 XIAOTU_CERT_PASSWORD"
        }
    } elseif ($Thumbprint) {
        $Thumbprint = ($Thumbprint -replace "\s", "").ToUpperInvariant()
        if (-not $Thumbprint) {
            throw "Thumbprint 不能为空"
        }
    } else {
        $guidance = @(
            "还没有可用的代码签名证书，本次未签名。",
            "Windows Authenticode 证书需向 CA 购买（OV/EV）；私钥通常放在 USB token 或云端 HSM，不再直接交付裸 PFX。",
            "拿到证书后：token/证书存储用 -Thumbprint，CA 仍提供 PFX 时用 -PfxPath。",
            "CI 侧配置 WINDOWS_CERT_PFX_BASE64 / WINDOWS_CERT_PFX_PASSWORD，或改用云签名服务。",
            "详见 packaging/win/README.md 的「代码签名」一节。"
        )
        $guidance | ForEach-Object { Write-Host $_ }
        exit 2
    }

    $signtool = Resolve-SignTool
    $signed = New-Object System.Collections.ArrayList
    foreach ($file in $Files) {
        $signArgs = @("sign", "/fd", "SHA256")
        if (-not $NoTimestamp) {
            $signArgs += @("/tr", $TimestampUrl, "/td", "SHA256")
        }
        if ($usePfx) {
            $signArgs += @("/f", $pfx, "/p", $PfxPassword)
        } else {
            $signArgs += @("/sha1", $Thumbprint)
            if ($MachineStore) {
                $signArgs += "/sm"
            }
        }
        $signArgs += $file

        Write-Host "签名：$file"
        $signOutput = & $signtool @signArgs 2>&1
        if ($LASTEXITCODE -ne 0) {
            throw "签名失败（exit=$LASTEXITCODE）：$file`n$signOutput"
        }

        $verifyOutput = & $signtool verify /pa /v $file 2>&1
        if ($LASTEXITCODE -ne 0) {
            throw "签名校验失败（exit=$LASTEXITCODE）：$file`n$verifyOutput"
        }

        $auth = Get-AuthenticodeSignature -FilePath $file
        if (-not $auth.SignerCertificate) {
            throw "签名后未读到证书：$file"
        }
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash
        [void]$signed.Add([pscustomobject]@{
            Path = $file
            Sha256 = $hash
            Subject = if ($auth.SignerCertificate) { $auth.SignerCertificate.Subject } else { "" }
            Thumbprint = if ($auth.SignerCertificate) { $auth.SignerCertificate.Thumbprint } else { "" }
        })
        Write-Host "  通过：$([System.IO.Path]::GetFileName($file))"
    }

    New-Item -ItemType Directory -Force (Split-Path $SigningNote -Parent) | Out-Null
    $lines = New-Object System.Collections.ArrayList
    [void]$lines.Add("已签名：$($signed.Count) 个文件；SHA256 + RFC3161 时间戳（$(if ($NoTimestamp) { '未加时间戳' } else { $TimestampUrl })）。")
    [void]$lines.Add("证书：$($signed[0].Subject)")
    [void]$lines.Add("指纹：$($signed[0].Thumbprint)")
    [void]$lines.Add("文件：")
    foreach ($item in $signed) {
        [void]$lines.Add("  $($item.Path)")
        [void]$lines.Add("    SHA256 $($item.Sha256)")
    }
    $lines | Out-File -FilePath $SigningNote -Encoding utf8

    Write-Host ""
    Write-Host "已完成：$($signed.Count) 个文件签名并通过 verify /pa。"
    Write-Host "说明文件：$SigningNote"
    if (-not ($signed | Where-Object { $_.Path -match '-setup\.exe$' })) {
        Write-Host "下一步：运行 ISCC 打包，再用同一证书运行本脚本签安装包。"
    }
    exit 0
} catch {
    $reason = $_.Exception.Message
    Write-FailureNote $reason
    Write-Host "签名失败：$reason" -ForegroundColor Red
    exit 1
}
