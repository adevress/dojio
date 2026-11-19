#pragma once

// Private header used on non-Linux platforms: it stands in for the Linux-only
// ring.hpp so the pimpl member of RingEngine stays a complete type. None of the
// dojio public headers include it.

namespace dojio::detail {

/// Stub of the io_uring ring wrapper. io_uring only exists on Linux, so the ring
/// is never created and every method of this class is unreachable.
class IoUring {};

} // namespace dojio::detail
