cmake_minimum_required(VERSION 3.20)

project(MatchingEngine LANGUAGES CXX)

add_executable(matching_engine
    src/main.cpp
)

target_compile_features(matching_engine PRIVATE cxx_std_20)

set_target_properties(matching_engine PROPERTIES
    CXX_EXTENSIONS OFF
)

target_compile_options(matching_engine PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
