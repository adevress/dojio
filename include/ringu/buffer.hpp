#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ringu {

/// Size of a sockaddr_storage on Linux, used for the peer address of Accept.
inline constexpr std::uint64_t k_sockaddr_storage_size = 128;

/// A fixed-size buffer usable as an operation buffer.
///
/// StaticBuffer is an alternative to std::vector / std::array / RegisteredBuffer for
/// operations whose buffer size is known at compile time (e.g. the peer address of
/// an Accept operation). It satisfies the BufferLike concept accepted by
/// Reactor::submit.
template <std::uint64_t N> class StaticBuffer {
public:
  using value_type = std::byte;
  static constexpr std::uint64_t static_size = N;

  StaticBuffer() = default;

  [[nodiscard]] std::byte* data() noexcept { return storage_.data(); }
  [[nodiscard]] const std::byte* data() const noexcept { return storage_.data(); }
  [[nodiscard]] static constexpr std::uint64_t size() noexcept { return N; }

  /// View of the whole buffer.
  [[nodiscard]] std::span<std::byte> span() noexcept { return std::span<std::byte>{storage_}; }
  /// View of the whole buffer.
  [[nodiscard]] std::span<const std::byte> span() const noexcept { return std::span<const std::byte>{storage_}; }

private:
  std::array<std::byte, static_cast<std::size_t>(N)> storage_{};
};

/// Static buffer holding a sockaddr_storage, for the peer address of Accept.
using AcceptBuffer = StaticBuffer<k_sockaddr_storage_size>;

} // namespace ringu
