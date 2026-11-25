param(
    # 性能测量使用的历史记录总数。
    [ValidateRange(1000,1000000)][int]$Rows=100000,
    # Qt 运行库和部署工具所在目录。
    [string]$QtPrefix='D:/Qt6/6.10.3/mingw_64',
    # MinGW 编译器及运行库所在目录。
    [string]$CompilerBin='D:/Qt6/Tools/mingw1310_64/bin'
)
# 脚本遇到错误时立即停止，防止继续执行后续步骤。
$ErrorActionPreference='Stop'
# 由脚本位置推导出的项目根目录。
$projectRoot=Split-Path $PSScriptRoot -Parent
# 历史查询性能测量程序的完整路径。
$executable=Join-Path $projectRoot 'build/quality/HistoryBenchmark.exe'
if(!(Test-Path -LiteralPath $executable)) {throw 'Run scripts/verify.ps1 to build the benchmark executable first.'}
# 脚本结束时需要恢复的原始 PATH。
$oldPath=$env:PATH
try {
    # 本次启动或构建所需的程序及 Qt 运行库搜索路径。
    $env:PATH="$CompilerBin;$QtPrefix/bin;$oldPath"
    # 保存历史查询性能测量结果的 JSON 路径。
    $output=Join-Path $projectRoot "build/history-benchmark-$Rows.json"
    & $executable --rows "$Rows" --output $output
    if($LASTEXITCODE -ne 0){throw "History benchmark failed: $LASTEXITCODE"}
    Get-Content -LiteralPath $output -Encoding UTF8
} finally {$env:PATH=$oldPath}
