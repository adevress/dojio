#include <dojio/contracts.hpp>

#include <fmt/format.h>

#include <stdexcept>
#include <string>

namespace dojio {
namespace intern {

void log_and_throw(const std::source_location& loc) {
  const std::string err = fmt::format("{}:{}:{}: [BUG] contract failure within function '{}'\n", loc.file_name(),
                                      loc.line(), loc.column(), loc.function_name());
  fmt::print(stderr, "{}", err);
  throw std::logic_error(err);
}

} // namespace intern
} // namespace dojio
