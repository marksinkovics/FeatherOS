set(CMAKE_CXX_FLAGS "")

# What the C++ library's headers do not define: std::string's members (basic_string<char> is
# instantiated here once, and declared extern in <string>), the sto* and to_string functions, the
# floating-point <charconv> conversions, which use the C library's float engine, the red-black
# tree's rebalancing behind map and set, the clocks' now(), the threads, mutexes, condition
# variables and stop states over pthreads, the futex words that atomic waits sleep on, the
# formatting loop with the standard types' formatters behind <format> and <print>, and the
# conversions (%Y, %H, ...) behind <chrono>'s formatters. Programs link it after their own code
# and before featheros_c.
add_library(featheros_cxx STATIC
    atomic_wait.cpp
    charconv.cpp
    chrono.cpp
    chrono_format.cpp
    condition_variable.cpp
    exception.cpp
    format.cpp
    mutex.cpp
    print.cpp
    stop_token.cpp
    string.cpp
    thread.cpp
    tree.cpp
)
target_compile_options(featheros_cxx PRIVATE ${FEATHEROS_USER_FLAGS})
target_include_directories(featheros_cxx PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(featheros_cxx PUBLIC featheros_c)
