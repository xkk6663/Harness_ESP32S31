# =============================================================================
#  idf_common.ps1 —— 构建/烧录公共前置（被 build.ps1 / flash.ps1 点源加载）
#
#  职责：
#    1. 加载 ESP-IDF v6.1 环境（Microsoft.v6.1.PowerShell_profile.ps1）
#    2. 点源加载本地私密配置 tools/idf_env.local.ps1（已被 .gitignore 忽略）
#    3. 把 QWEATHER_KEY 等密钥**只以环境变量形式**导出，
#       由 main/CMakeLists.txt 读取并转成编译期 -D 宏
#
#  安全约定（红线）：
#    - API Key 只存在于 tools/idf_env.local.ps1（untracked）与本次进程的环境变量中，
#      **绝不写进任何会被 git 跟踪的源码 / CMakeLists / sdkconfig**；
#    - 源码 main/middleware/net_time_service.h 保留 #ifndef 占位宏，
#      未注入时行为与"没配 key"完全一致。
# =============================================================================

$ErrorActionPreference = 'Stop'

# =============================================================================
#  ⚠ F21：先把"原生命令 stderr"这颗雷拆掉，再加载 IDF 环境
#
#  背景：PowerShell 5.1 + zh-CN(GBK/936) 终端下，任何**原生命令往 stderr 写东西**
#  （如 cmake 的 "CMake Deprecation Warning"）都会被 PS 包装成
#  `NativeCommandError`。若此时 $ErrorActionPreference='Stop'，
#  该错误会**直接终止脚本**，表现为：
#    - 脚本在中途静默退出，后续语句（含 Out-File 落盘）全不执行；
#    - 若发生在 idf.py 内部，cmake 拿不到完整环境，报
#      `CMake was unable to find a build program corresponding to "Ninja"`
#      ——**这是假象**，实际环境里 ninja 好好地在 PATH 上（见 F20/F21）。
#
#  三件套一起上才彻底：
#    1) 控制台 I/O 统一 UTF-8，消除 GBK 解码噪声（profile 里有中文/UTF-8 输出）；
#    2) PYTHONIOENCODING/PYTHONUTF8 让 profile 里被调起的 python 也走 UTF-8；
#    3) 包装原生命令，把它的 stderr 归到 Warning 流而非 Error 流，
#       这样即使 $ErrorActionPreference='Stop' 也不会被 stderr 掐断。
# =============================================================================
try {
    [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
    [Console]::InputEncoding  = [System.Text.UTF8Encoding]::new($false)
    $OutputEncoding           = [System.Text.UTF8Encoding]::new($false)
} catch { }
$env:PYTHONIOENCODING = 'utf-8'
$env:PYTHONUTF8       = '1'

# 项目根目录（本文件位于 <root>/tools/）
# 注意：用 $global: 而非 $Script: ——本文件被 dot-source 进调用方作用域后，
#       `-File` 方式启动的脚本里 $Script: 会指向调用方脚本，取值时机不稳。
$global:ProjectRoot = Split-Path -Parent $PSScriptRoot

# ---- IDF 环境入口（Espressif Install Manager 生成的标准 profile） ----
$global:IdfProfile = 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'

function Initialize-IdfEnv {
    if (-not (Test-Path $global:IdfProfile)) {
        throw "ESP-IDF profile not found: $($global:IdfProfile)"
    }
    # profile 会设置 IDF_PATH / PATH / Python venv，并定义 idf.py 函数。
    # 这里临时把 EAP 降级，避免 profile 内部（Activate.ps1 / 注册补全时的
    # python 调用）往 stderr 写字节而被 PS 5.1 判为致命错误、把加载打断。
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        . $global:IdfProfile *>&1 | Out-Null
    } finally {
        $ErrorActionPreference = $prevEap
    }
    if (-not $env:IDF_PATH) {
        throw "IDF_PATH not set after sourcing profile"
    }
}

# 原生命令安全包装：先把 stderr 并到 stdout 再执行，避免 PS 5.1 的
# NativeCommandError 把整个脚本掐断。用法：Invoke-Build reconfigure
#
# 注意实现细节（踩过坑）：
#   - 不能写成 `& idf.py @Args 2>&1 | ForEach-Object { $_ }`：
#     管道会让原生命令脱离当前作用域，$LASTEXITCODE 变 -1、输出被截断。
#   - 用 `cmd /c ... 2>&1` 或直接 `2>&1` 重定向、**不接管道**，才稳。
function Invoke-Build {
    param([Parameter(ValueFromRemainingArguments = $true)][object[]]$Args)
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        # 直接调用，不用管道；stderr 用 2>&1 合并，PS 5.1 不会再抛 NativeCommandError
        & idf.py @Args 2>&1
        $rc = $LASTEXITCODE
        if ($null -eq $rc) { $rc = 0 }
        return $rc
    } finally {
        $ErrorActionPreference = $prevEap
    }
}

function Import-LocalSecrets {
    $localEnv = Join-Path $PSScriptRoot 'idf_env.local.ps1'
    if (Test-Path $localEnv) {
        . $localEnv
        Write-Host "[secrets] loaded $localEnv" -ForegroundColor DarkCyan
    } else {
        Write-Host "[secrets] idf_env.local.ps1 not found - weather key stays placeholder" -ForegroundColor Yellow
    }
}

# 密钥以环境变量形式导出，具体转 -D 由 main/CMakeLists.txt 完成
function Show-SecretStatus {
    if ($env:QWEATHER_KEY -and $env:QWEATHER_KEY -ne 'YOUR_QWEATHER_KEY') {
        Write-Host "[secrets] QWEATHER_KEY ready (len=$($env:QWEATHER_KEY.Length)), will be injected by CMake" -ForegroundColor DarkCyan
    } else {
        Write-Host "[secrets] QWEATHER_KEY empty - weather will be disabled" -ForegroundColor Yellow
    }
    if ($env:QWEATHER_API_HOST) { Write-Host "[secrets] QWEATHER_API_HOST=$($env:QWEATHER_API_HOST)" -ForegroundColor DarkCyan }
    if ($env:QWEATHER_LOC_ID)  { Write-Host "[secrets] QWEATHER_LOC_ID=$($env:QWEATHER_LOC_ID)"  -ForegroundColor DarkCyan }
}

# ---- 供调用方使用的完整初始化（IDF 环境 + 本地密钥） ----
function Initialize-BuildEnv {
    Initialize-IdfEnv
    Import-LocalSecrets
    Show-SecretStatus
}
