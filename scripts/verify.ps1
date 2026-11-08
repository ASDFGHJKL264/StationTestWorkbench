param(
    [string]$QtPrefix='D:/Qt6/6.10.3/mingw_64',
    [string]$CompilerBin='D:/Qt6/Tools/mingw1310_64/bin',
    [string]$CMake='D:/Qt6/Tools/CMake_64/bin/cmake.exe',
    [string]$Ninja='D:/Qt6/Tools/Ninja/ninja.exe',
    [ValidateRange(1,10000)][int]$BatchRuns=100,
    [switch]$WithSimulator,
    [string]$SimulatorSourceDirectory=''
)
$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot -Parent
$build=Join-Path $projectRoot 'build/quality'
$oldPath=$env:PATH
$oldRuns=$env:WORKBENCH_BATCH_RUNS
function Run-Checked([string]$Executable,[string[]]$Arguments) {
    & $Executable @Arguments
    if($LASTEXITCODE -ne 0) { throw "$Executable failed: $LASTEXITCODE" }
}
try {
    $env:PATH="$CompilerBin;$QtPrefix/bin;$oldPath"
    $env:WORKBENCH_BATCH_RUNS="$BatchRuns"
    $configure=@('-S',$projectRoot,'-B',$build,'-G','Ninja',"-DCMAKE_MAKE_PROGRAM=$Ninja", "-DCMAKE_CXX_COMPILER=$CompilerBin/g++.exe", "-DCMAKE_PREFIX_PATH=$QtPrefix",'-DCMAKE_BUILD_TYPE=Debug','-DBUILD_TESTING=ON')
    if($WithSimulator) {
        if(!$SimulatorSourceDirectory) {$SimulatorSourceDirectory=Join-Path (Split-Path $projectRoot -Parent) 'StationSimulator'}
        $SimulatorSourceDirectory=(Resolve-Path -LiteralPath $SimulatorSourceDirectory).Path
        $configure+=@('-DWORKBENCH_ENABLE_SIMULATOR_TESTS=ON',"-DSTATION_SIMULATOR_SOURCE_DIR=$SimulatorSourceDirectory")
    } else {$configure+='-DWORKBENCH_ENABLE_SIMULATOR_TESTS=OFF'}
    Run-Checked $CMake $configure
    Run-Checked $CMake @('--build',$build,'--parallel','4')
    Run-Checked (Join-Path (Split-Path $CMake -Parent) 'ctest.exe') @('--test-dir',$build,'--output-on-failure','--output-junit',(Join-Path $build 'test-results.xml'))
    $ctest=Join-Path (Split-Path $CMake -Parent) 'ctest.exe'
    $registered=(& $ctest --test-dir $build --show-only=json-v1 | ConvertFrom-Json).tests
    foreach($test in $registered) {
        Select-String -LiteralPath (Join-Path $build ($test.name+'-results.txt')) -Encoding UTF8 -Pattern 'Totals:|batch|metrics|maximum|cycles'
    }
} finally { $env:PATH=$oldPath;$env:WORKBENCH_BATCH_RUNS=$oldRuns }
