#include <dojio/version.hpp>

#include <dojio/version_config.hpp>

namespace dojio {

std::tuple<std::uint8_t, std::uint8_t, std::uint8_t> dojio_version() noexcept {
  return {detail::version_major, detail::version_minor, detail::version_patch};
}

} // namespace dojio
