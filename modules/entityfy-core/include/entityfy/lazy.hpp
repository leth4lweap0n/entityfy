#pragma once

#include <entityfy/value.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace entityfy
{

class db_context;

enum class lazy_error_reason
{
    not_attached,      // no loader bound: the handle was never bound or was unbound
    context_destroyed, // the context that owned the binding is gone
    context_cleared,   // the context state that owned the binding no longer exists
    not_found,         // the loader found no row for the stored keys
};

namespace detail
{

[[nodiscard]] constexpr const char* lazy_error_message(lazy_error_reason reason) noexcept
{
    switch(reason)
    {
    case lazy_error_reason::not_attached:
        return "entityfy::lazy is not attached to a database context; bind a loader before accessing it.";
    case lazy_error_reason::context_destroyed:
        return "entityfy::lazy outlived the database context it was bound to.";
    case lazy_error_reason::context_cleared:
        return "entityfy::lazy was bound to a database context state that has since been cleared.";
    case lazy_error_reason::not_found:
        return "entityfy::lazy reference points at a row that no longer exists.";
    }

    return "entityfy::lazy could not be loaded.";
}

} // namespace detail

class lazy_error : public std::runtime_error
{
public:
    explicit lazy_error(lazy_error_reason reason)
        : std::runtime_error{detail::lazy_error_message(reason)}
        , reason_{reason}
    {
    }

    [[nodiscard]] lazy_error_reason reason() const noexcept
    {
        return reason_;
    }

private:
    lazy_error_reason reason_;
};

namespace detail
{

// Shared state owned by the context; handles only keep a weak reference so a
// destroyed context is detected instead of dereferenced.
struct lazy_link
{
    db_context*   owner = nullptr;
    std::uint64_t generation = 0;
    bool          alive = true;
};

} // namespace detail

// Shape metadata for a lazy handle: `lazy<child>` exposes one child reference,
// `lazy<std::vector<child>>` exposes a collection of child references.
template <typename T>
struct lazy_traits
{
    using element_type = T;
    using payload_type = T*;
    static constexpr bool is_collection = false;
};

template <typename T>
struct lazy_traits<std::vector<T>>
{
    using element_type = T;
    using payload_type = std::vector<T*>;
    static constexpr bool is_collection = true;
};

// Handle to a related row (or a set of related rows) that is loaded on first
// access through a loader bound by the context that materialised the owner.
//
// The handle never owns the entities it points at, and it is not thread safe:
// bind, load and access one handle from one thread at a time. A model stores
// the handle as an [[= entityfy::ignore]] member; persistence ignores it.
template <typename T>
class lazy
{
public:
    using traits = lazy_traits<T>;
    using element_type = typename traits::element_type;
    using payload_type = typename traits::payload_type;
    using loader_type = payload_type (*)(db_context* owner, std::span<const sql_value> keys);

    static constexpr bool is_collection = traits::is_collection;

    lazy() = default;
    lazy(const lazy&) = default;
    lazy(lazy&&) = default;
    lazy& operator=(const lazy&) = default;
    lazy& operator=(lazy&&) = default;
    ~lazy() = default;

    // True when a loader is bound and the context state it belongs to is still
    // alive and unchanged. Never loads anything.
    [[nodiscard]] bool is_attached() const noexcept
    {
        if(loader_ == nullptr)
        {
            return false;
        }

        const auto link = link_.lock();
        return link != nullptr && link->alive && link->generation == generation_;
    }

    // True when a payload is cached: an empty collection and a null reference
    // count as loaded. Never loads anything.
    [[nodiscard]] bool is_loaded() const noexcept
    {
        return payload_.has_value();
    }

    // Drops the cached payload but keeps the binding, so the next access loads
    // again.
    void reset() noexcept
    {
        payload_.reset();
    }

    // Drops the binding and the cached payload; the handle behaves like a
    // default constructed one.
    void unbind() noexcept
    {
        link_.reset();
        generation_ = 0;
        keys_.clear();
        loader_ = nullptr;
        payload_.reset();
    }

    // Snapshot of the key parts the handle was bound to. Never loads anything.
    [[nodiscard]] const std::vector<sql_value>& keys() const noexcept
    {
        return keys_;
    }

    // Joins the handle to a context state. A payload cached earlier is kept:
    // rows supplied by an include stay authoritative, only the binding and the
    // keys are replaced.
    void bind(std::weak_ptr<detail::lazy_link> link, std::uint64_t generation, std::vector<sql_value> keys, loader_type loader)
    {
        link_ = std::move(link);
        generation_ = generation;
        keys_ = std::move(keys);
        loader_ = loader;
    }

    // Fills the cache directly, e.g. from a query that already joined the
    // related rows or from an include. No loader is needed.
    void mark_loaded(payload_type payload)
    {
        payload_.emplace(std::move(payload));
    }

    // Loads through the bound loader when nothing is cached yet. The loader is
    // called at most once per cached payload.
    void ensure_loaded()
    {
        if(payload_.has_value())
        {
            return;
        }

        if(loader_ == nullptr)
        {
            throw lazy_error{lazy_error_reason::not_attached};
        }

        const auto link = link_.lock();
        if(link == nullptr || !link->alive)
        {
            throw lazy_error{lazy_error_reason::context_destroyed};
        }

        if(link->generation != generation_)
        {
            throw lazy_error{lazy_error_reason::context_cleared};
        }

        payload_.emplace(loader_(link->owner, std::span<const sql_value>{keys_}));
    }

    // Reference shape: nullptr when the loader found no row for the keys.
    [[nodiscard]] element_type* get()
        requires (!is_collection)
    {
        ensure_loaded();
        return *payload_;
    }

    // Reference shape: throws lazy_error{not_found} for a missing row.
    [[nodiscard]] element_type& operator*()
        requires (!is_collection)
    {
        element_type* value = get();
        if(value == nullptr)
        {
            throw lazy_error{lazy_error_reason::not_found};
        }

        return *value;
    }

    // Reference shape: returns nullptr for a missing row, like get().
    [[nodiscard]] element_type* operator->()
        requires (!is_collection)
    {
        return get();
    }

    // Reference shape: true when a row exists; never throws for a missing row.
    explicit operator bool()
        requires (!is_collection)
    {
        return get() != nullptr;
    }

    // Collection shape: loads on first access and returns the cached rows.
    [[nodiscard]] const payload_type& get()
        requires (is_collection)
    {
        ensure_loaded();
        return *payload_;
    }

    [[nodiscard]] const payload_type& operator*()
        requires (is_collection)
    {
        return get();
    }

    [[nodiscard]] const payload_type* operator->()
        requires (is_collection)
    {
        return std::addressof(get());
    }

    [[nodiscard]] std::size_t size()
        requires (is_collection)
    {
        return get().size();
    }

    [[nodiscard]] bool empty()
        requires (is_collection)
    {
        return get().empty();
    }

private:
    std::weak_ptr<detail::lazy_link> link_;
    std::uint64_t                    generation_ = 0;
    std::vector<sql_value>           keys_;
    loader_type                      loader_ = nullptr;
    std::optional<payload_type>      payload_;
};

namespace detail
{

template <typename T>
struct lazy_probe : std::false_type
{
};

template <typename T>
struct lazy_probe<lazy<T>> : std::true_type
{
};

} // namespace detail

template <typename T>
struct is_lazy : detail::lazy_probe<std::remove_cvref_t<T>>
{
};

template <typename T>
inline constexpr bool is_lazy_v = is_lazy<T>::value;

} // namespace entityfy
