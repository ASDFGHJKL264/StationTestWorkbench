param(
    [string]$ProgramDirectory='',
    [string]$DataDirectory='',
    [string]$HostAddress='127.0.0.1',
    [ValidateRange(1,4)][int]$Stations=4,
    [ValidateRange(1024,65532)][int]$BasePort=46501,
    [ValidateRange(0,60000)][int]$SmokeExitMs=0,
    [string]$QtPrefix='D:/Qt6/6.10.3/mingw_64',
    [string]$CompilerBin='D:/Qt6/Tools/mingw1310_64/bin'
)
$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot -Parent
if(!$ProgramDirectory) {
    if(Test-Path -LiteralPath (Join-Path $projectRoot 'StationTestWorkbench.exe')) {$ProgramDirectory=$projectRoot}
    else {$ProgramDirectory=Join-Path $projectRoot 'build/quality'}
}
$ProgramDirectory=(Resolve-Path -LiteralPath $ProgramDirectory).Path
if(!$DataDirectory) {$DataDirectory=Join-Path $projectRoot 'demo-data/client'}
$client=Join-Path $ProgramDirectory 'StationTestWorkbench.exe'
if(!(Test-Path -LiteralPath $client)) {throw 'Build the project or use the portable package first.'}
$arguments=@('--host',$HostAddress,'--base-port',"$BasePort",'--stations',"$Stations",'--data-dir',('"{0}"' -f $DataDirectory))
$windowStyle='Normal'
if($SmokeExitMs -gt 0) {$arguments+=@('--smoke-exit-ms',"$SmokeExitMs");$windowStyle='Hidden'}
$oldPath=$env:PATH
try {
    $env:PATH="$ProgramDirectory;$CompilerBin;$QtPrefix/bin;$oldPath"
    # Launch only the client. Its lifetime is independent of every simulator.
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
