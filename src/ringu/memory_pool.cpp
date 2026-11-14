#include <ringu/memory_pool.hpp>

#include <ringu/contracts.hpp>
#include <ringu/ring.hpp>

#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>

#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

namespace ringu {

namespace {

// sysconf(3) returns long (C API boundary); narrowed with an explicit cast.
// (The cache line size has a portable standard equivalent,
// std::hardware_destructive_interference_size, used instead.)
std::uint64_t query_page_size() {
  const long v = ::sysconf(_SC_PAGESIZE);
  return v > 0 ? static_cast<std::uint64_t>(v) : std::uint64_t{4096};
}

} // namespace

RegisteredBuffer::RegisteredBuffer(RegisteredMemoryPool& pool, std::uint64_t index) : pool_(&pool), index_(index) {}

RegisteredBuffer::RegisteredBuffer(RegisteredBuffer&& other) noexcept : pool_(other.pool_), index_(other.index_) {
  other.pool_ = nullptr;
}

RegisteredBuffer& RegisteredBuffer::operator=(RegisteredBuffer&& other) noexcept {
  if (this != &other) {
    release();
    pool_ = other.pool_;
    index_ = other.index_;
    other.pool_ = nullptr;
  }
  return *this;
}

RegisteredBuffer::~RegisteredBuffer() { release(); }

void RegisteredBuffer::release() noexcept {
  if (pool_ != nullptr) {
    pool_->return_buffer(index_);
    pool_ = nullptr;
  }
}

std::byte* RegisteredBuffer::data() const {
  enforce_precondition(pool_ != nullptr);
  return pool_->base() + static_cast<std::size_t>(index_ * pool_->chunk_size_);
}

std::uint64_t RegisteredBuffer::size() const { return pool_ != nullptr ? pool_->buffer_size() : 0; }

RegisteredMemoryPool::RegisteredMemoryPool(detail::IoUring& ring, std::uint64_t buffer_size, std::uint64_t count,
                                           std::pmr::memory_resource* resource)
    : ring_(ring), buffer_size_(buffer_size), count_(count), resource_(resource), free_(count, 1, resource) {
  enforce_precondition(buffer_size > 0 && count > 0);
  // portable cache line size from the C++ standard (<new>)
  cache_line_size_ = static_cast<std::uint64_t>(std::hardware_destructive_interference_size);
  page_size_ = query_page_size();
  // every chunk starts on a cache-line boundary: round the stride up to the cache line size
  chunk_size_ = (buffer_size_ + cache_line_size_ - 1) / cache_line_size_ * cache_line_size_;
  enforce_precondition(count_ <= std::numeric_limits<std::uint64_t>::max() / chunk_size_);
  const std::uint64_t total = chunk_size_ * count_;
  enforce_precondition(total <= std::numeric_limits<std::uint64_t>::max() - page_size_ + 1);
  // the backing memory is allocated as whole pages and distributed in chunks
  const std::uint64_t pages = (total + page_size_ - 1) / page_size_;
  mapped_size_ = pages * page_size_;
  // mmap takes a size_t length and an off_t offset (C API boundaries)
  void* p = ::mmap(nullptr, static_cast<std::size_t>(mapped_size_), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                   -1, 0);
  if (p == MAP_FAILED) {
    throw std::runtime_error("RegisteredMemoryPool: mmap failed");
  }
  memory_ = static_cast<std::byte*>(p);
  // register every chunk with the ring as a fixed buffer
  std::vector<iovec> iovecs(static_cast<std::size_t>(count));
  for (std::uint64_t i = 0; i < count; ++i) {
    iovecs[static_cast<std::size_t>(i)].iov_base = memory_ + static_cast<std::size_t>(i * chunk_size_);
    iovecs[static_cast<std::size_t>(i)].iov_len = static_cast<std::size_t>(chunk_size_);
  }
  if (detail::register_buffers(ring_.fd(), iovecs.data(), static_cast<std::uint32_t>(count)) != 0) {
    ::munmap(memory_, static_cast<std::size_t>(mapped_size_));
    memory_ = nullptr;
    throw std::runtime_error("RegisteredMemoryPool: io_uring_register(IORING_REGISTER_BUFFERS) failed");
  }
}

RegisteredMemoryPool::~RegisteredMemoryPool() {
  // Invariant: the pool cannot be destroyed while a RegisteredBuffer is active.
  // (The destructor is noexcept, so the std::logic_error raised on violation turns
  // into std::terminate.)
  enforce_invariant(active_count_ == 0);
  detail::unregister_buffers(ring_.fd());
  if (memory_ != nullptr) {
    // munmap takes a size_t length (C API boundary)
    ::munmap(memory_, static_cast<std::size_t>(mapped_size_));
  }
}

RegisteredBuffer RegisteredMemoryPool::borrow() {
  for (std::uint64_t n = 0; n < count_; ++n) {
    const std::uint64_t i = (hint_ + n) % count_;
    if (free_[static_cast<std::size_t>(i)] != 0) {
      free_[static_cast<std::size_t>(i)] = 0;
      hint_ = (i + 1) % count_;
      // a free slot was found, so the active counter cannot be at capacity
      enforce_invariant(active_count_ < count_);
      active_count_ += 1;
      return RegisteredBuffer(*this, i);
    }
  }
  throw std::runtime_error("RegisteredMemoryPool: pool exhausted");
}

void RegisteredMemoryPool::return_buffer(std::uint64_t index) noexcept {
  // invariant: the index is valid and the buffer is currently borrowed
  // (returning the same buffer twice is a bug)
  enforce_invariant(index < count_ && free_[static_cast<std::size_t>(index)] == 0);
  free_[static_cast<std::size_t>(index)] = 1;
  active_count_ -= 1;
}

} // namespace ringu
