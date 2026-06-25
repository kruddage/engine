add_library(log log.c)
target_include_directories(log PUBLIC . ${CMAKE_SOURCE_DIR}/plugins/include)
