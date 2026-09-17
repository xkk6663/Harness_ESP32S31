# =============================================================================
#  flash.ps1 —— 闭环烧录（app-flash：只烧应用分区，不动 bootloader/分区表）
#
#  用法：
#     powershell -ExecutionPolicy Bypass -File tools\flash.ps1
#     powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -Port COM9
#     powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -BuildFirst
#     powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -NoCapture
#
#  说明：
#    - 密钥注入方式与 build.ps1 完全一致（环境变量 -> CMake -D）；
#    - 只改了 main/ 下的应用代码时用 app-flash 即可，速度远快于整片 flash；
#    - 烧录成功后自动用 docs/_serial_capture.py 抓串口日志并检查 panic。
# =============================================================================
param(
    [string]$Port = 'COM9',
    [switch]$BuildFirst,
    [int]$CaptureSeconds = 15,
    [switch]$NoCapture
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'idf_common.ps1')

Write-Host "=== IDF closed-loop FLASH ===" -ForegroundColor Green
Initialize-BuildEnv
Set-Location $global:ProjectRoot
$env:IDF_CCACHE_ENABLE = '0'

if ($BuildFirst) {
    Write-Host "[flash] build first..." -ForegroundColor Cyan
    idf.py build
    if ($LASTEXITCODE -ne 0) { Write-Host "BUILD FAILED" -ForegroundColor Red; exit $LASTEXITCODE }
}

Write-Host "[flash] app-flash -p $Port" -ForegroundColor Cyan
idf.py -p $Port app-flash
$code = $LASTEXITCODE
if ($code -ne 0) { Write-Host "=== FLASH FAILED (exit $code) ===" -ForegroundColor Red; exit $code }
Write-Host "=== FLASH OK ===" -ForegroundColor Green

if ($NoCapture) { exit 0 }

# ---- 自动串口抓取（idf.py monitor 在无 TTY 环境下不可用，用 pyserial 脚本） ----
$capture = Join-Path $global:ProjectRoot 'docs\_serial_capture.py'
if (-not (Test-Path $capture)) {
    Write-Host "[flash] docs\_serial_capture.py not found, skip capture" -ForegroundColor Yellow
    exit 0
}

$py = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
$log = Join-Path $global:ProjectRoot ("docs\_log_{0}.txt" -f (Get-Date -Format 'HHmmss'))
Write-Host "[flash] capture ${CaptureSeconds}s serial -> $log" -ForegroundColor Cyan

& $py $capture --port $Port --secs $CaptureSeconds --out $log

if (Test-Path $log) {
    Write-Host "----- last serial log -----" -ForegroundColor DarkGray
    Get-Content $log -Tail 45
    $bad = Select-String -Path $log -Pattern 'Guru Meditation|panic|WDT|abort\(\)|assert failed' -Quiet
    if ($bad) { Write-Host "!!! PANIC/WDT DETECTED in serial log !!!" -ForegroundColor Red }
    else      { Write-Host "[flash] serial log clean (no panic)" -ForegroundColor Green }
}
