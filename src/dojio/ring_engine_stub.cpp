// Non-Linux stub of the RingEngine: io_uring only exists on Linux, so create()
// always fails with a NotSupported DojioError and no engine can be instantiated.

#include <dojio/ring_engine.hpp>

#include <dojio/contracts.hpp>

#include <fmt/format.h>

#include <string_view>

#include <dojio/io_uring_stub.hpp>

namespace dojio {

namespace {

DojioError not_supported_error(std::string_view what) {
  return DojioError{ErrorOpcode::NotSupported,
                    fmt::format("dojio: {} is not supported on this platform (io_uring requires Linux)", what)};
}

} // namespace

tl::expected<std::unique_ptr<RingEngine>, DojioError> RingEngine::create(RingEngineOptions) {
  return tl::unexpected(not_supported_error("RingEngine"));
}

RingEngine::RingEngine(std::unique_ptr<detail::IoUring> ring) : ring_(std::move(ring)), pending_(&pool_resource_) {}

RingEngine::~RingEngine() = default;

bool RingEngine::cancel(OperationId) {
  unreachable();
  // cppcheck-suppress unreachableCode ; MISRA C++ deviation (Rule 0.0.1): defensive return after unreachable()
  return false;
}

void RingEngine::process(ProcessMode) { unreachable(); }

std::uint64_t RingEngine::count_operations(OperationState) const {
  unreachable();
  // cppcheck-suppress unreachableCode ; MISRA C++ deviation (Rule 0.0.1): defensive return after unreachable()
  return 0;
}

std::int32_t RingEngine::ring_fd() const {
  unreachable();
  // cppcheck-suppress unreachableCode ; MISRA C++ deviation (Rule 0.0.1): defensive return after unreachable()
  return -1;
}

bool RingEngine::sqpoll_enabled() const {
  unreachable();
  // cppcheck-suppress unreachableCode ; MISRA C++ deviation (Rule 0.0.1): defensive return after unreachable()
  return false;
}

std::chrono::milliseconds RingEngine::sqpoll_poll_time() const {
  unreachable();
  // cppcheck-suppress unreachableCode ; MISRA C++ deviation (Rule 0.0.1): defensive return after unreachable()
  return std::chrono::milliseconds{0};
}

} // namespace dojio
