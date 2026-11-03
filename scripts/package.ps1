param(
    [string]$QtPrefix='D:/Qt6/6.10.3/mingw_64',
    [string]$CompilerBin='D:/Qt6/Tools/mingw1310_64/bin',
    [string]$CMake='D:/Qt6/Tools/CMake_64/bin/cmake.exe',
    [string]$Ninja='D:/Qt6/Tools/Ninja/ninja.exe'
)
$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot -Parent
$build=Join-Path $projectRoot 'build/release'
$package=Join-Path $projectRoot 'dist/StationTestWorkbench-0.1.0-win64'
$oldPath=$env:PATH
function Run-Checked([string]$Executable,[string[]]$Arguments) {
    & $Executable @Arguments
    if($LASTEXITCODE -ne 0) {throw "$Executable failed: $LASTEXITCODE"}
}
try {
    $env:PATH="$CompilerBin;$QtPrefix/bin;$oldPath"
    Run-Checked $CMake @('-S',$projectRoot,'-B',$build,'-G','Ninja',"-DCMAKE_MAKE_PROGRAM=$Ninja", "-DCMAKE_CXX_COMPILER=$CompilerBin/g++.exe", "-DCMAKE_PREFIX_PATH=$QtPrefix",'-DCMAKE_BUILD_TYPE=Release','-DBUILD_TESTING=OFF')
    Run-Checked $CMake @('--build',$build,'--parallel','4')
    [void][IO.Directory]::CreateDirectory($package)
    foreach($name in @('StationTestWorkbench.exe','StationSimulator.exe')) {
        $source=Join-Path $build $name
        $destination=Join-Path $package $name
        # An unchanged running client does not need to be overwritten during simulator updates.
        $identical=(Test-Path -LiteralPath $destination) -and ((Get-FileHash -LiteralPath $source).Hash -eq (Get-FileHash -LiteralPath $destination).Hash)
        if(!$identical) {Copy-Item -LiteralPath $source -Destination $destination -Force}
    }
    foreach($name in @('StationTestWorkbench.exe','StationSimulator.exe')) {
        Run-Checked (Join-Path $QtPrefix 'bin/windeployqt.exe') @('--release','--compiler-runtime','--no-translations','--no-system-d3d-compiler','--no-opengl-sw',(Join-Path $package $name))
    }
    Copy-Item -LiteralPath (Join-Path $projectRoot 'README.md') -Destination $package -Force
    Copy-Item -LiteralPath (Join-Path $projectRoot 'docs') -Destination $package -Recurse -Force
    Copy-Item -LiteralPath (Join-Path $projectRoot 'recipes') -Destination $package -Recurse -Force
    foreach($name in @('启动主程序.cmd','启动演示.cmd')) {
        Copy-Item -LiteralPath (Join-Path $projectRoot $name) -Destination $package -Force
    }
    [void][IO.Directory]::CreateDirectory((Join-Path $package 'scripts'))
    foreach($name in @('start-client.ps1','start-demo.ps1')) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination (Join-Path $package 'scripts') -Force
    }
    # Keep local demonstration data on disk, but never put it into the distributable ZIP.
    $runtimeDirectories=@('demo-data','simulator-data','standalone-simulator-data')
    $releaseFiles=@(Get-ChildItem -LiteralPath $package -Force | Where-Object {$_.Name -notin $runtimeDirectories} | Select-Object -ExpandProperty FullName)
    Compress-Archive -LiteralPath $releaseFiles -DestinationPath "$package.zip" -Force
    Write-Output "Portable package: $package.zip"
} finally {$env:PATH=$oldPath}
