
## Code style instructions
- Follow `misc/CODE_GUIDELINES.md`
- Use '//' comment style and '///' for documentations
- Use `pragma once` instead of include guards
- Respect the existing namespace hierarchy
- never use `using namespace` in C++
- Type need to be written in CamelCase
- functions and variable names are in snakecase
- do not use pre-increment and post-increment syntax outside of for loops
- Use fixed-size integer types everywhere (`std::int32_t`, `std::uint64_t`, ...). Platform-dependent
  integer types (`int`, `long`, `unsigned`, `short`, `std::size_t`, ...) are only allowed at C API call
  boundaries (syscalls, POSIX functions, kernel structs, `main`'s signature), with an explicit cast
- Never use `printf`/`fprintf`/`iostream` for output, and use libfmt (`fmt::format`, `fmt::print`) for
  all formatted output and string building; fmt is used in header-only mode inside the library
- For the rest, in C++ use a style similar to the CPP core guidelines

## Compilation instructions
 - Create build directory `mkdir -p build-agent`
 - Run `cd build-agent && cmake -G Ninja ../ && ninja -v`


## Test instructions
 - Run `ctest -V`

## Formatting instructions
- Run `task format`


