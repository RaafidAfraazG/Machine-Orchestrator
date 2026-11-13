#pragma once
#include <chrono>
namespace mo::test {
using namespace std::chrono_literals;
inline constexpr auto ScenarioDeadline = 15s;
inline constexpr auto RequestTimeout = 150ms;
inline constexpr auto ProbeTimeout = 200ms;
inline constexpr auto InitialBackoff = 100ms;
inline constexpr auto MaxBackoff = 400ms;
inline constexpr auto SlowSuccess = 20ms;
inline constexpr auto SlowFailure = 600ms;
inline constexpr auto LongDelay = 60s;
} // namespace mo::test
