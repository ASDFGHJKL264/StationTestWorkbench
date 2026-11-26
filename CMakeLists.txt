cmake_minimum_required(VERSION 3.19)
project(StationTestWorkbench VERSION 0.4.1 LANGUAGES CXX)
# 指定项目源码使用的 C++ 语言标准。
set(CMAKE_CXX_STANDARD 17)
# 要求编译器严格支持所选 C++ 标准。
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 自动编译 Qt 资源文件。
set(CMAKE_AUTORCC ON)
include(CTest)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Network Sql Widgets)
qt_standard_project_setup()

find_package(Git QUIET)
# 记录构建对应的源码提交，缺少 Git 信息时使用 unknown。
set(WORKBENCH_REVISION "unknown")
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE WORKBENCH_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT WORKBENCH_REVISION)
        set(WORKBENCH_REVISION "unknown")
    endif()
    # sourceChanges 保存工作树相对当前提交的检查结果。
    execute_process(COMMAND "${GIT_EXECUTABLE}" diff HEAD --quiet
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" RESULT_VARIABLE sourceChanges ERROR_QUIET)
    if(NOT sourceChanges EQUAL 0)
        string(APPEND WORKBENCH_REVISION "-modified")
    endif()
endif()
# 记录写入版本窗口和发布清单的 UTC 构建时间。
string(TIMESTAMP WORKBENCH_BUILD_UTC "%Y-%m-%dT%H:%M:%SZ" UTC)
configure_file(appversion.h.in generated/appversion.h @ONLY)
configure_file(build-info.json.in build-info.json @ONLY)

# 控制是否导入独立模拟器并构建跨项目测试。
option(WORKBENCH_ENABLE_SIMULATOR_TESTS "Build optional integration tests with StationSimulator" OFF)
# 指定可选联调使用的独立模拟器源码目录。
set(STATION_SIMULATOR_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../StationSimulator"
    CACHE PATH "Independent StationSimulator source directory (integration tests only)")

# Keep launch scripts, portable packaging and process tests on the same executable paths.
# 统一主程序、测试和启动脚本所使用的可执行文件输出目录。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

qt_add_library(WorkbenchRuntime STATIC
    domain.h domain.cpp
    stationendpoint.h
    framecodec.h framecodec.cpp
    asyncfilelogger.h asyncfilelogger.cpp
    testengine.h testengine.cpp
    repository.h repository.cpp
    runtimeconfiguration.h runtimeconfiguration.cpp
    adminaccess.h adminaccess.cpp
    databasemaintenance.h databasemaintenance.cpp
    workbenchcontroller.h workbenchcontroller.cpp workbenchmaintenance.cpp)
target_include_directories(WorkbenchRuntime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(WorkbenchRuntime PUBLIC Qt6::Core Qt6::Network Qt6::Sql)

qt_add_library(WorkbenchWidgets STATIC
    mainwindow.h mainwindow.cpp
    historydialog.h historydialog.cpp
    logdialog.h logdialog.cpp
    models.h models.cpp
    maintenancedialog.h maintenancedialog.cpp
    stationconfigurationdialog.h stationconfigurationdialog.cpp
    toolstyle.h)
target_link_libraries(WorkbenchWidgets PUBLIC WorkbenchRuntime Qt6::Widgets)

qt_add_executable(StationTestWorkbench WIN32 MACOSX_BUNDLE main.cpp)
target_include_directories(StationTestWorkbench PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
if(WIN32)
    configure_file(version.rc.in generated/version.rc @ONLY)
    target_sources(StationTestWorkbench PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc")
endif()
target_link_libraries(StationTestWorkbench PRIVATE WorkbenchWidgets)

install(TARGETS StationTestWorkbench BUNDLE DESTINATION . RUNTIME DESTINATION .)

if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)

    # 注册测试程序；name 是测试目标名，file 是测试源文件，其余参数为依赖库。
    function(workbench_test name file)
        qt_add_executable(${name} tests/${file})
        target_link_libraries(${name} PRIVATE ${ARGN} Qt6::Test)
        add_test(NAME ${name} COMMAND ${name} -o ${name}-results.txt,txt)
        # Keep reports and process fixtures beside the application executables.
        set_tests_properties(${name} PROPERTIES TIMEOUT 90 WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
    endfunction()

    workbench_test(ProtocolTests test_protocol.cpp WorkbenchRuntime)
    workbench_test(DomainTests test_domain.cpp WorkbenchRuntime)
    workbench_test(RepositoryTests test_repository.cpp WorkbenchRuntime)
    workbench_test(ModelTests test_models.cpp WorkbenchWidgets)
    workbench_test(WorkflowTests test_workflow.cpp WorkbenchRuntime)
    workbench_test(WindowTests test_window.cpp WorkbenchWidgets)
    workbench_test(LoggerTests test_logger.cpp WorkbenchRuntime)
    workbench_test(AdminAccessTests test_adminaccess.cpp WorkbenchRuntime)
    workbench_test(DatabaseMaintenanceTests test_database_maintenance.cpp WorkbenchRuntime)
    workbench_test(MaintenanceWorkflowTests test_maintenance_workflow.cpp WorkbenchRuntime)
    if(WIN32)
        add_test(NAME LauncherPathTests COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_launcher_paths.ps1")
        set_tests_properties(LauncherPathTests PROPERTIES TIMEOUT 30)
        add_test(NAME InstallMigrationTests COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_install_migration.ps1")
        set_tests_properties(InstallMigrationTests PROPERTIES TIMEOUT 30)
    endif()

    # 收集需要配置无显示器图形平台的界面测试目标。
    set(workbench_gui_tests ModelTests WindowTests)
    if(WORKBENCH_ENABLE_SIMULATOR_TESTS)
        if(NOT EXISTS "${STATION_SIMULATOR_SOURCE_DIR}/CMakeLists.txt")
            message(FATAL_ERROR "Set STATION_SIMULATOR_SOURCE_DIR to the independent StationSimulator project.")
        endif()
        # Only optional integration tests import simulator code.
        add_subdirectory("${STATION_SIMULATOR_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/external/station-simulator")
        workbench_test(WindowSimulatorTests test_window_simulator.cpp
            WorkbenchWidgets StationSimulation)
        workbench_test(SimulatorWindowTests test_simulator_window.cpp
            StationSimulationWidgets StationSimulation WorkbenchWidgets)
        workbench_test(ProcessTests test_process.cpp WorkbenchRuntime)
        add_dependencies(ProcessTests StationSimulator)
        list(APPEND workbench_gui_tests WindowSimulatorTests SimulatorWindowTests)
    endif()

    qt_add_executable(HistoryBenchmark tests/benchmark_history.cpp)
    target_link_libraries(HistoryBenchmark PRIVATE WorkbenchRuntime)
    if(WIN32)
        target_link_libraries(HistoryBenchmark PRIVATE psapi)
    endif()

    set_tests_properties(WorkflowTests PROPERTIES TIMEOUT 900)
    set_tests_properties(${workbench_gui_tests}
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(WIN32)
        # 保存界面测试使用的 Windows 系统字体目录。
        file(TO_CMAKE_PATH "$ENV{WINDIR}/Fonts" systemFonts)
        set_tests_properties(${workbench_gui_tests}
            PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_FONTDIR=${systemFonts}")
    endif()
endif()
