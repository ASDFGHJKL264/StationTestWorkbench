param(
    [string]$QtPrefix='D:/Qt6/6.10.3/mingw_64',
    [string]$CompilerBin='D:/Qt6/Tools/mingw1310_64/bin',
    [string]$CMake='D:/Qt6/Tools/CMake_64/bin/cmake.exe',
    [string]$Ninja='D:/Qt6/Tools/Ninja/ninja.exe',
    [ValidateRange(1,10000)][int]$BatchRuns=100
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
    Run-Checked $CMake @('-S',$projectRoot,'-B',$build,'-G','Ninja',"-DCMAKE_MAKE_PROGRAM=$Ninja", "-DCMAKE_CXX_COMPILER=$CompilerBin/g++.exe", "-DCMAKE_PREFIX_PATH=$QtPrefix",'-DCMAKE_BUILD_TYPE=Debug','-DBUILD_TESTING=ON')
    Run-Checked $CMake @('--build',$build,'--parallel','4')
    Run-Checked (Join-Path (Split-Path $CMake -Parent) 'ctest.exe') @('--test-dir',$build,'--output-on-failure','--output-junit',(Join-Path $build 'test-results.xml'))
    Get-ChildItem -LiteralPath $build -Filter '*-results.txt' | ForEach-Object {
        Select-String -LiteralPath $_.FullName -Encoding UTF8 -Pattern 'Totals:|batch|metrics|maximum|cycles'
    }
} finally { $env:PATH=$oldPath;$env:WORKBENCH_BATCH_RUNS=$oldRuns }
