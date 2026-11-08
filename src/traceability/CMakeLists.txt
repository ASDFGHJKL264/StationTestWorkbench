# A single database owner keeps reservation, terminal and recovery transactions coherent.
add_library(WorkbenchStorage STATIC repository.h repository.cpp)
target_link_libraries(WorkbenchStorage PUBLIC WorkbenchDomain Qt6::Sql)
