cmake_minimum_required(VERSION 3.14)
project(UltraLowLatencyOrderBook VERSION 1.0)

# C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compiler flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")

# Include directories
include_directories(include)

# Source files
set(SOURCES
    src/orderbook.cpp
)

# Main library
add_library(orderbook_lib ${SOURCES})

# Tests
enable_testing()
find_package(GTest REQUIRED)
include_directories(${GTEST_INCLUDE_DIRS})

add_executable(test_orderbook tests/test_orderbook.cpp)
target_link_libraries(test_orderbook orderbook_lib ${GTEST_LIBRARIES} pthread)

add_test(NAME OrderBookTests COMMAND test_orderbook)

# Manual test
add_executable(manual_test tests/manual_test.cpp)
target_link_libraries(manual_test orderbook_lib)

# RDTSC benchmark (optimized build)
add_executable(benchmark_orderbook benchmarks/benchmark_orderbook.cpp)
target_link_libraries(benchmark_orderbook orderbook_lib)
target_compile_options(benchmark_orderbook PRIVATE -O3 -march=native)


# ITCH tools
add_executable(itch_replay benchmarks/itch_replay.cpp)
target_compile_options(itch_replay PRIVATE -O3 -march=native)

add_executable(itch_book_replay benchmarks/itch_book_replay.cpp)
target_link_libraries(itch_book_replay orderbook_lib)
target_compile_options(itch_book_replay PRIVATE -O3 -march=native)