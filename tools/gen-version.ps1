# gen-version.ps1 - 生成 build\obj\version.rc.inc（构建时调用，产物不入库）。
#
# 版本号默认取构建时刻：年.月.日.时分，四段正好填满 VERSIONINFO 的四个数字字段。
# 数字字段不带前导零（rc 会把 08、09 当八进制而报错），字符串里月日与时分补零，
# 和资源管理器属性页显示的一致。
#
# CI 发布时用环境变量 MDV_VERSION 固定一次：插件与皮肤工具是两次独立构建，
# 跨过一分钟就会各算各的版本号，发布包与 tag 就对不上了。
#
# 只写 build\ 下的产物；仓库里没有任何文件记着版本号，每次构建重新算。

$ErrorActionPreference = 'Stop'

$proj = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $proj 'build\obj'
New-Item -ItemType Directory -Force -Path $dir | Out-Null

$ver = $env:MDV_VERSION
if (-not $ver -or $ver -notmatch '^(\d{4})\.(\d{1,2})\.(\d{1,2})\.(\d{1,4})$') {
    $now = Get-Date
    $hhmm = $now.Hour * 100 + $now.Minute
    $ver = '{0}.{1:D2}.{2:D2}.{3:D4}' -f $now.Year, $now.Month, $now.Day, $hhmm
}

$m = [regex]::Match($ver, '^(\d{4})\.(\d{1,2})\.(\d{1,2})\.(\d{1,4})$')
$y = [int]$m.Groups[1].Value
$mo = [int]$m.Groups[2].Value
$d = [int]$m.Groups[3].Value
$rev = [int]$m.Groups[4].Value

$text = @"
#define MDV_VER_MAJOR $y
#define MDV_VER_MINOR $mo
#define MDV_VER_BUILD $d
#define MDV_VER_REVISION $rev
#define MDV_VER_NUMERIC MDV_VER_MAJOR,MDV_VER_MINOR,MDV_VER_BUILD,MDV_VER_REVISION
#define MDV_VER_STRING "$ver"
"@

Set-Content -Path (Join-Path $dir 'version.rc.inc') -Value $text -Encoding ascii
Write-Output ('version ' + $ver)
