#include <dojio/result.hpp>

#include <cerrno>
#include <system_error>

namespace dojio {

std::string IoError::message() const {
  // std::error_code gives the same textual description as strerror(3), without
  // relying on a shared static buffer
  return std::error_code(-value, std::generic_category()).message();
}

} // namespace dojio
