param(
    # 已解压且包含release.json的工作台发布目录。
    [Parameter(Mandatory=$true)][string]$PackageDirectory,
    # 安装新版程序的位置，缺省为D盘的对应版本目录。
    [string]$InstallDirectory='',
    # 需要迁移的旧客户端数据目录，留空则沿用固定目录。
    [string]$LegacyClientDirectory='',
    # 需要迁移的旧模拟器状态根目录，仅复制station-*内容。
    [string]$LegacySimulatorDirectory=''
)
$ErrorActionPreference='Stop'
# 从当前PowerShell安装位置加载哈希与JSON命令，避免继承其他版本的模块路径。
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -ErrorAction Stop
. (Join-Path $PSScriptRoot 'data-paths.ps1')

# 解析已激活的恢复目录，避免迁移到已废弃的旧数据库。
function Resolve-ActiveData([string]$Directory) {
    # 记录经过的路径以拒绝循环配置。
    $seen=@{}
    for($depth=0;$depth -lt 32;$depth++) {
        $Directory=[IO.Path]::GetFullPath($Directory)
        if($seen.ContainsKey($Directory)) {throw 'Data-directory redirect loop.'}
        $seen[$Directory]=$true
        # 当前目录保存的恢复指向。
        $pointer=Join-Path $Directory 'maintenance/active-data-dir.json'
        if(!(Test-Path -LiteralPath $pointer)) {return $Directory}
        if((Get-Item -LiteralPath $pointer).Length -gt 65536) {throw 'Invalid data-directory redirect.'}
        # 只读取恢复指向，不读取或输出管理员凭据。
        $record=Get-Content -LiteralPath $pointer -Raw | ConvertFrom-Json
        if($record.version -ne 1 -or ![IO.Path]::IsPathRooted($record.activeDataDirectory) -or
           !(Test-Path -LiteralPath (Join-Path $record.activeDataDirectory 'data/workbench.db'))) {throw 'Invalid restored data directory.'}
        $Directory=$record.activeDataDirectory
    }
    throw 'Too many data-directory redirects.'
}

