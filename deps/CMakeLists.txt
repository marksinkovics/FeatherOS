set(DEPS_SOURCE_DIR ${PROJECT_SOURCE_DIR}/deps)

add_library(limine INTERFACE)
target_include_directories(limine INTERFACE ${DEPS_SOURCE_DIR}/limine)
