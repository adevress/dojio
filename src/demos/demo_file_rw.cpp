// Serial vs concurrent file-write demo.
//
// Creates <base>/test_demo_file_rw/serial/ with 40 files of 4 MiB using classical
// blocking POSIX open/write/fsync/close, one file after another. Then creates
// <base>/test_demo_file_rw/async/ with the same 40 files by submitting the opens,
// writes, fsyncs and closes to a single dojio::RingEngine so that all 40 files
// progress concurrently. Every file is written from its own 4 MiB buffer, the 40
// buffers being pre-filled with a deterministic Mersenne twister. The demo reports
// the wall-clock time of both runs and removes the test directory afterwards.
//
// usage: demo_file_rw [base_directory=/tmp]

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio> // for stderr
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include <dojio/dojio.hpp>

namespace {

/// Number of files written in each directory.
inline constexpr std::int32_t k_file_count = 40;
/// Size of one file (and of its payload buffer): 4 MiB.
inline constexpr std::uint64_t k_file_size = 4ULL * 1024ULL * 1024ULL;
/// Fixed seed so that the payload of every run is reproducible.
inline constexpr std::uint32_t k_random_seed = 0x5eed1234U;

/// Fills a buffer with bytes drawn from a Mersenne twister, four bytes at a time.
void fill_random(std::vector<std::byte>& buffer, std::mt19937& rng) {
  const std::uint64_t size = static_cast<std::uint64_t>(buffer.size());
  for (std::uint64_t offset = 0; offset + sizeof(std::uint32_t) <= size; offset += sizeof(std::uint32_t)) {
    const std::uint32_t word = static_cast<std::uint32_t>(rng());
    std::memcpy(buffer.data() + static_cast<std::size_t>(offset), &word, sizeof(word));
  }
}

/// Writes the whole buffer with blocking write(2) calls, looping on short writes.
bool write_all(std::int32_t fd, const std::vector<std::byte>& buffer) {
  const std::uint64_t size = static_cast<std::uint64_t>(buffer.size());
  std::uint64_t written = 0;
  while (written < size) {
    const std::int64_t n = static_cast<std::int64_t>(
        ::write(fd, buffer.data() + static_cast<std::size_t>(written), static_cast<std::size_t>(size - written)));
    if (n <= 0) {
      return false;
    }
    written += static_cast<std::uint64_t>(n);
  }
  return true;
}

/// Removes path if it exists, then recreates it as an empty directory.
bool reset_directory(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::remove_all(path, error);
  return std::filesystem::create_directories(path, error);
}

/// Name of the index-th file of a run.
std::string file_name(std::int32_t index) { return fmt::format("file_{:04d}.bin", index); }

/// Writes the k_file_count files sequentially with blocking POSIX calls and
/// returns the elapsed wall-clock time.
std::chrono::nanoseconds run_serial(const std::filesystem::path& directory,
                                    const std::vector<std::vector<std::byte>>& buffers) {
  const auto start = std::chrono::steady_clock::now();
  for (std::int32_t i = 0; i < k_file_count; ++i) {
    const std::string path = (directory / file_name(i)).string();
    const std::int32_t fd =
        static_cast<std::int32_t>(::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, static_cast<mode_t>(0644)));
    if (fd < 0) {
      fmt::print(stderr, "serial: open {}: {}\n", path, std::error_code(errno, std::system_category()).message());
      std::exit(1);
    }
    if (!write_all(fd, buffers[static_cast<std::size_t>(i)])) {
      fmt::print(stderr, "serial: write {}: {}\n", path, std::error_code(errno, std::system_category()).message());
      ::close(fd);
      std::exit(1);
    }
    if (::fsync(fd) != 0) {
      fmt::print(stderr, "serial: fsync {}: {}\n", path, std::error_code(errno, std::system_category()).message());
      ::close(fd);
      std::exit(1);
    }
    if (::close(fd) != 0) {
      fmt::print(stderr, "serial: close {}: {}\n", path, std::error_code(errno, std::system_category()).message());
      std::exit(1);
    }
  }
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start);
}

