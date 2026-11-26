# 路径回归只使用本次 build 夹具和内存进程替身，不打开真实程序或修改用户数据。
$ErrorActionPreference='Stop'
# 定位被验证的启动脚本和共享路径函数。
$projectRoot=Split-Path $PSScriptRoot -Parent
. (Join-Path $projectRoot 'scripts/data-paths.ps1')
# 隔离模拟的安装目录、数据目录和可执行文件。
$fixture=Join-Path $projectRoot ('build/launcher-paths-'+[Guid]::NewGuid().ToString('N'))
# 提供临时的用户本地数据根目录，避免访问真实管理员配置。
$localData=Join-Path $fixture 'Local Application Data'
# 模拟已删除的旧版本安装目录。
$oldVersion=Join-Path $fixture 'StationTestWorkbench-0.3.1-win64'
# 模拟升级后的安装目录。
$newVersion=Join-Path $fixture 'StationTestWorkbench-0.4.1-win64'
[void][IO.Directory]::CreateDirectory($oldVersion)
[void][IO.Directory]::CreateDirectory($newVersion)

# 失败时只报告本地测试上下文，不读取或打印任何管理员凭据。
function Assert-Equal {
    param(
        # 必须满足的目标值。
        $Expected,
        # 当前实际值。
        $Actual,
        # 标识失败的业务行为。
        [string]$Context
    )
    if($Actual -cne $Expected) {throw ('Assertion failed: '+$Context)}
}

Push-Location -LiteralPath $oldVersion
try {
    # 从旧版本工作目录解析默认路径。
    $before=Get-WorkbenchDataPaths -LocalAppDataRoot $localData
} finally {Pop-Location}
Push-Location -LiteralPath $newVersion
try {
    # 从新版本工作目录解析同一个默认路径。
    $after=Get-WorkbenchDataPaths -LocalAppDataRoot $localData
} finally {Pop-Location}
Assert-Equal (Join-Path $localData 'QtPortfolio\StationTestWorkbench') $before.Client 'The default client directory uses local application data.'
Assert-Equal $before.Client $after.Client 'Upgrading or changing the working directory preserves the client directory.'
Assert-Equal (Join-Path $before.Client 'simulator') $after.Simulator 'Simulator state uses its own version-independent subdirectory.'
Assert-Equal $before.Client (Get-WorkbenchDataPaths -JointDemo -LocalAppDataRoot $localData).Client 'The default joint demo uses the same client directory as a standalone launch.'
Assert-Equal $after.Simulator (Get-WorkbenchDataPaths -JointDemo -LocalAppDataRoot $localData).Simulator 'The default joint demo uses the same stable simulator directory.'

# 删除仅由本回归创建的空旧版本目录，再模拟首次管理员配置写入。
[IO.Directory]::Delete($oldVersion)
# 保存无认证意义的测试标记，代表新管理员配置的写入位置。
$registrationDirectory=Join-Path $after.Client 'maintenance'
[void][IO.Directory]::CreateDirectory($registrationDirectory)
[IO.File]::WriteAllText((Join-Path $registrationDirectory 'registration-probe.txt'),'temporary launcher path probe')
Assert-Equal $false (Test-Path -LiteralPath $oldVersion) 'Saving registration data must not recreate the deleted old installation.'
Assert-Equal $true (Test-Path -LiteralPath (Join-Path $before.Client 'maintenance/registration-probe.txt')) 'Registration data stays in the same stable directory after an upgrade.'

# 验证显式客户端目录不被自动附加 client 子目录。
$customClient=Join-Path $fixture 'selected client data'
# 验证显式联合演示目录沿用原有的根目录及 client 子目录布局。
$customDemo=Join-Path $fixture 'selected demo data'
Assert-Equal $customClient (Get-WorkbenchDataPaths -DataDirectory $customClient).Client 'An explicit client data directory is preserved.'
# 接收联合演示指定目录对应的两个业务路径。
$explicitDemo=Get-WorkbenchDataPaths -DataDirectory $customDemo -JointDemo
Assert-Equal (Join-Path $customDemo 'client') $explicitDemo.Client 'An explicit demo root preserves the client subdirectory.'
Assert-Equal $customDemo $explicitDemo.Simulator 'An explicit demo root remains the graphical simulator data directory.'

# 只创建占位文件供启动脚本校验存在性，后续进程替身绝不执行它们。
[IO.File]::WriteAllBytes((Join-Path $newVersion 'StationTestWorkbench.exe'),[byte[]]@())
[IO.File]::WriteAllBytes((Join-Path $newVersion 'StationSimulator.exe'),[byte[]]@())
# 捕获真实启动脚本生成的程序、参数和窗口显示方式。
$capturedStarts=[Collections.Generic.List[object]]::new()

