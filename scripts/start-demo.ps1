param(
    # 待启动或验证的工作台程序目录。
    [string]$ProgramDirectory='',
    # 独立模拟器的可执行文件目录。
    [string]$SimulatorProgramDirectory='',
    # 本次运行保存数据库、日志和工位状态的根目录。
    [string]$DataDirectory='',
    # 本次启动或连接的工位数量。
    [ValidateRange(1,4)][int]$Stations=4,
    # 首个工位使用的 TCP 端口。
    [ValidateRange(1024,65532)][int]$BasePort=46501,
    # 演示中注入测试失败的工位编号。
    [ValidateRange(0,4)][int]$FailStation=3,
    # 部署验证时让程序自动退出的等待毫秒数。
    [ValidateRange(0,60000)][int]$SmokeExitMs=0,
    # 是否用独立后台进程启动各模拟工位。
    [switch]$HeadlessSimulators,
    # Qt 运行库和部署工具所在目录。
    [string]$QtPrefix='D:/Qt6/6.10.3/mingw_64',
    # MinGW 编译器及运行库所在目录。
    [string]$CompilerBin='D:/Qt6/Tools/mingw1310_64/bin'
)
# 脚本遇到错误时立即停止，防止继续执行后续步骤。
$ErrorActionPreference='Stop'
# 由脚本位置推导出的项目根目录。
$projectRoot=Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'data-paths.ps1')
if(!$ProgramDirectory) {
    if(Test-Path -LiteralPath (Join-Path $projectRoot 'StationTestWorkbench.exe')) {$ProgramDirectory=$projectRoot}
    else {$ProgramDirectory=Join-Path $projectRoot 'build/quality'}
}
# 默认路径与主程序一致；显式演示目录继续保持根目录和 client 子目录的布局。
$dataPaths=Get-WorkbenchDataPaths -DataDirectory $DataDirectory -JointDemo
$DataDirectory=$dataPaths.Simulator
if(!$SimulatorProgramDirectory) {
    # 同级独立模拟器项目的根目录。
    $simulatorRoot=Join-Path (Split-Path $projectRoot -Parent) 'StationSimulator'
    if(Test-Path -LiteralPath (Join-Path $simulatorRoot 'StationSimulator.exe')) {$SimulatorProgramDirectory=$simulatorRoot}
    else {$SimulatorProgramDirectory=Join-Path $simulatorRoot 'build/quality'}
}
# 待启动的工作台程序路径或进程对象。
$client=Join-Path $ProgramDirectory 'StationTestWorkbench.exe'
# 待启动的独立模拟器程序路径。
$simulator=Join-Path $SimulatorProgramDirectory 'StationSimulator.exe'
if(!(Test-Path -LiteralPath $client) -or !(Test-Path -LiteralPath $simulator)) {throw 'Build both independent projects, or specify -ProgramDirectory and -SimulatorProgramDirectory.'}
[void][IO.Directory]::CreateDirectory($DataDirectory)
# 本次演示启动并负责退出清理的模拟器进程。
$ownedProcesses=[Collections.Generic.List[Diagnostics.Process]]::new()
# 脚本结束时需要恢复的原始 PATH。
$oldPath=$env:PATH
try {
    # 本次启动或构建所需的程序及 Qt 运行库搜索路径。
    $env:PATH="$ProgramDirectory;$SimulatorProgramDirectory;$CompilerBin;$QtPrefix/bin;$oldPath"
    if($HeadlessSimulators -or $SmokeExitMs -gt 0) {
        # Automated deployment checks retain four independent, non-graphical station processes.
        for($station=1;$station -le $Stations;$station++) {
            # 当前模拟工位保存持久状态的独立目录。
            $stationDirectory=Join-Path $DataDirectory "station-$station"
            # 传给目标工具或程序的启动参数。
            $arguments=@('--headless','--port',"$($BasePort+$station-1)",'--station-id',"$station",'--data-dir',('"{0}"' -f $stationDirectory),'--step-ms','400')
            if($station -eq $FailStation) {$arguments+=@('--fail-step','current')}
            if($station -eq 4) {$arguments+='--drop-first-result-ack'}
            # 本次脚本创建或正在检查的程序进程。
            $process=Start-Process -FilePath $simulator -ArgumentList $arguments -PassThru -WindowStyle Hidden
            $ownedProcesses.Add($process)
        }
    } else {
        # One operator-facing panel manages all stations; servers run in its dedicated worker thread.
        $arguments=@('--auto-start','--stations',"$Stations",'--port',"$BasePort",'--data-dir',('"{0}"' -f $DataDirectory),'--step-ms','400','--fail-station',"$FailStation",'--demo-faults')
        $process=Start-Process -FilePath $simulator -ArgumentList $arguments -PassThru -WindowStyle Normal
        $ownedProcesses.Add($process)
    }
    Start-Sleep -Milliseconds 400
    foreach($process in $ownedProcesses) {if($process.HasExited) {throw 'A simulator failed to start; check port conflicts and simulator state.'}}
    $arguments=@('--base-port',"$BasePort",'--stations',"$Stations",'--data-dir',('"{0}"' -f $dataPaths.Client))
    # 根据交互运行或自动验证选择的窗口显示方式。
    $windowStyle='Normal'
    if($SmokeExitMs -gt 0) {$arguments+=@('--smoke-exit-ms',"$SmokeExitMs");$windowStyle='Hidden'}
    # 本次联合演示启动的工作台进程。
    $window=Start-Process -FilePath $client -ArgumentList $arguments -PassThru -WindowStyle $windowStyle
    try {
        if($SmokeExitMs -gt 0) {
            if(!$window.WaitForExit($SmokeExitMs+10000)) {$window.Kill();$window.WaitForExit();throw 'Joint demo startup check timed out.'}
        } else {$window.WaitForExit()}
        if($window.ExitCode -ne 0) {throw "Client exited with code $($window.ExitCode)."}
    } finally {$window.Dispose()}
} finally {
    # Stop only the simulators owned by this launch. Let graphical workers close normally first.
    foreach($process in $ownedProcesses) {
        if(!$process.HasExited) {
            # 请求模拟器窗口正常关闭是否已被接受。
            $closing=$process.CloseMainWindow()
            if(!$closing -or !$process.WaitForExit(3000)) {$process.Kill();$process.WaitForExit()}
        }
        $process.Dispose()
    }
    $env:PATH=$oldPath
}
