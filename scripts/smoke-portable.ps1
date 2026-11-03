param(
    [string]$ProgramDirectory='',
    [ValidateRange(1024,65532)][int]$BasePort=49521
)
$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot -Parent
if(!$ProgramDirectory) {$ProgramDirectory=Join-Path $projectRoot 'dist/StationTestWorkbench-0.1.0-win64'}
$data=Join-Path $projectRoot ('build/portable-smoke-'+[Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($data)
$owned=[Collections.Generic.List[Diagnostics.Process]]::new()
function Start-Isolated([string]$FileName,[string]$Arguments) {
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
    $process=[Diagnostics.Process]::Start($info)
    return $process
}
try {
    for($station=1;$station -le 4;$station++) {
        $path=Join-Path $data "station-$station"
        $arguments='--headless --port {0} --station-id {1} --data-dir "{2}"' -f ($BasePort+$station-1),$station,$path
        $owned.Add((Start-Isolated 'StationSimulator.exe' $arguments))
    }
    Start-Sleep -Milliseconds 300
    foreach($process in $owned) {if($process.HasExited) {throw 'Packaged simulator failed to start.'}}
    $clientPath=Join-Path $data 'client'
    $arguments='--base-port {0} --stations 4 --data-dir "{1}" --smoke-exit-ms 2000' -f $BasePort,$clientPath
    $client=Start-Isolated 'StationTestWorkbench.exe' $arguments
    try {
        if(!$client.WaitForExit(15000)) {$client.Kill();throw 'Packaged client did not exit normally.'}
        if($client.ExitCode -ne 0) {throw "Packaged client exit code: $($client.ExitCode)"}
    } finally {$client.Dispose()}
    $log=Join-Path $clientPath 'logs/workbench.log'
    $database=Join-Path $clientPath 'data/workbench.db'
    if(!(Test-Path -LiteralPath $database) -or !(Test-Path -LiteralPath $log)) {throw 'Packaged database/log initialization failed.'}
    $logText=[IO.File]::ReadAllText($log,[Text.Encoding]::UTF8)
    if(!$logText.Contains('数据库就绪')) {throw 'No successful Qt SQLite initialization evidence in packaged log.'}
    $report=[ordered]@{clientExitCode=0;simulatorProcesses=4;databaseCreated=$true;sqliteReady=$true;qtOnPath=$false;package=$ProgramDirectory;evidenceDirectory=$data}
    $report | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $projectRoot 'build/portable-smoke-result.json') -Encoding UTF8
    $report | ConvertTo-Json
} finally {
    foreach($process in $owned) {if(!$process.HasExited){$process.Kill();$process.WaitForExit()};$process.Dispose()}
}
