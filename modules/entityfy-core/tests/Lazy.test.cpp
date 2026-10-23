#include <catch2/catch_test_macros.hpp>

#include <entityfy/lazy.hpp>

#include <concepts>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>

namespace
{

struct child
{
    int value = 0;
};

using entityfy::lazy;
using entityfy::lazy_error;
using entityfy::lazy_error_reason;
using entityfy::sql_value;

static_assert(std::is_base_of_v<std::runtime_error, lazy_error>);

static_assert(entityfy::is_lazy_v<lazy<child>>);
static_assert(entityfy::is_lazy_v<const lazy<child>&>);
static_assert(!entityfy::is_lazy_v<child>);
static_assert(!entityfy::is_lazy_v<std::vector<child>>);

static_assert(std::same_as<entityfy::lazy_traits<child>::element_type, child>);
static_assert(std::same_as<entityfy::lazy_traits<child>::payload_type, child*>);
static_assert(!entityfy::lazy_traits<child>::is_collection);

static_assert(std::same_as<entityfy::lazy_traits<std::vector<child>>::element_type, child>);
static_assert(std::same_as<entityfy::lazy_traits<std::vector<child>>::payload_type, std::vector<child*>>);
static_assert(entityfy::lazy_traits<std::vector<child>>::is_collection);

static_assert(std::same_as<lazy<child>::element_type, child>);
static_assert(std::same_as<lazy<child>::loader_type, child* (*)(entityfy::db_context*, std::span<const sql_value>)>);
static_assert(std::same_as<lazy<std::vector<child>>::payload_type, std::vector<child*>>);

using link_ptr = std::shared_ptr<entityfy::detail::lazy_link>;

child alice{1};
child bob{2};

int                    load_count = 0;
std::vector<sql_value> last_keys;

[[nodiscard]] link_ptr make_link(std::uint64_t generation = 0)
{
    auto link = std::make_shared<entityfy::detail::lazy_link>();
    link->generation = generation;
    return link;
}

[[nodiscard]] std::vector<sql_value> int_keys(std::int64_t value)
{
    return {sql_value{value}};
}

// A core test has no context instance, so every loader ignores its owner. The
// null owner still proves that the handle never dereferences it itself.
child* load_alice(entityfy::db_context* owner, std::span<const sql_value> keys)
{
    (void)owner;
    ++load_count;
    last_keys.assign(keys.begin(), keys.end());
    return &alice;
}

child* load_nothing(entityfy::db_context* owner, std::span<const sql_value> keys)
{
    (void)owner;
    (void)keys;
    ++load_count;
    return nullptr;
}

std::vector<child*> load_pair(entityfy::db_context* owner, std::span<const sql_value> keys)
{
    (void)owner;
    (void)keys;
    ++load_count;
    return {&alice, &bob};
}

std::vector<child*> load_empty(entityfy::db_context* owner, std::span<const sql_value> keys)
{
    (void)owner;
    (void)keys;
    ++load_count;
    return {};
}

template <typename Function>
[[nodiscard]] lazy_error_reason thrown_reason(Function&& function)
{
    try
    {
        function();
    }
    catch(const lazy_error& error)
    {
        return error.reason();
    }

    FAIL("expected entityfy::lazy_error");
    return lazy_error_reason::not_attached;
}

void reset_loads()
{
    load_count = 0;
    last_keys.clear();
}

} // namespace

TEST_CASE("lazy reference handle starts detached", "[lazy]")
{
    reset_loads();

    lazy<child> handle;
    REQUIRE_FALSE(handle.is_attached());
    REQUIRE_FALSE(handle.is_loaded());
    REQUIRE(handle.keys().empty());

    REQUIRE(thrown_reason([&] { (void)handle.get(); }) == lazy_error_reason::not_attached);
    REQUIRE(thrown_reason([&] { (void)*handle; }) == lazy_error_reason::not_attached);
    REQUIRE(thrown_reason([&] { (void)handle.operator->(); }) == lazy_error_reason::not_attached);
    REQUIRE(load_count == 0);

    // The message names the handle and the actual problem.
    try
    {
        (void)handle.get();
        FAIL("expected entityfy::lazy_error");
    }
    catch(const lazy_error& error)
    {
        const std::string_view message{error.what()};
        REQUIRE(message.find("entityfy::lazy") != std::string_view::npos);
        REQUIRE(message.find("not attached") != std::string_view::npos);
    }
}

