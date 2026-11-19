// Non-Linux stub of the registered memory pool: registering fixed buffers needs
// io_uring, so create() and borrow() always fail with a NotSupported RinguError.

#include <ringu/memory_pool.hpp>

#include <ringu/contracts.hpp>

#include <fmt/format.h>

namespace ringu {

namespace {

RinguError not_supported_error() {
  return RinguError{ErrorOpcode::NotSupported,
                    fmt::format("ringu: RegisteredMemoryPool is not supported on this platform (io_uring requires "
                                "Linux)")};
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

void RegisteredBuffer::release() noexcept { pool_ = nullptr; }

std::byte* RegisteredBuffer::data() const {
  enforce_precondition(pool_ != nullptr);
  return nullptr;
}

std::uint64_t RegisteredBuffer::size() const { return 0; }

tl::expected<std::unique_ptr<RegisteredMemoryPool>, RinguError>
RegisteredMemoryPool::create(detail::IoUring& ring, std::uint64_t buffer_size, std::uint64_t count,
                             std::pmr::memory_resource* resource) {
  (void)ring;
  (void)buffer_size;
  (void)count;
  (void)resource;
  return tl::unexpected(not_supported_error());
}

RegisteredMemoryPool::~RegisteredMemoryPool() = default;

tl::expected<RegisteredBuffer, RinguError> RegisteredMemoryPool::borrow() {
  return tl::unexpected(not_supported_error());
}

void RegisteredMemoryPool::return_buffer(std::uint64_t index) noexcept { (void)index; }

} // namespace ringu
