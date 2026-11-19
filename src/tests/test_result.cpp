#include <doctest/doctest.h>

#include <cerrno>
#include <cstdint>
#include <string>
#include <variant>

#include <dojio/result.hpp>

TEST_CASE("Result carries a value on success") {
  dojio::Result<std::int32_t> r = 42;
  CHECK(r.has_value());
  CHECK(r.value() == 42);
  CHECK(*r == 42);
}

TEST_CASE("Result carries the mapped I/O code on failure") {
  dojio::Result<std::int32_t> r = tl::unexpected(dojio::IoError{-ENOENT});
  CHECK_FALSE(r.has_value());
  const dojio::ErrorCode& err = r.error();
  CHECK(std::holds_alternative<dojio::IoError>(err));
  CHECK(std::get<dojio::IoError>(err).value == -ENOENT);
  CHECK(std::get<dojio::IoError>(err).message() == std::string{"No such file or directory"});
}

TEST_CASE("Result carries the cancel code as a variant alternative") {
  dojio::Result<std::int32_t> r = tl::unexpected(dojio::Cancelled{});
  CHECK_FALSE(r.has_value());
  CHECK(std::holds_alternative<dojio::Cancelled>(r.error()));
}

TEST_CASE("Result can carry a registered item (buffer)") {
  std::vector<std::byte> buf(4);
  dojio::Result<std::vector<std::byte>> r = std::move(buf);
  REQUIRE(r.has_value());
  CHECK(r->size() == 4);
}
