#include <doctest/doctest.h>

#include <cstdint>
#include <tuple>

#include <dojio/version.hpp>

TEST_CASE("dojio_version reports the packaged version") {
  const std::tuple<std::uint8_t, std::uint8_t, std::uint8_t> version = dojio::dojio_version();
  CHECK(std::get<0>(version) == 0);
  CHECK(std::get<1>(version) == 2);
  CHECK(std::get<2>(version) == 0);
}
