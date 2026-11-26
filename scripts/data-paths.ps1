# 返回独立于程序安装目录的数据位置；自定义演示目录沿用原有布局。
function Get-WorkbenchDataPaths {
    param(
        # 显式指定的客户端目录或联合演示根目录。
        [string]$DataDirectory='',
        # 联合演示的自定义根目录下继续使用 client 子目录。
        [switch]$JointDemo,
        # 默认用户数据根目录；回归验证可替换为临时目录。
        [string]$LocalAppDataRoot=[Environment]::GetFolderPath('LocalApplicationData')
    )
    if($DataDirectory) {
        # 自定义客户端目录保持原值，联合演示保留原来的 client 子目录。
        $clientDirectory=$DataDirectory
        if($JointDemo) {$clientDirectory=Join-Path $DataDirectory 'client'}
        # 自定义联合演示继续在指定根目录保存模拟工位状态。
        $simulatorDirectory=Join-Path $DataDirectory 'simulator'
        if($JointDemo) {$simulatorDirectory=$DataDirectory}
    } else {
        if([string]::IsNullOrWhiteSpace($LocalAppDataRoot)) {throw 'The local application data directory is unavailable.'}
        # 与 Qt AppLocalDataLocation 对应，升级或删除旧程序不改变此目录。
        $clientDirectory=Join-Path $LocalAppDataRoot 'QtPortfolio\StationTestWorkbench'
        # 设备状态与客户端数据库分开存放，但都不依赖版本号。
        $simulatorDirectory=Join-Path $clientDirectory 'simulator'
    }
    return [pscustomobject]@{Client=$clientDirectory;Simulator=$simulatorDirectory}
}
