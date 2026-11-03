param(
    [ValidateRange(1000,1000000)][int]$Rows=100000,
    [string]$QtPrefix='D:/Qt6/6.10.3/mingw_64',
    [string]$CompilerBin='D:/Qt6/Tools/mingw1310_64/bin'
)
$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot -Parent
$executable=Join-Path $projectRoot 'build/quality/HistoryBenchmark.exe'
if(!(Test-Path -LiteralPath $executable)) {throw 'Run scripts/verify.ps1 to build the benchmark executable first.'}
$oldPath=$env:PATH
try {
    $env:PATH="$CompilerBin;$QtPrefix/bin;$oldPath"
    $output=Join-Path $projectRoot "build/history-benchmark-$Rows.json"
    & $executable --rows "$Rows" --output $output
    if($LASTEXITCODE -ne 0){throw "History benchmark failed: $LASTEXITCODE"}
    Get-Content -LiteralPath $output -Encoding UTF8
} finally {$env:PATH=$oldPath}
