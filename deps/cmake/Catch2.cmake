function(featheros_fetch_catch2)
    include(FetchContent)

    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.7.1
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(Catch2)
    FetchContent_GetProperties(Catch2 SOURCE_DIR catch2_source_dir)
    set(FEATHEROS_CATCH2_SOURCE_DIR "${catch2_source_dir}" PARENT_SCOPE)
endfunction()