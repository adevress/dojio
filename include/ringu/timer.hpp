#pragma once

#include <chrono>

namespace ringu {

/// Typed handler for a timer request.
///
/// Submitting a Timer to a Reactor arms an io_uring timeout: the associated
/// action completes successfully once the duration has elapsed.
class Timer {
public:
  using Duration = std::chrono::nanoseconds;

  constexpr Timer() = default;

  template <typename Rep, typename Period>
  constexpr Timer(std::chrono::duration<Rep, Period> duration)
      : duration_(std::chrono::duration_cast<Duration>(duration)) {}

  /// Requested timeout.
  [[nodiscard]] constexpr Duration duration() const { return duration_; }

private:
  Duration duration_{};
};

} // namespace ringu
