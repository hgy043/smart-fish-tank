# ============================================================================
# build.ps1 — 智能鱼缸项目构建脚本
#
# 作用：加载 ESP-IDF v5.5.3 环境（含 PowerShell 执行策略绕过），然后执行 idf.py。
# 用法：
#   .\build.ps1                 # 增量构建
#   .\build.ps1 reconfigure     # 重新生成配置
#   .\build.ps1 fullclean build # 全清后重建
#   .\build.ps1 -p COM3 flash monitor
#
# 说明：本机 ESP-IDF 安装在 C:\Espressif，装有三个版本，本项目锁定 v5.5.3
#       （与已验证的参考工程一致）。export.ps1 生成的激活脚本未签名，
#       故需在当前进程内将执行策略设为 Bypass。
# ============================================================================
[CmdletBinding()]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$IdfArgs = @('build')
)

Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
$env:IDF_TOOLS_PATH = 'C:\Espressif'

# export.ps1 需要 python 在 PATH 上，先挂载 IDF 自带的 Python 环境
$env:PATH = 'C:\Espressif\python_env\idf5.5_py3.11_env\Scripts;C:\Espressif\tools\idf-python\3.11.2;' + $env:PATH

# 注意：export.ps1 会向 stderr 输出大量进度信息（"Activating ESP-IDF..."、
# "Checking python dependencies..." 等）。在 $ErrorActionPreference='Stop' 下，
# PowerShell 会把原生命令的 stderr 当作终止性错误而中断，因此必须在其间用
# 'Continue'，之后再恢复。
$ErrorActionPreference = 'Continue'
& 'C:\Espressif\frameworks\esp-idf-v5.5.3\export.ps1' *> $null

if ($env:IDF_PATH -ne 'C:\Espressif\frameworks\esp-idf-v5.5.3') {
    throw "ESP-IDF 环境加载失败，IDF_PATH=$env:IDF_PATH"
}

Write-Host "[build.ps1] IDF_PATH = $env:IDF_PATH" -ForegroundColor Cyan

# 切到本脚本所在目录（工程根）
Push-Location -Path $PSScriptRoot
try {
    $ErrorActionPreference = 'Continue'   # 保留 idf.py 的彩色输出与警告
    idf.py @IdfArgs
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}

exit $code
