# 只加载安装脚本中的纯迁移函数，不安装程序或访问真实桌面入口。
$ErrorActionPreference='Stop'
# 从当前PowerShell安装位置加载哈希与JSON命令，避免继承其他版本的模块路径。
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -ErrorAction Stop
# 被验证的安装脚本。
$installer=Join-Path (Split-Path $PSScriptRoot -Parent) 'scripts/install-portable.ps1'
# 收集解析结果，避免执行安装脚本的主体。
$tokens=$null
# 收集脚本语法错误。
$parseErrors=$null
# 提取已知迁移函数的语法树。
$tree=[Management.Automation.Language.Parser]::ParseFile($installer,[ref]$tokens,[ref]$parseErrors)
if($parseErrors.Count) {throw 'Installer script has syntax errors.'}
foreach($definition in $tree.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst]},$true)) {
    . ([scriptblock]::Create($definition.Extent.Text))
}
# 为迁移单元验证提供确定的停机状态，不操作真实进程。
function Get-Process {param([string[]]$Name,$ErrorAction) return @()}
# 全部数据文件均为临时标记，不是用户数据库或凭据。
$fixture=Join-Path (Split-Path $PSScriptRoot -Parent) ('build/install-migration-'+[Guid]::NewGuid().ToString('N'))
# 旧版目录与固定数据目录使用不同根路径。
$old=Join-Path $fixture 'StationTestWorkbench-0.3.1-win64/client'
# 新固定数据目录。
$stable=Join-Path $fixture 'Local/QtPortfolio/StationTestWorkbench'
[void][IO.Directory]::CreateDirectory((Join-Path $old 'data'))
[void][IO.Directory]::CreateDirectory((Join-Path $old 'maintenance'))
[IO.File]::WriteAllText((Join-Path $old 'data/workbench.db'),'temporary database marker')
[IO.File]::WriteAllText((Join-Path $old 'maintenance/admin-auth.json'),'temporary credential marker')
# 在源锁被占用时必须拒绝迁移，不发布新数据目录。
$lockPath=Join-Path $old '.workbench.lock'
# 模拟工作台持有目录锁。
$held=[IO.File]::Open($lockPath,[IO.FileMode]::Create,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
# 记录锁冲突是否正确拒绝。
$rejected=$false
try {try {Copy-VerifiedData $old $stable | Out-Null} catch {$rejected=$true}} finally {$held.Dispose();[IO.File]::Delete($lockPath)}
if(!$rejected -or (Test-Path -LiteralPath $stable)) {throw 'Locked source was migrated.'}
# 停写后复制完整数据库和凭据，原目录保持完整。
$copied=Copy-VerifiedData $old $stable
if($copied -ne 2 -or !(Test-Path -LiteralPath (Join-Path $old 'maintenance/admin-auth.json'))) {throw 'Migration did not preserve the original files.'}
foreach($relative in @('data/workbench.db','maintenance/admin-auth.json')) {
    if((Get-FileHash -LiteralPath (Join-Path $old $relative)).Hash -cne (Get-FileHash -LiteralPath (Join-Path $stable $relative)).Hash) {throw 'Migrated file changed.'}
}
$rejected=$false
try {Copy-VerifiedData $old $stable | Out-Null} catch {$rejected=$true}
if(!$rejected) {throw 'An existing target was overwritten.'}
# 构造恢复切换，确认迁移选择已激活的新库，而非旧库。
$restored=Join-Path $fixture 'restored'
[void][IO.Directory]::CreateDirectory((Join-Path $restored 'data'))
[IO.File]::WriteAllText((Join-Path $restored 'data/workbench.db'),'restored database marker')
[IO.File]::WriteAllText((Join-Path $old 'maintenance/active-data-dir.json'),(@{version=1;activeDataDirectory=$restored}|ConvertTo-Json))
if((Resolve-ActiveData $old) -ine [IO.Path]::GetFullPath($restored)) {throw 'Restore redirect was ignored.'}
# 构造循环恢复指向并确认拒绝。
[void][IO.Directory]::CreateDirectory((Join-Path $restored 'maintenance'))
[IO.File]::WriteAllText((Join-Path $restored 'maintenance/active-data-dir.json'),(@{version=1;activeDataDirectory=$old}|ConvertTo-Json))
$rejected=$false
try {Resolve-ActiveData $old | Out-Null} catch {$rejected=$true}
if(!$rejected) {throw 'Restore redirect loop was accepted.'}
'PASS: locked source, verified full copy, preserved original, existing-target rejection and restored-directory redirects.'
