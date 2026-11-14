#include <doctest/doctest.h>

#include <cerrno>
#include <cstdint>
#include <string>
#include <variant>

#include <ringu/result.hpp>

TEST_CASE("Result carries a value on success") {
  ringu::Result<std::int32_t> r = 42;
  CHECK(r.has_value());
  CHECK(r.value() == 42);
  CHECK(*r == 42);
}

TEST_CASE("Result carries the mapped I/O code on failure") {
  ringu::Result<std::int32_t> r = tl::unexpected(ringu::IoError{-ENOENT});
  CHECK_FALSE(r.has_value());
  const ringu::ErrorCode& err = r.error();
  CHECK(std::holds_alternative<ringu::IoError>(err));
  CHECK(std::get<ringu::IoError>(err).value == -ENOENT);
  CHECK(std::get<ringu::IoError>(err).message() == std::string{"No such file or directory"});
}

TEST_CASE("Result carries the cancel code as a variant alternative") {
  ringu::Result<std::int32_t> r = tl::unexpected(ringu::Cancelled{});
  CHECK_FALSE(r.has_value());
  CHECK(std::holds_alternative<ringu::Cancelled>(r.error()));
}

TEST_CASE("Result can carry a registered item (buffer)") {
  std::vector<std::byte> buf(4);
  ringu::Result<std::vector<std::byte>> r = std::move(buf);
  REQUIRE(r.has_value());
  CHECK(r->size() == 4);
}
