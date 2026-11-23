#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <dojio/dojio_memory.hpp>

namespace {

/// Counts allocation / deallocation on a backing resource, to check that the single
/// allocation of a SharedValue is returned exactly once.
class CountingResource final : public std::pmr::memory_resource {
public:
  explicit CountingResource(std::pmr::memory_resource* upstream = std::pmr::new_delete_resource())
      : upstream_(upstream) {}

  std::uint64_t allocations = 0;
  std::uint64_t deallocations = 0;
  std::size_t last_deallocation_size = 0;

private:
  void* do_allocate(std::size_t bytes, std::size_t alignment) override {
    allocations += 1;
    return upstream_->allocate(bytes, alignment);
  }

  void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
    deallocations += 1;
    last_deallocation_size = bytes;
    upstream_->deallocate(p, bytes, alignment);
  }

  [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
    return this == &other;
  }

  std::pmr::memory_resource* upstream_;
};

/// Counts live instances and records the constructor arguments it received.
struct Widget {
  static inline std::int32_t alive = 0;

  Widget(std::int32_t a, std::int32_t b) : sum(a + b) { alive += 1; }
  explicit Widget(std::int32_t a) : sum(a) { alive += 1; }
  ~Widget() { alive -= 1; }

  [[nodiscard]] std::int32_t doubled() const { return sum * 2; }

  std::int32_t sum;
};

} // namespace

static_assert(std::is_copy_constructible_v<dojio::SharedValue<Widget>>);
static_assert(std::is_copy_assignable_v<dojio::SharedValue<Widget>>);
static_assert(std::is_move_constructible_v<dojio::SharedValue<Widget>>);

TEST_CASE("SharedValue default initializes to an empty box") {
  dojio::SharedValue<Widget> box;
  CHECK_FALSE(static_cast<bool>(box));
  CHECK(box.get() == nullptr);
  CHECK(box.use_count() == 0);
}

TEST_CASE("SharedValue::make allocates a single region from the resource and forwards the arguments") {
  CountingResource resource;

  CHECK(Widget::alive == 0);
  {
    auto box = dojio::SharedValue<Widget>::make(&resource, 20, 22);

    CHECK(static_cast<bool>(box));
    CHECK(box.get() != nullptr);
    CHECK(Widget::alive == 1);
    CHECK(box->sum == 42);
    CHECK(box->doubled() == 84);
    CHECK((*box).sum == 42);
    CHECK(box.use_count() == 1);

    // the reference count, the memory resource and the object live in one allocation
    CHECK(resource.allocations == 1);
    CHECK(resource.deallocations == 0);
    CHECK(box.resource() == &resource);
  }
  CHECK(Widget::alive == 0);
  CHECK(resource.allocations == 1);
  CHECK(resource.deallocations == 1);
}

TEST_CASE("SharedValue::make works on the default resource") {
  auto box = dojio::SharedValue<Widget>::make(std::pmr::new_delete_resource(), 7);
  CHECK(static_cast<bool>(box));
  CHECK(box->sum == 7);
  CHECK(box.resource() == std::pmr::new_delete_resource());
}

TEST_CASE("SharedValue move construction transfers the reference") {
  auto source = dojio::SharedValue<Widget>::make(std::pmr::new_delete_resource(), 5);
  Widget* raw = source.get();

  dojio::SharedValue<Widget> destination{std::move(source)};
  CHECK_FALSE(static_cast<bool>(source));
  CHECK(destination.get() == raw);
  CHECK(destination->sum == 5);
  CHECK(destination.use_count() == 1);
  CHECK(Widget::alive == 1);
}

TEST_CASE("SharedValue move assignment drops the previous reference") {
  auto first = dojio::SharedValue<Widget>::make(std::pmr::new_delete_resource(), 1);
  auto second = dojio::SharedValue<Widget>::make(std::pmr::new_delete_resource(), 2);
  Widget* second_raw = second.get();

  CHECK(Widget::alive == 2);
  first = std::move(second);
  CHECK(Widget::alive == 1);
  CHECK_FALSE(static_cast<bool>(second));
  CHECK(first.get() == second_raw);
  CHECK(first->sum == 2);
}

TEST_CASE("copy construction and assignment share the object and its reference count") {
  CountingResource resource;

  auto box = dojio::SharedValue<Widget>::make(&resource, 7);
  CHECK(box.use_count() == 1);
  {
    dojio::SharedValue<Widget> shared = box;
    CHECK(shared.use_count() == 2);
    CHECK(box.use_count() == 2);
    CHECK(shared.get() == box.get());
    CHECK(static_cast<bool>(box));
    // no new allocation: the copy only increments the count
    CHECK(resource.allocations == 1);

    dojio::SharedValue<Widget> assigned;
    assigned = box;
    CHECK(assigned.use_count() == 3);
    CHECK(assigned.get() == box.get());

    box.reset();
    CHECK_FALSE(static_cast<bool>(box));
    CHECK(Widget::alive == 1);
    CHECK(shared->sum == 7);
  }
  // the last reference destroyed the object and deallocated the single region
  CHECK(Widget::alive == 0);
  CHECK(resource.allocations == 1);
  CHECK(resource.deallocations == 1);
}

TEST_CASE("SharedValue reset on an empty box is a no-op") {
  CountingResource resource;

  auto box = dojio::SharedValue<Widget>::make(&resource, 3);
  box.reset();
  CHECK_FALSE(static_cast<bool>(box));
  CHECK(box.get() == nullptr);
  CHECK(resource.deallocations == 1);
  box.reset();
  CHECK(resource.deallocations == 1);
}

TEST_CASE("SharedValue::make returns the storage when the constructor throws") {
  struct Throwing {
    explicit Throwing(bool fail) {
      if (fail) {
        throw std::runtime_error{"boom"};
      }
    }
  };

  CountingResource resource;

  CHECK_THROWS_AS(dojio::SharedValue<Throwing>::make(&resource, true), std::runtime_error);
  CHECK(resource.allocations == 1);
  CHECK(resource.deallocations == 1);
}

namespace {
struct Base {
  virtual ~Base() = default;
  explicit Base(std::int32_t v) : value(v) {}
  std::int32_t value = 0;
};

struct Derived final : Base {
  explicit Derived(std::int32_t v) : Base(v) {}
  std::uint64_t payload[8]{};
};
} // namespace

TEST_CASE("SharedValue converting move construction deallocates the concrete object size") {
  CountingResource resource;

  CHECK(sizeof(Derived) > sizeof(Base));
  {
    auto derived = dojio::SharedValue<Derived>::make(&resource, 42);
    Derived* raw = derived.get();

    dojio::SharedValue<Base> base{std::move(derived)};
    CHECK_FALSE(static_cast<bool>(derived));
    CHECK(base.get() == static_cast<Base*>(raw));
    CHECK(base->value == 42);
    CHECK(resource.allocations == 1);
  }
  CHECK(resource.deallocations == 1);
  // the storage is returned with the size of the derived object, not the base
  CHECK(resource.last_deallocation_size >= sizeof(Derived));
}
