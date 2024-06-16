add_library(tar INTERFACE)
target_include_directories(tar INTERFACE ${CMAKE_CURRENT_LIST_DIR})
target_sources(tar INTERFACE
    tar.c tar.h
)
