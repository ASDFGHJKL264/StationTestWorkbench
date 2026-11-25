param(
    # Qt 运行库和部署工具所在目录。
    [string]$QtPrefix='D:/Qt6/6.10.3/mingw_64',
    # MinGW 编译器及运行库所在目录。
    [string]$CompilerBin='D:/Qt6/Tools/mingw1310_64/bin',
    # 执行项目配置和构建的 CMake 路径。
    [string]$CMake='D:/Qt6/Tools/CMake_64/bin/cmake.exe',
    # 执行增量编译的 Ninja 路径。
    [string]$Ninja='D:/Qt6/Tools/Ninja/ninja.exe'
)
# 脚本遇到错误时立即停止，防止继续执行后续步骤。
$ErrorActionPreference='Stop'
# 由脚本位置推导出的项目根目录。
$projectRoot=Split-Path $PSScriptRoot -Parent
# 本次构建使用的输出目录。
$build=Join-Path $projectRoot 'build/release'
# 用于提取统一发布版本的 CMake 配置原文。
$projectText=[IO.File]::ReadAllText((Join-Path $projectRoot 'CMakeLists.txt'))
# 从 CMake 配置提取版本号的匹配结果。
$versionMatch=[regex]::Match($projectText,'project\(StationTestWorkbench VERSION (\d+\.\d+\.\d+)')
if(!$versionMatch.Success) {throw 'Cannot read the project version.'}
# 用于命名便携目录和压缩包的项目版本号。
$version=$versionMatch.Groups[1].Value
# 最终对外提供的版本化便携目录。
$finalPackage=[IO.Path]::GetFullPath((Join-Path $projectRoot "dist/StationTestWorkbench-$version-win64"))
# 本次打包独享的干净暂存目录。
$stagingRoot=[IO.Path]::GetFullPath((Join-Path $projectRoot ('build/package-stage-'+[Guid]::NewGuid().ToString('N'))))
# 收集程序、依赖和说明的暂存便携目录。
$package=Join-Path $stagingRoot "StationTestWorkbench-$version-win64"
# 脚本结束时需要恢复的原始 PATH。
$oldPath=$env:PATH
# 执行外部工具并在退出码异常时停止流程。 Executable：需要执行并检查退出码的工具路径。 Arguments：传给目标工具或程序的启动参数。
function Run-Checked([string]$Executable,[string[]]$Arguments) {
    & $Executable @Arguments
    if($LASTEXITCODE -ne 0) {throw "$Executable failed: $LASTEXITCODE"}
}
try {
    # 本次启动或构建所需的程序及 Qt 运行库搜索路径。
    $env:PATH="$CompilerBin;$QtPrefix/bin;$oldPath"
    Run-Checked $CMake @('-S',$projectRoot,'-B',$build,'-G','Ninja',"-DCMAKE_MAKE_PROGRAM=$Ninja", "-DCMAKE_CXX_COMPILER=$CompilerBin/g++.exe", "-DCMAKE_PREFIX_PATH=$QtPrefix",'-DCMAKE_BUILD_TYPE=Release','-DBUILD_TESTING=OFF','-DWORKBENCH_ENABLE_SIMULATOR_TESTS=OFF')
    Run-Checked $CMake @('--build',$build,'--parallel','4')
    [void][IO.Directory]::CreateDirectory($package)
    foreach($name in @('StationTestWorkbench.exe')) {
        # 从构建目录读取的主程序路径。
        $source=Join-Path $build $name
        # 当前发布文件应写入的目标路径。
        $destination=Join-Path $package $name
        Copy-Item -LiteralPath $source -Destination $destination
    }
    foreach($name in @('StationTestWorkbench.exe')) {
        Run-Checked (Join-Path $QtPrefix 'bin/windeployqt.exe') @('--release','--compiler-runtime','--no-translations','--no-system-d3d-compiler','--no-opengl-sw',(Join-Path $package $name))
    }
    Copy-Item -LiteralPath (Join-Path $projectRoot 'README.md') -Destination $package -Force
    # Copy only version-controlled documentation and recipes, excluding local drafts.
    # 纳入版本控制且允许进入发布包的说明和配方文件清单。
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
    # 记录发布版本、源码提交及主程序哈希的清单对象。
    $manifest=Get-Content -LiteralPath (Join-Path $build 'build-info.json') -Raw | ConvertFrom-Json
    if($manifest.version -cne $version) {throw 'Build and package versions differ.'}
    $manifest | Add-Member -NotePropertyName executableSha256 -NotePropertyValue (Get-FileHash -LiteralPath (Join-Path $package 'StationTestWorkbench.exe') -Algorithm SHA256).Hash
    $manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $package 'release.json') -Encoding UTF8
    # Package a fresh staging tree; old release files can never enter the new archive.
    # 本次干净暂存目录中允许压缩的发布文件。
    $releaseFiles=@(Get-ChildItem -LiteralPath $package -Force | Select-Object -ExpandProperty FullName)
    Compress-Archive -LiteralPath $releaseFiles -DestinationPath "$package.zip" -Force
    # 用于检查最终发布路径边界的发行目录。
    $distRoot=[IO.Path]::GetFullPath((Join-Path $projectRoot 'dist')).TrimEnd('\')+'\'
    # 用于检查暂存和备份路径边界的构建目录。
    $buildRoot=[IO.Path]::GetFullPath((Join-Path $projectRoot 'build')).TrimEnd('\')+'\'
    # 完整保留上一份便携目录及压缩包的备份位置。
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
