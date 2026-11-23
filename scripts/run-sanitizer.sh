#!/usr/bin/env bash
# Configure, build and run the dojio test suite under one sanitizer, using a
# pinned nixpkgs toolchain. The script exits non-zero as soon as the configure,
# the build or any test fails, so it can gate CI.
#
# usage: scripts/run-sanitizer.sh <address|undefined|memory>
#
# Environment (defaults match the ones pinned in Taskfile.yml and the CI):
#   NIXPKGS_REV            nixpkgs revision used by the address/undefined builds
#   NIXPKGS_SANITIZER_REV  nixpkgs revision used by the memory build (LLVM 19)

set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $(basename "$0") <address|undefined|memory>" >&2
  exit 2
fi

sanitizer="$1"
build_type="RelWithDebInfo"
nixpkgs_rev="${NIXPKGS_REV:-5646423bfac84ec68dfc60f2a322e627ef0d6a95}"
# LLVM 19 ships a libc++ that provides std::atomic_ref (libc++ 18 does not) and is
# the toolchain used by the MemorySanitizer build.
sanitizer_rev="${NIXPKGS_SANITIZER_REV:-50ab793786d9de88ee30ec4e4c24fb4236fc2674}"

run_address() {
  local build_dir="build/sanitizer-address"
  export build_dir build_type
  nix shell \
    "github:NixOS/nixpkgs/${nixpkgs_rev}#llvmPackages_18.clang" \
    "github:NixOS/nixpkgs/${nixpkgs_rev}#cmake" \
    "github:NixOS/nixpkgs/${nixpkgs_rev}#ninja" \
    "github:NixOS/nixpkgs/${nixpkgs_rev}#git" \
    "github:NixOS/nixpkgs/${nixpkgs_rev}#cacert" \
    --command sh -c '
      set -e
      cmake -G Ninja -S . -B "$build_dir" \
        -DCMAKE_BUILD_TYPE="$build_type" \
        -DDOJIO_SANITIZER_ADDRESS=ON \
        -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_COMPILER=clang++
      cmake --build "$build_dir"
      ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1:strict_string_checks=1:check_initialization_order=1:strict_init_order=1 \
        ctest --test-dir "$build_dir" --output-on-failure
    '
}

run_undefined() {
  local build_dir="build/sanitizer-undefined"
  export build_dir build_type
  nix shell \
    "github:NixOS/nixpkgs/${nixpkgs_rev}#llvmPackages_18.clang" \
    "github:NixOS/nixpkgs/${nixpkgs_rev}#cmake" \
    "github:NixOS/nixpkgs/${nixpkgs_rev}#ninja" \
    "github:NixOS/nixpkgs/${nixpkgs_rev}#git" \
    "github:NixOS/nixpkgs/${nixpkgs_rev}#cacert" \
    --command sh -c '
      set -e
      cmake -G Ninja -S . -B "$build_dir" \
        -DCMAKE_BUILD_TYPE="$build_type" \
        -DDOJIO_SANITIZER_UNDEFINED=ON \
        -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_COMPILER=clang++
      cmake --build "$build_dir"
      UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
        ctest --test-dir "$build_dir" --output-on-failure
    '
}

run_memory() {
  local build_dir="build/sanitizer-memory"
  local libcxx
  libcxx="$(nix build --no-link --print-out-paths "github:NixOS/nixpkgs/${sanitizer_rev}#llvmPackages_19.libcxx")"
  export build_dir build_type libcxx
  nix shell \
    "github:NixOS/nixpkgs/${sanitizer_rev}#llvmPackages_19.libcxxClang" \
    "github:NixOS/nixpkgs/${sanitizer_rev}#cmake" \
    "github:NixOS/nixpkgs/${sanitizer_rev}#ninja" \
    "github:NixOS/nixpkgs/${sanitizer_rev}#git" \
    "github:NixOS/nixpkgs/${sanitizer_rev}#cacert" \
    --command sh -c '
      set -e
      cmake -G Ninja -S . -B "$build_dir" \
        -DCMAKE_BUILD_TYPE="$build_type" \
        -DDOJIO_SANITIZER_MEMORY=ON \
        -DCMAKE_CXX_FLAGS="-L$libcxx/lib" \
        -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_COMPILER=clang++
      cmake --build "$build_dir"
      LD_LIBRARY_PATH="$libcxx/lib" \
      MSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:track_origins=2 \
        ctest --test-dir "$build_dir" --output-on-failure
    '
}

case "${sanitizer}" in
  address)
    run_address
    ;;
  undefined)
    run_undefined
    ;;
  memory)
    run_memory
    ;;
  *)
    echo "unknown sanitizer: ${sanitizer} (expected address, undefined or memory)" >&2
    exit 2
    ;;
esac
