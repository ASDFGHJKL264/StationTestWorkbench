add_library(WorkbenchDomain STATIC domain.h domain.cpp stationendpoint.h)
target_include_directories(WorkbenchDomain PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(WorkbenchDomain PUBLIC Qt6::Core)

# The engine emits persistence requests; it never includes storage or UI controllers.
add_library(WorkbenchTesting STATIC testengine.h testengine.cpp)
target_link_libraries(WorkbenchTesting PUBLIC WorkbenchDomain WorkbenchProtocol Qt6::Network)
