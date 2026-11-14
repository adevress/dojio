#pragma once

#include <cstddef>
#include <iterator>

// std::ssize_t is required by C++20 ([iterator.syn]), but libstdc++ only provides
// it from GCC 14. When the standard library does not define it yet, provide the
// standard definition ourselves. A repeated alias-declaration naming the same
// type is legal, so this also stays correct on newer libstdc++ versions.
#if defined(__GLIBCXX__) && __GNUC__ < 14
namespace std {
using ssize_t = ptrdiff_t;
} // namespace std
#endif
