set(CMAKE_CXX_FLAGS "")

add_library(featheros_c STATIC
    ctype.cpp
    cxx_runtime.cpp
    environment.cpp
    errno.cpp
    exit.cpp
    stat.cpp
    stdlib.cpp
    string.cpp
    unistd.cpp
    arch/${ARCH}/setjmp.cpp
)
# Program startup (_start, which calls main) is linked only into programs that have a main.
add_library(featheros_crt0 OBJECT start.cpp)

foreach (target featheros_c featheros_crt0)
    # Built at -O0 like the programs. With optimization, GCC also needs
    # -fno-tree-loop-distribute-patterns here, or it can turn the loop inside memset into a call
    # to memset.
    target_compile_options(${target} PRIVATE ${FEATHEROS_USER_FLAGS})
    target_include_directories(${target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    set_target_properties(${target} PROPERTIES CXX_STANDARD 17)
endforeach ()