# 拦截启动操作并返回兼容脚本等待及关闭逻辑的内存对象。
function Start-Process {
    param(
        # 被脚本选择的可执行文件。
        [string]$FilePath,
        # 被脚本拼出的完整参数集合。
        [string[]]$ArgumentList,
        # 主程序工作目录，模拟器分支可以省略。
        [string]$WorkingDirectory,
        # 与真实 Start-Process 保持相同调用形式。
        [switch]$PassThru,
        # 被脚本选择的窗口显示方式。
        [string]$WindowStyle
    )
    $capturedStarts.Add([pscustomobject]@{Program=$FilePath;Arguments=@($ArgumentList);WindowStyle=$WindowStyle})
    # 使用未启动的进程对象兼容脚本的强类型集合，所有操作由内存成员替代。
    $fakeProcess=[Diagnostics.Process]::new()
    $fakeProcess | Add-Member -MemberType NoteProperty -Name HasExited -Value $false -Force
    $fakeProcess | Add-Member -MemberType NoteProperty -Name ExitCode -Value 0 -Force
    $fakeProcess | Add-Member -MemberType ScriptMethod -Name WaitForExit -Value {$this.HasExited=$true;if($args.Count -gt 0){return $true}} -Force
    $fakeProcess | Add-Member -MemberType ScriptMethod -Name CloseMainWindow -Value {return $true} -Force
    $fakeProcess | Add-Member -MemberType ScriptMethod -Name Kill -Value {$this.HasExited=$true} -Force
    $fakeProcess | Add-Member -MemberType ScriptMethod -Name Dispose -Value {} -Force
    return $fakeProcess
}

# 从实际生成的启动参数提取数据目录，并移除供 Windows 命令行使用的引号。
function Get-CapturedDataDirectory {
    param(
        # 单个已捕获的启动操作。
        $Launch
    )
    # 定位启动脚本实际传入的数据目录选项。
    $index=[Array]::IndexOf($Launch.Arguments,'--data-dir')
    if($index -lt 0 -or $index+1 -ge $Launch.Arguments.Count) {throw 'The launch did not include a data directory.'}
    return $Launch.Arguments[$index+1].Trim('"')
}

# 默认客户端启动只捕获参数，不创建或写入真实本机数据目录。
& (Join-Path $projectRoot 'scripts/start-client.ps1') -ProgramDirectory $newVersion -SmokeExitMs 1
Assert-Equal 1 $capturedStarts.Count 'The standalone launcher starts only one client.'
Assert-Equal (Get-WorkbenchDataPaths).Client (Get-CapturedDataDirectory $capturedStarts[0]) 'The standalone launcher passes the same stable directory as the application default.'
Assert-Equal 'Hidden' $capturedStarts[0].WindowStyle 'Automated startup checks keep their process hidden.'
$capturedStarts.Clear()
& (Join-Path $projectRoot 'scripts/start-client.ps1') -ProgramDirectory $newVersion -DataDirectory $customClient -SmokeExitMs 1
Assert-Equal $customClient (Get-CapturedDataDirectory $capturedStarts[0]) 'The standalone launcher preserves a custom path including spaces.'
$capturedStarts.Clear()

# 验证联合演示的后台分支，只会在夹具内创建指定根目录。
& (Join-Path $projectRoot 'scripts/start-demo.ps1') -ProgramDirectory $newVersion -SimulatorProgramDirectory $newVersion -DataDirectory $customDemo -Stations 1 -SmokeExitMs 1
Assert-Equal 2 $capturedStarts.Count 'The headless demo starts one station and one client.'
Assert-Equal (Join-Path $customDemo 'station-1') (Get-CapturedDataDirectory $capturedStarts[0]) 'A headless station preserves its custom per-station directory.'
Assert-Equal (Join-Path $customDemo 'client') (Get-CapturedDataDirectory $capturedStarts[1]) 'The demo client preserves its custom client directory.'
$capturedStarts.Clear()

# 验证联合演示的图形面板分支，仍不启动真实模拟器或窗口。
& (Join-Path $projectRoot 'scripts/start-demo.ps1') -ProgramDirectory $newVersion -SimulatorProgramDirectory $newVersion -DataDirectory $customDemo -Stations 1
Assert-Equal 2 $capturedStarts.Count 'The graphical demo starts one panel and one client.'
Assert-Equal $customDemo (Get-CapturedDataDirectory $capturedStarts[0]) 'The graphical simulator preserves its custom root directory.'
Assert-Equal (Join-Path $customDemo 'client') (Get-CapturedDataDirectory $capturedStarts[1]) 'The graphical demo client preserves its custom client directory.'
Assert-Equal $false (Test-Path -LiteralPath $oldVersion) 'No launch branch recreates the old installation directory.'

Write-Output 'Launcher path regression passed: stable defaults, deleted old installation, custom client paths, headless demo and graphical demo.'
