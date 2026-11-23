#pragma once

// Private header: the low-level syscall tracing of the library. It is controlled by
// the DOJIO_SYS_DEBUGGING CMake option (OFF by default). When the macro is defined,
// detail::dojio_debug() writes to stderr; otherwise it compiles to a no-op that is
// optimized away and never reaches the binary.
//
// This header is private because it exposes fmt types: fmt stays an implementation
// detail and must not appear in include/dojio/.

#include <cstdio>
#include <utility>

#include <fmt/format.h>

namespace dojio::detail {

#if defined(DOJIO_SYS_DEBUGGING)

/// Logs a formatted message to stderr. Accepts exactly what fmt::print(stderr, ...)
/// accepts: a compile-time format string followed by its arguments.
template <typename... Args> void dojio_debug(fmt::format_string<Args...> format, Args&&... args) {
  fmt::print(stderr, format, std::forward<Args>(args)...);
}

#else

/// Debugging build disabled: accepts the same arguments as the enabled variant and
/// does nothing.
template <typename... Args> void dojio_debug(fmt::format_string<Args...>, Args&&...) noexcept {}

#endif

} // namespace dojio::detail
