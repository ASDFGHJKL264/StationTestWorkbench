param(
    [string]$QtPrefix='D:/Qt6/6.10.3/mingw_64',
    [string]$CompilerBin='D:/Qt6/Tools/mingw1310_64/bin',
    [string]$CMake='D:/Qt6/Tools/CMake_64/bin/cmake.exe',
    [string]$Ninja='D:/Qt6/Tools/Ninja/ninja.exe'
)
$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot -Parent
$build=Join-Path $projectRoot 'build/release'
$projectText=[IO.File]::ReadAllText((Join-Path $projectRoot 'CMakeLists.txt'))
$versionMatch=[regex]::Match($projectText,'project\(StationTestWorkbench VERSION (\d+\.\d+\.\d+)')
if(!$versionMatch.Success) {throw 'Cannot read the project version.'}
$version=$versionMatch.Groups[1].Value
$finalPackage=[IO.Path]::GetFullPath((Join-Path $projectRoot "dist/StationTestWorkbench-$version-win64"))
$stagingRoot=[IO.Path]::GetFullPath((Join-Path $projectRoot ('build/package-stage-'+[Guid]::NewGuid().ToString('N'))))
$package=Join-Path $stagingRoot "StationTestWorkbench-$version-win64"
$oldPath=$env:PATH
function Run-Checked([string]$Executable,[string[]]$Arguments) {
    & $Executable @Arguments
    if($LASTEXITCODE -ne 0) {throw "$Executable failed: $LASTEXITCODE"}
}
try {
    $env:PATH="$CompilerBin;$QtPrefix/bin;$oldPath"
    Run-Checked $CMake @('-S',$projectRoot,'-B',$build,'-G','Ninja',"-DCMAKE_MAKE_PROGRAM=$Ninja", "-DCMAKE_CXX_COMPILER=$CompilerBin/g++.exe", "-DCMAKE_PREFIX_PATH=$QtPrefix",'-DCMAKE_BUILD_TYPE=Release','-DBUILD_TESTING=OFF','-DWORKBENCH_ENABLE_SIMULATOR_TESTS=OFF')
    Run-Checked $CMake @('--build',$build,'--parallel','4')
    [void][IO.Directory]::CreateDirectory($package)
    foreach($name in @('StationTestWorkbench.exe')) {
        $source=Join-Path $build $name
        $destination=Join-Path $package $name
        Copy-Item -LiteralPath $source -Destination $destination
    }
    foreach($name in @('StationTestWorkbench.exe')) {
        Run-Checked (Join-Path $QtPrefix 'bin/windeployqt.exe') @('--release','--compiler-runtime','--no-translations','--no-system-d3d-compiler','--no-opengl-sw',(Join-Path $package $name))
    }
    Copy-Item -LiteralPath (Join-Path $projectRoot 'README.md') -Destination $package -Force
    # Copy only version-controlled documentation and recipes, excluding local drafts.
    $tracked=@(& git -C $projectRoot -c core.quotepath=false ls-files -- docs recipes)
    if($LASTEXITCODE -ne 0) {throw 'Cannot enumerate release documentation.'}
    foreach($relative in $tracked) {
        $destination=Join-Path $package $relative
        [void][IO.Directory]::CreateDirectory((Split-Path $destination -Parent))
        Copy-Item -LiteralPath (Join-Path $projectRoot $relative) -Destination $destination -Force
    }
    foreach($name in @('启动主程序.cmd')) {
        Copy-Item -LiteralPath (Join-Path $projectRoot $name) -Destination $package -Force
    }
    [void][IO.Directory]::CreateDirectory((Join-Path $package 'scripts'))
    foreach($name in @('start-client.ps1')) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination (Join-Path $package 'scripts') -Force
    }
    $manifest=Get-Content -LiteralPath (Join-Path $build 'build-info.json') -Raw | ConvertFrom-Json
    if($manifest.version -cne $version) {throw 'Build and package versions differ.'}
    $manifest | Add-Member -NotePropertyName executableSha256 -NotePropertyValue (Get-FileHash -LiteralPath (Join-Path $package 'StationTestWorkbench.exe') -Algorithm SHA256).Hash
    $manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $package 'release.json') -Encoding UTF8
    # Package a fresh staging tree; old release files can never enter the new archive.
    $releaseFiles=@(Get-ChildItem -LiteralPath $package -Force | Select-Object -ExpandProperty FullName)
    Compress-Archive -LiteralPath $releaseFiles -DestinationPath "$package.zip" -Force
    $distRoot=[IO.Path]::GetFullPath((Join-Path $projectRoot 'dist')).TrimEnd('\')+'\'
    $buildRoot=[IO.Path]::GetFullPath((Join-Path $projectRoot 'build')).TrimEnd('\')+'\'
    $backupRoot=[IO.Path]::GetFullPath((Join-Path $projectRoot ('build/package-backup-'+[Guid]::NewGuid().ToString('N'))))
    if(!$finalPackage.StartsWith($distRoot,[StringComparison]::OrdinalIgnoreCase) -or
       !$package.StartsWith($buildRoot,[StringComparison]::OrdinalIgnoreCase) -or
       !$backupRoot.StartsWith($buildRoot,[StringComparison]::OrdinalIgnoreCase)) {throw 'Invalid package publication paths.'}
    if(@(Get-Process -Name StationTestWorkbench -ErrorAction SilentlyContinue | Where-Object {$_.Path -ieq (Join-Path $finalPackage 'StationTestWorkbench.exe')}).Count) {throw 'Close the previous source portable application before repackaging.'}
    [void][IO.Directory]::CreateDirectory($distRoot)
    [void][IO.Directory]::CreateDirectory($backupRoot)
    # Preserve the entire old tree, including local data, in a non-distributable backup.
    if(Test-Path -LiteralPath $finalPackage) {Move-Item -LiteralPath $finalPackage -Destination (Join-Path $backupRoot 'previous-package')}
    if(Test-Path -LiteralPath ($finalPackage+'.zip')) {Move-Item -LiteralPath ($finalPackage+'.zip') -Destination (Join-Path $backupRoot 'previous-package.zip')}
    Move-Item -LiteralPath $package -Destination $finalPackage
    Move-Item -LiteralPath ($package+'.zip') -Destination ($finalPackage+'.zip')
    Write-Output "Portable package: $finalPackage.zip"
    Write-Output "Previous package backup: $backupRoot"
} finally {$env:PATH=$oldPath}
