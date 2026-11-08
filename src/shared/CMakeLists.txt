add_library(WorkbenchProtocol STATIC protocol/framecodec.h protocol/framecodec.cpp)
target_include_directories(WorkbenchProtocol PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(WorkbenchProtocol PUBLIC Qt6::Core)

add_library(WorkbenchLogging STATIC logging/asyncfilelogger.h logging/asyncfilelogger.cpp)
target_include_directories(WorkbenchLogging PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(WorkbenchLogging PUBLIC Qt6::Core)

add_library(WorkbenchToolStyle INTERFACE)
target_sources(WorkbenchToolStyle INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/widgets/toolstyle.h")
target_include_directories(WorkbenchToolStyle INTERFACE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(WorkbenchToolStyle INTERFACE Qt6::Widgets)
