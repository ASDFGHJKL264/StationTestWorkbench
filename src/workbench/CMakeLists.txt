add_library(WorkbenchApplication STATIC
    application/workbenchcontroller.h
    application/workbenchcontroller.cpp
)
target_link_libraries(WorkbenchApplication
    PUBLIC WorkbenchTesting
    PRIVATE WorkbenchStorage WorkbenchLogging
)

add_library(WorkbenchWidgets STATIC
    ui/mainwindow.h
    ui/mainwindow.cpp
    ui/models.h
    ui/models.cpp
)
target_link_libraries(WorkbenchWidgets
    PUBLIC WorkbenchApplication Qt6::Widgets
    PRIVATE WorkbenchToolStyle
)
