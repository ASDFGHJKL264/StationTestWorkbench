# StationTestWorkbench

**电子模块多工位功能测试与质量追溯工作台**，Qt 6 Widgets / C++17 / TCP / SQLite。

界面采用固定浅色配色，文字、输入框、表格、选中项和禁用状态统一设置。Windows 使用深色主题或运行中切换主题时，工作界面仍保持浅色和清晰的文字。

本工程只构建操作员主程序。模拟器已经拆为同级独立工程 `D:\demoqt6\StationSimulator`，有自己的源码、CMake、测试、启动脚本和便携包。两个程序在运行时通过 TCP 通信，主程序的默认构建和测试不需要模拟器源码。

目前使用模拟工位验证业务流程；电压、电流和 IO 数值由模拟器生成，默认限值是演示配方。

## 直接运行

双击 `启动主程序.cmd`，只打开操作员主程序。默认连接本机 46501～46504 端口；未启动模拟器时显示离线，随后启动工位会自动尝试连接。

模拟器在另一工程中编译并通过其 `启动模拟器.cmd` 独立启动，点击“启动全部”开始监听。也可直接运行 `StationSimulator.exe`，详细配置见模拟器工程 README。两个程序分别关闭。

在工位表选择一个工位，输入产品 SN 和配方，点击“开始测试”。步骤逐项更新；测试完成后分别显示质量判定、归档提交和工位确认。取消操作等待工位返回真实终态。历史查询按完整 SN 精确匹配，留空查询全部，每页最多100条，采用 ID 游标翻页。

历史记录页支持两种导出：

- “导出本次报告”：导出所选运行的 HTML 报告，包含身份、配方快照哈希、限值、步骤与未执行项。
- “导出异常 SN”：读取全部已归档 FAIL 记录，按 SN 去重导出 UTF-8 BOM CSV，不受当前查询和分页限制。复测 PASS 不抹除历史 FAIL；取消、未知和未归档运行不作为失败记录。没有异常时输出 SN 表头，成功后日志显示数量。表格软件导入时将 SN 列设为文本可保留前导零。

两个工程均已构建后，可选的 `启动演示.cmd` 从同级模拟器工程启动模拟器和客户端，退出本次客户端时结束该脚本启动的模拟器。联合演示默认工位3电流超限、工位4丢弃首次归档确认。它只作为源码目录中的辅助入口，不包含在主程序独立便携包内。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\start-demo.ps1
# 使用分别解压的便携目录时，显式指定模拟器位置：
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\start-demo.ps1 -SimulatorProgramDirectory D:\demoqt6\StationSimulator\dist\StationSimulator-0.1.0-win64
```

联合演示可通过 `-BasePort` 更换端口，`-Stations 2 -FailStation 0` 关闭异常示例，`-HeadlessSimulators` 使用独立后台进程。`-DataDirectory` 为联合演示指定数据根目录；不要让两个客户端同时使用同一数据目录。

## 构建与验证

在 Qt Creator 中打开根目录 `CMakeLists.txt`，选择 Qt 6 MinGW 64位套件。主程序需要 Core、Network、Sql、Widgets，测试额外需要 Qt Test。

```powershell
# 独立构建及7组测试，不要求模拟器源码存在：
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\verify.ps1 -BatchRuns 100

