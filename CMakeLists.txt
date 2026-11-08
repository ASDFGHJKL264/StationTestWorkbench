cmake_minimum_required(VERSION 3.19)
project(StationTestWorkbench VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Network Sql Widgets)
include(CTest)
option(WORKBENCH_ENABLE_SIMULATOR_TESTS "Build optional integration tests with StationSimulator" OFF)
set(STATION_SIMULATOR_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../StationSimulator"
    CACHE PATH "Independent StationSimulator source directory (integration tests only)")

# Keep launch scripts, portable packaging and process tests on the same executable paths.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_subdirectory(src)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
install(TARGETS StationTestWorkbench RUNTIME DESTINATION .)
