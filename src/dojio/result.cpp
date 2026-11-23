#include <dojio/result.hpp>

#include <fmt/format.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <system_error>
#include <variant>

namespace dojio {

std::string IoError::message() const {
  // std::error_code gives the same textual description as strerror(3), without
  // relying on a shared static buffer
  return std::error_code(-value, std::generic_category()).message();
}

namespace detail {

std::string describe(const ErrorCode& error) {
  if (const auto* io_error = std::get_if<IoError>(&error)) {
    return io_error->message();
  }
  return "operation cancelled";
}

void fail(std::string_view message, std::string_view detail) {
  fmt::print(stderr, "{}: {}\n", message, detail);
  std::exit(EXIT_FAILURE);
}

} // namespace detail

} // namespace dojio
