param(
    # 待启动或验证的工作台程序目录。
    [string]$ProgramDirectory='',
    # 首个工位使用的 TCP 端口。
    [ValidateRange(1024,65532)][int]$BasePort=49521
)
# 脚本遇到错误时立即停止，防止继续执行后续步骤。
$ErrorActionPreference='Stop'
# 由脚本位置推导出的项目根目录。
$projectRoot=Split-Path $PSScriptRoot -Parent
if(!$ProgramDirectory) {
    # 用于提取统一发布版本的 CMake 配置原文。
    $projectText=[IO.File]::ReadAllText((Join-Path $projectRoot 'CMakeLists.txt'))
    # 从 CMake 配置提取版本号的匹配结果。
    $versionMatch=[regex]::Match($projectText,'project\(StationTestWorkbench VERSION (\d+\.\d+\.\d+)')
    if(!$versionMatch.Success) {throw 'Cannot read the project version.'}
    $ProgramDirectory=Join-Path $projectRoot ('dist/StationTestWorkbench-'+$versionMatch.Groups[1].Value+'-win64')
}
# 待验证便携包中的发布清单路径。
$manifestPath=Join-Path $ProgramDirectory 'release.json'
if(!(Test-Path -LiteralPath $manifestPath)) {throw 'Missing release manifest.'}
# 记录发布版本、源码提交及主程序哈希的清单对象。
$manifest=Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
# 用于核对文件版本和哈希的主程序文件对象。
$exe=Get-Item -LiteralPath (Join-Path $ProgramDirectory 'StationTestWorkbench.exe')
if((Get-FileHash -LiteralPath $exe.FullName).Hash -cne $manifest.executableSha256 -or $exe.VersionInfo.ProductVersion -cne $manifest.version) {throw 'Release version or executable hash mismatch.'}
# 启动验证独享的临时数据根目录。
$data=Join-Path $projectRoot ('build/portable-smoke-'+[Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($data)
# 验证结束时需要清理的自有进程集合。
$owned=[Collections.Generic.List[Diagnostics.Process]]::new()
# 隔离启动 FileName 指定的程序，并传入 Arguments 参数。
function Start-Isolated([string]$FileName,[string]$Arguments) {
    # 配置程序路径、参数和隔离环境的进程启动信息。
    $info=[Diagnostics.ProcessStartInfo]::new()
    $info.FileName=Join-Path $ProgramDirectory $FileName
    $info.Arguments=$Arguments
    $info.WorkingDirectory=$ProgramDirectory
    $info.UseShellExecute=$false
    $info.CreateNoWindow=$true
    $info.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
    $info.EnvironmentVariables['PATH']="$ProgramDirectory;$env:WINDIR\System32;$env:WINDIR"
    $info.EnvironmentVariables['QT_PLUGIN_PATH']=''
    $info.EnvironmentVariables['QT_QPA_PLATFORM_PLUGIN_PATH']=''
    $info.EnvironmentVariables['QT_QPA_PLATFORM']='windows'
    # 本次脚本创建或正在检查的程序进程。
    $process=[Diagnostics.Process]::Start($info)
    return $process
}
try {
    # 启动验证使用的工作台临时数据目录。
    $clientPath=Join-Path $data 'client'
    # 传给目标工具或程序的启动参数。
    $arguments='--base-port {0} --stations 4 --data-dir "{1}" --smoke-exit-ms 2000' -f $BasePort,$clientPath
    # 隔离启动后需要检查退出码的工作台进程。
    $client=Start-Isolated 'StationTestWorkbench.exe' $arguments
    try {
        if(!$client.WaitForExit(15000)) {$client.Kill();throw 'Packaged client did not exit normally.'}
        if($client.ExitCode -ne 0) {throw "Packaged client exit code: $($client.ExitCode)"}
    } finally {$client.Dispose()}
    # 用于核对工作台初始化结果的日志文件路径。
    $log=Join-Path $clientPath 'logs/workbench.log'
    # 用于核对 SQLite 初始化结果的数据库路径。
    $database=Join-Path $clientPath 'data/workbench.db'
    if(!(Test-Path -LiteralPath $database) -or !(Test-Path -LiteralPath $log)) {throw 'Packaged database/log initialization failed.'}
    # 用于确认数据库已就绪的启动日志内容。
    $logText=[IO.File]::ReadAllText($log,[Text.Encoding]::UTF8)
    if(!$logText.Contains('数据库就绪')) {throw 'No successful Qt SQLite initialization evidence in packaged log.'}
    # 汇总便携启动、版本和数据初始化检查结果。
    $report=[ordered]@{clientExitCode=0;simulatorProcesses=0;databaseCreated=$true;sqliteReady=$true;qtOnPath=$false;version=$manifest.version;revision=$manifest.revision;package=$ProgramDirectory;evidenceDirectory=$data}
    $report | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $projectRoot 'build/portable-smoke-result.json') -Encoding UTF8
    $report | ConvertTo-Json
} finally {
    foreach($process in $owned) {if(!$process.HasExited){$process.Kill();$process.WaitForExit()};$process.Dispose()}
}
