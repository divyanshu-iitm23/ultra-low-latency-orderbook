#pragma once
#include <cstdint>
// non-serializing timestamp read. using __rdtsc(),
#if defined(__x86_64__) || defined(__i386__)
#  include <x86intrin.h>
namespace metrics { inline uint64_t rdtsc() { return __rdtsc(); } }
#else
#  include <chrono>
namespace metrics { inline uint64_t rdtsc() {
    return (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count(); } }
#endif