TEST_CASE("lazy binds, loads once and exposes the keys", "[lazy]")
{
    reset_loads();

    const auto link = make_link();
    REQUIRE(link->owner == nullptr);

    lazy<child> handle;
    handle.bind(link, link->generation, int_keys(42), &load_alice);

    REQUIRE(handle.is_attached());
    REQUIRE_FALSE(handle.is_loaded());
    REQUIRE(handle.keys().size() == 1);
    REQUIRE(handle.keys().front() == sql_value{std::int64_t{42}});
    REQUIRE(load_count == 0);

    REQUIRE(handle.get() == &alice);
    REQUIRE(load_count == 1);
    REQUIRE(last_keys.size() == 1);
    REQUIRE(last_keys.front() == sql_value{std::int64_t{42}});
    REQUIRE(handle.is_loaded());
    REQUIRE(handle.is_attached());

    REQUIRE(handle.get() == &alice);
    REQUIRE(handle.get()->value == 1);
    REQUIRE((*handle).value == 1);
    REQUIRE(handle->value == 1);
    REQUIRE(static_cast<bool>(handle));
    REQUIRE(load_count == 1);
}

TEST_CASE("lazy collection loads once and exposes size and elements", "[lazy]")
{
    reset_loads();

    const auto link = make_link();

    lazy<std::vector<child>> handle;
    handle.bind(link, link->generation, int_keys(9), &load_pair);

    REQUIRE(handle.is_attached());
    REQUIRE_FALSE(handle.is_loaded());
    REQUIRE(handle.keys().size() == 1);
    REQUIRE(load_count == 0);

    REQUIRE(handle.size() == 2);
    REQUIRE(load_count == 1);
    REQUIRE(handle.is_loaded());
    REQUIRE_FALSE(handle.empty());

    REQUIRE(handle.get().size() == 2);
    REQUIRE(handle.get()[0] == &alice);
    REQUIRE(handle.get()[1] == &bob);
    REQUIRE((*handle).size() == 2);
    REQUIRE(handle->size() == 2);

    REQUIRE(handle.size() == 2);
    REQUIRE(handle.empty() == false);
    REQUIRE(load_count == 1);
}

TEST_CASE("lazy empty collection is a loaded result", "[lazy]")
{
    reset_loads();

    const auto link = make_link();

    lazy<std::vector<child>> handle;
    handle.bind(link, link->generation, {}, &load_empty);

    REQUIRE(handle.empty());
    REQUIRE(handle.is_loaded());
    REQUIRE(handle.size() == 0);
    REQUIRE(handle.get().empty());
    REQUIRE(load_count == 1);

    handle.reset();
    REQUIRE_FALSE(handle.is_loaded());
    REQUIRE(handle.is_attached());
    REQUIRE(handle.empty());
    REQUIRE(load_count == 2);
}

TEST_CASE("lazy reset reloads while unbind detaches", "[lazy]")
{
    reset_loads();

    const auto link = make_link();

    lazy<child> handle;
    handle.bind(link, link->generation, int_keys(7), &load_alice);

    REQUIRE(handle.get() == &alice);
    REQUIRE(load_count == 1);

    handle.reset();
    REQUIRE_FALSE(handle.is_loaded());
    REQUIRE(handle.is_attached());
    REQUIRE(handle.keys().size() == 1);
    REQUIRE(handle.get() == &alice);
    REQUIRE(load_count == 2);

    handle.unbind();
    REQUIRE_FALSE(handle.is_attached());
    REQUIRE_FALSE(handle.is_loaded());
    REQUIRE(handle.keys().empty());
    REQUIRE(thrown_reason([&] { (void)handle.get(); }) == lazy_error_reason::not_attached);
    REQUIRE(load_count == 2);
}

TEST_CASE("lazy mark_loaded caches without a loader", "[lazy]")
{
    reset_loads();

    const auto link = make_link();

    lazy<child> reference;
    reference.bind(link, link->generation, int_keys(3), nullptr);
    REQUIRE_FALSE(reference.is_attached());

    reference.mark_loaded(&bob);
    REQUIRE(reference.is_loaded());
    REQUIRE(reference.get() == &bob);
    REQUIRE((*reference).value == 2);
    REQUIRE(reference->value == 2);
    REQUIRE(load_count == 0);

    lazy<std::vector<child>> collection;
    collection.bind(link, link->generation, {}, nullptr);
    collection.mark_loaded({&alice});

    REQUIRE(collection.size() == 1);
    REQUIRE(collection.get().front() == &alice);
    REQUIRE(collection.is_loaded());
    REQUIRE(load_count == 0);
}

TEST_CASE("lazy bind keeps an include supplied payload", "[lazy]")
{
    reset_loads();

    const auto link = make_link();

    lazy<child> handle;
    handle.bind(link, link->generation, int_keys(1), &load_alice);
    REQUIRE(handle.get() == &alice);
    REQUIRE(load_count == 1);

    // Rebinding replaces the binding and the keys but never the cached rows.
    const auto other = make_link(link->generation);
    handle.bind(other, other->generation, int_keys(2), &load_nothing);

    REQUIRE(handle.keys().size() == 1);
    REQUIRE(handle.keys().front() == sql_value{std::int64_t{2}});
    REQUIRE(handle.is_attached());
    REQUIRE(handle.get() == &alice);
    REQUIRE(load_count == 1);

    // Dropping the include cache exposes the new loader, which finds no row.
    handle.reset();
    REQUIRE(handle.get() == nullptr);
    REQUIRE(load_count == 2);
    REQUIRE(handle.is_loaded());
    REQUIRE_FALSE(static_cast<bool>(handle));
}