# 在空暂存目录复制并核验所有文件，原目录保持完整。
function Copy-VerifiedData([string]$Source,[string]$Destination,[switch]$StationFilesOnly) {
    if(!(Test-Path -LiteralPath $Source -PathType Container)) {throw 'Legacy data directory is missing.'}
    if(Test-Path -LiteralPath $Destination) {throw 'Migration target already exists; existing data was not overwritten.'}
    $Source=[IO.Path]::GetFullPath($Source).TrimEnd('\')
    $Destination=[IO.Path]::GetFullPath($Destination)
    if(@(Get-ChildItem -LiteralPath $Source -Recurse -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) {throw 'Legacy data contains a junction or symbolic link; migration stopped.'}
    # 在复制客户端期间独占其锁文件，阻止旧程序同时重新打开数据库。
    $sourceLock=$null
    # 仅清理本次创建的临时锁文件，不删除原来的锁记录。
    $createdLock=$false
    # 锁文件与Qt主程序使用同一个位置。
    $lockPath=Join-Path $Source '.workbench.lock'
    if(!$StationFilesOnly) {
        $createdLock=!(Test-Path -LiteralPath $lockPath)
        $sourceLock=[IO.File]::Open($lockPath,[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    }
    try {
    # 暂存目录限定在目标目录的同一个父目录内。
    $parent=Split-Path $Destination -Parent
    [void][IO.Directory]::CreateDirectory($parent)
    # 每次迁移使用独立暂存名，失败保留原数据供重试排查。
    $stage=Join-Path $parent ('data-migrate-'+[Guid]::NewGuid().ToString('N'))
    [void][IO.Directory]::CreateDirectory($stage)
    # 保存核验文件数量，不在日志输出文件内容或凭据摘要。
    $count=0
    foreach($file in Get-ChildItem -LiteralPath $Source -File -Recurse -Force) {
        if($file.Name -eq '.workbench.lock') {continue}
        # 计算复制文件相对于原目录的位置。
        $relative=$file.FullName.Substring($Source.Length+1)
        if($StationFilesOnly -and $relative -notmatch '^station-[^\\/]+([\\/]|$)') {continue}
        # 新目录内与原文件一一对应的目标路径。
        $target=Join-Path $stage $relative
        [void][IO.Directory]::CreateDirectory((Split-Path $target -Parent))
        Copy-Item -LiteralPath $file.FullName -Destination $target
        if((Get-FileHash -LiteralPath $file.FullName).Hash -cne (Get-FileHash -LiteralPath $target).Hash) {throw 'Data migration hash mismatch.'}
        $count++
    }
    if(@(Get-Process -Name StationTestWorkbench,StationSimulator -ErrorAction SilentlyContinue).Count) {throw 'An application started during migration; original data and shortcuts were retained.'}
    if(![IO.Path]::GetFullPath($stage).StartsWith($parent.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase) -or
       [IO.Path]::GetFullPath($Destination) -cne $Destination) {throw 'Unexpected migration publication path.'}
    Move-Item -LiteralPath $stage -Destination $Destination
    return $count
    } finally {
        if($null -ne $sourceLock) {$sourceLock.Dispose(); if($createdLock) {[IO.File]::Delete($lockPath)}}
    }
}

# 当前用户的固定客户端和模拟器数据目录，不包含程序版本号。
$paths=Get-WorkbenchDataPaths
$PackageDirectory=(Resolve-Path -LiteralPath $PackageDirectory).Path
# 校验发行版本和程序哈希，拒绝未提交源码构建。
$manifest=Get-Content -LiteralPath (Join-Path $PackageDirectory 'release.json') -Raw | ConvertFrom-Json
if($manifest.version -notmatch '^\d+\.\d+\.\d+$' -or $manifest.revision -match 'modified|unknown' -or
   (Get-FileHash -LiteralPath (Join-Path $PackageDirectory 'StationTestWorkbench.exe')).Hash -cne $manifest.executableSha256) {throw 'Invalid release payload.'}
if(!$InstallDirectory) {$InstallDirectory='D:\StationTestWorkbench-'+$manifest.version+'-win64'}
$InstallDirectory=[IO.Path]::GetFullPath($InstallDirectory)
if(Test-Path -LiteralPath $InstallDirectory) {throw 'Installation directory already exists.'}
if(@(Get-Process -Name StationTestWorkbench,StationSimulator -ErrorAction SilentlyContinue).Count) {throw 'Close the workbench and simulator before migrating data. No processes were terminated.'}

# 保留两个桌面入口，只有迁移及程序核验成功才更新工作台入口。
$desktop=[Environment]::GetFolderPath('Desktop')
# 使用现有模拟器程序位置，模拟器本身不随本次修复升级。
$shell=New-Object -ComObject WScript.Shell
# 原工作台桌面入口。
$workbenchPath=Join-Path $desktop '多工位功能测试与追溯.lnk'
# 原模拟器桌面入口。
$simulatorPath=Join-Path $desktop '多工位设备模拟器.lnk'
# 读取两个原入口的参数及目标。
$workbench=$shell.CreateShortcut($workbenchPath)
# 保留模拟器参数中的端口、工位数和故障选项。
$simulator=$shell.CreateShortcut($simulatorPath)
if(!(Test-Path -LiteralPath $simulator.TargetPath)) {throw 'Existing simulator shortcut target is missing.'}
# 提前核验旧数据参数，防止升级时意外丢弃用户明确指定的自定义目录。
$dataArgument='(?:^|\s)--data-dir(?:\s+|=)(?:"([^"]*)"|(\S+))'
# 读取原工作台入口的数据参数，不输出参数内容。
$oldArgument=[regex]::Match($workbench.Arguments,$dataArgument)
if($LegacyClientDirectory -and $oldArgument.Success) {
    # 只允许迁移已明确指定的旧入口目录。
    $oldDirectory=if($oldArgument.Groups[1].Success) {$oldArgument.Groups[1].Value} else {$oldArgument.Groups[2].Value}
    if([IO.Path]::GetFullPath($oldDirectory).TrimEnd('\') -ine [IO.Path]::GetFullPath($LegacyClientDirectory).TrimEnd('\')) {throw 'Legacy directory differs from the existing desktop configuration.'}
}
if(!$LegacyClientDirectory -and $oldArgument.Success -and $oldArgument.Value -match 'StationTestWorkbench-\d+\.\d+\.\d+-win64') {throw 'Supply LegacyClientDirectory to migrate this old version-bound desktop configuration.'}
# 保留原快捷方式，便于失败排查。
$backup=Join-Path (Split-Path $paths.Client -Parent) ('StationTestWorkbench-shortcuts-'+[DateTime]::UtcNow.ToString('yyyyMMddHHmmss'))
[void][IO.Directory]::CreateDirectory($backup)
foreach($link in @($workbenchPath,$simulatorPath)) {if(Test-Path -LiteralPath $link) {Copy-Item -LiteralPath $link -Destination $backup}}
# 统计已核验的迁移文件数。
$migratedFiles=0
if($LegacyClientDirectory) {$migratedFiles+=Copy-VerifiedData (Resolve-ActiveData $LegacyClientDirectory) $paths.Client}
if($LegacySimulatorDirectory) {$migratedFiles+=Copy-VerifiedData $LegacySimulatorDirectory $paths.Simulator -StationFilesOnly}
Copy-Item -LiteralPath $PackageDirectory -Destination $InstallDirectory -Recurse
foreach($file in Get-ChildItem -LiteralPath $PackageDirectory -Recurse -File) {
    # 核验安装后的每一个程序文件。
    $relative=$file.FullName.Substring($PackageDirectory.Length+1)
    if((Get-FileHash -LiteralPath $file.FullName).Hash -cne (Get-FileHash -LiteralPath (Join-Path $InstallDirectory $relative)).Hash) {throw 'Installed payload mismatch.'}
}
if(Test-Path -LiteralPath ($PackageDirectory+'.zip')) {
    Copy-Item -LiteralPath ($PackageDirectory+'.zip') -Destination ($InstallDirectory+'.zip')
    if((Get-FileHash -LiteralPath ($PackageDirectory+'.zip')).Hash -cne (Get-FileHash -LiteralPath ($InstallDirectory+'.zip')).Hash) {throw 'Installed archive mismatch.'}
}

# 删除历史版本的数据路径参数，其他连接参数保持原样。
if($LegacyClientDirectory) {$workbench.Arguments=([regex]::Replace($workbench.Arguments,$dataArgument,'')).Trim()}
$workbench.TargetPath=Join-Path $InstallDirectory 'StationTestWorkbench.exe'
$workbench.WorkingDirectory=$InstallDirectory
$workbench.IconLocation=$workbench.TargetPath+',0'
$workbench.Save()
$simulator.Arguments=([regex]::Replace($simulator.Arguments,$dataArgument,'')).Trim()+' --data-dir "'+$paths.Simulator+'"'
$simulator.Save()
# 重读入口，核验数据位置不再绑定旧版程序目录。
$check=$shell.CreateShortcut($workbenchPath)
if($check.TargetPath -ine $workbench.TargetPath -or $check.Arguments -cne $workbench.Arguments) {throw 'Workbench shortcut verification failed.'}
$check=$shell.CreateShortcut($simulatorPath)
if(!$check.Arguments.Contains($paths.Simulator)) {throw 'Simulator shortcut verification failed.'}
[ordered]@{version=$manifest.version;installation=$InstallDirectory;clientData=$paths.Client;simulatorData=$paths.Simulator;migratedFilesVerified=$migratedFiles;shortcutBackup=$backup;originalDataRetained=$true} | ConvertTo-Json
