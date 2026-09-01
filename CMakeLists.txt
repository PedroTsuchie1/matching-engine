cmake_minimum_required(VERSION 3.20)

project(MatchingEngine LANGUAGES CXX)

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
