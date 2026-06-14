cmake_minimum_required(VERSION 3.20)
project(codenavigator VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CODENAVIGATOR_BUILD_TESTS "Build CodeNavigator tests" ON)

add_library(codenavigator_core
    src/engine.cpp
    src/json.cpp
)

target_include_directories(codenavigator_core PUBLIC include)
target_compile_features(codenavigator_core PUBLIC cxx_std_23)

if(MSVC)
    target_compile_options(codenavigator_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(codenavigator_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(codenavigator src/main.cpp)
target_link_libraries(codenavigator PRIVATE codenavigator_core)

if(CODENAVIGATOR_BUILD_TESTS)
    enable_testing()
    add_executable(codenavigator_tests tests/engine_test.cpp)
    target_link_libraries(codenavigator_tests PRIVATE codenavigator_core)
    add_test(NAME codenavigator_tests COMMAND codenavigator_tests)
endif()