# 另外启用两项目联调，执行10组工作台测试：
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\verify.ps1 -WithSimulator -BatchRuns 100
```

联调默认读取同级 `StationSimulator`；非同级布局可通过 `-SimulatorSourceDirectory` 指定。Qt Creator 中也可设置 `WORKBENCH_ENABLE_SIMULATOR_TESTS=ON` 和 `STATION_SIMULATOR_SOURCE_DIR`。只有这些可选测试会导入模拟器目标，生产主程序不链接模拟器。

测试使用临时目录与本机 TCP，不写入演示数据。测试文件统一平铺在 `tests/`；其中 `test_window_simulator.cpp`、`test_simulator_window.cpp` 和 `test_process.cpp` 为可选跨项目回归。结果位于 `build/quality/*-results.txt` 和 JUnit 报告。

10万轮次历史数据的独立查询性能测量：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\benchmark.ps1 -Rows 100000
```

## 独立便携包

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\package.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\smoke-portable.ps1
```

主程序包位于 `dist/StationTestWorkbench-0.1.0-win64.zip`，只包含工作台及所需 Qt 运行库。模拟器使用另一工程的打包脚本，生成单独的 `StationSimulator-0.1.0-win64.zip`。主程序部署验证允许工位离线，确认独立启动、SQLite 与日志初始化正常。

## 配方与一致性

内置 `demo-5v-v1` 包含输出电压、待机电流、IO 回环。“发布配方”可以加载 `recipes/demo-5v-v2.json` 或新增启动耗时步骤的 `recipes/demo-5v-startup-v1.json`。模拟器按 TCP 配方预览更新异常选项，无需先执行一次测试。

每个配方步骤包含 `id`、`label`、`unit`、`min`、`max`、`nominal`，满足 `min <= nominal <= max`，最多16步。限值包含端点，步骤顺序参与契约。已发布版本不可原地修改，同版本同内容发布为幂等操作；运行另存完整配方快照，历史结果不随新配方变化。

程序先创建 UUID runId 并持久化产品 SN、工位和配方，再发送 start。同一未结算 SN 不会同时在多个工位执行。accepted 只表示受理，终态的 PASS/FAIL 由本地配方规则重新计算。结果、步骤、判定和运行终态在同一事务提交后，才发送 resultAck，并等待工位持久化确认后放开下一次测试。

相同 runId 的重复终态不会增加记录，冲突终态不覆盖已有结果。断线后按原 runId 查询，不盲目重复 start；客户端重启继续保留未核对身份的占用。存储失败保留终态并重试，数据库、网络和日志文件分别在后台线程处理。

协议仍为 STW2 v2。两工程分别维护配方校验及编解码实现；修改线上契约时需同步两端并运行跨项目回归。CRC 用于传输差错检测。更多说明见 [架构与协议](docs/ARCHITECTURE.md)、[目录与业务职责](docs/PROJECT_STRUCTURE.md)、[验证记录](docs/VALIDATION.md)。

## 文件与数据

```text
CMakeLists.txt           两个库、主程序及测试的统一构建配置
main.cpp                 操作员主程序入口
domain.* stationendpoint.h 配方、身份、质量判定和连接配置
testengine.*             工位通信与测试执行状态机
repository.*             SQLite事务、恢复、历史、HTML与异常SN导出
workbenchcontroller.*    应用协调与线程生命周期
framecodec.* asyncfilelogger.* 通信帧与异步日志
mainwindow.* models.* toolstyle.h 窗口、表格模型和样式
tests/                   独立测试、可选联调及历史查询基准
scripts/ docs/ recipes/  验证、启动、打包、说明与配方示例
```

沿用同级温湿度项目 `Qwenshiducaiji` 的目录风格：`.h/.cpp` 平铺在根目录，测试平铺在 `tests/`，由一个根 `CMakeLists.txt` 管理。构建分为 `WorkbenchRuntime`（运行逻辑）和 `WorkbenchWidgets`（界面）两个库，对应温湿度项目的 `SensorRuntime` / `SensorWidgets`。业务职责仍按文件区分，模拟器保持同级独立工程。

`启动主程序.cmd` 继续使用 `demo-data/client/data/workbench.db` 和 `demo-data/client/logs/workbench.log`。源码调整不搬迁已有数据库、日志和工位状态文件。新模拟器启动脚本默认使用自己的 `demo-data`；继续使用原工位状态时，可对模拟器显式指定原目录。

裸客户端默认使用 Qt AppDataLocation，`--data-dir` 可覆盖。原温湿度、串口和其他 Qt 工程均独立保留。本项目尚未接入实际测试仪器、PLC 或治具，模拟验证不代表真实电气检测交付。
