set(DEPS_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

add_library(limine INTERFACE)
target_include_directories(limine INTERFACE ${DEPS_SOURCE_DIR}/limine)
