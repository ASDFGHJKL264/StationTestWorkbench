param(
    [string]$ProgramDirectory='',
    [string]$DataDirectory='',
    [ValidateRange(1,4)][int]$Stations=4,
    [ValidateRange(1024,65532)][int]$BasePort=46501,
    [ValidateRange(0,4)][int]$FailStation=3,
    [ValidateRange(0,60000)][int]$SmokeExitMs=0,
    [switch]$HeadlessSimulators,
    [string]$QtPrefix='D:/Qt6/6.10.3/mingw_64',
    [string]$CompilerBin='D:/Qt6/Tools/mingw1310_64/bin'
)
$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot -Parent
if(!$ProgramDirectory) {
    if(Test-Path -LiteralPath (Join-Path $projectRoot 'StationTestWorkbench.exe')) {$ProgramDirectory=$projectRoot}
    else {$ProgramDirectory=Join-Path $projectRoot 'build/quality'}
}
if(!$DataDirectory) {$DataDirectory=Join-Path $projectRoot 'demo-data'}
$client=Join-Path $ProgramDirectory 'StationTestWorkbench.exe'
$simulator=Join-Path $ProgramDirectory 'StationSimulator.exe'
if(!(Test-Path -LiteralPath $client) -or !(Test-Path -LiteralPath $simulator)) {throw 'Build the project or use the portable package first.'}
[void][IO.Directory]::CreateDirectory($DataDirectory)
$ownedProcesses=[Collections.Generic.List[Diagnostics.Process]]::new()
$oldPath=$env:PATH
try {
    $env:PATH="$ProgramDirectory;$CompilerBin;$QtPrefix/bin;$oldPath"
    if($HeadlessSimulators -or $SmokeExitMs -gt 0) {
        # Automated deployment checks retain four independent, non-graphical station processes.
        for($station=1;$station -le $Stations;$station++) {
            $stationDirectory=Join-Path $DataDirectory "station-$station"
            $arguments=@('--headless','--port',"$($BasePort+$station-1)",'--station-id',"$station",'--data-dir',('"{0}"' -f $stationDirectory),'--step-ms','400')
            if($station -eq $FailStation) {$arguments+=@('--fail-step','current')}
            if($station -eq 4) {$arguments+='--drop-first-result-ack'}
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
    $arguments=@('--base-port',"$BasePort",'--stations',"$Stations",'--data-dir',('"{0}"' -f (Join-Path $DataDirectory 'client')))
    $windowStyle='Normal'
    if($SmokeExitMs -gt 0) {$arguments+=@('--smoke-exit-ms',"$SmokeExitMs");$windowStyle='Hidden'}
    $window=Start-Process -FilePath $client -ArgumentList $arguments -PassThru -WindowStyle $windowStyle
    try {$window.WaitForExit()} finally {$window.Dispose()}
} finally {
    # Stop only the simulators owned by this launch. Let graphical workers close normally first.
    foreach($process in $ownedProcesses) {
        if(!$process.HasExited) {
            $closing=$process.CloseMainWindow()
            if(!$closing -or !$process.WaitForExit(3000)) {$process.Kill();$process.WaitForExit()}
        }
        $process.Dispose()
    }
    $env:PATH=$oldPath
}
