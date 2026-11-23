#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <dojio/slot_table.hpp>

namespace {

using dojio::OperationId;
using dojio::detail::SlotTable;

/// Reads the value stored in id through the visitor API.
template <typename T> std::optional<T> read_value(const SlotTable<T>& table, OperationId id) {
  std::optional<T> result;
  static_cast<void>(table.visit(id, [&](const T& value) { result = value; }));
  return result;
}

TEST_CASE("SlotTable insert/visit/take round-trip") {
  SlotTable<std::int32_t> table;
  CHECK(table.empty());
  const OperationId a = table.insert(42);
  const OperationId b = table.insert(-7);
  CHECK(a != b);
  CHECK(a != dojio::k_invalid_operation_id);
  CHECK(b != dojio::k_invalid_operation_id);
  CHECK(table.size() == 2);
  CHECK(table.contains(a));
  CHECK(table.contains(b));
  const std::optional<std::int32_t> value_a = read_value(table, a);
  REQUIRE(value_a.has_value());
  CHECK(*value_a == 42);
  const std::optional<std::int32_t> value_b = read_value(table, b);
  REQUIRE(value_b.has_value());
  CHECK(*value_b == -7);

  std::optional<std::int32_t> taken = table.take(a);
  REQUIRE(taken.has_value());
  CHECK(*taken == 42);
  CHECK_FALSE(table.contains(a));
  CHECK(table.size() == 1);
  CHECK_FALSE(table.take(a).has_value());
  CHECK(table.contains(b));
}

TEST_CASE("SlotTable packs the index in the low 44 bits and the generation in the high 20") {
  SlotTable<std::int32_t> table;
  const OperationId id = table.insert(1);
  constexpr std::uint64_t k_index_mask = (std::uint64_t{1} << 44) - 1U;
  CHECK((id.value & k_index_mask) == 0); // first slot
  CHECK((id.value >> 44) == 1);          // first generation (0 is reserved)
  CHECK((id.value >> 44) <= 0xFFFFFu);   // the generation fits its 20-bit field
}

TEST_CASE("SlotTable recycles freed slots with a new generation") {
  SlotTable<std::int32_t> table;
  const OperationId first = table.insert(1);
  CHECK(table.take(first).has_value());
  const OperationId second = table.insert(2);
  // the slot index is reused but the id differs, so the stale id is rejected
  constexpr std::uint64_t k_index_mask = (std::uint64_t{1} << 44) - 1U;
  CHECK((first.value & k_index_mask) == (second.value & k_index_mask));
  CHECK(first != second);
  CHECK_FALSE(table.contains(first));
  CHECK_FALSE(table.take(first).has_value());
  CHECK(table.contains(second));
  const std::optional<std::int32_t> value = read_value(table, second);
  REQUIRE(value.has_value());
  CHECK(*value == 2);
}

TEST_CASE("SlotTable rejects the reserved OperationIds") {
  SlotTable<std::int32_t> table;
  static_cast<void>(table.insert(5));
  CHECK_FALSE(table.contains(dojio::k_invalid_operation_id));
  CHECK_FALSE(table.take(dojio::k_invalid_operation_id).has_value());
  const OperationId cancel_user_data{~static_cast<std::uint64_t>(0)};
  CHECK_FALSE(table.contains(cancel_user_data));
  CHECK_FALSE(table.take(cancel_user_data).has_value());
}

TEST_CASE("SlotTable grows beyond the initial capacity preserving entries") {
  SlotTable<std::string> table{2};
  std::vector<OperationId> ids;
  for (std::int32_t i = 0; i < 32; ++i) {
    ids.push_back(table.insert("value_" + std::to_string(i)));
  }
  CHECK(table.size() == 32);
  for (std::int32_t i = 0; i < 32; ++i) {
    const std::optional<std::string> value = read_value(table, ids[static_cast<std::size_t>(i)]);
    REQUIRE(value.has_value());
    CHECK(*value == "value_" + std::to_string(i));
  }
  // erase every other entry and reinsert: the remaining entries stay reachable and
  // the stale ids of the recycled slots are rejected
  for (std::size_t i = 0; i < ids.size(); i += 2) {
    CHECK(table.take(ids[i]).has_value());
  }
  const OperationId fresh = table.insert("fresh");
  for (std::size_t i = 1; i < ids.size(); i += 2) {
    CHECK(table.contains(ids[i]));
  }
  CHECK_FALSE(table.contains(ids[0]));
  CHECK(table.contains(fresh));
  const std::optional<std::string> value = read_value(table, fresh);
  REQUIRE(value.has_value());
  CHECK(*value == "fresh");
}

TEST_CASE("SlotTable reserve_id/assign/erase") {
  SlotTable<std::int32_t> table;
  const OperationId id = table.reserve_id();
  CHECK_FALSE(table.contains(id)); // reserved but not assigned
  table.assign(id, 11);
  CHECK(table.contains(id));
  const std::optional<std::int32_t> value = read_value(table, id);
  REQUIRE(value.has_value());
  CHECK(*value == 11);

  const OperationId other = table.reserve_id();
  table.erase(other);
  CHECK(table.size() == 1);
  CHECK_FALSE(table.contains(other));
}

TEST_CASE("SlotTable visit reports stale ids") {
  SlotTable<std::int32_t> table;
  const OperationId id = table.insert(3);
  bool visited = false;
  CHECK(table.visit(id, [&](const std::int32_t& value) {
    visited = true;
    CHECK(value == 3);
  }));
  CHECK(visited);
  CHECK(table.take(id).has_value());
  visited = false;
  CHECK_FALSE(table.visit(id, [&](const std::int32_t&) { visited = true; }));
  CHECK_FALSE(visited);
}

} // namespace
