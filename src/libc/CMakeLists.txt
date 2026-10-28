set(CMAKE_CXX_FLAGS "")

add_library(featheros_c STATIC
    ctype.cpp
    cxx_runtime.cpp
    dirent.cpp
    environment.cpp
    errno.cpp
    exit.cpp
    featheros.cpp
    ioctl.cpp
    lock.cpp
    big_integer.cpp
    environment_table.cpp
    float_bits.cpp
    float_format.cpp
    float_parse.cpp
    format.cpp
    heap_allocator.cpp
    heap_sort.cpp
    math_functions.cpp
    scan.cpp
    spawn_request.cpp
    time_format.cpp
    ../user/calendar.cpp
    termios.cpp
    malloc.cpp
    pthread.cpp
    signal.cpp
    math.cpp
    spawn.cpp
    stat.cpp
    stdio.cpp
    stdlib.cpp
    string.cpp
    time.cpp
    unistd.cpp
    arch/${ARCH}/setjmp.cpp
    arch/${ARCH}/signal_return.cpp
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
endforeach ()
