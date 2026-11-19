#include <dojio/result.hpp>

#include <fmt/format.h>

#include <cerrno>
#include <cstring>

namespace dojio {

std::string IoError::message() const {
  // strerror(3) is the C API for the message; fmt does the string formatting
  return fmt::format("{}", std::strerror(-value));
}

} // namespace dojio
