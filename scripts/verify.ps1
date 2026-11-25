param(
    # Qt 运行库和部署工具所在目录。
    [string]$QtPrefix='D:/Qt6/6.10.3/mingw_64',
    # MinGW 编译器及运行库所在目录。
    [string]$CompilerBin='D:/Qt6/Tools/mingw1310_64/bin',
    # 执行项目配置和构建的 CMake 路径。
    [string]$CMake='D:/Qt6/Tools/CMake_64/bin/cmake.exe',
    # 执行增量编译的 Ninja 路径。
    [string]$Ninja='D:/Qt6/Tools/Ninja/ninja.exe',
    # 连续测试归档的验证轮数。
    [ValidateRange(1,10000)][int]$BatchRuns=100,
    # 是否启用独立模拟器的跨项目联调测试。
    [switch]$WithSimulator,
    # 可选联调使用的模拟器源码目录。
    [string]$SimulatorSourceDirectory=''
)
# 脚本遇到错误时立即停止，防止继续执行后续步骤。
$ErrorActionPreference='Stop'
# 由脚本位置推导出的项目根目录。
$projectRoot=Split-Path $PSScriptRoot -Parent
# 本次构建使用的输出目录。
$build=Join-Path $projectRoot 'build/quality'
# 脚本结束时需要恢复的原始 PATH。
$oldPath=$env:PATH
# 脚本结束时需要恢复的原始批量测试轮数设置。
$oldRuns=$env:WORKBENCH_BATCH_RUNS
# 执行外部工具并在退出码异常时停止流程。 Executable：需要执行并检查退出码的工具路径。 Arguments：传给目标工具或程序的启动参数。
function Run-Checked([string]$Executable,[string[]]$Arguments) {
    & $Executable @Arguments
    if($LASTEXITCODE -ne 0) { throw "$Executable failed: $LASTEXITCODE" }
}
try {
    # 本次启动或构建所需的程序及 Qt 运行库搜索路径。
    $env:PATH="$CompilerBin;$QtPrefix/bin;$oldPath"
    # 向批量流程测试传递的连续归档轮数。
    $env:WORKBENCH_BATCH_RUNS="$BatchRuns"
    # 传给 CMake 的项目、工具链和测试配置参数。
    $configure=@('-S',$projectRoot,'-B',$build,'-G','Ninja',"-DCMAKE_MAKE_PROGRAM=$Ninja", "-DCMAKE_CXX_COMPILER=$CompilerBin/g++.exe", "-DCMAKE_PREFIX_PATH=$QtPrefix",'-DCMAKE_BUILD_TYPE=Debug','-DBUILD_TESTING=ON')
    if($WithSimulator) {
        if(!$SimulatorSourceDirectory) {$SimulatorSourceDirectory=Join-Path (Split-Path $projectRoot -Parent) 'StationSimulator'}
        $SimulatorSourceDirectory=(Resolve-Path -LiteralPath $SimulatorSourceDirectory).Path
        $configure+=@('-DWORKBENCH_ENABLE_SIMULATOR_TESTS=ON',"-DSTATION_SIMULATOR_SOURCE_DIR=$SimulatorSourceDirectory")
    } else {$configure+='-DWORKBENCH_ENABLE_SIMULATOR_TESTS=OFF'}
    Run-Checked $CMake $configure
    Run-Checked $CMake @('--build',$build,'--parallel','4')
    Run-Checked (Join-Path (Split-Path $CMake -Parent) 'ctest.exe') @('--test-dir',$build,'--output-on-failure','--output-junit',(Join-Path $build 'test-results.xml'))
    # 读取已注册测试清单的 CTest 工具路径。
    $ctest=Join-Path (Split-Path $CMake -Parent) 'ctest.exe'
    # 当前构建目录中已注册的自动测试清单。
    $registered=(& $ctest --test-dir $build --show-only=json-v1 | ConvertFrom-Json).tests
    foreach($test in $registered) {
        Select-String -LiteralPath (Join-Path $build ($test.name+'-results.txt')) -Encoding UTF8 -Pattern 'Totals:|batch|metrics|maximum|cycles'
    }
} finally { $env:PATH=$oldPath;$env:WORKBENCH_BATCH_RUNS=$oldRuns }
