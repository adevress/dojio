#include <dojio/memory_pool.hpp>

// The fixed-buffer registration only exists on Linux, where io_uring appeared in
// kernel 5.1 (with its IORING_REGISTER_BUFFERS table). On every other platform, with
// too old kernel headers, and in the DOJIO_FORCE_LINUX_OLD_KERNEL configuration that
// fakes an older kernel, the pool still maps and hands out its chunks but registers
// nothing: no ring fd is duplicated, no io_uring_register(2) call is made.
#if defined(__linux__) && !defined(DOJIO_FORCE_LINUX_OLD_KERNEL) && __has_include(<linux/io_uring.h>)
#define DOJIO_MEMORY_POOL_REGISTERS_BUFFERS 1
#else
#define DOJIO_MEMORY_POOL_REGISTERS_BUFFERS 0
#endif

#include <dojio/contracts.hpp>

#include <fmt/format.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if DOJIO_MEMORY_POOL_REGISTERS_BUFFERS
#include <dojio/ring.hpp>

#include <fcntl.h>
#include <linux/io_uring.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#endif

namespace dojio {

namespace {

// Combines flag constants as unsigned values (MISRA Rule 7.0.4) and returns the result
// as the int expected by the C API.
template <typename... Flags> [[nodiscard]] constexpr int combine_flags(Flags... flags) noexcept {
  return static_cast<int>((static_cast<unsigned int>(flags) | ...));
}

// sysconf(3) returns long (C API boundary); narrowed with an explicit cast.
std::uint64_t read_page_size() {
  const auto v = ::sysconf(_SC_PAGESIZE);
  return v > 0 ? static_cast<std::uint64_t>(v) : std::uint64_t{4096};
}

#if DOJIO_MEMORY_POOL_REGISTERS_BUFFERS

// syscall(2) returns long (C API boundary): narrowed to the raw io_uring_register(2)
// result, which is 0 on success and the negated errno on failure.
std::int32_t sys_io_uring_register(std::int32_t ring_fd, std::uint32_t opcode, const void* arg, std::uint32_t nr_args) {
  enforce_precondition(ring_fd >= 0);
  return static_cast<std::int32_t>(::syscall(__NR_io_uring_register, ring_fd, opcode, arg, nr_args));
}

// io_uring_register(IORING_REGISTER_BUFFERS, ...): registers nr fixed buffers.
std::int32_t register_buffers(std::int32_t ring_fd, const iovec* iovecs, std::uint32_t nr) {
  enforce_precondition(iovecs != nullptr && nr > 0);
  return sys_io_uring_register(ring_fd, IORING_REGISTER_BUFFERS, iovecs, nr);
}

// io_uring_register(IORING_UNREGISTER_BUFFERS, ...): drops all the fixed buffers of the ring.
std::int32_t unregister_buffers(std::int32_t ring_fd) {
  return sys_io_uring_register(ring_fd, IORING_UNREGISTER_BUFFERS, nullptr, 0);
}

#endif // DOJIO_MEMORY_POOL_REGISTERS_BUFFERS

} // namespace

std::uint64_t cache_line_size() noexcept { return 64; }

std::uint64_t page_size() noexcept {
  // function-local static: initialized once, thread-safe since C++11
  // cppcheck-suppress misra-cpp-2023-6.7.1 ; MISRA C++ deviation DEV-009: immutable, thread-safe page-size cache
  static const std::uint64_t size = read_page_size();
  return size;
}

// The pool implementation: every data member of the pool lives here, so the public
// header only carries a unique_ptr and the pool is movable by construction.
class RegisteredMemoryPool::Impl {
public:
  Impl(std::int32_t ring_fd, std::uint64_t buffer_size, std::uint64_t chunk_size, std::uint64_t count,
       std::uint64_t mapped_size, std::byte* memory);
  ~Impl();

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  [[nodiscard]] std::uint64_t buffer_size() const noexcept { return buffer_size_; }
  [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
  [[nodiscard]] std::uint64_t active_count() const noexcept {
    return count_ - static_cast<std::uint64_t>(free_indices_.size());
  }
  [[nodiscard]] std::uint64_t available() const noexcept { return static_cast<std::uint64_t>(free_indices_.size()); }
  [[nodiscard]] std::byte* base() const noexcept { return memory_; }
  [[nodiscard]] std::byte* chunk(std::uint64_t index) const noexcept {
    return memory_ + static_cast<std::size_t>(index * chunk_size_);
  }

  /// Pops a free chunk index. Returns false when the pool is exhausted.
  [[nodiscard]] bool acquire(std::uint64_t& index) noexcept;
  /// Returns a chunk index to the free stack.
  void release(std::uint64_t index) noexcept;

private:
  std::int32_t ring_fd_ = -1; // close-on-exec duplicate of the ring fd; -1 when not registered
  std::uint64_t buffer_size_ = 0;
  std::uint64_t chunk_size_ = 0; // cache-line aligned stride between two chunks
  std::uint64_t count_ = 0;
  std::uint64_t mapped_size_ = 0; // size of the whole mmap'ed page range
  std::byte* memory_ = nullptr;
  std::vector<std::uint64_t> free_indices_; // stack of the free chunk indices
};

RegisteredMemoryPool::Impl::Impl(std::int32_t ring_fd, std::uint64_t buffer_size, std::uint64_t chunk_size,
                                 std::uint64_t count, std::uint64_t mapped_size, std::byte* memory)
    : ring_fd_(ring_fd), buffer_size_(buffer_size), chunk_size_(chunk_size), count_(count), mapped_size_(mapped_size),
      memory_(memory) {
  // pre-sized free stack: release() never allocates, so it stays noexcept
  free_indices_.reserve(static_cast<std::size_t>(count_));
  // index 0 is popped first: the chunks are handed out in address order
  for (std::uint64_t i = count_; i > 0; --i) {
    free_indices_.push_back(i - 1);
  }
}

RegisteredMemoryPool::Impl::~Impl() {
  // Invariant: the pool cannot be destroyed while a RegisteredBuffer is active.
  // (The destructor is noexcept, so the std::logic_error raised on violation turns
  // into std::terminate.)
  enforce_invariant(free_indices_.size() == static_cast<std::size_t>(count_));
#if DOJIO_MEMORY_POOL_REGISTERS_BUFFERS
  // the fixed buffers must be dropped before the mapping is released
  // (ring_fd_ is only set when the chunks were actually registered)
  if (ring_fd_ >= 0) {
    enforce_invariant(unregister_buffers(ring_fd_) == 0);
  }
#endif
  // munmap takes a size_t length (C API boundary)
  enforce_invariant(::munmap(memory_, static_cast<std::size_t>(mapped_size_)) == 0);
#if DOJIO_MEMORY_POOL_REGISTERS_BUFFERS
  // last reference on the io_uring instance owned by the pool
  if (ring_fd_ >= 0) {
    enforce_invariant(::close(ring_fd_) == 0);
  }
#endif
}

bool RegisteredMemoryPool::Impl::acquire(std::uint64_t& index) noexcept {
  if (free_indices_.empty()) {
    return false;
  }
  index = free_indices_.back();
  free_indices_.pop_back();
  return true;
}

void RegisteredMemoryPool::Impl::release(std::uint64_t index) noexcept {
  // invariant: the index is valid and the chunk is currently borrowed (returning the
  // same buffer twice is a bug: the free stack would then hold more indices than the
  // pool has chunks)
  enforce_invariant(index < count_ && free_indices_.size() < static_cast<std::size_t>(count_));
  free_indices_.push_back(index);
}

RegisteredMemoryPool::RegisteredMemoryPool(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

RegisteredMemoryPool::RegisteredMemoryPool(RegisteredMemoryPool&& other) noexcept = default;

RegisteredMemoryPool& RegisteredMemoryPool::operator=(RegisteredMemoryPool&& other) noexcept = default;

RegisteredMemoryPool::~RegisteredMemoryPool() = default;

RegisteredMemoryPool::Impl& RegisteredMemoryPool::impl() const noexcept {
  enforce_invariant(impl_ != nullptr);
  return *impl_;
}

tl::expected<RegisteredMemoryPool, DojioError>
RegisteredMemoryPool::create(detail::IoUring& ring, std::uint64_t buffer_size, std::uint64_t count) {
  enforce_precondition(buffer_size > 0 && count > 0);
  // io_uring identifies a fixed buffer by a 16-bit index (see RegisteredBuffer::index)
  enforce_precondition(count <= std::numeric_limits<std::uint16_t>::max());

  // every chunk starts on a cache-line boundary: round the stride up to the cache line size
  const std::uint64_t cache_line = cache_line_size();
  enforce_precondition(buffer_size <= std::numeric_limits<std::uint64_t>::max() - (cache_line - 1));
  const std::uint64_t chunk_size = (buffer_size + cache_line - 1) / cache_line * cache_line;
  enforce_precondition(count <= std::numeric_limits<std::uint64_t>::max() / chunk_size);
  const std::uint64_t total = chunk_size * count;
  // the backing memory is allocated as whole pages and distributed in chunks
  const std::uint64_t page = page_size();
  enforce_precondition(total <= std::numeric_limits<std::uint64_t>::max() - (page - 1));
  const std::uint64_t mapped_size = (total + page - 1) / page * page;

  // ring_fd stays -1 when the chunks are not registered: the pool then only owns the
  // mmap'ed memory and never touches the ring
  std::int32_t ring_fd = -1;
#if DOJIO_MEMORY_POOL_REGISTERS_BUFFERS
  // the ring exists (io_uring is available by construction), so the fixed-buffer
  // table is there: register the chunks. The pool owns its own close-on-exec
  // reference on the io_uring instance, so the fixed buffers stay registered even
  // if the ring object is destroyed first, and closing the duplicate in the
  // destructor drops that reference.
  ring_fd = static_cast<std::int32_t>(::fcntl(ring.fd(), F_DUPFD_CLOEXEC, 0));
  if (ring_fd < 0) {
    // errno is only valid right after the failing call: capture it before formatting
    const std::int32_t err = static_cast<std::int32_t>(errno);
    return tl::unexpected(
        DojioError{ErrorOpcode::MemoryPoolRingFd,
                   fmt::format("dojio: RegisteredMemoryPool fcntl(F_DUPFD_CLOEXEC, ring_fd={}) failed: {}", ring.fd(),
                               std::error_code(err, std::generic_category()).message())});
  }
#else
  // no io_uring fixed-buffer table on this platform: the ring is not used
  static_cast<void>(ring);
#endif

  // mmap takes a size_t length and an off_t offset (C API boundaries)
  void* p = ::mmap(nullptr, static_cast<std::size_t>(mapped_size), combine_flags(PROT_READ, PROT_WRITE),
                   combine_flags(MAP_PRIVATE, MAP_ANONYMOUS), -1, 0);
  if (p == MAP_FAILED) {
    // errno is only valid right after the failing syscall: capture it before formatting
    const std::int32_t err = static_cast<std::int32_t>(errno);
    if (ring_fd >= 0) {
      static_cast<void>(::close(ring_fd));
    }
    return tl::unexpected(DojioError{ErrorOpcode::MemoryPoolMmap,
                                     fmt::format("dojio: RegisteredMemoryPool mmap(size={}) failed: {}", mapped_size,
                                                 std::error_code(err, std::generic_category()).message())});
  }
  std::byte* memory = static_cast<std::byte*>(p);

#if DOJIO_MEMORY_POOL_REGISTERS_BUFFERS
  if (ring_fd >= 0) {
    // register every chunk with the ring as a fixed buffer
    std::vector<iovec> iovecs(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
      iovecs[static_cast<std::size_t>(i)].iov_base = memory + static_cast<std::size_t>(i * chunk_size);
      iovecs[static_cast<std::size_t>(i)].iov_len = static_cast<std::size_t>(chunk_size);
    }
    const std::int32_t ret = register_buffers(ring_fd, iovecs.data(), static_cast<std::uint32_t>(count));
    if (ret != 0) {
      const std::int32_t err = -ret;
      // the registration failed: undo the mapping and drop the duplicated fd
      static_cast<void>(::munmap(memory, static_cast<std::size_t>(mapped_size)));
      static_cast<void>(::close(ring_fd));
      return tl::unexpected(
          DojioError{ErrorOpcode::MemoryPoolRegisterBuffers,
                     fmt::format("dojio: io_uring_register(IORING_REGISTER_BUFFERS, nr={}) failed: {}", count,
                                 std::error_code(err, std::generic_category()).message())});
    }
  }
#endif

  // the implementation owns the mapping (and the duplicated ring fd when registered)
  return RegisteredMemoryPool(std::make_unique<Impl>(ring_fd, buffer_size, chunk_size, count, mapped_size, memory));
}

tl::expected<RegisteredBuffer, DojioError> RegisteredMemoryPool::borrow() {
  Impl& pool = impl();
  std::uint64_t index = 0;
  if (!pool.acquire(index)) {
    return tl::unexpected(DojioError{ErrorOpcode::MemoryPoolExhausted,
                                     fmt::format("dojio: RegisteredMemoryPool exhausted (count={})", pool.count())});
  }
  return RegisteredBuffer(pool, index);
}

std::uint64_t RegisteredMemoryPool::buffer_size() const noexcept { return impl().buffer_size(); }

std::uint64_t RegisteredMemoryPool::capacity() const noexcept { return impl().count(); }

std::uint64_t RegisteredMemoryPool::active_count() const noexcept { return impl().active_count(); }

std::uint64_t RegisteredMemoryPool::available() const noexcept { return impl().available(); }

std::byte* RegisteredMemoryPool::base() const noexcept { return impl().base(); }

RegisteredBuffer::RegisteredBuffer(RegisteredMemoryPool::Impl& impl, std::uint64_t index) noexcept
    : impl_(&impl), index_(index) {}

RegisteredBuffer::RegisteredBuffer(RegisteredBuffer&& other) noexcept
    : impl_(other.impl_), index_(other.index_), length_(other.length_) {
  other.impl_ = nullptr;
  other.length_ = 0;
}

RegisteredBuffer& RegisteredBuffer::operator=(RegisteredBuffer&& other) noexcept {
  if (this != &other) {
    release();
    impl_ = other.impl_;
    index_ = other.index_;
    length_ = other.length_;
    other.impl_ = nullptr;
    other.length_ = 0;
  }
  return *this;
}

RegisteredBuffer::~RegisteredBuffer() { release(); }

void RegisteredBuffer::release() noexcept {
  if (impl_ != nullptr) {
    impl_->release(index_);
    impl_ = nullptr;
  }
}

std::byte* RegisteredBuffer::data() const {
  enforce_precondition(impl_ != nullptr);
  return impl_->chunk(index_);
}

std::uint64_t RegisteredBuffer::size() const { return impl_ != nullptr ? length_ : 0; }

std::uint64_t RegisteredBuffer::capacity() const { return impl_ != nullptr ? impl_->buffer_size() : 0; }

void RegisteredBuffer::append(std::span<const std::byte> bytes) {
  enforce_precondition(impl_ != nullptr);
  enforce_precondition(bytes.size() <= (capacity() - length_));
  std::copy(bytes.begin(), bytes.end(), data() + static_cast<std::ptrdiff_t>(length_));
  length_ += static_cast<std::uint64_t>(bytes.size());
}

void RegisteredBuffer::append(std::string_view text) {
  // the byte representation of the text is appended as is
  append(std::span<const std::byte>{reinterpret_cast<const std::byte*>(text.data()), text.size()});
}

void RegisteredBuffer::resize(std::uint64_t size) {
  enforce_precondition(impl_ != nullptr && size <= capacity());
  length_ = size;
}

void RegisteredBuffer::clear() { length_ = 0; }

} // namespace dojio

#undef DOJIO_MEMORY_POOL_REGISTERS_BUFFERS
