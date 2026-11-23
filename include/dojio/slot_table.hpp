#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include <dojio/contracts.hpp>
#include <dojio/operation_id.hpp>

namespace dojio::detail {

/// Generational slot table: an O(1) insert / find / take map from OperationId to a
/// value, backing the RingEngine's pending-operation set.
///
/// The OperationId value packs the slot index in the low 44 bits and the slot
/// generation in the high 20 bits:
///   bits [0..43]  : index (at most 2^44 - 1 slots)
///   bits [44..63] : generation (at most 2^20 - 1)
///
/// The generations 0 (so OperationId{0} stays k_invalid_operation_id) and 0xFFFFF
/// (so ~0ULL stays the RingEngine's cancel user_data) are reserved and never handed
/// out; the generation wraps around both. A stale id, looked up after its slot was
/// recycled, therefore never matches the current slot generation and resolves to
/// false / an empty optional.
///
/// Unlike a hash map, the slots are a flat vector indexed by the id, so a lookup is
/// an array index plus a generation compare, with no hashing and no allocation per
/// operation; freed slots are recycled through an intrusive free list.
///
/// The value is never exposed through a raw pointer: callers either visit it in
/// place (visit) or take ownership of it (take), so no pointer escapes the table.
template <typename T> class SlotTable {
public:
  explicit SlotTable(std::uint32_t initial_capacity = 0) {
    if (initial_capacity > 0) {
      slots_.reserve(static_cast<std::size_t>(initial_capacity));
    }
  }

  SlotTable(const SlotTable&) = delete;
  SlotTable& operator=(const SlotTable&) = delete;

  /// Reserves a free slot and returns its OperationId, leaving the slot value empty.
  /// Store the value with assign(id, value). The slot is removed from the free list
  /// immediately so it cannot be handed out twice.
  ///
  /// Precondition: the table has not reached its maximum size (2^44 slots).
  [[nodiscard]] OperationId reserve_id() {
    std::uint64_t index = 0;
    if (free_head_ == k_no_free) {
      // a new slot is appended at the end: its index must fit the 44-bit table field
      enforce_precondition(slots_.size() <= k_max_index);
      index = static_cast<std::uint64_t>(slots_.size());
      slots_.emplace_back();
    } else {
      index = free_head_;
      free_head_ = slots_[static_cast<std::size_t>(index)].next_free;
    }
    Slot& slot = slots_[static_cast<std::size_t>(index)];
    slot.next_free = k_no_free;
    active_count_ += 1;
    return make_id(index, slot.generation);
  }

  /// Stores value in the slot previously reserved with reserve_id().
  void assign(OperationId id, T value) {
    const std::optional<std::size_t> index = index_for(id);
    enforce_invariant(index.has_value() && !slots_[*index].value.has_value());
    slots_[*index].value.emplace(std::move(value));
  }

  /// Inserts value in a fresh slot and returns its OperationId.
  [[nodiscard]] OperationId insert(T value) {
    const OperationId id = reserve_id();
    assign(id, std::move(value));
    return id;
  }

  /// Invokes fn with the value stored in id and returns true. Returns false without
  /// invoking fn when id is stale, reserved or unassigned.
  template <typename Fn> bool visit(OperationId id, Fn&& fn) {
    const std::optional<std::size_t> index = index_for(id);
    if (!index.has_value() || !slots_[*index].value.has_value()) {
      return false;
    }
    fn(*slots_[*index].value);
    return true;
  }

  /// Invokes fn with the value stored in id and returns true. Returns false without
  /// invoking fn when id is stale, reserved or unassigned.
  template <typename Fn> bool visit(OperationId id, Fn&& fn) const {
    const std::optional<std::size_t> index = index_for(id);
    if (!index.has_value() || !slots_[*index].value.has_value()) {
      return false;
    }
    fn(*slots_[*index].value);
    return true;
  }

  /// True when id currently holds a value.
  [[nodiscard]] bool contains(OperationId id) const noexcept {
    const std::optional<std::size_t> index = index_for(id);
    return index.has_value() && slots_[*index].value.has_value();
  }

  /// Removes and returns the value of id, recycling its slot. An empty optional is
  /// returned when id is stale or was never assigned.
  [[nodiscard]] std::optional<T> take(OperationId id) noexcept {
    const std::optional<std::size_t> index = index_for(id);
    if (!index.has_value()) {
      return std::nullopt;
    }
    std::optional<T> result = std::move(slots_[*index].value);
    release_slot(*index);
    return result;
  }

  /// Frees a slot reserved with reserve_id() without ever assigning it, e.g. when
  /// the value construction fails. A stale id is ignored.
  void erase(OperationId id) noexcept {
    const std::optional<std::size_t> index = index_for(id);
    if (index.has_value()) {
      release_slot(*index);
    }
  }

  [[nodiscard]] std::uint64_t size() const noexcept { return active_count_; }
  [[nodiscard]] bool empty() const noexcept { return active_count_ == 0; }

  /// Drops every slot. The backing storage keeps its capacity.
  void clear() noexcept {
    slots_.clear();
    free_head_ = k_no_free;
    active_count_ = 0;
  }

private:
  /// Width of the packed OperationId fields: 44-bit slot index, 20-bit generation.
  static constexpr std::uint64_t k_index_bits = 44;
  static constexpr std::uint64_t k_generation_bits = 20;
  static constexpr std::uint64_t k_index_mask = (std::uint64_t{1} << k_index_bits) - 1U;
  static constexpr std::uint64_t k_max_index = k_index_mask;
  static constexpr std::uint32_t k_generation_mask =
      static_cast<std::uint32_t>((std::uint64_t{1} << k_generation_bits) - 1U);
  static constexpr std::uint64_t k_no_free = std::numeric_limits<std::uint64_t>::max();
  static constexpr std::uint32_t k_reserved_generation = k_generation_mask;

  struct Slot {
    std::optional<T> value;
    std::uint32_t generation = 1;
    std::uint64_t next_free = k_no_free;
  };

  static OperationId make_id(std::uint64_t index, std::uint32_t generation) noexcept {
    return OperationId{(static_cast<std::uint64_t>(generation) << k_index_bits) | (index & k_index_mask)};
  }
  static std::uint64_t index_of(OperationId id) noexcept { return id.value & k_index_mask; }
  static std::uint32_t generation_of(OperationId id) noexcept {
    return static_cast<std::uint32_t>((id.value >> k_index_bits) & k_generation_mask);
  }

  /// Index of the slot whose id matches (index + generation), or nullopt when stale.
  [[nodiscard]] std::optional<std::size_t> index_for(OperationId id) const noexcept {
    const std::uint64_t index = index_of(id);
    if (index >= slots_.size()) {
      return std::nullopt;
    }
    const std::size_t slot_index = static_cast<std::size_t>(index);
    if (slots_[slot_index].generation != generation_of(id)) {
      return std::nullopt;
    }
    return slot_index;
  }

  void release_slot(std::size_t index) noexcept {
    Slot& slot = slots_[index];
    slot.value.reset();
    // advance the generation within its 20-bit field, skipping both reserved values
    // so the id of a recycled slot never collides with k_invalid_operation_id or ~0ULL
    do {
      slot.generation = (slot.generation + 1U) & k_generation_mask;
    } while (slot.generation == 0 || slot.generation == k_reserved_generation);
    slot.next_free = free_head_;
    free_head_ = static_cast<std::uint64_t>(index);
    active_count_ -= 1;
  }

  std::vector<Slot> slots_;
  std::uint64_t free_head_ = k_no_free;
  std::uint64_t active_count_ = 0;
};

} // namespace dojio::detail
