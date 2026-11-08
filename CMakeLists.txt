cmake_minimum_required(VERSION 3.19)
project(StationTestWorkbench VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTORCC ON)
include(CTest)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Network Sql Widgets)
qt_standard_project_setup()

option(WORKBENCH_ENABLE_SIMULATOR_TESTS "Build optional integration tests with StationSimulator" OFF)
set(STATION_SIMULATOR_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../StationSimulator"
    CACHE PATH "Independent StationSimulator source directory (integration tests only)")

# Keep launch scripts, portable packaging and process tests on the same executable paths.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

qt_add_library(WorkbenchRuntime STATIC
    domain.h domain.cpp
    stationendpoint.h
    framecodec.h framecodec.cpp
    asyncfilelogger.h asyncfilelogger.cpp
    testengine.h testengine.cpp
    repository.h repository.cpp
    workbenchcontroller.h workbenchcontroller.cpp)
target_include_directories(WorkbenchRuntime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(WorkbenchRuntime PUBLIC Qt6::Core Qt6::Network Qt6::Sql)

qt_add_library(WorkbenchWidgets STATIC
    mainwindow.h mainwindow.cpp
    models.h models.cpp
    toolstyle.h)
target_link_libraries(WorkbenchWidgets PUBLIC WorkbenchRuntime Qt6::Widgets)

qt_add_executable(StationTestWorkbench WIN32 MACOSX_BUNDLE main.cpp)
target_link_libraries(StationTestWorkbench PRIVATE WorkbenchWidgets)

install(TARGETS StationTestWorkbench BUNDLE DESTINATION . RUNTIME DESTINATION .)

if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)

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
        file(TO_CMAKE_PATH "$ENV{WINDIR}/Fonts" systemFonts)
        set_tests_properties(${workbench_gui_tests}
            PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_FONTDIR=${systemFonts}")
    endif()
endif()