/// Writes the k_file_count files concurrently through one RingEngine: every file is
/// opened, then its whole payload buffer is written at offset 0, then fsynced and
/// closed, with the 40 per-file chains progressing in parallel. Returns the elapsed
/// wall-clock time.
std::chrono::nanoseconds run_async(const std::filesystem::path& directory,
                                   std::vector<std::vector<std::byte>>& buffers) {
  auto engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 256});
  if (!engine_result.has_value()) {
    fmt::print(stderr, "async: ring engine: {}\n", engine_result.error().message);
    std::exit(1);
  }
  auto engine = std::move(engine_result).value();

  std::int32_t completed = 0;
  std::int32_t failed = 0;
  const auto flags = static_cast<std::int32_t>(O_WRONLY | O_CREAT | O_TRUNC);
  const auto mode = static_cast<std::uint32_t>(0644);

  const auto start = std::chrono::steady_clock::now();
  for (std::int32_t i = 0; i < k_file_count; ++i) {
    const std::string path = (directory / file_name(i)).string();
    engine->submit(dojio::open_at(path, flags, mode), [&engine, &buffers, &completed, &failed,
                                                       i](dojio::Result<dojio::FileDescriptor> opened,
                                                          dojio::OperationId, std::int32_t) {
      if (!opened.has_value()) {
        failed += 1;
        fmt::print(stderr, "async: open file {}: {}\n", i, std::get<dojio::IoError>(opened.error()).message());
        return;
      }
      const std::int32_t fd = opened->value();
      engine->submit(dojio::pwrite(dojio::FileDescriptor{fd}, 0), std::move(buffers[static_cast<std::size_t>(i)]),
                     [&engine, &buffers, &completed, &failed, i, fd](dojio::Result<std::vector<std::byte>> written,
                                                                     dojio::OperationId, std::int32_t result) {
                       if (!written.has_value() || static_cast<std::uint64_t>(result) != k_file_size) {
                         failed += 1;
                         fmt::print(stderr, "async: write file {}\n", i);
                         return;
                       }
                       buffers[static_cast<std::size_t>(i)] = std::move(*written);
                       engine->submit(dojio::fsync(dojio::FileDescriptor{fd}),
                                      [&engine, &completed, &failed, i, fd](dojio::Result<std::int32_t> synced,
                                                                            dojio::OperationId, std::int32_t) {
                                        if (!synced.has_value()) {
                                          failed += 1;
                                          fmt::print(stderr, "async: fsync file {}\n", i);
                                          return;
                                        }
                                        engine->submit(dojio::close(dojio::FileDescriptor{fd}),
                                                       [&completed, &failed](dojio::Result<std::int32_t> closed,
                                                                             dojio::OperationId, std::int32_t) {
                                                         if (!closed.has_value()) {
                                                           failed += 1;
                                                           return;
                                                         }
                                                         completed += 1;
                                                       });
                                      });
                     });
    });
  }
  engine->process(dojio::RingEngine::ProcessMode::Batch);
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start);

  if (failed != 0 || completed != k_file_count) {
    fmt::print(stderr, "async: {} files completed, {} failed\n", completed, failed);
    std::exit(1);
  }
  return elapsed;
}

} // namespace

int main(int argc, char** argv) { // main() signature is the C runtime ABI
  const std::filesystem::path base = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("/tmp");
  const std::filesystem::path root = base / "test_demo_file_rw";
  const std::filesystem::path serial_dir = root / "serial";
  const std::filesystem::path async_dir = root / "async";

  if (!reset_directory(serial_dir) || !reset_directory(async_dir)) {
    fmt::print(stderr, "cannot create test directories under {}\n", root.string());
    return 1;
  }

  std::vector<std::vector<std::byte>> buffers(static_cast<std::size_t>(k_file_count),
                                              std::vector<std::byte>(static_cast<std::size_t>(k_file_size)));
  std::mt19937 rng{k_random_seed};
  for (auto& buffer : buffers) {
    fill_random(buffer, rng);
  }

  fmt::print("dojio file write demo\n");
  fmt::print("  base directory: {}\n", root.string());
  fmt::print("  payload: {} files x {} MiB from {} random buffers\n", k_file_count, k_file_size / (1024ULL * 1024ULL),
             k_file_count);

  const auto serial_time = run_serial(serial_dir, buffers);
  const auto async_time = run_async(async_dir, buffers);

  const double mib = static_cast<double>(k_file_count) * static_cast<double>(k_file_size) / (1024.0 * 1024.0);
  const double serial_s = static_cast<double>(serial_time.count()) / 1e9;
  const double async_s = static_cast<double>(async_time.count()) / 1e9;
  fmt::print("  serial (blocking open/write/fsync/close):   {:8.3f} s  ({:8.1f} MiB/s)\n", serial_s, mib / serial_s);
  fmt::print("  async  (RingEngine, all files concurrent):  {:8.3f} s  ({:8.1f} MiB/s)\n", async_s, mib / async_s);
  fmt::print("  speedup: {:.2f}x\n", serial_s / async_s);

  std::error_code error;
  std::filesystem::remove_all(root, error);
  fmt::print("  removed {}\n", root.string());
  return 0;
}
