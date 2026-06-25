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
include_directories(metrics/include)

# Phase-2 metrics instrumentation switch
# ON  : ScopedLatency is compiled into the engine (add/cancel/modify timed).
# OFF : METRICS_ENABLED=0 : instrumentation compiles to nothing (clean baseline).
option(ENABLE_METRICS "Compile in latency metrics instrumentation" ON)
if(ENABLE_METRICS)
    add_compile_definitions(METRICS_ENABLED=1)
else()
    add_compile_definitions(METRICS_ENABLED=0)
endif()

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

# Matching Engine testing..
add_executable(test_matching test_matching.cpp)
target_link_libraries(test_matching orderbook_lib)

# Phase-2 metrics pipeline
# SPSC ring torture test is independent of the METRICS_ENABLED switch.
add_executable(test_spsc metrics/test_spsc.cpp)
target_link_libraries(test_spsc pthread)

if(ENABLE_METRICS)
    # event-flow / accounting check for the recorder + ring
    add_executable(test_instrument metrics/test_instrument.cpp)
    target_link_libraries(test_instrument pthread)

    # deterministic test that the engine wiring fires one typed event per op
    add_executable(test_engine_wiring metrics/test_engine_wiring.cpp)
    target_link_libraries(test_engine_wiring orderbook_lib pthread)
    add_test(NAME EngineWiring COMMAND test_engine_wiring)

    # synthetic producer + aggregator live readout
    add_executable(aggregator_demo metrics/aggregator_demo.cpp)
    target_link_libraries(aggregator_demo pthread)
    target_compile_options(aggregator_demo PRIVATE -O3 -march=native)

    # the real wiring: metrics-instrumented OrderBook driven live (synthetic churn)
    add_executable(engine_metrics_demo metrics/engine_metrics_demo.cpp)
    target_link_libraries(engine_metrics_demo orderbook_lib pthread)
    target_compile_options(engine_metrics_demo PRIVATE -O3 -march=native)

    # same, but the producer is a REAL NASDAQ ITCH feed via BookReplay
    add_executable(itch_metrics_replay metrics/itch_metrics_replay.cpp)
    target_link_libraries(itch_metrics_replay orderbook_lib pthread)
    target_compile_options(itch_metrics_replay PRIVATE -O3 -march=native)
endif()