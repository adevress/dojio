#include <ringu/memory_pool.hpp>

#include <ringu/contracts.hpp>
#include <ringu/ring.hpp>

#include <sys/uio.h>

#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

namespace ringu {

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
  return pool_->base() + static_cast<std::size_t>(index_ * pool_->buffer_size());
}

std::uint64_t RegisteredBuffer::size() const { return pool_ != nullptr ? pool_->buffer_size() : 0; }

RegisteredMemoryPool::RegisteredMemoryPool(std::int32_t ring_fd, std::uint64_t buffer_size, std::uint64_t count,
                                           std::pmr::memory_resource* resource)
    : ring_fd_(ring_fd), buffer_size_(buffer_size), count_(count), resource_(resource), free_(count, 1, resource) {
  enforce_precondition(buffer_size > 0 && count > 0);
  enforce_precondition(count <= std::numeric_limits<std::uint64_t>::max() / buffer_size);
  const std::uint64_t total = buffer_size * count;
  // memory_resource::allocate takes a size_t (C++ std ABI boundary)
  memory_ = static_cast<std::byte*>(resource_->allocate(static_cast<std::size_t>(total), alignof(std::max_align_t)));
  std::vector<iovec> iovecs(static_cast<std::size_t>(count));
  for (std::uint64_t i = 0; i < count; ++i) {
    iovecs[static_cast<std::size_t>(i)].iov_base = memory_ + static_cast<std::size_t>(i * buffer_size);
    iovecs[static_cast<std::size_t>(i)].iov_len = static_cast<std::size_t>(buffer_size);
  }
  if (detail::register_buffers(ring_fd_, iovecs.data(), static_cast<std::uint32_t>(count)) != 0) {
    resource_->deallocate(memory_, static_cast<std::size_t>(total), alignof(std::max_align_t));
    memory_ = nullptr;
    throw std::runtime_error("RegisteredMemoryPool: io_uring_register(IORING_REGISTER_BUFFERS) failed");
  }
}

RegisteredMemoryPool::~RegisteredMemoryPool() {
  // Invariant: the pool cannot be destroyed while a RegisteredBuffer is active.
  // (The destructor is noexcept, so the std::logic_error raised on violation turns
  // into std::terminate.)
  enforce_invariant(active_count_ == 0);
  detail::unregister_buffers(ring_fd_);
  if (memory_ != nullptr) {
    resource_->deallocate(memory_, static_cast<std::size_t>(buffer_size_ * count_), alignof(std::max_align_t));
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
