# FeatherOS's C++ standard library. Its headers are a system include directory, as a compiler's
# own library would be: warnings inside them are not reported where they are used, and the unit
# tests check them with warnings on instead.
add_library(featheros_cxx_headers INTERFACE)
target_include_directories(featheros_cxx_headers SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
