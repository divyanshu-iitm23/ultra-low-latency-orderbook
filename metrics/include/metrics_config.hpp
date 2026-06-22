#pragma once
#include "spsc_ring.hpp"
#include "metrics_event.hpp"
namespace metrics {
// defining ring depth, 64K events (~2 MB) gives a good burst tolerance;
constexpr std::size_t METRICS_RING_CAP = 1u << 16;
using EventRing = SPSCRing<MetricsEvent, METRICS_RING_CAP>;
}
