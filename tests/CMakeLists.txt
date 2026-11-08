find_package(Qt6 REQUIRED COMPONENTS Test)

function(workbench_test name file)
    add_executable(${name} ${file})
    target_link_libraries(${name} PRIVATE ${ARGN} Qt6::Test)
    add_test(NAME ${name} COMMAND ${name} -o ${name}-results.txt,txt)
    # Preserve result paths consumed by verify.ps1 and keep process fixtures beside the apps.
    set_tests_properties(${name} PROPERTIES TIMEOUT 90 WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
endfunction()

workbench_test(ProtocolTests shared/test_protocol.cpp WorkbenchProtocol)
workbench_test(DomainTests testing/test_domain.cpp WorkbenchDomain)
workbench_test(RepositoryTests traceability/test_repository.cpp WorkbenchStorage)
workbench_test(ModelTests workbench/test_models.cpp WorkbenchWidgets)
workbench_test(WorkflowTests testing/test_workflow.cpp
    WorkbenchApplication WorkbenchStorage)
workbench_test(WindowTests workbench/test_window.cpp
    WorkbenchWidgets WorkbenchStorage)
workbench_test(LoggerTests shared/test_logger.cpp WorkbenchLogging)

set(workbench_gui_tests ModelTests WindowTests)
if(WORKBENCH_ENABLE_SIMULATOR_TESTS)
    if(NOT EXISTS "${STATION_SIMULATOR_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "Set STATION_SIMULATOR_SOURCE_DIR to the independent StationSimulator project.")
    endif()
    # Only optional integration tests import simulator libraries; the application does not.
    add_subdirectory("${STATION_SIMULATOR_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/external/station-simulator")
    workbench_test(WindowSimulatorTests integration/test_window_simulator.cpp
        WorkbenchWidgets WorkbenchStorage StationSimulation)
    workbench_test(SimulatorWindowTests integration/test_simulator_window.cpp
        StationSimulationWidgets StationSimulation WorkbenchWidgets)
    workbench_test(ProcessTests integration/test_process.cpp WorkbenchApplication)
    add_dependencies(ProcessTests StationSimulator)
    list(APPEND workbench_gui_tests WindowSimulatorTests SimulatorWindowTests)
endif()

add_executable(HistoryBenchmark traceability/benchmark_history.cpp)
target_link_libraries(HistoryBenchmark PRIVATE WorkbenchStorage)
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
