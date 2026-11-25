param(
    # 待启动或验证的工作台程序目录。
    [string]$ProgramDirectory='',
    # 本次运行保存数据库、日志和工位状态的根目录。
    [string]$DataDirectory='',
    # 工作台连接的工位主机地址。
    [string]$HostAddress='127.0.0.1',
    # 本次启动或连接的工位数量。
    [ValidateRange(1,4)][int]$Stations=4,
    # 首个工位使用的 TCP 端口。
    [ValidateRange(1024,65532)][int]$BasePort=46501,
    # 部署验证时让程序自动退出的等待毫秒数。
    [ValidateRange(0,60000)][int]$SmokeExitMs=0,
    # Qt 运行库和部署工具所在目录。
    [string]$QtPrefix='D:/Qt6/6.10.3/mingw_64',
    # MinGW 编译器及运行库所在目录。
    [string]$CompilerBin='D:/Qt6/Tools/mingw1310_64/bin'
)
# 脚本遇到错误时立即停止，防止继续执行后续步骤。
$ErrorActionPreference='Stop'
# 由脚本位置推导出的项目根目录。
$projectRoot=Split-Path $PSScriptRoot -Parent
if(!$ProgramDirectory) {
    if(Test-Path -LiteralPath (Join-Path $projectRoot 'StationTestWorkbench.exe')) {$ProgramDirectory=$projectRoot}
    else {$ProgramDirectory=Join-Path $projectRoot 'build/quality'}
}
$ProgramDirectory=(Resolve-Path -LiteralPath $ProgramDirectory).Path
if(!$DataDirectory) {$DataDirectory=Join-Path $projectRoot 'demo-data/client'}
# 待启动的工作台程序路径或进程对象。
$client=Join-Path $ProgramDirectory 'StationTestWorkbench.exe'
if(!(Test-Path -LiteralPath $client)) {throw 'Build the project or use the portable package first.'}
# 传给目标工具或程序的启动参数。
$arguments=@('--host',$HostAddress,'--base-port',"$BasePort",'--stations',"$Stations",'--data-dir',('"{0}"' -f $DataDirectory))
# 根据交互运行或自动验证选择的窗口显示方式。
$windowStyle='Normal'
if($SmokeExitMs -gt 0) {$arguments+=@('--smoke-exit-ms',"$SmokeExitMs");$windowStyle='Hidden'}
# 脚本结束时需要恢复的原始 PATH。
$oldPath=$env:PATH
try {
    # 本次启动或构建所需的程序及 Qt 运行库搜索路径。
    $env:PATH="$ProgramDirectory;$CompilerBin;$QtPrefix/bin;$oldPath"
    # Launch only the client. Its lifetime is independent of every simulator.
    # 本次脚本创建或正在检查的程序进程。
    $process=Start-Process -FilePath $client -ArgumentList $arguments -WorkingDirectory $ProgramDirectory -PassThru -WindowStyle $windowStyle
    try {
        if($SmokeExitMs -gt 0) {
            if(!$process.WaitForExit($SmokeExitMs+10000)) {
                $process.Kill()
                $process.WaitForExit()
                throw 'Client startup check did not exit normally.'
            }
            if($process.ExitCode -ne 0) {throw "Client startup check failed: $($process.ExitCode)"}
            Write-Output 'Standalone client exit code: 0'
        }
    } finally {$process.Dispose()}
} finally {$env:PATH=$oldPath}
