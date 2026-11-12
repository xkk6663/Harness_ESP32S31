<#
固件构建入口（版本管理第 1/2 层，见 docs/项目规划/固件版本管理.md）

  pwsh -f tools/build_fw.ps1                 # 日常构建：2.1.0+g<sha>[.dirty]
  pwsh -f tools/build_fw.ps1 -NoMeta         # 发布构建：严格等于 version.txt，且要求工作区干净
  pwsh -f tools/build_fw.ps1 -Flash          # 构建完顺带 app-flash（日常调试用）

版本号本身由 CMakeLists.txt 在 project() 之前算好（读 version.txt + git rev-parse），
所以 `idf.py build` 直接跑也会带 +g<sha>。本脚本只是额外做三件事：
  1) 必要时初始化 IDF 环境；
  2) -NoMeta 时置 RELEASE_BUILD=1 并拒绝脏工作区（避免把不可复现的包发出去）；
  3) 构建后跑一次体积门禁。
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
$dirty = ((& git status --porcelain | Out-String).Trim().Length -gt 0)

if ($NoMeta -and $dirty) {
    throw '工作区有未提交改动 —— 拒绝构建"发布版本"（先提交，或去掉 -NoMeta 做日常构建）'
}

if ($NoMeta) {
    $env:RELEASE_BUILD = '1'
    Write-Host "[build] 发布构建：PROJECT_VER = $ver（无 build metadata）" -ForegroundColor Green
} else {
    Remove-Item Env:RELEASE_BUILD -ErrorAction SilentlyContinue
    $appVer = if ($dirty) { "$ver+g$sha.dirty" } else { "$ver+g$sha" }
    Write-Host "[build] 日常构建：PROJECT_VER = $appVer" -ForegroundColor Cyan
}

try {
    & idf.py build
    if ($LASTEXITCODE -ne 0) { throw "idf.py build 失败（exit $LASTEXITCODE）" }

    & python (Join-Path $root 'tools\check_fw_size.py') (Join-Path $root 'build\display_audio_photo.bin')
    Write-Host "[build] 体积门禁 exit=$LASTEXITCODE" -ForegroundColor Yellow

    if ($Flash) {
        & idf.py -p COM9 -b 921600 app-flash
        if ($LASTEXITCODE -ne 0) { throw "app-flash 失败（exit $LASTEXITCODE）" }
    }
} finally {
    Remove-Item Env:RELEASE_BUILD -ErrorAction SilentlyContinue
}
