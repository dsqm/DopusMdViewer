# gen-release-notes.ps1 - 生成 GitHub Release 说明（下载说明 + 更新记录）。
#
# 更新记录按 tag 切段：取 HEAD 上最近的一个 tag 到本次发布之间的提交，按
# Conventional Commits 前缀分组。仓库还没有任何 tag 时，列出全部提交。
#
# 用法：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\gen-release-notes.ps1 `
#       -Version 2026.11.22.1330 -Repo dsqm/DopusMdViewer -OutFile release-notes.md

param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$Repo = '',
    [Parameter(Mandatory = $true)][string]$OutFile
)

$ErrorActionPreference = 'Stop'

# Windows PowerShell 5.1 默认按控制台代码页解码原生命令输出，中文提交信息会变乱码。
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8

$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    $prev = ''
    $described = & git describe --tags --abbrev=0 HEAD 2>$null
    if ($LASTEXITCODE -eq 0 -and $described) {
        $prev = $described.Trim()
    }
    if ($prev -eq $Version) {
        $prev = ''
    }

    $range = if ($prev) { "$prev..HEAD" } else { 'HEAD' }
    $subjects = @(& git log --no-merges --pretty=format:%s $range)
    if ($LASTEXITCODE -ne 0) {
        throw "git log 失败：$range"
    }

    $order = @('feat', 'fix', 'perf', 'refactor', 'docs', 'style', 'build', 'ci', 'chore', 'test', 'other')
    $titles = @{
        feat = '新功能'; fix = '修复'; perf = '性能'; refactor = '重构'; docs = '文档'
        style = '样式'; build = '构建'; ci = 'CI'; chore = '杂项'; test = '测试'; other = '其他'
    }

    $buckets = @{}
    foreach ($subject in $subjects) {
        $line = "$subject".Trim()
        if (-not $line) { continue }

        $m = [regex]::Match($line, '^(?<type>[A-Za-z]+)(\([^)]*\))?!?:\s*(?<text>.+)$')
        if ($m.Success) {
            $type = $m.Groups['type'].Value.ToLowerInvariant()
            $text = $m.Groups['text'].Value
        } else {
            $type = 'other'
            $text = $line
        }
        if (-not $titles.ContainsKey($type)) { $type = 'other' }

        if (-not $buckets.ContainsKey($type)) {
            $buckets[$type] = New-Object System.Collections.Generic.List[string]
        }
        $buckets[$type].Add($text)
    }

    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine('## 下载')
    [void]$sb.AppendLine('')
    [void]$sb.AppendLine('| 包 | 内容 |')
    [void]$sb.AppendLine('| --- | --- |')
    [void]$sb.AppendLine("| ``DopusMdViewer-$Version.zip`` | 插件本体：``DopusMdViewer.dll`` + ``DopusMdViewer\`` 资源目录，装进 DOpus 的 ``Viewers`` |")
    [void]$sb.AppendLine("| ``skinedit-$Version.zip`` | 皮肤工具：``skinedit.exe`` + ``assets\``，独立运行，不装进 DOpus |")
    [void]$sb.AppendLine('')
    [void]$sb.AppendLine('## 安装')
    [void]$sb.AppendLine('')
    [void]$sb.AppendLine('- **插件本体**：解压后把 `DopusMdViewer.dll` 与 `DopusMdViewer\` 一起复制到 DOpus 的 `Viewers` 目录，重启 DOpus。')
    [void]$sb.AppendLine('- **皮肤工具**：解压到任意目录，双击 `skinedit.exe`；需要系统已装 WebView2 Runtime。')
    [void]$sb.AppendLine('')
    [void]$sb.AppendLine('## 更新记录')
    [void]$sb.AppendLine('')
    if ($prev) {
        [void]$sb.AppendLine("自 ``$prev`` 以来的提交：")
    } else {
        [void]$sb.AppendLine('仓库首个发布，包含全部提交：')
    }
    [void]$sb.AppendLine('')

    $any = $false
    foreach ($type in $order) {
        if (-not $buckets.ContainsKey($type)) { continue }
        $any = $true
        [void]$sb.AppendLine("### $($titles[$type])")
        foreach ($text in $buckets[$type]) {
            [void]$sb.AppendLine("- $text")
        }
        [void]$sb.AppendLine('')
    }
    if (-not $any) {
        [void]$sb.AppendLine('本次发布没有新的提交。')
        [void]$sb.AppendLine('')
    }

    if ($prev -and $Repo) {
        [void]$sb.AppendLine("完整对比：https://github.com/$Repo/compare/$prev...$Version")
    }

    $notes = $sb.ToString()
    [System.IO.File]::WriteAllText((Join-Path $root $OutFile), $notes,
                                   (New-Object System.Text.UTF8Encoding($false)))
    Write-Output $notes
} finally {
    Pop-Location
}
