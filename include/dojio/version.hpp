#pragma once

#include <cstdint>
#include <tuple>

/// Library version query.

namespace dojio {

/// Returns the dojio version as a (major, minor, patch) tuple of 8-bit components.
[[nodiscard]] std::tuple<std::uint8_t, std::uint8_t, std::uint8_t> dojio_version() noexcept;

} // namespace dojio