TEST_CASE("lazy copy keeps the payload and the binding", "[lazy]")
{
    reset_loads();

    const auto link = make_link();

    lazy<child> original;
    original.bind(link, link->generation, int_keys(11), &load_alice);
    REQUIRE(original.get() == &alice);
    REQUIRE(load_count == 1);

    lazy<child> copy = original;
    REQUIRE(copy.is_loaded());
    REQUIRE(copy.is_attached());
    REQUIRE(copy.keys().size() == 1);
    REQUIRE(copy.keys().front() == sql_value{std::int64_t{11}});
    REQUIRE(copy.get() == &alice);
    REQUIRE(load_count == 1);

    lazy<child> assigned;
    assigned = original;
    REQUIRE(assigned.is_loaded());
    REQUIRE(assigned.is_attached());
    REQUIRE(assigned.get() == &alice);
    REQUIRE(load_count == 1);

    // Copying a bound but unloaded handle keeps the binding and its own cache.
    lazy<child> pending;
    pending.bind(link, link->generation, int_keys(12), &load_alice);
    lazy<child> pending_copy = pending;
    REQUIRE(pending_copy.is_attached());
    REQUIRE_FALSE(pending_copy.is_loaded());
    REQUIRE(pending_copy.get() == &alice);
    REQUIRE(load_count == 2);
}

TEST_CASE("lazy detects a destroyed or cleared context", "[lazy]")
{
    reset_loads();

    auto link = make_link();

    lazy<child> handle;
    handle.bind(link, link->generation, int_keys(5), &load_alice);

    link->alive = false;
    REQUIRE_FALSE(handle.is_attached());
    REQUIRE(handle.is_loaded() == false);
    REQUIRE(thrown_reason([&] { (void)handle.get(); }) == lazy_error_reason::context_destroyed);
    REQUIRE(load_count == 0);

    // A cached payload never touches the context, even a dead one.
    handle.mark_loaded(&bob);
    REQUIRE(link->alive == false);
    REQUIRE(handle.get() == &bob);
    REQUIRE(load_count == 0);

    // A generation bump means the context state the handle knew is gone.
    const auto fresh = make_link();
    lazy<child> cleared;
    cleared.bind(fresh, fresh->generation, int_keys(5), &load_alice);
    REQUIRE(cleared.is_attached());
    fresh->generation += 1;
    REQUIRE_FALSE(cleared.is_attached());
    REQUIRE(thrown_reason([&] { (void)cleared.get(); }) == lazy_error_reason::context_cleared);
    REQUIRE(load_count == 0);

    // An expired link is reported the same way as a destroyed context.
    lazy<child> expired;
    {
        const auto temporary = make_link();
        expired.bind(temporary, temporary->generation, int_keys(5), &load_alice);
        REQUIRE(expired.is_attached());
    }

    REQUIRE_FALSE(expired.is_attached());
    REQUIRE(thrown_reason([&] { (void)expired.get(); }) == lazy_error_reason::context_destroyed);
    REQUIRE(load_count == 0);
}

TEST_CASE("lazy reference reports a missing row", "[lazy]")
{
    reset_loads();

    const auto link = make_link();

    lazy<child> handle;
    handle.bind(link, link->generation, int_keys(99), &load_nothing);

    REQUIRE(handle.get() == nullptr);
    REQUIRE(load_count == 1);
    REQUIRE(handle.is_loaded());
    REQUIRE(handle.is_attached());
    REQUIRE_FALSE(static_cast<bool>(handle));

    // The missing row is cached: neither access nor a second get reloads it.
    REQUIRE(handle.get() == nullptr);
    REQUIRE(load_count == 1);
    REQUIRE(thrown_reason([&] { (void)*handle; }) == lazy_error_reason::not_found);
    REQUIRE(thrown_reason([&] { (void)handle.operator*(); }) == lazy_error_reason::not_found);
    REQUIRE(load_count == 1);

    handle.reset();
    REQUIRE(handle.get() == nullptr);
    REQUIRE(load_count == 2);
}

TEST_CASE("lazy keys are a snapshot of the binding", "[lazy]")
{
    reset_loads();

    const auto link = make_link();

    std::vector<sql_value> keys = int_keys(21);

    lazy<child> handle;
    handle.bind(link, link->generation, keys, &load_alice);

    keys.front() = sql_value{std::int64_t{22}};
    REQUIRE(handle.keys().size() == 1);
    REQUIRE(handle.keys().front() == sql_value{std::int64_t{21}});

    REQUIRE(handle.get() == &alice);
    REQUIRE(last_keys.size() == 1);
    REQUIRE(last_keys.front() == sql_value{std::int64_t{21}});
    REQUIRE(load_count == 1);
}
