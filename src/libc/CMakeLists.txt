set(CMAKE_CXX_FLAGS "")

add_library(featheros_c STATIC
    address_text.cpp
    arc4random.cpp
    ctype.cpp
    compiler_runtime.cpp
    cxx_runtime.cpp
    dirent.cpp
    dns_message.cpp
    hosts_file.cpp
    resolv_conf.cpp
    environment.cpp
    errno.cpp
    exit.cpp
    featheros.cpp
    inet.cpp
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
    pseudo_terminal.cpp
    termios.cpp
    malloc.cpp
    netdb.cpp
    poll.cpp
    pthread.cpp
    regex.cpp
    regex_engine.cpp
    thread_keys.cpp
    signal.cpp
    socket.cpp
    math.cpp
    spawn.cpp
    stat.cpp
    stdio.cpp
    stdlib.cpp
    string.cpp
    time.cpp
    uchar.cpp
    unicode_conversion.cpp
    unistd.cpp
    wide_division.cpp
    arch/${ARCH}/setjmp.cpp
    arch/${ARCH}/signal_return.cpp
)
if (ARCH STREQUAL "x86_64")
    target_sources(featheros_c PRIVATE arch/x86_64/long_double.cpp)
endif ()
# Program startup (_start, which calls main) is linked only into programs that have a main.
add_library(featheros_crt0 OBJECT start.cpp)

foreach (target featheros_c featheros_crt0)
    target_compile_options(${target} PRIVATE ${FEATHEROS_USER_FLAGS})
    target_include_directories(${target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(${target} PUBLIC featheros_cxx_headers)
endforeach ()
