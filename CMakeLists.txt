cmake_minimum_required(VERSION 3.20)

project(MatchingEngine LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)

add_library(matching_engine_core STATIC
    src/order.cpp
)

target_include_directories(matching_engine_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(matching_engine_core PUBLIC cxx_std_20)

set_target_properties(matching_engine_core PROPERTIES
    CXX_EXTENSIONS OFF
)

target_compile_options(matching_engine_core PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_executable(matching_engine
    src/main.cpp
)

target_link_libraries(matching_engine PRIVATE matching_engine_core)

target_compile_features(matching_engine PRIVATE cxx_std_20)

set_target_properties(matching_engine PROPERTIES
    CXX_EXTENSIONS OFF
)

target_compile_options(matching_engine PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/order_test.cpp")
    include(FetchContent)

    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )

    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_executable(matching_engine_tests
        tests/order_test.cpp
    )

    target_link_libraries(matching_engine_tests PRIVATE
        matching_engine_core
        GTest::gtest_main
    )

    target_compile_features(matching_engine_tests PRIVATE cxx_std_20)

    set_target_properties(matching_engine_tests PROPERTIES
        CXX_EXTENSIONS OFF
    )

    target_compile_options(matching_engine_tests PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )

    include(GoogleTest)
    gtest_discover_tests(matching_engine_tests)
endif()
