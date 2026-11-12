<#
固件构建入口（版本管理第 1/2 层，见 docs/项目规划/固件版本管理.md）

  pwsh -f tools/build_fw.ps1                 # 日常构建：2.1.0+g<sha>[.dirty]
  pwsh -f tools/build_fw.ps1 -NoMeta         # 发布构建：严格等于 version.txt，且要求工作区干净
  pwsh -f tools/build_fw.ps1 -Flash          # 构建完顺带 app-flash（日常调试用）

规则：
  * 版本号的"人工/流程决定"部分只写在仓库根的 version.txt（ESP-IDF 会自动当 PROJECT_VER）。
  * 日常构建把 commit 作为 SemVer 的 build metadata 拼在后面（+gXXXXXXX），
    设备/PC 的 ver_parse 都会在 '+' 处截断，所以它不影响"要不要升"的判断，
    但日志里一眼能看出是哪个 commit 编出来的。
  * 工作区脏时日常构建带 .dirty 后缀；发布构建（-NoMeta）则直接拒绝，避免把不可复现的包发出去。
#>
param(
    [switch]$NoMeta,
    [switch]$Flash
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    $prof  = 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
    $local = Join-Path $root 'tools\idf_env.local.ps1'
    if (Test-Path -LiteralPath $prof)  { . $prof }
    if (Test-Path -LiteralPath $local) { . $local }
    if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
        throw '找不到 idf.py：请先在本终端执行 IDF 环境初始化（见 docs / skill 说明）'
    }
}

$verFile = Join-Path $root 'version.txt'
if (-not (Test-Path -LiteralPath $verFile)) { throw "缺少 version.txt（内容应为形如 2.1.0 的一行）" }
$ver = (Get-Content -LiteralPath $verFile -Raw).Trim()
if ($ver -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') {
    throw "version.txt 必须是 x.y.z 三段数字，当前是：'$ver'"
}

$sha = (& git rev-parse --short HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or -not $sha) { throw 'git rev-parse --short HEAD 失败（不是 git 仓库？）' }
$dirty = ((& git status --porcelain | Out-String).Trim().Length -gt 0)

if ($NoMeta -and $dirty) {
    throw '工作区有未提交改动 —— 拒绝构建"发布版本"（先提交，或去掉 -NoMeta 做日常构建）'
}

if ($NoMeta) {
    $appVer = $ver
} elseif ($dirty) {
    $appVer = "$ver+g$sha.dirty"
} else {
    $appVer = "$ver+g$sha"
}

Write-Host "[build] version.txt = $ver" -ForegroundColor Cyan
Write-Host "[build] commit      = $sha$(if ($dirty) { ' (dirty)' })" -ForegroundColor Cyan
Write-Host "[build] PROJECT_VER = $appVer" -ForegroundColor Green

& idf.py -DPROJECT_VER="$appVer" build
if ($LASTEXITCODE -ne 0) { throw "idf.py build 失败（exit $LASTEXITCODE）" }

& python (Join-Path $root 'tools\check_fw_size.py') (Join-Path $root 'build\display_audio_photo.bin')
Write-Host "[build] 体积门禁 exit=$LASTEXITCODE" -ForegroundColor Yellow

if ($Flash) {
    & idf.py -p COM9 -b 921600 app-flash
    if ($LASTEXITCODE -ne 0) { throw "app-flash 失败（exit $LASTEXITCODE）" }
}
