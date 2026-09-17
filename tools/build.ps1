# =============================================================================
#  build.ps1 —— 闭环构建（加载 IDF 环境 + 注入本地密钥）
#
#  用法：
#     powershell -ExecutionPolicy Bypass -File tools\build.ps1
#     powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Clean
#     powershell -ExecutionPolicy Bypass -File tools\build.ps1 -UseCcache
#
#  说明：
#    - 自动加载 tools/idf_env.local.ps1 里的和风 API Key / API Host（以环境变量导出，
#      再由 main/CMakeLists.txt 转成编译宏，Key 不落盘到任何被跟踪的文件）；
#    - **默认先 reconfigure**：CMake 的 $ENV{} 只在 configure 阶段求值，
#      改了密钥/host 若不重配会静默沿用旧值（见 docs/工程规范/11_遇到的问题.md F18）；
#    - 构建后**自动校验**固件里确实含有当前 Key/Host，防止再次静默用旧值；
#    - 默认关闭 ccache（IDF v6.1 + 本机曾出现 "CreateProcess failed" 噪声，
#      见 F15），加 -UseCcache 可开启；
#    - 构建产物：build/display_audio_photo.bin
# =============================================================================
param(
    [switch]$Clean,
    [switch]$UseCcache,
    [switch]$NoReconfigure
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'idf_common.ps1')
Write-Host "=== IDF closed-loop BUILD ===" -ForegroundColor Green
Initialize-BuildEnv

# ccache 默认关闭（F15）；显式 -UseCcache 才开
if ($UseCcache) {
    $env:IDF_CCACHE_ENABLE = '1'
} else {
    $env:IDF_CCACHE_ENABLE = '0'
    Write-Host "[build] ccache disabled (use -UseCcache to enable)" -ForegroundColor DarkGray
}

Set-Location $global:ProjectRoot

if ($Clean) {
    Write-Host "[build] idf.py fullclean" -ForegroundColor Cyan
    $c0 = Invoke-Build fullclean
    if ($c0 -ne 0) { Write-Host "FULLCLEAN FAILED" -ForegroundColor Red; exit $c0 }
}

# ⚠ 关键：CMake 的 $ENV{} 只在 configure 阶段求值一次。
#   若只改 tools/idf_env.local.ps1 里的 host/key 而直接增量 build，
#   build.ninja 会沿用旧值（曾导致"配置改了但固件里还是旧域名"，见 F18）。
#   因此默认强制 reconfigure，用 -NoReconfigure 跳过。
#
# ⚠ 又：必须走 Invoke-Build（内部把原生 stderr 并流），
#   否则 PS 5.1 + zh-CN 下 cmake 往 stderr 写的 Deprecation Warning
#   会被判成 NativeCommandError 直接掐断脚本（见 F21）。
if (-not $NoReconfigure) {
    Write-Host "[build] reconfigure (refresh env-injected macros)" -ForegroundColor Cyan
    $c1 = Invoke-Build reconfigure
    if ($c1 -ne 0) { Write-Host "RECONFIGURE FAILED (exit $c1)" -ForegroundColor Red; exit $c1 }
}

Write-Host "[build] idf.py build" -ForegroundColor Cyan
$code = Invoke-Build build

if ($code -eq 0) {
    Write-Host "=== BUILD OK ===" -ForegroundColor Green
    $bin = Join-Path $global:ProjectRoot 'build\display_audio_photo.bin'
    if (Test-Path $bin) {
        $mb = [math]::Round((Get-Item $bin).Length / 1MB, 2)
        Write-Host "[build] artifact: $bin ($mb MB)" -ForegroundColor Green

        # 校验注入的符号确实进了固件（防止静默沿用旧值）
        Write-Host "[build] verifying injected symbols in firmware..." -ForegroundColor Cyan
        $ascii = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($bin))
        $ok = $true
        if ($env:QWEATHER_KEY -and $env:QWEATHER_KEY -ne 'YOUR_QWEATHER_KEY') {
            if ($ascii.Contains($env:QWEATHER_KEY)) { Write-Host "  [OK] QWEATHER_KEY present" -ForegroundColor Green }
            else { Write-Host "  [FAIL] QWEATHER_KEY NOT in firmware!" -ForegroundColor Red; $ok = $false }
        }
        if ($env:QWEATHER_API_HOST) {
            if ($ascii.Contains($env:QWEATHER_API_HOST)) { Write-Host "  [OK] QWEATHER_API_HOST=$($env:QWEATHER_API_HOST) present" -ForegroundColor Green }
            else { Write-Host "  [FAIL] QWEATHER_API_HOST=$($env:QWEATHER_API_HOST) NOT in firmware (stale!)" -ForegroundColor Red; $ok = $false }
        }
        if (-not $ok) {
            Write-Host "=== BUILT BUT INJECTION STALE - retry with -Clean ===" -ForegroundColor Red
            exit 3
        }
    }
} else {
    Write-Host "=== BUILD FAILED (exit $code) ===" -ForegroundColor Red
}
exit $code
