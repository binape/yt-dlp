cmake_minimum_required(VERSION 3.20)
project(SmartQuant VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(smartquant STATIC
    src/SmartQuant.cpp
)

set_target_properties(smartquant PROPERTIES
    OUTPUT_NAME smartquant
)

target_include_directories(smartquant
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

option(SMARTQUANT_BUILD_TESTS "Build SmartQuant unit tests" ON)
if (SMARTQUANT_BUILD_TESTS)
    enable_testing()
    add_executable(smartquant_tests tests/test_main.cpp)
    target_link_libraries(smartquant_tests PRIVATE smartquant)
    add_test(NAME smartquant_tests COMMAND smartquant_tests)
endif()
